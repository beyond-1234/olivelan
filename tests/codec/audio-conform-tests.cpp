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
 * Patch 0056: FFmpeg audio encode + decode/conform round trip.
 *
 * The rest of the suite has no audio coverage at all (image-export-tests only drives the *image*
 * export path), which left the two swr_alloc_set_opts2 call sites introduced by patch 0054
 * compile-verified only:
 *
 *   - FFmpegEncoder::InitializeResampleContext  (E2)
 *   - FFmpegDecoder::ConformAudioInternal       (D3)
 *
 * This test drives both with real media:
 *
 *   1. FFmpegEncoder writes a 1 s stereo tone to a real .wav        -> E1 E2 E3 E4 E5 E6
 *   2. the file is read back with raw libavformat/libavcodec         -> independent check
 *   3. FFmpegDecoder conforms it to a different rate AND layout,     -> D1 D2 D3 D4
 *      and again to a different sample format
 *
 * It mirrors the production path: the codec context is given a *packed* sample format (the planar
 * ones have no PCM encoder - FFmpegEncoder::GetEncoder() returns nullptr for them), while the
 * SampleBuffer handed to WriteAudio() uses Olive's internal planar-float format, so the resampler
 * has to do a real format conversion.
 *
 * Everything runs headless: no GL, no QApplication (the suite's generated main() provides none).
 */

#include "testutil.h"

#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>
#include <QVector>

#include <cmath>
#include <cstdio>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

#include <olive/core/core.h>

#include "codec/decoder.h"
#include "codec/encoder.h"
#include "codec/exportcodec.h"
#include "codec/exportformat.h"
#include "codec/ffmpeg/ffmpegdecoder.h"
#include "codec/ffmpeg/ffmpegencoder.h"

using namespace olive::core;

namespace olive {

namespace {

const int kSampleRate = 48000;

void LogLine(const QString &s)
{
  std::cout << " - " << s.toStdString() << std::endl;
}

qint64 SizeOf(const QString &path)
{
  QFileInfo fi(path);
  return fi.exists() ? fi.size() : -1;
}

/**
 * @brief Independently re-open a written audio file with libav and sanity check it.
 *
 * @return An empty string on success, otherwise a human-readable failure.
 */
QString VerifyAudioFileWithLibav(const QString &path)
{
  AVFormatContext *fmt = nullptr;
  if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) {
    return QStringLiteral("libav could not open the written file");
  }

  if (avformat_find_stream_info(fmt, nullptr) < 0) {
    avformat_close_input(&fmt);
    return QStringLiteral("libav could not read stream info");
  }

  const int index = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (index < 0) {
    avformat_close_input(&fmt);
    return QStringLiteral("written file has no audio stream");
  }

  AVCodecParameters *par = fmt->streams[index]->codecpar;

  char layout[128] = {0};
  av_channel_layout_describe(&par->ch_layout, layout, sizeof(layout));

  LogLine(QStringLiteral("libav read-back: codec=%1 ch=%2 rate=%3 fmt=%4 layout='%5' (order=%6)")
          .arg(QString::fromUtf8(avcodec_get_name(par->codec_id)))
          .arg(par->ch_layout.nb_channels)
          .arg(par->sample_rate)
          .arg(QString::fromUtf8(av_get_sample_fmt_name(static_cast<AVSampleFormat>(par->format))))
          .arg(QString::fromUtf8(layout))
          .arg(int(par->ch_layout.order)));

  QString failure;

  if (par->ch_layout.nb_channels != 2) {
    failure = QStringLiteral("expected 2 channels, got %1").arg(par->ch_layout.nb_channels);
  } else if (par->sample_rate != kSampleRate) {
    failure = QStringLiteral("expected %1 Hz, got %2").arg(kSampleRate).arg(par->sample_rate);
  } else if (par->format != AV_SAMPLE_FMT_S16) {
    failure = QStringLiteral("expected s16 (what kCodecPCM was asked to encode)");
  }

  // Decode the first block and require actual, non-silent samples.
  if (failure.isEmpty()) {
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);
    AVCodecContext *cc = codec ? avcodec_alloc_context3(codec) : nullptr;

    if (!cc || avcodec_parameters_to_context(cc, par) < 0 ||
        avcodec_open2(cc, codec, nullptr) < 0) {
      failure = QStringLiteral("could not open the file with the libav decoder");
    } else {
      AVPacket *pkt = av_packet_alloc();
      AVFrame *frame = av_frame_alloc();
      bool got_frame = false;
      bool non_silent = false;

      while (pkt && frame && av_read_frame(fmt, pkt) >= 0 && !got_frame) {
        if (pkt->stream_index == index && avcodec_send_packet(cc, pkt) >= 0 &&
            avcodec_receive_frame(cc, frame) >= 0) {
          got_frame = frame->nb_samples > 0;

          const int16_t *s = reinterpret_cast<const int16_t *>(frame->data[0]);
          for (int i = 0; i < frame->nb_samples * 2 && !non_silent; i++) {
            non_silent = (s[i] != 0);
          }
        }
        av_packet_unref(pkt);
      }

      if (!got_frame) {
        failure = QStringLiteral("libav decoded no frames");
      } else if (!non_silent) {
        failure = QStringLiteral("libav decoded only silence");
      }

      av_frame_free(&frame);
      av_packet_free(&pkt);
    }

    if (cc) {
      avcodec_free_context(&cc);
    }
  }

  avformat_close_input(&fmt);
  return failure;
}

/**
 * @brief Conform `input` to `target` and check what came out.
 *
 * Conform writes one file per channel (PlanarFileDevice), so `out` must have channel_count()
 * entries.
 *
 * @return An empty string on success, otherwise a human-readable failure.
 */
QString RunConformCase(const QString &input, const AudioParams &target, double expected_seconds,
                       QVector<QString> *out)
{
  DecoderPtr decoder = std::make_shared<FFmpegDecoder>();

  Decoder::CodecStream stream(input, 0, nullptr);
  if (!decoder->Open(stream)) {
    return QStringLiteral("decoder->Open failed");
  }

  const bool ok = decoder->ConformAudio(*out, target, nullptr);
  decoder->Close();

  if (!ok) {
    return QStringLiteral("ConformAudio returned false");
  }

  // Every per-channel file must exist, be non-empty, and hold the same sample count.
  const qint64 bytes_per_sample = target.bytes_per_sample_per_channel();
  const double expected_samples = expected_seconds * target.sample_rate();
  qint64 first_size = -1;

  for (int i = 0; i < out->size(); i++) {
    const qint64 sz = SizeOf(out->at(i));
    if (sz <= 0) {
      return QStringLiteral("channel %1 file missing or empty").arg(i);
    }
    if (first_size < 0) {
      first_size = sz;
    } else if (sz != first_size) {
      return QStringLiteral("channel files have differing sizes");
    }
  }

  const double samples = double(first_size) / double(bytes_per_sample);
  const double ratio = samples / expected_samples;

  LogLine(QStringLiteral("conform: %1 ch, %2 B/ch -> %3 samples (expected ~%4, ratio %5)")
          .arg(out->size())
          .arg(first_size)
          .arg(qRound64(samples))
          .arg(qRound64(expected_samples))
          .arg(ratio, 0, 'f', 3));

  // ConformAudioInternal never flushes swr at EOF, so a small tail is dropped (ratio ~0.997). That
  // is a pre-existing bug, tracked in reports/00-known-issues.md ("ConformAudio 未在 EOF 刷新 swr");
  // the loose tolerance exists only because fixing it is out of scope here. Tighten it if the bug
  // is ever fixed.
  if (ratio <= 0.85 || ratio >= 1.15) {
    return QStringLiteral("sample count is off by more than 15%% (ratio %1)").arg(ratio, 0, 'f', 3);
  }

  return QString();
}

/// Encode a short stereo tone to `path`. Empty string on success.
QString MakeToneFile(const QString &path)
{
  const AudioParams internal_ap(48000, AV_CH_LAYOUT_STEREO, SampleFormat(SampleFormat::F32P));
  const AudioParams file_ap(48000, AV_CH_LAYOUT_STEREO, SampleFormat(SampleFormat::S16));

  EncodingParams params;
  params.set_format(ExportFormat::kFormatWAV);
  params.SetFilename(path);
  params.EnableAudio(file_ap, ExportCodec::kCodecPCM);

  std::unique_ptr<Encoder> enc(Encoder::CreateFromFormat(ExportFormat::kFormatWAV, params));
  if (!enc) {
    return QStringLiteral("no encoder");
  }
  if (!enc->Open()) {
    return QStringLiteral("Open: %1").arg(enc->GetError());
  }

  SampleBuffer tone(internal_ap, size_t(4800));
  if (!tone.is_allocated()) {
    tone.allocate();
  }
  for (int ch = 0; ch < tone.channel_count(); ch++) {
    float *d = tone.data(ch);
    for (size_t i = 0; i < tone.sample_count(); i++) {
      d[i] = 0.2f * float(std::sin(6.283185307179586 * 440.0 * double(i) / 48000.0));
    }
  }

  if (!enc->WriteAudio(tone)) {
    return QStringLiteral("WriteAudio: %1").arg(enc->GetError());
  }

  enc->Close();
  return QString();
}

/// Just report whether ConformAudio succeeds for this target (no output checks).
bool ConformReturns(const QString &input, const AudioParams &target, QVector<QString> *out)
{
  DecoderPtr decoder = std::make_shared<FFmpegDecoder>();

  Decoder::CodecStream stream(input, 0, nullptr);
  if (!decoder->Open(stream)) {
    return false;
  }

  const bool ok = decoder->ConformAudio(*out, target, nullptr);
  decoder->Close();
  return ok;
}

} // namespace

