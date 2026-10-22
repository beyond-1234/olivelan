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

#include "ocioutils.h"

#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;

namespace olive {

// Patch 0038: OCIO-free role names (see ocioutils.h). Kept in sync with the
// `roles:` block of app/render/ocioconf/config.ocio.
const QString OCIOUtils::kRoleDefault = QStringLiteral("default");
const QString OCIOUtils::kReferenceSpaceSceneLinear = QStringLiteral("scene_linear");
const QString OCIOUtils::kReferenceSpaceCompositingLog = QStringLiteral("compositing_log");

int OCIOUtils::GetOCIOBitDepthFromPixelFormat(PixelFormat format)
{
  switch (format) {
  case PixelFormat::U8:
    return static_cast<int>(OCIO::BIT_DEPTH_UINT8);
  case PixelFormat::U16:
    return static_cast<int>(OCIO::BIT_DEPTH_UINT16);
    break;
  case PixelFormat::F16:
    return static_cast<int>(OCIO::BIT_DEPTH_F16);
    break;
  case PixelFormat::F32:
    return static_cast<int>(OCIO::BIT_DEPTH_F32);
    break;
  case PixelFormat::INVALID:
  case PixelFormat::COUNT:
    break;
  }

  return static_cast<int>(OCIO::BIT_DEPTH_UNKNOWN);
}

}
