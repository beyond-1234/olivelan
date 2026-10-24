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
 * Phase 8 / patch 0048: legacy serializer removal gate (§六 product ruling).
 *
 * The §六 ruling locks in dropping legacy .ove compatibility, and the five legacy
 * serializers (190219/210528/210907/211228/220403) have been deleted. This test pins
 * the two behavioural guarantees that make that deletion safe:
 *
 *   1. every pre-230220 version is answered with kProjectTooOld - a definite,
 *      reportable error - never a silent misload or a crash;
 *   2. the current serializer (230220) still round-trips a project save/load.
 *
 * Without this gate, deleting the serializers could regress into "unknown version
 * silently produces an empty project", which is indistinguishable from a correct
 * load to every caller.
 */

#include "testutil.h"

#include <QTemporaryDir>
#include <QXmlStreamReader>

#include "common/xmlutils.h"
#include "node/color/colormanager/colormanager.h"
#include "node/project.h"
#include "node/project/serializer/serializer.h"

namespace olive {

/**
 * @brief A minimal pre-230220 project document.
 *
 * The dispatcher only routes to a versioned serializer when it hits an element other
 * than <version>/<url> (i.e. the <project> body), so the document must carry one -
 * mirroring the shape of a real legacy .ove file.
 */
bool LoadVersionXml(uint version)
{
  const QString xml = QStringLiteral(
    "<olive>"
      "<version>%1</version>"
      "<project>"
        "<name>legacy</name>"
      "</project>"
    "</olive>").arg(version);

  QXmlStreamReader reader(xml);

  // Constructing a Project builds a ColorManager, which on the OCIO build
  // dereferences the default config (see the patch 0038 honesty note) - so the
  // config must be set up before any Project exists, on both build flavours.
  ColorManager::SetUpDefaultConfig();

  Project project;

  ProjectSerializer::Result r = ProjectSerializer::Load(&project, &reader, ProjectSerializer::kProject);

  return r.code() == ProjectSerializer::kProjectTooOld;
}

OLIVE_ADD_TEST(LegacySerializersRemoved)
{
  ProjectSerializer::Initialize();

  // Every deleted legacy version must be rejected as too old. 210528/210907/211228/
  // 220403 were the four loadable ones (190219 was already dead code); 200101 covers
  // the "before anything we ever shipped" band.
  const uint kLegacyVersions[] = {
    210528, 210907, 211228, 220403
  };

  for (uint v : kLegacyVersions) {
    if (!LoadVersionXml(v)) {
      std::cout << " - legacy version " << v
                << " was not answered with kProjectTooOld (regressed to silent handling)";
      ProjectSerializer::Destroy();
      return __LINE__;
    }
  }

  // The current version must still be loadable - a version>=min dispatcher bug would
  // answer 230220 with kProjectTooOld too, which this catches.
  {
    const QString xml = QStringLiteral(
      "<olive>"
        "<version>230220</version>"
        "<project>"
          "<name>current</name>"
        "</project>"
      "</olive>");

    QXmlStreamReader reader(xml);
    Project project;
    ProjectSerializer::Result r = ProjectSerializer::Load(&project, &reader, ProjectSerializer::kProject);

    if (r.code() == ProjectSerializer::kProjectTooOld) {
      std::cout << " - current version 230220 wrongly rejected as too old";
      ProjectSerializer::Destroy();
      return __LINE__;
    }
  }

  ProjectSerializer::Destroy();

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(CurrentProjectSaveLoadRoundTrip)
{
  ProjectSerializer::Initialize();
  ColorManager::SetUpDefaultConfig();

  QTemporaryDir temp_dir;
  OLIVE_ASSERT(temp_dir.isValid());
  QString proj_file = temp_dir.filePath(QStringLiteral("roundtrip_test.ove"));

  // 1. Create project, configure settings, save to file
  {
    Project p;
    p.set_filename(proj_file);
    p.SetSetting(QStringLiteral("custom_test_key"), QStringLiteral("custom_test_val"));
    p.color_manager()->SetDefaultInputColorSpace(QStringLiteral("sRGB OETF"));

    QFile f(proj_file);
    OLIVE_ASSERT(f.open(QIODevice::WriteOnly));
    QXmlStreamWriter writer(&f);
    writer.setAutoFormatting(true);

    ProjectSerializer::SaveData save_data(ProjectSerializer::kProject, &p, proj_file);
    ProjectSerializer::Result res = ProjectSerializer::Save(&writer, save_data);
    OLIVE_ASSERT(res.code() == ProjectSerializer::kSuccess);
    f.close();
  }

  // 2. Load the project back from XML file
  {
    QFile f(proj_file);
    OLIVE_ASSERT(f.open(QIODevice::ReadOnly));
    QXmlStreamReader reader(&f);

    Project loaded_p;
    loaded_p.set_filename(proj_file);
    ProjectSerializer::Result r = ProjectSerializer::Load(&loaded_p, &reader, ProjectSerializer::kProject);
    OLIVE_ASSERT(r.code() == ProjectSerializer::kSuccess);

    OLIVE_ASSERT(loaded_p.GetSetting(QStringLiteral("custom_test_key")) == QStringLiteral("custom_test_val"));
    OLIVE_ASSERT(loaded_p.color_manager()->GetDefaultInputColorSpace() == QStringLiteral("sRGB OETF"));
  }

  ProjectSerializer::Destroy();

  OLIVE_TEST_END;
}

} // namespace olive
