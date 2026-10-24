# 01. 系统总体架构与工程底座 (Architecture Overview)

## 1. 核心设计哲学

Olivelan 是一款现代化、高性能、专为现代影视制作与短视频创作者打造的跨平台非线性视频编辑软件（NLE）。

本项目彻底摒弃了传统开源剪辑软件因层层抽象与跨总线拷贝导致的性能顽疾，确立了以下三大底层核心原则：

1. **游戏引擎式 GPU 驻留 (GPU-Resident Pipeline)**：
   - 消除总线搬运与内存锁，而非单纯堆砌算力。
   - 核心链路坚持「硬解直通 $\to$ 显存原位色彩转换 $\to$ 渲染引擎零回读直通」。
2. **纯粹的单轨色彩架构 (Single-Source Color Architecture)**：
   - 绝不妥协双轨逻辑，全面绑定 OpenColorIO (OCIO) 2.5 现代色彩引擎，杜绝色彩在不同预览配置下的静默失真。
3. **极简、高现代性的工程依赖树 (Minimal Modern Dependencies)**：
   - 彻底剥离历史包袱沉重的庞大 VFX 依赖，拥抱轻量化容器与原生系统多态能力。

---

## 2. 依赖管理与解耦边界

在系统的长期重构中，底层依赖体系已实现严格的轻量化收敛：

| 依赖库 | 状态 | 规范与版本基线 | 职责说明 |
|---|:---:|---|---|
| **FFmpeg** | 核心必需 | **≥ 6.1**（产品随包固定） | 唯一媒体解码、解复用、音频重采样与图像序列导出底层。全面适配 `AVChannelLayout` 现代音频规范。 |
| **Qt 6** | 核心必需 | **Qt 6.8 LTS**（最低 ≥ 6.6） | 核心事件循环、QOpenGLWidget 视口、高 DPI 自适应及跨平台基础库。 |
| **OpenColorIO** | 核心必需 | **OCIO 2.5.1**（最低 ≥ 2.2） | 唯一随包分发色彩引擎，负责 Display View、Camera Log、ACEScg 转换与用户 LUT 加载。 |
| **PortAudio** | 核心必需 | 源码编译并随包绑定 | 跨平台低延迟音频播放引擎，支持设备热插拔与平滑淡出安全停流（Ramp-Drain-Stop）。 |
| **KDDockWidgets** | 树内编译 | **KDAB 官方 v2.1.0 标签** | 工业级停靠面板管理器，支持多窗口浮动与布局持久化。 |
| **LZ4** | 源码内嵌 | `third_party/lz4` | 高速 `.olvcache` 帧池容器压缩引擎，极低 CPU 开销。 |
| **olivecore** | In-tree | 树内 Vendor 化模块 (`ext/core`) | 基础数学（`Vec2d`、`half`）、有理数时间基准、时间码处理。**完全剔除 Imath 依赖**。 |

### 严禁重新引入的历史依赖（已全量剔除）：
- ❌ **OpenImageIO (OIIO)**：图像读写全量归口 FFmpeg；
- ❌ **OpenEXR**：帧缓存改为自描述 `OLVC`+LZ4 容器；
- ❌ **Imath**：半精度浮点与向量已由轻量原生数学单元替代；
- ❌ **Direct3D 11 (D3D11)**：Windows 平台支持已全盘废除，相关胶水假桩已物理清除。

---

## 3. UI 架构与设计系统 (Metro / Fluent)

Olivelan 采用了全新的现代黑体磁贴设计系统与声明式工作区架构：

### 3.1 声明式工作区能力 (`ThemeCapabilities`)
主题不再只是修改文字颜色的皮肤，而是能声明界面的宏观布局模式：
- **三栏式现代工作区 (`threeColumn`)**：左侧素材中心、中间视口播放、右侧统一属性检查器、底部全景多轨时间线；
- **智能面板折叠**：根据工作流场景，可按需声明隐藏工具条（ToolPanel）、节点视口（NodePanel）或素材预览窗，释放屏幕空间。

### 3.2 动态设计令牌 (`DesignTokenRegistry`) 与 QSS 宏预处理
- 界面控件与自绘组件（时间线、节点图、素材卡片）统一通过 `tokens.json` 消费设计令牌：
  - `color.accent.primary`：现代青色高亮色（`#00C4CC`）；
  - `color.canvas.base`：深邃黑曜石画布色（`#16171A`）；
  - `color.card.fill`：浮层与面板卡片背景（`#23252B`）；
  - `stroke.hairline.width`：1px 精致发丝边框。
- QSS 样式表通过 `QssMacroPreprocessor` 支持 `var(--token.name)` 动态求值与热重载。
