#ifndef VIDEOTOOLBOX_PIPELINE_H
#define VIDEOTOOLBOX_PIPELINE_H

#include "video_hardware_pipeline.h"

namespace olive {

class OpenGLRenderer;

class VideoToolboxPipeline : public IVideoHardwarePipeline {
public:
  explicit VideoToolboxPipeline(OpenGLRenderer* renderer = nullptr);
  virtual ~VideoToolboxPipeline() override;

  virtual bool ProbeAndInitialize(AVCodecContext* codec_ctx, int max_w, int max_h) override;
  virtual bool ProcessHardwareFrame(AVFrame* hw_frame, HardwareDecodedFrame& out_frame) override;
  virtual void Close() override;

  virtual HardwareBackendType GetType() const override { return HardwareBackendType::VideoToolboxInterop; }
  virtual const char* GetBackendName() const override { return "VideoToolbox IOSurface (Tier 2 macOS: AMD/Intel)"; }
  virtual AVBufferRef* GetHwDeviceContext() const override { return hw_device_ctx_; }

  void SetRenderer(OpenGLRenderer* renderer) override { renderer_ = renderer; }

private:
  OpenGLRenderer* renderer_{nullptr};
  AVBufferRef* hw_device_ctx_{nullptr};
  int max_width_{0};
  int max_height_{0};
  uint32_t resident_gl_texture_{0};
  void* active_iosurface_{nullptr};
};

} // namespace olive

#endif // VIDEOTOOLBOX_PIPELINE_H
