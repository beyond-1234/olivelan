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

#ifndef COLORSTATS_H
#define COLORSTATS_H

#include <atomic>

#include <QtGlobal>

namespace olive {

/**
 * @brief Counters for the HDR/SDR display-view and colour-shader lifecycle.
 *
 * These are intentionally separate from HAL decode counters. They make it
 * possible to quantify how often a mixed HDR/SDR timeline rebuilds its OCIO
 * processor/shader at edit points.
 */
struct ColorStats {
  std::atomic<quint64> transform_changes{0};
  std::atomic<quint64> hdr_view_switches{0};
  std::atomic<quint64> shader_compiles{0};
  std::atomic<quint64> shader_cache_hits{0};

  void Reset()
  {
    transform_changes.store(0, std::memory_order_relaxed);
    hdr_view_switches.store(0, std::memory_order_relaxed);
    shader_compiles.store(0, std::memory_order_relaxed);
    shader_cache_hits.store(0, std::memory_order_relaxed);
  }
};

inline ColorStats& GetColorStats()
{
  static ColorStats stats;
  return stats;
}

} // namespace olive

#endif // COLORSTATS_H
