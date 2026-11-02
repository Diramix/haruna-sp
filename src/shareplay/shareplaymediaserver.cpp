/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "shareplaymediaserver.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QMimeDatabase>
#include <QTcpSocket>
#include <QTimer>
#include <memory>
#ifdef Q_OS_LINUX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace Qt::StringLiterals;
namespace
{
constexpr qint64 ChunkSize = 256 * 1024;
constexpr int TimeoutMs = 15000;

bool decimal(const QByteArray &value, qint64 &number)
{
    if (value.isEmpty()) {
        return false;
    }
    for (char c : value) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    bool ok = false;
    number = value.toLongLong(&ok);
    return ok;
}
}

SharePlayMediaServer::SharePlayMediaServer(QObject *parent)
    : QObject(parent)
    , m_watcher(new QFileSystemWatcher(this))
    , m_poll(new QTimer(this))
{
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this]() {
        checkFile();
    });
    m_poll->setInterval(250);
    connect(m_poll, &QTimer::timeout, this, [this]() {
        checkFile();
    });
}

SharePlayMediaServer::~SharePlayMediaServer()
{
    stop();
}

QByteArray SharePlayMediaServer::fingerprint(const QString &path, QFile *opened)
{
#ifdef Q_OS_LINUX
    struct stat st;
    const int result = opened ? ::fstat(opened->handle(), &st) : ::stat(QFile::encodeName(path).constData(), &st);
    if (result != 0 || !S_ISREG(st.st_mode) || (st.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) == 0
        || (!opened && ::access(QFile::encodeName(path).constData(), R_OK) != 0)) {
        return {};
    }
    return QByteArray::number(quint64(st.st_dev)) + ':' + QByteArray::number(quint64(st.st_ino)) + ':' + QByteArray::number(st.st_size) + ':'
        + QByteArray::number(st.st_mtim.tv_sec) + ':' + QByteArray::number(st.st_mtim.tv_nsec) + ':' + QByteArray::number(st.st_ctim.tv_sec) + ':'
        + QByteArray::number(st.st_ctim.tv_nsec);
#else
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable() || (opened && opened->size() != info.size())) {
        return {};
    }
    return info.canonicalFilePath().toUtf8() + ':' + QByteArray::number(info.size()) + ':' + QByteArray::number(info.lastModified().toMSecsSinceEpoch()) + ':'
        + QByteArray::number(info.metadataChangeTime().toMSecsSinceEpoch());
#endif
}

void SharePlayMediaServer::abortSockets()
{
    const auto sockets = m_sockets;
    for (auto *socket : sockets) {
        socket->abort();
        socket->deleteLater();
    }
    m_sockets.clear();
}

void SharePlayMediaServer::stop()
{
    if (!m_file.isEmpty()) {
        ++m_revision;
    }
    m_file.clear();
    m_fingerprint.clear();
    m_tokenReady = false;
    m_poll->stop();
    if (!m_watcher->files().isEmpty()) {
        m_watcher->removePaths(m_watcher->files());
    }
    abortSockets();
}

void SharePlayMediaServer::invalidate()
{
    if (m_file.isEmpty()) {
        return;
    }
    stop();
    Q_EMIT fileInvalidated();
}

bool SharePlayMediaServer::checkFile(QFile *opened)
{
    if (m_file.isEmpty()) {
        return false;
    }
    if (fingerprint(m_file) != m_fingerprint || (opened && fingerprint(m_file, opened) != m_fingerprint)) {
        invalidate();
        return false;
    }
    return true;
}

void SharePlayMediaServer::setFile(const QString &path)
{
    const QString absolute = path.isEmpty() ? QString() : QFileInfo(path).absoluteFilePath();
    const QByteArray snapshot = absolute.isEmpty() ? QByteArray() : fingerprint(absolute);
    if (!snapshot.isEmpty() && absolute == m_file && snapshot == m_fingerprint) {
        return;
    }
    stop();
    if (!snapshot.isEmpty()) {
        ++m_revision;
        m_file = absolute;
        m_fingerprint = snapshot;
        m_watcher->addPath(absolute);
        m_poll->start();
    }
}

void SharePlayMediaServer::setToken(quint64 token)
{
    if (m_hasToken && token == m_token) {
        return;
    }
    abortSockets();
    m_token = token;
    m_hasToken = true;
    m_tokenReady = !m_file.isEmpty();
}

QString SharePlayMediaServer::file() const
{
    return m_file;
}

quint64 SharePlayMediaServer::revision() const
{
    return m_revision;
}

void SharePlayMediaServer::handleSocket(QTcpSocket *socket)
{
    m_sockets.insert(socket);
    connect(socket, &QObject::destroyed, this, [this, socket]() {
        m_sockets.remove(socket);
    });
    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    socket->setReadBufferSize(16 * 1024 + 1);
    auto *timer = new QTimer(socket);
    timer->setSingleShot(true);
    timer->start(TimeoutMs);
    connect(timer, &QTimer::timeout, socket, [socket]() {
        socket->abort();
    });
    connect(socket, &QTcpSocket::bytesWritten, timer, [timer]() {
        timer->start(TimeoutMs);
    });
    auto buffer = std::make_shared<QByteArray>();
    auto connection = std::make_shared<QMetaObject::Connection>();
    auto readHeader = [this, socket, buffer, connection]() {
        buffer->append(socket->readAll());
        const qsizetype end = buffer->indexOf("\r\n\r\n");
        if (end < 0 && buffer->size() <= 16 * 1024) {
            return;
        }
        disconnect(*connection);
        // Never permit a second request to create another writer on this socket.
        connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
            socket->abort();
        });
        if (buffer->size() > 16 * 1024) {
            sendError(socket, 431, u"Request Header Fields Too Large"_s);
        } else if (end + 4 != buffer->size()) {
            socket->abort();
        } else {
            handleRequest(socket, buffer->left(end));
        }
    };
    *connection = connect(socket, &QTcpSocket::readyRead, this, readHeader);
    if (socket->bytesAvailable() > 0) {
        readHeader();
    }
}

