/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SHAREPLAYMANAGER_H
#define SHAREPLAYMANAGER_H

#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <qqmlintegration.h>

class SharePlayClient;
class SharePlayMediaServer;
class SharePlayServer;

class SharePlayManager : public QObject
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit SharePlayManager(QObject *parent = nullptr);
    ~SharePlayManager() override;

    enum class Mode {
        Off,
        Host,
        Client,
    };
    Q_ENUM(Mode)

    enum class State {
        Disconnected,
        Connecting,
        Connected,
    };
    Q_ENUM(State)

    Q_PROPERTY(Mode mode READ mode NOTIFY modeChanged)
    Mode mode() const;

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    State state() const;

    Q_PROPERTY(bool isActive READ isActive NOTIFY modeChanged)
    bool isActive() const;

    Q_PROPERTY(bool isHost READ isHost NOTIFY modeChanged)
    bool isHost() const;

    /** false only for a connected client: playback is driven by the host */
    Q_PROPERTY(bool canControl READ canControl NOTIFY modeChanged)
    bool canControl() const;

    Q_PROPERTY(QStringList peers READ peers NOTIFY peersChanged)
    QStringList peers() const;

    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    QString statusText() const;

    Q_PROPERTY(double syncThreshold READ syncThreshold WRITE setSyncThreshold NOTIFY syncThresholdChanged)
    double syncThreshold() const;
    void setSyncThreshold(double value);

    /** title of the file being shared, empty when there is none */
    Q_PROPERTY(QString mediaTitle READ mediaTitle NOTIFY mediaTitleChanged)
    QString mediaTitle() const;

    /** client side: the url of the host media, empty when there is none */
    Q_PROPERTY(QUrl mediaUrl READ mediaUrl NOTIFY mediaUrlChanged)
    QUrl mediaUrl() const;

    Q_PROPERTY(int controlPort READ controlPort CONSTANT)
    int controlPort() const;

    Q_INVOKABLE bool startHost(const QString &password, bool streamMedia);
    Q_INVOKABLE void connectToHost(const QString &address, const QString &nickname, const QString &password);
    Q_INVOKABLE void leave();

    /** host side: the file currently playing, shared with the clients */
    Q_INVOKABLE void setHostMedia(const QUrl &fileUrl, const QString &title, double duration);
    /** host side: called on every position/pause change */
    Q_INVOKABLE void publishState(double position, bool paused);

    /** client side: the live player position, drives the drift correction */
    Q_INVOKABLE void reportPosition(double position);
    /** how far the player is from the host, in seconds, positive when behind */
    Q_PROPERTY(double drift READ drift NOTIFY driftChanged)
    double drift() const;

    /** client side: ask the host to pause or resume */
    Q_INVOKABLE void requestPause(bool paused);

    Q_INVOKABLE QStringList localAddresses() const;

Q_SIGNALS:
    void modeChanged();
    void stateChanged();
    void peersChanged();
    void statusTextChanged();
    void syncThresholdChanged();
    void mediaTitleChanged();
    void mediaUrlChanged();
    void driftChanged();

    /** client side: apply the host state to the local player */
    void applyPause(bool paused);
    void applySeek(double position);
    /** client side: slight speed change that absorbs a small drift without a jump */
    void applySpeed(double speed);
    /** client side: the host media became available at this url */
    void mediaAvailable(const QUrl &url, const QString &title);
    /** show a message in the osd */
    void notice(const QString &text);
    void peerRequestedPause(const QString &name, bool paused);
    void errorOccurred(const QString &message);

private:
    void setMode(Mode mode);
    void setState(State state);
    void setStatusText(const QString &text);
    void setMediaTitle(const QString &title);
    void setMediaUrl(const QUrl &url);
    void setDrift(double value);
    void setSpeedFactor(double speed);
    void evaluateSync();
    void teardown();

    SharePlayServer *m_server{nullptr};
    SharePlayMediaServer *m_mediaServer{nullptr};
    SharePlayClient *m_client{nullptr};

    Mode m_mode{Mode::Off};
    State m_state{State::Disconnected};
    QStringList m_peers;
    QString m_statusText;
    QString m_mediaTitle;
    QUrl m_mediaUrl;
    QString m_nickname;
    double m_syncThreshold{0.8};
    double m_lastPosition{0.0};
    double m_drift{0.0};
    double m_speedFactor{1.0};
    QElapsedTimer m_seekClock;
    bool m_lastPaused{true};
    bool m_streamMedia{true};
    QUrl m_hostMediaUrl;
    quint64 m_mediaToken{0};
};

#endif // SHAREPLAYMANAGER_H
