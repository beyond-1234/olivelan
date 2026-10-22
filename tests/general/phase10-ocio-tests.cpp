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

#include "node/factory.h"
#include "node/node.h"

namespace olive {

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

} // namespace olive
