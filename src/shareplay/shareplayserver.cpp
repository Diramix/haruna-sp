/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "shareplayserver.h"
#include "shareplaymediaserver.h"
#include "shareplayprotocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSslCertificate>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <QWebSocket>
#include <QWebSocketServer>
#include <cmath>

using namespace Qt::StringLiterals;

namespace
{
class SharePlayListener : public QTcpServer
{
public:
    using QTcpServer::QTcpServer;

protected:
    void incomingConnection(qintptr descriptor) override
    {
        auto *socket = new QSslSocket(this);
        if (!socket->setSocketDescriptor(descriptor)) {
            socket->deleteLater();
            return;
        }
        addPendingConnection(socket);
    }
};
}

SharePlayServer::SharePlayServer(QObject *parent)
    : QObject(parent)
    , m_listener(new SharePlayListener(this))
    , m_server(new QWebSocketServer(u"Haruna SharePlay"_s, QWebSocketServer::NonSecureMode, this))
{
    // the WebSocket server never listens on its own, it only gets the sockets
    // that turn out to be a WebSocket handshake, the rest go to the media server
    connect(m_listener, &QTcpServer::newConnection, this, &SharePlayServer::onTcpConnection);
    connect(m_server, &QWebSocketServer::newConnection, this, &SharePlayServer::onNewConnection);
}

SharePlayServer::~SharePlayServer()
{
    stop();
}

void SharePlayServer::setMediaServer(SharePlayMediaServer *mediaServer)
{
    m_mediaServer = mediaServer;
}

bool SharePlayServer::start(quint16 port, Transport transport, const QSslConfiguration &tls)
{
    stop();
    m_error.clear();
    if (transport == Transport::Tls) {
        if (!QSslSocket::supportsSsl()) {
            m_error = u"TLS backend is unavailable"_s;
            return false;
        }
        if (tls.localCertificate().isNull() || tls.privateKey().isNull()) {
            m_error = u"TLS requires a certificate and private key"_s;
            return false;
        }
    } else if (transport != Transport::Insecure) {
        m_error = u"Invalid SharePlay transport"_s;
        return false;
    }
    m_transport = transport;
    m_tls = tls;
    m_clock.start();
    m_running = m_listener->listen(QHostAddress::Any, port);
    if (!m_running) {
        m_error = m_listener->errorString();
    }
    return m_running;
}

double SharePlayServer::hostTime() const
{
    return m_clock.isValid() ? m_clock.nsecsElapsed() / 1000000.0 : 0.0;
}

void SharePlayServer::stop()
{
    m_running = false;
    ++m_session;
    m_listener->close();
    m_peers.clear();
    const auto webSockets = m_webSockets;
    m_webSockets.clear();
    for (QWebSocket *peer : webSockets) {
        peer->abort();
        peer->deleteLater();
    }
    const auto tcpSockets = m_tcpSockets;
    m_tcpSockets.clear();
    for (const QPointer<QTcpSocket> &socket : tcpSockets) {
        if (socket) {
            socket->abort();
            if (socket) {
                socket->deleteLater();
            }
        }
    }
    while (QWebSocket *peer = m_server->nextPendingConnection()) {
        peer->abort();
        peer->deleteLater();
    }
    if (m_mediaServer) {
        m_mediaServer->stop();
    }
    m_mediaName.clear();
    m_mediaAvailable = false;
    m_mediaDuration = 0.0;
    m_mediaToken = 0;
    m_lastPosition = 0.0;
    m_lastStateTime = 0.0;
    m_lastPaused = true;
    m_seq = 0;
    Q_EMIT peersChanged();
}

bool SharePlayServer::isListening() const
{
    return m_listener->isListening();
}

QString SharePlayServer::errorString() const
{
    return m_error;
}

void SharePlayServer::onTcpConnection()
{
    // WebSocket handoff may disconnect the original socket's signals; guarded
    // entries still expire, and are pruned before accepting more connections.
    for (auto it = m_tcpSockets.begin(); it != m_tcpSockets.end();) {
        if (it.value().isNull()) {
            it = m_tcpSockets.erase(it);
        } else {
            ++it;
        }
    }
    while (QTcpSocket *socket = m_listener->nextPendingConnection()) {
        if (!m_running) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_tcpSockets.insert(socket, socket);
        connect(socket, &QObject::destroyed, this, [this, socket]() {
            m_tcpSockets.remove(socket);
        });
        auto *deadline = new QTimer(socket);
        deadline->setObjectName(u"shareplayHeaderDeadline"_s);
        deadline->setSingleShot(true);
        connect(deadline, &QTimer::timeout, socket, &QTcpSocket::abort);
        deadline->start(15000);
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            dispatch(socket);
        });
        if (m_transport == Transport::Tls) {
            auto *secureSocket = static_cast<QSslSocket *>(socket);
            secureSocket->setSslConfiguration(m_tls);
            secureSocket->startServerEncryption();
        }
    }
}

