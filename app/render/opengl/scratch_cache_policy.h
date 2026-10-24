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

#ifndef OLIVE_SCRATCH_CACHE_POLICY_H
#define OLIVE_SCRATCH_CACHE_POLICY_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

namespace olive {

// One 3840x2160 P010 set is ~24 MiB; 96 MiB keeps the three common landscape/portrait sizes
// resident on a 4 GB laptop GPU while still bounding the cache. The entry cap also prevents a
// long tail of tiny sizes from accumulating.
constexpr int64_t kIOSurfaceScratchDefaultBudgetBytes = 96 * 1024 * 1024;
constexpr std::size_t kIOSurfaceScratchDefaultMaxEntries = 4;

/**
 * @brief Approximate VRAM footprint of a VideoToolbox NV12/P010 scratch pair.
 *
 * Y is a full-size R8/R16 plane; UV is a half-size RG8/RG16 plane. These are the same formats
 * AcquireIOSurfaceScratchTexture() allocates, so the cache budget tracks real storage rather
 * than an arbitrary entry count.
 */
inline int64_t IOSurfaceScratchBytes(int width, int height, int bits_per_pixel)
{
  if (width <= 0 || height <= 0) {
    return 0;
  }

  const int64_t bytes_per_luma = (bits_per_pixel > 8) ? 2 : 1;
  const int64_t bytes_per_chroma_sample = (bits_per_pixel > 8) ? 4 : 2;
  const int64_t chroma_width = (static_cast<int64_t>(width) + 1) / 2;
  const int64_t chroma_height = (static_cast<int64_t>(height) + 1) / 2;

  return static_cast<int64_t>(width) * height * bytes_per_luma
      + chroma_width * chroma_height * bytes_per_chroma_sample;
}

struct ScratchCacheEntryInfo {
  int64_t bytes = 0;
  uint64_t last_used = 0;
};

/**
 * @brief Choose which cached scratch sets to evict before inserting a new one.
 *
 * Entries are evicted oldest-first until both the byte budget and the entry-count cap can
 * accommodate one more set. The result is the list of indices (in eviction order). If the new
 * set alone exceeds the budget, every existing entry is evicted and the caller still allocates
 * one set for the current frame: the frame must be displayed even on a size the cache cannot
 * retain.
 *
 * Keeping this policy free of OpenGL makes the eviction/VRAM-budget behaviour unit-testable
 * without a graphics context.
 */
inline std::vector<int> PlanScratchCacheEvictions(const std::vector<ScratchCacheEntryInfo>& entries,
                                                  int64_t new_entry_bytes,
                                                  int64_t budget_bytes,
                                                  size_t max_entries)
{
  std::vector<int> evicted;
  if (entries.empty()) {
    return evicted;
  }

  if (budget_bytes < 0) {
    budget_bytes = 0;
  }
  if (max_entries < 1) {
    max_entries = 1;
  }

  int64_t retained_bytes = 0;
  for (const ScratchCacheEntryInfo& entry : entries) {
    retained_bytes += entry.bytes;
  }

  std::vector<int> oldest_first(entries.size());
  std::iota(oldest_first.begin(), oldest_first.end(), 0);
  std::stable_sort(oldest_first.begin(), oldest_first.end(),
                   [&entries](int lhs, int rhs) {
                     if (entries[lhs].last_used != entries[rhs].last_used) {
                       return entries[lhs].last_used < entries[rhs].last_used;
                     }
                     return lhs < rhs;
                   });

  for (int index : oldest_first) {
    const size_t retained_entries = entries.size() - evicted.size();
    if (retained_bytes + new_entry_bytes <= budget_bytes && retained_entries + 1 <= max_entries) {
      break;
    }
    evicted.push_back(index);
    retained_bytes -= entries[index].bytes;
  }

  return evicted;
}

} // namespace olive

#endif // OLIVE_SCRATCH_CACHE_POLICY_H
