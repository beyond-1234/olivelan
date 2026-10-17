#include "vaapi_dmabuf_pipeline.h"
#include "gpu_environment_probe.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mastering_display_metadata.h>
}

#include <QDebug>
#include <QScopeGuard>
#include "render/opengl/openglrenderer.h"

#if defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
#include <libavutil/hwcontext_vaapi.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <vector>

// swscale.h must be inside extern "C" (see ffmpegutils.h for the same pattern);
// including it standalone here mangles the link names.
extern "C" {
#include <libswscale/swscale.h>
}

#include "common/ffmpegutils.h"
#endif

namespace olive {

VideoParams VaapiDmaBufPipeline::ResolveColorMetadata(const VideoParams& params,
                                                       const AVFrame* frame)
{
  VideoParams effective = params;

  if (!frame) {
    return effective;
  }

  if (frame->colorspace != AVCOL_SPC_UNSPECIFIED) {
    effective.set_color_space(frame->colorspace);
  }

  if (frame->color_primaries != AVCOL_PRI_UNSPECIFIED) {
    effective.set_color_primaries(frame->color_primaries);
  }

  if (frame->color_trc != AVCOL_TRC_UNSPECIFIED) {
    effective.set_color_trc(frame->color_trc);

    // Only fill in the HDR display space; an SDR frame must not clear an explicit project
    // override. The stale-cache case this fixes is an HDR stream whose saved params predate the
    // import that found PQ/HLG.
    const QString hdr_colorspace =
        VideoParams::GetHdrColorSpaceForTransfer(frame->color_trc);
    if (!hdr_colorspace.isEmpty()) {
      effective.set_colorspace(hdr_colorspace);
    }
  }

  if (frame->color_range == AVCOL_RANGE_JPEG) {
    effective.set_color_range(VideoParams::kColorRangeFull);
  } else if (frame->color_range == AVCOL_RANGE_MPEG) {
    effective.set_color_range(VideoParams::kColorRangeLimited);
  }

  for (int i = 0; i < frame->nb_side_data; i++) {
    const AVFrameSideData* side_data = frame->side_data[i];
    if (side_data
        && side_data->type == AV_FRAME_DATA_MASTERING_DISPLAY_METADATA
        && side_data->size >= int(sizeof(AVMasteringDisplayMetadata))) {
      const auto* metadata =
          reinterpret_cast<const AVMasteringDisplayMetadata*>(side_data->data);
      if (metadata->has_luminance && metadata->max_luminance.den != 0) {
        effective.set_mastering_peak_nits(VideoParams::MasteringDisplayMaxLuminanceToNits(
            metadata->max_luminance.num, metadata->max_luminance.den));
      }
    }
  }

  return effective;
}

void VaapiDmaBufPipeline::ReleaseExportedFds()
{
#if defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
  for (int fd : exported_fds_) {
    if (fd >= 0) {
      close(fd);
    }
  }
#endif
  exported_fds_.clear();
}

VaapiDmaBufPipeline::VaapiDmaBufPipeline(OpenGLRenderer* renderer)
  : renderer_(renderer)
{
}

VaapiDmaBufPipeline::~VaapiDmaBufPipeline()
{
  Close();
}

void VaapiDmaBufPipeline::SetRenderer(Renderer* renderer)
{
  renderer_ = dynamic_cast<OpenGLRenderer*>(renderer);
}

bool VaapiDmaBufPipeline::ProbeAndInitialize(AVCodecContext* codec_ctx, int max_w, int max_h)
{
#if !defined(__linux__) || !defined(OLIVE_ENABLE_VAAPI_DMABUF)
  Q_UNUSED(codec_ctx);
  Q_UNUSED(max_w);
  Q_UNUSED(max_h);
  return false;
#else
  if (!codec_ctx) {
    return false;
  }

  max_width_ = max_w;
  max_height_ = max_h;

  int err = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
  if (err < 0 || !hw_device_ctx_) {
    qWarning() << "[VAAPI-HAL] Failed to create VAAPI device context, code:" << err;
    return false;
  }

  codec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
  if (!codec_ctx->hw_device_ctx) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
    return false;
  }

