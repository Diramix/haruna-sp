/*
 * SPDX-FileCopyrightText: 2026 Haruna SharePlay
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts

import org.kde.ki18n
import org.kde.kirigami as Kirigami

import org.kde.haruna.settings
import org.kde.haruna.shareplay

Loader {
    id: root

    required property SharePlayManager m_sharePlay

    function open() : void {
        if (!root.active) {
            root.active = true
            root.loaded.connect(function() {
                root.open()
            })
            return
        }

        const window = root.item as Window
        if (window.visible) {
            window.raise()
        } else {
            window.visible = true
        }
    }

    active: false
    asynchronous: true

    Connections {
        target: root.m_sharePlay
        function onInsecureConfirmationPendingChanged() : void {
            if (root.m_sharePlay.insecureConfirmationPending && !root.active) {
                root.open()
            }
        }
        function onPendingFingerprintChanged() : void {
            if (root.m_sharePlay.pendingFingerprint !== "" && !root.active) {
                root.open()
            }
        }
    }

    sourceComponent: Kirigami.ApplicationWindow {
        id: window

        readonly property SharePlayManager sharePlay: root.m_sharePlay
        readonly property bool isIdle: sharePlay.mode === SharePlayManager.Mode.Off

        readonly property color accentColor: {
            switch (sharePlay.state) {
            case SharePlayManager.State.Connected:
                return Kirigami.Theme.positiveTextColor
            case SharePlayManager.State.Connecting:
                return Kirigami.Theme.neutralTextColor
            default:
                return Kirigami.Theme.disabledTextColor
            }
        }

        width: 620
        height: 640
        minimumWidth: 460
        minimumHeight: 480
        title: KI18n.i18nc("@title:window", "SharePlay")
        transientParent: null
        visible: false

        Component.onCompleted: {
            if (sharePlay.insecureConfirmationPending) {
                visible = true
                insecureDialog.open()
            } else if (sharePlay.pendingFingerprint !== "") {
                visible = true
                fingerprintDialog.open()
            }
        }

        Connections {
            target: window.sharePlay
            function onPendingFingerprintChanged() : void {
                if (window.sharePlay.pendingFingerprint !== "") {
                    window.visible = true
                    fingerprintDialog.open()
                } else {
                    fingerprintDialog.close()
                }
            }
            function onInsecureConfirmationPendingChanged() : void {
                if (window.sharePlay.insecureConfirmationPending) {
                    window.visible = true
                    insecureDialog.open()
                } else {
                    insecureDialog.close()
                }
            }
        }

        QQC2.Dialog {
            id: fingerprintDialog
            anchors.centerIn: parent
            modal: true
            title: KI18n.i18nc("@title:window", "Confirm host certificate")
            standardButtons: QQC2.Dialog.Ok | QQC2.Dialog.Cancel
            onAccepted: window.sharePlay.confirmFingerprint()
            onRejected: window.sharePlay.cancelFingerprint()

            contentItem: ColumnLayout {
                QQC2.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: KI18n.i18nc("@info", "Compare this SHA-256 fingerprint with the host using a trusted channel before connecting.")
                }
                QQC2.TextField {
                    Layout.fillWidth: true
                    readOnly: true
                    selectByMouse: true
                    text: window.sharePlay.pendingFingerprint
                }
            }
        }

        QQC2.Dialog {
            id: insecureDialog
            anchors.centerIn: parent
            modal: true
            title: KI18n.i18nc("@title:window", "Unencrypted connection")
            standardButtons: QQC2.Dialog.Yes | QQC2.Dialog.Cancel
            onAccepted: window.sharePlay.confirmInsecureConnection()
            onRejected: window.sharePlay.cancelInsecureConnection()

            contentItem: QQC2.Label {
                width: Math.min(window.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 25)
                wrapMode: Text.WordWrap
                text: KI18n.i18nc("@info", "The server accepted an unencrypted connection. Its identity cannot be verified, and your password, playback controls and media will not be encrypted. Connect anyway?")
            }
        }

        Shortcut {
            sequence: "Esc"
            onActivated: window.visible = false
        }

        // ------------------------------------------------------------------
        // live status banner
        // ------------------------------------------------------------------
        component StatusBanner: Rectangle {
            Layout.fillWidth: true
            implicitHeight: bannerLayout.implicitHeight + Kirigami.Units.smallSpacing * 2
            radius: Kirigami.Units.cornerRadius
            color: Qt.alpha(window.accentColor, 0.12)
            border.width: 1
            border.color: Qt.alpha(window.accentColor, 0.35)

            RowLayout {
                id: bannerLayout

                anchors.fill: parent
                anchors.leftMargin: Kirigami.Units.largeSpacing
                anchors.rightMargin: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.smallSpacing

                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.rightMargin: Kirigami.Units.smallSpacing
                    implicitWidth: Kirigami.Units.gridUnit * 0.6
                    implicitHeight: implicitWidth
                    radius: width / 2
                    color: window.accentColor
                    visible: !(window.sharePlay.state === SharePlayManager.State.Connected && window.sharePlay.insecureConnection)
                }

                Kirigami.Icon {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.rightMargin: Kirigami.Units.smallSpacing
                    implicitWidth: Kirigami.Units.iconSizes.small
                    implicitHeight: implicitWidth
                    source: "dialog-warning-symbolic"
                    color: Kirigami.Theme.positiveTextColor
                    visible: window.sharePlay.state === SharePlayManager.State.Connected && window.sharePlay.insecureConnection
                    QQC2.ToolTip.visible: warningHover.hovered
                    QQC2.ToolTip.text: KI18n.i18nc("@info:tooltip", "This connection is not encrypted")

                    HoverHandler { id: warningHover }
                }

                QQC2.Label {
                    Layout.alignment: Qt.AlignVCenter
                    font.bold: true
                    text: {
                        if (window.sharePlay.mode === SharePlayManager.Mode.Host) {
                            return KI18n.i18nc("@info:status", "Hosting")
                        }
                        switch (window.sharePlay.state) {
                        case SharePlayManager.State.Connected:
                            return KI18n.i18nc("@info:status", "Connected")
                        case SharePlayManager.State.Connecting:
                            return KI18n.i18nc("@info:status", "Connecting")
                        default:
                            return KI18n.i18nc("@info:status", "Disconnected")
                        }
                    }
                }

                QQC2.Label {
                    id: hostFingerprintField
                    Layout.alignment: Qt.AlignVCenter
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    visible: window.sharePlay.hostFingerprint !== ""
                    text: window.sharePlay.hostFingerprint
                    elide: Text.ElideMiddle
                    horizontalAlignment: Text.AlignHCenter
                    activeFocusOnTab: true
                    Accessible.role: Accessible.Button
                    Accessible.name: KI18n.i18nc("@action:button", "Copy host certificate fingerprint")
                    Accessible.onPressAction: copyFingerprint()
                    QQC2.ToolTip.visible: fingerprintHover.hovered
                    QQC2.ToolTip.text: KI18n.i18nc("@info:tooltip", "Click to copy the host certificate fingerprint")

                    function copyFingerprint() : void {
                        fingerprintClipboard.selectAll()
                        fingerprintClipboard.copy()
                        window.sharePlay.notice(KI18n.i18nc("@info", "Fingerprint copied"))
                    }

                    TextInput {
                        id: fingerprintClipboard
                        visible: false
                        text: window.sharePlay.hostFingerprint
                    }
                    Keys.onReturnPressed: copyFingerprint()
                    Keys.onSpacePressed: copyFingerprint()
                    TapHandler { onTapped: hostFingerprintField.copyFingerprint() }
                    HoverHandler {
                        id: fingerprintHover
                        cursorShape: Qt.PointingHandCursor
                    }
                }

                QQC2.Label {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.fillWidth: true
                    visible: !hostFingerprintField.visible
                    elide: Text.ElideMiddle
                    opacity: 0.75
                    text: window.sharePlay.mediaTitle !== "" ? `— ${window.sharePlay.mediaTitle}` : ""
                }

                QQC2.Label {
                    Layout.alignment: Qt.AlignVCenter
                    visible: !window.isIdle
                    opacity: 0.75
                    text: KI18n.i18ncp("@info:status number of connected viewers", "%1 viewer", "%1 viewers",
                                       window.sharePlay.peers.length)
                }
            }
        }

        // ------------------------------------------------------------------
        // reusable list of names
        // ------------------------------------------------------------------
        component PeerList: ColumnLayout {
            id: peerList

            required property string emptyText

            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Kirigami.Units.smallSpacing

            Kirigami.Heading {
                level: 5
                text: KI18n.i18nc("@title:group", "Viewers")
            }

            QQC2.ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.minimumHeight: Kirigami.Units.gridUnit * 4

                ListView {
                    id: peerListView

                    clip: true
                    model: window.sharePlay.peers
                    spacing: 0

                    delegate: QQC2.ItemDelegate {
                        required property string modelData

                        width: ListView.view.width
                        hoverEnabled: false
                        down: false
                        icon.name: "user-identity"
                        text: modelData
                    }

                    QQC2.Label {
                        anchors.centerIn: parent
                        visible: peerListView.count === 0
                        opacity: 0.6
                        text: peerList.emptyText
                    }
                }
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.largeSpacing

                StatusBanner {}

                QQC2.TabBar {
                    id: tabBar

                    Layout.fillWidth: true
                    currentIndex: window.sharePlay.mode === SharePlayManager.Mode.Client ? 1 : 0

                    QQC2.TabButton {
                        text: KI18n.i18nc("@title:tab host a SharePlay session", "Host")
                        icon.name: "network-server"
                    }
                    QQC2.TabButton {
                        text: KI18n.i18nc("@title:tab join a SharePlay session", "Client")
                        icon.name: "network-workgroup"
                    }
                }

                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: tabBar.currentIndex

                    // ----------------------------------------------------------
                    // Host
                    // ----------------------------------------------------------
                    ColumnLayout {
                        spacing: Kirigami.Units.largeSpacing

                        Kirigami.FormLayout {
                            Layout.fillWidth: true

                            Kirigami.PasswordField {
                                id: hostPassword

                                Kirigami.FormData.label: KI18n.i18nc("@label:textbox", "Password:")
                                enabled: window.isIdle
                                text: SharePlaySettings.password
                                placeholderText: KI18n.i18nc("@info:placeholder", "Optional")
                            }

                            QQC2.CheckBox {
                                id: hostStream

                                Kirigami.FormData.label: KI18n.i18nc("@label:checkbox", "Streaming:")
                                text: KI18n.i18nc("@option:check", "Send the playing file to the viewers")
                                enabled: window.isIdle
                                checked: SharePlaySettings.streamMedia
                            }

                            QQC2.CheckBox {
                                id: hostAllowInsecure

                                Kirigami.FormData.label: KI18n.i18nc("@label:checkbox", "Security:")
                                text: KI18n.i18nc("@option:check", "Allow insecure")
                                enabled: window.isIdle
                                checked: false
                            }
                        }

                        QQC2.Button {
                            Layout.alignment: Qt.AlignLeft
                            text: window.sharePlay.isHost
                                  ? KI18n.i18nc("@action:button", "Stop Session")
                                  : KI18n.i18nc("@action:button", "Start Session")
                            icon.name: window.sharePlay.isHost ? "media-playback-stop" : "media-playback-start"
                            enabled: window.isIdle || window.sharePlay.isHost
                            onClicked: {
                                if (window.sharePlay.isHost) {
                                    window.sharePlay.leave()
                                    return
                                }

                                SharePlaySettings.password = hostPassword.text
                                SharePlaySettings.streamMedia = hostStream.checked
                                SharePlaySettings.save()

                                window.sharePlay.startHost(hostPassword.text, hostStream.checked, hostAllowInsecure.checked)
                            }
                        }


                        PeerList {
                            emptyText: KI18n.i18nc("@info", "Nobody has joined yet")
                        }
                    }

                    // ----------------------------------------------------------
                    // Client
                    // ----------------------------------------------------------
                    ColumnLayout {
                        spacing: Kirigami.Units.largeSpacing

                        Kirigami.FormLayout {
                            Layout.fillWidth: true

                            QQC2.TextField {
                                id: clientAddress

                                Kirigami.FormData.label: KI18n.i18nc("@label:textbox", "Host address:")
                                enabled: window.isIdle
                                text: SharePlaySettings.lastServerAddress
                                placeholderText: "192.168.1.10"
                                onAccepted: connectButton.clicked()
                            }

                            QQC2.TextField {
                                id: clientNickname

                                Kirigami.FormData.label: KI18n.i18nc("@label:textbox", "Your name:")
                                enabled: window.isIdle
                                maximumLength: 32
                                text: SharePlaySettings.nickname
                                placeholderText: KI18n.i18nc("@info:placeholder", "Shown to the other viewers")
                                onAccepted: connectButton.clicked()
                            }


                            Kirigami.PasswordField {
                                id: clientPassword

                                Kirigami.FormData.label: KI18n.i18nc("@label:textbox", "Password:")
                                enabled: window.isIdle
                                text: SharePlaySettings.lastServerPassword
                                placeholderText: KI18n.i18nc("@info:placeholder", "Optional")
                                onAccepted: connectButton.clicked()
                            }
                        }

                        QQC2.Button {
                            id: connectButton

                            Layout.alignment: Qt.AlignLeft
                            enabled: !window.isIdle
                                     || (clientAddress.text.trim() !== "" && clientNickname.text.trim() !== "")
                            text: window.isIdle
                                  ? KI18n.i18nc("@action:button", "Connect")
                                  : KI18n.i18nc("@action:button", "Disconnect")
                            icon.name: window.isIdle ? "network-connect" : "network-disconnect"
                            onClicked: {
                                if (!window.isIdle) {
                                    window.sharePlay.leave()
                                    return
                                }

                                SharePlaySettings.lastServerAddress = clientAddress.text.trim()
                                SharePlaySettings.nickname = clientNickname.text.trim()
                                SharePlaySettings.lastServerPassword = clientPassword.text
                                SharePlaySettings.save()

                                window.sharePlay.connectToHost(clientAddress.text.trim(),
                                                               clientNickname.text.trim(),
                                                               clientPassword.text)
                            }
                        }

                        Kirigami.Separator { Layout.fillWidth: true }

                        PeerList {
                            emptyText: KI18n.i18nc("@info", "Not connected")
                        }
                    }
                }
        }
    }
}
