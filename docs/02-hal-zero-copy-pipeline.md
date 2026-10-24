# 02. 多厂商 HAL 硬件解码与零拷贝管线 (HAL Zero-Copy Pipeline)

## 1. 架构定位：硬件抽象层 (HAL)

在现代非线性编辑系统（NLE）中，4K60 及更高规格预览的物理瓶颈**不在于显卡计算能力，而在于 PCIe 总线搬运（显存到内存的 Readback 税）与内存锁竞争**。

Olivelan 通过 `IVideoHardwarePipeline` 抽象与 `HardwarePipelineFactory` 工厂模式，构建了统一硬件分流与分级架构：

```text
               视频源帧 (H.264 / HEVC / ProRes / DNxHD)
                                  │
                  ┌───────────────┴───────────────┐
                  ▼                               ▼
          硬件硬解适用格式                      原生帧 / CPU 软解
                  │                               │
        ┌─────────┴─────────┐                     │
        ▼                   ▼                     │
     Tier 1              Tier 2                   │
(NVIDIA NVDEC)     (系统原生多态零拷贝)             │
        │          ┌────────┴────────┐            │
        │          ▼                 ▼            │
        │      Linux VA-API    macOS VideoToolbox │
        │        (DMA-BUF)        (IOSurface)     │
        │          │                 │            │
        └──────────┼─────────────────┘            │
                   ▼                              ▼
          显存原位纹理槽位                   Tier 3 强化软解保底
        (OpenGL Resident Pool)         (全局线程配额 + 音频防欠载)
                   │                              │
                   └──────────────┬───────────────┘
                                  ▼
                         合成与视口渲染引擎
```

---

## 2. 三级分流通路详解

### 2.1 Tier 1: NVIDIA CUDA-NVDEC（原位极速直通）
- **核心链路**：
  - FFmpeg `AV_PIX_FMT_CUDA` 硬件解码输出 NV12 显存帧；
  - 调度 `cuda_nv12_to_rgba.cu` 核函数完成显存内 NV12 $\to$ RGBA 转换（单帧耗时仅 ~0.13ms）；
  - 通过 `cudaGraphicsGLRegisterImage` / `cudaGraphicsMapResources` 原位映射为 OpenGL 纹理，总线搬运量严格为 0。

### 2.2 Tier 2: 平台原生多态零拷贝
- **Linux 平台 (`VaapiDmaBufPipeline`)**：
  - VA-API 解码输出 `VASurfaceID`；
  - 导出 Linux 内核标准 **DMA-BUF (`dma-buf fd`)**，提取平面 `stride`、`offset` 及 `modifier`；
  - 通过 `eglCreateImageKHR` 将 Y/UV 双平面分别导入为两张 `GL_TEXTURE_2D` 纹理，随后通过片元着色器合并写入 `GL_RGBA16F` / `GL_RGBA8` 纹理池。
- **macOS 平台 (`VideoToolboxPipeline`)**：
  - VideoToolbox 硬解输出 `CVPixelBufferRef`；
  - 提取底层共享内存 `IOSurfaceRef`；
  - 通过 `CGLTexImageIOSurface2D` 零拷贝绑定为 `GL_TEXTURE_RECTANGLE`。

### 2.3 Tier 3: Olive 强化原生软解保底 (Safety Net)
针对老旧显卡、格式不兼容或硬件驱动异常时的安全防线：
- **全局线程配额调度 (`Tier3ThreadBudget`)**：
  - 动态感知 CPU 核心数，**始终保底预留 2 核专供操作系统、实时音频渲染与 UI 响应**，彻底消除解码器过订阅导致的音频撕裂与卡死；
  - 主视口保障 2~6 个线程，后台流受全局水位压制。
- **消除破坏性跳帧**：
  - 动态采用 `AVDISCARD_DEFAULT` 解码全量 B/P 帧，消除开放 GOP 切片报错。
- **音频预排队水位倍增**：
  - 起播预缓冲安全水位提升至 1.0 秒（4 个缓冲块），彻底吸收复杂 4K 帧软解初始计算抖动。

