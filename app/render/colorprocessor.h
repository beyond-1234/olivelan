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

#ifndef COLORPROCESSOR_H
#define COLORPROCESSOR_H

#include <memory>

#include "codec/frame.h"
#include "render/colortransform.h"

namespace olive {

class ColorManager;

class ColorProcessor;
using ColorProcessorPtr = std::shared_ptr<ColorProcessor>;

class ColorProcessor
{
public:
  enum Direction {
    kNormal,
    kInverse
  };

  ColorProcessor(ColorManager* config, const QString& input, const ColorTransform& dest_space, Direction direction = kNormal);
  ~ColorProcessor();

  DISABLE_COPY_MOVE(ColorProcessor)

  static ColorProcessorPtr Create(ColorManager* config, const QString& input, const ColorTransform& dest_space, Direction direction = kNormal);

  void ConvertFrame(FramePtr f);
  void ConvertFrame(Frame* f);

  Color ConvertColor(const Color &in);

  const char *id() const;

  /**
   * @brief Patch 0045 (Phase 8): true when either endpoint was a colour space the
   *        engine could not evaluate, so the processor fell back to a default.
   *
   * Under the built-in engine (OLIVE_ENABLE_OCIO=OFF) the requested colour space name
   * is remapped to Rec.709 OETF and a warning is emitted; this accessor lets callers
   * and tests detect that remap instead of having the degradation pass unnoticed.
   *
   * Under OpenColorIO (ON) the config resolves the names itself, so this is always
   * false - an unresolvable name fails at processor construction, not silently.
   */
  bool IsDefaulted() const;

private:
  ColorProcessor();

  struct Impl;

  std::unique_ptr<Impl> impl_;

  // Patch 0037: the OCIO-typed constructor/accessor live in common/ociobridge.h,
  // which only the translation units that actually touch OpenColorIO include.
  friend struct ColorProcessorOCIO;

};

using ColorProcessorChain = QVector<ColorProcessorPtr>;

}

Q_DECLARE_METATYPE(olive::ColorProcessorPtr)

#endif // COLORPROCESSOR_H
