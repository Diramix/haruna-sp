#!/usr/bin/env bash
# Builds Haruna into an AppImage. Run from the repository root: bash appimage.sh
set -euo pipefail

root="$(cd "$(dirname "$0")" && pwd)"
build="$root/build-appimage"
appdir="$root/AppDir"
tools="$root/.appimage-tools"

mkdir -p "$tools"
for tool in linuxdeploy linuxdeploy-plugin-qt; do
    if [ ! -x "$tools/$tool" ]; then
        echo ">> downloading $tool"
        curl -fL --progress-bar -o "$tools/$tool" \
            "https://github.com/linuxdeploy/$tool/releases/download/continuous/$tool-x86_64.AppImage"
        chmod +x "$tools/$tool"
    fi
done
export PATH="$tools:$PATH"

echo ">> building"
cmake -B "$build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DBUILD_TESTING=OFF
cmake --build "$build"

echo ">> installing into AppDir"
rm -rf "$appdir"
DESTDIR="$appdir" cmake --install "$build"

# the Breeze icons and the desktop style are not linked in, they are loaded at runtime
cp -r /usr/share/icons/breeze "$appdir/usr/share/icons/"
mkdir -p "$appdir/usr/lib/qt6/qml"
cp -r /usr/lib/qt6/qml/org/kde/desktop "$appdir/usr/lib/qt6/qml/" 2>/dev/null || true

mkdir -p "$appdir/apprun-hooks"
cat > "$appdir/apprun-hooks/haruna.sh" <<'HOOK'
export XDG_DATA_DIRS="$APPDIR/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
export QT_QUICK_CONTROLS_STYLE=org.kde.desktop
HOOK

echo ">> packing"
# the plugin deploys every SQL driver it finds and dies on the Firebird one, whose
# libfbclient is not installed. Haruna only uses SQLite, so point the plugin at a
# plugin tree that mirrors the real one but keeps a single sql driver.
plugins="$(/usr/bin/qmake6 -query QT_INSTALL_PLUGINS)"
shadow="$tools/qt-plugins"
rm -rf "$shadow"
mkdir -p "$shadow/sqldrivers"
for dir in "$plugins"/*; do
    [ "$(basename "$dir")" = sqldrivers ] || ln -s "$dir" "$shadow/"
done
ln -s "$plugins/sqldrivers/libqsqlite.so" "$shadow/sqldrivers/"

cat > "$tools/qmake-shim" <<SHIM
#!/usr/bin/env bash
/usr/bin/qmake6 "\$@" | sed -e "s|^QT_INSTALL_PLUGINS:.*|QT_INSTALL_PLUGINS:$shadow|" -e "s|^$plugins\$|$shadow|"
SHIM
chmod +x "$tools/qmake-shim"

# without this the plugin picks up qmake-qt5 from /usr/sbin and finds no Qt6 modules
export QMAKE="$tools/qmake-shim"
export QML_SOURCES_PATHS="$root/src:$build/src"
export EXTRA_QT_MODULES="iconengines;imageformats;platformthemes;wayland-decoration-client;wayland-graphics-integration-client;wayland-shell-integration"
export DEPLOY_PLATFORM_THEMES=1
# only xcb is deployed by default, without this the AppImage refuses to start on wayland
export EXTRA_PLATFORM_PLUGINS="libqwayland.so"
# the strip shipped inside linuxdeploy predates .relr.dyn and chokes on our libraries
export NO_STRIP=1
export OUTPUT="Haruna-SharePlay-x86_64.AppImage"

linuxdeploy --appdir "$appdir" --plugin qt

# the qt plugin never deploys this one, and without it wayland has no egl client buffer
# integration, so no gl context and the scene graph aborts on the first expose
egl="$appdir/usr/plugins/wayland-graphics-integration-client"
mkdir -p "$egl"
cp /usr/lib/qt6/plugins/wayland-graphics-integration-client/libqt-plugin-wayland-egl.so "$egl/"
patchelf --set-rpath '$ORIGIN/../../lib:$ORIGIN' "$egl/libqt-plugin-wayland-egl.so"

linuxdeploy \
    --appdir "$appdir" \
    --output appimage \
    --desktop-file "$appdir/usr/share/applications/org.kde.haruna.desktop" \
    --icon-file "$appdir/usr/share/icons/hicolor/256x256/apps/haruna.png"

echo ">> done: $root/$OUTPUT"
