/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "shareplaymanager.h"
#include "shareplayclient.h"
#include "shareplaymediaserver.h"
#include "shareplayprotocol.h"
#include "shareplayserver.h"

#include <QNetworkInterface>

#include <KLocalizedString>

using namespace Qt::StringLiterals;

namespace
{
/** how long a hard seek is given to land before the drift is trusted again */
constexpr int SeekSettleMs = 1500;
/** while the host is paused the position has to match exactly, there is nothing to smooth */
constexpr double PausedThreshold = 0.05;
/** a hard seek always overshoots a little, this covers the time it takes to land */
constexpr double SeekLead = 0.12;
}

SharePlayManager::SharePlayManager(QObject *parent)
    : QObject(parent)
    , m_server(new SharePlayServer(this))
    , m_mediaServer(new SharePlayMediaServer(this))
    , m_client(new SharePlayClient(this))
{
    m_server->setMediaServer(m_mediaServer);

    connect(m_server, &SharePlayServer::peersChanged, this, [this]() {
        m_peers = m_server->peerNames();
        Q_EMIT peersChanged();
    });

    connect(m_server, &SharePlayServer::pauseRequested, this, [this](const QString &name, bool paused, QWebSocket *peer) {
        Q_EMIT peerRequestedPause(name, paused);
        m_server->sendNotice(peer,
                             paused ? i18nc("@info notice shown to the client that asked for a pause", "You requested a pause")
                                    : i18nc("@info notice shown to the client that asked to resume", "You requested to resume"));
    });

    connect(m_client, &SharePlayClient::connected, this, [this]() {
        setState(State::Connected);
        setStatusText(i18nc("@info:status shareplay", "Connected"));
    });

    connect(m_client, &SharePlayClient::disconnected, this, [this]() {
        leave();
        Q_EMIT notice(i18nc("@info osd message", "Disconnected from the SharePlay host"));
    });

    connect(m_client, &SharePlayClient::rejected, this, [this](const QString &reason) {
        QString message;
        if (reason == SharePlayProtocol::Error::BadPassword) {
            message = i18nc("@info", "Wrong password");
        } else if (reason == SharePlayProtocol::Error::BadProtocol) {
            message = i18nc("@info", "Incompatible SharePlay version");
        } else if (reason == SharePlayProtocol::Error::BadName) {
            message = i18nc("@info", "Invalid name");
        } else {
            message = i18nc("@info", "Connection rejected by the host");
        }
        leave();
        setStatusText(message);
        Q_EMIT errorOccurred(message);
    });

    connect(m_client, &SharePlayClient::errorOccurred, this, [this](const QString &message) {
        setStatusText(message);
        Q_EMIT errorOccurred(message);
    });

    connect(m_client, &SharePlayClient::peersReceived, this, [this](const QStringList &peers) {
        m_peers = peers;
        Q_EMIT peersChanged();
    });

    connect(m_client, &SharePlayClient::noticeReceived, this, &SharePlayManager::notice);

    connect(m_client, &SharePlayClient::mediaReceived, this, [this](const QUrl &url, const QString &title, double) {
        setMediaTitle(title);
        if (url.isValid() && !url.isEmpty()) {
            setMediaUrl(url);
            Q_EMIT mediaAvailable(url, title);
        } else {
            setMediaUrl(QUrl());
        }
    });

    connect(m_client, &SharePlayClient::stateReceived, this, [this](double, bool paused) {
        Q_EMIT applyPause(paused);
        // react to a seek by the host right away instead of waiting for the next tick
        evaluateSync();
    });
}

SharePlayManager::~SharePlayManager()
{
    teardown();
}

SharePlayManager::Mode SharePlayManager::mode() const
{
    return m_mode;
}

SharePlayManager::State SharePlayManager::state() const
{
    return m_state;
}

bool SharePlayManager::isActive() const
{
    return m_mode != Mode::Off;
}

bool SharePlayManager::isHost() const
{
    return m_mode == Mode::Host;
}

bool SharePlayManager::canControl() const
{
    return m_mode != Mode::Client;
}

QStringList SharePlayManager::peers() const
{
    return m_peers;
}

QString SharePlayManager::statusText() const
{
    return m_statusText;
}

QString SharePlayManager::mediaTitle() const
{
    return m_mediaTitle;
}

QUrl SharePlayManager::mediaUrl() const
{
    return m_mediaUrl;
}

void SharePlayManager::setMediaUrl(const QUrl &url)
{
    if (m_mediaUrl == url) {
        return;
    }
    m_mediaUrl = url;
    Q_EMIT mediaUrlChanged();
}

int SharePlayManager::controlPort() const
{
    return SharePlayProtocol::ControlPort;
}

double SharePlayManager::drift() const
{
    return m_drift;
}

void SharePlayManager::reportPosition(double position)
{
    m_lastPosition = position;
    evaluateSync();
}

void SharePlayManager::evaluateSync()
{
    if (m_mode != Mode::Client || !m_client->hasHostState()) {
        return;
    }

    const bool hostPaused = m_client->hostPaused();
    const double target = m_client->hostPositionNow();
    const double drift = target - m_lastPosition;
    setDrift(drift);

    // a seek needs a moment to land, until then the measured drift means nothing
    if (m_seekClock.isValid() && m_seekClock.elapsed() < SeekSettleMs) {
        return;
    }

    const double threshold = hostPaused ? PausedThreshold : m_syncThreshold;
    if (qAbs(drift) > threshold) {
        setSpeedFactor(1.0);
        m_seekClock.restart();
        Q_EMIT applySeek(hostPaused ? target : target + SeekLead);
        return;
    }

    // the playback speed stays pinned to 1.0 during a SharePlay session, so a small
    // drift inside the tolerance is simply left to the next hard seek to correct
    setSpeedFactor(1.0);
}