---

## 3. Linux 平台桌面环境矩阵与物理实测法则

Linux 下的视频呈现与硬解直通受 **显示服务器（Wayland vs X11）、合成器（Compositor）及显卡拓扑（核显 / 独显 / 混合 PRIME）** 严密制约。

经过大量的实机探针测试，我们总结出 Linux 平台零拷贝的核心物理定律：  
👉 **「解码设备必须等于呈现设备，跨卡 Client Buffer 必产生严重退化或故障」**。

### 3.1 桌面环境与显卡拓扑分流主矩阵

| 序号 | 会话环境 | 显卡硬件拓扑 | 呈现（合成器）落在 | 正确管线路由 | 真零拷贝状态 | 运行机制与实测依据 |
|---|---|---|---|:---:|:---:|---|
| **#1** | **原生 Wayland** | **纯核显 (AMD/Intel)** | 集显 (如 780M / Iris) | **Tier 2 (VA-API)** | ✅ **真零拷贝** | 【实测】`dma-buf` 直接在同一 DRI 节点（`card1`）挂载为 EGLImage，回读为 0。 |
| **#2** | **原生 Wayland** | **纯独显 (NVIDIA)** | NVIDIA 独显 | **Tier 1 (CUDA)** | ✅ **真零拷贝** | 专有驱动接管 Wayland 合成，CUDA-GL 同卡同上下文原位互操作。 |
| **#3** | **原生 Wayland** | **混合显卡 (笔记本)** | **核显** (屏幕物理直连) | **Tier 2 (核显解码)** | ✅ **真零拷贝** | 【实测】系统自动跳过 Tier 1，选择核显 VA-API 直通呈现，避免跨卡传输。 |
| **#4** | **X11 / Xwayland** | **纯独显 (NVIDIA)** | NVIDIA 独显 | **Tier 1 (CUDA)** | ✅ **真零拷贝** | 【实测】传统 X11 直通，帧率稳定 60fps。 |
| **#5** | **X11 / Xwayland** | **混合显卡 + PRIME** | NVIDIA 独显 | **Tier 1 (CUDA)** | ✅ **真零拷贝** | 【实测】通过 PRIME 卸载将整个应用运行在独显上，独显解码并呈现。 |
| **#6** | **X11 / Xwayland** | **混合显卡 (无 PRIME)** | 核显 | **Tier 2 (VA-API)** | ✅ **真零拷贝** | 默认路由回退至核显解码，走 VA-API 直通。 |

---

## 4. 深度复盘：Linux 硬件直通的四大“致命陷阱”与避坑规范

在实机探索中，出现过多次“看似能跑、日志正常，实际完全不可用”的隐蔽缺陷。以下是必须固化的避坑防线：

### 陷阱 1：跨 GPU 互操作静默失败（Silent Context Failure）
- **现象**：
  在原生 Wayland 混合显卡笔记本上，若强制配置使用 Tier 1，启动日志提示 `Selected Tier 1: CUDA-NVDEC`，但时间线黑屏、播放无画面，帧池可用槽位为 0。
- **物理机理**：
  Wayland 合成器（KWin / Mutter）运行在核显上，OpenGL Context 也绑定在核显；而 CUDA 硬解运行在 NVIDIA 独显上。
  `cudaGraphicsGLRegisterImage` 无法跨硬件设备注册 OpenGL 纹理，底层抛出 `cudaErrorInvalidGraphicsContext` 静默失败。
- **系统防线（Patch 0076）**：
  在 `GpuEnvironmentProbe` 中引入 `IsCudaGlInteropBlocked()` 闸门。若探测到当前 GL 上下文与 CUDA 设备不一致，**强制跳过 Tier 1，自动优雅降级为 Tier 2 或 Tier 3**。

