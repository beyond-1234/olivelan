#include "gpu_environment_probe.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QString>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#if defined(OLIVE_ENABLE_CUDA_NVDEC)
#include <cuda_runtime_api.h>
#endif

#if defined(__linux__)
#include <dirent.h>
#include <unistd.h>
#endif

namespace olive {

namespace {

#if defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
// F-7 fix (HAL/Phase6-8 review): "compiled with VA-API" is not the same as "a VA-API
// device exists". Try to actually create a VA-API device and only report the capability
// when that succeeds. Before this, a pure-NVIDIA machine advertised "VA-API ... Available"
// (halbackenddialog) and broadcast h264/hevc/vp9 decode support purely because the backend
// had been compiled in, then failed in ProbeAndInitialize and fell back to Tier 3. This
// applies the same honesty standard patch 0049 set for D3D11.
bool ProbeVaapiDeviceAvailable()
{
  AVBufferRef* device = nullptr;
  const int err = av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);

  if (device) {
    av_buffer_unref(&device);
  }

  return err >= 0;
}
#endif

}

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
  DetectDecodeCapabilityMatrix();
  DetectOpenGlDevice();
  DetectConnectedDisplays();
}

bool GpuEnvironmentProbe::HasConnectedDisplayOn(GpuVendor vendor) const
{
  for (const auto& display : connected_displays_) {
    if (display.gpu == vendor) {
      return true;
    }
  }
  return false;
}

void GpuEnvironmentProbe::DetectConnectedDisplays()
{
  connected_displays_.clear();

#if defined(__linux__)
  QDir drm(QStringLiteral("/sys/class/drm"));
  if (!drm.exists()) {
    return;
  }

  const QStringList entries =
      drm.entryList({QStringLiteral("card*-*")}, QDir::Dirs | QDir::NoDotAndDotDot);

  for (const QString& entry : entries) {
    // "card0-HDMI-A-1" -> card "card0", connector "HDMI-A-1"
    const int dash = entry.indexOf(QLatin1Char('-'));
    if (dash <= 0) {
      continue;
    }

    QFile status(drm.filePath(entry + QStringLiteral("/status")));
    if (!status.open(QIODevice::ReadOnly)) {
      continue;
    }
    if (status.readAll().trimmed() != QByteArrayLiteral("connected")) {
      continue;
    }

    // The connector belongs to the card in its own name; that card's PCI vendor tells us which GPU
    // is actually driving the panel.
    QFile vendor_file(QStringLiteral("/sys/class/drm/%1/device/vendor").arg(entry.left(dash)));
    GpuVendor vendor = GpuVendor::Unknown;
    if (vendor_file.open(QIODevice::ReadOnly)) {
      const QString id = QString::fromLatin1(vendor_file.readAll()).trimmed().toLower();

      if (id == QLatin1String("0x10de")) {
        vendor = GpuVendor::Nvidia;
      } else if (id == QLatin1String("0x1002") || id == QLatin1String("0x1022")) {
        vendor = GpuVendor::Amd;
      } else if (id == QLatin1String("0x8086")) {
        vendor = GpuVendor::Intel;
      }
    }

    DisplayConnection connection;
    connection.connector = entry.toStdString();
    connection.gpu = vendor;
    connected_displays_.push_back(connection);
  }
#endif
}

bool GpuEnvironmentProbe::RecordOpenGlVendor(const char* vendor, bool from_render_context)
{
  if (!vendor || !*vendor) {
    return false;
  }

  if (!gl_vendor_.empty()) {
    // A render-context reading is authoritative. A platform-level (offscreen) context is only a
    // stand-in recorded before the renderer exists, so it must never displace the real thing.
    if (!from_render_context || gl_vendor_from_render_context_) {
      return true;
    }
  }

  gl_vendor_ = vendor;
  gl_is_nvidia_ = QString::fromLatin1(vendor).contains(QStringLiteral("NVIDIA"), Qt::CaseInsensitive);
  gl_vendor_from_render_context_ = from_render_context;

  return true;
}

bool GpuEnvironmentProbe::ProbePlatformOpenGlVendor()
{
  QOffscreenSurface surface;
  surface.create();

  QOpenGLContext context;
  context.create();

  if (!surface.isValid() || !context.isValid() || !context.makeCurrent(&surface)) {
    qWarning() << "[GPU Probe] Could not create an offscreen OpenGL context to determine the"
               << "active GL vendor; interop availability will stay unknown for now.";
    return false;
  }

  const char* vendor = reinterpret_cast<const char*>(context.functions()->glGetString(GL_VENDOR));
  const bool recorded = RecordOpenGlVendor(vendor);

  context.doneCurrent();
  return recorded;
}

void GpuEnvironmentProbe::DetectOpenGlDevice()
{
  // Only trust a context that is already current on this thread. Creating a throw-away offscreen
  // context here is deliberately avoided: on hybrid laptops it can bind a different GPU than the
  // real window context, which would make this check report the wrong device.
  QOpenGLContext* ctx = QOpenGLContext::currentContext();
  if (!ctx || !ctx->isValid()) {
    return;
  }

  if (QOpenGLFunctions* functions = ctx->functions()) {
    RecordOpenGlVendor(reinterpret_cast<const char*>(functions->glGetString(GL_VENDOR)));
  }
}

