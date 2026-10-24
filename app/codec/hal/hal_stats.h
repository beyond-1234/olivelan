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

#ifndef HAL_STATS_H
#define HAL_STATS_H

#include <atomic>

#include <QtGlobal>

namespace olive {

/**
 * @brief Process-wide HAL counters for the playback harness.
 *
 * These counters deliberately separate "hardware path succeeded" from "CPU
 * readback happened". A framebuffer dump also performs a readback, so it must
 * never be used as evidence that the decode/import path was zero-copy.
 */
struct HalStats {
  std::atomic<quint64> hardware_frames{0};
  std::atomic<quint64> cpu_readback_frames{0};
  std::atomic<quint64> import_failures{0};

  void Reset()
  {
    hardware_frames.store(0, std::memory_order_relaxed);
    cpu_readback_frames.store(0, std::memory_order_relaxed);
    import_failures.store(0, std::memory_order_relaxed);
  }

  void RecordHardwareFrame()
  {
    hardware_frames.fetch_add(1, std::memory_order_relaxed);
  }

  void RecordCpuReadback()
  {
    cpu_readback_frames.fetch_add(1, std::memory_order_relaxed);
  }

  void RecordImportFailure()
  {
    import_failures.fetch_add(1, std::memory_order_relaxed);
  }
};

inline HalStats& GetHalStats()
{
  static HalStats stats;
  return stats;
}

} // namespace olive

#endif // HAL_STATS_H
