#include "hardware_pipeline_factory.h"
#include "gpu_environment_probe.h"
#include "codec/ffmpeg/hardware_quota.h"
#include "cuda_nvdec_pipeline.h"
#include "vaapi_dmabuf_pipeline.h"
#include "videotoolbox_pipeline.h"
#include "d3d11_interop_pipeline.h"
#include "libplacebo_pipeline.h"
#include "reinforced_software_pipeline.h"
#include "render/opengl/openglrenderer.h"

#include <QDebug>

namespace olive {

namespace {

// Process-wide forced backend (phase5-07). Empty = dynamic auto routing.
QString& ForcedBackendRef()
{
  static QString forced_backend;
  return forced_backend;
}

std::unique_ptr<IVideoHardwarePipeline> CreateForcedPipeline(
    const QString& name, AVCodecContext* codec_ctx, int width, int height, OpenGLRenderer* renderer)
{
  if (name == QStringLiteral("cuda")) {
#if defined(OLIVE_ENABLE_CUDA_NVDEC)
    return std::make_unique<CudaNvdecPipeline>(renderer);
#else
    qWarning() << "[HAL Factory] Forced backend 'cuda' not compiled in (OLIVE_ENABLE_CUDA_NVDEC off); ignoring.";
#endif
  } else if (name == QStringLiteral("videotoolbox")) {
#if defined(__APPLE__)
    return std::make_unique<VideoToolboxPipeline>(renderer);
#else
    qWarning() << "[HAL Factory] Forced backend 'videotoolbox' only exists on macOS; ignoring.";
#endif
  } else if (name == QStringLiteral("vaapi")) {
#if defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
    return std::make_unique<VaapiDmaBufPipeline>(renderer);
#else
    qWarning() << "[HAL Factory] Forced backend 'vaapi' not compiled in (needs Linux + OLIVE_ENABLE_VAAPI_DMABUF); ignoring.";
#endif
  } else if (name == QStringLiteral("d3d11")) {
#if defined(_WIN32) && defined(OLIVE_ENABLE_D3D11_INTEROP)
    return std::make_unique<D3D11InteropPipeline>(renderer);
#else
    qWarning() << "[HAL Factory] Forced backend 'd3d11' not compiled in (needs Windows + OLIVE_ENABLE_D3D11_INTEROP); ignoring.";
#endif
  } else if (name == QStringLiteral("software")) {
    return std::make_unique<ReinforcedSoftwarePipeline>(renderer);
  }
  return nullptr;
}

} // namespace

void HardwarePipelineFactory::SetForcedBackend(const QString& name)
{
  ForcedBackendRef() = name.toLower().trimmed();
}

const QString& HardwarePipelineFactory::GetForcedBackend()
{
  return ForcedBackendRef();
}

std::unique_ptr<IVideoHardwarePipeline> HardwarePipelineFactory::CreatePipeline(
    AVCodecContext* codec_ctx,
    int width,
    int height,
    OpenGLRenderer* renderer)
{
  if (!codec_ctx) {
    return nullptr;
  }

  // Ensure hardware quota profile is probed and initialized from GPU driver
  HardwareQuotaManager::AutoDetectHardwareTopology();

  auto& probe = GpuEnvironmentProbe::Instance();

  // 0. Forced backend override (phase5-07): user explicitly requests a backend via
  //    -hal-backend / -hal-dialog / OLIVE_HAL_BACKEND. Deliberately bypasses the
  //    ShouldUseHardwareDecode pre-filter ("user knows best" semantics), but a failed
  //    physical ProbeAndInitialize still falls back to the normal tiered routing.
  const QString& forced = GetForcedBackend();
  if (!forced.isEmpty() && forced != QStringLiteral("auto")) {
    auto forced_pipeline = CreateForcedPipeline(forced, codec_ctx, width, height, renderer);
    if (forced_pipeline) {
      if (forced_pipeline->ProbeAndInitialize(codec_ctx, width, height)) {
        qInfo() << "[HAL Factory] Forced backend selected:" << forced_pipeline->GetBackendName();
        return forced_pipeline;
      }
      qWarning() << "[HAL Factory] Forced backend" << forced
                << "failed physical initialization; falling back to auto routing.";
    }
  }

  // 0. Pre-filter: Prevent hardware allocation for codecs that perform better on CPU (ProRes, 4:2:2)
  if (!probe.ShouldUseHardwareDecode(codec_ctx->codec_id, codec_ctx->pix_fmt)) {
    qInfo() << "[HAL Factory] Codec" << avcodec_get_name(codec_ctx->codec_id)
            << "or format is routed directly to Tier 3 CPU decoding (bypassing HW quota).";
    auto sw_pipeline = std::make_unique<ReinforcedSoftwarePipeline>(renderer);
    sw_pipeline->ProbeAndInitialize(codec_ctx, width, height);
    return sw_pipeline;
  }

  // 1. Tier 1: Dedicated NVIDIA CUDA-NVDEC path
#if defined(OLIVE_ENABLE_CUDA_NVDEC)
  if (probe.IsNvidiaGpu() && probe.HasNvdecSupport()) {
    auto pipeline = std::make_unique<CudaNvdecPipeline>(renderer);
    if (pipeline->ProbeAndInitialize(codec_ctx, width, height)) {
      qInfo() << "[HAL Factory] Selected Tier 1:" << pipeline->GetBackendName();
      return pipeline;
    }
    qWarning() << "[HAL Factory] Tier 1 CudaNvdec initialization failed, evaluating fallback...";
  }
#endif

  // 2. Tier 2: Platform Native lightweight poly-morphic bypass
#if defined(__APPLE__)
  if (probe.SupportsVideoToolbox()) {
    auto pipeline = std::make_unique<VideoToolboxPipeline>(renderer);
    if (pipeline->ProbeAndInitialize(codec_ctx, width, height)) {
      qInfo() << "[HAL Factory] Selected Tier 2 macOS:" << pipeline->GetBackendName();
      return pipeline;
    }
  }
#elif defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
  if (probe.SupportsVaapiDmaBuf()) {
    auto pipeline = std::make_unique<VaapiDmaBufPipeline>(renderer);
    if (pipeline->ProbeAndInitialize(codec_ctx, width, height)) {
      qInfo() << "[HAL Factory] Selected Tier 2 Linux:" << pipeline->GetBackendName();
      return pipeline;
    }
  }
#elif defined(_WIN32) && defined(OLIVE_ENABLE_D3D11_INTEROP)
  if (probe.SupportsD3D11()) {
    auto pipeline = std::make_unique<D3D11InteropPipeline>(renderer);
    if (pipeline->ProbeAndInitialize(codec_ctx, width, height)) {
      qInfo() << "[HAL Factory] Selected Tier 2 Windows:" << pipeline->GetBackendName();
      return pipeline;
    }
  }
#endif

  // 3. Optional Plugin Placeholder (libplacebo for specialized external HDR tonemapping)
#if defined(OLIVE_ENABLE_LIBPLACEBO)
  if (probe.SupportsLibPlacebo() && probe.NeedsToneMapping(codec_ctx->colorspace, codec_ctx->color_trc)) {
    auto placebo = std::make_unique<LibPlaceboPipeline>(renderer);
    if (placebo->ProbeAndInitialize(codec_ctx, width, height)) {
      qInfo() << "[HAL Factory] Selected Plugin:" << placebo->GetBackendName();
      return placebo;
    }
  }
#endif

  // 4. Tier 3: Olive Native Reinforced CPU Decode & Upload Pipeline (Zero-Dependency Ultimate Safety Net)
  // When HW pipelines are saturated or unsupported (e.g. over-budget),
  // routes to Olive's reinforced CPU decode pipeline protected by adaptive thread clamps and B-frame skipping.
  qInfo() << "[HAL Factory] Routing to Tier 3: Olive Native Reinforced Software Pipeline (Zero-Dependency).";
  auto sw_pipeline = std::make_unique<ReinforcedSoftwarePipeline>(renderer);
  sw_pipeline->ProbeAndInitialize(codec_ctx, width, height);
  return sw_pipeline;
}

} // namespace olive
