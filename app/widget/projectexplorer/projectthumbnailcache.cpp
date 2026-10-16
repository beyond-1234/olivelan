/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2024 Olive Team

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

#include "projectthumbnailcache.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
}

#include <QDebug>
#include <QFile>
#include <QImage>
#include <QThreadPool>

#include <utility>

namespace olive {

namespace {

/**
 * @brief Decode the first video frame of `filename` and scale it to `out_w` x `out_h` RGBA.
 *
 * Runs on a worker thread and uses libav* directly (rather than Olive's Decoder/Renderer stack) so
 * that it needs no OpenGL context. Returns a null QImage for audio-only or unreadable files.
 */
QImage DecodeFirstFrame(const QString &filename, int out_w, int out_h)
{
  QImage result;

  const QByteArray fn = QFile::encodeName(filename);

  AVFormatContext *format_ctx = nullptr;
  if (avformat_open_input(&format_ctx, fn.constData(), nullptr, nullptr) < 0) {
    return result;
  }

  if (avformat_find_stream_info(format_ctx, nullptr) < 0) {
    avformat_close_input(&format_ctx);
    return result;
  }

  const int video_stream = av_find_best_stream(format_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (video_stream < 0) {
    avformat_close_input(&format_ctx);
    return result;
  }

  const AVCodec *codec = avcodec_find_decoder(format_ctx->streams[video_stream]->codecpar->codec_id);
  if (!codec) {
    avformat_close_input(&format_ctx);
    return result;
  }

  AVCodecContext *codec_ctx = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(codec_ctx, format_ctx->streams[video_stream]->codecpar);

  if (avcodec_open2(codec_ctx, codec, nullptr) < 0) {
    avcodec_free_context(&codec_ctx);
    avformat_close_input(&format_ctx);
    return result;
  }

  AVPacket *packet = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  bool got_frame = false;

  while (!got_frame && av_read_frame(format_ctx, packet) >= 0) {
    if (packet->stream_index == video_stream) {
      if (avcodec_send_packet(codec_ctx, packet) >= 0
          && avcodec_receive_frame(codec_ctx, frame) >= 0) {
        got_frame = true;
      }
    }

    av_packet_unref(packet);
  }

  // Flush the decoder in case the first frame was buffered.
  if (!got_frame && avcodec_send_packet(codec_ctx, nullptr) >= 0) {
    got_frame = avcodec_receive_frame(codec_ctx, frame) >= 0;
  }

  if (got_frame && frame->width > 0 && frame->height > 0) {
    SwsContext *sws = sws_getContext(frame->width, frame->height,
                                     static_cast<AVPixelFormat>(frame->format),
                                     out_w, out_h, AV_PIX_FMT_RGBA,
                                     SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (sws) {
      QImage image(out_w, out_h, QImage::Format_RGBA8888);
      uint8_t *dst_data[4] = { image.bits(), nullptr, nullptr, nullptr };
      int dst_linesize[4] = { static_cast<int>(image.bytesPerLine()), 0, 0, 0 };

      sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst_data, dst_linesize);

      sws_freeContext(sws);

      result = image;
    }
  }

  av_frame_free(&frame);
  av_packet_free(&packet);
  avcodec_free_context(&codec_ctx);
  avformat_close_input(&format_ctx);

  return result;
}

}

/**
 * @brief Pool runnable that decodes a single thumbnail and posts it back to the cache.
 *
 * A plain QRunnable (not a QObject) so AUTOMOC isn't involved; the result is delivered onto the
 * GUI thread via a queued functor invocation.
 */
class ProjectThumbnailWorker : public QRunnable
{
public:
  ProjectThumbnailWorker(ProjectThumbnailCache *cache, QString key, QString filename) :
    cache_(cache),
    key_(std::move(key)),
    filename_(std::move(filename))
  {
    setAutoDelete(true);
  }

  void run() override
  {
    const QImage image = DecodeFirstFrame(filename_,
                                          ProjectThumbnailCache::kThumbnailWidth,
                                          ProjectThumbnailCache::kThumbnailHeight);
    qDebug() << "[Thumbnail] decoded" << filename_ << (image.isNull() ? "(no video)" : "");

    ProjectThumbnailCache *cache = cache_;
    const QString key = key_;
    QMetaObject::invokeMethod(cache, [cache, key, image]() { cache->OnWorkerDone(key, image); },
                              Qt::QueuedConnection);
  }

private:
  ProjectThumbnailCache *cache_;
  QString key_;
  QString filename_;
};

ProjectThumbnailCache::ProjectThumbnailCache(QObject *parent) :
  QObject(parent)
{
}

ProjectThumbnailCache::~ProjectThumbnailCache()
{
  // Don't let in-flight workers outlive us (they post results back into this object).
  QThreadPool::globalInstance()->waitForDone();
}

QPixmap ProjectThumbnailCache::Get(const QString &key) const
{
  auto it = cache_.constFind(key);
  if (it == cache_.constEnd()) {
    return QPixmap();
  }

  lru_.removeOne(key);
  lru_.prepend(key);

  return it.value();
}

void ProjectThumbnailCache::Request(const QString &key, const QString &filename)
{
  if (key.isEmpty() || filename.isEmpty() || cache_.contains(key) || in_flight_.contains(key)) {
    return;
  }

  in_flight_.insert(key);

  QThreadPool::globalInstance()->start(new ProjectThumbnailWorker(this, key, filename));
}

void ProjectThumbnailCache::OnWorkerDone(const QString &key, const QImage &image)
{
  in_flight_.remove(key);

  if (image.isNull()) {
    return;
  }

  Insert(key, QPixmap::fromImage(image));

  emit Ready(key);
}

void ProjectThumbnailCache::Insert(const QString &key, const QPixmap &pixmap)
{
  cache_.insert(key, pixmap);
  lru_.removeOne(key);
  lru_.prepend(key);

  while (lru_.size() > kMaxEntries) {
    cache_.remove(lru_.takeLast());
  }
}

}
