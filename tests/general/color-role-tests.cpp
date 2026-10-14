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

}
