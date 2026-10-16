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

#include <cmath>

#include <QFile>
#include <QRegularExpression>
#include <QString>

#include "common/ocioutils.h"
#include "core.h"
#include "node/color/colormanager/colormanager.h"
#include "node/project.h"
#include "render/builtincolorspaces.h"
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

  // 5. F-2 (HAL/Phase6-8 review): the constants emitted into the GLSL must equal the
  //    CPU-side constants in render/builtincolorspaces.h (single source of truth) to
  //    within 1e-7. This is the assertion that catches the historical drift where the
  //    generator hardcoded kPqM2/kPqC2/kPqC3 without the ST 2084 *128 / *32 factors.
  {
    auto extract_float = [](const QString &text, const QString &name, bool *ok) -> double {
      const QString needle = QStringLiteral("const float %1 = ").arg(name);
      const int start = text.indexOf(needle);
      if (start < 0) {
        *ok = false;
        return 0.0;
      }
      const int value_start = start + needle.size();
      const int end = text.indexOf(QLatin1Char(';'), value_start);
      if (end < 0) {
        *ok = false;
        return 0.0;
      }
      return text.mid(value_start, end - value_start).toDouble(ok);
    };

    struct EmittedConstant {
      const char *glsl_name;
      double cpu_value;
    };
    const EmittedConstant expected[] = {
      { "kHlgA", olive::kHlgA },
      { "kHlgB", olive::kHlgB },
      { "kHlgC", olive::kHlgC },
      { "kHlgSystemGamma", olive::kHlgSystemGamma },
      { "kDisplayRefWhiteNits", olive::kDisplayReferenceWhiteNits },
      { "kPqM1", olive::kPqM1 },
      { "kPqM2", olive::kPqM2 },
      { "kPqC1", olive::kPqC1 },
      { "kPqC2", olive::kPqC2 },
      { "kPqC3", olive::kPqC3 },
    };

    for (const EmittedConstant &c : expected) {
      bool ok = false;
      const double emitted = extract_float(pq, QString::fromUtf8(c.glsl_name), &ok);
      if (!ok) {
        std::cout << " - GLSL is missing constant " << c.glsl_name;
        return __LINE__;
      }
      if (std::abs(emitted - c.cpu_value) > 1e-7) {
        std::cout << " - GLSL " << c.glsl_name << "=" << emitted
                  << " disagrees with CPU value " << c.cpu_value;
        return __LINE__;
      }
    }

    // The corrected ST 2084 values are on the order of 78.8 / 18.9, not ~0.6. Guard the
    // magnitude directly so a future edit cannot pass by re-introducing the un-scaled form.
    if (pq.contains(QStringLiteral("kPqM2 = 0.61597")) || pq.contains(QStringLiteral("kPqC2 = 0.58911"))) {
      std::cout << " - GLSL PQ constants are back to the un-scaled ST 2084 form";
      return __LINE__;
    }
  }

  // 6. F-4 (HAL/Phase6-8 review): the production default path passes peak_nits = 0,
  //    which must bake a roll-off that short-circuits; a positive peak must be embedded
  //    verbatim. This closes the "the gate used 1000.0, so 0 was never exercised" hole.
  if (!basic.contains(QStringLiteral("HdrRollOff(col.r, 0)"))) {
    std::cout << " - default (peak_nits=0) shader does not short-circuit the roll-off";
    return __LINE__;
  }
  if (!pq.contains(QStringLiteral("HdrRollOff(col.r, 1000)"))) {
    std::cout << " - a positive mastering peak did not reach HdrRollOff";
    return __LINE__;
  }

  // 7. Patch 0064: no GLSL reserved word may be used as an identifier in the text that reaches the
  //    GPU. Patch 0061 shipped `vec4 SceneLinearToCIEXYZ_d65(vec4 in)` - a parameter literally
  //    named `in` - so the built-in colour shader failed to compile at *runtime* and the viewer
  //    went black. No C++ compiler can catch that, and the tests only looked at
  //    GenerateBuiltinGlslShader(), so the CIEXYZ helper was never asserted on at all. It now comes
  //    from GenerateBuiltinGlslAuxFunctions() so it can be.
  {
    const QString aux = GenerateBuiltinGlslAuxFunctions();

    // `<type> <reserved>` is the shape of the 0061 defect. Legitimate uses of these words are as
    // *qualifiers* before a type (`in vec3 x`), which this deliberately does not match.
    static const char *kReserved[] = { "in",       "out",      "inout",     "uniform",
                                       "varying",  "attribute", "layout",   "flat",
                                       "smooth",   "centroid", "patch",     "this",
                                       "class",    "template", "namespace", "typedef",
                                       "asm",      "union",    "enum",      "static" };

    // Assert on what the renderer actually assembles, not just the helper.
    const QString assembled = basic + aux;

    for (const char *kw : kReserved) {
      const QRegularExpression re(
          QStringLiteral("\\b(float|int|uint|bool|vec2|vec3|vec4|ivec2|ivec3|ivec4|bvec2|bvec3|bvec4|"
                         "mat2|mat3|mat4)\\s+")
          + QString::fromLatin1(kw) + QStringLiteral("\\b"));
      if (re.match(assembled).hasMatch()) {
        std::cout << " - GLSL reserved word used as an identifier: " << kw;
        return __LINE__;
      }
    }

    // Positive half: the helper is present and its parameter carries the fixed name.
    if (!aux.contains(QStringLiteral("vec4 SceneLinearToCIEXYZ_d65(vec4 c)"))) {
      std::cout << " - CIEXYZ helper missing, or its parameter is not named `c`";
      return __LINE__;
    }
  }

  OLIVE_TEST_END;
#endif
}

}
