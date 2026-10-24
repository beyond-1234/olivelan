# Olivelan 工程脚本集 (Scripts Directory)

本目录收录 Olivelan 项目在打包分发、自动化构建与核心质量门禁验证时所使用的精简工具脚本。

---

## 1. 目录结构

```text
scripts/
├── dist/                     # 跨平台自包含打包脚本
│   ├── build_linux_appimage.sh  # 生成 Ubuntu 22.04 LTS 兼容的 Linux Fat AppImage
│   ├── build_linux_dist.sh      # 制作 Linux 自包含 tar.gz 运行包
│   └── build_macos_dist.sh      # 制作 macOS 自包含 .app / .dmg 分发包
├── verify/                   # 核心渲染与硬件门禁验证脚本
│   ├── verify_zero_copy.sh      # 遥测硬解硬件帧与零拷贝 CPU 回读数
│   ├── verify_gpu_color.sh      # 离屏 Cocoa / OpenGL GPU 像素与 OCIO CPU 结果比对
│   ├── verify_mixed_hdr_sdr.sh  # 混合 HDR/SDR 时间线与 Sticky HDR 状态测试
│   └── verify_frame_dump.sh     # 帧缓冲区像素校验与转储测试
└── generate_metro_icons.py   # Metro 扁平黑体主题图标生成脚本
```

---

## 2. 核心脚本用法说明

### 2.1 零拷贝遥测验证 (`scripts/verify/verify_zero_copy.sh`)
用于严密防范“日志显示硬件解码，底层偷偷进行 CPU 内存回读”的虚假零拷贝：
```bash
# 默认使用 build/app/Olive.app，播放 2 秒并核验 hardware_frames > 0 与 cpu_readback == 0
OLIVE_APP="build/app/Olive.app/Contents/MacOS/Olive" bash scripts/verify/verify_zero_copy.sh <工程路径.ove>
```

### 2.2 离屏 GPU 像素级色彩验证 (`scripts/verify/verify_gpu_color.sh`)
在无窗口/离屏 OpenGL 核心上下文下运行，严密比对 GPU 片元着色器回读像素与 OCIO CPU 参考数学结果（单通道误差 < 0.005）：
```bash
bash scripts/verify/verify_gpu_color.sh
```

### 2.3 跨平台自包含打包 (`scripts/dist/`)
- **Linux AppImage**：
  在 Ubuntu 22.04 容器内执行 `bash scripts/dist/build_linux_appimage.sh`，生成包含 Qt 6.8、OCIO 2.5 和 FFmpeg 6.1 运行时的单文件 AppImage；
- **macOS 自包含分发**：
  在 macOS 宿主机上执行 `bash scripts/dist/build_macos_dist.sh`，自动调用 `macdeployqt` 打包 Frameworks 与依赖，生成开箱即用的 `.app`。
