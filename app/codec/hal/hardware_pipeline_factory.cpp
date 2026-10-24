#include "hardware_pipeline_factory.h"
#include "gpu_environment_probe.h"
#include "codec/ffmpeg/hardware_quota.h"
#include "cuda_nvdec_pipeline.h"
#include "vaapi_dmabuf_pipeline.h"
#include "videotoolbox_pipeline.h"
#include "reinforced_software_pipeline.h"
#include "render/opengl/openglrenderer.h"

#include <QDebug>
#include <QSet>

#include <utility>

namespace olive {

namespace {

int TierForForcedBackend(const QString& backend)
{
  if (backend == QStringLiteral("cuda")) {
    return 1;
  }
  if (backend == QStringLiteral("videotoolbox") || backend == QStringLiteral("vaapi")) {
    return 2;
  }
  if (backend == QStringLiteral("software")) {
    return 3;
  }
  return 0;
}

std::unique_ptr<IVideoHardwarePipeline> CreateForcedPipeline(
    const QString& name, AVCodecContext* codec_ctx, int width, int height, OpenGLRenderer* renderer)
{
  if (name == QStringLiteral("cuda")) {
#if defined(OLIVE_ENABLE_CUDA_NVDEC)
    return std::make_unique<CudaNvdecPipeline>(renderer);
#else
    qWarning() << "[HAL Factory] Forced backend 'cuda' not compiled in (OLIVE_ENABLE_CUDA_NVDEC off); ignoring.";
    HalSessionState::Instance().NotifyFallback(QStringLiteral("forced backend \"cuda\" is not compiled in; using automatic routing"));
#endif
  } else if (name == QStringLiteral("videotoolbox")) {
#if defined(__APPLE__)
    return std::make_unique<VideoToolboxPipeline>(renderer);
#else
    qWarning() << "[HAL Factory] Forced backend 'videotoolbox' only exists on macOS; ignoring.";
    HalSessionState::Instance().NotifyFallback(QStringLiteral("forced backend \"videotoolbox\" is unavailable on this platform; using automatic routing"));
#endif
  } else if (name == QStringLiteral("vaapi")) {
#if defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
    return std::make_unique<VaapiDmaBufPipeline>(renderer);
#else
    qWarning() << "[HAL Factory] Forced backend 'vaapi' not compiled in (needs Linux + OLIVE_ENABLE_VAAPI_DMABUF); ignoring.";
    HalSessionState::Instance().NotifyFallback(QStringLiteral("forced backend \"vaapi\" is not compiled in; using automatic routing"));
#endif
  } else if (name == QStringLiteral("d3d11")) {
    qWarning() << "[HAL Factory] Forced backend 'd3d11' is abandoned (Windows distribution discontinued); using automatic routing.";
    HalSessionState::Instance().NotifyFallback(QStringLiteral("forced backend \"d3d11\" is abandoned; using automatic routing"));
  } else if (name == QStringLiteral("software")) {
    return std::make_unique<ReinforcedSoftwarePipeline>(renderer);
  } else {
    qWarning() << "[HAL Factory] Unknown forced backend" << name << "; ignoring.";
    HalSessionState::Instance().NotifyFallback(QStringLiteral("unknown forced backend \"%1\"; using automatic routing").arg(name));
  }
  return nullptr;
}

} // namespace

void HardwarePipelineFactory::SetForcedBackend(const QString& name)
{
  HalSessionState::Instance().SetForcedBackend(name);
}

QString HardwarePipelineFactory::GetForcedBackend()
{
  return HalSessionState::Instance().ForcedBackend();
}

int HardwarePipelineFactory::GetLastSelectedTier()
{
  return HalSessionState::Instance().LastSelectedTier();
}

QString HardwarePipelineFactory::GetLastSelectedBackend()
{
  return HalSessionState::Instance().LastSelectedBackend();
}

void HardwarePipelineFactory::SetFallbackHandler(FallbackHandler handler)
{
  HalSessionState::Instance().SetFallbackHandler(std::move(handler));
}

QString HardwarePipelineFactory::ForcedFailureKey(const QString& backend, AVCodecContext* codec_ctx)
{
  return QStringLiteral("%1|%2|%3|%4x%5")
      .arg(backend)
      .arg(static_cast<int>(codec_ctx->codec_id))
      .arg(static_cast<int>(codec_ctx->pix_fmt))
      .arg(codec_ctx->width)
      .arg(codec_ctx->height);
}

bool HardwarePipelineFactory::IsForcedBackendAlreadyFailed(
    const QString& backend,
    const QSet<QString>& global_failed_keys,
    const StreamFallbackContext* fallback_ctx,
    AVCodecContext* codec_ctx)
{
  return global_failed_keys.contains(ForcedFailureKey(backend, codec_ctx))
         || (fallback_ctx && fallback_ctx->failed_backends.contains(backend));
}

std::unique_ptr<IVideoHardwarePipeline> HardwarePipelineFactory::CreatePipeline(
    AVCodecContext* codec_ctx,
    int width,
    int height,
    OpenGLRenderer* renderer,
    StreamFallbackContext* fallback_ctx)
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
  auto& session = HalSessionState::Instance();
  const QString forced = session.ForcedBackend();
  bool forced_already_failed = false;
  if (!forced.isEmpty() && forced != QStringLiteral("auto")) {
    forced_already_failed = IsForcedBackendAlreadyFailed(forced, session.FailedKeys(),
                                                         fallback_ctx, codec_ctx);
  }

