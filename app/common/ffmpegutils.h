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

#ifndef FFMPEGABSTRACTION_H
#define FFMPEGABSTRACTION_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
}

#include <olive/core/core.h>

#include "render/videoparams.h"

namespace olive {

using namespace core;

class FFmpegUtils {
public:
  /**
   * @brief Returns an AVPixelFormat that can be used to convert a frame to a data type Olive supports with minimal data loss
   */
  static AVPixelFormat GetCompatiblePixelFormat(const AVPixelFormat& pix_fmt, PixelFormat maximum = PixelFormat::INVALID);

  /**
   * @brief Returns a native pixel format that can be used to convert from a native frame to an AVFrame with minimal data loss
   */
  static PixelFormat GetCompatiblePixelFormat(const PixelFormat& pix_fmt);

  /**
   * @brief Returns an FFmpeg pixel format for a given native pixel format
   */
  static AVPixelFormat GetFFmpegPixelFormat(const PixelFormat& pix_fmt, int channel_layout);

  /**
   * @brief Returns a native sample format type for a given AVSampleFormat
   */
  static SampleFormat GetNativeSampleFormat(const AVSampleFormat& smp_fmt);

  /**
   * @brief Returns an FFmpeg sample format type for a given native type
   */
  static AVSampleFormat GetFFmpegSampleFormat(const SampleFormat &smp_fmt);

  /**
   * @brief Returns an SWS_CS_* macro from an AVColorSpace enum member
   *
   * Why aren't these the same thing anyway? And for that matter, why doesn't FFmpeg provide a
   * convenience function to do this conversion for us? Who knows, but here we are.
   */
  static int GetSwsColorspaceFromAVColorSpace(AVColorSpace cs);

  /**
   * @brief Convert "JPEG"/full-range colorspace to its regular counterpart
   *
   * "JPEG "spaces are deprecated in favor of the regular space and setting `color_range`. For the
   * time being, FFmpeg still uses these JPEG spaces, so for simplicity (since we *are* color_range
   * aware), we use this function.
   */
  static AVPixelFormat ConvertJPEGSpaceToRegularSpace(AVPixelFormat f);

  /**
   * @brief RAII holder for an AVChannelLayout
   *
   * FFmpeg's contract is that an AVChannelLayout must be released with av_channel_layout_uninit().
   * Every layout constructed here is either AV_CHANNEL_ORDER_NATIVE (from a mask) or
   * AV_CHANNEL_ORDER_UNSPEC, neither of which allocates, so the destructor is currently a no-op —
   * it is kept so the type stays correct if a future call site ever stores a
   * AV_CHANNEL_ORDER_CUSTOM layout (which does allocate a channel map).
   */
  class ChannelLayoutGuard {
  public:
    ChannelLayoutGuard()
    {
      av_channel_layout_default(&layout_, 0);
    }

    explicit ChannelLayoutGuard(uint64_t mask)
    {
      // av_channel_layout_from_mask() rejects a zero mask with AVERROR(EINVAL), so fall back to
      // an unspecified layout. Note a non-zero mask always yields AV_CHANNEL_ORDER_NATIVE.
      if (mask == 0 || av_channel_layout_from_mask(&layout_, mask) < 0) {
        av_channel_layout_default(&layout_, 0);
      }
    }

    ChannelLayoutGuard(const ChannelLayoutGuard &) = delete;
    ChannelLayoutGuard &operator=(const ChannelLayoutGuard &) = delete;

    ~ChannelLayoutGuard()
    {
      av_channel_layout_uninit(&layout_);
    }

    const AVChannelLayout *get() const { return &layout_; }

  private:
    AVChannelLayout layout_{};
  };

  /**
   * @brief Convert an AVChannelLayout into Olive's uint64_t channel-layout mask
   *
   * Replaces the removed `AVCodecParameters::channel_layout` field. Only
   * AV_CHANNEL_ORDER_NATIVE can be represented losslessly by a mask; any other order returns 0,
   * leaving the caller to fall back to a default layout for the channel count.
   */
  static uint64_t ChannelLayoutToMask(const AVChannelLayout &layout);

  /**
   * @brief Get the default channel-layout mask for a given channel count
   *
   * Equivalent to the removed av_get_default_channel_layout().
   */
  static uint64_t DefaultChannelLayoutMask(int nb_channels);
};

using AVFramePtr = std::shared_ptr<AVFrame>;
inline AVFramePtr CreateAVFramePtr(AVFrame *f)
{
  return std::shared_ptr<AVFrame>(f, [](AVFrame *g){ av_frame_free(&g); });
}
inline AVFramePtr CreateAVFramePtr()
{
  return CreateAVFramePtr(av_frame_alloc());
}

}

#endif // FFMPEGABSTRACTION_H
