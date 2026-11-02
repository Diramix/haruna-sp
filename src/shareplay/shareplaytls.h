/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QString>

class QSslConfiguration;

namespace SharePlayTls
{
bool loadOrCreate(QSslConfiguration &configuration, QString &error);
}
