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
 * Phase 7 / patch 0034: frame cache container probe.
 *
 * Patch 0034 replaces the OpenEXR/JPEG on-disk frame cache with a small
 * self-describing "OLVC" container (a fixed header followed by LZ4-compressed
 * pixel data). This test drives the real FrameHashCache save/load statics:
 *
 *   1. CachePathName() must produce a ".olvcache" filename.
 *   2. Every supported format (U8/U16/F16/F32) x channel count (RGB/RGBA) x
 *      divider (1/2) x pixel aspect ratio must round-trip bit-exact, including
 *      the reconstructed VideoParams.
 *   3. A missing file, a foreign file, a truncated header, an unknown
 *      version, and a valid-looking header with an impossible compressed_size
 *      must all be reported as a cache miss (nullptr) rather than misread.
 */

#include "testutil.h"

#include <cstring>

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <QUuid>
#include <QtGlobal>

#include "codec/frame.h"
#include "render/framehashcache.h"
#include "render/videoparams.h"

using namespace olive::core;

namespace olive {

namespace {

const int kEffectiveWidth = 16;
const int kEffectiveHeight = 8;

/**
 * @brief Fill the meaningful pixels of a frame with a deterministic byte pattern.
 *
 * The pattern is varied enough that LZ4 has real work to do, but never all-zero.
 */
void FillFrame(const FramePtr &frame, int seed)
{
  const int row_bytes = frame->width() * frame->channel_count() * VideoParams::GetBytesPerChannel(frame->format());

  for (int y = 0; y < frame->height(); y++) {
    unsigned char *row = reinterpret_cast<unsigned char *>(frame->data()) + static_cast<size_t>(y) * frame->linesize_bytes();

    for (int i = 0; i < row_bytes; i++) {
      row[i] = static_cast<unsigned char>((seed * 31 + y * 131 + i * 7) & 0xFF);
    }
  }
}

/**
 * @brief Compare two frames on the fields the cache is required to preserve.
 *
 * Only the meaningful bytes of each row are compared; Frame::allocate() leaves
 * alignment padding uninitialised, and the cache does not store it.
 */
bool FramesEqual(const FramePtr &a, const FramePtr &b)
{
  if (a->width() != b->width() || a->height() != b->height()) {
    return false;
  }

  if (a->format() != b->format()) {
    return false;
  }

  if (a->channel_count() != b->channel_count()) {
    return false;
  }

  if (a->video_params().divider() != b->video_params().divider()) {
    return false;
  }

  if (!(a->video_params().pixel_aspect_ratio() == b->video_params().pixel_aspect_ratio())) {
    return false;
  }

  const int row_bytes = a->width() * a->channel_count() * VideoParams::GetBytesPerChannel(a->format());

  for (int y = 0; y < a->height(); y++) {
    if (memcmp(a->const_data() + static_cast<size_t>(y) * a->linesize_bytes(),
               b->const_data() + static_cast<size_t>(y) * b->linesize_bytes(),
               row_bytes) != 0) {
      return false;
    }
  }

  return true;
}

} // namespace

OLIVE_ADD_TEST(FrameCacheContainer)
{
  QTemporaryDir dir;
  if (!dir.isValid()) {
    std::cout << " - could not create temp dir";
    return __LINE__;
  }

  // 1. The on-disk name must carry the new extension. CachePathName() is
  // private, so verify it through the public path-based API: write a cache file
  // by hand at <cache_path>/<uuid>/<time>.olvcache and require
  // LoadCacheFrame(cache_path, uuid, time) to find it.
  {
    FramePtr probe = Frame::Create();
    probe->set_video_params(VideoParams(kEffectiveWidth,
                                        kEffectiveHeight,
                                        PixelFormat::U8,
                                        VideoParams::kRGBAChannelCount,
                                        rational(1, 1),
                                        VideoParams::kInterlaceNone,
                                        1));

    if (!probe->allocate()) {
      std::cout << " - probe allocation failed";
      return __LINE__;
    }

    FillFrame(probe, 7);

    const QUuid uuid = QUuid::createUuid();
    const int64_t timestamp = 12345;
    const QString probe_fn = QDir(dir.path()).filePath(uuid.toString() + QLatin1Char('/') + QString::number(timestamp) + QStringLiteral(".olvcache"));

    if (!QDir().mkpath(QFileInfo(probe_fn).path())) {
      std::cout << " - could not create cache subdir";
      return __LINE__;
    }

    if (!FrameHashCache::SaveCacheFrame(probe_fn, probe)) {
      std::cout << " - probe save failed";
      return __LINE__;
    }

    FramePtr via_api = FrameHashCache::LoadCacheFrame(dir.path(), uuid, timestamp);

    if (!via_api || !FramesEqual(probe, via_api)) {
      std::cout << " - path-based load did not find the .olvcache file";
      return __LINE__;
    }
  }

  // 2. Round-trip every supported configuration.
  const PixelFormat::Format formats[] = {
    PixelFormat::U8,
    PixelFormat::U16,
    PixelFormat::F16,
    PixelFormat::F32
  };
  const int channel_counts[] = {
    VideoParams::kRGBChannelCount,
    VideoParams::kRGBAChannelCount
  };
  const int dividers[] = { 1, 2 };
  const rational pars[] = { rational(1, 1), rational(4, 3) };

  int case_index = 0;

  for (PixelFormat::Format format : formats) {
    for (int channel_count : channel_counts) {
      for (int divider : dividers) {
        for (const rational &par : pars) {
          FramePtr src = Frame::Create();
          src->set_video_params(VideoParams(kEffectiveWidth * divider,
                                            kEffectiveHeight * divider,
                                            format,
                                            channel_count,
                                            par,
                                            VideoParams::kInterlaceNone,
                                            divider));

          if (!src->allocate()) {
            std::cout << " - allocation failed for case " << case_index;
            return __LINE__;
          }

          FillFrame(src, case_index);

          const QString fn = dir.filePath(QStringLiteral("case_%1.olvcache").arg(case_index));

          if (!FrameHashCache::SaveCacheFrame(fn, src)) {
            std::cout << " - SaveCacheFrame failed for case " << case_index;
            return __LINE__;
          }

          if (!QFileInfo::exists(fn) || QFileInfo(fn).size() == 0) {
            std::cout << " - cache file missing/empty for case " << case_index;
            return __LINE__;
          }

          FramePtr loaded = FrameHashCache::LoadCacheFrame(fn);

          if (!loaded) {
            std::cout << " - LoadCacheFrame returned null for case " << case_index;
            return __LINE__;
          }

          if (!FramesEqual(src, loaded)) {
            std::cout << " - round-trip mismatch for case " << case_index;
            return __LINE__;
          }

          case_index++;
        }
      }
    }
  }

  // 3. Everything that is not a valid OLVC container is a cache miss.
  if (FrameHashCache::LoadCacheFrame(dir.filePath(QStringLiteral("does_not_exist.olvcache")))) {
    std::cout << " - missing file was not a cache miss";
    return __LINE__;
  }

  const QString foreign = dir.filePath(QStringLiteral("legacy_cache_file"));
  {
    QFile f(foreign);
    if (!f.open(QFile::WriteOnly) || f.write("this is not an olive cache") < 0) {
      std::cout << " - could not write foreign file";
      return __LINE__;
    }
  }
  if (FrameHashCache::LoadCacheFrame(foreign)) {
    std::cout << " - foreign file was not a cache miss";
    return __LINE__;
  }

  const QString truncated = dir.filePath(QStringLiteral("truncated.olvcache"));
  {
    QFile f(truncated);
    if (!f.open(QFile::WriteOnly)) {
      std::cout << " - could not write truncated file";
      return __LINE__;
    }
    QDataStream stream(&f);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("OLVC", 4);
    stream << quint32(1);
  }
  if (FrameHashCache::LoadCacheFrame(truncated)) {
    std::cout << " - truncated header was not a cache miss";
    return __LINE__;
  }

  const QString wrong_version = dir.filePath(QStringLiteral("wrong_version.olvcache"));
  {
    QFile f(wrong_version);
    if (!f.open(QFile::WriteOnly)) {
      std::cout << " - could not write wrong-version file";
      return __LINE__;
    }
    QDataStream stream(&f);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("OLVC", 4);
    stream << quint32(99);
  }
  if (FrameHashCache::LoadCacheFrame(wrong_version)) {
    std::cout << " - unknown version was not a cache miss";
    return __LINE__;
  }

  // 4. F-6 (HAL/Phase6-8 review): a header that is otherwise well-formed but declares an
  //    impossible compressed_size must be rejected before it is used to allocate a buffer.
  //    The fields mirror exactly what SaveCacheFrame() writes (see framehashcache.cpp),
  //    with a consistent raw_size for a 16x8 RGBA U8 frame and an absurd compressed_size.
  //    This is a hardening contract: the pre-fix build also happened to return nullptr here
  //    (the short read failed), but only after asking for the bogus allocation described by
  //    the header.
  {
    const QString oversized = dir.filePath(QStringLiteral("oversized_payload.olvcache"));
    QFile f(oversized);
    if (!f.open(QFile::WriteOnly)) {
      std::cout << " - could not write oversized-payload file";
      return __LINE__;
    }
    QDataStream stream(&f);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("OLVC", 4);
    stream << quint32(1);                       // version
    stream << quint32(kEffectiveWidth);         // width
    stream << quint32(kEffectiveHeight);        // height
    stream << quint32(PixelFormat::U8);         // format
    stream << quint32(VideoParams::kRGBAChannelCount);
    stream << quint32(1);                       // divider
    stream << quint32(1);                       // par numerator
    stream << quint32(1);                       // par denominator
    stream << quint64(kEffectiveWidth * kEffectiveHeight
                      * VideoParams::kRGBAChannelCount);  // raw_size (U8 = 1 byte/channel)
    stream << quint32(0x40000000);              // compressed_size: 1 GiB, far above the LZ4 bound
    f.close();

    if (FrameHashCache::LoadCacheFrame(oversized)) {
      std::cout << " - oversized compressed_size was not a cache miss";
      return __LINE__;
    }
  }

  OLIVE_TEST_END;
}

} // namespace olive
