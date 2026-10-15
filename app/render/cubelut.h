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

#ifndef OLIVE_CUBELUT_H
#define OLIVE_CUBELUT_H

#include <QString>
#include <QVector>

namespace olive {

/**
 * @brief Patch 0040 (Phase 8): lightweight Adobe .cube LUT reader.
 *
 * This is part of the built-in colour engine. OpenColorIO can parse .cube files
 * and bake them into its generated GPU shader; without OCIO we need our own
 * reader so DisplayTransformNode can still mount a user LUT.
 *
 * Supported syntax:
 *   - "#" comment lines and blank lines;
 *   - TITLE "..." (ignored);
 *   - LUT_1D_SIZE <n> / LUT_3D_SIZE <n>;
 *   - DOMAIN_MIN r g b / DOMAIN_MAX r g b (default 0 0 0 / 1 1 1);
 *   - n (1D) or n^3 (3D) rows of "r g b", red varying fastest in the 3D case.
 *
 * Sampling is trilinear for 3D LUTs and per-channel linear for 1D LUTs, both
 * clamped to the domain.
 */
class CubeLUT
{
public:
  CubeLUT() = default;

  /**
   * @brief Read a .cube file from disk.
   */
  bool Load(const QString &filename);

  /**
   * @brief Parse .cube text that has already been read into memory.
   *
   * Exposed separately so the format can be unit-tested without touching disk.
   */
  bool LoadFromString(const QString &text);

  bool IsValid() const { return size_ > 0; }
  bool Is1D() const { return is_1d_; }

  /**
   * @brief Patch 0047 (Phase 8): true for a 3D LUT (uploadable to GL_TEXTURE_3D).
   */
  bool Is3D() const { return !is_1d_; }

  /**
   * @brief Patch 0047 (Phase 8): texture edge length - `size` for both 1D and 3D
   *        LUTs (a 3D LUT of `LUT_3D_SIZE n` is an n^3 cubic texture).
   */
  int texture_edge_length() const { return size_; }

  int size() const { return size_; }

  /**
   * @brief Raw LUT entries as RGB triples (3 * size^3 floats for 3D, 3 * size for 1D).
   */
  const QVector<float> &data() const { return data_; }

  const float *domain_min() const { return domain_min_; }
  const float *domain_max() const { return domain_max_; }

  /**
   * @brief Sample the LUT at (r, g, b), writing the result to out_rgb[0..2].
   *
   * Values outside the LUT's domain are clamped.
   */
  void Sample(float r, float g, float b, float *out_rgb) const;

private:
  bool Parse(const QString &text);

  void Sample1D(int channel, float v, float *out) const;
  void Sample3D(float r, float g, float b, float *out) const;

  int size_ = 0;
  bool is_1d_ = false;
  QVector<float> data_;
  float domain_min_[3] = {0.0f, 0.0f, 0.0f};
  float domain_max_[3] = {1.0f, 1.0f, 1.0f};
};

}

#endif // OLIVE_CUBELUT_H
