#include "reinforced_software_pipeline.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <QDebug>
#include <algorithm>
#include <mutex>
#include <thread>
#include "render/renderer.h"

namespace olive {

namespace {

std::mutex g_tier3_budget_mutex;
int g_tier3_allocated_threads = 0;

} // namespace

int Tier3ThreadBudget::Acquire(int width, int height, bool is_primary)
{
  const int total_cores = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));
  // Always reserve at least 2 cores for OS, audio callback and OpenGL upload
  const int reserved_cores = (total_cores >= 4) ? 2 : 1;
  const int max_global_budget = std::max(1, total_cores - reserved_cores);

  std::lock_guard<std::mutex> lock(g_tier3_budget_mutex);

  int desired = 1;
  if (is_primary) {
    if (width >= 3840 || height >= 2160) {
      desired = std::clamp(total_cores / 4, 2, 6);
    } else if (width >= 1920 || height >= 1080) {
      desired = std::clamp(total_cores / 6, 2, 4);
    } else {
      desired = 2;
    }
  } else {
    // Secondary, thumbnail and auxiliary streams get clamped tightly
    desired = (width >= 1920 || height >= 1080) ? 2 : 1;
  }

  const int remaining = max_global_budget - g_tier3_allocated_threads;
  if (is_primary) {
    // Primary viewport is guaranteed at least 2 threads even under severe pressure
    desired = std::max(2, std::min(desired, std::max(2, remaining)));
  } else {
    if (remaining <= 2) {
      desired = 1;
    } else {
      desired = std::min(desired, remaining);
    }
  }
  desired = std::max(1, desired);

  g_tier3_allocated_threads += desired;
  return desired;
}

void Tier3ThreadBudget::Release(int threads)
{
  if (threads <= 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_tier3_budget_mutex);
  g_tier3_allocated_threads = std::max(0, g_tier3_allocated_threads - threads);
}

int Tier3ThreadBudget::CurrentAllocated()
{
  std::lock_guard<std::mutex> lock(g_tier3_budget_mutex);
  return g_tier3_allocated_threads;
}

void Tier3ThreadBudget::ResetForTests()
{
  std::lock_guard<std::mutex> lock(g_tier3_budget_mutex);
  g_tier3_allocated_threads = 0;
}

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

  codec_ctx_ = codec_ctx;
  max_width_ = max_w;
  max_height_ = max_h;

  allocated_threads_ = Tier3ThreadBudget::Acquire(max_w, max_h, is_primary_);
  codec_ctx_->thread_count = allocated_threads_;

  // Primary playback must decode all B/P frames cleanly.
  // Secondary background streams may drop non-reference frames to preserve CPU.
  codec_ctx_->skip_frame = is_primary_ ? AVDISCARD_DEFAULT : AVDISCARD_NONREF;

  qInfo() << "[Reinforced-SW HAL] Initialized Tier 3 software pipeline with"
          << allocated_threads_ << "worker threads (global allocated:"
          << Tier3ThreadBudget::CurrentAllocated() << "threads, is_primary:" << is_primary_ << ")";

  static bool noted = false;
  if (!noted) {
    noted = true;
    qDebug() << "[Reinforced-SW HAL] Division of labour: reinforced CPU decode (thread budget + adaptive skip) + native YUV shader upload (Yuv2RgbShader)";
  }
  return true;
}

void ReinforcedSoftwarePipeline::UpdatePriority(bool is_primary)
{
  is_primary_ = is_primary;
  if (codec_ctx_) {
    codec_ctx_->skip_frame = is_primary_ ? AVDISCARD_DEFAULT : AVDISCARD_NONREF;
  }
}

bool ReinforcedSoftwarePipeline::ProcessHardwareFrame(AVFrame* hw_frame, const VideoParams& params, HardwareDecodedFrame& out_frame)
{
  if (!hw_frame || !renderer_) {
    return false;
  }

  if (hw_frame->format == AV_PIX_FMT_RGBA || hw_frame->format == AV_PIX_FMT_BGRA) {
    TexturePtr tex = renderer_->CreateTexture(params);
    if (tex) {
      const int bytes_per_pixel = params.GetBytesPerPixel();
      tex->Upload(hw_frame->data[0], bytes_per_pixel > 0 ? hw_frame->linesize[0] / bytes_per_pixel : hw_frame->linesize[0]);
      out_frame.texture = tex;
      out_frame.pts = hw_frame->pts;
      out_frame.width = hw_frame->width;
      out_frame.height = hw_frame->height;
      return true;
    }
  }

  return false;
}

void ReinforcedSoftwarePipeline::Close()
{
  if (allocated_threads_ > 0) {
    Tier3ThreadBudget::Release(allocated_threads_);
    allocated_threads_ = 0;
  }
  codec_ctx_ = nullptr;
}

} // namespace olive
