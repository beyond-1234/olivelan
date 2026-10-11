#ifndef HARDWARE_QUOTA_H
#define HARDWARE_QUOTA_H

#include <chrono>
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

  // Maximum concurrent NVDEC hardware decode sessions allowed before routing to software fallback.
  // Benchmarked on RTX 4060 (290 FPS capacity / 8GB VRAM) to prevent driver ENOMEM (-542398533).
  int max_nvdec_sessions = 4;

  // Maximum persistent CUDA-GL texture frame pool capacity.
  int frame_pool_capacity = 8;

  // Bounding box dimensions for hardware resident pipeline.
  int max_frame_width = 4096;
  int max_frame_height = 4096;

  // Idle timeout in milliseconds before an inactive session can be preempted by an active playing session.
  int64_t idle_preemption_threshold_ms = 500;
};

class HardwareQuotaManager {
public:
  struct SessionSlot {
    void* owner = nullptr;
    int64_t last_accessed_ms = 0;
  };

  static const HardwareQuotaConfig& GetConfig() {
    static HardwareQuotaConfig config;
    return config;
  }

  static int GetMaxNvdecSessions() {
    return GetConfig().max_nvdec_sessions;
  }

  static int GetActiveNvdecSessions() {
    std::lock_guard<std::mutex> lock(GetMutex());
    int count = 0;
    for (const auto& slot : GetSessionSlots()) {
      if (slot.owner != nullptr) {
        count++;
      }
    }
    return count;
  }

  // Atomically acquire or preempt an idle hardware decode session slot.
  // Returns slot_index (>= 0) on success, or -1 if all sessions are actively busy (< 500ms).
  static int AcquireOrPreemptNvdecQuota(void* owner) {
    if (!owner) return -1;
    std::lock_guard<std::mutex> lock(GetMutex());
    auto& session_slots = GetSessionSlots();
    const int max_sessions = GetMaxNvdecSessions();
    const int64_t now = GetCurrentTimeMs();

    // 1. If owner already holds a slot, refresh heartbeat and return it
    for (int i = 0; i < static_cast<int>(session_slots.size()); ++i) {
      if (session_slots[i].owner == owner) {
        session_slots[i].last_accessed_ms = now;
        return i;
      }
    }

    // 2. Check for an empty/idle slot
    for (int i = 0; i < static_cast<int>(session_slots.size()); ++i) {
      if (session_slots[i].owner == nullptr) {
        session_slots[i].owner = owner;
        session_slots[i].last_accessed_ms = now;
        return i;
      }
    }

    // 3. If under capacity, expand slots
    if (static_cast<int>(session_slots.size()) < max_sessions) {
      SessionSlot slot;
      slot.owner = owner;
      slot.last_accessed_ms = now;
      session_slots.push_back(slot);
      return static_cast<int>(session_slots.size() - 1);
    }

    // 4. Preemption pass: find the least recently accessed slot
    int oldest_idx = -1;
    int64_t oldest_time = now;
    for (int i = 0; i < static_cast<int>(session_slots.size()); ++i) {
      if (session_slots[i].last_accessed_ms < oldest_time) {
        oldest_time = session_slots[i].last_accessed_ms;
        oldest_idx = i;
      }
    }

    const int64_t threshold = GetConfig().idle_preemption_threshold_ms;
    if (oldest_idx >= 0 && (now - oldest_time) >= threshold) {
      // Preempt the idle session!
      session_slots[oldest_idx].owner = owner;
      session_slots[oldest_idx].last_accessed_ms = now;
      return oldest_idx;
    }

    // All slots are actively in use
    return -1;
  }

  // Refresh heartbeat for the active slot
  static void TouchNvdecQuota(int slot_index, void* owner) {
    if (slot_index < 0 || !owner) return;
    std::lock_guard<std::mutex> lock(GetMutex());
    auto& session_slots = GetSessionSlots();
    if (slot_index < static_cast<int>(session_slots.size()) && session_slots[slot_index].owner == owner) {
      session_slots[slot_index].last_accessed_ms = GetCurrentTimeMs();
    }
  }

  // Release slot back to pool
  static void ReleaseNvdecQuota(int slot_index, void* owner) {
    if (slot_index < 0 || !owner) return;
    std::lock_guard<std::mutex> lock(GetMutex());
    auto& session_slots = GetSessionSlots();
    if (slot_index < static_cast<int>(session_slots.size()) && session_slots[slot_index].owner == owner) {
      session_slots[slot_index].owner = nullptr;
      session_slots[slot_index].last_accessed_ms = 0;
    }
  }

  // Check if owner still holds the valid slot (or if it was preempted due to inactivity)
  static bool IsSlotValid(int slot_index, void* owner) {
    if (slot_index < 0 || !owner) return false;
    std::lock_guard<std::mutex> lock(GetMutex());
    const auto& session_slots = GetSessionSlots();
    return (slot_index < static_cast<int>(session_slots.size()) && session_slots[slot_index].owner == owner);
  }

private:
  static int64_t GetCurrentTimeMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }

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
