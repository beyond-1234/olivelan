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
 * Patch 0058: interlaced encoding must survive a round trip.
 *
 * Patch 0054 replaced the removed AVFrame::interlaced_frame / top_field_first reads with
 * AV_FRAME_FLAG_* in FFmpegDecoder::Probe(). Verifying that (patch 0058's sibling work) surfaced a
 * separate, pre-existing encoder bug: requesting top-field-first produced bottom-field-first.
 *
 * Root cause: libavcodec's libx264 wrapper never reads AVCodecContext::field_order - it derives
 * b_tff per frame from AV_FRAME_FLAG_TOP_FIELD_FIRST (libavcodec/libx264.c). Olive set field_order
 * and, for bottom-first only, an "x264opts bff=1" hack, so bottom-first happened to work while
 * top-first silently became bottom-first. WriteMovieFrame() now flags the frames it submits.
 *
 * This test encodes the same content three ways and requires FFmpegDecoder::Probe() to report back
 * exactly what was requested. The progressive case is the control.
 *
 * Media is produced by Olive's own FFmpegEncoder, so nothing external is needed and no assets are
 * committed. Headless: no GL, no QApplication.
 */

#include "testutil.h"

#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <olive/core/core.h>

#include "codec/decoder.h"
#include "codec/encoder.h"
#include "codec/exportcodec.h"
#include "codec/exportformat.h"
#include "codec/frame.h"
#include "codec/ffmpeg/ffmpegdecoder.h"
#include "codec/ffmpeg/ffmpegencoder.h"
#include "node/project/footage/footagedescription.h"
#include "render/videoparams.h"

using namespace olive::core;

namespace olive {

namespace {

const int kWidth = 64;
const int kHeight = 64;
const int kFrames = 5;

void LogLine(const QString &s)
{
  std::cout << " - " << s.toStdString() << std::endl;
}

const char *InterlaceName(VideoParams::Interlacing i)
{
  switch (i) {
  case VideoParams::kInterlaceNone:
    return "none";
  case VideoParams::kInterlacedTopFirst:
    return "tff";
  case VideoParams::kInterlacedBottomFirst:
    return "bff";
  }
  return "?";
}

FramePtr MakeFrame(const VideoParams &vp)
{
  FramePtr f = Frame::Create();
  f->set_video_params(vp);

  if (!f->allocate()) {
    return nullptr;
  }

  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < kWidth; x++) {
      const float v = ((x + y) % 16) / 15.0f;
      f->set_pixel(x, y, Color(v, 1.0f - v, 0.5f));
    }
  }

  return f;
}

/**
 * @brief Encode kFrames at the given interlacing mode.
 *
 * @return An empty string on success, otherwise a human-readable failure.
 */
QString EncodeFile(const QString &path, VideoParams::Interlacing interlacing)
{
  VideoParams vp(kWidth, kHeight, rational(1, 25), PixelFormat::U8, VideoParams::kRGBAChannelCount);
  vp.set_color_range(VideoParams::kColorRangeFull);
  vp.set_interlacing(interlacing);

  EncodingParams params;
  params.set_format(ExportFormat::kFormatMatroska);
  params.SetFilename(path);
  params.set_video_pix_fmt(QStringLiteral("yuv420p"));
  params.EnableVideo(vp, ExportCodec::kCodecH264);

  std::unique_ptr<Encoder> enc(Encoder::CreateFromFormat(ExportFormat::kFormatMatroska, params));
  if (!enc) {
    return QStringLiteral("no encoder");
  }

  if (!enc->Open()) {
    return QStringLiteral("Open: %1").arg(enc->GetError());
  }

  for (int i = 0; i < kFrames; i++) {
    FramePtr f = MakeFrame(vp);
    if (!f || !enc->WriteFrame(f, rational(i, 25))) {
      return QStringLiteral("WriteFrame(%1): %2").arg(i).arg(enc->GetError());
    }
  }

  enc->Close();
  return QString();
}

/// Decode the first frame with raw libav and return its interlace flags (0 on failure).
int ReadFirstFrameFlags(const QString &path, bool *ok)
{
  *ok = false;
  int flags = 0;

  AVFormatContext *fmt = nullptr;
  if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) {
    return 0;
  }

  if (avformat_find_stream_info(fmt, nullptr) >= 0) {
    const int index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (index >= 0) {
      AVCodecParameters *par = fmt->streams[index]->codecpar;
      const AVCodec *codec = avcodec_find_decoder(par->codec_id);
      AVCodecContext *cc = codec ? avcodec_alloc_context3(codec) : nullptr;

      if (cc && avcodec_parameters_to_context(cc, par) >= 0 &&
          avcodec_open2(cc, codec, nullptr) >= 0) {
        AVPacket *pkt = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();

        while (pkt && frame && av_read_frame(fmt, pkt) >= 0) {
          if (pkt->stream_index == index && avcodec_send_packet(cc, pkt) >= 0 &&
              avcodec_receive_frame(cc, frame) >= 0) {
            flags = frame->flags;
            *ok = true;
            break;
          }
          av_packet_unref(pkt);
        }

        av_frame_free(&frame);
        av_packet_free(&pkt);
      }

      if (cc) {
        avcodec_free_context(&cc);
      }
    }
  }

  avformat_close_input(&fmt);
  return flags;
}

