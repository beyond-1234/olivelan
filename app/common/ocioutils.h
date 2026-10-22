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

#ifndef OCIOUTILS_H
#define OCIOUTILS_H

#include <QString>

#include "render/videoparams.h"

namespace olive {

class OCIOUtils
{
public:
  /**
   * @brief Map an Olive PixelFormat to an OpenColorIO bit-depth code.
   *
   * Patch 0037: the OpenColorIO headers are deliberately NOT included here any
   * more, so this header no longer drags them through every includer. The return
   * value is the numeric OCIO::BitDepth value; the few call sites that need the
   * enum cast it back inside their own translation unit.
   */
  static int GetOCIOBitDepthFromPixelFormat(PixelFormat format);

  /**
   * @brief Reference-space role identifiers.
   *
   * Patch 0038: these are the OpenColorIO role names, but declared as plain
   * strings so that project serialization and the colour-management UI can read
   * and write legacy .ove files without including the OpenColorIO headers. That
   * keeps the serializers independent of OpenColorIO types.
   * They must stay in sync with the `roles:` block of
   * app/render/ocioconf/config.ocio.
   */
  static const QString kRoleDefault;                 // "default"
  static const QString kReferenceSpaceSceneLinear;   // "scene_linear"
  static const QString kReferenceSpaceCompositingLog; // "compositing_log"
};

}

#endif // OCIOUTILS_H
