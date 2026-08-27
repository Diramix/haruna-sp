/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

import org.kde.ki18n

import org.kde.haruna.settings
import org.kde.haruna.shareplay

Menu {
    id: root

    required property SharePlayManager m_sharePlay
    required property SharePlayConfigWindow m_configWindow

    readonly property bool isConfigured: SharePlaySettings.lastServerAddress !== ""
                                         && SharePlaySettings.nickname !== ""

    title: KI18n.i18nc("@title:menu", "S&harePlay")

    MenuItem {
        enabled: root.m_sharePlay.isActive || root.isConfigured
        icon.name: root.m_sharePlay.isActive ? "network-disconnect" : "network-connect"
        text: root.m_sharePlay.isActive
              ? KI18n.i18nc("@action:inmenu", "Disconnect")
              : KI18n.i18nc("@action:inmenu", "Connect")
        onClicked: {
            if (root.m_sharePlay.isActive) {
                root.m_sharePlay.leave()
            } else {
                root.m_sharePlay.connectToHost(SharePlaySettings.lastServerAddress,
                                               SharePlaySettings.nickname,
                                               SharePlaySettings.lastServerPassword)
            }
        }
    }

    MenuSeparator {}

    MenuItem {
        icon.name: "configure"
        text: KI18n.i18nc("@action:inmenu", "Configure Server…")
        onClicked: root.m_configWindow.open()
    }
}
