# 02. 多厂商 HAL 硬件解码与零拷贝管线 (HAL Zero-Copy Pipeline)

## 1. 架构定位：硬件抽象层 (HAL)

为了消除专有显卡绑定并实现真正跨平台的高性能播放，系统建立了统一的硬件抽象层接口：
`IVideoHardwarePipeline` 与工场模式 `HardwarePipelineFactory`。

运行时根据操作系统与硬件探测（`GpuEnvironmentProbe`）结果，自适应分流至三级分级架构：

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
- **核心原理**：
  - FFmpeg `AV_PIX_FMT_CUDA` 硬解输出 NV12 显存指针；
  - 调用原位 CUDA 核函数（`cuda_nv12_to_rgba.cu`）将 NV12 转换为 RGBA 浮点；
  - 通过 `cudaGraphicsGLRegisterImage` 映射为 OpenGL 纹理，单帧耗时极低（~0.13ms），**总线搬运量为 0**。

### 2.2 Tier 2: 平台原生多态零拷贝
针对无 NVIDIA 专有 CUDA 环境但具备现代显卡加速的平台：
- **Linux 平台 (`VaapiDmaBufPipeline`)**：
  - VA-API 解码输出 `VASurfaceID`；
  - 导出 Linux 内核标准的 **DMA-BUF (`dma-buf fd`)**；
  - 通过 `eglCreateImageKHR` 与 `glEGLImageTargetTexture2DOES` 直通绑定为 OpenGL 纹理。
- **macOS 平台 (`VideoToolboxPipeline`)**：
  - VideoToolbox 硬解输出 `CVPixelBufferRef`；
  - 提取底层硬件共享缓冲 `IOSurfaceRef`；
  - 通过 `CGLTexImageIOSurface2D` 原生零拷贝绑定为 `GL_TEXTURE_RECTANGLE` 纹理。

### 2.3 Tier 3: Olive 强化原生软解保底 (Safety Net)
针对老旧显卡、格式不支持或硬件管线发生异常时的绝对稳定保底：
- **全局线程配额调度 (`Tier3ThreadBudget`)**：
  - 动态侦测系统 CPU 逻辑核心数，**始终保底预留 2 核专供系统操作、UI 交互与实时音频渲染**，彻底解决多路视频并发时 CPU 100% 导致的音频卡顿；
  - 主播放视口保障分配 2~6 个线程，后台与缩略图任务受全局水位压制。
- **消除破坏性跳帧**：
  - 动态使用 `AVDISCARD_DEFAULT` 完整解码全部 B/P 帧，消除开放 GOP 切片报错。
- **音频预排队倍增**：
  - 起播预缓冲安全水位提升至 1.0 秒（4 个缓冲块），完全吸收复杂 4K 帧软解初始计算抖动。

---

## 3. 跨平台显示环境零拷贝充要条件矩阵

在复杂桌面环境下，零拷贝直通遵循核心物理定律：**「解码设备必须等于呈现设备」**。

| 操作系统 | 窗口合成器 | GPU 拓扑 | 推荐管线路由 | 零拷贝关键注意点 |
|---|---|---|:---:|---|
| **macOS** | Quartz / Metal | 纯核显 / 独立显卡 | **Tier 2 (VideoToolbox)** | `IOSurface` 原生支持跨 GPU 虚拟直通，零 CPU 回读。 |
| **Linux** | X11 / XWayland | 纯 NVIDIA 独显 | **Tier 1 (CUDA)** | 必须安装 NVIDIA 专有驱动；Mesa NVK/Zink 不支持 CUDA 互操作。 |
| **Linux** | 原生 Wayland | AMD / Intel 核显 | **Tier 2 (VA-API)** | EGLImage 直接挂载于同一 DRI 卡节点（`/dev/dri/renderD128`）。 |
| **Linux** | 混合双显卡 (PRIME) | 核显显示 + 独显计算 | **Tier 2 或 Tier 3** | 原生 Wayland 下独显解码到核显呈现存在 Cross-card 复制税，系统自动安全调度。 |
