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

  // OCIO is the only supported colour pipeline, so the bundled config must exist.
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

  OLIVE_TEST_END;
}

/**
 * Phase 15: OCIO-only build. Colour spaces are supplied by config.ocio, so the
 * former built-in "advertised == implemented" probe no longer applies. Keep the
 * test registered so the runner layout stays stable.
 */
/**
 * Phase 15: legacy project loaders and the removed built-in engine used role
 * names rather than canonical OCIO colour-space names. Those aliases must migrate
 * explicitly, not silently become the project default.
 */
OLIVE_ADD_TEST(LegacyColorSpaceAliases)
{
  ColorManager::SetUpDefaultConfig();
  Project project;
  ColorManager *manager = project.color_manager();

  if (manager->GetCompliantColorSpace(QStringLiteral("scene_linear")) != QStringLiteral("Linear")) {
    std::cout << " - scene_linear did not migrate to Linear";
    return __LINE__;
  }

  if (manager->GetCompliantColorSpace(QStringLiteral("Linear Raw")) != QStringLiteral("Linear")) {
    std::cout << " - Linear Raw did not migrate to Linear";
    return __LINE__;
  }

  if (manager->GetCompliantColorSpace(QStringLiteral("sRGB")) != QStringLiteral("sRGB OETF")) {
    std::cout << " - sRGB did not migrate to sRGB OETF";
    return __LINE__;
  }

  if (manager->GetCompliantColorSpace(QStringLiteral("compositing_log")) != QStringLiteral("Filmic Log Encoding")) {
    std::cout << " - compositing_log did not migrate to Filmic Log Encoding";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(InvalidColorSpaceRejected)
{
  ColorManager::SetUpDefaultConfig();
  Project project;
  ColorManager *manager = project.color_manager();

  ColorProcessorPtr invalid = ColorProcessor::Create(
        manager,
        QStringLiteral("Definitely Not A Colour Space"),
        ColorTransform(QStringLiteral("Linear")));

  if (invalid) {
    std::cout << " - unknown colour space unexpectedly produced a processor";
    return __LINE__;
  }

  if (manager->GetCompliantColorSpace(QStringLiteral("Definitely Not A Colour Space"))
      != QStringLiteral("Definitely Not A Colour Space")) {
    std::cout << " - unknown colour space was silently replaced by a default";
    return __LINE__;
  }

  if (manager->GetCompliantColorSpace(QString()) != manager->GetDefaultInputColorSpace()) {
    std::cout << " - empty colour space did not resolve to the project default";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ColorspaceImplementationParity)
{
  std::cout << " (OCIO build - colour spaces come from config.ocio)";
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(GeneratedGlslTracksColorProcessor)
{
  std::cout << " (OCIO build - shader text comes from the OCIO GPU shader generator)";
  OLIVE_TEST_END;
}

}
