# 03. OCIO 2.5 现代单轨色彩管线规范 (Color Pipeline OCIO)

## 1. 纯单轨哲学：废除双轨分支

在 NLE 和调色系统中，**颜色管线不一致比程序崩溃更为致命**，因为它会静默产出错误的画幅与成片。

OliveLan 坚决废除历史编译开关 `OLIVE_ENABLE_OCIO` 与内置的简化着色器，收敛为以 **OpenColorIO 2.5.1 为唯一、强制随包分发的单轨色彩引擎**。

```text
素材输入 (SDR / HDR10 / HLG / Camera Log / ACES)
                   │
                   ▼
  OCIO 动态着色器生成 (ColorManager / OpenGLRenderGraph)
  - 18% 场景灰度对齐与线性化
  - 3D LUT (OCIOFileTransformNode) 节点计算
  - 用户选定调色空间 (Primary Grading / ChromaKey)
                   │
                   ▼
  浮点线性合成工作空间 (Scene Linear / Half F16 / Float F32)
                   │
                   ▼
  OCIO 视口显示转换 (Managed Display View)
  - sRGB / Display P3 / Rec.709 变换
  - HDR10 PQ / HLG 动态 Sticky 映射
                   │
                   ▼
              监视器呈现 (ViewerWidget)
```

---

## 2. 核心色彩能力与空间支持

### 2.1 高动态范围 (HDR) 动态感知与 Sticky 视图
- **元数据感知**：解码时提取 `Mastering Display Color Volume`、`Content Light Level (MaxCLL/MaxFALL)` 及色彩传输曲线（SMPTE ST 2084 / ARIB STD-B67）；
- **动态 Sticky HDR**：
  - 工程根据时间线是否包含 HDR 素材自动切换显示映射（如 `HDR to SDR` 曲线或原生 HDR 视口）；
  - 提供 `Auto HDR (Follow Project)` 显式复位机制，允许用户随时在手动锁死与跟随工程之间自由切换。

### 2.2 工业级摄影机 Log 曲线支持
在内置 `config.ocio` 中固化了官方参数化变换，通过 `CameraLogCurvesToSceneLinear` 验证 18% 灰卡严谨线性化：
- **ARRI**：`ARRI LogC3 (EI 800 / EI 400)`；
- **Sony**：`Sony S-Gamut3.Cine / S-Log3` 与 `Sony S-Gamut3 / S-Log3`；
- **Panasonic**：`Panasonic V-Log / V-Gamut`。

### 2.3 ACES 1.3 核心工作流
- 内置 `ACEScg (AP1)` 与 `ACES2065-1 (AP0)` 核心色彩空间及其转换矩阵；
- 为素材输入提供原生宽色域工作流保障，并支持加载外部 Studio ACES 复杂配置。

### 2.4 用户 LUT 节点 (`OCIOFileTransformNode`)
- 支持导入 `.cube`、`.spi1d`、`.spi3d` 用户 1D/3D LUT 资产；
- 通过 OpenColorIO 的 `FileTransform` 统一生成 GPU 纹理与片元着色器代码，彻底取代历史遗留的自写解析器；
- 具备严格的坏表保护：非法或损坏的 LUT 文件会在节点与属性面板高亮红字告警，并自动降级为安全直通（Passthrough），绝不引起崩溃。

---

## 3. GPU 资源生命周期与 Shader 缓存

- **LRU 色彩着色器缓存**：
  针对动态调整的调色参数，生成的 GLSL Shader 建立基于最近最少使用（LRU）的显存缓存；
- **安全解耦**：
  切换色彩空间或重载外部配置时，触发安全的纹理对象与 Program 销毁，杜绝显存泄漏。
