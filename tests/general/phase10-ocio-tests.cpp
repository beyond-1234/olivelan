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
 * Phase 10 gate: the Primary grading node must only be advertised when the
 * OpenColorIO backend is compiled in. Under OLIVE_ENABLE_OCIO=OFF it still
 * loads from existing projects, but must not appear in the create menu as a
 * node that silently forwards its input.
 */

#include "testutil.h"

#include "node/factory.h"
#include "node/node.h"

namespace olive {

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

#ifdef OLIVE_ENABLE_OCIO
  if (hidden) {
    delete node;
    NodeFactory::Destroy();
    std::cout << " - OCIO build hides the Primary grading node";
    return __LINE__;
  }
#else
  if (!hidden) {
    delete node;
    NodeFactory::Destroy();
    std::cout << " - built-in build advertises a no-op Primary grading node";
    return __LINE__;
  }
#endif

  delete node;
  NodeFactory::Destroy();
  OLIVE_TEST_END;
}

} // namespace olive
