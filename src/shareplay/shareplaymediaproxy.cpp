/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "shareplaymediaproxy.h"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSslCertificate>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace
{
constexpr qint64 BufferSize = 256 * 1024;

class Transfer : public QObject
{
public:
    Transfer(QTcpSocket *local, const QUrl &remote, const QByteArray &fingerprint, const QByteArray &path, QObject *parent)
        : QObject(parent)
        , m_local(local)
        , m_remote(remote)
        , m_fingerprint(fingerprint)
        , m_path(path)
        , m_tls(new QSslSocket(this))
    {
        local->setParent(this);
        local->setReadBufferSize(16385);
        m_tls->setReadBufferSize(BufferSize);
        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->start(15000);
        connect(timer, &QTimer::timeout, this, [this]() {
            close();
        });
        connect(local, &QTcpSocket::readyRead, this, [this, timer]() {
            m_request += m_local->readAll();
            if (m_request.size() > 16384) {
                close();
                return;
            }
            if (!m_request.contains("\r\n\r\n")) {
                return;
            }
            timer->start();
            begin();
        });
        connect(local, &QTcpSocket::disconnected, this, [this]() {
            close();
        });
        connect(local, &QTcpSocket::bytesWritten, this, [this]() {
            pump();
        });
        connect(m_tls, &QSslSocket::sslErrors, this, [this](const QList<QSslError> &) {
            if (matches()) {
                m_tls->ignoreSslErrors();
            }
        });
        connect(m_tls, &QSslSocket::encrypted, this, [this, timer]() {
            if (!matches()) {
                close();
                return;
            }
            timer->stop();
            m_tls->write(m_outgoing);
        });
        connect(m_tls, &QSslSocket::readyRead, this, [this]() {
            pump();
        });
        connect(m_tls, &QSslSocket::disconnected, this, [this]() {
            pump();
            if (m_tls->bytesAvailable() == 0) {
                m_local->disconnectFromHost();
            }
        });
        connect(m_tls, &QSslSocket::errorOccurred, this, [this](QAbstractSocket::SocketError error) {
            if (error != QAbstractSocket::RemoteHostClosedError) {
                close();
            }
        });
    }

private:
    bool matches() const
    {
        const auto certificate = m_tls->peerCertificate();
        return !certificate.isNull() && certificate.digest(QCryptographicHash::Sha256).toHex() == m_fingerprint;
    }

    void begin()
    {
        const auto lines = m_request.left(m_request.indexOf("\r\n\r\n")).split('\n');
        const auto first = lines.first().trimmed().split(' ');
        if (first.size() != 3 || (first[0] != "GET" && first[0] != "HEAD") || first[1] != m_path || !first[2].startsWith("HTTP/1.")) {
            close();
            return;
        }
        QByteArray range;
        static const QRegularExpression validRange(QStringLiteral("^bytes=(?:[0-9]+-[0-9]*|-[0-9]+)$"));
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines[i].trimmed();
            const auto colon = line.indexOf(':');
            if (colon <= 0) {
                close();
                return;
            }
            if (line.left(colon).trimmed().toLower() == "range") {
                const QByteArray value = line.mid(colon + 1).trimmed();
                if (!range.isEmpty() || !validRange.match(QString::fromLatin1(value)).hasMatch()) {
                    close();
                    return;
                }
                range = value;
            }
        }
        m_local->disconnect(this);
        connect(m_local, &QTcpSocket::disconnected, this, [this]() {
            close();
        });
        connect(m_local, &QTcpSocket::bytesWritten, this, [this]() {
            pump();
        });
        m_outgoing = first[0] + ' ' + m_remote.toEncoded(QUrl::FullyEncoded | QUrl::RemoveScheme | QUrl::RemoveAuthority)
            + " HTTP/1.1\r\nHost: " + m_remote.authority(QUrl::FullyEncoded).toLatin1() + "\r\nConnection: close\r\n";
        if (!range.isEmpty()) {
            m_outgoing += "Range: " + range + "\r\n";
        }
        m_outgoing += "\r\n";
        m_tls->connectToHostEncrypted(m_remote.host(), m_remote.port(443));
    }

    void pump()
    {
        if (!m_headersDone) {
            m_headers += m_tls->read(qMax<qint64>(0, 16385 - m_headers.size()));
            const auto end = m_headers.indexOf("\r\n\r\n");
            if (end < 0) {
                if (m_headers.size() > 16384) {
                    close();
                }
                return;
            }
            const auto status = m_headers.left(m_headers.indexOf("\r\n")).split(' ');
            if (status.size() < 2 || (status[1] != "200" && status[1] != "206" && status[1] != "404" && status[1] != "416")) {
                close();
                return;
            }
            m_headersDone = true;
            m_local->write(m_headers);
            m_headers.clear();
        }
        const qint64 room = BufferSize - m_local->bytesToWrite();
        if (room > 0) {
            m_local->write(m_tls->read(room));
        }
        if (m_tls->state() == QAbstractSocket::UnconnectedState && m_tls->bytesAvailable() == 0) {
            m_local->disconnectFromHost();
        }
    }

    void close()
    {
        m_local->disconnect(this);
        m_tls->disconnect(this);
        m_local->abort();
        m_tls->abort();
        deleteLater();
    }

    QTcpSocket *m_local;
    QUrl m_remote;
    QByteArray m_fingerprint;
    QByteArray m_path;
    QSslSocket *m_tls;
    QByteArray m_request;
    QByteArray m_outgoing;
    QByteArray m_headers;
    bool m_headersDone = false;
};
}

SharePlayMediaProxy::SharePlayMediaProxy(QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
{
    connect(m_server, &QTcpServer::newConnection, this, [this]() {
        while (auto *socket = m_server->nextPendingConnection()) {
            new Transfer(socket, m_remote, m_fingerprint, m_path, this);
        }
    });
}

QUrl SharePlayMediaProxy::start(const QUrl &remote, const QByteArray &fingerprint)
{
    stop();
    if (remote.scheme() != QStringLiteral("https") || fingerprint.size() != 64 || !m_server->listen(QHostAddress::LocalHost, 0)) {
        return {};
    }
    m_remote = remote;
    m_fingerprint = fingerprint;
    QByteArray random(32, Qt::Uninitialized);
    for (auto &byte : random) {
        byte = char(QRandomGenerator::system()->generate() & 0xff);
    }
    m_path = '/' + random.toHex();
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(QStringLiteral("127.0.0.1"));
    url.setPort(m_server->serverPort());
    url.setPath(QString::fromLatin1(m_path));
    return url;
}

void SharePlayMediaProxy::stop()
{
    m_server->close();
    const auto objects = children();
    for (auto *object : objects) {
        if (object != m_server) {
            delete object;
        }
    }
    m_remote.clear();
    m_fingerprint.clear();
    m_path.clear();
}
