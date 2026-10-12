#include "d3d11_interop_pipeline.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

#include <QDebug>
#include "render/opengl/openglrenderer.h"

#if defined(_WIN32) && defined(OLIVE_ENABLE_D3D11_INTEROP)
#include <d3d11.h>
#include <wglext.h>
#endif

namespace olive {

D3D11InteropPipeline::D3D11InteropPipeline(OpenGLRenderer* renderer)
  : renderer_(renderer)
{
}

D3D11InteropPipeline::~D3D11InteropPipeline()
{
  Close();
}

bool D3D11InteropPipeline::ProbeAndInitialize(AVCodecContext* codec_ctx, int max_w, int max_h)
{
#if !defined(_WIN32) || !defined(OLIVE_ENABLE_D3D11_INTEROP)
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

  int err = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
  if (err < 0 || !hw_device_ctx_) {
    qWarning() << "[D3D11-HAL] Failed to create D3D11VA hw context, code:" << err;
    return false;
  }

  codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
  if (!codec_ctx->hw_device_ctx) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
    return false;
  }

  qInfo() << "[D3D11-HAL] Initialized Direct3D 11 interop hardware pipeline";
  return true;
#endif
}

bool D3D11InteropPipeline::ProcessHardwareFrame(AVFrame* hw_frame, HardwareDecodedFrame& out_frame)
{
#if !defined(_WIN32) || !defined(OLIVE_ENABLE_D3D11_INTEROP)
  Q_UNUSED(hw_frame);
  Q_UNUSED(out_frame);
  return false;
#else
  if (!hw_frame || hw_frame->format != AV_PIX_FMT_D3D11) {
    return false;
  }

  out_frame.gl_texture_id = gl_texture_id_;
  out_frame.width = hw_frame->width;
  out_frame.height = hw_frame->height;
  out_frame.coded_width = hw_frame->width;
  out_frame.coded_height = hw_frame->height;
  out_frame.uv_scale_x = 1.0f;
  out_frame.uv_scale_y = 1.0f;
  out_frame.pts = hw_frame->pts;

  return true;
#endif
}

void D3D11InteropPipeline::ReleaseFrame(const HardwareDecodedFrame& frame)
{
  Q_UNUSED(frame);
}

void D3D11InteropPipeline::Close()
{
  if (hw_device_ctx_) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
  }
}

} // namespace olive
