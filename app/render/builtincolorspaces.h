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

}

#endif // OLIVE_BUILTINCOLORSPACES_H
