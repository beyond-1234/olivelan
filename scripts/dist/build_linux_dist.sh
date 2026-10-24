#!/usr/bin/env bash
# ==============================================================================
# 在 distrobox 容器 `olive-build` 内运行。
# 把已构建好的 `build/olive-full-auto-qt68/app/olive-editor` 及其第三方依赖，
# 打成自包含的 Linux x86_64 可移植目录，输出到 dist/linux-x86_64/。
#
# 目标：测试分发。目标机解压后无需安装任何库（仅依赖系统 glibc + libGL + GPU 驱动）。
# 用法：
#   ./run-remote.sh --box olive-build 'cd /home/name/code/gpu_nle_preview && bash scripts/build_linux_dist.sh'
# ==============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
SRC_BIN="${OLIVE_BIN:-$BUILD_DIR/app/olive-editor}"
DIST="$ROOT/dist/linux-x86_64"
# Qt6 only (minimum 6.6; product pinned 6.8 LTS). Qt5 is no longer supported.
QMAKE_BIN="$(command -v qmake6 || command -v qmake || true)"
QT_VERSION="$([ -n "$QMAKE_BIN" ] && "$QMAKE_BIN" -query QT_VERSION 2>/dev/null || echo unknown)"
QT_PLUGINS="$([ -n "$QMAKE_BIN" ] && "$QMAKE_BIN" -query QT_INSTALL_PLUGINS 2>/dev/null || true)"
PLUGINS_SRC="${PLUGINS_SRC:-${QT_PLUGINS:-/usr/lib/x86_64-linux-gnu/qt6/plugins}}"
if [ "$QT_VERSION" != "unknown" ]; then
  QT_MAJOR="${QT_VERSION%%.*}"
  QT_REST="${QT_VERSION#*.}"
  QT_MINOR="${QT_REST%%.*}"
  if [ "$QT_MAJOR" -lt 6 ] || { [ "$QT_MAJOR" -eq 6 ] && [ "$QT_MINOR" -lt 6 ]; }; then
    echo "[ERROR] Qt $QT_VERSION is below the 6.6 build baseline; product uses Qt 6.8 LTS." >&2
    exit 1
  fi
fi
if [ ! -d "$PLUGINS_SRC" ]; then
  echo "[ERROR] Qt6 plugin directory not found: $PLUGINS_SRC" >&2
  exit 1
fi

# 若产物缺失，尝试自动构建（全量自动探测后端，含 CUDA 副本 nvcc 自动定位）。
if [ ! -f "$SRC_BIN" ]; then
  echo "[INFO] 未找到构建产物，先执行全量自动构建..."
  NVCC="${OLIVE_NVCC:-$ROOT/build/cuda-toolkit-12.4/bin/nvcc}"
  [ -f "$NVCC" ] || NVCC=""
  cmake -S "$ROOT/olive-0.2.0-nightly" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_TESTS=OFF \
    -DOLIVE_FETCH_METRO_FONT=OFF \
    -DOLIVE_REGENERATE_METRO_ICONS=OFF \
    ${NVCC:+-DCMAKE_CUDA_COMPILER="$NVCC"}
  cmake --build "$BUILD_DIR" -j"$(nproc)"
fi

if [ ! -f "$SRC_BIN" ]; then
  echo "[ERROR] 未找到构建产物: $SRC_BIN" >&2
  exit 1
fi

echo "[INFO] 打包 Linux x86_64 自包含目录 (Qt $QT_VERSION)"
rm -rf "$DIST"
mkdir -p "$DIST/bin" "$DIST/lib" "$DIST/plugins" "$DIST/share"

cp -p "$SRC_BIN" "$DIST/bin/olive-editor"

