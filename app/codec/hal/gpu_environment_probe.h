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

/**
 * @brief Per-backend hardware decode capability (codec x bit-depth matrix).
 *
 * Replaces the previous coarse single-boolean gating: a GPU may decode H.264 but
 * refuse AV1 (e.g. macOS AMD Radeon Pro 5300M has no AV1 decode engine), and a
 * stream that "looks" hardware-decodable by codec alone would otherwise be routed
 * to a backend that is guaranteed to fail at decode time.
 */
struct HardwareDecodeCapability {
  bool h264{false};
  bool hevc_8bit{false};
  bool hevc_10bit{false};
  bool vp9{false};
  bool av1_8bit{false};
  bool av1_10bit{false};
};

class GpuEnvironmentProbe {
public:
  static GpuEnvironmentProbe& Instance();

  // Core hardware environment queries
  GpuVendor GetPrimaryVendor() const { return primary_vendor_; }
  DisplayServer GetDisplayServer() const { return display_server_; }
  const std::string& GetPrimaryGpuName() const { return primary_gpu_name_; }
  size_t GetTotalVramMb() const { return total_vram_mb_; }
  const std::vector<GpuDeviceInfo>& GetDetectedDevices() const { return detected_devices_; }

  /**
   * @brief Patch 0080 (Phase 9): a DRM connector that currently reports "connected", and the GPU
   *        driving it.
   *
   * This is what lets the UI answer "would moving the display to the discrete GPU actually be
   * possible?" instead of printing the same generic advice everywhere. On a hybrid laptop the
   * panel is typically wired to the iGPU, so the dGPU may have no connected output at all.
   */
  struct DisplayConnection {
    std::string connector;
    GpuVendor gpu{GpuVendor::Unknown};
  };

  const std::vector<DisplayConnection>& GetConnectedDisplays() const { return connected_displays_; }

  /// @brief True when a currently connected display is driven by a GPU of this vendor.
  bool HasConnectedDisplayOn(GpuVendor vendor) const;

  // Vendor convenience checks
  bool IsNvidiaGpu() const { return primary_vendor_ == GpuVendor::Nvidia; }
  bool IsAmdGpu() const { return primary_vendor_ == GpuVendor::Amd; }
  bool IsIntelGpu() const { return primary_vendor_ == GpuVendor::Intel; }

  // Topology and backend capability checks
  bool IsHybridTopology() const { return is_hybrid_topology_; }
  bool HasNvdecSupport() const { return has_nvdec_support_; }

  /**
   * @brief Vendor string of the OpenGL context the app actually renders with (may be empty).
   *
   * Only recorded when a GL context is current on the calling thread (or explicitly provided),
   * because creating a throw-away offscreen context is unreliable on hybrid laptops: it may bind
   * a different GPU than the real window context.
   */
  const std::string& GetOpenGlVendor() const { return gl_vendor_; }
  bool IsOpenGlOnNvidia() const { return gl_is_nvidia_; }

  /**
   * @brief True when Tier 1 (CUDA-NVDEC + CUDA-GL interop) cannot work on this machine.
   *
   * CUDA-GL interop requires the OpenGL context to live on the same NVIDIA GPU as CUDA/NVDEC.
   * On hybrid laptops the window context can land on the iGPU (e.g. native Wayland without PRIME
   * offload), in which case cudaGraphicsGLRegisterImage fails with
   * cudaErrorInvalidGraphicsContext and the "zero-copy" path silently does nothing. In that case
   * the factory routes straight to Tier 3 instead of pretending Tier 1 is available.
   *
   * Only answers true once the GL vendor is actually known; an empty vendor keeps the old
   * behaviour so headless/import-time decodes are unaffected.
   */
  bool IsCudaGlInteropBlocked() const;

  /// @brief Records the OpenGL vendor. First non-empty wins, except that a reading taken from the
  ///        render context supersedes one taken from a platform-level (offscreen) context.
  ///
  /// @param from_render_context true when the vendor came from the context that actually renders.
  ///        That is the authoritative source for CUDA-GL interop, which needs the *render* context
  ///        to be on the NVIDIA GPU. A platform-level context is only a stand-in, recorded before
  ///        any renderer exists, so the routing decision is not left blind until the first frame.
  bool RecordOpenGlVendor(const char* vendor, bool from_render_context = false);

  /**
   * @brief Probes the platform's GL vendor with a throw-away offscreen context and records it.
   *
   * For UI that must judge interop before any renderer exists - the HAL backend dialog runs before
   * the main window, so nothing has reported a vendor yet and every Tier 1 would otherwise look
   * available. The reading is recorded as a platform-level (non-authoritative) one, so a later
   * report from the render context supersedes it.
   *
   * @return true if a vendor was obtained and recorded.
   */
  bool ProbePlatformOpenGlVendor();
  /**
   * @brief F-7 fix (HAL/Phase6-8 review): true only when a VA-API device could actually
   *        be created at runtime, not merely when the backend was compiled in. A pure
   *        NVIDIA machine therefore reports false instead of advertising a capability it
   *        cannot honour.
   */
  bool SupportsVaapiDmaBuf() const { return has_vaapi_dmabuf_support_; }
  bool SupportsVideoToolbox() const { return has_videotoolbox_support_; }

  /**
   * @brief Patch 0049 (Phase 8): D3D11 interop is a placeholder - never supported,
   *        on any platform, Windows included. Windows hardware fallback is Tier 3.
   *
   * Defined in the .cpp so the policy lives in one place (and so a future
   * implementation only has to flip this one function).
   */
  bool SupportsD3D11() const;

  // Decision helper: should this stream even attempt hardware acceleration?
  // Prevents allocating hardware decoder slots for formats better handled by CPU (ProRes, 4:2:2).
  bool ShouldUseHardwareDecode(AVCodecID codec_id, AVPixelFormat sw_format) const;

  /**
   * @brief Runtime capability query: does the active hardware backend decode this stream?
   *
   * Consults the detected codec x bit-depth capability matrix. Use this to reject a stream
   * during negotiation, rather than discovering the failure after the decode pipeline starts.
   *
   * @param codec_id  FFmpeg codec id.
   * @param sw_format Software pixel format of the stream (bit depth derived from its descriptor).
   * @return true if the active backend is known to support decoding this codec/bit-depth.
   */
  bool SupportsStream(AVCodecID codec_id, AVPixelFormat sw_format) const;

  // Decision helper: does this stream require HDR / BT.2020 tone mapping?
  // Drives Olive's OCIO color pipeline (not a separate decode backend).
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
  void DetectDecodeCapabilityMatrix();
  void DetectOpenGlDevice();
  void DetectConnectedDisplays();

  GpuVendor primary_vendor_{GpuVendor::Unknown};
  DisplayServer display_server_{DisplayServer::Unknown};
  std::string primary_gpu_name_{"Unknown GPU"};
  size_t total_vram_mb_{0};
  bool is_hybrid_topology_{false};
  std::string gl_vendor_;
  bool gl_is_nvidia_{false};
  bool gl_vendor_from_render_context_{false};
  bool has_nvdec_support_{false};
  bool has_vaapi_dmabuf_support_{false};
  bool has_videotoolbox_support_{false};
  // Patch 0049: has_d3d11_support_ removed - D3D11 interop is a placeholder and
  // SupportsD3D11() answers a hard false from the .cpp.
  HardwareDecodeCapability decode_capability_;
  std::vector<GpuDeviceInfo> detected_devices_;
  std::vector<DisplayConnection> connected_displays_;
};

} // namespace olive

#endif // GPU_ENVIRONMENT_PROBE_H
