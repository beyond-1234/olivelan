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

/**
 * Phase 14 decode-input policy gates.
 *
 * These pin the two routing decisions exposed by the colour-fixture decode tests:
 *
 *   1. VideoToolbox IOSurface import only supports H.264/HEVC bi-planar 4:2:0.
 *      ProRes 4444 / 4:2:2 / MPEG-2 / VP8 must be rejected before a hardware
 *      device is created, so a forced VideoToolbox backend falls back cleanly
 *      instead of warning on every frame.
 *
 *   2. Dolby Vision Profile 5 (IPT/ICtCp, no HDR10/HLG base layer) must be
 *      detected and refused. Profile 8.x is allowed to fall back to its base
 *      layer, with the RPU deliberately unprocessed.
 */

#include "testutil.h"

#include <cstdint>

#include "codec/decode_input_policy.h"

namespace olive {

OLIVE_ADD_TEST(VideoToolboxInputPolicy)
{
  // H.264/HEVC 4:2:0 at 8/10-bit are the only layouts the IOSurface import can consume.
  OLIVE_ASSERT(IsVideoToolboxBiPlanar420Input(AV_CODEC_ID_H264, AV_PIX_FMT_YUV420P));
  OLIVE_ASSERT(IsVideoToolboxBiPlanar420Input(AV_CODEC_ID_H264, AV_PIX_FMT_YUVJ420P));
  OLIVE_ASSERT(IsVideoToolboxBiPlanar420Input(AV_CODEC_ID_HEVC, AV_PIX_FMT_YUV420P10LE));
  OLIVE_ASSERT(IsVideoToolboxBiPlanar420Input(AV_CODEC_ID_HEVC, AV_PIX_FMT_P010LE));

  // ProRes 4444 (yuv444p12le) and 4:2:2 must be rejected, even though the codec is
  // nominally a VideoToolbox-decodable one.
  OLIVE_ASSERT(!IsVideoToolboxBiPlanar420Input(AV_CODEC_ID_PRORES, AV_PIX_FMT_YUV444P12LE));
  OLIVE_ASSERT(!IsVideoToolboxBiPlanar420Input(AV_CODEC_ID_PRORES, AV_PIX_FMT_YUV422P10LE));

  // 4:2:2 H.264 (XAVC) is a format-level rejection.
  OLIVE_ASSERT(!IsVideoToolboxBiPlanar420Input(AV_CODEC_ID_H264, AV_PIX_FMT_YUV422P10LE));

  // Codec-level rejections: VideoToolbox has no MPEG-2/VP8 path in this pipeline.
  OLIVE_ASSERT(!IsVideoToolboxBiPlanar420Input(AV_CODEC_ID_MPEG2VIDEO, AV_PIX_FMT_YUV420P));
  OLIVE_ASSERT(!IsVideoToolboxBiPlanar420Input(AV_CODEC_ID_VP8, AV_PIX_FMT_YUV420P));

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(DolbyVisionProfilePolicy)
{
  AVDOVIDecoderConfigurationRecord record{};
  AVPacketSideData side_data{};

  side_data.type = AV_PKT_DATA_DOVI_CONF;
  side_data.data = reinterpret_cast<uint8_t*>(&record);
  side_data.size = sizeof(record);

  // Profile 5: reject (IPT/ICtCp, no HDR10/HLG base layer).
  record.dv_profile = 5;
  OLIVE_ASSERT(GetDoviProfileFromSideData(&side_data, 1) == 5);

  // Profile 8.x: allow base-layer fallback, with the RPU unprocessed.
  record.dv_profile = 8;
  OLIVE_ASSERT(GetDoviProfileFromSideData(&side_data, 1) == 8);

  // Unrelated side data must not be mistaken for DoVi.
  side_data.type = AV_PKT_DATA_MASTERING_DISPLAY_METADATA;
  OLIVE_ASSERT(GetDoviProfileFromSideData(&side_data, 1) == 0);

  // Truncated records are ignored rather than read out of bounds.
  side_data.type = AV_PKT_DATA_DOVI_CONF;
  side_data.size = sizeof(record) - 1;
  OLIVE_ASSERT(GetDoviProfileFromSideData(&side_data, 1) == 0);

  // Null/empty inputs are safe.
  OLIVE_ASSERT(GetDoviProfileFromSideData(nullptr, 1) == 0);
  OLIVE_ASSERT(GetDoviProfile(nullptr) == 0);

  OLIVE_TEST_END;
}

} // namespace olive
