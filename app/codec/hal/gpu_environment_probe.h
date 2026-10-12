#ifndef GPU_ENVIRONMENT_PROBE_H
#define GPU_ENVIRONMENT_PROBE_H

#include <cstddef>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

namespace olive {

enum class GpuVendor {
  Nvidia,
  Amd,
  Intel,
  Apple,
  Unknown
};

enum class DisplayServer {
  Wayland,
  X11,
  WindowsDWM,
  Cocoa,
  Headless,
  Unknown
};

struct GpuDeviceInfo {
  GpuVendor vendor{GpuVendor::Unknown};
  std::string device_name;
  std::string driver_version;
  size_t vram_total_bytes{0};
  size_t vram_available_bytes{0};
  int pci_domain{0};
  int pci_bus{0};
  int pci_device{0};
  bool is_primary_display{false};
};

class GpuEnvironmentProbe {
public:
  static GpuEnvironmentProbe& Instance();

  // Core hardware environment queries
  GpuVendor GetPrimaryVendor() const { return primary_vendor_; }
  DisplayServer GetDisplayServer() const { return display_server_; }
  const std::string& GetPrimaryGpuName() const { return primary_gpu_name_; }
  size_t GetTotalVramMb() const { return total_vram_mb_; }

  // Vendor convenience checks
  bool IsNvidiaGpu() const { return primary_vendor_ == GpuVendor::Nvidia; }
  bool IsAmdGpu() const { return primary_vendor_ == GpuVendor::Amd; }
  bool IsIntelGpu() const { return primary_vendor_ == GpuVendor::Intel; }

  // Topology and backend capability checks
  bool IsHybridTopology() const { return is_hybrid_topology_; }
  bool HasNvdecSupport() const { return has_nvdec_support_; }
  bool SupportsVaapiDmaBuf() const { return has_vaapi_dmabuf_support_; }
  bool SupportsVideoToolbox() const { return has_videotoolbox_support_; }
  bool SupportsD3D11() const { return has_d3d11_support_; }
  bool SupportsLibPlacebo() const { return has_libplacebo_support_; }

  // Decision helper: should this stream even attempt hardware acceleration?
  // Prevents allocating hardware decoder slots for formats better handled by CPU (ProRes, 4:2:2).
  bool ShouldUseHardwareDecode(AVCodecID codec_id, AVPixelFormat sw_format) const;

  // Decision helper: does this stream require complex HDR / BT.2020 tone mapping via Tier 3?
  bool NeedsToneMapping(AVColorSpace col_space, AVColorTransferCharacteristic col_trc) const;

  // Refresh dynamic state (e.g. available VRAM)
  void Refresh();

private:
  GpuEnvironmentProbe();
  ~GpuEnvironmentProbe() = default;
  GpuEnvironmentProbe(const GpuEnvironmentProbe&) = delete;
  GpuEnvironmentProbe& operator=(const GpuEnvironmentProbe&) = delete;

  void DetectDisplayServer();
  void DetectGpuTopology();
  void DetectBackendCapabilities();

  GpuVendor primary_vendor_{GpuVendor::Unknown};
  DisplayServer display_server_{DisplayServer::Unknown};
  std::string primary_gpu_name_{"Unknown GPU"};
  size_t total_vram_mb_{0};
  bool is_hybrid_topology_{false};
  bool has_nvdec_support_{false};
  bool has_vaapi_dmabuf_support_{false};
  bool has_videotoolbox_support_{false};
  bool has_d3d11_support_{false};
  bool has_libplacebo_support_{false};
  std::vector<GpuDeviceInfo> detected_devices_;
};

} // namespace olive

#endif // GPU_ENVIRONMENT_PROBE_H
