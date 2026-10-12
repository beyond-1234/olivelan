#ifndef VAAPI_DMABUF_PIPELINE_H
#define VAAPI_DMABUF_PIPELINE_H

#include "video_hardware_pipeline.h"

namespace olive {

class OpenGLRenderer;

class VaapiDmaBufPipeline : public IVideoHardwarePipeline {
public:
  explicit VaapiDmaBufPipeline(OpenGLRenderer* renderer = nullptr);
  virtual ~VaapiDmaBufPipeline() override;

  virtual bool ProbeAndInitialize(AVCodecContext* codec_ctx, int max_w, int max_h) override;
  virtual bool ProcessHardwareFrame(AVFrame* hw_frame, const VideoParams& params, HardwareDecodedFrame& out_frame) override;
  virtual void Close() override;

  virtual HardwareBackendType GetType() const override { return HardwareBackendType::VaapiDmaBuf; }
  virtual const char* GetBackendName() const override { return "VA-API DMA-BUF (Tier 2 Linux)"; }
  virtual AVBufferRef* GetHwDeviceContext() const override { return hw_device_ctx_; }

  void SetRenderer(Renderer* renderer) override { (void)renderer; }

private:
  AVBufferRef* hw_device_ctx_{nullptr};
  int max_width_{0};
  int max_height_{0};
  uint32_t resident_gl_texture_{0};
  void* egl_image_{nullptr};
  int dma_buf_fd_{-1};
};

} // namespace olive

#endif // VAAPI_DMABUF_PIPELINE_H
