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

#ifndef OLIVE_DECODE_INPUT_POLICY_H
#define OLIVE_DECODE_INPUT_POLICY_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/packet.h>
#include <libavutil/dovi_meta.h>
}

namespace olive {

/**
 * @brief Formats the VideoToolbox IOSurface import can actually consume.
 *
 * The import path is bi-planar 4:2:0 only: NV12 for 8-bit and P010 for 10-bit. H.264/HEVC are
 * the codecs that produce those surfaces. ProRes 4:2:2/4:4:4, MPEG-2, VP8 and 4:2:2 H.264 must
 * be rejected before a hardware device is created; otherwise a forced VideoToolbox backend logs
 * "Unsupported CVPixelBuffer format" for every frame before falling back.
 */
inline bool IsVideoToolboxBiPlanar420Input(AVCodecID codec_id, AVPixelFormat pix_fmt)
{
  if (codec_id != AV_CODEC_ID_H264 && codec_id != AV_CODEC_ID_HEVC) {
    return false;
  }

  switch (pix_fmt) {
  case AV_PIX_FMT_YUV420P:
  case AV_PIX_FMT_YUVJ420P:
  case AV_PIX_FMT_NV12:
  case AV_PIX_FMT_YUV420P10LE:
  case AV_PIX_FMT_YUV420P10BE:
  case AV_PIX_FMT_P010LE:
  case AV_PIX_FMT_P010BE:
    return true;
  default:
    return false;
  }
}

/**
 * @brief Dolby Vision profile from a DOVI configuration side-data entry, or 0.
 *
 * Profile 5 has an IPT/ICtCp base layer and no HDR10/HLG fallback; decoding it as YCbCr produces
 * wrong colours. Profiles 8.x carry an HDR10/HLG base layer and may fall back to it (without
 * applying the RPU). Keeping this pure makes the policy testable without a decoder.
 */
inline int GetDoviProfileFromSideData(const AVPacketSideData *side_data, int count)
{
  if (!side_data || count <= 0) {
    return 0;
  }

  for (int i = 0; i < count; ++i) {
    const AVPacketSideData &data = side_data[i];
    if (data.type == AV_PKT_DATA_DOVI_CONF
        && data.size >= int(sizeof(AVDOVIDecoderConfigurationRecord))) {
      const auto *record =
          reinterpret_cast<const AVDOVIDecoderConfigurationRecord *>(data.data);
      return record->dv_profile;
    }
  }

  return 0;
}

inline int GetDoviProfile(const AVCodecParameters *codecpar)
{
  if (!codecpar) {
    return 0;
  }
  return GetDoviProfileFromSideData(codecpar->coded_side_data,
                                    codecpar->nb_coded_side_data);
}

} // namespace olive

#endif // OLIVE_DECODE_INPUT_POLICY_H
