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

#include "framehashcache.h"

#include <cstring>

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "codec/frame.h"
#include "common/filefunctions.h"
#include "render/diskmanager.h"

#include "lz4.h"

namespace olive {

#define super PlaybackCache

namespace {

// Patch 0034: the on-disk frame cache is a small self-describing container
// instead of an OpenEXR file. It starts with this magic so legacy caches (and
// anything else that isn't ours) are treated as a miss rather than misread.
const char kCacheMagic[4] = {'O', 'L', 'V', 'C'};
const quint32 kCacheVersion = 1;
const char kCacheExtension[] = ".olvcache";

bool IsValidCacheFormat(PixelFormat::Format format)
{
  switch (format) {
  case PixelFormat::U8:
  case PixelFormat::U16:
  case PixelFormat::F16:
  case PixelFormat::F32:
    return true;
  case PixelFormat::INVALID:
  case PixelFormat::COUNT:
    return false;
  }

  return false;
}

void DeleteCorruptCacheFile(const QString &fn)
{
  if (DiskManager::instance()) {
    QMetaObject::invokeMethod(DiskManager::instance(), "DeleteSpecificFile", Q_ARG(QString, fn));
  }
}

} // namespace

FrameHashCache::FrameHashCache(QObject *parent) :
  super(parent)
{
  if (DiskManager::instance()) {
    connect(DiskManager::instance(), &DiskManager::DeletedFrame, this, &FrameHashCache::HashDeleted);
    connect(DiskManager::instance(), &DiskManager::InvalidateProject, this, &FrameHashCache::ProjectInvalidated);
  }
}

void FrameHashCache::SetTimebase(const rational &tb)
{
  timebase_ = tb;
}

void FrameHashCache::ValidateTimestamp(const int64_t &ts)
{
  TimeRange frame_range(ToTime(ts), ToTime(ts+1));
  Validate(frame_range);
}

void FrameHashCache::ValidateTime(const rational &time)
{
  Validate(TimeRange(time, time + timebase_));
}

QString FrameHashCache::GetValidCacheFilename(const rational &time) const
{
  if (IsFrameCached(time)) {
    return CachePathName(time);
  } else if (!GetPassthroughs().empty()) {
    for (const Passthrough &p : GetPassthroughs()) {
      if (p.Contains(time)) {
        return CachePathName(GetCacheDirectory(), p.cache, time, timebase_);
      }
    }
  }

  return QString();
}

bool FrameHashCache::SaveCacheFrame(const int64_t &time, FramePtr frame) const
{
  return SaveCacheFrame(GetCacheDirectory(), GetUuid(), time, frame);
}

bool FrameHashCache::SaveCacheFrame(const QString &cache_path, const QUuid &uuid, const int64_t &time, FramePtr frame)
{
  if (cache_path.isEmpty()) {
    qWarning() << "Failed to save cache frame with empty path";
    return false;
  }

  QString fn = CachePathName(cache_path, uuid, time);

  bool ret = SaveCacheFrame(fn, frame);

  // Register frame with the disk manager
  if (ret) {
    QMetaObject::invokeMethod(DiskManager::instance(), "CreatedFile", Q_ARG(QString, cache_path), Q_ARG(QString, fn));
  }

  return ret;
}

bool FrameHashCache::SaveCacheFrame(const QString &cache_path, const QUuid &uuid, const rational &time, const rational &tb, FramePtr frame)
{
  if (cache_path.isEmpty()) {
    qWarning() << "Failed to save cache frame with empty path";
    return false;
  }

  QString fn = CachePathName(cache_path, uuid, time, tb);

  bool ret = SaveCacheFrame(fn, frame);

  // Register frame with the disk manager
  if (ret) {
    QMetaObject::invokeMethod(DiskManager::instance(), "CreatedFile", Q_ARG(QString, cache_path), Q_ARG(QString, fn));
  }

  return ret;
}

FramePtr FrameHashCache::LoadCacheFrame(const QString &cache_path, const QUuid &uuid, const int64_t &time)
{
  // Minor optimization, we store frames currently being saved just in case something tries to load
  // while we're saving. This should *occasionally* optimize and also prevent scenarios where
  // we try to load a frame that's half way through being saved.
  QString filename = CachePathName(cache_path, uuid, time);

  if (cache_path.isEmpty()) {
    qWarning() << "Failed to load cache frame with empty path";
    return nullptr;
  }

  return LoadCacheFrame(filename);
}

FramePtr FrameHashCache::LoadCacheFrame(const int64_t &hash) const
{
  return LoadCacheFrame(GetCacheDirectory(), GetUuid(), hash);
}

FramePtr FrameHashCache::LoadCacheFrame(const QString &fn)
{
  if (fn.isEmpty() || !QFileInfo::exists(fn)) {
    return nullptr;
  }

  QFile file(fn);
  if (!file.open(QFile::ReadOnly)) {
    return nullptr;
  }

  QDataStream stream(&file);
  stream.setByteOrder(QDataStream::LittleEndian);

  char magic[4];
  quint32 version = 0;
  quint32 width = 0, height = 0, pixel_format = 0, channel_count = 0, divider = 0, par_num = 0, par_den = 0;
  quint64 raw_size = 0;
  quint32 compressed_size = 0;

  if (stream.readRawData(magic, 4) != 4 || memcmp(magic, kCacheMagic, 4) != 0) {
    // Not one of our cache files (e.g. a legacy EXR/JPEG cache). Treat it as a
    // miss and let the disk manager reclaim it.
    DeleteCorruptCacheFile(fn);
    return nullptr;
  }

  stream >> version;

  if (version != kCacheVersion) {
    qWarning() << "Cache frame" << fn << "has unsupported version" << version;
    DeleteCorruptCacheFile(fn);
    return nullptr;
  }

  stream >> width >> height >> pixel_format >> channel_count >> divider >> par_num >> par_den >> raw_size >> compressed_size;

  const PixelFormat image_format(static_cast<PixelFormat::Format>(pixel_format));

  if (stream.status() != QDataStream::Ok
      || !IsValidCacheFormat(image_format)
      || width == 0 || height == 0
      || divider == 0
      || channel_count < 1 || channel_count > VideoParams::kRGBAChannelCount
      || par_num == 0 || par_den == 0) {
    DeleteCorruptCacheFile(fn);
    return nullptr;
  }

  // Validate the header against the payload size before allocating anything, so
  // a corrupt header can't make us allocate an absurd buffer.
  const int bpc = VideoParams::GetBytesPerChannel(image_format);
  const quint64 expected_raw_size = quint64(width) * quint64(height) * quint64(channel_count) * quint64(bpc);
  if (raw_size != expected_raw_size) {
    DeleteCorruptCacheFile(fn);
    return nullptr;
  }

  // F-6 fix (HAL/Phase6-8 review): every field above comes from the file and is therefore
  // corrupt/adversarial input. Bound raw_size to a sane frame ceiling and compressed_size
  // to what LZ4 could ever have produced for this raw payload - otherwise a bogus
  // compressed_size allocates an arbitrary (up to ~4 GiB, or negative after
  // static_cast<int>) buffer.
  constexpr quint64 kMaxRawFrameBytes = 1ull << 30;  // 1 GiB: far above any real frame
  if (raw_size == 0
      || raw_size > kMaxRawFrameBytes
      || compressed_size == 0) {
    DeleteCorruptCacheFile(fn);
    return nullptr;
  }

  // raw_size <= 1 GiB keeps LZ4_compressBound() well inside int range.
  const int max_compressed_size = LZ4_compressBound(static_cast<int>(raw_size));
  if (compressed_size > quint64(max_compressed_size)) {
    DeleteCorruptCacheFile(fn);
    return nullptr;
  }

  QByteArray compressed(static_cast<int>(compressed_size), Qt::Uninitialized);
  if (stream.readRawData(compressed.data(), static_cast<int>(compressed_size)) != static_cast<int>(compressed_size)) {
    DeleteCorruptCacheFile(fn);
    return nullptr;
  }

  QByteArray raw(static_cast<int>(raw_size), Qt::Uninitialized);
  const int decompressed = LZ4_decompress_safe(compressed.constData(),
                                               raw.data(),
                                               static_cast<int>(compressed_size),
                                               static_cast<int>(raw_size));

  if (decompressed < 0 || quint64(decompressed) != raw_size) {
    qCritical() << "Failed to decompress cache frame" << fn;
    DeleteCorruptCacheFile(fn);
    return nullptr;
  }

  FramePtr frame = Frame::Create();
  frame->set_video_params(VideoParams(static_cast<int>(width) * static_cast<int>(divider),
                                      static_cast<int>(height) * static_cast<int>(divider),
                                      image_format,
                                      static_cast<int>(channel_count),
                                      rational(static_cast<int>(par_num), static_cast<int>(par_den)),
                                      VideoParams::kInterlaceNone,
                                      static_cast<int>(divider)));
  frame->allocate();

  const int row_bytes = static_cast<int>(width) * static_cast<int>(channel_count) * bpc;
  for (quint32 y = 0; y < height; y++) {
    memcpy(frame->data() + static_cast<size_t>(y) * frame->linesize_bytes(),
           raw.constData() + static_cast<size_t>(y) * row_bytes,
           row_bytes);
  }

  return frame;
}

void FrameHashCache::SetPassthrough(PlaybackCache *cache)
{
  super::SetPassthrough(cache);
  SetTimebase(static_cast<FrameHashCache*>(cache)->GetTimebase());
}

void FrameHashCache::LoadStateEvent(QDataStream &stream)
{
  uint32_t version;
  int num, den;

  stream >> version;

  switch (version) {
  case 1:
    stream >> num;
    stream >> den;
    timebase_ = rational(num, den);
    break;
  }
}

void FrameHashCache::SaveStateEvent(QDataStream &stream)
{
  uint32_t version = 1;

  stream << version;

  stream << timebase_.numerator();
  stream << timebase_.denominator();
}

rational FrameHashCache::ToTime(const int64_t &ts) const
{
  return Timecode::timestamp_to_time(ts, timebase_);
}

int64_t FrameHashCache::ToTimestamp(const rational &ts, Timecode::Rounding rounding) const
{
  return Timecode::time_to_timestamp(ts, timebase_, rounding);
}

void FrameHashCache::HashDeleted(const QString& path, const QString &filename)
{
  QString cache_dir = GetCacheDirectory();
  if (cache_dir.isEmpty() || path != cache_dir) {
    return;
  }

  QFileInfo info(filename);
  if (GetUuid().toString() != info.dir().dirName()) {
    return;
  }

  int64_t timestamp = info.completeBaseName().toLongLong();
  Invalidate(TimeRange(ToTime(timestamp), ToTime(timestamp + 1)));
}

void FrameHashCache::ProjectInvalidated(Project *p)
{
  if (GetProject() == p) {
    InvalidateAll();
  }
}

QString FrameHashCache::CachePathName(const int64_t &time) const
{
  return CachePathName(GetCacheDirectory(), GetUuid(), time);
}

QString FrameHashCache::CachePathName(const rational &time) const
{
  return CachePathName(GetCacheDirectory(), GetUuid(), time, timebase_);
}

QString FrameHashCache::CachePathName(const QString &cache_path, const QUuid &cache_id, const int64_t &time)
{
  QString filename = GetThisCacheDirectory(cache_path, cache_id).filePath(QString::number(time) + QString::fromLatin1(kCacheExtension));

  // Register that in some way this hash has been accessed
  if (DiskManager::instance()) {
    QMetaObject::invokeMethod(DiskManager::instance(), "Accessed", Q_ARG(QString, cache_path), Q_ARG(QString, filename));
  }

  return filename;
}

QString FrameHashCache::CachePathName(const QString &cache_path, const QUuid &cache_id, const rational &time, const rational &tb)
{
  return CachePathName(cache_path, cache_id, Timecode::time_to_timestamp(time, tb, Timecode::kRound));
}

bool FrameHashCache::SaveCacheFrame(const QString &filename, const FramePtr frame)
{
  // Ensure directory is created
  QDir cache_dir = QFileInfo(filename).dir();
  if (!FileFunctions::DirectoryIsValid(cache_dir)) {
    return false;
  }

  const PixelFormat format = frame->format();

  if (!IsValidCacheFormat(format)) {
    return false;
  }

  const int width = frame->width();
  const int height = frame->height();
  const int channel_count = frame->channel_count();
  const int bpc = VideoParams::GetBytesPerChannel(format);
  const int row_bytes = width * channel_count * bpc;
  const quint64 raw_size = quint64(row_bytes) * quint64(height);

  // Pack the (possibly row-padded) frame into a contiguous buffer. Only the
  // meaningful pixels are stored, so the cache file is portable across builds
  // with different alignment/padding.
  QByteArray raw(static_cast<int>(raw_size), Qt::Uninitialized);
  for (int y = 0; y < height; y++) {
    memcpy(raw.data() + static_cast<size_t>(y) * row_bytes,
           frame->const_data() + static_cast<size_t>(y) * frame->linesize_bytes(),
           row_bytes);
  }

  // LZ4-compress the pixels. Unlike the old EXR/DWA (float) and JPEG (integer)
  // caches, this is lossless, so a saved frame reads back bit-exact.
  const int compress_bound = LZ4_compressBound(static_cast<int>(raw_size));
  QByteArray compressed(compress_bound, Qt::Uninitialized);
  const int compressed_size = LZ4_compress_default(raw.constData(),
                                                   compressed.data(),
                                                   static_cast<int>(raw_size),
                                                   compress_bound);

  if (compressed_size <= 0) {
    qCritical() << "Failed to compress cache frame";
    return false;
  }

  QFile file(filename);
  if (!file.open(QFile::WriteOnly)) {
    qCritical() << "Failed to open cache frame for writing:" << filename;
    return false;
  }

  QDataStream stream(&file);
  stream.setByteOrder(QDataStream::LittleEndian);

  stream.writeRawData(kCacheMagic, 4);
  stream << kCacheVersion;
  stream << quint32(width);
  stream << quint32(height);
  stream << quint32(format);
  stream << quint32(channel_count);
  stream << quint32(frame->video_params().divider());
  stream << quint32(frame->video_params().pixel_aspect_ratio().numerator());
  stream << quint32(frame->video_params().pixel_aspect_ratio().denominator());
  stream << quint64(raw_size);
  stream << quint32(compressed_size);
  stream.writeRawData(compressed.constData(), compressed_size);

  return stream.status() == QDataStream::Ok;
}

}
