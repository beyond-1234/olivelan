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
 * Phase 8 / patch 0044: the A-class colour-equivalence gate.
 *
 * The plan's A-class gate is "compare the built-in engine against the recorded
 * OpenColorIO golden pixel-for-pixel, mean dE00 < 1.0 and PSNR > 45 dB". This file
 * implements the part of it that is measurable without a GPU.
 *
 * The comparison runs on the CPU through the same abstraction the renderer uses,
 * `ColorProcessor::ConvertColor()`: in an OCIO build that call dispatches to
 * OCIO's `getDefaultCPUProcessor()->applyRGBA()`, and in a built-in build it is a
 * pure per-channel transfer function. Both are CPU code, so the two backends can
 * be compared from a headless test with no display, no GPU and no frame buffer.
 *
 * Recording the golden
 * --------------------
 * The reference values are NOT derived from the implementation under test. They
 * are recorded from the OpenColorIO reference build: run
 * this test, and the ColorEquivalenceAclass test prints the table below (it asserts
 * nothing in that configuration). Those printed values were pasted into kGolden,
 * and the OFF build asserts against them. This is the same "record then compare"
 * arrangement the phase uses elsewhere.
 *
 * Coverage, honestly stated
 * -------------------------
 *   * Covered: the transfer-function colour spaces both backends define - Linear,
 *     sRGB OETF, Rec.709 OETF and BT.1886 EOTF - in both directions and crossed.
 *     In the bundled config each of these is defined by a transfer function alone
 *     (sRGB/Rec.709 by a sampled .spi1d curve, BT.1886 by an ExponentTransform,
 *     Linear by nothing), with no gamut matrix, so the built-in engine's
 *     transfer-function-only model is directly comparable.
 *   * Both engines are per-channel for these spaces, so the greyscale ramp samples
 *     the whole 1-D domain of each transfer function 8 times per pair. Three
 *     chromatic triples are added as well, so that any channel *coupling* in either
 *     engine would show up as a large dE00 rather than hiding behind the greys.
 *   * The metric interpretation is fixed and applied identically to both engines:
 *     outputs are read as sRGB-encoded display values, converted to CIE Lab (D65)
 *     and compared with CIEDE2000. PSNR is computed over 8-bit code values, i.e.
 *     the "pixel-for-pixel" reading. Neither convention can favour a backend
 *     because both sides go through it unchanged.
 *
 * NOT covered here (see the notes in patches/README.md):
 *   * the `.cube` 3D LUT trilinear sampling part of A-class - the built-in engine
 *     still has no LUT path (patch 0040 only landed the parser and its own unit
 *     test), so there is no OFF-side counterpart to compare yet;
 *   * alpha premultiply/unpremultiply - neither backend's ConvertColor touches
 *     alpha, so there is nothing to compare beyond "alpha is untouched", which
 *     the OffsideAlpha case below asserts;
 *   * anything requiring the GPU: the actual rendered frames, the 4K60
 *     performance regression, and the B-class HDR monotonicity/roll-off gate.
 *     Those still need a real GPU session.
 */

#include "testutil.h"

#include <cmath>
#include <iomanip>
#include <limits>

#include <QtGlobal>

#include <QString>

#include "core.h"
#include "node/color/colormanager/colormanager.h"
#include "node/project.h"
#include "render/colorprocessor.h"
#include "render/colortransform.h"

