#ifndef CUDA_NVDEC_PIPELINE_H
#define CUDA_NVDEC_PIPELINE_H

#include "video_hardware_pipeline.h"
#include <QPointer>

namespace olive {

class OpenGLRenderer;

class CudaNvdecPipeline : public IVideoHardwarePipeline {
public:
  explicit CudaNvdecPipeline(OpenGLRenderer* renderer = nullptr);
  virtual ~CudaNvdecPipeline() override;

  virtual bool ProbeAndInitialize(AVCodecContext* codec_ctx, int max_w, int max_h) override;
  virtual bool ProcessHardwareFrame(AVFrame* hw_frame, HardwareDecodedFrame& out_frame) override;
  virtual void Close() override;

  virtual HardwareBackendType GetType() const override { return HardwareBackendType::CudaNvdec; }
  virtual const char* GetBackendName() const override { return "CUDA-NVDEC (Tier 1)"; }
  virtual AVBufferRef* GetHwDeviceContext() const override { return hw_device_ctx_; }
  virtual void UpdatePriority(bool is_primary) override;

  int quota_slot() const { return quota_slot_; }

  void SetRenderer(OpenGLRenderer* renderer) override { renderer_ = renderer; }

private:
  OpenGLRenderer* renderer_{nullptr};
  AVBufferRef* hw_device_ctx_{nullptr};
  int quota_slot_{-1};
  int max_width_{4096};
  int max_height_{4096};
  int frame_count_{0};
};

} // namespace olive

#endif // CUDA_NVDEC_PIPELINE_H