  qInfo() << "[VAAPI-HAL] Initialized VA-API DMA-BUF hardware pipeline";
  return true;
#endif
}

bool VaapiDmaBufPipeline::ProcessHardwareFrame(AVFrame* hw_frame, const VideoParams& params, HardwareDecodedFrame& out_frame)
{
#if !defined(__linux__) || !defined(OLIVE_ENABLE_VAAPI_DMABUF)
  Q_UNUSED(hw_frame);
  Q_UNUSED(params);
  Q_UNUSED(out_frame);
  return false;
#else
  if (!hw_frame || hw_frame->format != AV_PIX_FMT_VAAPI) {
    return false;
  }

  // Patch 0092 (P2-7): check this before exporting a surface or creating any EGLImage. The old
  // check ran after both images existed, so that branch leaked two images per frame.
  if (!renderer_) {
    qWarning() << "[VAAPI-HAL] Renderer context not attached; cannot transfer frame";
    return false;
  }

  // Patch 0094: publish the real outcome of this transfer to the environment probe. The UI must
  // distinguish "VA-API device exists" from "EGL/DMA-BUF import actually works"; this is the only
  // place that knows the latter. A false result covers export, EGLImage creation, and composite.
  bool import_succeeded = false;
  auto import_result_guard = qScopeGuard(
      [&]() { GpuEnvironmentProbe::Instance().RecordVaapiImportResult(import_succeeded); });

  VASurfaceID surface_id = reinterpret_cast<uintptr_t>(hw_frame->data[3]);
  VADRMPRIMESurfaceDescriptor prime_desc{};
  
  auto* va_device_ctx = reinterpret_cast<AVVAAPIDeviceContext*>(
      reinterpret_cast<AVHWDeviceContext*>(hw_device_ctx_->data)->hwctx);
  VADisplay va_display = va_device_ctx->display;

  VAStatus status = vaExportSurfaceHandle(va_display,
                                          surface_id,
                                          VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                          VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
                                          &prime_desc);
  if (status != VA_STATUS_SUCCESS) {
    qWarning() << "[VAAPI-HAL] vaExportSurfaceHandle failed:" << status;
    return false;
  }

  // vaExportSurfaceHandle returned caller-owned fds on every call; drop the previous frame's
  // set before adopting the new one. Only object[0] used to be tracked, leaking one fd per
  // plane beyond the first (P010 and NV12 both export one object per plane here).
  ReleaseExportedFds();
  for (uint32_t i = 0; i < prime_desc.num_objects; ++i) {
    exported_fds_.push_back(prime_desc.objects[i].fd);
  }

  // Every return from here on closes the fds exported above; the normal path explicitly releases
  // them right after compositing, which makes this a no-op.
  auto fd_guard = qScopeGuard([this]() { ReleaseExportedFds(); });

  // Patch 0050 (item #5): import strategy selected by measurement on Radeon 780M /
  // Mesa radeonsi 25.2.8 (see p010_export_probe / vaapi_import_probe).
  //
  // SEPARATE_LAYERS + TWO EGLImages won the A/B measurement:
  //   - Path A (COMPOSED_LAYERS, single P010 EGLImage) binds, but Mesa reports the
  //     composite as a sizeless legacy GL_RGB with channel sizes R16 G0 B0 - the
  //     chroma channels report zero, so 10-bit precision would be silently lost.
  //   - Path B (SEPARATE_LAYERS, dual EGLImages) binds GL_R16 (luma, full size) and
  //     GL_RG16 (chroma, half size), every channel full 16-bit. radeonsi exports
  //     GR32 ('G','R','3','2' = 0x32335247) for chroma; some drivers use the newer
  //     DRM_FORMAT_GR1616 (0x36315247) - both are recognised below.
  //
  // The old code was broken on radeonsi regardless: it passed layers[0]'s fourcc
  // (R16, single-plane) with TWO plane attribs and read objects[1], which does not
  // exist (radeonsi exports both separate layers from ONE object).
  if (prime_desc.num_layers < 2) {
    qWarning() << "[VAAPI-HAL] Surface exported with" << prime_desc.num_layers
               << "layer(s); two-plane YUV import requires 2, skipping frame";
    return false;
  }

  // Surface bit-depth telemetry (patch 0050: constant-name fix + GR32 recognition).
  // radeonsi exports 10-bit chroma as DRM_FORMAT_GR32 ('G','R','3','2' = 0x32335247);
  // that numeric value was previously mislabelled DRM_FORMAT_GR1616 in this file.
  // The real GR1616 is 0x36315247. Both are recognised; separate-layer export on
  // radeonsi reports luma as R16 and chroma as GR32.
#ifndef DRM_FORMAT_R16
#define DRM_FORMAT_R16 0x20363152
#endif
#ifndef DRM_FORMAT_GR32
#define DRM_FORMAT_GR32 0x32335247
#endif
#ifndef DRM_FORMAT_GR1616
#define DRM_FORMAT_GR1616 0x36315247
#endif
  static_assert(DRM_FORMAT_GR32 != DRM_FORMAT_GR1616,
                "patch 0050: GR32 (Mesa radeonsi) and GR1616 (drm_fourcc) are distinct fourccs");
  constexpr uint32_t kFccR16 = DRM_FORMAT_R16;
  constexpr uint32_t kFccGR32 = DRM_FORMAT_GR32;
  constexpr uint32_t kFccGR1616 = DRM_FORMAT_GR1616;
  const bool is_10bit = (prime_desc.layers[0].drm_format == kFccR16)
                        || (prime_desc.layers[1].drm_format == kFccGR32)
                        || (prime_desc.layers[1].drm_format == kFccGR1616);
  if (is_10bit) {
    static std::atomic<bool> noted{false};
    bool expected = false;
    if (noted.compare_exchange_strong(expected, true)) {
      qInfo() << "[VAAPI-HAL] 10-bit surface detected (Y fourcc" << Qt::hex << prime_desc.layers[0].drm_format
              << ", UV fourcc" << prime_desc.layers[1].drm_format << Qt::dec
              << "); 16-bit planes are imported intact (GL_R16/GL_RG16) and composed into a"
                 " GL_RGBA16F pool slot via the internal YUV->RGB shader (validated on"
                 " Radeon 780M / Mesa radeonsi 25.2.8 by p010_export_probe + vaapi_import_probe)."
                 " No silent downsample.";
    }
  }

  // EGLImage creation and OpenGL texture binding
  #ifndef EGL_LINUX_DMA_BUF_EXT
  #define EGL_LINUX_DMA_BUF_EXT 0x3270
  #define EGL_LINUX_DRM_FOURCC_EXT 0x3271
  #define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
  #define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
  #define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
  #define EGL_DMA_BUF_PLANE1_FD_EXT 0x3275
  #define EGL_DMA_BUF_PLANE1_OFFSET_EXT 0x3276
  #define EGL_DMA_BUF_PLANE1_PITCH_EXT 0x3277
  #define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
  #define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
  #define EGL_WIDTH 0x3057
  #define EGL_HEIGHT 0x3056
  #endif

  typedef void* (*PFNEGLGETPROCADDRESS)(const char*);
  typedef void* (*PFNEGLGETCURRENTDISPLAY)(void);
  typedef void* (*PFNEGLCREATEIMAGEKHR)(void*, void*, unsigned int, void*, const int32_t*);
  typedef void* EGLImageKHRHandle;
  typedef void (*PFNEGLDESTROYIMAGEKHR)(void*, EGLImageKHRHandle);
  typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(unsigned int, void*);

  // Patch 0092 (P2-7): images/fds are owned by guards so every early return destroys them in the
  // order Mesa requires (images before fds). Declaration order matters: `images` is constructed
  // after `fd_guard` and therefore destroyed first.
  struct EglImageGuard {
    void* display = nullptr;
    void* y = nullptr;
    void* uv = nullptr;
    PFNEGLDESTROYIMAGEKHR destroy = nullptr;

    ~EglImageGuard() { Reset(); }

    void Reset()
    {
      if (destroy) {
        if (y) destroy(display, y);
        if (uv) destroy(display, uv);
      }
      y = nullptr;
      uv = nullptr;
    }
  };

  // Resolve every extension pointer on each call. A one-time static cache would freeze nullptrs if
  // the first invocation had no current GL context, permanently disabling DMA-BUF import afterwards
  // (fixes CR-11). EGL functions are reached through the Qt GL context's proc-address resolver.
  QOpenGLContext* ctx = QOpenGLContext::currentContext();
  if (!ctx) {
    qWarning() << "[VAAPI-HAL] No current OpenGL context while resolving EGL extensions";
    return false;
  }
  auto pfn_eglGetProcAddress = reinterpret_cast<PFNEGLGETPROCADDRESS>(ctx->getProcAddress("eglGetProcAddress"));
  if (!pfn_eglGetProcAddress) {
    qWarning() << "[VAAPI-HAL] eglGetProcAddress unavailable; cannot import DMA-BUF surface";
    return false;
  }
  auto pfn_eglCreateImageKHR = reinterpret_cast<PFNEGLCREATEIMAGEKHR>(pfn_eglGetProcAddress("eglCreateImageKHR"));
  auto pfn_glEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(pfn_eglGetProcAddress("glEGLImageTargetTexture2DOES"));

  if (!pfn_eglCreateImageKHR || !pfn_glEGLImageTargetTexture2DOES) {
    qWarning() << "[VAAPI-HAL] EGL DMA-BUF import unavailable; cannot import surface";
    return false;
  }

  // eglCreateImageKHR needs the EGLDisplay the current context is bound to. Passing EGL_NO_DISPLAY
  // is not a legal "use the current one" shorthand - it fails with EGL_BAD_DISPLAY and returns
  // EGL_NO_IMAGE_KHR, which silently demoted the DMA-BUF path to a GPU->CPU readback per frame.
  auto pfn_eglGetCurrentDisplay =
      reinterpret_cast<PFNEGLGETCURRENTDISPLAY>(pfn_eglGetProcAddress("eglGetCurrentDisplay"));
  if (!pfn_eglGetCurrentDisplay) {
    qWarning() << "[VAAPI-HAL] eglGetCurrentDisplay unavailable; cannot import DMA-BUF surface";
    return false;
  }
  void* egl_display = pfn_eglGetCurrentDisplay();
  if (!egl_display) {
    qWarning() << "[VAAPI-HAL] No current EGL display; cannot import DMA-BUF surface";
    return false;
  }

  // Patch 0050: dual-EGLImage import (measured winner). Each layer is a single-plane
  // fourcc (R8/R16 luma, GR88/GR32/GR1616 chroma) with its own width/height/offset -
  // the only attribs an EGLImage of that fourcc is allowed to carry. Plane fds come
  // from layer.object_index[]; no assumption that plane N lives in objects[N]
  // (radeonsi exports both layers from one object).
  const uint32_t y_obj = prime_desc.layers[0].object_index[0];
  const uint32_t uv_obj = prime_desc.layers[1].object_index[0];

  // DRM_PRIME_2 exports a tiled surface with a per-object modifier (radeonsi reports e.g.
  // 0x200000010401b04). When the import-modifiers extension is present the modifier MUST be handed
  // to eglCreateImageKHR: otherwise Mesa assumes DRM_FORMAT_MOD_LINEAR and rejects the tiled buffer,
  // which silently demoted this path to a GPU->CPU readback (av_hwframe_transfer_data) every frame.
  bool modifiers_supported = false;
  {
    typedef const char* (*PFNEGLQUERYSTRING)(void*, int);
    auto pfn_eglQueryString =
        reinterpret_cast<PFNEGLQUERYSTRING>(pfn_eglGetProcAddress("eglQueryString"));
    const char* extensions =
        pfn_eglQueryString ? pfn_eglQueryString(egl_display, 0x3055 /* EGL_EXTENSIONS */) : nullptr;
    modifiers_supported =
        extensions && std::strstr(extensions, "EGL_EXT_image_dma_buf_import_modifiers");
  }

  auto make_plane_attribs = [&](int plane_width, int plane_height, uint32_t fourcc, int fd,
                                int offset, int pitch, uint64_t modifier) {
    std::vector<int32_t> attribs = {
      static_cast<int32_t>(EGL_WIDTH), static_cast<int32_t>(plane_width),
      static_cast<int32_t>(EGL_HEIGHT), static_cast<int32_t>(plane_height),
      static_cast<int32_t>(EGL_LINUX_DRM_FOURCC_EXT), static_cast<int32_t>(fourcc),
      static_cast<int32_t>(EGL_DMA_BUF_PLANE0_FD_EXT), static_cast<int32_t>(fd),
      static_cast<int32_t>(EGL_DMA_BUF_PLANE0_OFFSET_EXT), static_cast<int32_t>(offset),
      static_cast<int32_t>(EGL_DMA_BUF_PLANE0_PITCH_EXT), static_cast<int32_t>(pitch),
    };
    if (modifiers_supported) {
      attribs.push_back(static_cast<int32_t>(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT));
      attribs.push_back(static_cast<int32_t>(modifier & 0xffffffffull));
      attribs.push_back(static_cast<int32_t>(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT));
      attribs.push_back(static_cast<int32_t>(modifier >> 32));
    }
    attribs.push_back(0x3038); // EGL_NONE
    return attribs;
  };

  const std::vector<int32_t> attribs_y =
      make_plane_attribs(prime_desc.width, prime_desc.height, prime_desc.layers[0].drm_format,
                         prime_desc.objects[y_obj].fd, prime_desc.layers[0].offset[0],
                         prime_desc.layers[0].pitch[0], prime_desc.objects[y_obj].drm_format_modifier);
  const std::vector<int32_t> attribs_uv =
      make_plane_attribs(prime_desc.width / 2, prime_desc.height / 2, prime_desc.layers[1].drm_format,
                         prime_desc.objects[uv_obj].fd, prime_desc.layers[1].offset[0],
                         prime_desc.layers[1].pitch[0], prime_desc.objects[uv_obj].drm_format_modifier);

  auto pfn_eglDestroyImageKHR =
      reinterpret_cast<PFNEGLDESTROYIMAGEKHR>(pfn_eglGetProcAddress("eglDestroyImageKHR"));
  if (!pfn_eglDestroyImageKHR) {
    qWarning() << "[VAAPI-HAL] eglDestroyImageKHR unavailable; refusing to create images that"
               << "cannot be released";
    return false;
  }

  EglImageGuard images;
  images.display = egl_display;
  images.destroy = pfn_eglDestroyImageKHR;
  images.y = pfn_eglCreateImageKHR(egl_display, nullptr, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs_y.data());
  images.uv = pfn_eglCreateImageKHR(egl_display, nullptr, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs_uv.data());
  if (!images.y || !images.uv) {

    // Reaching this point means a real driver/stream limitation, NOT an API misuse.
    //
    // History, because this note was wrong twice: patches 0077-0086 chased this failure through
    // dma-buf formats, modifiers, plane counts and the EGL display, and produced two claims that
    // had to be retracted (a bogus EGL_SUCCESS, and a bogus "the driver only accepts LINEAR" -
    // both were artifacts of malformed probe calls). Patch 0087 found the actual cause: this
    // attrib array used to be intptr_t/Mesa reads EGLint (int32_t) on x86_64, so the driver read
    // the attributes at the wrong stride, misparsed width/height as the next key, and returned
    // EGL_BAD_PARAMETER (0x300c) for every variant. With the correct EGLint array these same
    // R8+GR88 planes import successfully on radeonsi (Radeon 780M, Mesa 25.2.8).
    //
    // So: if a stream fails here now, log the fourcc/modifier/plane layout below and compare it
    // against values known to work - but do not revive the old "format/modifier/driver can never
    // be imported" conclusions without a fresh measurement of this specific case.
    //
    // For any future composed path: NV12 reports external_only=1, so it must be bound to
    // GL_TEXTURE_EXTERNAL_OES - GL_TEXTURE_2D would fail at bind time.

    qWarning() << "[VAAPI-HAL] eglCreateImageKHR failed for plane images (y:"
               << static_cast<bool>(images.y) << " uv:" << static_cast<bool>(images.uv) << ")"
               << "- plane fourcc" << Qt::hex << prime_desc.layers[0].drm_format
               << "/" << prime_desc.layers[1].drm_format << "with modifier"
               << prime_desc.objects[y_obj].drm_format_modifier
               << "is not importable by this EGL driver; this stream uses a GPU->CPU readback.";
    // EglImageGuard destroys any image that was created; fd_guard closes the exported fds.
    return false;
  }

  // Determine the bit depth of the planes for the composite shader and the pool slot
  // format (GL_RGBA16F for 10-bit, GL_RGBA8 for 8-bit - see AcquireNativeSurfaceSlot).
  // The slot outlives the DMA-BUF fds, so both images are destroyed right after the
  // composite pass below, and the exported fds are closed immediately.
  const bool y16 = (prime_desc.layers[0].drm_format == kFccR16);
  const bool uv16 = (prime_desc.layers[1].drm_format == kFccGR32
                     || prime_desc.layers[1].drm_format == kFccGR1616);
  const int bits = (y16 || uv16) ? 10 : 8;

  // Patch 0096: per-frame colour metadata wins over the cached project copy. The decoded frame
  // knows the stream it actually produced; VideoParams may have been serialized by an earlier
  // 8-bit import or edited after the fact.
  const VideoParams effective = ResolveColorMetadata(params, hw_frame);

  // YUV matrix coefficients from the resolved metadata (patch 0042 fields travel as plain ints
  // holding FFmpeg's stable ABI constants). Same source as the CPU path.
  auto* sws_coeffs = sws_getCoefficients(FFmpegUtils::GetSwsColorspaceFromAVColorSpace(
      static_cast<AVColorSpace>(effective.color_space())));
  float crv = 0.0f, cbu = 0.0f, cgu = 0.0f, cgv = 0.0f;
  if (sws_coeffs) {
    crv = static_cast<float>(sws_coeffs[0]) / 65536.0f;
    cbu = static_cast<float>(sws_coeffs[1]) / 65536.0f;
    cgu = static_cast<float>(sws_coeffs[2]) / 65536.0f;
    cgv = static_cast<float>(sws_coeffs[3]) / 65536.0f;
  }
  const bool full_range = (effective.color_range() == VideoParams::kColorRangeFull);

  TexturePtr tex = renderer_->CompositeTextureFromVaapiPlanes(effective, images.y, images.uv,
                                                              prime_desc.width, prime_desc.height,
                                                              bits, full_range,
                                                              crv, cbu, cgu, cgv);
  static std::atomic<bool> s_zero_copy_announced{false};
  if (tex && !s_zero_copy_announced.load()) {
    bool expected = false;
    if (s_zero_copy_announced.compare_exchange_strong(expected, true)) {
      qInfo() << "[VAAPI-HAL] DMA-BUF zero-copy pipeline successfully established on"
              << prime_desc.width << "x" << prime_desc.height << "surface;"
              << "bits:" << bits << "- direct GPU texture compositing active (zero CPU readbacks);"
              << "colour matrix" << effective.color_space()
              << "range" << (full_range ? "full" : "limited")
              << "transfer" << effective.color_trc()
              << "display space" << effective.colorspace()
              << "mastering peak" << effective.mastering_peak_nits();
    }
  }

  // The composite has rendered into the pooled GL texture; both EGLImages and the fds
  // are dead weight now. Destroying the images before the fd close keeps the
  // image-before-fd destruction order Mesa requires. The guards handle every other return path.
  images.Reset();
  ReleaseExportedFds();

  if (!tex) {
    qWarning() << "[VAAPI-HAL] Failed to composite VA-API planes into pool texture";
    return false;
  }

  out_frame.texture = tex;
  out_frame.pts = hw_frame->pts;
  out_frame.width = hw_frame->width;
  out_frame.height = hw_frame->height;
  import_succeeded = true;
  return true;
#endif
}

void VaapiDmaBufPipeline::Close()
{
  ReleaseExportedFds();

  if (hw_device_ctx_) {
    av_buffer_unref(&hw_device_ctx_);
    hw_device_ctx_ = nullptr;
  }
}

} // namespace olive
