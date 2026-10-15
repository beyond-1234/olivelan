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

#ifndef OLIVE_BUILTINCOLORSPACES_H
#define OLIVE_BUILTINCOLORSPACES_H

#include <QString>

namespace olive {

/**
 * @brief Patch 0046 (Phase 8): the common display-linear scale for the built-in engine.
 *
 * PQ (SMPTE ST 2084) normalises so that 1.0 == 10000 cd/m2; HLG (ITU-R BT.2100)
 * normalises so that its inverse OETF maps 1.0 to the nominal 1000 cd/m2 peak.
 * Those two scales are not comparable, so anything downstream of them (gamut
 * matrices, display roll-off) would be meaningless if applied to raw values.
 *
 * The built-in engine therefore works in a single display-referred scale anchored on
 * the ITU-R BT.2408 HDR reference diffuse white of 203 cd/m2 - i.e. 1.0 is exactly
 * the SDR (Rec.709/BT.1886) white point. The rationale for this anchor:
 *   * SDR content maps to 1.0 unchanged, so adding HDR support cannot alter
 *     existing SDR projects;
 *   * BT.2408 nominates 203 cd/m2 as the diffuse-white level that should sit at
 *     the SDR white, which is what the roll-off knee is built around below.
 */
constexpr double kDisplayReferenceWhiteNits = 203.0;

/**
 * @brief Default assumed mastering display peak when the container carries no
 *        mastering metadata (mastering_peak_nits == 0).
 *
 * 1000 cd/m2 covers the overwhelming majority of streaming and mobile HDR10
 * deliverables. Assuming the PQ *theoretical* maximum of 10000 cd/m2 instead would
 * compress a 100 cd/m2 diffuse white down to 0.01 and render ordinary footage as
 * if it were a night scene, so the default here is deliberately the pragmatic one.
 * A clip that does carry metadata always wins over this default.
 */
constexpr double kDefaultMasteringPeakNits = 1000.0;

/**
 * @brief Where the highlight roll-off starts, in units of the 203 cd/m2 reference white.
 *
 * Everything below 1.0 (i.e. below SDR diffuse white) passes through untouched, so
 * SDR whites and midtones are bit-stable. Only the range above that, up to the mastering
 * peak, is compressed.
 *
 * Note this is a "knee"-style design rather than a raw Reinhard curve over the whole
 * range: a global Reinhard would darken midtones slightly, which is exactly the artefact
 * the knee is meant to avoid.
 */
constexpr double kRollOffStartNormalized = 1.0;

/**
 * @brief Patch 0045 (Phase 8): the single source of truth for the built-in engine.
 *
 * Before this header existed, the built-in engine carried the same knowledge twice:
 *
 *   * `colorprocessor.cpp` had the `TransferFunction` enum + `GetTransferFunction()`
 *     that actually evaluates pixels;
 *   * `colormanager.cpp` had a hand-written 18-entry `kBuiltinColorspaces` list that
 *     merely *advertised* names to the UI and to the legacy project serializers.
 *
 * The two drifted apart: only 7 of the 18 advertised names had an implementation. The
 * other 11 (AppleP3 variants, V-Log, F-Log, Arri LogC, Blackmagic Gen 5, and the
 * Filmic log family) fell through `GetTransferFunction()` to `kTransferLinear`, so a user
 * who selected any of them got an unlabelled linear passthrough - silent degradation,
 * which this project treats as a correctness bug rather than a cosmetic one.
 *
 * The table below is now the only place colour spaces are declared. Both the evaluation
 * path and the advertised list derive from it, so it is structurally impossible to
 * advertise a colour space the engine cannot evaluate.
 *
 * Deliberately narrow: these are the transfer functions actually implemented. Adding a
 * colour space means adding its maths to `ToLinear()`/`FromLinear()` *and* a row here -
 * the parity gate (`ColorspaceImplementationParity`) fails the build if you only do one.
 */
enum BuiltinTransferFunction {
  kTransferLinear,
  kTransferSrgb,
  kTransferRec709,
  kTransferBt1886,
  kTransferHlg,
  kTransferPq,
  kTransferData
};

struct BuiltinColorSpace {
  const char *name;
  BuiltinTransferFunction transfer;
};

constexpr BuiltinColorSpace kBuiltinColorSpaces[] = {
  { "Linear",          kTransferLinear },
  { "sRGB OETF",       kTransferSrgb   },
  { "Rec.709 OETF",    kTransferRec709 },
  { "BT.1886 EOTF",    kTransferBt1886 },
  { "Rec.2020 HLG",    kTransferHlg    },
  { "Rec.2020 PQ",     kTransferPq     },
  { "Non-Colour Data", kTransferData   },
};

constexpr int kBuiltinColorSpaceCount =
    int(sizeof(kBuiltinColorSpaces) / sizeof(kBuiltinColorSpaces[0]));

/**
 * @brief Colour space a legacy project falls back to when it names something the
 *        built-in engine cannot evaluate (see the parity gate).
 */
inline QString BuiltinFallbackColorSpace()
{
  return QStringLiteral("Rec.709 OETF");
}

// ---------------------------------------------------------------------------
// Patch 0046 (Phase 8): Rec.2020 <-> Rec.709 linear gamut matrices.
//
// Taken from the primaries in ITU-R BT.2020 / BT.709 with the D65 white point
// (the standard NPM column-normalised form). The two matrices are exact inverses
// up to the 8-decimal rounding used here (~2e-5 relative), which the gamut
// round-trip probe asserts.
//
// They are shared by the CPU path (`ColorProcessor::ConvertColor`) and will be
// emitted into GLSL by patch 0047, so the two paths cannot drift apart.
// ---------------------------------------------------------------------------
constexpr double kRec2020ToRec709[3][3] = {
  {  1.66049633, -0.58765635, -0.07284083 },
  { -0.12454704,  1.13289513, -0.00834832 },
  { -0.01815369, -0.10059740,  1.11875110 }
};

constexpr double kRec709ToRec2020[3][3] = {
  { 0.62740390, 0.32928304, 0.04331307 },
  { 0.06909729, 0.91954040, 0.01136232 },
  { 0.01639144, 0.08801331, 0.89559525 }
};

}

#endif // OLIVE_BUILTINCOLORSPACES_H