bool GpuEnvironmentProbe::IsCudaGlInteropBlocked() const
{
  return is_hybrid_topology_ && !gl_vendor_.empty() && !gl_is_nvidia_;
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
  // Windows-specific vendor fallback.
  //
  // Patch 0049 (Phase 8): the D3D11 interop pipeline is a PLACEHOLDER - it is never
  // enabled, on any platform. Policy: Windows uses Tier 3 (reinforced CPU decode)
  // as its hardware fallback until the D3D11 path is actually implemented and
  // validated. Do not set has_d3d11_support_ here; see hardware_pipeline_factory.cpp.
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
  // F-7 fix: device availability, not compile-time presence (see ProbeVaapiDeviceAvailable).
  has_vaapi_dmabuf_support_ = ProbeVaapiDeviceAvailable();
#else
  has_vaapi_dmabuf_support_ = false;
#endif

#if defined(__APPLE__)
  has_videotoolbox_support_ = true;
#else
  has_videotoolbox_support_ = false;
#endif
}

// Patch 0049 (Phase 8): the D3D11 interop pipeline is a placeholder and is NEVER
// supported, on any platform - Windows included. Until the path is implemented and
// validated, Windows falls back to Tier 3 (reinforced CPU decode). Keeping the
// answer a hard false here makes the pipeline unreachable from every consumer
// (auto-routing, forced backend, backend dialog) through one choke point.
bool GpuEnvironmentProbe::SupportsD3D11() const
{
  return false;
}

void GpuEnvironmentProbe::DetectDecodeCapabilityMatrix()
{
  decode_capability_ = HardwareDecodeCapability{};

  // Capabilities are declared per active backend family. Where hardware support for a
  // codec/profile is genuinely absent we must leave it false, so that negotiation rejects
  // the stream instead of the decoder failing after the pipeline has already been built.

#if defined(OLIVE_ENABLE_CUDA_NVDEC)
  if (has_nvdec_support_ && primary_vendor_ == GpuVendor::Nvidia) {
    // NVDEC (Turing and later): H.264, HEVC 8/10-bit, VP9, AV1 8/10-bit.
    decode_capability_.h264 = true;
    decode_capability_.hevc_8bit = true;
    decode_capability_.hevc_10bit = true;
    decode_capability_.vp9 = true;
    decode_capability_.av1_8bit = true;
    decode_capability_.av1_10bit = true;
    return;
  }
#endif

#if defined(__APPLE__)
  if (has_videotoolbox_support_) {
    // VideoToolbox: H.264 and HEVC 8/10-bit universally; VP9 partial (depends on SoC);
    // AV1 decode exists only on Apple Silicon M3+ / recent AMD RX 7000, NOT on Radeon Pro 5300M.
    decode_capability_.h264 = true;
    decode_capability_.hevc_8bit = true;
    decode_capability_.hevc_10bit = true;
    decode_capability_.vp9 = (primary_vendor_ == GpuVendor::Apple);
    decode_capability_.av1_8bit = false;
    decode_capability_.av1_10bit = false;
    return;
  }
#endif

#if defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
  if (has_vaapi_dmabuf_support_) {
    // VA-API: H.264 and HEVC 8/10-bit; VP9/AV1 vary by generation, kept conservative.
    decode_capability_.h264 = true;
    decode_capability_.hevc_8bit = true;
    decode_capability_.hevc_10bit = true;
    decode_capability_.vp9 = true;
    decode_capability_.av1_8bit = false;
    decode_capability_.av1_10bit = false;
    return;
  }
#endif

  // Patch 0049 (Phase 8): the D3D11 decode capability block is removed along with
  // the rest of the placeholder path - Windows reports its decode matrix via the
  // fallback branch above (Tier 3), so no D3D11 capability rows are advertised.

  // No hardware backend: all capabilities remain false.
}

bool GpuEnvironmentProbe::SupportsStream(AVCodecID codec_id, AVPixelFormat sw_format) const
{
  // Derive bit depth from the pixel format descriptor rather than pattern-matching format enums.
  const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(sw_format);
  const int bit_depth = desc ? desc->comp[0].depth : 8;
  const bool is_10bit = (bit_depth > 8);

  switch (codec_id) {
    case AV_CODEC_ID_H264:
      return decode_capability_.h264;
    case AV_CODEC_ID_HEVC:
      return is_10bit ? decode_capability_.hevc_10bit : decode_capability_.hevc_8bit;
    case AV_CODEC_ID_VP9:
      return decode_capability_.vp9;
    case AV_CODEC_ID_AV1:
      return is_10bit ? decode_capability_.av1_10bit : decode_capability_.av1_8bit;
    default:
      return false;
  }
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

  // Format-level pre-filter complete; defer the codec/bit-depth decision to the
  // capability matrix so both queries share a single source of truth.
  return SupportsStream(codec_id, sw_format);
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
