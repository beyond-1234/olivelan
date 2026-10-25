# Olivelan Video Editor

<p align="center">
  <b>现代化、高性能、GPU 驻留直通的跨平台开源非线性视频编辑软件</b><br>
  <i>A Modern, High-Performance, GPU-Resident Cross-Platform Non-Linear Video Editor</i>
</p>

<p align="center">
  <a href="#简体中文">简体中文</a> | <a href="#english">English</a>
</p>

---

## 简体中文

### 🌟 项目愿景与设计哲学

**Olivelan** 是在开源非编 Olive 基础上深度演进的独立现代版本。

我们认为，桌面视频剪辑实时预览的物理瓶颈**不在于显卡计算能力，而在于总线搬运（显存到内存的 Readback 税）与内存锁竞争**。Olivelan 彻底打破了传统开源剪辑软件层层抽象的性能病灶，确立了以下核心工程基准：

1. **游戏引擎式 GPU 驻留 (GPU-Resident Pipeline)**：
   - 坚持「硬解直通 $\to$ 显存原位色彩转换 $\to$ 视口合成渲染零回读」；
   - 4K60 复杂时间线回放总线搬运量降为 **0 MB/s**，单帧上屏耗时低至 **0.13ms**。
2. **纯粹的单轨色彩架构 (Single-Source Color Architecture)**：
   - 坚决废除双轨分支与内置简化着色器，全量绑定 **OpenColorIO (OCIO) 2.5.1** 现代色彩管线；
   - 支持 HDR10/HLG 动态 Sticky 视口、工业级摄影机 Log（S-Log3/LogC3/V-Log）18% 线性化映射、ACEScg 宽色域以及用户 `.cube`/`.spi1d`/`.spi3d` LUT 节点。
3. **极简现代依赖体系 (Minimal Modern Tech Stack)**：
   - 彻底剥离历史包袱沉重的 OpenImageIO、OpenEXR 与 Imath 依赖；
   - 树内 Vendor 化核心数学库 `ext/core`；升级至 **Qt 6.8 LTS** 与 KDAB 官方 **KDDockWidgets 2.x**；
   - 采用内嵌 LZ4 快速压缩的自描述帧池容器（`.olvcache`）。
4. **多厂商硬件抽象层 (HAL)**：
   - **Tier 1 (N卡)**：NVIDIA NVDEC + CUDA Runtime 原位互操作；
   - **Tier 2 (系统原生多态)**：Linux 原生 VA-API DMA-BUF (`dma-buf fd`) 与 macOS VideoToolbox (`IOSurface`) 零拷贝直通；
   - **Tier 3 (安全防欠载保底)**：强化原生软解保底，内置全局线程配额（`Tier3ThreadBudget`，保底预留 2 核）与 1.0 秒音频预排队，彻底消除多实例过订阅卡顿。
5. **Metro / Fluent 现代化声明式设计系统**：
   - 0 圆角、高对比度黑曜石卡片风格；
   - 声明式三栏工作区架构，控件色彩全面由 `tokens.json` 动态令牌与 QSS 宏预处理器接管；
   - 内置开源可商用 Adobe 思源黑体（Source Han Sans）与 Inter 可变字体，彻底告别乱码方块。

---

### 📚 核心技术文档索引

系统详细架构设计、实测数据与避坑规范收录在 [`docs/`](docs/) 目录：

- 🏛️ [01. 系统总体架构与工程底座](docs/01-architecture-overview.md)：设计哲学、依赖管理边界与 Metro 设计系统。
- ⚡ [02. 多厂商 HAL 硬件解码与零拷贝管线](docs/02-hal-zero-copy-pipeline.md)：三级分流架构、Linux Wayland/X11 拓扑主矩阵与四大避坑实录。
- 🎨 [03. OCIO 2.5 现代单轨色彩管线规范](docs/03-color-pipeline-ocio.md)：单轨哲学、HDR 动态感知、Log 线性化与用户 LUT 节点。
- 📊 [04. 物理实测基准、已知问题与排障指南](docs/04-benchmarks-and-known-issues.md)：4K60 实测收益对比、Linux 驱动注意项与硬件待办。
- 🗺️ [05. 产品形态演进与全景路线图](docs/05-product-design-and-roadmap.md)：从 Phase 0 到 Phase 15C 研发回溯与未来规划。
- 🧩 [06. 统一资源中心、开放插件与 AI 生态](docs/06-plugin-and-ai-ecosystem.md)：Phase 17 零私有格式架构（Folder Mount / GL-Transitions）与 Phase 16 MCP AI 体系。

---

### 🛠️ 源码构建与验证

#### 依赖环境要求
- **CMake**：≥ 3.22（推荐 3.28+）
- **C++ 编译器**：支持 C++17 标准（GCC 11.4+ / Clang 14+ / AppleClang）
- **Qt**：≥ 6.6（推荐 **Qt 6.8 LTS**，需 Widgets / OpenGL / Concurrent）
- **FFmpeg**：≥ 6.1（开发包：`libavcodec`, `libavformat`, `libavutil`, `libswscale`, `libswresample`）
- **OpenColorIO**：≥ 2.2（推荐 **2.5.1**）
- **PortAudio**：用于跨平台低延迟音频输出

#### 快速构建流程
```bash
# 1. 克隆代码库（递归检出 ext/KDDockWidgets 子模块）
git clone --recurse-submodules https://github.com/beyond-1234/olivelan.git
cd olivelan

# 2. 配置 CMake
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON ..

# 3. 编译全量目标与测试套件
cmake --build . -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

# 4. 运行全量单元测试矩阵 (24 项测试全绿)
ctest --output-on-failure
```

