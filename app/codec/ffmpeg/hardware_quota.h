#ifndef HARDWARE_QUOTA_H
#define HARDWARE_QUOTA_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

#if defined(OLIVE_ENABLE_CUDA_NVDEC)
#include <cuda_runtime_api.h>
#endif

#include "codec/hal/gpu_environment_probe.h"

#include <QDebug>
#include <QString>

namespace olive {

/**
 * @brief Hardware-aware decoding and memory quota profile.
 * 
 * Provides configuration parameters for the current GPU / platform.
 * Supports runtime dynamic adjustment from GpuEnvironmentProbe.
 */
struct HardwareQuotaConfig {
#if defined(__APPLE__)
  // VideoToolbox profile. The generic 4096x4096 bounding box is deliberate: this project's
  // 5504x3072 H.264 clips are rejected by VideoToolbox (h264 hardware decode returns an
  // initialization error), while 4K and 2160x3840 streams are supported. Keeping the limit
  // here makes get_format fall back to software for the over-limit clips instead of selecting
  // a hardware decoder that cannot open.
  std::string gpu_name = "VideoToolbox Hardware Decoder";
  size_t vram_total_mb = 4096;
  int nvdec_engine_count = 1;

  int max_physical_sessions = 8;
  int primary_reserved_sessions = 2;

  int frame_pool_capacity = 8;

  int max_frame_width = 4096;
  int max_frame_height = 4096;
#else
  // Neutral baseline profile used when hardware dynamic probing is unavailable.
  // Never hardcode a specific GPU model (e.g. RTX 4060) that misleads diagnostics.
  std::string gpu_name = "Generic Baseline GPU Profile";
  size_t vram_total_mb = 4096;
  int nvdec_engine_count = 1;

  // Maximum concurrent NVDEC hardware decode sessions physically active in driver memory.
  int max_physical_sessions = 6;
  int primary_reserved_sessions = 2;

  // Maximum persistent CUDA-GL texture frame pool capacity.
  int frame_pool_capacity = 8;

  // Bounding box dimensions for hardware resident pipeline.
  int max_frame_width = 4096;
  int max_frame_height = 4096;
#endif
};

class HardwareQuotaManager {
public:
  struct SessionSlot {
    bool in_use = false;
    bool is_primary = false;
    int cost_points = 0;
    int width = 0;
    int height = 0;
    double fps = 60.0;
  };

