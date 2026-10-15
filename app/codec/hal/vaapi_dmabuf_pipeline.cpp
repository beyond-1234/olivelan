#include "vaapi_dmabuf_pipeline.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

#include <QDebug>
#include "render/opengl/openglrenderer.h"

#if defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
#include <libavutil/hwcontext_vaapi.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
#include <unistd.h>
#endif

namespace olive {

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

  // The import below maps the Y plane to EGL plane 0 and the chroma plane to EGL plane 1, so
  // a two-layer export is mandatory. Guard before touching layers[1] to avoid an out-of-bounds
  // read on drivers that export a single combined layer.
  if (prime_desc.num_layers < 2) {
    qWarning() << "[VAAPI-HAL] Surface exported with" << prime_desc.num_layers
               << "layer(s); two-plane YUV import requires 2, skipping frame";
    return false;
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
  #define EGL_WIDTH 0x3057
  #define EGL_HEIGHT 0x3056
  #endif

  typedef void* (*PFNEGLGETPROCADDRESS)(const char*);
  typedef void* (*PFNEGLCREATEIMAGEKHR)(void*, void*, unsigned int, void*, const intptr_t*);
  typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(unsigned int, void*);

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

  const intptr_t attribs[] = {
    EGL_WIDTH, static_cast<intptr_t>(prime_desc.width),
    EGL_HEIGHT, static_cast<intptr_t>(prime_desc.height),
    EGL_LINUX_DRM_FOURCC_EXT, static_cast<intptr_t>(prime_desc.layers[0].drm_format),
    EGL_DMA_BUF_PLANE0_FD_EXT, static_cast<intptr_t>(prime_desc.objects[0].fd),
    EGL_DMA_BUF_PLANE0_OFFSET_EXT, static_cast<intptr_t>(prime_desc.layers[0].offset[0]),
    EGL_DMA_BUF_PLANE0_PITCH_EXT, static_cast<intptr_t>(prime_desc.layers[0].pitch[0]),
    EGL_DMA_BUF_PLANE1_FD_EXT, static_cast<intptr_t>(prime_desc.objects[1].fd),
    EGL_DMA_BUF_PLANE1_OFFSET_EXT, static_cast<intptr_t>(prime_desc.layers[1].offset[0]),
    EGL_DMA_BUF_PLANE1_PITCH_EXT, static_cast<intptr_t>(prime_desc.layers[1].pitch[0]),
    0x3038 // EGL_NONE
  };

  egl_image_ = pfn_eglCreateImageKHR(nullptr, nullptr, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
  if (!egl_image_) {
    qWarning() << "[VAAPI-HAL] eglCreateImageKHR failed for surface";
    return false;
  }

  // Surface bit-depth telemetry. With VA_EXPORT_SURFACE_SEPARATE_LAYERS a 10-bit source is
  // reported either as a single DRM_FORMAT_P010 layer or, on most drivers, as an R16 luma layer
  // plus a GR1616 chroma layer. Detect both so the precision of the imported planes is at least
  // visible in the log rather than silently assumed.
#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010 0x30313050
#endif
#ifndef DRM_FORMAT_GR1616
#define DRM_FORMAT_GR1616 0x32335247
#endif
  const bool is_10bit = (prime_desc.layers[0].drm_format == DRM_FORMAT_P010)
                        || (prime_desc.layers[1].drm_format == DRM_FORMAT_GR1616);
  if (is_10bit) {
    static bool noted = false;
    if (!noted) {
      noted = true;
      qInfo() << "[VAAPI-HAL] 10-bit surface detected (Y fourcc" << Qt::hex << prime_desc.layers[0].drm_format
              << ", UV fourcc" << prime_desc.layers[1].drm_format << Qt::dec
              << "); both 16-bit planes are imported intact. Explicit capability note: this"
                 " pipeline delivers the planes as imported - it does NOT convert or truncate"
                 " them itself. Full-precision delivery (two-plane YUV->RGB shader + F16 target)"
                 " is future work pending validation on VA-API hardware; until then the"
                 " downstream consumer's handling of 16-bit plane textures is unvalidated."
                 " No silent downsample happens here.";
    }
  }

  if (!renderer_) {
    qWarning() << "[VAAPI-HAL] Renderer context not attached; cannot transfer frame";
    return false;
  }

  TexturePtr tex = renderer_->CreateTextureFromEglImage(params, egl_image_, prime_desc.width, prime_desc.height);
  if (!tex) {
    qWarning() << "[VAAPI-HAL] Failed to create owning texture from EGLImage pool";
    return false;
  }

  out_frame.texture = tex;
  out_frame.pts = hw_frame->pts;
  out_frame.width = hw_frame->width;
  out_frame.height = hw_frame->height;
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
