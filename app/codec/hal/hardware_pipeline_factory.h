#ifndef HARDWARE_PIPELINE_FACTORY_H
#define HARDWARE_PIPELINE_FACTORY_H

#include <memory>
#include "video_hardware_pipeline.h"

namespace olive {

class OpenGLRenderer;

class HardwarePipelineFactory {
public:
  /**
   * @brief Create and initialize the optimal hardware video pipeline according to the tiered routing policy:
   *        Tier 1 (NVIDIA CUDA-NVDEC) -> Tier 2 (Platform Native DMA-BUF/D3D11) -> Tier 3 (libplacebo fallback) -> CPU.
   */
  static std::unique_ptr<IVideoHardwarePipeline> CreatePipeline(
      AVCodecContext* codec_ctx,
      int width,
      int height,
      OpenGLRenderer* renderer = nullptr);
};

} // namespace olive

#endif // HARDWARE_PIPELINE_FACTORY_H