  static void AutoDetectHardwareTopologyInternal(HardwareQuotaConfig& config) {
#if defined(OLIVE_ENABLE_CUDA_NVDEC)
    int device_id = 0;
    if (cudaGetDevice(&device_id) == cudaSuccess) {
      cudaDeviceProp prop{};
      if (cudaGetDeviceProperties(&prop, device_id) == cudaSuccess) {
        config.gpu_name = prop.name;
        // 1. Dynamic VRAM detection (bytes to MiB)
        config.vram_total_mb = prop.totalGlobalMem / (1024 * 1024);

        // 2. Dynamic NVDEC engine count deduction based on architecture & SMs:
        // Ada Lovelace (sm_89): AD102 (4090/6000, SM >= 128), AD103 (4080, SM >= 76),
        // AD104 (4070 Ti, SM >= 60) have 2 NVDEC engines.
        // AD106 / AD107 (4060, SM < 60) have 1 NVDEC engine.
        if (prop.major == 8 && prop.minor == 9) {
          config.nvdec_engine_count = (prop.multiProcessorCount >= 60) ? 2 : 1;
        } else if (prop.major >= 9) {
          config.nvdec_engine_count = 2; // Hopper / Blackwell flagship cores default to 2+
        } else {
          config.nvdec_engine_count = 1;
        }

        // 3. Dynamic session cap & frame pool scaling
        config.max_physical_sessions = std::min(8, config.nvdec_engine_count * 6);
        if (config.vram_total_mb >= 20480) {
          config.frame_pool_capacity = 16;
        } else if (config.vram_total_mb >= 12288) {
          config.frame_pool_capacity = 12;
        } else {
          config.frame_pool_capacity = 8;
        }

        const int vram_budget = (config.vram_total_mb > 1536) ? static_cast<int>((config.vram_total_mb - 1536) / 22) : 200;
        const int total_budget_points = std::clamp(vram_budget, 200, config.nvdec_engine_count * 400);

        qInfo() << "[HardwareQuota] Dynamic Hardware Topology Detected:"
                << QString::fromStdString(config.gpu_name)
                << "| VRAM:" << config.vram_total_mb << "MiB"
                << "| NVDEC Engines:" << config.nvdec_engine_count
                << "| SM Count:" << prop.multiProcessorCount
                << "| Total Points Budget:" << total_budget_points
                << "| Max Sessions Cap:" << config.max_physical_sessions;
        return;
      }
    }
#endif
    // When CUDA is unavailable or not compiled in, populate identity from GpuEnvironmentProbe
    // rather than claiming an imaginary GPU model.
    const auto& probe = GpuEnvironmentProbe::Instance();
    const std::string& detected_name = probe.GetPrimaryGpuName();
    if (!detected_name.empty() && detected_name != "Unknown GPU") {
      config.gpu_name = detected_name;
    } else {
      switch (probe.GetPrimaryVendor()) {
        case GpuVendor::Nvidia:
          config.gpu_name = "NVIDIA GPU (No CUDA runtime)";
          break;
        case GpuVendor::Amd:
          config.gpu_name = "AMD GPU";
          break;
        case GpuVendor::Intel:
          config.gpu_name = "Intel Graphics";
          break;
        case GpuVendor::Apple:
          config.gpu_name = "Apple Silicon GPU";
          break;
        default:
          config.gpu_name = "Generic Baseline GPU Profile";
          break;
      }
    }
    if (probe.GetTotalVramMb() > 0) {
      config.vram_total_mb = probe.GetTotalVramMb();
    }

    qWarning() << "[HardwareQuota] Dynamic detection unavailable; using baseline profile:"
               << QString::fromStdString(config.gpu_name) << "| VRAM:" << config.vram_total_mb << "MiB";
  }

  static HardwareQuotaConfig& GetConfig() {
    static HardwareQuotaConfig config;
    static std::once_flag s_probe_flag;
    std::call_once(s_probe_flag, []() {
      AutoDetectHardwareTopologyInternal(config);
    });
    return config;
  }

  static void AutoDetectHardwareTopology() {
    (void)GetConfig();
  }

  // Cost scoring model: 100 points = 1 standard 4K 60fps stream (3840x2160, 60fps, 8-bit)
  static int CalculateStreamCost(int width, int height, double fps, bool is_10bit = false) {
    if (width <= 0) width = 1920;
    if (height <= 0) height = 1080;
    if (fps <= 0.0 || fps > 240.0) fps = 60.0;

    const double pixel_ratio = (static_cast<double>(width) * height) / (3840.0 * 2160.0);
    const double fps_ratio = fps / 60.0;
    const double depth_factor = is_10bit ? 1.25 : 1.0;

    int raw_cost = static_cast<int>(std::round(100.0 * pixel_ratio * fps_ratio * depth_factor));

    // Safety Guardrail 1: Minimum Cost Floor (15 points)
    // Accounts for base driver context / CUDA context / OpenGL FBO overhead (~80-120MB)
    // to prevent VRAM exhaustion from an unbounded number of tiny streams (e.g. 720p/360p).
    const int kMinStreamCostFloor = 15;
    return std::max(raw_cost, kMinStreamCostFloor);
  }

  // Dynamic point budget based on VRAM capacity and engine count
  static int GetTotalBudgetPoints() {
    const size_t vram_mb = GetConfig().vram_total_mb;
    const int engines = std::max(1, GetConfig().nvdec_engine_count);
    // 8GB: (8192 - 1536) / 22 = 302 -> 300 points
    // 16GB: (16384 - 1536) / 22 = 674 -> 600 points
    // 24GB: (24576 - 1536) / 22 = 1047 -> 800 points
    const int vram_budget = (vram_mb > 1536) ? static_cast<int>((vram_mb - 1536) / 22) : 200;
    return std::clamp(vram_budget, 200, engines * 400);
  }

