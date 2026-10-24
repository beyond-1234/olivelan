#!/usr/bin/env bash
# ==============================================================================
# macOS 自包含打包脚本。**必须在 macOS 上执行**（VideoToolbox/CGL 编译期绑定 Apple SDK）。
#
# 产出：dist/macos-<arch>/Olive.app（.app bundle）+ olive-macos-<arch>.tar.gz
#
# 依赖来源：MacPorts（推荐，隔离到 /opt/local，不污染 /usr/local）。
#   安装：sudo port install cmake qt6 ffmpeg
#         sudo port install opencolorio      # 必需：OCIO 是唯一色彩管线
#
# 用法：
#   ARCH=x86_64 bash scripts/build_macos_dist.sh          # Intel Mac
#   ARCH=arm64  bash scripts/build_macos_dist.sh          # Apple Silicon
#
# 环境变量：
#   DEPS_PREFIX : 依赖前缀（MacPorts 默认 /opt/local；Homebrew 则 /usr/local）
#   ARCH        : 目标架构（默认 x86_64）
#   OCIO_LOCATION     : OpenColorIO 安装前缀（默认使用 DEPS_PREFIX）
# ==============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARCH="${ARCH:-x86_64}"
SRC="${SRC:-$ROOT}"
BUILD="${BUILD:-$ROOT/build}"
APP="$ROOT/dist/macos-$ARCH/Olive.app"
DEPS_PREFIX="${DEPS_PREFIX:-/opt/local}"

echo "[INFO] 构建 macOS $ARCH 自包含包 (依赖前缀: $DEPS_PREFIX)"

# --- 1. 配置 & 构建 ---
# MacPorts 布局：include/lib 直接在 $DEPS_PREFIX 下；qt6 在 $DEPS_PREFIX/libexec/qt6
# Olive 的 find module 已硬编码 /opt/local（FFmpeg）；OCIO 现在是硬依赖。
export CMAKE_PREFIX_PATH="$DEPS_PREFIX/libexec/qt6;$DEPS_PREFIX"
export OCIO_LOCATION="${OCIO_LOCATION:-$DEPS_PREFIX}"

cmake -S "$SRC" -B "$BUILD" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
  -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH" \
  -DBUILD_TESTS=OFF \
  -DOLIVE_FETCH_METRO_FONT=OFF \
  -DOLIVE_REGENERATE_METRO_ICONS=OFF

cmake --build "$BUILD" -j"$(sysctl -n hw.ncpu)"

# --- 2. 组装 .app bundle ---
rm -rf "$APP"
mkdir -p "$ROOT/dist/macos-$ARCH"
# CMake 在 macOS 下将 target olive-editor 直接输出为标准的 Bundle: app/Olive.app
if [ -d "$BUILD/app/Olive.app" ]; then
  cp -Rp "$BUILD/app/Olive.app" "$APP"
elif [ -f "$BUILD/app/olive-editor" ]; then
  mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Frameworks" "$APP/Contents/PlugIns" "$APP/Contents/Resources"
  cp -p "$BUILD/app/olive-editor" "$APP/Contents/MacOS/Olive"
fi

# --- 3. 用 macdeployqt 拉入 Qt 依赖 + 插件 ---
# MacPorts 的 qt6 工具在 $DEPS_PREFIX/libexec/qt6/bin；Homebrew qt@6 在 opt/qt6/bin。
MACDEPLOYQT=""
for cand in \
  "$DEPS_PREFIX/libexec/qt6/bin/macdeployqt" \
  "$DEPS_PREFIX/bin/macdeployqt" \
  "$DEPS_PREFIX/opt/qt@6/bin/macdeployqt" \
  "/usr/local/opt/qt6/bin/macdeployqt"; do
  [ -x "$cand" ] && { MACDEPLOYQT="$cand"; break; }
done

if [ -n "$MACDEPLOYQT" ]; then
  "$MACDEPLOYQT" "$APP" -verbose=1
