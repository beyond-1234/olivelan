#ifndef HARDWARE_PIPELINE_FACTORY_H
#define HARDWARE_PIPELINE_FACTORY_H

#include <memory>
#include <QString>
#include "video_hardware_pipeline.h"

namespace olive {

class OpenGLRenderer;

class HardwarePipelineFactory {
public:
  /**
   * @brief Create and initialize the optimal hardware video pipeline according to the tiered routing policy:
   *        Tier 1 (NVIDIA CUDA-NVDEC) -> Tier 2 (Platform Native DMA-BUF/D3D11) -> Tier 3 (Reinforced Software).
   */
  static std::unique_ptr<IVideoHardwarePipeline> CreatePipeline(
      AVCodecContext* codec_ctx,
      int width,
      int height,
      OpenGLRenderer* renderer = nullptr);

  /**
   * @brief Process-wide forced backend override (phase5-07). Empty string means
   *        dynamic auto routing. Set from -hal-backend / -hal-dialog / OLIVE_HAL_BACKEND.
   *        Valid values: "cuda", "videotoolbox", "vaapi", "d3d11", "software".
   */
  static void SetForcedBackend(const QString& name);
  static const QString& GetForcedBackend();
};

} // namespace olive

#endif // HARDWARE_PIPELINE_FACTORY_H
