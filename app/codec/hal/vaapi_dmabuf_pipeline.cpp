#include "vaapi_dmabuf_pipeline.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

#include <QDebug>
#include "render/opengl/openglrenderer.h"

#if defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
#include <libavutil/hwcontext_vaapi.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <unistd.h>
#endif

namespace olive {

VaapiDmaBufPipeline::VaapiDmaBufPipeline(OpenGLRenderer* renderer)
  : renderer_(renderer)
{
}

VaapiDmaBufPipeline::~VaapiDmaBufPipeline()
{
  Close();
}

bool VaapiDmaBufPipeline::ProbeAndInitialize(AVCodecContext* codec_ctx, int max_w, int max_h)
{
#if !defined(__linux__) || !defined(OLIVE_ENABLE_VAAPI_DMABUF)
  Q_UNUSED(codec_ctx);
  Q_UNUSED(max_w);
  Q_UNUSED(max_h);
  return false;
#else
  if (!codec_ctx) {
    return false;
  }

  max_width_ = max_w;
  max_height_ = max_h;

  int err = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
  if (err < 0 || !hw_device_ctx_) {
    qWarning() << "[VAAPI-HAL] Failed to create VAAPI device context, code:" << err;
    return false;
  }

  codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
  if (!codec_ctx->hw_device_ctx) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
    return false;
  }

  qInfo() << "[VAAPI-HAL] Initialized VA-API DMA-BUF hardware pipeline";
  return true;
#endif
}

bool VaapiDmaBufPipeline::ProcessHardwareFrame(AVFrame* hw_frame, HardwareDecodedFrame& out_frame)
{
#if !defined(__linux__) || !defined(OLIVE_ENABLE_VAAPI_DMABUF)
  Q_UNUSED(hw_frame);
  Q_UNUSED(out_frame);
  return false;
#else
  if (!hw_frame || hw_frame->format != AV_PIX_FMT_VAAPI) {
    return false;
  }

  VASurfaceID surface_id = reinterpret_cast<uintptr_t>(hw_frame->data[3]);
  VADRMPRIMESurfaceDescriptor prime_desc{};
  
  auto* va_device_ctx = reinterpret_cast<AVVAAPIDeviceContext*>(
      reinterpret_cast<AVHWDeviceContext*>(hw_device_ctx_->data)->hwctx);
  VADisplay va_display = va_device_ctx->display;

  VAStatus status = vaExportSurfaceHandle(va_display,
                                          surface_id,
                                          VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                          VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                          &prime_desc);
  if (status != VA_STATUS_SUCCESS) {
    qWarning() << "[VAAPI-HAL] vaExportSurfaceHandle failed:" << status;
    return false;
  }

  if (dma_buf_fd_ >= 0) {
    close(dma_buf_fd_);
  }
  dma_buf_fd_ = prime_desc.objects[0].fd;

  // Set normalized UV dimensions for non-standard stride/padding
  out_frame.gl_texture_id = resident_gl_texture_;
  out_frame.width = hw_frame->width;
  out_frame.height = hw_frame->height;
  out_frame.coded_width = prime_desc.width;
  out_frame.coded_height = prime_desc.height;
  out_frame.uv_scale_x = (prime_desc.width > 0) ? static_cast<float>(hw_frame->width) / prime_desc.width : 1.0f;
  out_frame.uv_scale_y = (prime_desc.height > 0) ? static_cast<float>(hw_frame->height) / prime_desc.height : 1.0f;
  out_frame.pts = hw_frame->pts;

  return true;
#endif
}

void VaapiDmaBufPipeline::ReleaseFrame(const HardwareDecodedFrame& frame)
{
  Q_UNUSED(frame);
#if defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
  if (dma_buf_fd_ >= 0) {
    close(dma_buf_fd_);
    dma_buf_fd_ = -1;
  }
#endif
}

void VaapiDmaBufPipeline::Close()
{
#if defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
  if (dma_buf_fd_ >= 0) {
    close(dma_buf_fd_);
    dma_buf_fd_ = -1;
  }
#endif
  if (hw_device_ctx_) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
  }
}

} // namespace olive
