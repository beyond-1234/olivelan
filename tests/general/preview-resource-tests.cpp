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
 * Patch 0135 gates.
 *
 * These are the pure policies behind the two GPU-preview resource fixes:
 *
 *   1. CUDA-NVDEC preview scaling must use the same divider contract as VideoParams.
 *      The kernels are launched at the preview size and read source pixel centres, so
 *      the destination dimensions and coordinate mapping are pinned here independently
 *      of a CUDA device.
 *
 *   2. VideoToolbox IOSurface scratch planes are cached per size with a bounded LRU.
 *      The footprint calculation and eviction order are pinned here so a future change
 *      cannot turn the cache back into an unbounded VRAM leak.
 */

#include "testutil.h"

#include <cstdint>
#include <iostream>
#include <vector>

#include "render/opengl/preview_scale.h"
#include "render/opengl/scratch_cache_policy.h"

namespace olive {

namespace {

bool CheckDimensions(int source_width, int source_height, int divider,
                     int expected_width, int expected_height, int line)
{
  const PreviewScaleDimensions dims =
      ComputePreviewScaleDimensions(source_width, source_height, divider);
  if (dims.source_width != source_width || dims.source_height != source_height
      || dims.width != expected_width || dims.height != expected_height) {
    std::cout << " - line " << line << ": " << source_width << "x" << source_height
              << " / " << divider << " produced " << dims.width << "x" << dims.height
              << ", expected " << expected_width << "x" << expected_height;
    return false;
  }
  return true;
}

bool CheckEvictions(const std::vector<ScratchCacheEntryInfo>& entries,
                    int64_t new_bytes, int64_t budget, size_t max_entries,
                    const std::vector<int>& expected, int line)
{
  const std::vector<int> actual =
      PlanScratchCacheEvictions(entries, new_bytes, budget, max_entries);
  if (actual != expected) {
    std::cout << " - line " << line << ": eviction plan mismatch, got [";
    for (size_t i = 0; i < actual.size(); ++i) {
      if (i) std::cout << ",";
      std::cout << actual[i];
    }
    std::cout << "] expected [";
    for (size_t i = 0; i < expected.size(); ++i) {
      if (i) std::cout << ",";
      std::cout << expected[i];
    }
    std::cout << "]";
    return false;
  }
  return true;
}

} // namespace

OLIVE_ADD_TEST(PreviewScaleDimensionsFollowDivider)
{
  // The common landscape and portrait 4K shapes, at every divider the UI offers.
  if (!CheckDimensions(3840, 1600, 1, 3840, 1600, __LINE__)
      || !CheckDimensions(3840, 1600, 2, 1920, 800, __LINE__)
      || !CheckDimensions(3840, 1600, 4, 960, 400, __LINE__)
      || !CheckDimensions(2160, 3840, 2, 1080, 1920, __LINE__)
      || !CheckDimensions(2160, 3840, 4, 540, 960, __LINE__)) {
    return __LINE__;
  }

  // Integer division with a one-pixel floor, matching VideoParams::GetScaledDimension().
  if (!CheckDimensions(1919, 1079, 2, 959, 539, __LINE__)
      || !CheckDimensions(3, 2, 4, 1, 1, __LINE__)) {
    return __LINE__;
  }

  // Invalid dividers clamp to full size; empty sources produce an empty work size.
  if (!CheckDimensions(1920, 1080, 0, 1920, 1080, __LINE__)) {
    return __LINE__;
  }
  {
    const PreviewScaleDimensions empty = ComputePreviewScaleDimensions(0, 1080, 2);
    const PreviewScaleDimensions negative = ComputePreviewScaleDimensions(1920, -1, 2);
    if (empty.width != 0 || empty.height != 0
        || negative.width != 0 || negative.height != 0) {
      std::cout << " - empty sources must produce a zero work size";
      return __LINE__;
    }
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(PreviewCoordinateMapping)
{
  // Identity at divider 1.
  for (int i = 0; i < 64; ++i) {
    const int mapped = MapPreviewSourceCoordinate(i, 64, 64);
    if (mapped != i) {
      std::cout << " - identity mapping failed at " << i << " -> " << mapped;
      return __LINE__;
    }
  }

  // Downscaled mapping stays in range, is monotonic, and reaches the last source sample.
  const int source_sizes[] = {3840, 1919};
  const int destination_sizes[] = {1920, 959};
  for (int test = 0; test < 2; ++test) {
    const int source_size = source_sizes[test];
    const int destination_size = destination_sizes[test];
    int previous = -1;
    for (int i = 0; i < destination_size; ++i) {
      const int mapped = MapPreviewSourceCoordinate(i, destination_size, source_size);
      if (mapped < 0 || mapped >= source_size || mapped < previous) {
        std::cout << " - mapping out of range or non-monotonic at " << i << " -> " << mapped;
        return __LINE__;
      }
      previous = mapped;
    }
    if (previous != source_size - 1) {
      std::cout << " - last destination sample mapped to " << previous
                << " instead of the last source sample " << (source_size - 1);
      return __LINE__;
    }
  }

  // Out-of-range destinations clamp rather than index outside the source. The first
  // destination sample reads the centre of its 2x2 source footprint (index 1 at 2x).
  if (MapPreviewSourceCoordinate(-5, 1920, 3840) != 1
      || MapPreviewSourceCoordinate(99999, 1920, 3840) != 3839
      || MapPreviewSourceCoordinate(0, 0, 3840) != 0
      || MapPreviewSourceCoordinate(0, 1920, 0) != 0) {
    std::cout << " - destination/source clamping failed";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(IOSurfaceScratchFootprint)
{
  // 3840x1600: R8 full-size + RG8 half-size.
  if (IOSurfaceScratchBytes(3840, 1600, 8) != 9216000) {
    std::cout << " - 8-bit 3840x1600 footprint is "
              << IOSurfaceScratchBytes(3840, 1600, 8) << ", expected 9216000";
    return __LINE__;
  }

  // 3840x1600 P010: R16 full-size + RG16 half-size.
  if (IOSurfaceScratchBytes(3840, 1600, 10) != 18432000) {
    std::cout << " - 10-bit 3840x1600 footprint is "
              << IOSurfaceScratchBytes(3840, 1600, 10) << ", expected 18432000";
    return __LINE__;
  }

  // Odd dimensions round chroma up, matching AcquireIOSurfaceScratchTexture's (w+1)/2.
  if (IOSurfaceScratchBytes(1919, 1079, 8) != 3107401) {
    std::cout << " - 8-bit 1919x1079 footprint is "
              << IOSurfaceScratchBytes(1919, 1079, 8) << ", expected 3107401";
    return __LINE__;
  }

  if (IOSurfaceScratchBytes(0, 1080, 10) != 0
      || IOSurfaceScratchBytes(1920, -1, 8) != 0) {
    std::cout << " - invalid dimensions must have a zero footprint";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ScratchCacheEvictionPolicy)
{
  const std::vector<ScratchCacheEntryInfo> entries = {
    {10, 1},
    {20, 2},
    {30, 3},
  };

  // Fits both budget and entry cap: no eviction.
  if (!CheckEvictions(entries, 20, 100, 4, {}, __LINE__)) {
    return __LINE__;
  }

  // 60 bytes retained + 20 new > 70 budget: evict only the oldest.
  if (!CheckEvictions(entries, 20, 70, 4, {0}, __LINE__)) {
    return __LINE__;
  }

  // Entry cap alone forces the oldest out even with unlimited bytes.
  if (!CheckEvictions(entries, 1, 1000, 3, {0}, __LINE__)) {
    return __LINE__;
  }

  // A single set larger than the whole budget evicts everything; the caller still allocates
  // one set for the current frame.
  if (!CheckEvictions(entries, 200, 100, 4, {0, 1, 2}, __LINE__)) {
    return __LINE__;
  }

  // Ties are broken deterministically by index.
  const std::vector<ScratchCacheEntryInfo> tied = {
    {10, 7},
    {10, 7},
  };
  if (!CheckEvictions(tied, 1, 10, 4, {0, 1}, __LINE__)) {
    return __LINE__;
  }

  if (!CheckEvictions({}, 20, 100, 4, {}, __LINE__)) {
    return __LINE__;
  }

  // Real-world sizes: 3840x1600, 2160x3840 and 3840x2160 P010 (the three resolutions that
  // used to thrash the single-slot cache) fit in the default budget alongside a 1080p set.
  const std::vector<ScratchCacheEntryInfo> common = {
    {IOSurfaceScratchBytes(3840, 1600, 10), 1},
    {IOSurfaceScratchBytes(2160, 3840, 10), 2},
    {IOSurfaceScratchBytes(3840, 2160, 10), 3},
  };
  if (!CheckEvictions(common, IOSurfaceScratchBytes(1920, 1080, 8),
                      kIOSurfaceScratchDefaultBudgetBytes,
                      kIOSurfaceScratchDefaultMaxEntries, {}, __LINE__)) {
    return __LINE__;
  }

  // A 4096x4096 P010 set exceeds what remains, so the oldest entry is recycled while the
  // newer 4K portrait/landscape sets stay resident.
  if (!CheckEvictions(common, IOSurfaceScratchBytes(4096, 4096, 10),
                      kIOSurfaceScratchDefaultBudgetBytes,
                      kIOSurfaceScratchDefaultMaxEntries, {0}, __LINE__)) {
    return __LINE__;
  }

  OLIVE_TEST_END;
}

} // namespace olive
