#ifndef HARDWARE_PIPELINE_FACTORY_H
#define HARDWARE_PIPELINE_FACTORY_H

#include <functional>
#include <memory>
#include <QString>
#include <QSet>
#include "video_hardware_pipeline.h"

namespace olive {

class OpenGLRenderer;

struct StreamFallbackContext {
  QSet<QString> failed_backends;
};

class HardwarePipelineFactory {
public:
  /**
   * @brief Create and initialize the optimal hardware video pipeline according to the tiered routing policy:
   *        Tier 1 (NVIDIA CUDA-NVDEC) -> Tier 2 (Platform Native DMA-BUF/D3D11) -> Tier 3 (Reinforced CPU Decode + Native YUV Shader Upload).
   */
  static std::unique_ptr<IVideoHardwarePipeline> CreatePipeline(
      AVCodecContext* codec_ctx,
      int width,
      int height,
      OpenGLRenderer* renderer = nullptr,
      StreamFallbackContext* fallback_ctx = nullptr);

  /**
   * @brief Process-wide forced backend override (phase5-07). Empty string means
   *        dynamic auto routing. Set from -hal-backend / -hal-dialog / OLIVE_HAL_BACKEND.
   *        Valid values: "cuda", "videotoolbox", "vaapi", "d3d11", "software".
   */
  static void SetForcedBackend(const QString& name);
  static QString GetForcedBackend();

  /**
   * @brief Patch 0097: receive a visible, de-duplicated notice when a forced backend falls back.
   *
   * ProbeAndInitialize() runs on decoder threads. The handler may therefore be called from any
   * thread; the GUI installs one that queues the text onto the main window's status bar. Keeping
   * this out of the factory avoids a Qt widget dependency in the codec layer.
   */
  using FallbackHandler = std::function<void(const QString&)>;
  static void SetFallbackHandler(FallbackHandler handler);
};

} // namespace olive

#endif // HARDWARE_PIPELINE_FACTORY_H
