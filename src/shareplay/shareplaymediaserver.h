/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SHAREPLAYMEDIASERVER_H
#define SHAREPLAYMEDIASERVER_H

#include <QByteArray>
#include <QObject>
#include <QSet>
#include <QString>

class QTcpSocket;
class QFile;
class QFileSystemWatcher;
class QTimer;

/** Serves one snapshot on handed-off sockets. Tokens identify streams, not users. */
class SharePlayMediaServer : public QObject
{
    Q_OBJECT
public:
    explicit SharePlayMediaServer(QObject *parent = nullptr);
    ~SharePlayMediaServer() override;
    void handleSocket(QTcpSocket *socket);
    // A changed snapshot is unavailable until setToken() supplies a different token.
    void setFile(const QString &path);
    void setToken(quint64 token);
    void stop();
    QString file() const;
    quint64 revision() const;

Q_SIGNALS:
    void fileInvalidated();

private:
    static QByteArray fingerprint(const QString &path, QFile *opened = nullptr);
    bool checkFile(QFile *opened = nullptr);
    void invalidate();
    void abortSockets();
    void handleRequest(QTcpSocket *socket, const QByteArray &request);
    void sendError(QTcpSocket *socket, int code, const QString &text);

    QString m_file;
    QByteArray m_fingerprint;
    quint64 m_token = 0;
    quint64 m_revision = 0;
    bool m_hasToken = false;
    bool m_tokenReady = false;
    QSet<QTcpSocket *> m_sockets;
    QFileSystemWatcher *m_watcher;
    QTimer *m_poll;
};
#endif
