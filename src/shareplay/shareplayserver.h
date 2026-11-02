/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SHAREPLAYSERVER_H
#define SHAREPLAYSERVER_H

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QUrl>

class QWebSocket;
class QWebSocketServer;

class SharePlayServer : public QObject
{
    Q_OBJECT

public:
    explicit SharePlayServer(QObject *parent = nullptr);
    ~SharePlayServer() override;

    bool start(quint16 port);
    void stop();
    bool isListening() const;
    QString errorString() const;

    void setPassword(const QString &password);
    void setMedia(const QString &name, double duration, bool available, quint64 token);
    void broadcastState(double position, bool paused);
    void sendNotice(QWebSocket *peer, const QString &text);

    QStringList peerNames() const;

Q_SIGNALS:
    void peersChanged();
    void pauseRequested(const QString &name, bool paused, QWebSocket *peer);

private:
    void onNewConnection();
    void onTextMessage(QWebSocket *peer, const QString &message);
    void removePeer(QWebSocket *peer);
    void rejectPeer(QWebSocket *peer, const QString &reason);
    void broadcastPeers();
    void send(QWebSocket *peer, const QJsonObject &message);
    void broadcast(const QJsonObject &message);
    QJsonObject mediaMessageFor(QWebSocket *peer) const;
    /** milliseconds since the session started, the reference the clients sync their clock to */
    double hostTime() const;

    QWebSocketServer *m_server{nullptr};
    QElapsedTimer m_clock;
    QHash<QWebSocket *, QString> m_peers;
    QHash<QTcpSocket *, QPointer<QTcpSocket>> m_tcpSockets;
    QSet<QWebSocket *> m_webSockets;
    quint64 m_session{0};
    bool m_running{false};
    QString m_password;
    QString m_mediaName;
    double m_mediaDuration{0.0};
    quint64 m_mediaToken{0};
    bool m_mediaAvailable{false};
    double m_lastPosition{0.0};
    double m_lastStateTime{0.0};
    bool m_lastPaused{true};
    quint64 m_seq{0};
};

#endif // SHAREPLAYSERVER_H