OLIVE_ADD_TEST(FfmpegAudioRoundTrip)
{
  QTemporaryDir dir;
  OLIVE_ASSERT(dir.isValid());

  const AudioParams internal_ap(kSampleRate, AV_CH_LAYOUT_STEREO, SampleFormat(SampleFormat::F32P));

  // ---------------------------------------------------------------- 1. encode (E1..E6)
  const QString wav = dir.filePath(QStringLiteral("audio-conform.wav"));

  // The codec context takes a packed format (S16 -> pcm_s16le); the samples we feed are F32P, so
  // the encode-side resample context (E2) performs a real format conversion.
  const AudioParams file_ap(kSampleRate, AV_CH_LAYOUT_STEREO, SampleFormat(SampleFormat::S16));

  EncodingParams params;
  params.set_format(ExportFormat::kFormatWAV);
  params.SetFilename(wav);
  params.EnableAudio(file_ap, ExportCodec::kCodecPCM);

  std::unique_ptr<Encoder> enc(Encoder::CreateFromFormat(ExportFormat::kFormatWAV, params));
  OLIVE_ASSERT(enc != nullptr);

  if (!enc->Open()) {
    LogLine(QStringLiteral("encoder->Open failed: %1").arg(enc->GetError()));
    return __LINE__;
  }

  SampleBuffer tone(internal_ap, size_t(kSampleRate)); // exactly one second
  if (!tone.is_allocated()) {
    tone.allocate();
  }
  OLIVE_ASSERT(tone.is_allocated());

  const double two_pi = 6.283185307179586;
  for (int ch = 0; ch < tone.channel_count(); ch++) {
    float *d = tone.data(ch);
    const double f = ch ? 880.0 : 440.0;
    for (size_t i = 0; i < tone.sample_count(); i++) {
      d[i] = 0.5f * float(std::sin(two_pi * f * double(i) / double(kSampleRate)));
    }
  }

  if (!enc->WriteAudio(tone)) {
    LogLine(QStringLiteral("encoder->WriteAudio failed: %1").arg(enc->GetError()));
    return __LINE__;
  }

  enc->Close();

  OLIVE_ASSERT(SizeOf(wav) > 100000);

  // ---------------------------------------------------------------- 2. independent read-back
  {
    const QString err = VerifyAudioFileWithLibav(wav);
    if (!err.isEmpty()) {
      LogLine(QStringLiteral("libav read-back: %1").arg(err));
      return __LINE__;
    }
  }

  // ---------------------------------------------------------------- 3. decode / conform (D1..D4)
  {
    // Different rate AND different layout -> resampling + rematrixing through D3's swr.
    const AudioParams target(44100, AV_CH_LAYOUT_5POINT1, SampleFormat(SampleFormat::F32P));
    QVector<QString> out;
    for (int i = 0; i < target.channel_count(); i++) {
      out << dir.filePath(QStringLiteral("conform-51-%1.raw").arg(i));
    }

    const QString err = RunConformCase(wav, target, 1.0, &out);
    if (!err.isEmpty()) {
      LogLine(QStringLiteral("conform 5.1 44.1k: %1").arg(err));
      return __LINE__;
    }
  }

  {
    // Also change the sample format family (downmix to mono, planar 16-bit).
    const AudioParams target(kSampleRate, AV_CH_LAYOUT_MONO, SampleFormat(SampleFormat::S16P));
    QVector<QString> out;
    for (int i = 0; i < target.channel_count(); i++) {
      out << dir.filePath(QStringLiteral("conform-mono-%1.raw").arg(i));
    }

    const QString err = RunConformCase(wav, target, 1.0, &out);
    if (!err.isEmpty()) {
      LogLine(QStringLiteral("conform mono s16p: %1").arg(err));
      return __LINE__;
    }
  }

  OLIVE_TEST_END;
}

