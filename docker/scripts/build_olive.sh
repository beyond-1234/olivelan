#!/usr/bin/env bash

git clone --depth 1 https://github.com/olive-editor/olive.git
cd olive
mkdir build
cd build
cmake .. -G "Ninja"
cmake --build .

cmake --install app --prefix appdir/usr

# TODO: Can the following libs be excluded?
#libQt5DBus.so,\
#libQt5MultimediaGstTools.so,\
#libQt5MultimediaWidgets.so,\

/usr/local/linuxdeployqt-x86_64.AppImage \
  appdir/usr/share/applications/org.olivevideoeditor.Olive.desktop \
  -appimage \
  -exclude-libs=\
libQt5Pdf.so,\
libQt5Qml.so,\
libQt5QmlModels.so,\
libQt5Quick.so,\
libQt5VirtualKeyboard.so \
  -bundle-non-qt-libs \
  --appimage-extract-and-run

./Olive*.AppImage --appimage-extract-and-run --version
