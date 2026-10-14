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
};

}

#endif // OCIOUTILS_H
