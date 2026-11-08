/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "shareplaymanager.h"
#include "shareplayclient.h"
#include "shareplaymediaproxy.h"
#include "shareplaymediaserver.h"
#include "shareplayprotocol.h"
#include "shareplayserver.h"
#include "shareplaytls.h"

#include <QCryptographicHash>

#include <QRandomGenerator>
#include <QSslCertificate>
#include <QTimer>
#include <QWebSocket>

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
    , m_mediaProxy(new SharePlayMediaProxy(this))
    , m_client(new SharePlayClient(this))
{
    m_mediaToken = QRandomGenerator::global()->generate64();
    m_server->setMediaServer(m_mediaServer);
    connect(m_mediaServer, &SharePlayMediaServer::fileInvalidated, this, [this]() {
        if (m_mode != Mode::Host) {
            return;
        }
        ++m_mediaToken;
        m_server->setMedia(m_mediaTitle, 0.0, false, m_mediaToken);
        Q_EMIT errorOccurred(i18nc("@info", "The shared file changed or became unavailable. Select the file again to share it."));
    });

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

    connect(m_client, &SharePlayClient::certificateConfirmationRequired, this, [this](const QUrl &url, const QByteArray &fingerprint) {
        if (m_mode != Mode::Client || m_state != State::Connecting || url != m_pendingUrl) {
            return;
        }
        m_initialSecureAttempt = false;
        const quint64 generation = m_connectionGeneration;
        m_pendingFingerprint = QString::fromLatin1(fingerprint);
        setStatusText(i18nc("@info:status shareplay", "Confirm the host certificate"));
        if (generation != m_connectionGeneration || m_pendingFingerprint.isEmpty())
            return;
        Q_EMIT pendingFingerprintChanged();
    });

    connect(m_client, &SharePlayClient::connected, this, [this]() {
        m_pendingPassword.clear();
        setState(State::Connected);
        setStatusText(i18nc("@info:status shareplay", "Connected"));
    });

    connect(m_client, &SharePlayClient::disconnected, this, [this]() {
        if (m_initialSecureAttempt && m_mode == Mode::Client && m_state == State::Connecting) {
            m_initialSecureAttempt = false;
            m_ignoreInitialError = true;
            const quint64 generation = m_connectionGeneration;
            QTimer::singleShot(0, this, [this, generation]() {
                if (m_connectionGeneration == generation && m_ignoreInitialError) {
                    m_ignoreInitialError = false;
                    startInsecureProbe();
                }
            });
            return;
        }
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
        if (m_ignoreInitialError) {
            return;
        }
        setStatusText(message);
        Q_EMIT errorOccurred(message);
    });

    connect(m_client, &SharePlayClient::peersReceived, this, [this](const QStringList &peers) {
        m_peers = peers;
        Q_EMIT peersChanged();
    });

    connect(m_client, &SharePlayClient::noticeReceived, this, &SharePlayManager::notice);

    connect(m_client, &SharePlayClient::mediaReceived, this, [this](const QUrl &url, const QString &title, double) {
        const bool wasStreaming = !m_mediaUrl.isEmpty();
        QUrl playbackUrl = url;
        if (url.scheme() == QLatin1String("https")) {
            playbackUrl = m_mediaProxy->start(url, m_client->confirmedFingerprint());
            if (playbackUrl.isEmpty()) {
                setMediaUrl(QUrl());
                m_mediaUnavailable = true;
                if (wasStreaming) {
                    Q_EMIT mediaUnavailable();
                }
                Q_EMIT errorOccurred(i18nc("@info", "Could not prepare encrypted SharePlay media playback."));
                return;
            }
        } else {
            m_mediaProxy->stop();
        }
        m_seekClock.invalidate();
        setDrift(0.0);
        setMediaTitle(title);
        if (url.isValid() && !url.isEmpty()) {
            m_mediaUnavailable = false;
            setMediaUrl(playbackUrl);
            Q_EMIT mediaAvailable(playbackUrl, title);
        } else {
            setMediaUrl(QUrl());
            if (wasStreaming) {
                m_mediaUnavailable = true;
                Q_EMIT mediaUnavailable();
                Q_EMIT notice(i18nc("@info", "The shared media is no longer available"));
            }
        }
    });

    connect(m_client, &SharePlayClient::stateReceived, this, [this](double, bool paused) {
        if (m_mediaUnavailable) {
            return;
        }
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
    if (m_mode != Mode::Client || m_mediaUnavailable || !m_client->hasHostState()) {
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

bool SharePlayManager::startHost(const QString &password, bool streamMedia, bool allowInsecure)
{
    QSslConfiguration tls;
    if (!allowInsecure) {
        QString error;
        if (!SharePlayTls::loadOrCreate(tls, error)) {
            const QString message = i18nc("@info", "Could not prepare the SharePlay certificate: %1", error);
            setStatusText(message);
            Q_EMIT errorOccurred(message);
            return false;
        }
    }

    const quint64 generation = m_connectionGeneration + 1;
    teardown();
    if (generation != m_connectionGeneration)
        return false;

    m_streamMedia = streamMedia;
    m_server->setPassword(password);
    const auto transport = allowInsecure ? SharePlayServer::Transport::Insecure : SharePlayServer::Transport::Tls;
    if (!m_server->start(SharePlayProtocol::ControlPort, transport, tls)) {
        const QString message = i18nc("@info", "Could not start the server: %1", m_server->errorString());
        setMode(Mode::Off);
        setState(State::Disconnected);
        setStatusText(message);
        Q_EMIT errorOccurred(message);
        return false;
    }

    m_hostFingerprint = allowInsecure ? QString() : QString::fromLatin1(tls.localCertificate().digest(QCryptographicHash::Sha256).toHex());
    Q_EMIT hostFingerprintChanged();
    m_insecureConnection = allowInsecure;
    Q_EMIT insecureConnectionChanged();
    setMode(Mode::Host);
    setState(State::Connected);
    setStatusText(i18nc("@info:status shareplay", "Hosting"));
    return true;
}

void SharePlayManager::connectToHost(const QString &address, const QString &nickname, const QString &password)
{
    const quint64 generation = m_connectionGeneration + 1;
    teardown();
    if (generation != m_connectionGeneration)
        return;
    m_nickname = nickname;
    m_pendingPassword = password;
    m_initialSecureAttempt = true;
    m_insecureConnection = false;
    Q_EMIT insecureConnectionChanged();
    if (generation != m_connectionGeneration)
        return;
    setMode(Mode::Client);
    if (generation != m_connectionGeneration)
        return;
    setState(State::Connecting);
    if (generation != m_connectionGeneration)
        return;
    setStatusText(i18nc("@info:status shareplay", "Connecting…"));
    if (generation != m_connectionGeneration)
        return;

    m_pendingUrl.setScheme(u"wss"_s);
    m_pendingUrl.setHost(address);
    m_pendingUrl.setPort(SharePlayProtocol::ControlPort);
    m_client->connectToServer(m_pendingUrl, nickname, password);
}

void SharePlayManager::confirmFingerprint()
{
    if (m_mode != Mode::Client || m_state != State::Connecting || m_pendingFingerprint.isEmpty()) {
        return;
    }
    const quint64 generation = m_connectionGeneration;
    m_initialSecureAttempt = false;
    const QByteArray fingerprint = m_pendingFingerprint.toLatin1();
    m_pendingFingerprint.clear();
    Q_EMIT pendingFingerprintChanged();
    if (generation != m_connectionGeneration)
        return;
    setStatusText(i18nc("@info:status shareplay", "Connecting…"));
    if (generation != m_connectionGeneration)
        return;
    m_client->connectToServer(m_pendingUrl, m_nickname, m_pendingPassword, fingerprint);
}

void SharePlayManager::closeInsecureProbe()
{
    if (m_insecureProbe) {
        auto *socket = m_insecureProbe;
        m_insecureProbe = nullptr;
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
}

void SharePlayManager::startInsecureProbe()
{
    if (m_mode != Mode::Client || m_state != State::Connecting || m_pendingUrl.scheme() != u"wss"_s) {
        return;
    }
    QUrl url = m_pendingUrl;
    url.setScheme(u"ws"_s);
    auto *socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    m_insecureProbe = socket;
    const quint64 generation = m_connectionGeneration;
    auto fail = [this, socket, generation]() {
        if (m_connectionGeneration != generation || m_insecureProbe != socket)
            return;
        closeInsecureProbe();
        const QString message = i18nc("@info", "Could not establish a SharePlay connection");
        leave();
        if (m_connectionGeneration != generation + 1)
            return;
        setStatusText(message);
        if (m_connectionGeneration != generation + 1)
            return;
        Q_EMIT errorOccurred(message);
    };
    connect(socket, &QWebSocket::connected, this, [this, socket, generation]() {
        if (m_connectionGeneration != generation || m_insecureProbe != socket)
            return;
        closeInsecureProbe();
        m_insecureConfirmationPending = true;
        setStatusText(i18nc("@info:status shareplay", "Confirm the unencrypted connection"));
        if (m_connectionGeneration != generation || !m_insecureConfirmationPending)
            return;
        Q_EMIT insecureConfirmationPendingChanged();
    });
    connect(socket, &QWebSocket::errorOccurred, this, [fail](QAbstractSocket::SocketError) {
        fail();
    });
    connect(socket, &QWebSocket::disconnected, this, fail);
    QTimer::singleShot(15000, socket, fail);
    socket->open(url);
}

void SharePlayManager::confirmInsecureConnection()
{
    if (!m_insecureConfirmationPending || m_mode != Mode::Client || m_state != State::Connecting)
        return;
    const quint64 generation = m_connectionGeneration;
    m_insecureConfirmationPending = false;
    m_pendingUrl.setScheme(u"ws"_s);
    m_insecureConnection = true;
    Q_EMIT insecureConfirmationPendingChanged();
    if (generation != m_connectionGeneration)
        return;
    Q_EMIT insecureConnectionChanged();
    if (generation != m_connectionGeneration)
        return;
    setStatusText(i18nc("@info:status shareplay", "Connecting…"));
    if (generation != m_connectionGeneration)
        return;
    m_client->connectToServer(m_pendingUrl, m_nickname, m_pendingPassword);
}

void SharePlayManager::cancelInsecureConnection()
{
    if (m_insecureConfirmationPending || m_insecureProbe)
        leave();
}

void SharePlayManager::cancelFingerprint()
{
    if (!m_pendingFingerprint.isEmpty()) {
        leave();
    }
}

void SharePlayManager::leave()
{
    if (m_mode == Mode::Off && !m_initialSecureAttempt && m_pendingPassword.isEmpty() && !m_insecureProbe && !m_insecureConfirmationPending) {
        return;
    }
    const quint64 generation = m_connectionGeneration + 1;
    teardown();
    if (generation != m_connectionGeneration)
        return;
    setMode(Mode::Off);
    if (generation != m_connectionGeneration)
        return;
    setState(State::Disconnected);
    if (generation != m_connectionGeneration)
        return;
    setStatusText(i18nc("@info:status shareplay", "Not connected"));
}

void SharePlayManager::setHostMedia(const QUrl &fileUrl, const QString &title, double duration)
{
    if (m_mode != Mode::Host) {
        return;
    }

    const quint64 previousRevision = m_mediaServer->revision();
    const bool canStream = m_streamMedia && fileUrl.isLocalFile() && m_server->isListening();
    m_mediaServer->setFile(canStream ? fileUrl.toLocalFile() : QString());
    // Metadata-only notifications keep the stream; a new snapshot, even at the
    // same path, gets a different generation. Tokens are not access credentials.
    if (fileUrl != m_hostMediaUrl || previousRevision != m_mediaServer->revision()) {
        ++m_mediaToken;
    }
    m_hostMediaUrl = fileUrl;
    setMediaTitle(title);
    m_server->setMedia(title, duration, canStream && !m_mediaServer->file().isEmpty(), m_mediaToken);
}

void SharePlayManager::updateHostMediaMetadata(const QUrl &fileUrl, const QString &title, double duration)
{
    if (m_mode != Mode::Host || fileUrl != m_hostMediaUrl) {
        return;
    }
    // Metadata from an open inode must not re-adopt a replacement on disk.
    setMediaTitle(title);
    m_server->setMedia(title, duration, !m_mediaServer->file().isEmpty(), m_mediaToken);
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
    const quint64 generation = ++m_connectionGeneration;
    m_initialSecureAttempt = false;
    m_ignoreInitialError = false;
    closeInsecureProbe();
    m_pendingPassword.clear();
    if (m_insecureConfirmationPending) {
        m_insecureConfirmationPending = false;
        Q_EMIT insecureConfirmationPendingChanged();
        if (generation != m_connectionGeneration)
            return;
    }
    setSpeedFactor(1.0);
    if (generation != m_connectionGeneration)
        return;
    m_seekClock.invalidate();
    setDrift(0.0);
    if (generation != m_connectionGeneration)
        return;
    m_client->disconnectFromServer();
    m_mediaProxy->stop();
    m_pendingPassword.clear();
    m_pendingUrl.clear();
    if (!m_pendingFingerprint.isEmpty()) {
        m_pendingFingerprint.clear();
        Q_EMIT pendingFingerprintChanged();
        if (generation != m_connectionGeneration)
            return;
    }
    if (!m_hostFingerprint.isEmpty()) {
        m_hostFingerprint.clear();
        Q_EMIT hostFingerprintChanged();
        if (generation != m_connectionGeneration)
            return;
    }
    m_server->stop();
    m_mediaServer->setFile(QString());
    m_hostMediaUrl.clear();
    m_mediaUnavailable = false;
    setMediaTitle(QString());
    if (generation != m_connectionGeneration)
        return;
    setMediaUrl(QUrl());
    if (generation != m_connectionGeneration)
        return;
    if (!m_peers.isEmpty()) {
        m_peers.clear();
        Q_EMIT peersChanged();
    }
}
