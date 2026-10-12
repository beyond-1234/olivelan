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
  virtual bool ProcessHardwareFrame(AVFrame* hw_frame, const VideoParams& params, HardwareDecodedFrame& out_frame) override;
  virtual void Close() override;

  virtual HardwareBackendType GetType() const override { return HardwareBackendType::VideoToolboxInterop; }
  virtual const char* GetBackendName() const override { return "VideoToolbox IOSurface (Tier 2 macOS: AMD/Intel)"; }
  virtual AVBufferRef* GetHwDeviceContext() const override { return hw_device_ctx_; }
  virtual AVPixelFormat GetPreferredHwPixelFormat() const override { return AV_PIX_FMT_VIDEOTOOLBOX; }

  void SetRenderer(Renderer* renderer) override;

private:
  OpenGLRenderer* renderer_{nullptr};
  AVBufferRef* hw_device_ctx_{nullptr};
  int max_width_{0};
  int max_height_{0};
};

} // namespace olive

#endif // VIDEOTOOLBOX_PIPELINE_H