  // Safety Guardrail 2: Hard driver concurrent session limit
  // Consumer GeForce drivers cap concurrent decode/encode sessions (typically 5~8).
  static int GetMaxConcurrentSessionsCap() {
    const int engines = std::max(1, GetConfig().nvdec_engine_count);
    return std::min(8, engines * 6);
  }

  static int GetMaxPhysicalSessions() {
    return GetMaxConcurrentSessionsCap();
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

  static int GetAllocatedPoints() {
    std::lock_guard<std::mutex> lock(GetMutex());
    return GetAllocatedPointsInternal();
  }

  // Dynamic budget-based session slot acquisition
  static int AcquireBudgetSlot(int width, int height, double fps, bool is_10bit = false, bool is_primary = true) {
    std::lock_guard<std::mutex> lock(GetMutex());
    auto& session_slots = GetSessionSlots();
    const int max_sessions = GetMaxConcurrentSessionsCap();
    const int total_budget = GetTotalBudgetPoints();
    const int current_allocated = GetAllocatedPointsInternal();
    const int cost = CalculateStreamCost(width, height, fps, is_10bit);

    // Primary track reserved baseline (guarantees at least 100 points for viewport full-screen playback)
    const int kPrimaryReservedPoints = 100;

    int total_active = 0;
    for (const auto& slot : session_slots) {
      if (slot.in_use) {
        total_active++;
      }
    }

    // 1. Guardrail: Physical session count limit
    if (total_active >= max_sessions) {
      return -1;
    }

    // 2. Budget verification with primary track guarantee
    if (is_primary) {
      if (current_allocated + cost > total_budget) {
        return -1;
      }
    } else {
      // Secondary tracks cannot invade the primary reserved headroom
      if ((current_allocated + cost) > (total_budget - kPrimaryReservedPoints) ||
          (current_allocated + cost > total_budget)) {
        return -1;
      }
    }

    // 3. Find first free slot
    for (size_t i = 0; i < session_slots.size(); ++i) {
      if (!session_slots[i].in_use) {
        session_slots[i].in_use = true;
        session_slots[i].is_primary = is_primary;
        session_slots[i].cost_points = cost;
        session_slots[i].width = width;
        session_slots[i].height = height;
        session_slots[i].fps = fps;
        return static_cast<int>(i);
      }
    }

    // 4. Expand if under session cap
    if (static_cast<int>(session_slots.size()) < max_sessions) {
      SessionSlot slot;
      slot.in_use = true;
      slot.is_primary = is_primary;
      slot.cost_points = cost;
      slot.width = width;
      slot.height = height;
      slot.fps = fps;
      session_slots.push_back(slot);
      return static_cast<int>(session_slots.size() - 1);
    }

    return -1;
  }

  // Legacy convenience overload (defaults to 4K60 standard profile)
  static int AcquireSlot(bool is_primary = true) {
    return AcquireBudgetSlot(3840, 2160, 60.0, false, is_primary);
  }

  // Update slot primary status dynamically based on active AABB viewport coverage
  static bool UpdateSlotPriority(int slot_index, bool is_primary) {
    if (slot_index < 0) return false;
    std::lock_guard<std::mutex> lock(GetMutex());
    auto& session_slots = GetSessionSlots();
    if (slot_index >= static_cast<int>(session_slots.size()) || !session_slots[slot_index].in_use) {
      return false;
    }
    session_slots[slot_index].is_primary = is_primary;
    return true;
  }

  // Release slot strictly when driver resources have been freed via av_buffer_unref
  static void ReleaseSlot(int slot_index) {
    if (slot_index < 0) return;
    std::lock_guard<std::mutex> lock(GetMutex());
    auto& session_slots = GetSessionSlots();
    if (slot_index < static_cast<int>(session_slots.size())) {
      session_slots[slot_index].in_use = false;
      session_slots[slot_index].is_primary = false;
      session_slots[slot_index].cost_points = 0;
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

  static int GetAllocatedPointsInternal() {
    int sum = 0;
    for (const auto& slot : GetSessionSlots()) {
      if (slot.in_use) {
        sum += slot.cost_points;
      }
    }
    return sum;
  }
};

} // namespace olive

#endif // HARDWARE_QUOTA_H
