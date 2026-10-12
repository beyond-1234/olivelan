#include "hardware_pipeline_factory.h"
#include "gpu_environment_probe.h"
#include "codec/ffmpeg/hardware_quota.h"
#include "cuda_nvdec_pipeline.h"
#include "vaapi_dmabuf_pipeline.h"
#include "d3d11_interop_pipeline.h"
#include "libplacebo_pipeline.h"

#include <QDebug>

namespace olive {

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

  // 0. Pre-filter: Prevent hardware allocation for codecs that perform better on CPU (ProRes, 4:2:2)
  if (!probe.ShouldUseHardwareDecode(codec_ctx->codec_id, codec_ctx->pix_fmt)) {
    qInfo() << "[HAL Factory] Codec" << avcodec_get_name(codec_ctx->codec_id)
            << "or format is routed directly to CPU decoding (bypassing HW quota).";
    return nullptr;
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
#if defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
  if (probe.SupportsVaapiDmaBuf()) {
    auto pipeline = std::make_unique<VaapiDmaBufPipeline>(renderer);
    if (pipeline->ProbeAndInitialize(codec_ctx, width, height)) {
      qInfo() << "[HAL Factory] Selected Tier 2:" << pipeline->GetBackendName();
      return pipeline;
    }
  }
#elif defined(_WIN32) && defined(OLIVE_ENABLE_D3D11_INTEROP)
  if (probe.SupportsD3D11()) {
    auto pipeline = std::make_unique<D3D11InteropPipeline>(renderer);
    if (pipeline->ProbeAndInitialize(codec_ctx, width, height)) {
      qInfo() << "[HAL Factory] Selected Tier 2:" << pipeline->GetBackendName();
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
  // When HW pipelines are saturated or unsupported (e.g. ProRes, 4:2:2, or over-budget),
  // routes to Olive's reinforced CPU decode pipeline protected by adaptive thread clamps and B-frame skipping.
  qInfo() << "[HAL Factory] Routing to Tier 3: Olive Native Reinforced Software Pipeline (Zero-Dependency).";
  return nullptr;
}

} // namespace olive
