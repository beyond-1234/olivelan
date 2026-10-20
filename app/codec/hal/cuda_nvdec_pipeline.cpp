#include "cuda_nvdec_pipeline.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <QDebug>
#include "codec/ffmpeg/hardware_quota.h"
#include "render/opengl/openglrenderer.h"

namespace olive {

CudaNvdecPipeline::CudaNvdecPipeline(OpenGLRenderer* renderer)
  : renderer_(renderer)
{
}

CudaNvdecPipeline::~CudaNvdecPipeline()
{
  Close();
}

bool CudaNvdecPipeline::ProbeAndInitialize(AVCodecContext* codec_ctx, int max_w, int max_h)
{
#if !defined(OLIVE_ENABLE_CUDA_NVDEC)
  return false;
#else
  if (!codec_ctx) {
    return false;
  }

  max_width_ = max_w > 0 ? max_w : 4096;
  max_height_ = max_h > 0 ? max_h : 4096;

  // 1. Acquire physical session admission slot via dynamic cost scoring
  double fps = 60.0;
  if (codec_ctx->framerate.num > 0 && codec_ctx->framerate.den > 0) {
    fps = av_q2d(codec_ctx->framerate);
  }
  bool is_10bit = (codec_ctx->pix_fmt == AV_PIX_FMT_P010LE || codec_ctx->pix_fmt == AV_PIX_FMT_P010BE || codec_ctx->pix_fmt == AV_PIX_FMT_YUV420P10LE);
  quota_slot_ = HardwareQuotaManager::AcquireBudgetSlot(codec_ctx->width, codec_ctx->height, fps, is_10bit, true);
  if (quota_slot_ < 0) {
    qWarning() << "[CUDA-NVDEC HAL] Rejected: hardware session budget or capacity saturated ("
               << HardwareQuotaManager::GetAllocatedPoints() << "/"
               << HardwareQuotaManager::GetTotalBudgetPoints() << "points in use)";
    return false;
  }

  // 2. Allocate CUDA hardware device context
  int err = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
  if (err < 0 || !hw_device_ctx_) {
    qWarning() << "[CUDA-NVDEC HAL] Failed to create CUDA hw device context, code:" << err;
    if (hw_device_ctx_) {
      av_buffer_unref(&hw_device_ctx_);
      hw_device_ctx_ = nullptr;
    }
    HardwareQuotaManager::ReleaseSlot(quota_slot_);
    quota_slot_ = -1;
    return false;
  }

  // 3. Attach reference to codec context
  codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
  if (!codec_ctx->hw_device_ctx) {
    qWarning() << "[CUDA-NVDEC HAL] Failed to reference hw device context to codec";
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
    HardwareQuotaManager::ReleaseSlot(quota_slot_);
    quota_slot_ = -1;
    return false;
  }

  qInfo() << "[CUDA-NVDEC HAL] Successfully initialized session (slot:" << quota_slot_
          << "cost:" << HardwareQuotaManager::CalculateStreamCost(codec_ctx->width, codec_ctx->height, fps, is_10bit)
          << "pts; active:" << HardwareQuotaManager::GetAllocatedPoints()
          << "/" << HardwareQuotaManager::GetTotalBudgetPoints() << "pts)";
  return true;
#endif
}

bool CudaNvdecPipeline::ProcessHardwareFrame(AVFrame* hw_frame, const VideoParams& params, HardwareDecodedFrame& out_frame)
{
#if !defined(OLIVE_ENABLE_CUDA_NVDEC)
  return false;
#else
  if (!hw_frame || hw_frame->format != AV_PIX_FMT_CUDA || !renderer_) {
    return false;
  }

  TexturePtr tex = renderer_->CreateTextureFromCudaFrame(params, hw_frame);
  if (!tex) {
    return false;
  }

  out_frame.texture = tex;
  // Report the texture's real (divider-scaled) storage, not the decoded frame size.
  out_frame.width = tex->width();
  out_frame.height = tex->height();
  // F-8 fix (HAL/Phase6-8 review): the removed coded_width used to be assigned
  // hw_frame->linesize[0], which is a BYTE stride, not a padded pixel width - the two are
  // only equal for 8-bit 4:2:0 luma. Nothing consumed the field, so it is gone rather than
  // reported with the wrong units.
  out_frame.pts = hw_frame->pts;

  frame_count_++;
  return true;
#endif
}

void CudaNvdecPipeline::SetRenderer(Renderer* renderer)
{
  // This backend is CUDA-GL interop: it only ever works against the OpenGL renderer, so the
  // narrowing happens here rather than making every caller perform it.
  renderer_ = dynamic_cast<OpenGLRenderer*>(renderer);
}

void CudaNvdecPipeline::UpdatePriority(bool is_primary)
{
  if (quota_slot_ >= 0) {
    HardwareQuotaManager::UpdateSlotPriority(quota_slot_, is_primary);
  }
}

void CudaNvdecPipeline::Close()
{
  if (hw_device_ctx_) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
  }

  if (quota_slot_ >= 0) {
    HardwareQuotaManager::ReleaseSlot(quota_slot_);
    quota_slot_ = -1;
  }
}

} // namespace olive
