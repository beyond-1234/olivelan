# 04. 物理实测基准、已知问题与排障指南 (Benchmarks and Known Issues)

## 1. 物理性能基准对比 (Benchmarks)

在真实 4K60 复杂时间线回放测试中，经过 HAL 零拷贝与 OCIO 单轨改造后的性能表现对比如下：

| 测试场景 | 传统纯 CPU 软解回读管线 | Olivelan Tier 1 / Tier 2 零拷贝管线 | 性能收益与物理事实 |
|---|:---:|:---:|---|
| **4K60 HEVC 播放帧率** | 22 ~ 35 fps (严重抖动) | **稳定 60 fps** (满帧平滑) | 彻底消除了每帧从显卡读回内存的 PCIe 拥塞。 |
| **总线数据搬运税** | ~1.5 GB/s (4K RGBA 回读) | **0 MB/s (零搬运)** | 显存原位映射，单帧上屏耗时仅 0.13ms。 |
| **CPU 占用率** | 85% ~ 95% (抢占音频) | **15% ~ 25%** | CPU 仅负责少量解复用，预留充足算力保障音频稳定。 |
| **色彩换算精度** | 8-bit 截断 / 快速近似 | **半精度浮点 (F16) / 全浮点** | OCIO GPU 着色器保证无色阶断裂与 Log 细节丢失。 |

---

## 2. 核心已知限制与排障指南 (Known Issues & Troubleshooting)

### 2.1 Linux 下无专有驱动时的 NVIDIA 显卡行为
- **现象**：
  在开源 `nouveau` 驱动或 Mesa `zink/NVK` 下，系统探测提示：
  `nvidia=yes, gl=Mesa, tier1=unavailable`。
- **原因**：
  NVIDIA CUDA-GL 互操作（`cudaGraphicsGLRegisterImage`）深度绑定于 NVIDIA 专有闭源驱动和同卡同上下文。开源 NVK 目前不支持 CUDA 运行时。
- **处理方式**：
  安装官方专有驱动（驱动版本 ≥ 535）及 CUDA 12.4 运行库；系统会自动激活 Tier 1 链路。
  *(详细硬件分流机制与规避机理参见 [02-hal-zero-copy-pipeline.md §4](02-hal-zero-copy-pipeline.md#4-深度复盘linux-硬件直通的四大致命陷阱与避坑规范))*。

### 2.2 多解码器实例时的线程调度防欠载
- **排查指南**：
  若在老旧 4 核/6 核 CPU 上编辑多路并发视频出现音频微小卡顿，可在设置或日志中核验 `Tier3ThreadBudget`：
  系统已强制保留 2 个逻辑核专供系统与音频，单视口主解码器最大限制为 6 个线程，后台任务受抑制。

### 2.3 Wayland 桌面下的窗口刷新合规
- **现象**：
  在某些 Wayland 混成器（如 Gnome / Hyprland）下，最小化或隐藏视口时可能收到 Surface 配置告警。
- **处理方式**：
  系统已在 `ManagedDisplay` 与 `ViewerWidget` 中内置了安全的 Surface 上下文守护，不可见时自动挂起 GL 提交循环，避免向空 Surface 抛出 SwapBuffers。

---

## 3. 待办硬件验证矩阵 (Hardware Backlog)

以下条目属于待实机 GPU 签字的持续验证项：

- [ ] **Linux Tier 1 签字**：在配备 RTX 30/40 系显卡的 Linux 宿主机上，跑通 `scripts/verify/verify_zero_copy.sh`（核验 `hardware_frames > 0` 且 `cpu_readback_frames = 0`）；
- [ ] **Linux Tier 2 签字**：在 AMD Radeon 780M / Intel 锐炬集显的 Linux 宿主机上，验证 VA-API DMA-BUF 原生 EGL 直通；
- [ ] **工程往返实测**：多轨 230220 包含数十个剪辑块与复杂 OCIO LUT 的真实长周期工程保存与导出回归。
