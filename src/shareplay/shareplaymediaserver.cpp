/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "shareplaymediaserver.h"

#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QTcpServer>
#include <QTcpSocket>

using namespace Qt::StringLiterals;

namespace
{
constexpr qint64 ChunkSize = 256 * 1024;

QByteArray httpDate()
{
    return QDateTime::currentDateTimeUtc().toString(u"ddd, dd MMM yyyy hh:mm:ss 'GMT'"_s).toLatin1();
}
}

SharePlayMediaServer::SharePlayMediaServer(QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, &SharePlayMediaServer::onNewConnection);
}

SharePlayMediaServer::~SharePlayMediaServer()
{
    stop();
}

bool SharePlayMediaServer::start(quint16 port)
{
    if (m_server->isListening()) {
        stop();
    }
    return m_server->listen(QHostAddress::Any, port);
}

void SharePlayMediaServer::stop()
{
    if (m_server->isListening()) {
        m_server->close();
    }
}

bool SharePlayMediaServer::isListening() const
{
    return m_server->isListening();
}

quint16 SharePlayMediaServer::port() const
{
    return m_server->serverPort();
}

void SharePlayMediaServer::setFile(const QString &path)
{
    m_file = path;
}

QString SharePlayMediaServer::file() const
{
    return m_file;
}

void SharePlayMediaServer::onNewConnection()
{
    while (auto *socket = m_server->nextPendingConnection()) {
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
            buffer->append(socket->readAll());
            if (buffer->size() > 16 * 1024) {
                sendError(socket, 431, u"Request Header Fields Too Large"_s);
                return;
            }
            const int headerEnd = buffer->indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                return;
            }
            const QByteArray request = buffer->left(headerEnd);
            buffer->clear();
            handleRequest(socket, request);
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

void SharePlayMediaServer::handleRequest(QTcpSocket *socket, const QByteArray &request)
{
    const QList<QByteArray> lines = request.split('\n');
    if (lines.isEmpty()) {
        sendError(socket, 400, u"Bad Request"_s);
        return;
    }

    const QList<QByteArray> requestLine = lines.constFirst().trimmed().split(' ');
    if (requestLine.size() < 2) {
        sendError(socket, 400, u"Bad Request"_s);
        return;
    }

    const QByteArray method = requestLine.constFirst().toUpper();
    if (method != "GET" && method != "HEAD") {
        sendError(socket, 405, u"Method Not Allowed"_s);
        return;
    }

    if (m_file.isEmpty()) {
        sendError(socket, 404, u"No media"_s);
        return;
    }

    auto *media = new QFile(m_file, socket);
    if (!media->open(QIODevice::ReadOnly)) {
        sendError(socket, 404, u"No media"_s);
        return;
    }

    const qint64 fileSize = media->size();
    qint64 rangeStart = 0;
    qint64 rangeEnd = fileSize - 1;
    bool isPartial = false;

    for (const QByteArray &line : lines) {
        const QByteArray trimmed = line.trimmed();
        if (!trimmed.toLower().startsWith("range:")) {
            continue;
        }
        QByteArray value = trimmed.mid(6).trimmed();
        if (!value.toLower().startsWith("bytes=")) {
            break;
        }
        value = value.mid(6).trimmed();
        // only a single range is supported
        const int dash = value.indexOf('-');
        if (dash < 0) {
            break;
        }
        const QByteArray first = value.left(dash).trimmed();
        const QByteArray last = value.mid(dash + 1).trimmed();
        bool ok = false;
        if (first.isEmpty()) {
            const qint64 suffix = last.toLongLong(&ok);
            if (ok && suffix > 0) {
                rangeStart = qMax(qint64(0), fileSize - suffix);
                isPartial = true;
            }
        } else {
            const qint64 start = first.toLongLong(&ok);
            if (ok) {
                rangeStart = start;
                isPartial = true;
                if (!last.isEmpty()) {
                    bool endOk = false;
                    const qint64 end = last.toLongLong(&endOk);
                    if (endOk) {
                        rangeEnd = qMin(end, fileSize - 1);
                    }
                }
            }
        }
        break;
    }

    if (isPartial && (rangeStart < 0 || rangeStart >= fileSize || rangeEnd < rangeStart)) {
        QByteArray header;
        header.append("HTTP/1.1 416 Range Not Satisfiable\r\n");
        header.append("Content-Range: bytes */" + QByteArray::number(fileSize) + "\r\n");
        header.append("Content-Length: 0\r\n\r\n");
        socket->write(header);
        socket->disconnectFromHost();
        return;
    }

    const qint64 length = rangeEnd - rangeStart + 1;
    const QString mimeType = QMimeDatabase().mimeTypeForFile(m_file).name();

    QByteArray header;
    header.append(isPartial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n");
    header.append("Date: " + httpDate() + "\r\n");
    header.append("Server: Haruna SharePlay\r\n");
    header.append("Accept-Ranges: bytes\r\n");
    header.append("Content-Type: " + mimeType.toLatin1() + "\r\n");
    header.append("Content-Length: " + QByteArray::number(length) + "\r\n");
    if (isPartial) {
        header.append("Content-Range: bytes " + QByteArray::number(rangeStart) + "-" + QByteArray::number(rangeEnd) + "/" + QByteArray::number(fileSize)
                      + "\r\n");
    }
    header.append("Connection: close\r\n\r\n");
    socket->write(header);

    if (method == "HEAD") {
        socket->disconnectFromHost();
        return;
    }

    media->seek(rangeStart);
    auto remaining = std::make_shared<qint64>(length);

    auto writeChunk = [socket, media, remaining]() {
        while (*remaining > 0 && socket->bytesToWrite() < ChunkSize * 2) {
            const QByteArray chunk = media->read(qMin(ChunkSize, *remaining));
            if (chunk.isEmpty()) {
                *remaining = 0;
                break;
            }
            *remaining -= chunk.size();
            socket->write(chunk);
        }
        if (*remaining <= 0) {
            socket->disconnectFromHost();
        }
    };

    connect(socket, &QTcpSocket::bytesWritten, socket, [writeChunk](qint64) {
        writeChunk();
    });
    writeChunk();
}

void SharePlayMediaServer::sendError(QTcpSocket *socket, int code, const QString &text)
{
    const QByteArray body = text.toUtf8();
    QByteArray header;
    header.append("HTTP/1.1 " + QByteArray::number(code) + " " + body + "\r\n");
    header.append("Content-Type: text/plain; charset=utf-8\r\n");
    header.append("Content-Length: " + QByteArray::number(body.size()) + "\r\n");
    header.append("Connection: close\r\n\r\n");
    socket->write(header);
    socket->write(body);
    socket->disconnectFromHost();
}
