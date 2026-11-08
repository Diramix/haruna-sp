/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SHAREPLAYMEDIAPROXY_H
#define SHAREPLAYMEDIAPROXY_H

#include <QObject>
#include <QUrl>

class QTcpServer;

class SharePlayMediaProxy : public QObject
{
public:
    explicit SharePlayMediaProxy(QObject *parent = nullptr);
    QUrl start(const QUrl &remote, const QByteArray &fingerprint);
    void stop();

private:
    QTcpServer *m_server;
    QUrl m_remote;
    QByteArray m_fingerprint;
    QByteArray m_path;
};
#endif