else
  echo "[WARN] macdeployqt 未找到，跳过 Qt 依赖自动收集。" >&2
  echo "       请确认已安装 qt6（MacPorts: sudo port install qt6）" >&2
fi

# --- 4. 手动拉入 FFmpeg/OCIO 等 dylib 并重定位 @rpath ---
# OpenImageIO/OpenEXR 已不在依赖内（patches 0032/0033）；Imath 作为
# libOpenColorIO 的传递依赖随 OCIO 一起处理。
pull_dylib() {
  local lib="$1"
  local name; name="$(basename "$lib")"
  cp -pL "$lib" "$APP/Contents/Frameworks/"
  install_name_tool -id "@rpath/$name" "$APP/Contents/Frameworks/$name"
  # 重写该 dylib 对 $DEPS_PREFIX 下其他库的依赖为 @rpath
  while IFS= read -r dep; do
    [ -n "$dep" ] || continue
    local depname; depname="$(basename "$dep")"
    install_name_tool -change "$dep" "@rpath/$depname" "$APP/Contents/Frameworks/$name"
  done < <(otool -L "$APP/Contents/Frameworks/$name" | awk '{print $1}' | grep "^$DEPS_PREFIX" || true)
}

# 动态定位 dylib（MacPorts 布局：$DEPS_PREFIX/lib/lib*.dylib）
DYLIB_STEMS=(
  libavcodec libavformat libavutil libavfilter libswscale libswresample
)
DYLIB_STEMS+=(libOpenColorIO libImath)
for stem in "${DYLIB_STEMS[@]}"; do
  first="$(ls -1 "$DEPS_PREFIX/lib/${stem}"*.dylib 2>/dev/null | sort -V | tail -1 || true)"
  [ -n "$first" ] && pull_dylib "$first"
done

# 重写主二进制对这些库的引用为 @rpath
MAIN_BIN="$APP/Contents/MacOS/Olive"
[ -f "$MAIN_BIN" ] || MAIN_BIN="$APP/Contents/MacOS/olive-editor"

for name in $(ls "$APP/Contents/Frameworks" 2>/dev/null); do
  # No match is normal for Qt frameworks already rewritten by macdeployqt. Keep the
  # command-substitution status from aborting the whole packaging step under `set -e`.
  old="$(otool -L "$MAIN_BIN" 2>/dev/null | awk '{print $1}' | grep "^$DEPS_PREFIX" | grep "/$name$" | head -1 || true)"
  [ -n "$old" ] && install_name_tool -change "$old" "@rpath/$name" "$MAIN_BIN" 2>/dev/null || true
done
install_name_tool -add_rpath "@executable_path/../Frameworks" "$MAIN_BIN" 2>/dev/null || true

# --- 5. OCIO 配置（必需）---
# 优先复制本次构建生成的配置（含 bundled LUT 与 HDR 空间），避免使用
# 系统 OCIO 默认配置导致包内配置与内嵌 :/ocioconf 不一致。
if [ -d "$BUILD/app/render/ocioconf" ]; then
  cp -a "$BUILD/app/render/ocioconf" "$APP/Contents/Resources/ocioconf"
elif [ -d "$ROOT/olive-0.2.0-nightly/app/render/ocioconf" ]; then
  cp -a "$ROOT/olive-0.2.0-nightly/app/render/ocioconf" "$APP/Contents/Resources/ocioconf"
elif [ -d "$ROOT/dist/linux-x86_64/share/ocioconf" ]; then
  cp -a "$ROOT/dist/linux-x86_64/share/ocioconf" "$APP/Contents/Resources/ocioconf"
elif [ -d "$DEPS_PREFIX/share/OpenColorIO" ]; then
  cp -a "$DEPS_PREFIX/share/OpenColorIO" "$APP/Contents/Resources/ocioconf"
fi

# --- 6. tar.gz ---
cd "$ROOT/dist"
tar czf "olive-macos-$ARCH-$(date +%Y%m%d).tar.gz" "macos-$ARCH"
echo "[OK] 完成: $ROOT/dist/olive-macos-$ARCH-$(date +%Y%m%d).tar.gz"
