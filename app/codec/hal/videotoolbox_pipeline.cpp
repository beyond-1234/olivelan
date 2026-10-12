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

bool VideoToolboxPipeline::ProcessHardwareFrame(AVFrame* hw_frame, HardwareDecodedFrame& out_frame)
{
#if !defined(__APPLE__)
  Q_UNUSED(hw_frame);
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

  if (!resident_gl_texture_) {
    glGenTextures(1, &resident_gl_texture_);
  }

  // Bind to GL_TEXTURE_RECTANGLE for native macOS IOSurface zero-copy mapping
  glBindTexture(GL_TEXTURE_RECTANGLE, resident_gl_texture_);
  glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  CGLError cgl_err = CGLTexImageIOSurface2D(cgl_ctx,
                                           GL_TEXTURE_RECTANGLE,
                                           GL_RGBA,
                                           hw_frame->width,
                                           hw_frame->height,
                                           GL_BGRA,
                                           GL_UNSIGNED_INT_8_8_8_8_REV,
                                           iosurface,
                                           0);
  if (cgl_err != kCGLNoError) {
    qWarning() << "[VideoToolbox-HAL] CGLTexImageIOSurface2D failed with error:" << cgl_err;
    return false;
  }

  active_iosurface_ = iosurface;

  out_frame.gl_texture_id = resident_gl_texture_;
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

void VideoToolboxPipeline::ReleaseFrame(const HardwareDecodedFrame& frame)
{
  Q_UNUSED(frame);
  active_iosurface_ = nullptr;
}

void VideoToolboxPipeline::Close()
{
#if defined(__APPLE__)
  if (resident_gl_texture_) {
    glDeleteTextures(1, &resident_gl_texture_);
    resident_gl_texture_ = 0;
  }
#endif
  active_iosurface_ = nullptr;
  if (hw_device_ctx_) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
  }
}

} // namespace olive
