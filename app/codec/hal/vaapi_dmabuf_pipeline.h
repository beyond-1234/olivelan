#ifndef VAAPI_DMABUF_PIPELINE_H
#define VAAPI_DMABUF_PIPELINE_H

#include "video_hardware_pipeline.h"

#include <vector>

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
  virtual AVPixelFormat GetPreferredHwPixelFormat() const override { return AV_PIX_FMT_VAAPI; }

  void SetRenderer(Renderer* renderer) override;

  /**
   * @brief Patch 0096: prefer the decoded frame's colour description over cached project data.
   *
   * VideoParams is serialized with the project, so a stream re-imported at a different bit depth
   * can still carry the old container's matrix/range/transfer tags. The decoded AVFrame carries
   * the authoritative per-stream values (and per-frame mastering-display side data on HDR streams).
   * This helper is pure so the merge rules can be tested without a GPU or VA-API device.
   */
  static VideoParams ResolveColorMetadata(const VideoParams& params, const AVFrame* frame);

private:
  OpenGLRenderer* renderer_{nullptr};
  AVBufferRef* hw_device_ctx_{nullptr};
  int max_width_{0};
  int max_height_{0};
  // Patch 0050: egl_image_ member removed - plane images are now per-frame locals that
  // are destroyed before ProcessHardwareFrame returns (see the composite path).

  // vaExportSurfaceHandle(DRM_PRIME_2) hands back fresh, *caller-owned* file descriptors on
  // every call. A P010/NV12 surface can export more than one object (one per plane), so we
  // must retain the whole set and close it before re-exporting, otherwise every plane beyond
  // object[0] leaks one fd per decoded frame.
  std::vector<int> exported_fds_;

  void ReleaseExportedFds();
};

} // namespace olive

#endif // VAAPI_DMABUF_PIPELINE_H