void SharePlayServer::dispatch(QTcpSocket *socket)
{
    if (!m_running || !m_tcpSockets.contains(socket)) {
        socket->abort();
        return;
    }
    const QByteArray peeked = socket->peek(16 * 1024);
    const int headerEnd = peeked.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        // wait for the rest of the request headers, unless the client floods us
        if (peeked.size() >= 16 * 1024) {
            socket->disconnectFromHost();
        }
        return;
    }

    // the request is complete, this socket is now owned by whoever handles it
    disconnect(socket, &QTcpSocket::readyRead, this, nullptr);
    if (auto *deadline = socket->findChild<QTimer *>(u"shareplayHeaderDeadline"_s)) {
        deadline->stop();
    }
    const QByteArray header = peeked.left(headerEnd);

    if (isWebSocketUpgrade(header)) {
        disconnect(socket, &QTcpSocket::disconnected, socket, nullptr);
        m_server->handleConnection(socket);
        return;
    }

    if (m_mediaServer) {
        m_mediaServer->handleSocket(socket);
        return;
    }

    socket->disconnectFromHost();
}

bool SharePlayServer::isWebSocketUpgrade(const QByteArray &header)
{
    const QByteArray lower = header.toLower();
    return lower.contains("upgrade:") && lower.contains("websocket");
}

void SharePlayServer::setPassword(const QString &password)
{
    m_password = password;
}

void SharePlayServer::setMedia(const QString &name, double duration, bool available, quint64 token)
{
    const bool changed = token != m_mediaToken || available != m_mediaAvailable;
    if (changed) {
        m_lastPosition = 0.0;
        m_lastStateTime = hostTime();
        m_lastPaused = true;
    }
    if (m_mediaServer) {
        m_mediaServer->setToken(token);
    }
    m_mediaName = name;
    m_mediaDuration = std::isfinite(duration) && duration >= 0.0 ? duration : 0.0;
    m_mediaToken = token;
    m_mediaAvailable = available;
    for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it) {
        send(it.key(), mediaMessageFor(it.key()));
    }
}

QJsonObject SharePlayServer::mediaMessageFor(QWebSocket *peer) const
{
    QString mediaUrl;
    if (m_mediaAvailable) {
        // the address the peer used to reach us is the one it can reach the media server on
        QHostAddress address = peer->localAddress();
        bool isIPv4 = false;
        const quint32 ipv4 = address.toIPv4Address(&isIPv4);
        const QString host = isIPv4 ? QHostAddress(ipv4).toString() : u"[%1]"_s.arg(address.toString());
        // the token makes the url change whenever the host plays a different file,
        // otherwise the clients can't tell the new stream from the previous one
        const QString scheme = m_transport == Transport::Tls ? u"https"_s : u"http"_s;
        mediaUrl = u"%1://%2:%3/media?v=%4"_s.arg(scheme, host, QString::number(peer->localPort()), QString::number(m_mediaToken));
    }

    return QJsonObject{
        {u"type"_s, SharePlayProtocol::Type::Media},
        {u"mediaUrl"_s, mediaUrl},
        {u"mediaName"_s, m_mediaName},
        {u"duration"_s, m_mediaDuration},
    };
}

void SharePlayServer::broadcastState(double position, bool paused)
{
    if (!m_running || !std::isfinite(position) || position < 0.0) {
        return;
    }
    m_lastPosition = position;
    m_lastPaused = paused;
    // the position is only meaningful together with the instant it was sampled at
    m_lastStateTime = hostTime();
    broadcast(QJsonObject{
        {u"type"_s, SharePlayProtocol::Type::State},
        {u"position"_s, position},
        {u"paused"_s, paused},
        {u"hostTime"_s, m_lastStateTime},
        {u"seq"_s, static_cast<double>(++m_seq)},
    });
}

void SharePlayServer::sendNotice(QWebSocket *peer, const QString &text)
{
    if (!peer || !m_peers.contains(peer)) {
        return;
    }
    send(peer, QJsonObject{{u"type"_s, SharePlayProtocol::Type::Notice}, {u"text"_s, text}});
}