void SharePlayManager::setDrift(double value)
{
    if (qFuzzyCompare(m_drift, value)) {
        return;
    }
    m_drift = value;
    Q_EMIT driftChanged();
}

void SharePlayManager::setSpeedFactor(double speed)
{
    if (qAbs(m_speedFactor - speed) < 0.001) {
        return;
    }
    m_speedFactor = speed;
    Q_EMIT applySpeed(speed);
}

double SharePlayManager::syncThreshold() const
{
    return m_syncThreshold;
}

void SharePlayManager::setSyncThreshold(double value)
{
    if (qFuzzyCompare(m_syncThreshold, value)) {
        return;
    }
    m_syncThreshold = value;
    Q_EMIT syncThresholdChanged();
}

bool SharePlayManager::startHost(const QString &password, bool streamMedia)
{
    teardown();

    m_streamMedia = streamMedia;
    m_server->setPassword(password);
    if (!m_server->start(SharePlayProtocol::ControlPort)) {
        const QString message = i18nc("@info", "Could not start the server: %1", m_server->errorString());
        setStatusText(message);
        Q_EMIT errorOccurred(message);
        return false;
    }

    setMode(Mode::Host);
    setState(State::Connected);
    setStatusText(i18nc("@info:status shareplay", "Hosting"));
    return true;
}

void SharePlayManager::connectToHost(const QString &address, const QString &nickname, const QString &password)
{
    teardown();

    m_nickname = nickname;
    setMode(Mode::Client);
    setState(State::Connecting);
    setStatusText(i18nc("@info:status shareplay", "Connecting…"));

    QUrl url;
    url.setScheme(u"ws"_s);
    url.setHost(address);
    url.setPort(SharePlayProtocol::ControlPort);
    m_client->connectToServer(url, nickname, password);
}

void SharePlayManager::leave()
{
    if (m_mode == Mode::Off) {
        return;
    }
    teardown();
    setMode(Mode::Off);
    setState(State::Disconnected);
    setStatusText(i18nc("@info:status shareplay", "Not connected"));
}

void SharePlayManager::setHostMedia(const QUrl &fileUrl, const QString &title, double duration)
{
    if (m_mode != Mode::Host) {
        return;
    }

    // a new file gets a new token, so its url differs from the previous one and
    // the clients actually reload instead of assuming nothing changed
    if (fileUrl != m_hostMediaUrl) {
        m_hostMediaUrl = fileUrl;
        ++m_mediaToken;
    }

    const bool isLocal = fileUrl.isLocalFile();
    const bool canStream = m_streamMedia && isLocal && m_server->isListening();
    m_mediaServer->setFile(canStream ? fileUrl.toLocalFile() : QString());

    setMediaTitle(title);
    m_server->setMedia(title, duration, canStream, m_mediaToken);
}

void SharePlayManager::publishState(double position, bool paused)
{
    m_lastPosition = position;
    m_lastPaused = paused;

    if (m_mode != Mode::Host) {
        return;
    }
    m_server->broadcastState(position, paused);
}

void SharePlayManager::requestPause(bool paused)
{
    if (m_mode != Mode::Client) {
        return;
    }
    m_client->requestPause(paused);
    Q_EMIT notice(paused ? i18nc("@info osd message", "You requested a pause") : i18nc("@info osd message", "You requested to resume"));
}

QStringList SharePlayManager::localAddresses() const
{
    QStringList addresses;
    const auto interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &interface : interfaces) {
        if (!interface.flags().testFlag(QNetworkInterface::IsUp) || interface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        const auto entries = interface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress address = entry.ip();
            if (address.protocol() == QAbstractSocket::IPv4Protocol) {
                addresses.append(address.toString());
            }
        }
    }
    addresses.removeDuplicates();
    return addresses;
}

void SharePlayManager::setMode(Mode mode)
{
    if (m_mode == mode) {
        return;
    }
    m_mode = mode;
    Q_EMIT modeChanged();
}

void SharePlayManager::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    Q_EMIT stateChanged();
}

void SharePlayManager::setStatusText(const QString &text)
{
    if (m_statusText == text) {
        return;
    }
    m_statusText = text;
    Q_EMIT statusTextChanged();
}

void SharePlayManager::setMediaTitle(const QString &title)
{
    if (m_mediaTitle == title) {
        return;
    }
    m_mediaTitle = title;
    Q_EMIT mediaTitleChanged();
}

void SharePlayManager::teardown()
{
    setSpeedFactor(1.0);
    m_seekClock.invalidate();
    setDrift(0.0);
    m_client->disconnectFromServer();
    m_server->stop();
    m_mediaServer->setFile(QString());
    m_hostMediaUrl.clear();
    setMediaTitle(QString());
    setMediaUrl(QUrl());
    if (!m_peers.isEmpty()) {
        m_peers.clear();
        Q_EMIT peersChanged();
    }
}
