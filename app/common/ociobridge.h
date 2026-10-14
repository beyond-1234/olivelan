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

#ifndef OCIOBRIDGE_H
#define OCIOBRIDGE_H

/**
 * Patch 0037 (Phase 8): OpenColorIO bridge.
 *
 * The public ColorManager / ColorProcessor headers used to expose OCIO::* types
 * directly, which dragged <OpenColorIO/OpenColorIO.h> into every one of their
 * ~38 transitive includers. Those headers are now Pimpl-isolated and OCIO-free;
 * the OCIO-typed constructors and accessors live here instead.
 *
 * This is the ONE header that still includes OpenColorIO, and it is only
 * included by the translation units that genuinely touch OCIO. Anything that
 * merely holds a ColorProcessorPtr keeps including the plain headers.
 */

#include <QString>
#include <QStringList>

#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;

#include "node/color/colormanager/colormanager.h"
#include "render/colorprocessor.h"

namespace olive {

/**
 * @brief OCIO-typed accessors for ColorManager (declared as a friend).
 */
struct ColorManagerOCIO
{
  static OCIO::ConstConfigRcPtr GetConfig(const ColorManager *manager);

  static OCIO::ConstConfigRcPtr CreateConfigFromFile(const QString &filename);

  static OCIO::ConstConfigRcPtr GetDefaultConfig();

  static QStringList ListAvailableColorspaces(OCIO::ConstConfigRcPtr config);
};

/**
 * @brief OCIO-typed accessors for ColorProcessor (declared as a friend).
 */
struct ColorProcessorOCIO
{
  static ColorProcessorPtr Create(OCIO::ConstProcessorRcPtr processor);

  static OCIO::ConstProcessorRcPtr GetProcessor(const ColorProcessor *processor);
};

}

#endif // OCIOBRIDGE_H
