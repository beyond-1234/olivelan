#include "videotoolbox_pipeline.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

#include <QDebug>
#include "render/opengl/openglrenderer.h"

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/CGLCurrent.h>
#include <OpenGL/gl.h>
#endif

namespace olive {

VideoToolboxPipeline::VideoToolboxPipeline(OpenGLRenderer* renderer)
  : renderer_(renderer)
{
}

VideoToolboxPipeline::~VideoToolboxPipeline()
{
  Close();
}

void VideoToolboxPipeline::SetRenderer(Renderer* renderer)
{
  renderer_ = dynamic_cast<OpenGLRenderer*>(renderer);
}

bool VideoToolboxPipeline::ProbeAndInitialize(AVCodecContext* codec_ctx, int max_w, int max_h)
{
#if !defined(__APPLE__)
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

  int err = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0);
  if (err < 0 || !hw_device_ctx_) {
    qWarning() << "[VideoToolbox-HAL] Failed to create VideoToolbox hardware device context, code:" << err;
    return false;
  }

  codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
  if (!codec_ctx->hw_device_ctx) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
    return false;
  }

  qInfo() << "[VideoToolbox-HAL] Initialized VideoToolbox + IOSurface zero-copy pipeline (macOS AMD/Intel)";
  return true;
#endif
}

bool VideoToolboxPipeline::ProcessHardwareFrame(AVFrame* hw_frame, const VideoParams& params, HardwareDecodedFrame& out_frame)
{
#if !defined(__APPLE__)
  Q_UNUSED(hw_frame);
  Q_UNUSED(params);
  Q_UNUSED(out_frame);
  return false;
#else
  if (!hw_frame || hw_frame->format != AV_PIX_FMT_VIDEOTOOLBOX) {
    return false;
  }

  CVPixelBufferRef pixel_buffer = reinterpret_cast<CVPixelBufferRef>(hw_frame->data[3]);
  if (!pixel_buffer) {
    return false;
  }

  IOSurfaceRef iosurface = CVPixelBufferGetIOSurface(pixel_buffer);
  if (!iosurface) {
    qWarning() << "[VideoToolbox-HAL] CVPixelBuffer does not contain an IOSurface backing";
    return false;
  }

  CGLContextObj cgl_ctx = CGLGetCurrentContext();
  if (!cgl_ctx) {
    qWarning() << "[VideoToolbox-HAL] No current CGL OpenGL context on thread";
    return false;
  }

  if (!renderer_) {
    qWarning() << "[VideoToolbox-HAL] Renderer context not attached; cannot transfer frame";
    return false;
  }

  TexturePtr tex = renderer_->CreateTextureFromIOSurface(params, iosurface, hw_frame->width, hw_frame->height);
  if (!tex) {
    qWarning() << "[VideoToolbox-HAL] Failed to create owning texture from IOSurface pool";
    return false;
  }

  // CR-14: CGLTexImageIOSurface2D is physically limited to 8-bit RGBA. If the source carries
  // >8-bit / HDR (BT.2020) metadata, it is collapsed here. Log once so the capability-matrix
  // admission (CR-05) does not falsely imply 10-bit delivery on macOS.
  if (hw_frame->color_range == AVCOL_RANGE_UNSPECIFIED && hw_frame->colorspace == AVCOL_SPC_BT2020_NCL) {
    static bool noted = false;
    if (!noted) {
      noted = true;
      qInfo() << "[VideoToolbox-HAL] BT.2020/HDR source present; macOS IOSurface path delivers 8-bit RGBA (downsampled)";
    }
  }

  out_frame.texture = tex;
  out_frame.pts = hw_frame->pts;
  out_frame.width = hw_frame->width;
  out_frame.height = hw_frame->height;
  return true;
#endif
}

void VideoToolboxPipeline::Close()
{
  if (hw_device_ctx_) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
  }
}

} // namespace olive
