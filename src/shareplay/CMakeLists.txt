#
# SPDX-FileCopyrightText: 2026 Haruna SharePlay
#
# SPDX-License-Identifier: BSD-3-Clause
#

qt_add_library(shareplay STATIC)
qt_add_qml_module(shareplay
    URI org.kde.haruna.shareplay
    OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/org/kde/haruna/shareplay
    IMPORT_PATH ${CMAKE_BINARY_DIR}
    VERSION 1.0
    QML_FILES
        SharePlayConfigWindow.qml
    SOURCES
        shareplaytls.h
        shareplaytls.cpp
        shareplayprotocol.h
        shareplayclient.h
        shareplayclient.cpp
        shareplaymanager.h
        shareplaymanager.cpp
        shareplaymediaserver.h
        shareplaymediaserver.cpp
        shareplayserver.h
        shareplayserver.cpp
)

target_link_libraries(shareplay PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    OpenSSL::Crypto

    KF6::I18n

    settingsplugin
)
