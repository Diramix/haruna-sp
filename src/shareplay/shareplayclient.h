/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SHAREPLAYCLIENT_H
#define SHAREPLAYCLIENT_H

#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QUrl>

class QTimer;
class QWebSocket;

class SharePlayClient : public QObject
{
    Q_OBJECT

public:
    explicit SharePlayClient(QObject *parent = nullptr);
    ~SharePlayClient() override;

    void connectToServer(const QUrl &url, const QString &name, const QString &password, const QByteArray &confirmedSha256 = {});
    void disconnectFromServer();
    bool isConnected() const;

    void requestPause(bool paused);

    /** one way trip time in seconds, used to compensate the received position */
    double latency() const;

    /** true once a state message arrived and the clock offset is known */
    bool hasHostState() const;
    bool hostPaused() const;
    /** where the host is right now, the last known position extrapolated over the host clock */
    double hostPositionNow() const;

Q_SIGNALS:
    // Hexadecimal SHA-256 of the leaf certificate; reconnect with this pin only
    // after the user has verified it through a trusted channel.
    void certificateConfirmationRequired(const QUrl &url, const QByteArray &sha256);
    void connected();
    void disconnected();
    void rejected(const QString &reason);
    void errorOccurred(const QString &message);
    void stateReceived(double position, bool paused);
    void mediaReceived(const QUrl &url, const QString &name, double duration);
    void peersReceived(const QStringList &peers);
    void noticeReceived(const QString &text);

private:
    void onConnected();
    bool verifyCertificate();
    void finish(const QString &message = QString(), bool rejectedByHost = false);
    bool validMediaUrl(const QUrl &url) const;
    void onTextMessage(const QString &message);
    void send(const QJsonObject &message);
    void sendPing();
    void takeClockSample(double t0, double t3, double hostTime);
    /** milliseconds on the local monotonic clock */
    double now() const;

    QWebSocket *m_socket{nullptr};
    QTimer *m_pingTimer{nullptr};
    QTimer *m_timeoutTimer{nullptr};
    QUrl m_controlUrl;

    bool m_active{false};
    quint64 m_attempt{0};
    QElapsedTimer m_clock;
    QString m_name;
    QString m_password;
    QByteArray m_confirmedSha256;
    bool m_helloSent{false};
    double m_latency{0.0};
    /** hostClock - localClock, in milliseconds */
    double m_clockOffset{0.0};
    double m_bestRtt{0.0};
    int m_samplesSinceBest{0};
    int m_pingCount{0};
    bool m_clockValid{false};
    double m_hostPosition{0.0};
    double m_hostStateTime{0.0};
    bool m_hostPaused{true};
    bool m_hasHostState{false};
    bool m_accepted{false};
};

#endif // SHAREPLAYCLIENT_H
