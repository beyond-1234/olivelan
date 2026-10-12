#ifndef HARDWARE_QUOTA_H
#define HARDWARE_QUOTA_H

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <vector>

namespace olive {

/**
 * @brief Hardware-aware decoding and memory quota profile.
 * 
 * Provides configuration parameters for the current GPU / platform.
 * Currently hardcoded for the baseline profile (NVIDIA RTX 4060 Laptop 8GB),
 * and structured for modular extension once runtime GPU topology detection is connected.
 */
struct HardwareQuotaConfig {
  const char* gpu_name = "NVIDIA GeForce RTX 4060 Laptop GPU";
  size_t vram_total_mb = 8192;
  int nvdec_engine_count = 1;

  // Maximum concurrent NVDEC hardware decode sessions physically active in driver memory.
  // Set to 3 (2 primary reserved + 1 secondary shared) on RTX 4060 (8GB) to reserve a 1.5GB
  // elastic VRAM safety buffer against asynchronous cuCtxDestroy driver reclamation latency.
  int max_physical_sessions = 3;
  int primary_reserved_sessions = 2;

  // Maximum persistent CUDA-GL texture frame pool capacity.
  int frame_pool_capacity = 8;

  // Bounding box dimensions for hardware resident pipeline.
  int max_frame_width = 4096;
  int max_frame_height = 4096;
};

class HardwareQuotaManager {
public:
  struct SessionSlot {
    bool in_use = false;
    bool is_primary = false;
  };

  static const HardwareQuotaConfig& GetConfig() {
    static HardwareQuotaConfig config;
    return config;
  }

  static int GetMaxPhysicalSessions() {
    return GetConfig().max_physical_sessions;
  }

  static int GetActiveSessionCount() {
    std::lock_guard<std::mutex> lock(GetMutex());
    int count = 0;
    for (const auto& slot : GetSessionSlots()) {
      if (slot.in_use) {
        count++;
      }
    }
    return count;
  }

  // Strictly acquire a physical session slot.
  // Primary tracks (viewport full-screen / main footage) get dedicated reserved capacity.
  // Secondary tracks (PiP / thumbnails) can only occupy up to (max - primary_reserved) slots.
  // Returns slot_index >= 0 on success, or -1 if capacity is saturated.
  static int AcquireSlot(bool is_primary = true) {
    std::lock_guard<std::mutex> lock(GetMutex());
    auto& session_slots = GetSessionSlots();
    const int max_physical = GetMaxPhysicalSessions();
    const int primary_reserved = GetConfig().primary_reserved_sessions;
    const int max_secondary = std::max(1, max_physical - primary_reserved);

    // Count currently active sessions
    int total_active = 0;
    int secondary_active = 0;
    for (const auto& slot : session_slots) {
      if (slot.in_use) {
        total_active++;
        if (!slot.is_primary) {
          secondary_active++;
        }
      }
    }

    if (is_primary) {
      // Primary track: granted as long as total physically active sessions < max_physical
      if (total_active >= max_physical) {
        return -1;
      }
    } else {
      // Secondary track: strictly throttled to prevent crowding out primary tracks
      if (secondary_active >= max_secondary || total_active >= max_physical) {
        return -1;
      }
    }

    // Find first free slot
    for (size_t i = 0; i < session_slots.size(); ++i) {
      if (!session_slots[i].in_use) {
        session_slots[i].in_use = true;
        session_slots[i].is_primary = is_primary;
        return static_cast<int>(i);
      }
    }

    // Expand if under capacity
    if (static_cast<int>(session_slots.size()) < max_physical) {
      SessionSlot slot;
      slot.in_use = true;
      slot.is_primary = is_primary;
      session_slots.push_back(slot);
      return static_cast<int>(session_slots.size() - 1);
    }

    return -1;
  }

  // Release slot strictly when driver resources have been freed via av_buffer_unref
  static void ReleaseSlot(int slot_index) {
    if (slot_index < 0) return;
    std::lock_guard<std::mutex> lock(GetMutex());
    auto& session_slots = GetSessionSlots();
    if (slot_index < static_cast<int>(session_slots.size())) {
      session_slots[slot_index].in_use = false;
      session_slots[slot_index].is_primary = false;
    }
  }

private:
  static std::mutex& GetMutex() {
    static std::mutex m;
    return m;
  }

  static std::vector<SessionSlot>& GetSessionSlots() {
    static std::vector<SessionSlot> s;
    return s;
  }
};

} // namespace olive

#endif // HARDWARE_QUOTA_H