QStringList SharePlayServer::peerNames() const
{
    QStringList names = m_peers.values();
    names.sort();
    return names;
}

void SharePlayServer::send(QWebSocket *peer, const QJsonObject &message)
{
    peer->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void SharePlayServer::broadcast(const QJsonObject &message)
{
    const QString payload = QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact));
    for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it) {
        it.key()->sendTextMessage(payload);
    }
}

void SharePlayServer::onNewConnection()
{
    while (QWebSocket *peer = m_server->nextPendingConnection()) {
        if (!m_running) {
            peer->abort();
            peer->deleteLater();
            continue;
        }
        m_webSockets.insert(peer);
        const quint64 session = m_session;
        connect(peer, &QWebSocket::textMessageReceived, this, [this, peer, session](const QString &message) {
            if (m_running && session == m_session && m_webSockets.contains(peer)) {
                onTextMessage(peer, message);
            }
        });
        QTimer::singleShot(15000, peer, [this, peer, session]() {
            if (session == m_session && !m_peers.contains(peer)) {
                peer->abort();
            }
        });
        connect(peer, &QWebSocket::disconnected, this, [this, peer]() {
            removePeer(peer);
        });
    }
}

void SharePlayServer::onTextMessage(QWebSocket *peer, const QString &message)
{
    const QJsonObject object = QJsonDocument::fromJson(message.toUtf8()).object();
    const QString type = object.value(u"type"_s).toString();
    const bool isKnown = m_peers.contains(peer);

    if (type == SharePlayProtocol::Type::Hello) {
        if (isKnown) {
            return;
        }
        if (object.value(u"protocol"_s).toInt() != SharePlayProtocol::Version) {
            rejectPeer(peer, SharePlayProtocol::Error::BadProtocol);
            return;
        }
        if (object.value(u"password"_s).toString() != m_password) {
            rejectPeer(peer, SharePlayProtocol::Error::BadPassword);
            return;
        }
        const QString name = object.value(u"name"_s).toString().trimmed().left(32);
        if (name.isEmpty()) {
            rejectPeer(peer, SharePlayProtocol::Error::BadName);
            return;
        }

        m_peers.insert(peer, name);

        QJsonObject welcome = mediaMessageFor(peer);
        welcome[u"type"_s] = SharePlayProtocol::Type::Welcome;
        welcome[u"position"_s] = m_lastPosition;
        welcome[u"paused"_s] = m_lastPaused;
        welcome[u"hostTime"_s] = m_lastStateTime;
        welcome[u"peers"_s] = QJsonArray::fromStringList(peerNames());
        send(peer, welcome);

        broadcastPeers();
        Q_EMIT peersChanged();
        return;
    }

    if (!isKnown) {
        return;
    }

    if (type == SharePlayProtocol::Type::RequestPause) {
        if (!object.value(u"paused"_s).isBool()) {
            return;
        }
        Q_EMIT pauseRequested(m_peers.value(peer), object.value(u"paused"_s).toBool(), peer);
        return;
    }

    if (type == SharePlayProtocol::Type::Ping) {
        // t0 is echoed back untouched so the client can measure the round trip
        send(peer,
             QJsonObject{
                 {u"type"_s, SharePlayProtocol::Type::Pong},
                 {u"t0"_s, object.value(u"t0"_s).toDouble()},
                 {u"hostTime"_s, hostTime()},
                 {u"position"_s, m_lastPosition},
                 {u"paused"_s, m_lastPaused},
             });
    }
}

void SharePlayServer::removePeer(QWebSocket *peer)
{
    m_webSockets.remove(peer);
    const bool wasKnown = m_peers.remove(peer) > 0;
    peer->deleteLater();
    if (wasKnown) {
        broadcastPeers();
        Q_EMIT peersChanged();
    }
}

void SharePlayServer::rejectPeer(QWebSocket *peer, const QString &reason)
{
    send(peer, QJsonObject{{u"type"_s, SharePlayProtocol::Type::Error}, {u"reason"_s, reason}});
    peer->close(QWebSocketProtocol::CloseCodePolicyViolated, reason);
}

void SharePlayServer::broadcastPeers()
{
    broadcast(QJsonObject{
        {u"type"_s, SharePlayProtocol::Type::Peers},
        {u"list"_s, QJsonArray::fromStringList(peerNames())},
    });
}