namespace olive {

namespace {

const double kPi = 3.14159265358979323846;

bool NearlyEqual(double a, double b, double epsilon)
{
  return std::abs(a - b) <= epsilon;
}

// --- sRGB -> CIE Lab (D65) ---------------------------------------------------

double SrgbToLinearComponent(double v)
{
  return (v <= 0.04045) ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

void SrgbToLab(double r, double g, double b, double *L, double *a, double *bb)
{
  r = SrgbToLinearComponent(r);
  g = SrgbToLinearComponent(g);
  b = SrgbToLinearComponent(b);

  const double x = 0.4124564*r + 0.3575761*g + 0.1804375*b;
  const double y = 0.2126729*r + 0.7151522*g + 0.0721750*b;
  const double z = 0.0193339*r + 0.1191920*g + 0.9503041*b;

  const double xn = 0.95047, yn = 1.0, zn = 1.08883;
  const double delta = 6.0 / 29.0;

  auto f = [delta](double t) {
    return (t > delta*delta*delta) ? std::cbrt(t) : (t / (3.0*delta*delta) + 4.0/29.0);
  };

  const double fx = f(x/xn), fy = f(y/yn), fz = f(z/zn);

  *L = 116.0 * fy - 16.0;
  *a = 500.0 * (fx - fy);
  *bb = 200.0 * (fy - fz);
}

// --- CIEDE2000 (Sharma, Wu & Dalal 2005 formulation) -------------------------

double Ciede2000(double l1, double a1, double b1, double l2, double a2, double b2)
{
  const double c1 = std::sqrt(a1*a1 + b1*b1);
  const double c2 = std::sqrt(a2*a2 + b2*b2);
  const double c_bar = 0.5 * (c1 + c2);

  const double c_bar7 = std::pow(c_bar, 7.0);
  const double twentyfive7 = std::pow(25.0, 7.0);
  const double g = 0.5 * (1.0 - std::sqrt(c_bar7 / (c_bar7 + twentyfive7)));

  const double a1p = (1.0 + g) * a1;
  const double a2p = (1.0 + g) * a2;

  const double c1p = std::sqrt(a1p*a1p + b1*b1);
  const double c2p = std::sqrt(a2p*a2p + b2*b2);

  auto hue = [](double b, double ap) {
    if (b == 0.0 && ap == 0.0) {
      return 0.0;
    }
    double h = std::atan2(b, ap);
    if (h < 0.0) {
      h += 2.0 * kPi;
    }
    return h;
  };

  const double h1p = hue(b1, a1p);
  const double h2p = hue(b2, a2p);

  const double delta_lp = l2 - l1;
  const double delta_cp = c2p - c1p;

  double delta_hp;
  if (c1p * c2p == 0.0) {
    delta_hp = 0.0;
  } else {
    delta_hp = h2p - h1p;
    if (delta_hp > kPi) {
      delta_hp -= 2.0 * kPi;
    } else if (delta_hp < -kPi) {
      delta_hp += 2.0 * kPi;
    }
  }

  const double delta_big_hp = 2.0 * std::sqrt(c1p * c2p) * std::sin(delta_hp / 2.0);

  const double l_barp = 0.5 * (l1 + l2);
  const double c_barp = 0.5 * (c1p + c2p);

  double h_barp;
  if (c1p * c2p == 0.0) {
    h_barp = h1p + h2p;
  } else if (std::abs(h1p - h2p) <= kPi) {
    h_barp = 0.5 * (h1p + h2p);
  } else if (h1p + h2p < 2.0 * kPi) {
    h_barp = 0.5 * (h1p + h2p + 2.0 * kPi);
  } else {
    h_barp = 0.5 * (h1p + h2p - 2.0 * kPi);
  }

  const double t = 1.0
      - 0.17 * std::cos(h_barp - kPi/6.0)
      + 0.24 * std::cos(2.0 * h_barp)
      + 0.32 * std::cos(3.0 * h_barp + kPi/30.0)
      - 0.20 * std::cos(4.0 * h_barp - 63.0 * kPi/180.0);

  const double delta_theta = (30.0 * kPi/180.0)
      * std::exp(-std::pow(((h_barp * 180.0/kPi) - 275.0) / 25.0, 2.0));

  const double c_barp7 = std::pow(c_barp, 7.0);
  const double rc = 2.0 * std::sqrt(c_barp7 / (c_barp7 + twentyfive7));

  const double sl = 1.0 + (0.015 * std::pow(l_barp - 50.0, 2.0))
                          / std::sqrt(20.0 + std::pow(l_barp - 50.0, 2.0));
  const double sc = 1.0 + 0.045 * c_barp;
  const double sh = 1.0 + 0.015 * c_barp * t;
  const double rt = -std::sin(2.0 * delta_theta) * rc;

  const double dl = delta_lp / sl;
  const double dc = delta_cp / sc;
  const double dh = delta_big_hp / sh;

  return std::sqrt(dl*dl + dc*dc + dh*dh + rt*dc*dh);
}

// The published CIEDE2000 test data of Sharma, Wu and Dalal, fetched from
// https://hajim.rochester.edu/ece/sites/gsharma/ciede2000/ (the authors' own copy
// of the data set used in the paper). Pinning the metric against these vectors is
// what makes the dE00 numbers below meaningful: if this implementation drifted, the
// gate could pass while measuring nothing.
struct Ciede2000Reference
{
  double l1, a1, b1, l2, a2, b2, expected;
};

const Ciede2000Reference kCiede2000References[] = {
  {50.0000,   2.6772, -79.7751, 50.0000,   0.0000, -82.7485,  2.0425},
  {50.0000,   3.1571, -77.2803, 50.0000,   0.0000, -82.7485,  2.8615},
  {50.0000,   2.8361, -74.0200, 50.0000,   0.0000, -82.7485,  3.4412},
  {50.0000,  -1.3802, -84.2814, 50.0000,   0.0000, -82.7485,  1.0000},
  {50.0000,  -1.1848, -84.8006, 50.0000,   0.0000, -82.7485,  1.0000},
  {50.0000,  -0.9009, -85.5211, 50.0000,   0.0000, -82.7485,  1.0000},
  {50.0000,   0.0000,   0.0000, 50.0000,  -1.0000,   2.0000,  2.3669},
  {50.0000,  -1.0000,   2.0000, 50.0000,   0.0000,   0.0000,  2.3669},
  {50.0000,   2.4900,  -0.0010, 50.0000,  -2.4900,   0.0009,  7.1792},
  {50.0000,   2.4900,  -0.0010, 50.0000,  -2.4900,   0.0010,  7.1792},
  {50.0000,   2.4900,  -0.0010, 50.0000,  -2.4900,   0.0011,  7.2195},
  {50.0000,   2.4900,  -0.0010, 50.0000,  -2.4900,   0.0012,  7.2195},
  {50.0000,  -0.0010,   2.4900, 50.0000,   0.0009,  -2.4900,  4.8045},
  {50.0000,  -0.0010,   2.4900, 50.0000,   0.0010,  -2.4900,  4.8045},
  {50.0000,  -0.0010,   2.4900, 50.0000,   0.0011,  -2.4900,  4.7461},
  {50.0000,   2.5000,   0.0000, 50.0000,   0.0000,  -2.5000,  4.3065},
  {50.0000,   2.5000,   0.0000, 73.0000,  25.0000, -18.0000, 27.1492},
  {50.0000,   2.5000,   0.0000, 61.0000,  -5.0000,  29.0000, 22.8977},
  {50.0000,   2.5000,   0.0000, 56.0000, -27.0000,  -3.0000, 31.9030},
  {50.0000,   2.5000,   0.0000, 58.0000,  24.0000,  15.0000, 19.4535},
  {50.0000,   2.5000,   0.0000, 50.0000,   3.1736,   0.5854,  1.0000},
  {50.0000,   2.5000,   0.0000, 50.0000,   3.2972,   0.0000,  1.0000},
  {50.0000,   2.5000,   0.0000, 50.0000,   1.8634,   0.5757,  1.0000},
  {50.0000,   2.5000,   0.0000, 50.0000,   3.2592,   0.3350,  1.0000},
  {60.2574, -34.0099,  36.2677, 60.4626, -34.1751,  39.4387,  1.2644},
  {63.0109, -31.0961,  -5.8663, 62.8187, -29.7946,  -4.0864,  1.2630},
  {61.2901,   3.7196,  -5.3901, 61.4292,   2.2480,  -4.9620,  1.8731},
  {35.0831, -44.1164,   3.7933, 35.0232, -40.0716,   1.5901,  1.8645},
  {22.7233,  20.0904, -46.6940, 23.0331,  14.9730, -42.5619,  2.0373},
  {36.4612,  47.8580,  18.3852, 36.2715,  50.5065,  21.2231,  1.4146},
  {90.8027,  -2.0831,   1.4410, 91.1528,  -1.6435,   0.0447,  1.4441},
  {90.9257,  -0.5406,  -0.9208, 88.6381,  -0.8985,  -0.7239,  1.5381},
  { 6.7747,  -0.2908,  -2.4247,  5.8714,  -0.0985,  -2.2286,  0.6377},
  { 2.0776,   0.0795,  -1.1350,  0.9033,  -0.0636,  -0.5514,  0.9082},
};

// --- the A-class case matrix ------------------------------------------------

// Every space here is defined by a transfer function alone in both backends.
struct SpacePair
{
  const char *from;
  const char *to;
};

const SpacePair kSpacePairs[] = {
  {"Linear",         "sRGB OETF"},
  {"Linear",         "Rec.709 OETF"},
  {"Linear",         "BT.1886 EOTF"},
  {"sRGB OETF",      "Linear"},
  {"Rec.709 OETF",   "Linear"},
  {"BT.1886 EOTF",   "Linear"},
  {"sRGB OETF",      "Rec.709 OETF"},
  {"Rec.709 OETF",   "sRGB OETF"},
  {"sRGB OETF",      "BT.1886 EOTF"},
};

const int kSpacePairCount = int(sizeof(kSpacePairs) / sizeof(kSpacePairs[0]));

const float kLevels[] = {0.0f, 0.02f, 0.05f, 0.10f, 0.18f, 0.50f, 0.75f, 1.0f};
const int kLevelCount = int(sizeof(kLevels) / sizeof(kLevels[0]));

struct Triple
{
  float r, g, b;
};

const Triple kTriples[] = {
  {1.00f, 0.50f, 0.25f},
  {0.25f, 0.50f, 1.00f},
  {0.75f, 0.10f, 0.05f},
};

const int kTripleCount = int(sizeof(kTriples) / sizeof(kTriples[0]));

const int kConversionsPerPair = kLevelCount + kTripleCount;
const int kGoldenFloatCount = kSpacePairCount * kConversionsPerPair * 3;

Color InputForIndex(int i)
{
  if (i < kLevelCount) {
    const float v = kLevels[i];
    return Color(v, v, v);
  }

  const Triple &t = kTriples[i - kLevelCount];
  return Color(t.r, t.g, t.b);
}

// Recorded from the OpenColorIO build by running ColorEquivalenceAclass with
// from the OCIO reference build (see the file header). Shape is
// [pair][conversion][channel], in the kSpacePairs x (kLevels, kTriples) order.
const float kGolden[kGoldenFloatCount] = {
  // pair 0: Linear -> sRGB OETF
  0.0f, 0.0f, 0.0f, 0.1517036855f, 0.1517036855f,
  0.1517036855f, 0.2478005141f, 0.2478005141f, 0.2478005141f, 0.3491902351f,
  0.3491902351f, 0.3491902351f, 0.4613561332f, 0.4613561332f, 0.4613561332f,
  0.7353570461f, 0.7353570461f, 0.7353570461f, 0.8808251023f, 0.8808251023f,
  0.8808251023f, 1.0f, 1.0f, 1.0f, 1.0f,
  0.7353570461f, 0.5370987058f, 0.5370987058f, 0.7353570461f, 1.0f,
  0.8808251023f, 0.3491902351f, 0.2478005141f,
  // pair 1: Linear -> Rec.709 OETF
  0.0f, 0.0f, 0.0f, 0.0899996832f, 0.0899996832f,
  0.0899996832f, 0.1864527166f, 0.1864527166f, 0.1864527166f, 0.2909398973f,
  0.2909398973f, 0.2909398973f, 0.4090077281f, 0.4090077281f, 0.4090077281f,
  0.7055150867f, 0.7055150867f, 0.7055150867f, 0.8665511012f, 0.8665511012f,
  0.8665511012f, 1.0f, 1.0f, 1.0f, 1.0f,
  0.7055150867f, 0.4899395406f, 0.4899395406f, 0.7055150867f, 1.0f,
  0.8665511012f, 0.2909398973f, 0.1864527166f,
  // pair 2: Linear -> BT.1886 EOTF
  0.0f, 0.0f, 0.0f, 0.1959276944f, 0.1959276944f,
  0.1959276944f, 0.2870147228f, 0.2870147228f, 0.2870147228f, 0.3831186891f,
  0.3831186891f, 0.3831186891f, 0.4894371033f, 0.4894371033f, 0.4894371033f,
  0.7491535544f, 0.7491535544f, 0.7491535544f, 0.8870379329f, 0.8870379329f,
  0.8870379329f, 1.0f, 1.0f, 1.0f, 1.0f,
  0.7491535544f, 0.5612310171f, 0.5612310171f, 0.7491535544f, 1.0f,
  0.8870379329f, 0.3831186891f, 0.2870147228f,
  // pair 3: sRGB OETF -> Linear
  0.0f, 0.0f, 0.0f, 0.001547987573f, 0.001547987573f,
  0.001547987573f, 0.003935946152f, 0.003935946152f, 0.003935946152f, 0.01002283581f,
  0.01002283581f, 0.01002283581f, 0.02721178718f, 0.02721178718f, 0.02721178718f,
  0.2140411586f, 0.2140411586f, 0.2140411586f, 0.5225215554f, 0.5225215554f,
  0.5225215554f, 1.0f, 1.0f, 1.0f, 1.0f,
  0.2140411586f, 0.05087609962f, 0.05087609962f, 0.2140411586f, 1.0f,
  0.5225215554f, 0.01002283581f, 0.003935946152f,
  // pair 4: Rec.709 OETF -> Linear
  0.0f, 0.0f, 0.0f, 0.004444444552f, 0.004444444552f,
  0.004444444552f, 0.01111111138f, 0.01111111138f, 0.01111111138f, 0.02242796868f,
  0.02242796868f, 0.02242796868f, 0.04752295464f, 0.04752295464f, 0.04752295464f,
  0.2595894337f, 0.2595894337f, 0.2595894337f, 0.5635223389f, 0.5635223389f,
  0.5635223389f, 1.0f, 1.0f, 1.0f, 1.0f,
  0.2595894337f, 0.0781538859f, 0.0781538859f, 0.2595894337f, 1.0f,
  0.5635223389f, 0.02242796868f, 0.01111111138f,
  // pair 5: BT.1886 EOTF -> Linear
  0.0f, 0.0f, 0.0f, 8.365113172e-05f, 8.365113172e-05f,
  8.365113172e-05f, 0.0007542718668f, 0.0007542718668f, 0.0007542718668f, 0.003981071059f,
  0.003981071059f, 0.003981071059f, 0.01631751284f, 0.01631751284f, 0.01631751284f,
  0.1894645542f, 0.1894645542f, 0.1894645542f, 0.5013568997f, 0.5013568997f,
  0.5013568997f, 1.0f, 1.0f, 1.0f, 1.0f,
  0.1894645542f, 0.03589681908f, 0.03589681908f, 0.1894645542f, 1.0f,
  0.5013568997f, 0.003981071059f, 0.0007542718668f,
  // pair 6: sRGB OETF -> Rec.709 OETF
  0.0f, 0.0f, 0.0f, 0.006965944543f, 0.006965944543f,
  0.006965944543f, 0.01771175861f, 0.01771175861f, 0.01771175861f, 0.04510276765f,
  0.04510276765f, 0.04510276765f, 0.1180892438f, 0.1180892438f, 0.1180892438f,
  0.4501885772f, 0.4501885772f, 0.4501885772f, 0.721624732f, 0.721624732f,
  0.721624732f, 1.0f, 1.0f, 1.0f, 1.0f,
  0.4501885772f, 0.1886927187f, 0.1886927187f, 0.4501885772f, 1.0f,
  0.721624732f, 0.04510276765f, 0.01771175861f,
  // pair 7: Rec.709 OETF -> sRGB OETF
  0.0f, 0.0f, 0.0f, 0.05545268953f, 0.05545268953f,
  0.05545268953f, 0.1068022773f, 0.1068022773f, 0.1068022773f, 0.1618111134f,
  0.1618111134f, 0.1618111134f, 0.2414573431f, 0.2414573431f, 0.2414573431f,
  0.5464581251f, 0.5464581251f, 0.5464581251f, 0.7757405639f, 0.7757405639f,
  0.7757405639f, 1.0f, 1.0f, 1.0f, 1.0f,
  0.5464581251f, 0.3097386956f, 0.3097386956f, 0.5464581251f, 1.0f,
  0.7757405639f, 0.1618111134f, 0.1068022773f,
  // pair 8: sRGB OETF -> BT.1886 EOTF
  0.0f, 0.0f, 0.0f, 0.06746364385f, 0.06746364385f,
  0.06746364385f, 0.09952614456f, 0.09952614456f, 0.09952614456f, 0.1469195038f,
  0.1469195038f, 0.1469195038f, 0.2227488458f, 0.2227488458f, 0.2227488458f,
  0.5260663629f, 0.5260663629f, 0.5260663629f, 0.7630331516f, 0.7630331516f,
  0.7630331516f, 1.0f, 1.0f, 1.0f, 1.0f,
  0.5260663629f, 0.2890995741f, 0.2890995741f, 0.5260663629f, 1.0f,
  0.7630331516f, 0.1469195038f, 0.09952614456f,
};

}

OLIVE_ADD_TEST(ColorEquivalenceMetric)
{
  // Pin CIEDE2000 to the published reference vectors before it is used for anything.
  const int count = int(sizeof(kCiede2000References) / sizeof(kCiede2000References[0]));

  for (int i = 0; i < count; i++) {
    const Ciede2000Reference &v = kCiede2000References[i];
    const double de = Ciede2000(v.l1, v.a1, v.b1, v.l2, v.a2, v.b2);

    if (!NearlyEqual(de, v.expected, 1e-4)) {
      std::cout << " - CIEDE2000 reference vector " << i << ": got " << de
                << ", expected " << v.expected;
      return __LINE__;
    }
  }

  // Symmetry, which the vectors above only partly cover.
  if (!NearlyEqual(Ciede2000(50, 2.6772, -79.7751, 50, 0, -82.7485),
                   Ciede2000(50, 0, -82.7485, 50, 2.6772, -79.7751), 1e-12)) {
    std::cout << " - CIEDE2000 is not symmetric";
    return __LINE__;
  }

  if (!NearlyEqual(Ciede2000(50, 10, -20, 50, 10, -20), 0.0, 1e-12)) {
    std::cout << " - CIEDE2000 of identical colours is not zero";
    return __LINE__;
  }

  // sRGB -> Lab sanity: the two endpoints must be black and the D65 white point.
  double l, a, b;

  SrgbToLab(0.0, 0.0, 0.0, &l, &a, &b);
  if (!NearlyEqual(l, 0.0, 1e-9) || !NearlyEqual(a, 0.0, 1e-9) || !NearlyEqual(b, 0.0, 1e-9)) {
    std::cout << " - sRGB black is not Lab(0, 0, 0)";
    return __LINE__;
  }

  SrgbToLab(1.0, 1.0, 1.0, &l, &a, &b);
  if (!NearlyEqual(l, 100.0, 1e-3) || !NearlyEqual(a, 0.0, 1e-3) || !NearlyEqual(b, 0.0, 1e-3)) {
    std::cout << " - sRGB white is not Lab(100, 0, 0)";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ColorEquivalenceAclass)
{
  ColorManager::SetUpDefaultConfig();
  Project project;
  ColorManager *manager = project.color_manager();

  int index = 0;

  double de_sum = 0.0;
  int de_count = 0;
  double worst_de = 0.0;
  int worst_de_index = -1;

  double squared_error = 0.0;
  int sample_count = 0;
  int out_of_range = 0;

  for (int p = 0; p < kSpacePairCount; p++) {
    ColorProcessorPtr proc = ColorProcessor::Create(
          manager,
          QString::fromUtf8(kSpacePairs[p].from),
          ColorTransform(QString::fromUtf8(kSpacePairs[p].to)));

    for (int i = 0; i < kConversionsPerPair; i++) {
      const Color in = InputForIndex(i);
      const Color out = proc->ConvertColor(in);

      const double golden[3] = {kGolden[index*3+0], kGolden[index*3+1], kGolden[index*3+2]};
      const double builtin[3] = {out.red(), out.green(), out.blue()};

      double golden_lab[3], builtin_lab[3];
      SrgbToLab(golden[0], golden[1], golden[2], &golden_lab[0], &golden_lab[1], &golden_lab[2]);
      SrgbToLab(builtin[0], builtin[1], builtin[2], &builtin_lab[0], &builtin_lab[1], &builtin_lab[2]);

      const double de = Ciede2000(golden_lab[0], golden_lab[1], golden_lab[2],
                                  builtin_lab[0], builtin_lab[1], builtin_lab[2]);

      de_sum += de;
      de_count++;

      if (de > worst_de) {
        worst_de = de;
        worst_de_index = index;
      }

      for (int c = 0; c < 3; c++) {
        if (builtin[c] < 0.0 || builtin[c] > 1.0 || golden[c] < 0.0 || golden[c] > 1.0) {
          out_of_range++;
        }

        const int bq = int(qBound(0.0, builtin[c], 1.0) * 255.0 + 0.5);
        const int gq = int(qBound(0.0, golden[c], 1.0) * 255.0 + 0.5);

        squared_error += double((bq - gq) * (bq - gq));
        sample_count++;
      }

      index++;
    }
  }

  const double mean_de = de_sum / de_count;
  const double mse = squared_error / sample_count;
  // MSE of exactly zero means the two backends agreed on every 8-bit code value;
  // report that as an infinite ratio rather than inventing a ceiling.
  const double psnr = (mse > 0.0)
      ? 10.0 * std::log10(255.0 * 255.0 / mse)
      : std::numeric_limits<double>::infinity();

  std::cout << " (mean dE00=" << std::setprecision(4) << mean_de
            << ", worst dE00=" << worst_de << " @case " << worst_de_index
            << ", PSNR=" << psnr << " dB"
            << ", cases=" << de_count
            << ", out-of-range samples=" << out_of_range << ")";

  // The A-class thresholds from the plan.
  if (!(mean_de < 1.0)) {
    std::cout << " - A-class failed: mean dE00 " << mean_de << " is not < 1.0";
    return __LINE__;
  }

  if (!(psnr > 45.0)) {
    std::cout << " - A-class failed: PSNR " << psnr << " dB is not > 45 dB";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ColorEquivalenceAlphaUntouched)
{
  ColorManager::SetUpDefaultConfig();
  Project project;
  ColorManager *manager = project.color_manager();

  ColorProcessorPtr proc = ColorProcessor::Create(
        manager,
        QStringLiteral("Linear"),
        ColorTransform(QStringLiteral("sRGB OETF")));

  const Color out = proc->ConvertColor(Color(0.18f, 0.18f, 0.18f, 0.25f));

  if (!NearlyEqual(out.alpha(), 0.25, 1e-9)) {
    std::cout << " - alpha was modified by a colour space conversion";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

}
