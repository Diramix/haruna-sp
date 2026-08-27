/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SHAREPLAYPROTOCOL_H
#define SHAREPLAYPROTOCOL_H

#include <QString>
#include <QtTypes>

namespace SharePlayProtocol
{
constexpr int Version = 2;

/** control channel, the media is served on ControlPort + 1 */
constexpr quint16 ControlPort = 7439;
constexpr quint16 MediaPort = ControlPort + 1;

namespace Type
{
inline const QString Hello = QStringLiteral("hello");
inline const QString Welcome = QStringLiteral("welcome");
inline const QString Error = QStringLiteral("error");
inline const QString State = QStringLiteral("state");
inline const QString Media = QStringLiteral("media");
inline const QString Peers = QStringLiteral("peers");
inline const QString RequestPause = QStringLiteral("requestPause");
inline const QString Notice = QStringLiteral("notice");
inline const QString Ping = QStringLiteral("ping");
inline const QString Pong = QStringLiteral("pong");
}

namespace Error
{
inline const QString BadProtocol = QStringLiteral("badProtocol");
inline const QString BadPassword = QStringLiteral("badPassword");
inline const QString BadName = QStringLiteral("badName");
inline const QString NoMedia = QStringLiteral("noMedia");
}
}

#endif // SHAREPLAYPROTOCOL_H
