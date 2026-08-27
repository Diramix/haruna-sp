/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "shareplayserver.h"
#include "shareplayprotocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QWebSocket>
#include <QWebSocketServer>

using namespace Qt::StringLiterals;

SharePlayServer::SharePlayServer(QObject *parent)
    : QObject(parent)
    , m_server(new QWebSocketServer(u"Haruna SharePlay"_s, QWebSocketServer::NonSecureMode, this))
{
    connect(m_server, &QWebSocketServer::newConnection, this, &SharePlayServer::onNewConnection);
}

SharePlayServer::~SharePlayServer()
{
    stop();
}

bool SharePlayServer::start(quint16 port)
{
    if (m_server->isListening()) {
        stop();
    }
    m_clock.start();
    return m_server->listen(QHostAddress::Any, port);
}

double SharePlayServer::hostTime() const
{
    return m_clock.isValid() ? m_clock.nsecsElapsed() / 1000000.0 : 0.0;
}

void SharePlayServer::stop()
{
    const auto peers = m_peers.keys();
    for (QWebSocket *peer : peers) {
        peer->close(QWebSocketProtocol::CloseCodeGoingAway);
        peer->deleteLater();
    }
    m_peers.clear();
    if (m_server->isListening()) {
        m_server->close();
    }
    Q_EMIT peersChanged();
}

bool SharePlayServer::isListening() const
{
    return m_server->isListening();
}

QString SharePlayServer::errorString() const
{
    return m_server->errorString();
}

void SharePlayServer::setPassword(const QString &password)
{
    m_password = password;
}

void SharePlayServer::setMedia(const QString &name, double duration, bool available, quint64 token)
{
    m_mediaName = name;
    m_mediaDuration = duration;
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
        mediaUrl = u"http://%1:%2/media?v=%3"_s.arg(host, QString::number(SharePlayProtocol::MediaPort), QString::number(m_mediaToken));
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
        connect(peer, &QWebSocket::textMessageReceived, this, [this, peer](const QString &message) {
            onTextMessage(peer, message);
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