# --- 1. 收集二进制及其所有非系统依赖 (libc/libm/libGL/libEGL 等系统库除外) ---
# 系统级库（目标机自带）与 GPU 驱动层库（随驱动提供）不打包。
# libstdc++/libgcc are deliberately host-owned. The build must therefore target the oldest
# supported glibc/GCC baseline; see harness/build-dependency-baseline.md. Building on a newer
# distro and running on an older one can fail before main() with missing GLIBCXX_*/CXXABI_* symbols.
is_system_lib() {
  case "$1" in
    libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libstdc++.so*|libgcc_s.so*|ld-linux*)
      return 0 ;;
    libGL.so*|libEGL.so*|libGLX.so*|libGLdispatch.so*|libOpenGL.so*)
      return 0 ;;
    libcuda.so*|libnvcuvid.so*|libnvidia-*.so*|libva*.so*|libdrm.so*)
      return 0 ;;
    # Audio stacks are host/driver-owned. Bundling Ubuntu's libasound makes the
    # host PipeWire/JACK ALSA plugins fail with an ALSA_* symbol-version mismatch
    # (the host plugin is newer than the bundled library). Keep the host copy.
    libasound.so*|libjack.so*|libpulse.so*)
      return 0 ;;
  esac
  return 1
}

# BFS 递归收集：从 target 开始，把所有非系统依赖拷贝进 $DIST/lib，再递归
# 处理这些依赖自身的依赖。用进程替换避免 `| while` 子 shell 丢失 queue 更新。
collect_libs() {
  local target="$1"
  local seen="$DIST/lib/.copied"
  mkdir -p "$DIST/lib" "$seen"
  local -a queue=("$target")
  while [ ${#queue[@]} -gt 0 ]; do
    local bin="${queue[0]}"
    queue=("${queue[@]:1}")
    local -a deps=()
    while IFS= read -r lib; do
      [ -z "$lib" ] && continue
      local base; base="$(basename "$lib")"
      is_system_lib "$base" && continue
      if [ ! -f "$seen/$base" ]; then
        cp -pL "$lib" "$DIST/lib/"
        touch "$seen/$base"
        deps+=("$lib")
      fi
    # Capture every absolute dependency, not just /lib and /usr/lib:
    # Qt 6.8 lives under /opt/Qt, OCIO under /usr/local, and CUDA under the
    # project's build/cuda-toolkit tree. The is_system_lib filter below decides
    # which of these must be bundled.
    done < <(ldd "$bin" 2>/dev/null | awk '$2 == "=>" && $3 ~ /^\// {print $3} $1 ~ /^\// {print $1}')
    queue+=("${deps[@]}")
  done
}

collect_libs "$DIST/bin/olive-editor"

# --- 2. 拷贝 Qt 平台/图像插件（Olive 运行时必需） ---
# Wayland needs the shell/decoration/graphics-integration plugin subdirectories;
# copying only platforms/ makes Qt find libqwayland-*.so but fail to load a shell
# integration, after which it silently falls back to xcb.
for subdir in \
  platforms imageformats platformthemes xcbglintegrations iconengines \
  platforminputcontexts wayland-decoration-client wayland-graphics-integration-client wayland-shell-integration; do
  if [ -d "$PLUGINS_SRC/$subdir" ]; then
    mkdir -p "$DIST/plugins/$subdir"
    cp -pL "$PLUGINS_SRC/$subdir"/*.so "$DIST/plugins/$subdir/" 2>/dev/null || true
  fi
done

# 插件自身依赖也收集进 lib/
for plug in "$DIST"/plugins/*/*.so; do
  [ -e "$plug" ] || continue
  collect_libs "$plug"
done

# --- 3. 拷贝 OCIO 默认配置（所有构建都需要）---
# 依赖收集（步骤 1/2）由 ldd 递归驱动，与具体依赖无关，无需在此硬编码 OIIO/EXR。
# OpenColorIO 是唯一色彩管线，配置资源必须随包。优先使用源码配置，
# 保证新增 LUT/色彩空间与内嵌 :/ocioconf 一致。
if [ -d "$BUILD_DIR/app/render/ocioconf" ]; then
  cp -a "$BUILD_DIR/app/render/ocioconf" "$DIST/share/ocioconf"
elif [ -d "$ROOT/olive-0.2.0-nightly/app/render/ocioconf" ]; then
  cp -a "$ROOT/olive-0.2.0-nightly/app/render/ocioconf" "$DIST/share/ocioconf"
elif [ -d "$HOME/.cache/olivevideoeditor.org/Olive/ocioconf" ]; then
  cp -a "$HOME/.cache/olivevideoeditor.org/Olive/ocioconf" "$DIST/share/ocioconf"
fi

# --- 4. 写启动脚本，设置 RPATH/LD_LIBRARY_PATH + Qt 插件路径 ---
cat > "$DIST/run-olive.sh" <<'EOF'
#!/usr/bin/env bash
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Best-effort desktop discovery for SSH / file-manager launches. An explicit
# DISPLAY/XAUTHORITY from the caller always wins.
if [ -z "${DISPLAY:-}" ]; then
  for candidate in :0 :1; do
    if [ -S "/tmp/.X11-unix/X${candidate#:}" ]; then
      export DISPLAY="$candidate"
      break
    fi
  done
fi
if [ -z "${XAUTHORITY:-}" ]; then
  for candidate in "/run/user/$(id -u)"/xauth* "$HOME/.Xauthority"; do
    if [ -r "$candidate" ]; then
      export XAUTHORITY="$candidate"
      break
    fi
  done
fi

export LD_LIBRARY_PATH="$HERE/lib:${LD_LIBRARY_PATH:-}"
export QT_PLUGIN_PATH="$HERE/plugins"

# Do not force QT_QPA_PLATFORM here. GpuSessionAdvisor applies the matching
# platform (and PRIME variables) before QApplication is constructed. Forcing
# xcb here would bypass the advisor's native-session path and make every dist
# launch an Xwayland launch.
#
# Session policy mirrors harness/interactive_test.sh:
#   native   = default; use the native Wayland/X11 session
#   xwayland = opt-in; enables X11(xcb) + PRIME for Tier 1 CUDA-GL on hybrid GPUs
if [ -z "${OLIVE_GPU_SESSION:-}" ] && [ "${HARNESS_XCB:-0}" = "1" ]; then
  export OLIVE_GPU_SESSION=xwayland
fi
if [ -z "${OLIVE_GPU_SESSION:-}" ]; then
  export OLIVE_GPU_SESSION=native
fi

# Consume the small session-routing subset before exec. Everything else is
# passed through unchanged to Olive (project path, --hal-backend, --autoplay,
# --hal-dialog, --help, ...). Session flags may appear before or after the
# project path, matching run-interactive.sh's argument handling.
OLIVE_ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --session)
      if [ $# -lt 2 ]; then
        echo "[ERROR] --session requires native or xwayland" >&2
        exit 2
      fi
      case "$2" in
        native|wayland) export OLIVE_GPU_SESSION=native ;;
        xwayland|xcb)   export OLIVE_GPU_SESSION=xwayland ;;
        *)
          echo "[ERROR] unknown session '$2' (native|xwayland)" >&2
          exit 2
          ;;
      esac
      shift 2
      ;;
    --wayland|--native)
      export OLIVE_GPU_SESSION=native
      shift
      ;;
    --xwayland)
      export OLIVE_GPU_SESSION=xwayland
      shift
      ;;
    *)
      OLIVE_ARGS+=("$1")
      shift
      ;;
  esac
