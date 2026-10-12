#ifndef D3D11_INTEROP_PIPELINE_H
#define D3D11_INTEROP_PIPELINE_H

#include "video_hardware_pipeline.h"

namespace olive {

class OpenGLRenderer;

class D3D11InteropPipeline : public IVideoHardwarePipeline {
public:
  explicit D3D11InteropPipeline(OpenGLRenderer* renderer = nullptr);
  virtual ~D3D11InteropPipeline() override;

  virtual bool ProbeAndInitialize(AVCodecContext* codec_ctx, int max_w, int max_h) override;
  virtual bool ProcessHardwareFrame(AVFrame* hw_frame, HardwareDecodedFrame& out_frame) override;
  virtual void ReleaseFrame(const HardwareDecodedFrame& frame) override;
  virtual void Close() override;

  virtual HardwareBackendType GetType() const override { return HardwareBackendType::D3D11Interop; }
  virtual const char* GetBackendName() const override { return "D3D11 Shared Texture (Tier 2 Windows)"; }
  virtual AVBufferRef* GetHwDeviceContext() const override { return hw_device_ctx_; }

  void SetRenderer(OpenGLRenderer* renderer) { renderer_ = renderer; }

private:
  OpenGLRenderer* renderer_{nullptr};
  AVBufferRef* hw_device_ctx_{nullptr};
  int max_width_{0};
  int max_height_{0};
  void* shared_handle_{nullptr};
  void* wgl_device_handle_{nullptr};
  void* wgl_dx_object_{nullptr};
  uint32_t gl_texture_id_{0};
};

} // namespace olive

#endif // D3D11_INTEROP_PIPELINE_H