### 陷阱 2：原生 Wayland 下导 PRIME 变量导致整窗白屏与残影坏死（The White-Screen Trap）
- **现象**：
  - **单屏/内屏场景**：在原生 Wayland 环境下若强加 `__NV_PRIME_RENDER_OFFLOAD=1` 环境变量，互操作虽然成功，但**Olivelan 整个窗口变为纯白死屏**。
  - **多屏/外接 HDMI 场景**：在接有外接显示器（如挂在 `card0`）时强导 PRIME，虽然窗口没有白屏，但窗口交换链严重损坏，**视口无限重叠重绘屏幕上出现过的任何其他窗口残影，完全无法交互**。
- **物理机理**：
  PRIME 将应用的 OpenGL 渲染拉到了 NVIDIA 独显上，但 Wayland 合成器仍然驻留在核显上。跨 GPU 的 Client Buffer 穿透由核显接管的 Wayland 上屏链时，`dma-buf` 交换链协议断裂。
- **系统铁律（Patch 0099）**：
  **严禁在原生 Wayland 下自动或诱导用户导出 PRIME 环境变量！**
  混合双显卡笔记本若想使用 NVIDIA 独显解码，必须切换为 X11 / Xwayland 会话运行。

### 陷阱 3：EGL 属性数组 ABI 错位导致导入恒为 `0x300c`（ABI Mismatch）
- **现象**：
  在 AMD 780M / Mesa 驱动下，VA-API 硬解完全正常，驱动也支持 DMA-BUF 导入，但每帧必定导入失败回退 CPU 读回，且 `eglGetError()` 恒为 `0x300c`（`EGL_BAD_PARAMETER`）。
- **物理机理**：
  早期的手写动态加载函数使用了 `intptr_t` 作为 EGL 属性数组指针，而 64 位 Linux 下 `intptr_t` 为 64-bit，Mesa 驱动内部严格按 32-bit `EGLint` 解析属性流，导致传入的 `EGL_LINUX_DRM_FOURCC_EXT` 等参数完全错位。
- **系统防线（Patch 0087 / 0101）**：
  彻底废弃手写函数指针，标准化引入 `EGL/egl.h` 和 `EGL/eglext.h`，严格使用官方 `EGLint` 属性键值对；经真机 `LD_PRELOAD` 拦截 `av_hwframe_transfer_data` 核验，修复后全分辨率导入**回读次数严格为 0**。

### 陷阱 4：缩放预览与全分辨率渲染的“假回读”误报
- **现象**：
  在已打通零拷贝的路径上，抓取某些 10-bit 素材首次加载时，偶尔会拦截到 1 次 `av_hwframe_transfer_data` CPU 回读。
- **物理机理**：
  这是设计内行为，而非管线故障。
  `ffmpegdecoder.cpp` 严格约定：**只有在全分辨率渲染（`divider == 1` 且无逐行交错反交错）时才走直通纹理**；首次导入生成素材箱超小缩略图（例如 `divider = 24`，3840×1600 $\to$ 160×66）时，会触发一次小尺寸帧的软降采样。
- **系统规范（Patch 0102）**：
  日志中准确标注为：`no CPU readback on this full-resolution path; scaled previews bypass this path`，消除误报疑虑。

---

## 5. 零拷贝性能验证与测试手段

为了避免“日志报成功，后台偷偷在回读”的假读数，Olivelan 建立了多维度的独立交叉验证体系：

1. **`scripts/verify/verify_zero_copy.sh` 遥测拦截**：
   - 通过进程级探针，持续监控硬件帧生产数（`hardware_frames`）与主机回读数（`cpu_readback_frames`）。验收合格标准：`hardware_frames > 0` 且 `cpu_readback_frames == 0`。
2. **底层符号拦截器 (`readback_shim`)**：
   - 通过 `LD_PRELOAD` 劫持 FFmpeg 的 `av_hwframe_transfer_data`，任何悄悄跨 PCIe 搬移显存的动作都会触发计数断言。
3. **连续回放丢帧遥测 (`--autoplay N`)**：
   - 暴露隐藏的统计探针，在无头或自动化脚本中跑满 N 秒，精确输出 `presented`（呈现帧）、`expected`（期望帧）和 `skipped`（丢弃帧），以硬指标考核 4K60 的流畅度。
