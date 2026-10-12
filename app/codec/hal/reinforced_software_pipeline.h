#ifndef REINFORCED_SOFTWARE_PIPELINE_H
#define REINFORCED_SOFTWARE_PIPELINE_H

#include "video_hardware_pipeline.h"

namespace olive {

class ReinforcedSoftwarePipeline : public IVideoHardwarePipeline {
public:
  explicit ReinforcedSoftwarePipeline(Renderer* renderer = nullptr);
  virtual ~ReinforcedSoftwarePipeline() override;

  virtual bool ProbeAndInitialize(AVCodecContext* codec_ctx, int max_w, int max_h) override;
  virtual bool ProcessHardwareFrame(AVFrame* hw_frame, const VideoParams& params, HardwareDecodedFrame& out_frame) override;
  virtual void Close() override;

  virtual HardwareBackendType GetType() const override { return HardwareBackendType::SoftwareFallback; }
  virtual const char* GetBackendName() const override { return "Reinforced Software (Tier 3 Native)"; }
  virtual AVBufferRef* GetHwDeviceContext() const override { return nullptr; }

  virtual void SetRenderer(Renderer* renderer) override { renderer_ = renderer; }
  virtual void UpdatePriority(bool is_primary) override { is_primary_ = is_primary; }

private:
  Renderer* renderer_{nullptr};
  int max_width_{0};
  int max_height_{0};
  bool is_primary_{true};
};

} // namespace olive

#endif // REINFORCED_SOFTWARE_PIPELINE_H
