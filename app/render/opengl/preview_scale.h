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

#ifndef OLIVE_PREVIEW_SCALE_H
#define OLIVE_PREVIEW_SCALE_H

#include <cstdint>

#if defined(__CUDACC__)
#define OLIVE_PREVIEW_HOST_DEVICE __host__ __device__
#else
#define OLIVE_PREVIEW_HOST_DEVICE
#endif

namespace olive {

/**
 * @brief The CUDA/IOSurface preview work size for a source frame and preview divider.
 *
 * The hardware pipelines receive the same divider the software path uses, but must apply it
 * themselves: allocating a full-resolution surface and then relying on the compositor to scale
 * still pays the full NVDEC post-processing and VRAM cost. This helper is shared by the CUDA
 * kernels, the renderer and the unit tests so the destination size cannot drift between them.
 *
 * Integer division with a one-pixel floor matches VideoParams::GetScaledDimension(), which is the
 * contract `Texture::width()`/`height()` expose to the render graph.
 */
struct PreviewScaleDimensions {
  int source_width = 0;
  int source_height = 0;
  int width = 0;
  int height = 0;
};

OLIVE_PREVIEW_HOST_DEVICE inline PreviewScaleDimensions
ComputePreviewScaleDimensions(int source_width, int source_height, int divider)
{
  PreviewScaleDimensions dims;

  if (source_width <= 0 || source_height <= 0) {
    return dims;
  }

  if (divider < 1) {
    divider = 1;
  }

  dims.source_width = source_width;
  dims.source_height = source_height;
  dims.width = source_width / divider;
  dims.height = source_height / divider;

  if (dims.width < 1) {
    dims.width = 1;
  }
  if (dims.height < 1) {
    dims.height = 1;
  }

  return dims;
}

/**
 * @brief Map a destination sample to its source coordinate using pixel centres.
 *
 * Nearest-neighbour sampling at the centre of each destination footprint keeps the full source
 * extent reachable (the last destination sample maps to the last source sample) without turning
 * the scaled kernels into a bilinear filter. Equal source/destination sizes are the identity.
 */
OLIVE_PREVIEW_HOST_DEVICE inline int
MapPreviewSourceCoordinate(int destination, int destination_size, int source_size)
{
  if (destination_size <= 0 || source_size <= 0) {
    return 0;
  }

  if (destination < 0) {
    destination = 0;
  } else if (destination >= destination_size) {
    destination = destination_size - 1;
  }

  // Always include the final source sample. With a non-integer ratio (e.g. 1919 -> 959) the
  // centre formula lands one sample short; pinning the last destination sample keeps the full
  // source width visible instead of cropping the right/bottom edge.
  if (destination == destination_size - 1) {
    return source_size - 1;
  }

  const int64_t mapped =
      (static_cast<int64_t>(destination) * 2 + 1) * source_size / (static_cast<int64_t>(destination_size) * 2);

  return static_cast<int>(mapped >= source_size ? source_size - 1 : mapped);
}

} // namespace olive

#endif // OLIVE_PREVIEW_SCALE_H