/// What does FFmpegDecoder::Probe() think the file is?
bool ProbeInterlacing(const QString &path, VideoParams::Interlacing *out, QString *error)
{
  FFmpegDecoder decoder;
  const FootageDescription desc = decoder.Probe(path, nullptr);

  if (desc.GetVideoStreams().isEmpty()) {
    *error = QStringLiteral("Probe() returned no video streams");
    return false;
  }

  *out = desc.GetVideoStreams().first().interlacing();
  return true;
}

/**
 * @brief Encode at `requested` and require the whole chain to agree on it.
 *
 * @return An empty string on success, otherwise a human-readable failure.
 */
QString RunCase(VideoParams::Interlacing requested, const QString &dir_path)
{
  const QString path =
      QStringLiteral("%1/%2.mkv").arg(dir_path).arg(QString::fromUtf8(InterlaceName(requested)));

  QString err = EncodeFile(path, requested);
  if (!err.isEmpty()) {
    return QStringLiteral("encode(%1): %2").arg(QString::fromUtf8(InterlaceName(requested))).arg(err);
  }

  if (QFileInfo(path).size() <= 0) {
    return QStringLiteral("encoder produced an empty file");
  }

  bool flags_ok = false;
  const int flags = ReadFirstFrameFlags(path, &flags_ok);
  if (!flags_ok) {
    return QStringLiteral("could not decode the first frame with libav");
  }

  const bool dec_interlaced = flags & AV_FRAME_FLAG_INTERLACED;
  const bool dec_tff = flags & AV_FRAME_FLAG_TOP_FIELD_FIRST;

  LogLine(QStringLiteral("%1: frame flags=0x%2 (INTERLACED=%3 TOP_FIELD_FIRST=%4)")
          .arg(QString::fromUtf8(InterlaceName(requested)))
          .arg(flags, 0, 16)
          .arg(dec_interlaced ? 1 : 0)
          .arg(dec_tff ? 1 : 0));

  VideoParams::Interlacing detected = VideoParams::kInterlaceNone;
  if (!ProbeInterlacing(path, &detected, &err)) {
    return QStringLiteral("Probe(): %1").arg(err);
  }

  if (detected != requested) {
    return QStringLiteral("requested '%1' but the round trip reported '%2'")
        .arg(QString::fromUtf8(InterlaceName(requested)))
        .arg(QString::fromUtf8(InterlaceName(detected)));
  }

  return QString();
}

} // namespace

OLIVE_ADD_TEST(FfmpegInterlaceRoundTrip)
{
  QTemporaryDir dir;
  OLIVE_ASSERT(dir.isValid());

  // Bottom-first, top-first, and the progressive control.
  const VideoParams::Interlacing cases[] = {
    VideoParams::kInterlacedBottomFirst,
    VideoParams::kInterlacedTopFirst,
    VideoParams::kInterlaceNone,
  };

  for (VideoParams::Interlacing c : cases) {
    const QString err = RunCase(c, dir.path());
    if (!err.isEmpty()) {
      LogLine(err);
      return __LINE__;
    }
  }

  OLIVE_TEST_END;
}

} // namespace olive
