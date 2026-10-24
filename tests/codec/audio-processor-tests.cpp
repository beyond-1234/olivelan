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
 * Patch 0064: AudioProcessor must survive *repeated* Convert() pushes.
 *
 * Patch 0061 fixed a regression that patch 0054 had introduced here: after the migration stopped
 * setting AVFrame::channels, FFmpeg 6.x's abuffersrc (which compares frame->channel_layout against
 * frame->channels on every push, and zeroes channel_layout on the frame it consumes) rejected the
 * frame from the *second* Convert() onwards with EINVAL (-22), so timeline audio went silent.
 *
 * Nothing in the suite called Convert() at all, so the defect only surfaced in a real GUI session.
 * The one-off probe used while verifying 0054 missed it as well, because it pushed a single real
 * frame before draining - exactly the shape of coverage that hides this class of bug.
 *
 * This test therefore pushes three real frames through each configuration and requires every push
 * to succeed. Both halves matter: pushing more than once, and doing it across the filter
 * configurations that AudioProcessor builds (identical params, format-converting, resampling).
 *
 * Headless: no GL, no QApplication (the suite's generated main() provides none).
 */

#include "testutil.h"

#include <QString>
#include <QVector>

#include <cmath>
#include <cstdio>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
}

#include <olive/core/core.h>

#include "audio/audioprocessor.h"

using namespace olive::core;

namespace olive {

namespace {

const int kSampleRate = 48000;
const int kSamples = 4096;
const int kFrames = 3; // 0061 only manifested from the second push onwards

void LogLine(const QString &s)
{
  std::cout << " - " << s.toStdString() << std::endl;
}

/// Fill `channels` planar float buffers with a continuous tone across frames.
void FillTone(QVector<QVector<float> > *channels, int frame_index, int nb_channels, double base_freq)
{
  channels->resize(nb_channels);

  for (int c = 0; c < nb_channels; c++) {
    QVector<float> &buf = (*channels)[c];
    buf.resize(kSamples);

    const double f = base_freq * double(c + 1);

    for (int i = 0; i < kSamples; i++) {
      const double t = double(frame_index * kSamples + i) / double(kSampleRate);
      buf[i] = 0.25f * float(std::sin(6.283185307179586 * f * t));
    }
  }
}

/**
 * @brief Push kFrames real frames through one AudioProcessor configuration.
 *
 * @return An empty string on success, otherwise a human-readable failure.
 */
QString RunCase(const QString &label, const AudioParams &from, const AudioParams &to)
{
  AudioProcessor proc;

  if (!proc.Open(from, to, 1.0)) {
    return QStringLiteral("%1: Open() failed").arg(label);
  }

  AudioProcessor::Buffer out;

  for (int frame = 0; frame < kFrames; frame++) {
    QVector<QVector<float> > channels;
    FillTone(&channels, frame, from.channel_count(), 220.0);

    QVector<float *> ptrs(from.channel_count());
    for (int c = 0; c < from.channel_count(); c++) {
      ptrs[c] = channels[c].data();
    }

    const int r = proc.Convert(ptrs.data(), kSamples, &out);

    if (r < 0) {
      // This is the assertion the 0061 regression would have tripped on push #2.
      return QStringLiteral("%1: push #%2 of %3 failed with %4 (abuffersrc channel sync?)")
          .arg(label)
          .arg(frame + 1)
          .arg(kFrames)
          .arg(r);
    }
  }

  // Drain. AudioProcessor::Convert reports AVERROR_EOF once the graph is empty; that is the normal
  // end-of-stream signal, not a failure.
  proc.Flush();
  const int drain = proc.Convert(nullptr, 0, &out);
  if (drain < 0 && drain != AVERROR_EOF) {
    return QStringLiteral("%1: drain failed with %2").arg(label).arg(drain);
  }

  proc.Close();

  const int planes = to.format().is_packed() ? 1 : to.channel_count();
  if (out.size() != planes || out.first().isEmpty()) {
    return QStringLiteral("%1: expected %2 non-empty output plane(s), got %3")
        .arg(label)
        .arg(planes)
        .arg(out.size());
  }

  LogLine(QStringLiteral("%1: %2 pushes ok, %3 plane(s) x %4 bytes")
              .arg(label)
              .arg(kFrames)
              .arg(out.size())
              .arg(out.first().size()));

  return QString();
}

} // namespace

OLIVE_ADD_TEST(AudioProcessorMultiFramePush)
{
  const AudioParams f32p_stereo(kSampleRate, AV_CH_LAYOUT_STEREO, SampleFormat(SampleFormat::F32P));

  struct Case {
    const char *label;
    AudioParams from;
    AudioParams to;
  };

  // Same parameters (no aformat filter), format conversion only, and rate+layout conversion.
  // The last one also covers the resampler that 0054 touched.
  const Case cases[] = {
    { "identity stereo F32P", f32p_stereo,
      AudioParams(kSampleRate, AV_CH_LAYOUT_STEREO, SampleFormat(SampleFormat::F32P)) },
    { "stereo F32P -> S16P", f32p_stereo,
      AudioParams(kSampleRate, AV_CH_LAYOUT_STEREO, SampleFormat(SampleFormat::S16P)) },
    { "stereo 48k F32P -> 5.1 44.1k F32P", f32p_stereo,
      AudioParams(44100, AV_CH_LAYOUT_5POINT1, SampleFormat(SampleFormat::F32P)) },
  };

  for (const Case &c : cases) {
    const QString err = RunCase(QString::fromUtf8(c.label), c.from, c.to);
    if (!err.isEmpty()) {
      LogLine(err);
      return __LINE__;
    }
  }

  OLIVE_TEST_END;
}

} // namespace olive