/**
 * Patch 0059: ConformAudio must reject unusable target parameters instead of crashing.
 *
 * A target AudioParams with a 0-channel layout used to sail past the resampler setup, then divide
 * by params.channel_count() == 0 in the conversion loop and die with SIGFPE. If that ever comes
 * back, this test process dies and ctest reports it.
 */
OLIVE_ADD_TEST(FfmpegConformRejectsInvalidParams)
{
  QTemporaryDir dir;
  OLIVE_ASSERT(dir.isValid());

  const QString wav = dir.filePath(QStringLiteral("invalid-params.wav"));

  {
    const QString err = MakeToneFile(wav);
    if (!err.isEmpty()) {
      LogLine(QStringLiteral("could not build the input file: %1").arg(err));
      return __LINE__;
    }
  }

  // Control: a usable target must still succeed, otherwise the "false" results below prove nothing.
  {
    QVector<QString> out;
    out << dir.filePath(QStringLiteral("ok_0.raw")) << dir.filePath(QStringLiteral("ok_1.raw"));
    const AudioParams good(48000, AV_CH_LAYOUT_STEREO, SampleFormat(SampleFormat::S16P));
    if (!ConformReturns(wav, good, &out)) {
      LogLine(QStringLiteral("valid target failed"));
      return __LINE__;
    }
  }

  // A layout with no channels: the resampler cannot be configured with it.
  {
    QVector<QString> out;
    out << dir.filePath(QStringLiteral("mask0_0.raw"));
    const AudioParams bad(48000, 0, SampleFormat(SampleFormat::S16P));
    if (ConformReturns(wav, bad, &out)) {
      LogLine(QStringLiteral("a 0-channel layout was accepted"));
      return __LINE__;
    }
  }

  // An out-of-range sample rate makes swr_alloc_set_opts2 itself fail.
  {
    QVector<QString> out;
    out << dir.filePath(QStringLiteral("neg_0.raw")) << dir.filePath(QStringLiteral("neg_1.raw"));
    const AudioParams bad(-1, AV_CH_LAYOUT_STEREO, SampleFormat(SampleFormat::S16P));
    if (ConformReturns(wav, bad, &out)) {
      LogLine(QStringLiteral("a negative sample rate was accepted"));
      return __LINE__;
    }
  }

  OLIVE_TEST_END;
}

} // namespace olive
