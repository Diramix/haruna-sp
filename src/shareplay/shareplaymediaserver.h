/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SHAREPLAYMEDIASERVER_H
#define SHAREPLAYMEDIASERVER_H

#include <QObject>
#include <QString>

class QTcpServer;
class QTcpSocket;

/**
 * Minimal HTTP/1.1 server that serves a single local file, the one the host is
 * currently playing. The request path is ignored on purpose, so it can never be
 * used to read anything else from the file system.
 */
class SharePlayMediaServer : public QObject
{
    Q_OBJECT

public:
    explicit SharePlayMediaServer(QObject *parent = nullptr);
    ~SharePlayMediaServer() override;

    bool start(quint16 port);
    void stop();
    bool isListening() const;
    quint16 port() const;

    void setFile(const QString &path);
    QString file() const;

private:
    void onNewConnection();
    void handleRequest(QTcpSocket *socket, const QByteArray &request);
    void sendError(QTcpSocket *socket, int code, const QString &text);

    QTcpServer *m_server{nullptr};
    QString m_file;
};

#endif // SHAREPLAYMEDIASERVER_H
