/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SHAREPLAYMEDIASERVER_H
#define SHAREPLAYMEDIASERVER_H

#include <QObject>
#include <QString>

class QTcpSocket;

/**
 * Minimal HTTP/1.1 handler that serves a single local file, the one the host is
 * currently playing. The request path is ignored on purpose, so it can never be
 * used to read anything else from the file system.
 *
 * It does not own a listening socket: the sockets are accepted by SharePlayServer
 * on the shared control port and handed over through handleSocket().
 */
class SharePlayMediaServer : public QObject
{
    Q_OBJECT

public:
    explicit SharePlayMediaServer(QObject *parent = nullptr);
    ~SharePlayMediaServer() override;

    /** take over an already accepted connection and answer the HTTP request on it */
    void handleSocket(QTcpSocket *socket);

    void setFile(const QString &path);
    QString file() const;

private:
    void handleRequest(QTcpSocket *socket, const QByteArray &request);
    void sendError(QTcpSocket *socket, int code, const QString &text);

    QString m_file;
};

#endif // SHAREPLAYMEDIASERVER_H