  if (!forced.isEmpty() && forced != QStringLiteral("auto") && !forced_already_failed) {
    if (forced == QStringLiteral("cuda") && probe.IsCudaGlInteropBlocked()) {
      qWarning() << "[HAL Factory] Forcing the CUDA backend, but the active OpenGL context is on"
                 << QString::fromStdString(probe.GetOpenGlVendor())
                 << "- CUDA-GL interop will fail (cudaErrorInvalidGraphicsContext) and the"
                 << "zero-copy path will not actually be active.";
    }

    auto forced_pipeline = CreateForcedPipeline(forced, codec_ctx, width, height, renderer);
    if (forced_pipeline) {
      if (forced_pipeline->ProbeAndInitialize(codec_ctx, width, height)) {
        qInfo() << "[HAL Factory] Forced backend selected:" << forced_pipeline->GetBackendName();
        HalSessionState::Instance().RecordSelection(TierForForcedBackend(forced), forced_pipeline->GetBackendName());
        return forced_pipeline;
      }

      session.RecordFailure(ForcedFailureKey(forced, codec_ctx), forced);

      if (fallback_ctx) {
        fallback_ctx->failed_backends.insert(forced);
      }

      qWarning() << "[HAL Factory] Forced backend" << forced
                 << "failed physical initialization for this codec/format; other streams will"
                 << "still try it. Falling back to auto routing for this stream.";
      HalSessionState::Instance().NotifyFallback(QStringLiteral("forced backend \"%1\" failed to initialize; this stream uses automatic routing").arg(forced));
    }
  }

  // 0. Pre-filter: Prevent hardware allocation for codecs that perform better on CPU (ProRes, 4:2:2)
  if (!probe.ShouldUseHardwareDecode(codec_ctx->codec_id, codec_ctx->pix_fmt)) {
    qInfo() << "[HAL Factory] Codec" << avcodec_get_name(codec_ctx->codec_id)
            << "or format is routed directly to Tier 3 CPU decoding (bypassing HW quota).";
    auto sw_pipeline = std::make_unique<ReinforcedSoftwarePipeline>(renderer);
    sw_pipeline->ProbeAndInitialize(codec_ctx, width, height);
    HalSessionState::Instance().RecordSelection(3, sw_pipeline->GetBackendName());
    return sw_pipeline;
  }

  // 1. Tier 1: Dedicated NVIDIA CUDA-NVDEC path
#if defined(OLIVE_ENABLE_CUDA_NVDEC)
  // Capability honesty guard: CUDA-GL interop requires the OpenGL context to live on the same
  // NVIDIA GPU as CUDA/NVDEC. On hybrid laptops the window context can land on the iGPU (native
  // Wayland composites on the iGPU), where cudaGraphicsGLRegisterImage fails with
  // cudaErrorInvalidGraphicsContext and Tier 1's zero-copy path silently does nothing.
  //
  // Only Tier 1 is skipped - deliberately NOT the whole tier chain: Tier 2 VA-API decodes on the
  // display GPU itself, so it stays a genuine same-GPU zero-copy path on exactly this topology,
  // and is the correct answer for native Wayland. Tier 3 remains the final fallback.
  if (probe.IsCudaGlInteropBlocked()) {
    qWarning() << "[HAL Factory] Hybrid GPU topology detected, but the active OpenGL context is on"
               << QString::fromStdString(probe.GetOpenGlVendor())
               << "- CUDA-GL interop (Tier 1 zero-copy) is unavailable on this session."
               << "Skipping Tier 1 and evaluating the remaining tiers.";
  } else if (probe.IsNvidiaGpu() && probe.HasNvdecSupport()) {
    auto pipeline = std::make_unique<CudaNvdecPipeline>(renderer);
    if (pipeline->ProbeAndInitialize(codec_ctx, width, height)) {
      qInfo() << "[HAL Factory] Selected Tier 1:" << pipeline->GetBackendName();
      HalSessionState::Instance().RecordSelection(1, pipeline->GetBackendName());
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
      HalSessionState::Instance().RecordSelection(2, pipeline->GetBackendName());
      return pipeline;
    }
  }
#elif defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
  if (probe.SupportsVaapiDmaBuf()) {
    auto pipeline = std::make_unique<VaapiDmaBufPipeline>(renderer);
    if (pipeline->ProbeAndInitialize(codec_ctx, width, height)) {
      qInfo() << "[HAL Factory] Selected Tier 2 Linux:" << pipeline->GetBackendName();
      HalSessionState::Instance().RecordSelection(2, pipeline->GetBackendName());
      return pipeline;
    }
  }
#endif
  // Windows platform distribution has been completely abandoned. Auto-routing
  // falls straight through to Tier 3 reinforced CPU decode.

  // 3. Tier 3: Olive Native Reinforced CPU Decode & Upload Pipeline (Zero-Dependency Ultimate Safety Net)
  // When HW pipelines are saturated or unsupported (e.g. over-budget),
  // routes to Olive's reinforced CPU decode pipeline protected by adaptive thread clamps and B-frame skipping.
  // HDR/BT.2020 tonemapping is handled by Olive's existing OCIO color pipeline, not a separate decode backend.
  qInfo() << "[HAL Factory] Routing to Tier 3: Reinforced CPU Decode + Native YUV Shader Upload (Zero-Dependency).";
  auto sw_pipeline = std::make_unique<ReinforcedSoftwarePipeline>(renderer);
  sw_pipeline->ProbeAndInitialize(codec_ctx, width, height);
  HalSessionState::Instance().RecordSelection(3, sw_pipeline->GetBackendName());
  return sw_pipeline;
}

} // namespace olive
