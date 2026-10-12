#include "gpu_environment_probe.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#if defined(OLIVE_ENABLE_CUDA_NVDEC)
#include <cuda_runtime_api.h>
#endif

#if defined(__linux__)
#include <dirent.h>
#include <unistd.h>
#endif

namespace olive {

GpuEnvironmentProbe& GpuEnvironmentProbe::Instance()
{
  static GpuEnvironmentProbe instance;
  return instance;
}

GpuEnvironmentProbe::GpuEnvironmentProbe()
{
  Refresh();
}

void GpuEnvironmentProbe::Refresh()
{
  detected_devices_.clear();
  DetectDisplayServer();
  DetectGpuTopology();
  DetectBackendCapabilities();
}

void GpuEnvironmentProbe::DetectDisplayServer()
{
#if defined(_WIN32)
  display_server_ = DisplayServer::WindowsDWM;
#elif defined(__APPLE__)
  display_server_ = DisplayServer::Cocoa;
#elif defined(__linux__)
  const char* wayland_disp = std::getenv("WAYLAND_DISPLAY");
  const char* xdg_session = std::getenv("XDG_SESSION_TYPE");
  if ((wayland_disp && *wayland_disp) || (xdg_session && std::string(xdg_session) == "wayland")) {
    display_server_ = DisplayServer::Wayland;
  } else if (std::getenv("DISPLAY")) {
    display_server_ = DisplayServer::X11;
  } else {
    display_server_ = DisplayServer::Headless;
  }
#else
  display_server_ = DisplayServer::Unknown;
#endif
}

void GpuEnvironmentProbe::DetectGpuTopology()
{
  primary_vendor_ = GpuVendor::Unknown;
  primary_gpu_name_ = "Unknown GPU";
  total_vram_mb_ = 0;
  is_hybrid_topology_ = false;

#if defined(OLIVE_ENABLE_CUDA_NVDEC)
  int cuda_count = 0;
  cudaError_t cuda_err = cudaGetDeviceCount(&cuda_count);
  if (cuda_err == cudaSuccess && cuda_count > 0) {
    cudaDeviceProp prop{};
    if (cudaGetDeviceProperties(&prop, 0) == cudaSuccess) {
      primary_vendor_ = GpuVendor::Nvidia;
      primary_gpu_name_ = prop.name;
      total_vram_mb_ = prop.totalGlobalMem / (1024 * 1024);
      has_nvdec_support_ = true;

      GpuDeviceInfo dev{};
      dev.vendor = GpuVendor::Nvidia;
      dev.device_name = prop.name;
      dev.vram_total_bytes = prop.totalGlobalMem;
      dev.is_primary_display = true;
      dev.pci_bus = prop.pciBusID;
      dev.pci_device = prop.pciDeviceID;
      dev.pci_domain = prop.pciDomainID;
      detected_devices_.push_back(dev);
    }
  }
#endif

#if defined(__linux__)
  // Check Linux DRM nodes in /sys/class/drm to inspect GPU count and vendor
  DIR* drm_dir = opendir("/sys/class/drm");
  if (drm_dir) {
    struct dirent* entry = nullptr;
    int render_nodes = 0;
    while ((entry = readdir(drm_dir)) != nullptr) {
      if (std::string(entry->d_name).rfind("renderD", 0) == 0) {
        render_nodes++;
        // Read vendor id if available: /sys/class/drm/renderD*/device/vendor
        std::string vendor_path = std::string("/sys/class/drm/") + entry->d_name + "/device/vendor";
        std::ifstream vendor_file(vendor_path);
        if (vendor_file.is_open()) {
          std::string vendor_hex;
          vendor_file >> vendor_hex;
          if (vendor_hex == "0x10de") { // NVIDIA
            if (primary_vendor_ == GpuVendor::Unknown) {
              primary_vendor_ = GpuVendor::Nvidia;
            }
          } else if (vendor_hex == "0x1002") { // AMD
            if (primary_vendor_ == GpuVendor::Unknown) {
              primary_vendor_ = GpuVendor::Amd;
              primary_gpu_name_ = "AMD Radeon GPU";
            }
          } else if (vendor_hex == "0x8086") { // Intel
            if (primary_vendor_ == GpuVendor::Unknown) {
              primary_vendor_ = GpuVendor::Intel;
              primary_gpu_name_ = "Intel Graphics";
            }
          }
        }
      }
    }
    closedir(drm_dir);

    // If more than 1 render node exists, or PRIME render offload is set, it's a hybrid topology
    if (render_nodes > 1 || std::getenv("__NV_PRIME_RENDER_OFFLOAD") || std::getenv("DRI_PRIME")) {
      is_hybrid_topology_ = true;
    }
  }
#endif

#if defined(_WIN32)
  // Windows-specific vendor fallback
  if (primary_vendor_ == GpuVendor::Unknown) {
#if defined(OLIVE_ENABLE_D3D11_INTEROP)
    has_d3d11_support_ = true;
#endif
  }
#endif
}

void GpuEnvironmentProbe::DetectBackendCapabilities()
{
#if defined(OLIVE_ENABLE_CUDA_NVDEC)
  has_nvdec_support_ = (primary_vendor_ == GpuVendor::Nvidia);
#else
  has_nvdec_support_ = false;
#endif

#if defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
  has_vaapi_dmabuf_support_ = true;
#else
  has_vaapi_dmabuf_support_ = false;
#endif

#if defined(_WIN32) && defined(OLIVE_ENABLE_D3D11_INTEROP)
  has_d3d11_support_ = true;
#else
  has_d3d11_support_ = false;
#endif

#if defined(OLIVE_ENABLE_LIBPLACEBO)
  has_libplacebo_support_ = true;
#else
  has_libplacebo_support_ = false;
#endif
}

bool GpuEnvironmentProbe::ShouldUseHardwareDecode(AVCodecID codec_id, AVPixelFormat sw_format) const
{
  // Known CPU-optimized intra / high-bitrate codecs:
  // Hardware decoders either lack support or suffer significant latency vs CPU multi-threaded decoders.
  switch (codec_id) {
    case AV_CODEC_ID_PRORES:
    case AV_CODEC_ID_DNXHD:
    case AV_CODEC_ID_RAWVIDEO:
    case AV_CODEC_ID_PNG:
    case AV_CODEC_ID_TIFF:
    case AV_CODEC_ID_JPEG2000:
      return false;
    default:
      break;
  }

  // 4:2:2 or 4:4:4 formats generally lack consumer NVDEC / VAAPI hardware pipelines
  if (sw_format == AV_PIX_FMT_YUV422P ||
      sw_format == AV_PIX_FMT_YUV422P10LE ||
      sw_format == AV_PIX_FMT_YUV444P ||
      sw_format == AV_PIX_FMT_YUV444P10LE ||
      sw_format == AV_PIX_FMT_YUV444P12LE) {
    return false;
  }

  // Common codecs supported by hardware decoders:
  if (codec_id == AV_CODEC_ID_H264 ||
      codec_id == AV_CODEC_ID_HEVC ||
      codec_id == AV_CODEC_ID_VP9 ||
      codec_id == AV_CODEC_ID_AV1) {
    return has_nvdec_support_ || has_vaapi_dmabuf_support_ || has_d3d11_support_ || has_libplacebo_support_;
  }

  return false;
}

bool GpuEnvironmentProbe::NeedsToneMapping(AVColorSpace col_space, AVColorTransferCharacteristic col_trc) const
{
  if (col_trc == AVCOL_TRC_SMPTE2084 ||   // HDR10 / PQ
      col_trc == AVCOL_TRC_ARIB_STD_B67 || // HLG
      col_space == AVCOL_SPC_BT2020_NCL || // BT.2020 Non-constant luminance
      col_space == AVCOL_SPC_BT2020_CL) {  // BT.2020 Constant luminance
    return true;
  }
  return false;
}

} // namespace olive
