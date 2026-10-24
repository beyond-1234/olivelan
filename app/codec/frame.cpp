/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2022 Olive Team

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

#include "frame.h"

#include <algorithm>
#include <cstdint>

#include <QDebug>
#include <QtGlobal>
#include <QtMath>

#include <olive/core/util/half.h>

#include "render/framemanager.h"

namespace olive {

Frame::Frame() :
  data_(nullptr),
  data_size_(0),
  timestamp_(0)
{
}

Frame::~Frame()
{
  destroy();
}

FramePtr Frame::Create()
{
  return std::make_shared<Frame>();
}

const VideoParams &Frame::video_params() const
{
  return params_;
}

void Frame::set_video_params(const VideoParams &params)
{
  params_ = params;

  linesize_ = generate_linesize_bytes(width(), params_.format(), params_.channel_count());
  linesize_pixels_ = linesize_ / params_.GetBytesPerPixel();
}

FramePtr Frame::Interlace(FramePtr top, FramePtr bottom)
{
  if (top->video_params() != bottom->video_params()) {
    qCritical() << "Tried to interlace two frames that had incompatible parameters";
    return nullptr;
  }

  FramePtr interlaced = Frame::Create();
  interlaced->set_video_params(top->video_params());
  interlaced->allocate();

  int linesize = interlaced->linesize_bytes();

  for (int i=0; i<interlaced->height(); i++) {
    FramePtr which = (i%2 == 0) ? top : bottom;

    memcpy(interlaced->data() + i*linesize,
           which->const_data() + i*linesize,
           linesize);
  }

  return interlaced;
}

int Frame::generate_linesize_bytes(int width, PixelFormat format, int channel_count)
{
  // Align to 32 bytes (not sure if this is necessary?)
  return VideoParams::GetBytesPerPixel(format, channel_count) * ((width + 31) & ~31);
}

Color Frame::get_pixel(int x, int y) const
{
  if (!contains_pixel(x, y)) {
    return Color();
  }

  int byte_offset = y * linesize_bytes() + x * video_params().GetBytesPerPixel();

  return Color(reinterpret_cast<const char*>(data_ + byte_offset), video_params().format(), video_params().channel_count());
}

bool Frame::contains_pixel(int x, int y) const
{
  return (is_allocated() && x >= 0 && x < width() && y >= 0 && y < height());
}

void Frame::set_pixel(int x, int y, const Color &c)
{
  if (!contains_pixel(x, y)) {
    return;
  }

  int byte_offset = y * linesize_bytes() + x * video_params().GetBytesPerPixel();

  c.toData(reinterpret_cast<char*>(data_ + byte_offset), video_params().format(), video_params().channel_count());
}

bool Frame::allocate()
{
  // Assume this frame is intended to be a video frame
  if (!params_.is_valid()) {
    qWarning() << "Tried to allocate a frame with invalid parameters";
    return false;
  }

  if (is_allocated()) {
    // Already allocated
    return true;
  }

  data_size_ = linesize_ * height();
  data_ = FrameManager::Allocate(data_size_);

  return true;
}

void Frame::destroy()
{
  if (is_allocated()) {
    FrameManager::Deallocate(data_size_, data_);

    data_size_ = 0;
    data_ = nullptr;
  }
}

namespace {

inline float Clamp01(float v)
{
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Per-component codecs. The decode/encode formulas deliberately mirror
// OpenImageIO's conversion - normalised integer scaling, and round-to-nearest
// implemented as "multiply, add 0.5f, truncate" in float arithmetic - so the
// phase-6 Golden matrix in tests/golden/pixel_convert stays bit-exact after
// OIIO is removed (see patch 0031).
struct U8Codec
{
  static constexpr int kSize = 1;
  static float From(const char *p) { return float(*reinterpret_cast<const uint8_t *>(p)) / 255.0f; }
  static void To(char *p, float v) { *reinterpret_cast<uint8_t *>(p) = static_cast<uint8_t>(Clamp01(v) * 255.0f + 0.5f); }
};

struct U16Codec
{
  static constexpr int kSize = 2;
  static float From(const char *p) { return float(*reinterpret_cast<const uint16_t *>(p)) / 65535.0f; }
  static void To(char *p, float v) { *reinterpret_cast<uint16_t *>(p) = static_cast<uint16_t>(Clamp01(v) * 65535.0f + 0.5f); }
};

struct F16Codec
{
  static constexpr int kSize = 2;
  static float From(const char *p) { return core::half_to_float(*reinterpret_cast<const uint16_t *>(p)); }
  static void To(char *p, float v) { *reinterpret_cast<uint16_t *>(p) = core::float_to_half(v); }
};

struct F32Codec
{
  static constexpr int kSize = 4;
  static float From(const char *p) { return *reinterpret_cast<const float *>(p); }
  static void To(char *p, float v) { *reinterpret_cast<float *>(p) = v; }
};

template <typename SrcCodec, typename DstCodec>
void ConvertPixels(const Frame *src, Frame *dst)
{
  const int width = std::min(src->width(), dst->width());
  const int height = std::min(src->height(), dst->height());
  const int src_ch = src->channel_count();
  const int dst_ch = dst->channel_count();
  const int copy_ch = std::min(src_ch, dst_ch);

  for (int y = 0; y < height; ++y) {
    const char *src_row = src->const_data() + y * src->linesize_bytes();
    char *dst_row = dst->data() + y * dst->linesize_bytes();

    for (int x = 0; x < width; ++x) {
      const char *src_px = src_row + x * src_ch * SrcCodec::kSize;
      char *dst_px = dst_row + x * dst_ch * DstCodec::kSize;

      for (int c = 0; c < copy_ch; ++c) {
        DstCodec::To(dst_px + c * DstCodec::kSize, SrcCodec::From(src_px + c * SrcCodec::kSize));
      }

      // Expanding RGB -> RGBA (or any trailing channel) fills opaque.
      for (int c = copy_ch; c < dst_ch; ++c) {
        DstCodec::To(dst_px + c * DstCodec::kSize, 1.0f);
      }
    }
  }
}

void ConvertPixelsDispatch(const Frame *src, Frame *dst)
{
  const PixelFormat dst_format = dst->format();

#define OLIVE_CONVERT_DST(SRC_CODEC)                                            \
    case PixelFormat::U8:  ConvertPixels<SRC_CODEC, U8Codec>(src, dst); break;  \
    case PixelFormat::U16: ConvertPixels<SRC_CODEC, U16Codec>(src, dst); break; \
    case PixelFormat::F16: ConvertPixels<SRC_CODEC, F16Codec>(src, dst); break; \
    case PixelFormat::F32: ConvertPixels<SRC_CODEC, F32Codec>(src, dst); break;

  switch (src->format()) {
  case PixelFormat::U8:
    switch (dst_format) { OLIVE_CONVERT_DST(U8Codec) default: break; }
    break;
  case PixelFormat::U16:
    switch (dst_format) { OLIVE_CONVERT_DST(U16Codec) default: break; }
    break;
  case PixelFormat::F16:
    switch (dst_format) { OLIVE_CONVERT_DST(F16Codec) default: break; }
    break;
  case PixelFormat::F32:
    switch (dst_format) { OLIVE_CONVERT_DST(F32Codec) default: break; }
    break;
  default:
    break;
  }

#undef OLIVE_CONVERT_DST
}

} // namespace

FramePtr Frame::convert(PixelFormat format) const
{
  // Create new params with destination format
  VideoParams params = params_;
  params.set_format(format);

  // Create new frame
  FramePtr converted = Frame::Create();
  converted->set_video_params(params);
  converted->set_timestamp(timestamp_);

  if (!converted->allocate()) {
    return nullptr;
  }

  // Direct, linesize-safe conversion. This used to round-trip through
  // OIIO::ImageBuf; see the codec structs above for the exact quantisation
  // contract that keeps the phase-6 Golden matrix bit-exact.
  ConvertPixelsDispatch(this, converted.get());

  return converted;
}

}
