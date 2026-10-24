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
 * Phase 6 / patch 0029: color golden probe (pixel-conversion half).
 *
 * Records an immutable Golden snapshot of Frame::convert() across every
 * PixelFormat x channel_count combination so the Phase 7 rewrite of
 * Frame::convert() (patch 0031, which drops OpenImageIO) can be asserted
 * against the pre-rewrite behaviour:
 *
 *   - integer destinations (U8/U16): bit-exact, zero error
 *   - float destinations  (F16/F32): |delta| < 1e-5 (F32) / 1e-3 (F16)
 *
 * Record mode (writes the Golden set):
 *
 *   OLIVE_GOLDEN_RECORD=1 ./color-golden-tests
 *
 * Verify mode (default, used by ctest) reads the on-disk Golden set and fails
 * on any mismatch or missing file.
 *
 * NOTE: the second half of the 0029 spec - the *colour-equivalence* Golden set
 * (Rec.709 -> Linear -> sRGB, sRGB image load, .cube 3D LUT node render) - has
 * to be recorded from the live OCIO render pipeline and therefore requires a
 * GL/OCIO runtime. It is intentionally not implemented here; see
 * tests/golden/README.md for the recording procedure that has to run on a GPU
 * session before Phase 8 lands.
 */

#include "testutil.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QtGlobal>

#include <cmath>

#include "codec/frame.h"
#include "render/videoparams.h"

using namespace olive;
using namespace olive::core;

namespace olive {

namespace {

const int kWidth = 16;
const int kHeight = 16;

const int kChannelCounts[] = {1, 2, 3, 4};
const PixelFormat kFormats[] = {
  PixelFormat::U8,
  PixelFormat::U16,
  PixelFormat::F16,
  PixelFormat::F32
};

QString FormatName(PixelFormat f)
{
  switch (f) {
  case PixelFormat::U8:  return QStringLiteral("U8");
  case PixelFormat::U16: return QStringLiteral("U16");
  case PixelFormat::F16: return QStringLiteral("F16");
  case PixelFormat::F32: return QStringLiteral("F32");
  default:               return QStringLiteral("INVALID");
  }
}

QString GoldenDir()
{
#ifdef OLIVE_TEST_GOLDEN_DIR
  return QStringLiteral(OLIVE_TEST_GOLDEN_DIR) + QStringLiteral("/pixel_convert");
#else
  return QStringLiteral("tests/golden/pixel_convert");
#endif
}

bool Recording()
{
  return qEnvironmentVariableIsSet("OLIVE_GOLDEN_RECORD");
}

// Deterministic, in-range per-channel ramps so the Golden set is reproducible
// bit-for-bit on a given build.
Color Sample(int x, int y)
{
  const unsigned int idx = static_cast<unsigned int>(y * kWidth + x);
  const float r = static_cast<float>((idx * 7u) % 251u) / 250.0f;
  const float g = static_cast<float>((idx * 13u + 5u) % 251u) / 250.0f;
  const float b = static_cast<float>((idx * 29u + 11u) % 251u) / 250.0f;
  const float a = static_cast<float>((idx * 3u + 1u) % 251u) / 250.0f;
  return Color(r, g, b, a);
}

FramePtr MakeSourceFrame(PixelFormat format, int channels)
{
  VideoParams params(kWidth, kHeight, format, channels);

  FramePtr frame = Frame::Create();
  frame->set_video_params(params);

  if (!frame->allocate()) {
    return nullptr;
  }

  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      frame->set_pixel(x, y, Sample(x, y));
    }
  }

  return frame;
}

// Only the pixel-bearing bytes of each row are deterministic; the 32-byte row
// alignment padding is left uninitialised by Frame::allocate(), so it must be
// excluded from the comparison or the Golden set would be flaky.
QByteArray MeaningfulBytes(const FramePtr &frame)
{
  const int row_bytes = frame->width() * frame->video_params().GetBytesPerPixel();
  const int stride = frame->linesize_bytes();

  QByteArray out;
  out.reserve(row_bytes * frame->height());
  for (int y = 0; y < frame->height(); ++y) {
    out.append(frame->const_data() + y * stride, row_bytes);
  }
  return out;
}

float FloatTolerance(PixelFormat format)
{
  return (format == PixelFormat::F32) ? 1e-5f : 1e-3f;
}

bool BytesMatch(const QByteArray &produced, const QByteArray &golden,
                PixelFormat dst_format, QString *why)
{
  if (dst_format == PixelFormat::U8 || dst_format == PixelFormat::U16) {
    if (produced == golden) {
      return true;
    }
    *why = QStringLiteral("integer output differs from Golden");
    return false;
  }

  if (produced.size() != golden.size()) {
    *why = QStringLiteral("size differs from Golden");
    return false;
  }

  const int comp_size = (dst_format == PixelFormat::F32) ? 4 : 2;
  const int components = produced.size() / comp_size;
  const float tol = FloatTolerance(dst_format);

  for (int i = 0; i < components; ++i) {
    const Color a = Color::fromData(produced.constData() + i * comp_size, dst_format, 1);
    const Color b = Color::fromData(golden.constData() + i * comp_size, dst_format, 1);

    if (std::fabs(a.red() - b.red()) > tol) {
      *why = QStringLiteral("float component %1 differs: %2 vs %3 (tol %4)")
                 .arg(i).arg(a.red()).arg(b.red()).arg(tol);
      return false;
    }
  }

  return true;
}

} // namespace

OLIVE_ADD_TEST(pixel_convert_golden)
{
  const QString dir = GoldenDir();
  const bool record = Recording();

  if (record && !QDir().mkpath(dir)) {
    std::cout << " - cannot create Golden dir " << dir.toStdString();
    return __LINE__;
  }

  int combos = 0;

  for (PixelFormat src_format : kFormats) {
    for (PixelFormat dst_format : kFormats) {
      for (int channels : kChannelCounts) {
        FramePtr src = MakeSourceFrame(src_format, channels);
        if (!src || !src->is_allocated()) {
          std::cout << " - could not build source frame "
                    << FormatName(src_format).toStdString() << " c" << channels;
          return __LINE__;
        }

        FramePtr dst = src->convert(dst_format);
        if (!dst || !dst->is_allocated()) {
          std::cout << " - Frame::convert returned null for "
                    << FormatName(src_format).toStdString() << " -> "
                    << FormatName(dst_format).toStdString() << " c" << channels;
          return __LINE__;
        }

        const QByteArray produced = MeaningfulBytes(dst);

        const QString name = QStringLiteral("convert_%1_to_%2_c%3.bin")
                                 .arg(FormatName(src_format))
                                 .arg(FormatName(dst_format))
                                 .arg(channels);
        const QString path = dir + QLatin1Char('/') + name;

        if (record) {
          QFile out(path);
          if (!out.open(QIODevice::WriteOnly)) {
            std::cout << " - cannot write " << path.toStdString();
            return __LINE__;
          }
          out.write(produced);
          out.close();
        } else {
          QFile in(path);
          if (!in.open(QIODevice::ReadOnly)) {
            std::cout << " - missing Golden " << path.toStdString()
                      << " (record with OLIVE_GOLDEN_RECORD=1)";
            return __LINE__;
          }

          QString why;
          if (!BytesMatch(produced, in.readAll(), dst_format, &why)) {
            std::cout << " - " << name.toStdString() << ": " << why.toStdString();
            return __LINE__;
          }
        }

        ++combos;
      }
    }
  }

  std::cout << " [" << combos << " combinations"
            << (record ? ", recorded" : ", verified") << "]";

  OLIVE_TEST_END;
}

} // namespace olive
