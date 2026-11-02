/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "shareplayclient.h"
#include "shareplayprotocol.h"

#include <cmath>

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QWebSocket>

using namespace Qt::StringLiterals;

namespace
{
constexpr int PingIntervalMs = 4000;
constexpr int ConnectTimeoutMs = 15000;
constexpr int LivenessTimeoutMs = 15000;

bool nonnegativeNumber(const QJsonValue &value)
{
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0.0;
}

bool validState(const QJsonObject &object)
{
    return object.value(u"paused"_s).isBool() && nonnegativeNumber(object.value(u"position"_s)) && nonnegativeNumber(object.value(u"hostTime"_s));
}
/** the first samples come fast so the clock offset settles right after joining */
constexpr int FastPingIntervalMs = 400;
constexpr int FastPingCount = 10;
/** a sample this much older than the best one is taken anyway, the route may have changed */
constexpr int MaxSamplesBeforeReset = 20;
/** never extrapolate further than this, a state that old means something went wrong */
constexpr double MaxExtrapolationMs = 5000.0;
}

SharePlayClient::SharePlayClient(QObject *parent)
    : QObject(parent)
    , m_pingTimer(new QTimer(this))
    , m_timeoutTimer(new QTimer(this))
{
    m_pingTimer->setInterval(FastPingIntervalMs);

    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this, [this]() {
        finish(m_accepted ? u"SharePlay host stopped responding"_s : u"SharePlay connection timed out"_s);
    });
    connect(m_pingTimer, &QTimer::timeout, this, &SharePlayClient::sendPing);
}

double SharePlayClient::now() const
{
    return m_clock.isValid() ? m_clock.nsecsElapsed() / 1000000.0 : 0.0;
}

void SharePlayClient::sendPing()
{
    if (++m_pingCount == FastPingCount) {
        m_pingTimer->setInterval(PingIntervalMs);
    }
    send(QJsonObject{{u"type"_s, SharePlayProtocol::Type::Ping}, {u"t0"_s, now()}});
}

void SharePlayClient::takeClockSample(double t0, double t3, double hostTime)
{
    const double rtt = t3 - t0;
    if (rtt < 0.0) {
        return;
    }
    m_latency = rtt / 2000.0;

    // the sample with the smallest round trip is the least distorted one, keep that
    // estimate until a better sample shows up or the current one gets stale
    ++m_samplesSinceBest;
    if (m_clockValid && rtt > m_bestRtt && m_samplesSinceBest < MaxSamplesBeforeReset) {
        return;
    }

    m_bestRtt = rtt;
    m_samplesSinceBest = 0;
    m_clockValid = true;
    m_clockOffset = hostTime + rtt / 2.0 - t3;
}

bool SharePlayClient::hasHostState() const
{
    return m_accepted && m_hasHostState && m_clockValid && (m_hostPaused || now() + m_clockOffset - m_hostStateTime <= MaxExtrapolationMs);
}

bool SharePlayClient::hostPaused() const
{
    return m_hostPaused;
}

double SharePlayClient::hostPositionNow() const
{
    if (m_hostPaused || !hasHostState()) {
        return m_hostPosition;
    }
    const double hostNow = now() + m_clockOffset;
    const double elapsed = qBound(0.0, hostNow - m_hostStateTime, MaxExtrapolationMs);
    return m_hostPosition + elapsed / 1000.0;
}

SharePlayClient::~SharePlayClient()
{
    disconnectFromServer();
}

void SharePlayClient::connectToServer(const QUrl &url, const QString &name, const QString &password)
{
    disconnectFromServer();
    ++m_attempt;
    m_name = name;
    m_password = password;
    m_latency = 0.0;
    m_clockOffset = 0.0;
    m_bestRtt = 0.0;
    m_samplesSinceBest = 0;
    m_pingCount = 0;
    m_clockValid = false;
    m_hasHostState = false;
    m_hostPosition = 0.0;
    m_hostStateTime = 0.0;
    m_hostPaused = true;
    m_pingTimer->setInterval(FastPingIntervalMs);
    m_controlUrl = url;
    m_active = true;
    m_socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    connect(m_socket, &QWebSocket::connected, this, &SharePlayClient::onConnected);
    connect(m_socket, &QWebSocket::textMessageReceived, this, &SharePlayClient::onTextMessage);
    connect(m_socket, &QWebSocket::disconnected, this, [this, socket = m_socket]() {
        // Qt may emit disconnected before errorOccurred for a failed handshake.
        // Let the error carry its diagnostic, without ending a replacement attempt.
        QTimer::singleShot(0, this, [this, socket]() {
            if (m_socket == socket) {
                finish();
            }
        });
    });
    connect(m_socket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        finish(m_socket->errorString());
    });
    m_timeoutTimer->start(ConnectTimeoutMs);
    m_socket->open(url);
}

void SharePlayClient::disconnectFromServer()
{
    m_active = false;
    m_pingTimer->stop();
    m_timeoutTimer->stop();
    m_accepted = false;
    m_hasHostState = false;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}

void SharePlayClient::finish(const QString &message, bool rejectedByHost)
{
    if (!m_active) {
        return;
    }
    const quint64 attempt = m_attempt;
    disconnectFromServer();
    if (rejectedByHost) {
        Q_EMIT rejected(message);
    } else {
        Q_EMIT disconnected();
        if (m_attempt == attempt && !message.isEmpty()) {
            Q_EMIT errorOccurred(message);
        }
    }
}

