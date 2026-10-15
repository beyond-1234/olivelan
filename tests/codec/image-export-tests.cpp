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
 * Phase 7 / patch 0032: FFmpeg still-image and image-sequence export probe.
 *
 * Patch 0032 reroutes PNG/TIFF/EXR export away from the OIIO encoder and onto
 * FFmpegEncoder (which gains a one-file-per-frame mode), and registers
 * FFmpegDecoder as the sole image decoder. This test drives the real
 * FFmpegEncoder end to end:
 *
 *   1. Encoder::GetTypeFromFormat() must report kEncoderTypeFFmpeg for the
 *      three still formats, and CreateFromFormat() must yield a FFmpegEncoder.
 *   2. A three-frame PNG image sequence must yield frame_0000/0001/0002.png and
 *      a single-frame TIFF / EXR must each yield exactly one valid file.
 *   3. Every written file is reopened with libavformat/libavcodec and both its
 *      dimensions and top-left pixel are checked against what was written.
 *
 * Read-back deliberately avoids Olive's Decoder API (which uploads to a GL
 * texture) so the test runs headless on a build machine.
 */

#include "testutil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <QtGlobal>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
}

#include "codec/decoder.h"
#include "codec/encoder.h"
#include "codec/exportcodec.h"
#include "codec/exportformat.h"
#include "codec/ffmpeg/ffmpegdecoder.h"
#include "codec/ffmpeg/ffmpegencoder.h"
#include "codec/frame.h"
#include "render/videoparams.h"

using namespace olive::core;

