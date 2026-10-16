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
#include "render/builtincolorspaces.h"
#include "render/colortransform.h"
#include "render/cubelut.h"

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
   * @brief Patch 0045 (Phase 8): true when the processor was asked for a colour
   *        space the engine cannot evaluate.
   *
   * Under the built-in engine (OLIVE_ENABLE_OCIO=OFF) an unrecognised name is a hard
   * error: Create() returns nullptr instead of building a processor that would
   * silently pass pixels through as linear. This accessor therefore always reports
   * false on a successfully constructed processor - it exists so the parity gate can
   * assert that no advertised space can ever reach the defaulted state.
   *
   * Under OpenColorIO (ON) the config resolves names at construction time, so an
   * unresolvable name throws rather than yielding an unconverted passthrough.
   */
  bool IsDefaulted() const;

  /**
   * @brief Patch 0047 (Phase 8): the CPU-side transfer functions this processor
   *        was built for.
   *
   * The GPU shader text is derived from these same enumerators (see
   * GenerateBuiltinGlslShader), so the CPU and GPU paths cannot drift apart.
   *
   * F-9 (HAL/Phase6-8 review) caveat: that guarantee holds for the *transfer curves* of
   * sRGB / Rec.709 / BT.1886 (identical maths on both sides). It does NOT hold for HLG/PQ
   * today: the GPU path additionally applies the BT.2100 HLG OOTF, the BT.2408 display
   * normalisation and the mastering-peak roll-off, while the CPU ConvertColor()/
   * ConvertFrame() path does not. Closing that gap is GPU backlog #2; until then the two
   * paths intentionally differ on HDR content.
   */
  BuiltinTransferFunction GetInputTransferFunction() const;
  BuiltinTransferFunction GetOutputTransferFunction() const;

  /**
   * @brief Patch 0047 (Phase 8): attach a user .cube LUT.
   *
   * When set, the generated GLSL samples it through the renderer's existing
   * 3D-texture slot, in addition to applying the transfer functions.
   */
  void SetCubeLUT(std::shared_ptr<CubeLUT> lut);
  std::shared_ptr<CubeLUT> GetCubeLUT() const;

private:
  ColorProcessor();

  struct Impl;

  std::unique_ptr<Impl> impl_;

  // Patch 0037: the OCIO-typed constructor/accessor live in common/ociobridge.h,
  // which only the translation units that actually touch OpenColorIO include.
  friend struct ColorProcessorOCIO;

};

using ColorProcessorChain = QVector<ColorProcessorPtr>;

/**
 * @brief Patch 0047 (Phase 8): generate the GLSL body for a built-in colour transform.
 *
 * Defined next to the CPU implementation in `colorprocessor.cpp` so both paths are
 * driven by the same enumerators and matrix constants.
 */
QString GenerateBuiltinGlslShader(BuiltinTransferFunction src,
                                  BuiltinTransferFunction dst,
                                  bool has_cube_lut,
                                  double peak_nits);

/**
 * @brief GLSL helper functions the built-in engine appends to the generated shader.
 *
 * Kept next to GenerateBuiltinGlslShader() so tests can assert on the text that actually reaches
 * the GPU. ChromaKeyNode calls SceneLinearToCIEXYZ_d65() directly, so this text is part of the
 * assembled colormanage.frag.
 *
 * Why it lives here rather than inline in renderer.cpp (patch 0064): a GLSL reserved word used as
 * an identifier in this text is a *runtime* shader-compile failure that no C++ compiler can catch.
 * Patch 0061 shipped exactly that - the parameter was literally named `in` - and the viewer went
 * black because the whole built-in colour shader failed to compile. Untestable text is how that
 * survived; this makes it testable.
 */
QString GenerateBuiltinGlslAuxFunctions();

}

Q_DECLARE_METATYPE(olive::ColorProcessorPtr)

#endif // COLORPROCESSOR_H
