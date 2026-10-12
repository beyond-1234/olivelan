#ifndef LIBPLACEBO_PIPELINE_H
#define LIBPLACEBO_PIPELINE_H

#include "video_hardware_pipeline.h"

namespace olive {

class OpenGLRenderer;

class LibPlaceboPipeline : public IVideoHardwarePipeline {
public:
  explicit LibPlaceboPipeline(OpenGLRenderer* renderer = nullptr);
  virtual ~LibPlaceboPipeline() override;

  virtual bool ProbeAndInitialize(AVCodecContext* codec_ctx, int max_w, int max_h) override;
  virtual bool ProcessHardwareFrame(AVFrame* hw_frame, const VideoParams& params, HardwareDecodedFrame& out_frame) override;
  virtual void Close() override;

  virtual HardwareBackendType GetType() const override { return HardwareBackendType::LibPlaceboFallback; }
  virtual const char* GetBackendName() const override { return "libplacebo Plugin (Optional / Placeholder)"; }
  virtual AVBufferRef* GetHwDeviceContext() const override { return hw_device_ctx_; }

  void SetRenderer(Renderer* renderer) override { (void)renderer; }

private:
  AVBufferRef* hw_device_ctx_{nullptr};
  int max_width_{0};
  int max_height_{0};
  void* pl_context_{nullptr};
  void* pl_gpu_{nullptr};
  void* pl_renderer_{nullptr};
};

} // namespace olive

#endif // LIBPLACEBO_PIPELINE_H