namespace olive {

namespace {

const int kWidth = 8;
const int kHeight = 8;

struct ImageCase {
  const char *label;
  ExportFormat::Format format;
  ExportCodec::Codec codec;
  const char *extension;
  const char *pix_fmt;
  bool sequence;
};

/**
 * @brief Decode the top-left pixel of a written image file.
 *
 * Fills rgba[0..3] with 8-bit R/G/B/A. Returns false if the file could not be
 * opened or decoded at all.
 */
bool ProbeImage(const QString &filename, int *width, int *height, int *rgba)
{
  AVFormatContext *fmt = nullptr;
  if (avformat_open_input(&fmt, filename.toUtf8().constData(), nullptr, nullptr) < 0) {
    return false;
  }

  bool ok = false;

  if (avformat_find_stream_info(fmt, nullptr) >= 0) {
    const int stream_index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    if (stream_index >= 0) {
      AVStream *stream = fmt->streams[stream_index];
      *width = stream->codecpar->width;
      *height = stream->codecpar->height;

      const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
      AVCodecContext *cc = codec ? avcodec_alloc_context3(codec) : nullptr;

      if (cc && avcodec_parameters_to_context(cc, stream->codecpar) >= 0 &&
          avcodec_open2(cc, codec, nullptr) >= 0) {
        AVPacket *pkt = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();

        if (pkt && frame && av_read_frame(fmt, pkt) >= 0 &&
            avcodec_send_packet(cc, pkt) >= 0 &&
            avcodec_receive_frame(cc, frame) >= 0) {
          switch (frame->format) {
          case AV_PIX_FMT_RGBA:
            rgba[0] = frame->data[0][0];
            rgba[1] = frame->data[0][1];
            rgba[2] = frame->data[0][2];
            rgba[3] = frame->data[0][3];
            ok = true;
            break;
          case AV_PIX_FMT_RGB24:
            rgba[0] = frame->data[0][0];
            rgba[1] = frame->data[0][1];
            rgba[2] = frame->data[0][2];
            rgba[3] = 255;
            ok = true;
            break;
          case AV_PIX_FMT_GBRAPF32LE:
          case AV_PIX_FMT_GBRPF32LE: {
            // Planar GBR: plane 0 = G, plane 1 = B, plane 2 = R (and plane 3 = A).
            const float *g = reinterpret_cast<const float *>(frame->data[0]);
            const float *b = reinterpret_cast<const float *>(frame->data[1]);
            const float *r = reinterpret_cast<const float *>(frame->data[2]);
            rgba[0] = int(r[0] * 255.0f + 0.5f);
            rgba[1] = int(g[0] * 255.0f + 0.5f);
            rgba[2] = int(b[0] * 255.0f + 0.5f);
            rgba[3] = 255;
            ok = true;
            break;
          }
          default:
            break;
          }
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
  return ok;
}

VideoParams MakeVideoParams()
{
  // An explicit time base is required: FFmpeg's image encoders reject an unset
  // encoder timebase. This mirrors a real export, where the sequence's frame
  // rate supplies it.
  VideoParams vp(kWidth, kHeight, rational(1, 25), PixelFormat::U8, VideoParams::kRGBAChannelCount);
  vp.set_color_range(VideoParams::kColorRangeFull);
  return vp;
}

FramePtr MakeSolidFrame(const Color &c)
{
  FramePtr f = Frame::Create();
  f->set_video_params(MakeVideoParams());

  if (!f->allocate()) {
    return nullptr;
  }

  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < kWidth; x++) {
      f->set_pixel(x, y, c);
    }
  }

  return f;
}

/**
 * @brief Run one image export case end to end.
 *
 * @return An empty string on success, otherwise a human-readable failure.
 */
QString RunCase(const ImageCase &c)
{
  QTemporaryDir dir;
  if (!dir.isValid()) {
    return QStringLiteral("could not create temp dir");
  }

  const QString base = c.sequence
                           ? dir.filePath(QStringLiteral("frame_[####].%1").arg(c.extension))
                           : dir.filePath(QStringLiteral("still.%1").arg(c.extension));

  EncodingParams params;
  params.set_format(c.format);
  params.SetFilename(base);
  params.set_video_is_image_sequence(c.sequence);
  params.set_video_pix_fmt(QString::fromUtf8(c.pix_fmt));
  params.EnableVideo(MakeVideoParams(), c.codec);

  if (Encoder::GetTypeFromFormat(c.format) != Encoder::kEncoderTypeFFmpeg) {
    return QStringLiteral("%1 is not routed to FFmpeg").arg(c.label);
  }

  Encoder *enc = Encoder::CreateFromFormat(c.format, params);
  if (!enc) {
    return QStringLiteral("%1: no encoder").arg(c.label);
  }

  if (!dynamic_cast<FFmpegEncoder *>(enc)) {
    delete enc;
    return QStringLiteral("%1: encoder is not a FFmpegEncoder").arg(c.label);
  }

  if (!enc->Open()) {
    const QString e = enc->GetError();
    delete enc;
    return QStringLiteral("%1: Open() failed: %2").arg(c.label, e);
  }

  const Color colors[] = {
    Color(1.0f, 0.0f, 0.0f, 1.0f),
    Color(0.0f, 1.0f, 0.0f, 1.0f),
    Color(0.0f, 0.0f, 1.0f, 1.0f)
  };
  const int frame_count = c.sequence ? 3 : 1;

  for (int i = 0; i < frame_count; i++) {
    FramePtr frame = MakeSolidFrame(colors[i]);
    if (!frame) {
      delete enc;
      return QStringLiteral("%1: frame %2 allocation failed").arg(c.label).arg(i);
    }

    frame->set_timestamp(rational(i, 25));

    if (!enc->WriteFrame(frame, rational(i, 25))) {
      const QString e = enc->GetError();
      delete enc;
      return QStringLiteral("%1: WriteFrame(%2) failed: %3").arg(c.label).arg(i).arg(e);
    }
  }

  enc->Close();
  delete enc;

  for (int i = 0; i < frame_count; i++) {
    const QString file = c.sequence
                             ? dir.filePath(QStringLiteral("frame_%1.%2").arg(i, 4, 10, QChar('0')).arg(c.extension))
                             : dir.filePath(QStringLiteral("still.%1").arg(c.extension));

    if (!QFile::exists(file) || QFileInfo(file).size() == 0) {
      return QStringLiteral("%1: missing or empty output %2").arg(c.label, file);
    }

    int w = 0, h = 0, rgba[4] = {0, 0, 0, 0};
    if (!ProbeImage(file, &w, &h, rgba)) {
      return QStringLiteral("%1: could not decode %2").arg(c.label, file);
    }

    if (w != kWidth || h != kHeight) {
      return QStringLiteral("%1: %2 is %3x%4, expected %5x%6")
          .arg(c.label, file).arg(w).arg(h).arg(kWidth).arg(kHeight);
    }

    const int expected[4] = {
      int(colors[i].red() * 255.0f + 0.5f),
      int(colors[i].green() * 255.0f + 0.5f),
      int(colors[i].blue() * 255.0f + 0.5f),
      255
    };

    for (int ch = 0; ch < 4; ch++) {
      if (qAbs(rgba[ch] - expected[ch]) > 3) {
        return QStringLiteral("%1: %2 channel %3 = %4, expected %5")
            .arg(c.label, file).arg(ch).arg(rgba[ch]).arg(expected[ch]);
      }
    }
  }

  return QString();
}

const ImageCase kCases[] = {
  { "PNG sequence", ExportFormat::kFormatPNG,     ExportCodec::kCodecPNG,     "png",  "rgba",       true  },
  { "TIFF still",   ExportFormat::kFormatTIFF,    ExportCodec::kCodecTIFF,    "tiff", "rgba",       false },
  { "EXR still",    ExportFormat::kFormatOpenEXR, ExportCodec::kCodecOpenEXR, "exr",  "gbrapf32le", false },
};

/**
 * Phase 8 / patch 0047: 10-bit export probe.
 *
 * The built-in colour engine (0039~0047) works in float; any export path that
 * quantises to 8 bits silently destroys the HDR headroom the BT.2408 roll-off is
 * meant to preserve. This case drives FFmpegEncoder end to end with PixelFormat::U16
 * (10-bit values left-shifted into the top bits of a 16-bit container, exactly what
 * an actual 10-bit pipeline produces) and asserts the decoded values come back with
 * no more rounding than the 10-bit quantum itself.
 *
 * Read-back reuses ProbeImage's libavformat/libavcodec path (headless-safe).
 */

const int kTenBitValue = 512; // mid-grey on a 10-bit scale (of 1023)

/**
 * @brief Decode the top-left pixel as native 16-bit samples.
 *
 * Returns false if the file could not be opened or decoded at all.
 */
bool ProbeImage16(const QString &filename, int *width, int *height, int64_t *rgba16)
{
  AVFormatContext *fmt = nullptr;
  if (avformat_open_input(&fmt, filename.toUtf8().constData(), nullptr, nullptr) < 0) {
    return false;
  }

  bool ok = false;

  if (avformat_find_stream_info(fmt, nullptr) >= 0) {
    const int stream_index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);

    if (stream_index >= 0) {
      AVStream *stream = fmt->streams[stream_index];
      *width = stream->codecpar->width;
      *height = stream->codecpar->height;

      const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
      AVCodecContext *cc = codec ? avcodec_alloc_context3(codec) : nullptr;

      if (cc && avcodec_parameters_to_context(cc, stream->codecpar) >= 0 &&
          avcodec_open2(cc, codec, nullptr) >= 0) {
        AVPacket *pkt = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();

        if (pkt && frame && av_read_frame(fmt, pkt) >= 0 &&
            avcodec_send_packet(cc, pkt) >= 0 &&
            avcodec_receive_frame(cc, frame) >= 0) {
          switch (frame->format) {
          case AV_PIX_FMT_RGB48LE:
          case AV_PIX_FMT_RGB48BE:
          case AV_PIX_FMT_RGBA64LE:
          case AV_PIX_FMT_RGBA64BE: {
            const int bytes_per_sample = (frame->format == AV_PIX_FMT_RGB48LE
                                          || frame->format == AV_PIX_FMT_RGB48BE) ? 2 : 2;
            const uint16_t *s = reinterpret_cast<const uint16_t *>(frame->data[0]);
            const int ncomp = (frame->format == AV_PIX_FMT_RGB48LE
                               || frame->format == AV_PIX_FMT_RGB48BE) ? 3 : 4;
            for (int i = 0; i < ncomp; i++) {
              uint16_t v = s[i];
              if (QSysInfo::ByteOrder == QSysInfo::LittleEndian
                  && (frame->format == AV_PIX_FMT_RGB48BE
                      || frame->format == AV_PIX_FMT_RGBA64BE)) {
                v = ((v & 0xFF) << 8) | (v >> 8);
              }
              rgba16[i] = v;
            }
            if (ncomp == 3) {
              rgba16[3] = 65535;
            }
            (void) bytes_per_sample;
            ok = true;
            break;
          }
          default:
            break;
          }
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
  return ok;
}

QString RunTenBitCase()
{
  QTemporaryDir dir;
  if (!dir.isValid()) {
    return QStringLiteral("could not create temp dir");
  }

  const QString file = dir.filePath(QStringLiteral("tenbit.png"));

  EncodingParams params;
  params.set_format(ExportFormat::kFormatPNG);
  params.SetFilename(file);
  params.set_video_is_image_sequence(false);
  params.set_video_pix_fmt(QStringLiteral("rgb48be"));
  params.EnableVideo(VideoParams(kWidth, kHeight, rational(1, 25), PixelFormat::U16,
                                 VideoParams::kRGBChannelCount),
                     ExportCodec::kCodecPNG);

  if (Encoder::GetTypeFromFormat(ExportFormat::kFormatPNG) != Encoder::kEncoderTypeFFmpeg) {
    return QStringLiteral("PNG is not routed to FFmpeg");
  }

  Encoder *enc = Encoder::CreateFromFormat(ExportFormat::kFormatPNG, params);
  if (!enc) {
    return QStringLiteral("no encoder");
  }

  if (!dynamic_cast<FFmpegEncoder *>(enc)) {
    delete enc;
    return QStringLiteral("encoder is not a FFmpegEncoder");
  }

  if (!enc->Open()) {
    const QString e = enc->GetError();
    delete enc;
    return QStringLiteral("Open() failed: %1").arg(e);
  }

  // 10-bit 512/1023 expressed in the U16 container (<< 6) and normalised to the
  // float 0..1 range the Frame API takes.
  const float v = float((kTenBitValue << 6) / 65535.0);
  FramePtr frame = Frame::Create();
  frame->set_video_params(VideoParams(kWidth, kHeight, rational(1, 25), PixelFormat::U16,
                                      VideoParams::kRGBChannelCount));
  if (!frame->allocate()) {
    delete enc;
    return QStringLiteral("frame allocation failed");
  }

  // Write via the U8 path? No: allocate() honouring U16 means data is uint16_t.
  for (int y = 0; y < kHeight; y++) {
    for (int x = 0; x < kWidth; x++) {
      uint16_t *px = reinterpret_cast<uint16_t *>(
          frame->data() + y * frame->linesize_pixels() * 2 + x * 2 * 3);
      px[0] = px[1] = px[2] = uint16_t(kTenBitValue << 6);
    }
  }

  frame->set_timestamp(rational(0));

  if (!enc->WriteFrame(frame, rational(0))) {
    const QString e = enc->GetError();
    delete enc;
    return QStringLiteral("WriteFrame failed: %1").arg(e);
  }

  enc->Close();
  delete enc;

  if (!QFile::exists(file) || QFileInfo(file).size() == 0) {
    return QStringLiteral("missing or empty output %1").arg(file);
  }

  int w = 0, h = 0;
  int64_t rgba16[4] = {0, 0, 0, 0};
  if (!ProbeImage16(file, &w, &h, rgba16)) {
    return QStringLiteral("could not decode %1").arg(file);
  }

  if (w != kWidth || h != kHeight) {
    return QStringLiteral("%1 is %2x%3, expected %4x%5").arg(file).arg(w).arg(h).arg(kWidth).arg(kHeight);
  }

  // 16-bit stored value must equal 512 << 6 = 32768 exactly (or ±1 LSB of 16-bit
  // for encoder rounding - far tighter than any 8-bit path could achieve).
  const int64_t expected = int64_t(kTenBitValue) << 6;
  for (int ch = 0; ch < 3; ch++) {
    if (qAbs(rgba16[ch] - expected) > 1) {
      return QStringLiteral("channel %1 = %2, expected %3 (10-bit precision lost)")
          .arg(ch).arg(rgba16[ch]).arg(expected);
    }
  }

  return QString();
}

} // namespace

OLIVE_ADD_TEST(FfmpegImageExport)
{
  // 0032 also drops the OIIO decoder from the probe list; FFmpegDecoder must now
  // be the one and only image decoder.
  const QVector<DecoderPtr> decoders = Decoder::ReceiveListOfAllDecoders();
  if (decoders.size() != 1 || !dynamic_cast<FFmpegDecoder *>(decoders.first().get())) {
    std::cout << " - image decoder list is not FFmpeg-only";
    return __LINE__;
  }

  for (const ImageCase &c : kCases) {
    const QString err = RunCase(c);
    if (!err.isEmpty()) {
      std::cout << " - " << err.toStdString();
      return __LINE__;
    }
  }

  // Patch 0047: 10-bit export must survive the round trip without collapsing to 8 bits.
  {
    const QString err = RunTenBitCase();
    if (!err.isEmpty()) {
      std::cout << " - [TenBitExport] " << err.toStdString();
      return __LINE__;
    }
  }

  OLIVE_TEST_END;
}

} // namespace olive
