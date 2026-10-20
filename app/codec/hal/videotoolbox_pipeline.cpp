#include "videotoolbox_pipeline.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
}

#include <atomic>

#include <QDebug>
#include "codec/decode_input_policy.h"
#include "common/ffmpegutils.h"
#include "render/opengl/openglrenderer.h"

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/CGLCurrent.h>
#include <OpenGL/gl.h>
#endif

namespace olive {

#if defined(__APPLE__)
// Resolve the matrix from the decoded frame and the range from the CVPixelBuffer pixel format.
// FFmpeg exposes an unspecified frame range as MPEG when the decoder falls back to the cached
// VideoParams, so frame->color_range alone cannot distinguish a limited NV12 surface from a
// full-range one. The pixel format names the range VideoToolbox actually produced and is
// therefore authoritative unless the user explicitly overrode it in Footage Properties.
static VideoParams ResolveColorMetadata(const VideoParams& params, const AVFrame* frame,
                                        bool pixel_format_full_range)
{
  VideoParams effective = params;

  if (!frame) {
    return effective;
  }

  if (frame->colorspace != AVCOL_SPC_UNSPECIFIED) {
    effective.set_color_space(frame->colorspace);
  }

  if (frame->color_primaries != AVCOL_PRI_UNSPECIFIED) {
    effective.set_color_primaries(frame->color_primaries);
  }

  if (frame->color_trc != AVCOL_TRC_UNSPECIFIED) {
    effective.set_color_trc(frame->color_trc);

    const QString hdr_colorspace =
        VideoParams::GetHdrColorSpaceForTransfer(frame->color_trc);
    if (!hdr_colorspace.isEmpty()) {
      effective.set_colorspace(hdr_colorspace);
    }
  }

  if (params.color_range_overridden()) {
    effective.set_color_range(params.color_range());
  } else {
    effective.set_color_range(pixel_format_full_range ? VideoParams::kColorRangeFull
                                                      : VideoParams::kColorRangeLimited);
  }

  for (int i = 0; i < frame->nb_side_data; i++) {
    const AVFrameSideData* side_data = frame->side_data[i];
    if (side_data
        && side_data->type == AV_FRAME_DATA_MASTERING_DISPLAY_METADATA
        && side_data->size >= int(sizeof(AVMasteringDisplayMetadata))) {
      const auto* metadata =
          reinterpret_cast<const AVMasteringDisplayMetadata*>(side_data->data);
      if (metadata->has_luminance && metadata->max_luminance.den != 0) {
        effective.set_mastering_peak_nits(VideoParams::MasteringDisplayMaxLuminanceToNits(
            metadata->max_luminance.num, metadata->max_luminance.den));
      }
    }
  }

  return effective;
}
#endif

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

  if (!IsVideoToolboxBiPlanar420Input(codec_ctx->codec_id, codec_ctx->pix_fmt)) {
    qWarning() << "[VideoToolbox-HAL] Input layout" << av_get_pix_fmt_name(codec_ctx->pix_fmt)
               << "for codec" << avcodec_get_name(codec_ctx->codec_id)
               << "is not a 4:2:0 bi-planar format; refusing VideoToolbox import"
               << "(route to software/Tier 3 instead).";
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

  if (!CVPixelBufferIsPlanar(pixel_buffer) || CVPixelBufferGetPlaneCount(pixel_buffer) < 2) {
    qWarning() << "[VideoToolbox-HAL] Unsupported non-planar CVPixelBuffer"
               << "format" << Qt::hex << CVPixelBufferGetPixelFormatType(pixel_buffer) << Qt::dec;
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

  // VideoToolbox outputs NV12 for 8-bit and P010 for 10-bit. Read the pixel format rather than
  // inferring depth from the luma stride: NV12 rows may be padded beyond 2*width, which would
  // misclassify an 8-bit surface as P010 and feed the shader the wrong normalization domain.
  const OSType pixel_format = CVPixelBufferGetPixelFormatType(pixel_buffer);
  int bits_per_pixel = 0;
  bool pixel_format_full_range = false;
  switch (pixel_format) {
  case kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange:
    bits_per_pixel = 8;
    break;
  case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
    bits_per_pixel = 8;
    pixel_format_full_range = true;
    break;
  case kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange:
    bits_per_pixel = 10;
    break;
  case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
    bits_per_pixel = 10;
    pixel_format_full_range = true;
    break;
  default:
    qWarning() << "[VideoToolbox-HAL] Unsupported CVPixelBuffer format:"
               << Qt::hex << pixel_format << Qt::dec;
    return false;
  }

  const VideoParams effective =
      ResolveColorMetadata(params, hw_frame, pixel_format_full_range);
  auto* sws_coeffs = sws_getCoefficients(FFmpegUtils::GetSwsColorspaceFromAVColorSpace(
      static_cast<AVColorSpace>(effective.color_space())));
  float crv = 0.0f, cbu = 0.0f, cgu = 0.0f, cgv = 0.0f;
  if (sws_coeffs) {
    crv = static_cast<float>(sws_coeffs[0]) / 65536.0f;
    cbu = static_cast<float>(sws_coeffs[1]) / 65536.0f;
    cgu = static_cast<float>(sws_coeffs[2]) / 65536.0f;
    cgv = static_cast<float>(sws_coeffs[3]) / 65536.0f;
  }
  const bool full_range = (effective.color_range() == VideoParams::kColorRangeFull);

  TexturePtr tex = renderer_->CreateTextureFromIOSurface(
      effective, iosurface, hw_frame->width, hw_frame->height, bits_per_pixel,
      full_range, crv, cbu, cgu, cgv);
  if (!tex) {
    qWarning() << "[VideoToolbox-HAL] Failed to import IOSurface planes";
    return false;
  }

  static std::atomic<bool> direct_path_announced{false};
  if (!direct_path_announced.exchange(true)) {
    qInfo() << "[VideoToolbox-HAL] Two-plane IOSurface import active:"
            << hw_frame->width << "x" << hw_frame->height
            << "bits:" << bits_per_pixel
            << "range:" << (full_range ? "full" : "limited")
            << "colour matrix:" << effective.color_space()
            << "- GPU-resident YUV->RGB compositing (no per-frame swscale)";
  }

  // The harness reads the last "hardware frames N" line as a VideoToolbox decode counter.
  // Log once immediately (so short playback sessions still produce a sample) and then once
  // per second at 24 fps. This is deliberately low-volume and does not affect frame pacing.
  static std::atomic<quint64> hardware_frame_count{0};
  const quint64 frame_number =
      hardware_frame_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (frame_number == 1 || frame_number % 24 == 0) {
    qInfo() << "[VideoToolbox-HAL] hardware frames" << frame_number
            << "size" << hw_frame->width << "x" << hw_frame->height
            << "bits" << bits_per_pixel;
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
