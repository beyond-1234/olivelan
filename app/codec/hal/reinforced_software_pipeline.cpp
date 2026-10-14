#include "reinforced_software_pipeline.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <QDebug>
#include <algorithm>
#include <thread>
#include "render/renderer.h"

namespace olive {

ReinforcedSoftwarePipeline::ReinforcedSoftwarePipeline(Renderer* renderer)
  : renderer_(renderer)
{
}

ReinforcedSoftwarePipeline::~ReinforcedSoftwarePipeline()
{
  Close();
}

bool ReinforcedSoftwarePipeline::ProbeAndInitialize(AVCodecContext* codec_ctx, int max_w, int max_h)
{
  if (!codec_ctx) {
    return false;
  }

  max_width_ = max_w;
  max_height_ = max_h;

  // Tier 3: Pure software decoding with adaptive thread clamping and B-frame skipping
  int fallback_threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency() / 4), 2, 6);
  codec_ctx->thread_count = fallback_threads;

  if (!is_primary_) {
    codec_ctx->skip_frame = AVDISCARD_NONREF;
  }

  qInfo() << "[Reinforced-SW HAL] Initialized Tier 3 software pipeline with" << fallback_threads << "worker threads";
  return true;
}

bool ReinforcedSoftwarePipeline::ProcessHardwareFrame(AVFrame* hw_frame, const VideoParams& params, HardwareDecodedFrame& out_frame)
{
  if (!hw_frame || !renderer_) {
    return false;
  }

  // If frame format is already compatible with direct single-plane upload (e.g. RGBA / BGRA)
  if (hw_frame->format == AV_PIX_FMT_RGBA || hw_frame->format == AV_PIX_FMT_BGRA) {
    TexturePtr tex = renderer_->CreateTexture(params);
    if (tex) {
      // UploadToTexture() consumes the row length in *pixels* (it maps to
      // GL_UNPACK_ROW_LENGTH), whereas AVFrame::linesize is in bytes.
      const int bytes_per_pixel = params.GetBytesPerPixel();
      tex->Upload(hw_frame->data[0], bytes_per_pixel > 0 ? hw_frame->linesize[0] / bytes_per_pixel : hw_frame->linesize[0]);
      out_frame.texture = tex;
      out_frame.pts = hw_frame->pts;
      out_frame.width = hw_frame->width;
      out_frame.height = hw_frame->height;
      return true;
    }
  }

  // For multi-plane YUV frames, return false so the decoder engages its specialized Yuv2RgbShader
  return false;
}

void ReinforcedSoftwarePipeline::Close()
{
}

} // namespace olive
