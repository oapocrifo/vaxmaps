#!/bin/sh
# Cross-compiles VaxMaps.exe on Ubuntu 24.04: static Qt 6.8 LTS (qtbase + qtimageformats) with MinGW-w64.
# apt install g++ g++-mingw-w64-x86-64-posix nsis cmake ninja-build zip osslsigncode
# Signed when SIGN_PFX, SIGN_PKCS11_MODULE or SIGN_CMD is set (see sign.sh); unsigned otherwise.
# Qt 6.4 bundled libwebp 1.2.4 (CVE-2023-4863) and libtiff 4.4.0: keep QTV on a maintained release.
set -e
HERE=$(cd "$(dirname "$0")" && pwd); W=${W:-$PWD/winbuild}; QT=$W/qt-win; HOST=$W/qt-host
QTV=${QTV:-v6.8.5-lts-lgpl}
mkdir -p "$W" && cd "$W"
[ -d qtbase ] || git clone --depth 1 --branch $QTV https://github.com/qt/qtbase.git
[ -d qtimageformats ] || git clone --depth 1 --branch $QTV https://github.com/qt/qtimageformats.git
# The host tools (moc, rcc, uic...) must be the same Qt version as the target: Ubuntu's Qt is 6.4.
if [ ! -x "$HOST/bin/qt-cmake" ]; then
  cmake -S qtbase -B qthost-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$HOST \
   -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DFEATURE_sql=OFF -DFEATURE_network=OFF -DFEATURE_dbus=OFF \
   -DFEATURE_testlib=OFF -DFEATURE_opengl=OFF -DFEATURE_printsupport=OFF -DFEATURE_xml=OFF -DFEATURE_concurrent=OFF \
   -DFEATURE_vulkan=OFF -DFEATURE_xcb=OFF -DFEATURE_glib=OFF -DFEATURE_icu=OFF -DFEATURE_system_harfbuzz=OFF \
   -DFEATURE_system_freetype=OFF -DFEATURE_fontconfig=OFF -DFEATURE_system_pcre2=OFF -DFEATURE_system_doubleconversion=OFF
  ninja -C qthost-build install
fi
COMMON="-G Ninja -DCMAKE_TOOLCHAIN_FILE=$HERE/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
 -DQT_HOST_PATH=$HOST -DQT_HOST_PATH_CMAKE_DIR=$HOST/lib/cmake"
cmake -S qtbase -B qtbase-build $COMMON -DCMAKE_INSTALL_PREFIX=$QT -DBUILD_SHARED_LIBS=OFF -DFEATURE_static_runtime=ON \
 -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF -DFEATURE_sql=OFF -DFEATURE_network=OFF -DFEATURE_dbus=OFF \
 -DFEATURE_testlib=OFF -DFEATURE_opengl=OFF -DFEATURE_dynamicgl=OFF -DFEATURE_printsupport=OFF -DFEATURE_xml=OFF \
 -DFEATURE_concurrent=OFF -DFEATURE_vulkan=OFF -DFEATURE_icu=OFF -DFEATURE_system_zlib=OFF -DFEATURE_system_png=OFF \
 -DFEATURE_system_jpeg=OFF -DFEATURE_system_harfbuzz=OFF -DFEATURE_system_freetype=OFF -DFEATURE_system_pcre2=OFF \
 -DFEATURE_system_doubleconversion=OFF
ninja -C qtbase-build install
# qt-cmake from here on: the toolchain file lets find_package look only under /usr/x86_64-w64-mingw32,
# so plain cmake with CMAKE_PREFIX_PATH=$QT does not find the Qt just installed. qt-cmake adds it (and keeps the toolchain).
QTCMAKE="$QT/bin/qt-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release"
$QTCMAKE -S qtimageformats -B qtimg-build -DCMAKE_INSTALL_PREFIX=$QT \
 -DFEATURE_jasper=OFF -DFEATURE_mng=OFF -DFEATURE_system_webp=OFF -DFEATURE_system_tiff=OFF
ninja -C qtimg-build install
$QTCMAKE -S "$HERE/.." -B vaxmaps-build
ninja -C vaxmaps-build
x86_64-w64-mingw32-strip vaxmaps-build/VaxMaps.exe
SIGNING=$SIGN_PFX$SIGN_PKCS11_MODULE$SIGN_CMD
if [ -n "$SIGNING" ]; then sh "$HERE/sign.sh" vaxmaps-build/VaxMaps.exe; fi
VERSION=$(sed -n 's/^project(VaxMaps VERSION \([0-9.]*\).*/\1/p' "$HERE/../CMakeLists.txt")
makensis -V2 ${SIGNING:+-DSIGN=1} -DSIGNER="$HERE/sign.sh" -DVERSION="$VERSION" -DEXE="$W/vaxmaps-build/VaxMaps.exe" -DREADME="$HERE/../README.md" \
 -DOUTFILE="$W/VaxMaps-$VERSION-setup.exe" "$HERE/installer.nsi"
# portable copy: the program beside an empty VaxMaps-data folder, which keeps settings and cache there
rm -rf "$W/portable" && mkdir -p "$W/portable/VaxMaps-$VERSION-portable/VaxMaps-data"
cp "$W/vaxmaps-build/VaxMaps.exe" "$HERE/../README.md" "$W/portable/VaxMaps-$VERSION-portable/"
(cd "$W/portable" && zip -q -r "$W/VaxMaps-$VERSION-portable.zip" "VaxMaps-$VERSION-portable")
echo "$W/VaxMaps-$VERSION-portable.zip"
echo "$W/vaxmaps-build/VaxMaps.exe"
echo "$W/VaxMaps-$VERSION-setup.exe"
