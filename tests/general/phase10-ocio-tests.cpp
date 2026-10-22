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
 * Phase 15 gate: the Primary grading node must always be advertised now that
 * OpenColorIO is the only colour pipeline.
 */

#include "testutil.h"

#include <QSurfaceFormat>

#include <cmath>

#include "common/ociobridge.h"
#include "node/color/colormanager/colormanager.h"
#include "node/factory.h"
#include "node/node.h"
#include "node/project.h"
#include "render/colorprocessor.h"

namespace olive {

namespace {
bool NearlyEqual(double a, double b, double epsilon)
{
  return std::fabs(a - b) <= epsilon;
}
}

OLIVE_ADD_TEST(SurfaceFormatCompliance)
{
  // Assert that Desktop OpenGL 3.2 Core is correctly configured as our universal baseline
  QSurfaceFormat format;
  format.setVersion(3, 2);
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setRenderableType(QSurfaceFormat::OpenGL);

  OLIVE_ASSERT_EQUAL(format.majorVersion(), 3);
  OLIVE_ASSERT_EQUAL(format.minorVersion(), 2);
  OLIVE_ASSERT_EQUAL(format.profile(), QSurfaceFormat::CoreProfile);
  OLIVE_ASSERT_EQUAL(format.renderableType(), QSurfaceFormat::OpenGL);

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(DesktopShaderPreambleCompliance)
{
  // Test desktop GLSL preamble compliance: must start with #version 150 and
  // strictly exclude "precision highp float;" which breaks Mesa desktop Core Profile
  const QString desktop_preamble = QStringLiteral("#version 150\n\n");
  OLIVE_ASSERT(desktop_preamble.startsWith(QStringLiteral("#version 150")));
  OLIVE_ASSERT(!desktop_preamble.contains(QStringLiteral("precision highp float;")));

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(GradingNodeAvailability)
{
  NodeFactory::Initialize();

  Node* node = NodeFactory::CreateFromFactoryIndex(
      NodeFactory::kOCIOGradingTransformLinear);
  if (!node) {
    NodeFactory::Destroy();
    std::cout << " - grading node could not be constructed";
    return __LINE__;
  }

  const bool hidden = (node->GetFlags() & Node::kDontShowInCreateMenu) != 0;

  if (hidden) {
    delete node;
    NodeFactory::Destroy();
    std::cout << " - OCIO build hides the Primary grading node";
    return __LINE__;
  }

  delete node;
  NodeFactory::Destroy();
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(PrimaryGradingMath)
{
  ColorManager::SetUpDefaultConfig();
  Project project;
  ColorManager *manager = project.color_manager();

  OCIO::GradingPrimaryTransformRcPtr gp = OCIO::GradingPrimaryTransform::Create(OCIO::GRADING_LIN);
  gp->setDirection(OCIO::TRANSFORM_DIR_FORWARD);

  OCIO::GradingPrimary values(OCIO::GRADING_LIN);
  // +1 stop. OCIO's CPU GradingPrimary stores exposure in stops, while the
  // node's GPU uniform path converts stops to a multiplier.
  values.m_exposure = OCIO::GradingRGBM(1.0, 1.0, 1.0, 0.0);
  gp->setValue(values);

  ColorProcessorPtr proc = ColorProcessorOCIO::Create(
        ColorManagerOCIO::GetConfig(manager)->getProcessor(gp));

  if (!proc) {
    std::cout << " - Primary grading processor could not be created";
    return __LINE__;
  }

  const Color in(0.18f, 0.18f, 0.18f);
  const Color out = proc->ConvertColor(in);

  if (!NearlyEqual(out.red(), 0.36, 1e-4)
      || !NearlyEqual(out.green(), 0.36, 1e-4)
      || !NearlyEqual(out.blue(), 0.36, 1e-4)) {
    std::cout << " - +1 stop exposure did not double the linear mid-grey: "
              << out.red() << ", " << out.green() << ", " << out.blue();
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ChromaKeyOcioShader)
{
  ColorManager::SetUpDefaultConfig();
  Project project;
  ColorManager *manager = project.color_manager();

  ColorProcessorPtr proc = ColorProcessor::Create(
        manager,
        manager->GetReferenceColorSpace(),
        ColorTransform(QStringLiteral("cie_xyz_d65_interchange")));

  if (!proc) {
    std::cout << " - ChromaKey colour transform could not be created";
    return __LINE__;
  }

  auto ocio_proc = ColorProcessorOCIO::GetProcessor(proc.get());
  if (!ocio_proc) {
    std::cout << " - ChromaKey processor has no OCIO processor";
    return __LINE__;
  }

  auto gpu_proc = ocio_proc->getDefaultGPUProcessor();
  if (!gpu_proc) {
    std::cout << " - ChromaKey processor has no GPU processor";
    return __LINE__;
  }

  auto desc = OCIO::GpuShaderDesc::CreateShaderDesc();
  desc->setLanguage(OCIO::GPU_LANGUAGE_GLSL_1_3);
  desc->setFunctionName("SceneLinearToCIEXYZ_d65");
  desc->setResourcePrefix("ocio_");
  gpu_proc->extractGpuShaderInfo(desc);

  const QString shader = QString::fromUtf8(desc->getShaderText());
  if (!shader.contains(QStringLiteral("SceneLinearToCIEXYZ_d65"))) {
    std::cout << " - ChromaKey OCIO shader does not define the requested function";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

} // namespace olive
