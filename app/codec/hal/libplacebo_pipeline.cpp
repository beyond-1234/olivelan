#include "libplacebo_pipeline.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

#include <QDebug>
#include "render/opengl/openglrenderer.h"

#if defined(OLIVE_ENABLE_LIBPLACEBO)
#include <libplacebo/vulkan.h>
#include <libplacebo/renderer.h>
#include <libplacebo/utils/upload.h>
#endif

namespace olive {

LibPlaceboPipeline::LibPlaceboPipeline(OpenGLRenderer* renderer)
{
  Q_UNUSED(renderer);
}

LibPlaceboPipeline::~LibPlaceboPipeline()
{
  Close();
}

bool LibPlaceboPipeline::ProbeAndInitialize(AVCodecContext* codec_ctx, int max_w, int max_h)
{
#if !defined(OLIVE_ENABLE_LIBPLACEBO)
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

  qInfo() << "[LibPlacebo-HAL] Initializing Tier 3 fallback pipeline";
  // libplacebo setup logic
  return true;
#endif
}

bool LibPlaceboPipeline::ProcessHardwareFrame(AVFrame* hw_frame, const VideoParams& params, HardwareDecodedFrame& out_frame)
{
#if !defined(OLIVE_ENABLE_LIBPLACEBO)
  Q_UNUSED(hw_frame);
  Q_UNUSED(params);
  Q_UNUSED(out_frame);
  return false;
#else
  Q_UNUSED(hw_frame);
  Q_UNUSED(params);
  Q_UNUSED(out_frame);
  // Placeholder plugin: routing slot reserved, render path not yet materialized.
  return false;
#endif
}

void LibPlaceboPipeline::Close()
{
  if (hw_device_ctx_) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
  }
}

} // namespace olive
