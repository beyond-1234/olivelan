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
 * Phase 8 / patch 0040: .cube LUT parser probe.
 *
 * The built-in colour engine needs its own .cube reader (OpenColorIO would
 * otherwise bake the LUT into its generated shader). This test pins the format
 * behaviour that the GPU upload depends on: entry ordering (red varies fastest),
 * trilinear interpolation, 1D per-channel curves, and DOMAIN_MIN/DOMAIN_MAX.
 *
 * The channel-swap cases are the important ones: if the red-fastest index
 * ordering were wrong, the corner values would come back permuted.
 */

#include "testutil.h"

#include <cmath>

#include <QDir>
#include <QFile>

#include "render/cubelut.h"

namespace olive {

namespace {

bool Near(float a, float b)
{
  return std::fabs(a - b) < 1e-5f;
}

bool NearRgb(const float *rgb, float r, float g, float b)
{
  return Near(rgb[0], r) && Near(rgb[1], g) && Near(rgb[2], b);
}

// 2x2x2 identity LUT, entries in red-fastest order.
const char *kIdentity3D =
    "# identity\n"
    "LUT_3D_SIZE 2\n"
    "0 0 0\n"
    "1 0 0\n"
    "0 1 0\n"
    "1 1 0\n"
    "0 0 1\n"
    "1 0 1\n"
    "0 1 1\n"
    "1 1 1\n";

// 2x2x2 LUT that maps out.r = in.b, out.g = in.r, out.b = in.g.
const char *kChannelSwap3D =
    "TITLE \"swap\"\n"
    "LUT_3D_SIZE 2\n"
    "0 0 0\n"
    "0 1 0\n"
    "0 0 1\n"
    "0 1 1\n"
    "1 0 0\n"
    "1 1 0\n"
    "1 0 1\n"
    "1 1 1\n";

}

OLIVE_ADD_TEST(CubeLutIdentity3D)
{
  CubeLUT lut;

  if (!lut.LoadFromString(QString::fromUtf8(kIdentity3D))) {
    std::cout << " - failed to parse the identity LUT";
    return __LINE__;
  }

  if (!lut.IsValid() || lut.Is1D() || lut.size() != 2) {
    std::cout << " - unexpected LUT metadata";
    return __LINE__;
  }

  float out[3];

  // Corners are exact.
  lut.Sample(1.0f, 0.0f, 0.0f, out);
  if (!NearRgb(out, 1.0f, 0.0f, 0.0f)) {
    std::cout << " - identity LUT is not the identity at a corner";
    return __LINE__;
  }

  // Trilinear interpolation of the identity is exact everywhere.
  lut.Sample(0.25f, 0.5f, 0.75f, out);
  if (!NearRgb(out, 0.25f, 0.5f, 0.75f)) {
    std::cout << " - identity LUT is not the identity at an interior point";
    return __LINE__;
  }

  // Out-of-domain values clamp.
  lut.Sample(-1.0f, 2.0f, 0.5f, out);
  if (!NearRgb(out, 0.0f, 1.0f, 0.5f)) {
    std::cout << " - identity LUT does not clamp out-of-domain input";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(CubeLutChannelOrder3D)
{
  CubeLUT lut;

  if (!lut.LoadFromString(QString::fromUtf8(kChannelSwap3D))) {
    std::cout << " - failed to parse the channel-swap LUT";
    return __LINE__;
  }

  float out[3];

  // These corners fail if the red-fastest index ordering is wrong.
  lut.Sample(1.0f, 0.0f, 0.0f, out);
  if (!NearRgb(out, 0.0f, 1.0f, 0.0f)) {
    std::cout << " - red-fastest ordering is wrong at (1,0,0)";
    return __LINE__;
  }

  lut.Sample(0.0f, 0.0f, 1.0f, out);
  if (!NearRgb(out, 1.0f, 0.0f, 0.0f)) {
    std::cout << " - red-fastest ordering is wrong at (0,0,1)";
    return __LINE__;
  }

  // A permutation is affine per channel, so trilinear interpolation is exact.
  lut.Sample(0.5f, 0.25f, 0.75f, out);
  if (!NearRgb(out, 0.75f, 0.5f, 0.25f)) {
    std::cout << " - channel-swap interpolation is wrong at an interior point";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(CubeLut1DPerChannel)
{
  // Per-channel 1D curve: r(0)=0, r(0.5)=0.5, r(1)=1; g and b have their own
  // curves, so sampling must index each channel with its own input value.
  const QString text = QStringLiteral(
      "LUT_1D_SIZE 3\n"
      "0 0 0\n"
      "0.5 0.25 0.75\n"
      "1 1 1\n");

  CubeLUT lut;

  if (!lut.LoadFromString(text)) {
    std::cout << " - failed to parse the 1D LUT";
    return __LINE__;
  }

  if (!lut.Is1D() || lut.size() != 3) {
    std::cout << " - unexpected 1D LUT metadata";
    return __LINE__;
  }

  float out[3];

  lut.Sample(0.25f, 0.25f, 0.25f, out);
  if (!NearRgb(out, 0.25f, 0.125f, 0.375f)) {
    std::cout << " - 1D LUT interpolation is wrong";
    return __LINE__;
  }

  lut.Sample(1.0f, 1.0f, 1.0f, out);
  if (!NearRgb(out, 1.0f, 1.0f, 1.0f)) {
    std::cout << " - 1D LUT is wrong at the top of the range";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(CubeLutDomain)
{
  // With DOMAIN_MAX 2, an input of 1.0 addresses the middle of the LUT.
  const QString text = QStringLiteral(
      "LUT_3D_SIZE 2\n"
      "DOMAIN_MIN 0 0 0\n"
      "DOMAIN_MAX 2 2 2\n"
      "0 0 0\n"
      "1 0 0\n"
      "0 1 0\n"
      "1 1 0\n"
      "0 0 1\n"
      "1 0 1\n"
      "0 1 1\n"
      "1 1 1\n");

  CubeLUT lut;

  if (!lut.LoadFromString(text)) {
    std::cout << " - failed to parse the domain LUT";
    return __LINE__;
  }

  float out[3];

  lut.Sample(1.0f, 1.0f, 1.0f, out);
  if (!NearRgb(out, 0.5f, 0.5f, 0.5f)) {
    std::cout << " - DOMAIN_MIN/DOMAIN_MAX were not applied";
    return __LINE__;
  }

  lut.Sample(2.0f, 0.0f, 0.0f, out);
  if (!NearRgb(out, 1.0f, 0.0f, 0.0f)) {
    std::cout << " - DOMAIN_MAX does not map to the top of the LUT";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(CubeLutRejectsMalformed)
{
  // Only 7 entries for a 2x2x2 LUT (8 expected).
  const QString short_lut = QStringLiteral(
      "LUT_3D_SIZE 2\n"
      "0 0 0\n"
      "1 0 0\n"
      "0 1 0\n"
      "1 1 0\n"
      "0 0 1\n"
      "1 0 1\n"
      "0 1 1\n");

  CubeLUT lut;

  if (lut.LoadFromString(short_lut)) {
    std::cout << " - truncated LUT was accepted";
    return __LINE__;
  }

  if (lut.LoadFromString(QStringLiteral("LUT_3D_SIZE 1\n0 0 0\n"))) {
    std::cout << " - LUT smaller than 2 entries was accepted";
    return __LINE__;
  }

  if (lut.LoadFromString(QStringLiteral("LUT_3D_SIZE 2\n0 0\n"))) {
    std::cout << " - row with two components was accepted";
    return __LINE__;
  }

  if (lut.LoadFromString(QStringLiteral("nothing here\n"))) {
    std::cout << " - file with no LUT declaration was accepted";
    return __LINE__;
  }

  // An invalid LUT must leave Sample() as a pass-through.
  float out[3];
  lut.Sample(0.3f, 0.6f, 0.9f, out);
  if (!NearRgb(out, 0.3f, 0.6f, 0.9f)) {
    std::cout << " - invalid LUT did not pass through";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(CubeLutLoadFromFile)
{
  const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("olive-cubelut-test.cube"));

  {
    QFile f(path);
    if (!f.open(QFile::WriteOnly | QFile::Text)) {
      std::cout << " - could not write the temporary .cube file";
      return __LINE__;
    }
    f.write(kIdentity3D);
  }

  CubeLUT lut;

  if (!lut.Load(path)) {
    QFile::remove(path);
    std::cout << " - failed to load the temporary .cube file";
    return __LINE__;
  }

  QFile::remove(path);

  if (!lut.IsValid() || lut.size() != 2) {
    std::cout << " - loaded LUT has unexpected metadata";
    return __LINE__;
  }

  float out[3];
  lut.Sample(0.75f, 0.25f, 0.5f, out);
  if (!NearRgb(out, 0.75f, 0.25f, 0.5f)) {
    std::cout << " - loaded LUT does not sample correctly";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

}