#### 打包分发与质量验证
工程自包含打包脚本与核心质量门禁收录于 [`scripts/`](scripts/) 目录（现阶段官方二进制发布聚焦于 Linux AppImage，macOS 暂未纳入官方预编译分发）：
```bash
# 制作 Linux AppImage（兼容 Ubuntu 22.04 LTS / glibc 2.35+，唯一官方推荐独立运行包）
bash scripts/dist/build_linux_appimage.sh

# 执行 GPU 离屏像素级色彩一致性测试
bash scripts/verify/verify_gpu_color.sh
```

---

## English

### 🌟 Project Vision & Philosophy

**Olivelan** is an advanced, independent evolution of the open-source Olive video editor.

We argue that the real physical bottleneck in NLE real-time playback **is not GPU compute capacity, but bus transmission overhead (host-to-device and readback taxes) and lock contention**. Olivelan breaks through traditional open-source video editor bottlenecks by establishing the following core engineering pillars:

1. **GPU-Resident Pipeline**:
   - Zero-copy hardware decode $\to$ in-place GPU color conversion $\to$ viewport compositing with zero CPU readback.
   - Reduces bus traffic to **0 MB/s** on 4K60 timelines with frame presentation latency as low as **0.13ms**.
2. **Single-Source Color Architecture**:
   - Eliminates dual-track shaders and built-in legacy fallbacks, standardizing strictly on **OpenColorIO (OCIO) 2.5.1**.
   - Supports HDR10/HLG dynamic sticky views, industrial camera log (S-Log3/LogC3/V-Log) 18% linearization, native ACEScg wide-gamut workflows, and user `.cube`/`.spi1d`/`.spi3d` LUT nodes via `OCIOFileTransformNode`.
3. **Streamlined Modern Dependencies**:
   - Completely removes legacy VFX baggage: OpenImageIO, OpenEXR, and Imath.
   - Vendors core math in-tree (`ext/core`), adopts **Qt 6.8 LTS**, and tracks KDAB upstream **KDDockWidgets 2.x**.
   - Integrates LZ4 compression for native, lightweight `.olvcache` frame pool containers.
4. **Multi-Vendor Hardware Abstraction Layer (HAL)**:
   - **Tier 1 (NVIDIA)**: NVDEC + CUDA Runtime in-place interop.
   - **Tier 2 (Native OS Multipath)**: Linux VA-API DMA-BUF (`dma-buf fd`) and macOS VideoToolbox (`IOSurface`) zero-copy pass-through.
   - **Tier 3 (Reinforced Software Fallback)**: Built-in global thread budgeting (`Tier3ThreadBudget`, always reserving 2 cores) and 1.0-second audio pre-queueing to eliminate underruns under heavy load.
5. **Modern Metro / Fluent Declarative Design System**:
   - High-contrast obsidian card styling with square geometry.
   - Declarative three-column workspace layout with design tokens driven by `tokens.json` and a QSS macro preprocessor.
   - Bundles Adobe Source Han Sans and Inter Variable fonts for out-of-the-box CJK rendering without missing-glyph tofu boxes.

---

### 📚 Technical Documentation

Comprehensive architecture designs, benchmark measurements, and Linux desktop matrix notes are located in [`docs/`](docs/):

- 🏛️ [01. Architecture Overview](docs/01-architecture-overview.md): Core philosophy, dependency boundaries, and Metro design system.
- ⚡ [02. HAL Zero-Copy Pipeline](docs/02-hal-zero-copy-pipeline.md): Tiered routing, Linux Wayland vs. X11 topology matrices, and postmortems of 4 major zero-copy traps.
- 🎨 [03. Color Pipeline & OCIO 2.5](docs/03-color-pipeline-ocio.md): Single-track philosophy, HDR detection, Log curves, and user LUT handling.
- 📊 [04. Benchmarks & Troubleshooting](docs/04-benchmarks-and-known-issues.md): 4K60 physical gains, NVIDIA proprietary driver prerequisites, and hardware checklist.
- 🗺️ [05. Product Roadmap](docs/05-product-design-and-roadmap.md): Milestone chronology (Phase 0 to 15C) and future roadmap.
- 🧩 [06. Plugin Hub & AI Ecosystem](docs/06-plugin-and-ai-ecosystem.md): Phase 17 zero-proprietary-format hub (Folder Mount / GL-Transitions) and Phase 16 MCP AI integration.

---

### 🛠️ Building & Verification

#### Prerequisites
- **CMake**: ≥ 3.22 (3.28+ recommended)
- **C++ Compiler**: C++17 compliant (GCC 11.4+ / Clang 14+ / AppleClang)
- **Qt**: ≥ 6.6 (**Qt 6.8 LTS** recommended, with Widgets / OpenGL / Concurrent)
- **FFmpeg**: ≥ 6.1 (`libavcodec`, `libavformat`, `libavutil`, `libswscale`, `libswresample`)
- **OpenColorIO**: ≥ 2.2 (**2.5.1** recommended)
- **PortAudio**: Low-latency cross-platform audio backend

#### Quick Build
```bash
# 1. Clone repository with submodules
git clone --recurse-submodules https://github.com/beyond-1234/olivelan.git
cd olivelan

# 2. Configure with CMake
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON ..

# 3. Build all targets
cmake --build . -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)

# 4. Run test suite (24/24 tests passing)
ctest --output-on-failure
```

---

## License

Olivelan is licensed under the **GNU General Public License version 3 (GPLv3)**. See [LICENSE](LICENSE) for details.  
Original copyright (C) 2018-2022 Olive Team (Matt KC, etc.). Portions copyright (C) 2024-2026 Olivelan Contributors.
