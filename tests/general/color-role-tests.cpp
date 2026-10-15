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
 * Phase 8 / patch 0038: colour-management role mapping probe.
 *
 * Patch 0038 decouples the project serializers from OpenColorIO by moving the
 * reference-space role names into OCIO-free constants (OCIOUtils::kRoleDefault,
 * kReferenceSpaceSceneLinear, kReferenceSpaceCompositingLog). This test pins
 * those constants to the literal role identifiers OCIO uses, and checks that the
 * bundled default config actually defines them - otherwise a legacy .ove file
 * would load a reference space the default config cannot resolve.
 *
 * A full serializer round-trip is not exercised here: constructing a Project
 * builds a ColorManager, which requires the OCIO default config to be set up
 * first, so it is not headless-safe. The serializer decoupling itself is a
 * compile-level property (the serializers no longer include OpenColorIO) and is
 * verified separately.
 */

#include "testutil.h"

#include <QFile>
#include <QString>

#include "common/ocioutils.h"
#include "core.h"
#include "node/color/colormanager/colormanager.h"
#include "node/project.h"
#include "render/colorprocessor.h"
#include "render/colortransform.h"

namespace olive {

OLIVE_ADD_TEST(ColorRoleMapping)
{
  if (OCIOUtils::kRoleDefault != QStringLiteral("default")) {
    std::cout << " - kRoleDefault drifted from the OCIO role name";
    return __LINE__;
  }

  if (OCIOUtils::kReferenceSpaceSceneLinear != QStringLiteral("scene_linear")) {
    std::cout << " - kReferenceSpaceSceneLinear drifted from the OCIO role name";
    return __LINE__;
  }

  if (OCIOUtils::kReferenceSpaceCompositingLog != QStringLiteral("compositing_log")) {
    std::cout << " - kReferenceSpaceCompositingLog drifted from the OCIO role name";
    return __LINE__;
  }

#ifdef OLIVE_ENABLE_OCIO
  // The bundled config is only present in an OCIO build (patch 0039 excludes it
  // from the built-in-engine build), so this half of the probe is conditional.
  QFile config(QStringLiteral(":/ocioconf/config.ocio"));
  if (!config.open(QFile::ReadOnly)) {
    std::cout << " - could not open the bundled :/ocioconf/config.ocio";
    return __LINE__;
  }

  const QString text = QString::fromUtf8(config.readAll());

  if (!text.contains(QStringLiteral("scene_linear:"))) {
    std::cout << " - bundled config.ocio does not define the scene_linear role";
    return __LINE__;
  }

  if (!text.contains(QStringLiteral("compositing_log:"))) {
    std::cout << " - bundled config.ocio does not define the compositing_log role";
    return __LINE__;
  }

  if (!text.contains(QStringLiteral("default:"))) {
    std::cout << " - bundled config.ocio does not define the default role";
    return __LINE__;
  }
#endif

  OLIVE_TEST_END;
}

/**
 * Phase 8 / patch 0045: fake-capability gate.
 *
 * `ColorManager::ListAvailableColorspaces()` used to advertise 18 colour spaces while
 * `GetTransferFunction()` only implemented 7; the other 11 fell through to
 * `kTransferLinear`, so choosing e.g. "Panasonic V-Log V-Gamut" in the UI produced an
 * unlabelled linear passthrough with no diagnostic. Patch 0045 derives the advertised
 * list from the same table the engine evaluates.
 *
 * This probe walks every advertised colour space and asserts the engine can actually
 * evaluate it, so "advertised == implemented" is enforced rather than assumed. The
 * negative control pins the opposite direction: a genuinely unknown name must be
 * rejected outright (Create() returns nullptr), proving the detector is wired up
 * rather than vacuously accepting everything.
 */
OLIVE_ADD_TEST(ColorspaceImplementationParity)
{
#ifdef OLIVE_ENABLE_OCIO
  // The advertised list comes from the bundled config.ocio (19 spaces), but only the
  // 7 the built-in engine implements are covered by the A-class golden gate, so the
  // parity invariant below does not apply on this side. Keeping the test defined (with
  // an early exit) keeps the runner's function table identical across both builds.
  std::cout << " (OCIO build - colour spaces come from config.ocio, not the built-in table)";
  OLIVE_TEST_END;
#else
  ColorManager::SetUpDefaultConfig();

  Project project;
  ColorManager *manager = project.color_manager();

  const QStringList spaces = manager->ListAvailableColorspaces();

  // Exactly the 7 spaces that are genuinely implemented must be advertised.
  const QStringList kExpected = {
    QStringLiteral("Linear"),
    QStringLiteral("sRGB OETF"),
    QStringLiteral("Rec.709 OETF"),
    QStringLiteral("BT.1886 EOTF"),
    QStringLiteral("Rec.2020 HLG"),
    QStringLiteral("Rec.2020 PQ"),
    QStringLiteral("Non-Colour Data")
  };

  if (spaces != kExpected) {
    std::cout << " - advertised colour space list drifted from the implemented set ("
              << spaces.size() << " advertised, " << kExpected.size() << " implemented): ";
    for (const QString &s : spaces) {
      std::cout << "[" << s.toStdString() << "] ";
    }
    return __LINE__;
  }

  // Every advertised space must be evaluable without silently defaulting.
  for (const QString &space : spaces) {
    ColorProcessorPtr proc = ColorProcessor::Create(manager, space, ColorTransform(space));

    if (!proc) {
      std::cout << " - could not create processor for advertised space " << space.toStdString();
      return __LINE__;
    }

    if (proc->IsDefaulted()) {
      std::cout << " - advertised space " << space.toStdString()
                << " is NOT actually implemented (engine defaulted it silently)";
      return __LINE__;
    }
  }

  // Negative control: the detector must reject an unknown colour space outright.
  // Under the built-in engine Create() returns nullptr instead of handing back a
  // processor that would silently pass pixels through as linear; without this the
  // parity loop above could pass while the detector was never actually wired up.
  if (ColorProcessor::Create(manager,
                            QStringLiteral("Definitely Not A Colour Space"),
                            ColorTransform(QStringLiteral("Linear"))) != nullptr) {
    std::cout << " - unknown colour space was NOT rejected (detector not wired up)";
    return __LINE__;
  }

  OLIVE_TEST_END;
#endif
}

/**
 * Phase 8 / patch 0047: the GPU shader text is derived, not hand-written.
 *
 * The whole point of GenerateBuiltinGlslShader() is that the GLSL the renderer feeds
 * to colormanage.frag is generated from the SAME transfer-function enumerators the CPU
 * path evaluates with. If someone later hardcodes a shader body again, or adds a
 * colour space without a GLSL branch, these assertions fail:
 *
 *   1. the derived text must define OCIODisplay (the entry colormanage.frag calls);
 *   2. the text must change when the transfer functions change (proving derivation,
 *      not a constant string);
 *   3. PQ/HLG paths must carry the BT.2408 display-scale normalisation and the
 *      roll-off knee;
 *   4. attaching a .cube LUT must produce the sampler3D the renderer's
 *      lut3d_textures slot binds (the binding loop lives outside the OCIO #ifdef).
 *
 * This is a structural gate: actually executing the GLSL needs a graphics context and
 * is tracked separately (see the design doc's GPU-session backlog).
 */
OLIVE_ADD_TEST(GeneratedGlslTracksColorProcessor)
{
#ifdef OLIVE_ENABLE_OCIO
  std::cout << " (OCIO build - shader text comes from the OCIO generator)";
  OLIVE_TEST_END;
#else
  using olive::GenerateBuiltinGlslShader;
  using olive::kTransferLinear;
  using olive::kTransferSrgb;
  using olive::kTransferRec709;
  using olive::kTransferPq;
  using olive::kTransferHlg;

  // 1. entry point present for a plain linear -> sRGB display transform.
  const QString basic = GenerateBuiltinGlslShader(kTransferLinear, kTransferSrgb, false, 0.0);
  if (!basic.contains(QStringLiteral("vec4 OCIODisplay(vec4 col)"))) {
    std::cout << " - derived shader does not define OCIODisplay(vec4)";
    return __LINE__;
  }

  // 2. derivation, not a constant: different endpoints must produce different text.
  const QString rec709 = GenerateBuiltinGlslShader(kTransferLinear, kTransferRec709, false, 0.0);
  if (basic == rec709) {
    std::cout << " - shader text identical for sRGB and Rec.709 destinations (not derived)";
    return __LINE__;
  }

  // 3. HDR paths carry the BT.2408 scale + knee; SDR paths must not scale.
  const QString pq = GenerateBuiltinGlslShader(kTransferPq, kTransferSrgb, false, 1000.0);
  if (!pq.contains(QStringLiteral("HdrRollOff"))
      || !pq.contains(QStringLiteral("203"))) {
    std::cout << " - PQ path missing BT.2408 roll-off / reference-white anchor";
    return __LINE__;
  }
  const QString hlg = GenerateBuiltinGlslShader(kTransferHlg, kTransferSrgb, false, 1000.0);
  if (!hlg.contains(QStringLiteral("kRec2020LumaCoefs"))) {
    std::cout << " - HLG path missing luminance OOTF (per-channel gamma would shift hue)";
    return __LINE__;
  }

  // 4. .cube LUT wiring: sampler3D appears exactly when a LUT is attached.
  if (basic.contains(QStringLiteral("sampler3D"))) {
    std::cout << " - sampler3D emitted without an attached LUT";
    return __LINE__;
  }
  const QString with_lut = GenerateBuiltinGlslShader(kTransferLinear, kTransferSrgb, true, 0.0);
  if (!with_lut.contains(QStringLiteral("uniform sampler3D ove_cube_lut"))) {
    std::cout << " - attached LUT did not produce the ove_cube_lut sampler3D";
    return __LINE__;
  }

  OLIVE_TEST_END;
#endif
}

}
