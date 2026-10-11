#ifndef HARDWARE_QUOTA_H
#define HARDWARE_QUOTA_H

#include <atomic>
#include <cstddef>

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
};

class HardwareQuotaManager {
public:
  static const HardwareQuotaConfig& GetConfig() {
    static HardwareQuotaConfig config;
    return config;
  }

  static int GetMaxNvdecSessions() {
    return GetConfig().max_nvdec_sessions;
  }

  static int GetActiveNvdecSessions() {
    return s_active_nvdec_sessions.load(std::memory_order_relaxed);
  }

  // Atomically acquire a hardware decode session quota slot using CAS loop.
  // Returns true if quota is granted, false if saturated.
  static bool TryAcquireNvdecQuota() {
    const int max_sessions = GetMaxNvdecSessions();
    int current = s_active_nvdec_sessions.load(std::memory_order_relaxed);
    while (current < max_sessions) {
      if (s_active_nvdec_sessions.compare_exchange_weak(
              current, current + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

  // Release an acquired hardware decode session quota slot.
  static void ReleaseNvdecQuota() {
    s_active_nvdec_sessions.fetch_sub(1, std::memory_order_release);
  }

private:
  static inline std::atomic<int> s_active_nvdec_sessions{0};
};

} // namespace olive

#endif // HARDWARE_QUOTA_H