bool SharePlayClient::validMediaUrl(const QUrl &url) const
{
    if (url.isEmpty()) {
        return true;
    }
    // Only literal addresses are accepted: never resolve a host supplied by the peer.
    const QHostAddress address(url.host());
    return url.isValid() && url.scheme() == u"http"_s && !url.authority().contains(u'@') && !url.hasFragment() && !address.isNull() && m_socket
        && address == m_socket->peerAddress() && url.port(80) == m_controlUrl.port(m_controlUrl.scheme() == u"wss"_s ? 443 : 80);
}

bool SharePlayClient::isConnected() const
{
    return m_accepted;
}

double SharePlayClient::latency() const
{
    return m_latency;
}

void SharePlayClient::requestPause(bool paused)
{
    if (!m_accepted) {
        return;
    }
    send(QJsonObject{{u"type"_s, SharePlayProtocol::Type::RequestPause}, {u"paused"_s, paused}});
}

void SharePlayClient::send(const QJsonObject &message)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(message).toJson(QJsonDocument::Compact)));
}

void SharePlayClient::onConnected()
{
    send(QJsonObject{
        {u"type"_s, SharePlayProtocol::Type::Hello},
        {u"protocol"_s, SharePlayProtocol::Version},
        {u"name"_s, m_name},
        {u"password"_s, m_password},
    });
}

void SharePlayClient::onTextMessage(const QString &message)
{
    if (!m_active) {
        return;
    }
    const QJsonObject object = QJsonDocument::fromJson(message.toUtf8()).object();
    const QString type = object.value(u"type"_s).toString();

    if (type == SharePlayProtocol::Type::Welcome) {
        if (m_accepted) {
            return;
        }
        const QJsonValue media = object.value(u"mediaUrl"_s);
        const QUrl url(media.toString(), QUrl::StrictMode);
        if (!validState(object) || !media.isString() || !validMediaUrl(url)) {
            finish(u"Invalid SharePlay welcome"_s);
            return;
        }
        m_accepted = true;
        m_timeoutTimer->start(LivenessTimeoutMs);
        m_clock.start();
        m_hostPosition = object.value(u"position"_s).toDouble();
        m_hostPaused = object.value(u"paused"_s).toBool(true);
        m_hostStateTime = object.value(u"hostTime"_s).toDouble();
        m_hasHostState = true;
        m_pingTimer->start();
        sendPing();
        const quint64 attempt = m_attempt;
        Q_EMIT connected();
        if (!m_active || m_attempt != attempt) {
            return;
        }
        Q_EMIT mediaReceived(url, object.value(u"mediaName"_s).toString(), object.value(u"duration"_s).toDouble());
        if (!m_active || m_attempt != attempt) {
            return;
        }
        Q_EMIT peersReceived(object.value(u"peers"_s).toVariant().toStringList());
        if (!m_active || m_attempt != attempt) {
            return;
        }
        Q_EMIT stateReceived(object.value(u"position"_s).toDouble(), object.value(u"paused"_s).toBool(true));
        return;
    }

    if (type == SharePlayProtocol::Type::Error) {
        finish(object.value(u"reason"_s).toString(), true);
        return;
    }

    if (!m_accepted) {
        return;
    }

    if (type == SharePlayProtocol::Type::State) {
        if (!validState(object)) {
            return;
        }
        m_hostPosition = object.value(u"position"_s).toDouble();
        m_hostPaused = object.value(u"paused"_s).toBool(true);
        m_hostStateTime = object.value(u"hostTime"_s).toDouble();
        m_hasHostState = true;
        Q_EMIT stateReceived(m_hostPosition, m_hostPaused);
        return;
    }

    if (type == SharePlayProtocol::Type::Media) {
        const QJsonValue media = object.value(u"mediaUrl"_s);
        const QUrl url(media.toString(), QUrl::StrictMode);
        if (!media.isString() || !validMediaUrl(url)) {
            finish(u"Invalid SharePlay media URL"_s);
            return;
        }
        // Even a repeated URL may describe a replacement at the same path.
        m_hasHostState = false;
        m_hostPosition = 0.0;
        m_hostStateTime = 0.0;
        m_hostPaused = true;
        Q_EMIT mediaReceived(url, object.value(u"mediaName"_s).toString(), object.value(u"duration"_s).toDouble());
        return;
    }

    if (type == SharePlayProtocol::Type::Peers) {
        Q_EMIT peersReceived(object.value(u"list"_s).toVariant().toStringList());
        return;
    }

    if (type == SharePlayProtocol::Type::Notice) {
        Q_EMIT noticeReceived(object.value(u"text"_s).toString());
        return;
    }

    if (type == SharePlayProtocol::Type::Pong) {
        if (!nonnegativeNumber(object.value(u"t0"_s)) || !nonnegativeNumber(object.value(u"hostTime"_s)) || object.value(u"t0"_s).toDouble() > now()) {
            return;
        }
        m_timeoutTimer->start(LivenessTimeoutMs);
        takeClockSample(object.value(u"t0"_s).toDouble(), now(), object.value(u"hostTime"_s).toDouble());
    }
}