void SharePlayMediaServer::handleRequest(QTcpSocket *socket, const QByteArray &request)
{
    const auto lines = request.split('\n');
    const auto parts = lines.first().trimmed().split(' ');
    if (parts.size() != 3 || (parts[2] != "HTTP/1.1" && parts[2] != "HTTP/1.0")) {
        sendError(socket, 400, u"Bad Request"_s);
        return;
    }
    const bool head = parts[0] == "HEAD";
    if (!head && parts[0] != "GET") {
        sendError(socket, 405, u"Method Not Allowed"_s);
        return;
    }
    // Exact canonical target: no additional queries, escaping, or alternate paths.
    if (!m_tokenReady) {
        sendError(socket, 404, u"No media"_s);
        return;
    }
    if (parts[1] != "/media?v=" + QByteArray::number(m_token)) {
        sendError(socket, 410, u"Gone"_s);
        return;
    }
    if (!checkFile()) {
        if (socket->state() == QAbstractSocket::ConnectedState) {
            sendError(socket, 404, u"No media"_s);
        }
        return;
    }
    auto *media = new QFile(m_file, socket);
#ifdef Q_OS_LINUX
    // O_NONBLOCK prevents a raced replacement with a FIFO from blocking open().
    const int fd = ::open(QFile::encodeName(m_file).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        invalidate();
        return;
    }
    if (!media->open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        invalidate();
        return;
    }
#else
    if (!media->open(QIODevice::ReadOnly)) {
        invalidate();
        return;
    }
#endif
    if (!checkFile(media)) {
        return;
    }
    const qint64 size = media->size();
    qint64 start = 0;
    qint64 end = size - 1;
    bool partial = false;
    bool invalidRange = false;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines[i].trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon < 0) {
            sendError(socket, 400, u"Bad Request"_s);
            return;
        }
        const auto name = line.left(colon).trimmed().toLower();
        if (name == "content-length" && line.mid(colon + 1).trimmed() != "0") {
            sendError(socket, 400, u"Bad Request"_s);
            return;
        }
        if (name == "transfer-encoding") {
            sendError(socket, 400, u"Bad Request"_s);
            return;
        }
        if (name != "range") {
            continue;
        }
        if (partial) {
            invalidRange = true;
            continue;
        }
        partial = true;
        const auto value = line.mid(colon + 1).trimmed();
        const auto spec = value.mid(6);
        const auto dash = spec.indexOf('-');
        qint64 first = 0;
        qint64 last = 0;
        if (!value.startsWith("bytes=") || dash < 0 || size == 0) {
            invalidRange = true;
        } else if (dash == 0) {
            if (!decimal(spec.mid(1), last) || last == 0) {
                invalidRange = true;
            } else {
                start = qMax(qint64(0), size - last);
            }
        } else if (!decimal(spec.left(dash), first) || first >= size) {
            invalidRange = true;
        } else {
            start = first;
            if (!spec.mid(dash + 1).isEmpty()) {
                if (!decimal(spec.mid(dash + 1), last) || last < first) {
                    invalidRange = true;
                } else {
                    end = qMin(last, size - 1);
                }
            }
        }
    }
    if (invalidRange) {
        socket->write("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" + QByteArray::number(size)
                      + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        socket->disconnectFromHost();
        return;
    }
    const qint64 length = end - start + 1;
    QByteArray header = partial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
    header += "Accept-Ranges: bytes\r\nContent-Type: " + QMimeDatabase().mimeTypeForFile(m_file, QMimeDatabase::MatchExtension).name().toLatin1()
        + "\r\nContent-Length: " + QByteArray::number(length) + "\r\n";
    if (partial) {
        header += "Content-Range: bytes " + QByteArray::number(start) + '-' + QByteArray::number(end) + '/' + QByteArray::number(size) + "\r\n";
    }
    header += "Connection: close\r\n\r\n";
    socket->write(header);
    if (head || length == 0) {
        socket->disconnectFromHost();
        return;
    }
    if (!media->seek(start)) {
        invalidate();
        return;
    }
    auto remaining = std::make_shared<qint64>(length);
    auto writeChunk = [this, socket, media, remaining]() {
        while (*remaining > 0 && socket->state() == QAbstractSocket::ConnectedState && socket->bytesToWrite() < ChunkSize * 2) {
            if (!checkFile(media)) {
                return;
            }
            const auto chunk = media->read(qMin(ChunkSize, *remaining));
            // Check again after read, before queueing possibly changed bytes.
            if (!checkFile(media)) {
                return;
            }
            if (chunk.isEmpty()) {
                invalidate();
                return;
            }
            *remaining -= chunk.size();
            if (socket->write(chunk) != chunk.size()) {
                socket->abort();
                return;
            }
        }
        if (*remaining == 0) {
            socket->disconnectFromHost();
        }
    };
    connect(socket, &QTcpSocket::bytesWritten, this, [writeChunk]() {
        writeChunk();
    });
    writeChunk();
}

void SharePlayMediaServer::sendError(QTcpSocket *socket, int code, const QString &text)
{
    // Empty responses also give correct HEAD semantics on every error path.
    socket->write("HTTP/1.1 " + QByteArray::number(code) + ' ' + text.toUtf8() + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    socket->disconnectFromHost();
}