done
set -- "${OLIVE_ARGS[@]}"

# The bundled libasound is from Ubuntu; let it discover the host's ALSA plugin
# directory (Fedora uses /usr/lib64/alsa-lib, Debian/Ubuntu uses the other path).
if [ -z "${ALSA_PLUGIN_DIR:-}" ]; then
  for candidate in /usr/lib/x86_64-linux-gnu/alsa-lib /usr/lib64/alsa-lib /usr/lib/alsa-lib; do
    if [ -d "$candidate" ]; then
      export ALSA_PLUGIN_DIR="$candidate"
      break
    fi
  done
fi

exec "$HERE/bin/olive-editor" "$@"
EOF
chmod +x "$DIST/run-olive.sh"

# 可选：用 patchelf 给二进制写死 $ORIGIN RPATH（若可用），否则依赖上面的 LD_LIBRARY_PATH
if command -v patchelf >/dev/null 2>&1; then
  patchelf --set-rpath '$ORIGIN/../lib' "$DIST/bin/olive-editor" 2>/dev/null || true
  echo "[INFO] 已写入 RPATH \$ORIGIN/../lib"
fi

# --- 5. 生成 tar.gz ---
cd "$ROOT/dist"
TARBALL="olive-linux-x86_64-$(date +%Y%m%d).tar.gz"
tar czf "$TARBALL" linux-x86_64
echo "[OK] 打包完成:"
echo "     $ROOT/dist/$TARBALL"
echo "     目录: $DIST"
du -sh "$DIST" "$ROOT/dist/$TARBALL" 2>/dev/null | sed 's/^/     /'
