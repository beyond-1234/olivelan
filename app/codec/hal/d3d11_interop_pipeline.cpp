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
{
  Q_UNUSED(renderer);
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

bool D3D11InteropPipeline::ProcessHardwareFrame(AVFrame* hw_frame, const VideoParams& params, HardwareDecodedFrame& out_frame)
{
#if !defined(_WIN32) || !defined(OLIVE_ENABLE_D3D11_INTEROP)
  Q_UNUSED(hw_frame);
  Q_UNUSED(params);
  Q_UNUSED(out_frame);
  return false;
#else
  Q_UNUSED(hw_frame);
  Q_UNUSED(params);
  Q_UNUSED(out_frame);
  // Not yet materialized: this backend advertises itself for routing purposes, but its texture
  // transfer path still has to be written and validated against real D3D11 hardware. Reporting
  // failure lets the caller choose another path instead of handing back a texture it cannot own.
  static bool warned = false;
  if (!warned) {
    warned = true;
    qWarning() << "[D3D11-HAL] ProcessHardwareFrame not yet implemented; refusing to claim the frame";
  }
  return false;
#endif
}

void D3D11InteropPipeline::Close()
{
  if (hw_device_ctx_) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
  }
}

} // namespace olive
