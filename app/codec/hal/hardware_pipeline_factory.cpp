#include "hardware_pipeline_factory.h"
#include "gpu_environment_probe.h"
#include "codec/ffmpeg/hardware_quota.h"
#include "cuda_nvdec_pipeline.h"
#include "vaapi_dmabuf_pipeline.h"
#include "videotoolbox_pipeline.h"
#include "d3d11_interop_pipeline.h"
#include "reinforced_software_pipeline.h"
#include "render/opengl/openglrenderer.h"

#include <QDebug>
#include <QSet>

#include <mutex>
#include <utility>

namespace olive {

namespace {

// Patch 0091 (P2-2): both process-wide forced-backend fields live under one mutex. CreatePipeline()
// runs on decoder threads, so the previous bare function-static QStrings were a data race the
// moment two streams started together.
struct ForcedBackendState {
  std::mutex mutex;
  QString backend;
  // Failure keys are (backend, codec, pix_fmt, width x height), not just the backend: a backend
  // can fail for one stream (unsupported profile, VA-API quota at 4K) while working for another.
  // Caching per session made the first failure disable the backend for every later stream.
  QSet<QString> failed_keys;
  // Patch 0097: one notice per distinct reason. A forced backend can fail on every stream; a
  // status-bar message per frame would be noise, and a modal per frame would be unusable.
  QSet<QString> notified_fallbacks;
  HardwarePipelineFactory::FallbackHandler fallback_handler;
};

ForcedBackendState& State()
{
  static ForcedBackendState state;
  return state;
}

void NotifyFallback(const QString& message)
{
  HardwarePipelineFactory::FallbackHandler handler;
  {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.notified_fallbacks.contains(message)) {
      return;
    }
    state.notified_fallbacks.insert(message);
    handler = state.fallback_handler;
  }

  // Call outside the lock: a GUI handler may queue work that comes back through this factory.
  if (handler) {
    handler(message);
  }
}

QString ForcedFailureKey(const QString& backend, AVCodecContext* codec_ctx)
{
  return QStringLiteral("%1|%2|%3|%4x%5")
      .arg(backend)
      .arg(static_cast<int>(codec_ctx->codec_id))
      .arg(static_cast<int>(codec_ctx->pix_fmt))
      .arg(codec_ctx->width)
      .arg(codec_ctx->height);
}

std::unique_ptr<IVideoHardwarePipeline> CreateForcedPipeline(
    const QString& name, AVCodecContext* codec_ctx, int width, int height, OpenGLRenderer* renderer)
{
  if (name == QStringLiteral("cuda")) {
#if defined(OLIVE_ENABLE_CUDA_NVDEC)
    return std::make_unique<CudaNvdecPipeline>(renderer);
#else
    qWarning() << "[HAL Factory] Forced backend 'cuda' not compiled in (OLIVE_ENABLE_CUDA_NVDEC off); ignoring.";
    NotifyFallback(QStringLiteral("forced backend \"cuda\" is not compiled in; using automatic routing"));
#endif
  } else if (name == QStringLiteral("videotoolbox")) {
#if defined(__APPLE__)
    return std::make_unique<VideoToolboxPipeline>(renderer);
#else
    qWarning() << "[HAL Factory] Forced backend 'videotoolbox' only exists on macOS; ignoring.";
    NotifyFallback(QStringLiteral("forced backend \"videotoolbox\" is unavailable on this platform; using automatic routing"));
#endif
  } else if (name == QStringLiteral("vaapi")) {
#if defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
    return std::make_unique<VaapiDmaBufPipeline>(renderer);
#else
    qWarning() << "[HAL Factory] Forced backend 'vaapi' not compiled in (needs Linux + OLIVE_ENABLE_VAAPI_DMABUF); ignoring.";
    NotifyFallback(QStringLiteral("forced backend \"vaapi\" is not compiled in; using automatic routing"));
#endif
  } else if (name == QStringLiteral("d3d11")) {
    // Patch 0049 (Phase 8): the D3D11 interop pipeline is a placeholder and is not
    // created on ANY platform - Windows included. Windows falls back to Tier 3
    // (reinforced CPU decode). The pipeline class is kept as the starting point for
    // future development.
    qWarning() << "[HAL Factory] Forced backend 'd3d11' is a placeholder, not implemented; ignoring (Windows uses Tier 3 fallback).";
    NotifyFallback(QStringLiteral("forced backend \"d3d11\" is not implemented; using automatic routing"));
  } else if (name == QStringLiteral("software")) {
    return std::make_unique<ReinforcedSoftwarePipeline>(renderer);
  } else {
    qWarning() << "[HAL Factory] Unknown forced backend" << name << "; ignoring.";
    NotifyFallback(QStringLiteral("unknown forced backend \"%1\"; using automatic routing").arg(name));
  }
  return nullptr;
}

} // namespace

void HardwarePipelineFactory::SetForcedBackend(const QString& name)
{
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.backend = name.toLower().trimmed();
  // Explicit (re)selection re-arms the attempt even for a backend that failed earlier.
  state.failed_keys.clear();
}

QString HardwarePipelineFactory::GetForcedBackend()
{
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  return state.backend;
}

void HardwarePipelineFactory::SetFallbackHandler(FallbackHandler handler)
{
  auto& state = State();
  std::lock_guard<std::mutex> lock(state.mutex);
  state.fallback_handler = std::move(handler);
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
  QString forced;
  bool forced_already_failed = false;
  {
    auto& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    forced = state.backend;
    if (!forced.isEmpty() && forced != QStringLiteral("auto")) {
      forced_already_failed = state.failed_keys.contains(ForcedFailureKey(forced, codec_ctx)) ||
                              (fallback_ctx && fallback_ctx->failed_backends.contains(forced));
    }
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
        return forced_pipeline;
      }

      {
        auto& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        // Only record the failure if the user has not selected a different backend while this
        // initialization was running.
        if (state.backend == forced) {
          state.failed_keys.insert(ForcedFailureKey(forced, codec_ctx));
        }
      }

      if (fallback_ctx) {
        fallback_ctx->failed_backends.insert(forced);
      }

      qWarning() << "[HAL Factory] Forced backend" << forced
                 << "failed physical initialization for this codec/format; other streams will"
                 << "still try it. Falling back to auto routing for this stream.";
      NotifyFallback(QStringLiteral("forced backend \"%1\" failed to initialize; this stream uses automatic routing").arg(forced));
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
#endif
  // Patch 0049 (Phase 8): no Windows Tier 2 branch. The D3D11 interop pipeline is a
  // placeholder (see SupportsD3D11()), so Windows auto-routing falls straight through
  // to Tier 3 below.

  // 3. Tier 3: Olive Native Reinforced CPU Decode & Upload Pipeline (Zero-Dependency Ultimate Safety Net)
  // When HW pipelines are saturated or unsupported (e.g. over-budget),
  // routes to Olive's reinforced CPU decode pipeline protected by adaptive thread clamps and B-frame skipping.
  // HDR/BT.2020 tonemapping is handled by Olive's existing OCIO color pipeline, not a separate decode backend.
  qInfo() << "[HAL Factory] Routing to Tier 3: Reinforced CPU Decode + Native YUV Shader Upload (Zero-Dependency).";
  auto sw_pipeline = std::make_unique<ReinforcedSoftwarePipeline>(renderer);
  sw_pipeline->ProbeAndInitialize(codec_ctx, width, height);
  return sw_pipeline;
}

} // namespace olive
