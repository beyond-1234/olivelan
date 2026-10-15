/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2022 Olive Team

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.

***/

#ifndef OPENGLCONTEXT_H
#define OPENGLCONTEXT_H

#if defined(OLIVE_ENABLE_CUDA_NVDEC)
#include <cuda_gl_interop.h>
#include <QHash>
#include <QMutex>
#include <vector>
struct AVFrame;
#endif

#include <QOffscreenSurface>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShader>
#include <QOpenGLVertexArrayObject>
#include <QThread>
#include <QTimer>

#include "render/renderer.h"

namespace olive {

class OpenGLRenderer : public Renderer
{
  Q_OBJECT
public:
  OpenGLRenderer(QObject* parent = nullptr);

  virtual ~OpenGLRenderer() override;

  void Init(QOpenGLContext* existing_ctx);

  virtual bool Init() override;

  virtual void PostDestroy() override;

  virtual void PostInit() override;

  virtual void ClearDestination(olive::Texture *texture = nullptr, double r = 0.0, double g = 0.0, double b = 0.0, double a = 0.0) override;

  virtual QVariant CreateNativeShader(olive::ShaderCode code) override;

  virtual void DestroyNativeShader(QVariant shader) override;

#if defined(OLIVE_ENABLE_CUDA_NVDEC)
  TexturePtr CreateTextureFromCudaFrame(const VideoParams &params, const AVFrame *frame);
#endif

  TexturePtr CreateTextureFromIOSurface(const VideoParams &params, void* iosurface, int width, int height);

  // Patch 0050 / F-10 (HAL/Phase6-8 review): CreateTextureFromEglImage() was removed.
  // It had no callers after 0050 replaced single-image EGLImage wrapping with the
  // two-plane CompositeTextureFromVaapiPlanes() path below.

  // Patch 0050 (item #5): two-plane VA-API DMA-BUF import + YUV->RGB composite.
  // Binds egl_image_y (R8/R16 luma) and egl_image_uv (GR88/GR32/GR1616 chroma) as
  // separate textures - the radeonsi-measured-safe shape (GL_R16/GL_RG16, full
  // per-channel precision) - renders the internal YUV->RGB shader into a pooled
  // GL_RGBA8/GL_RGBA16F slot, and leaves the caller free to destroy both images
  // immediately (the pool slot is a plain GL texture, independent of the DMA-BUF fds).
  TexturePtr CompositeTextureFromVaapiPlanes(const VideoParams &params,
                                             void* egl_image_y, void* egl_image_uv,
                                             int width, int height,
                                             int bits_per_pixel, bool full_range,
                                             float yuv_crv, float yuv_cbu,
                                             float yuv_cgu, float yuv_cgv);
  bool ReleaseNativeSurfacePoolTexture(GLuint tid);

  virtual void UploadToTexture(const QVariant &handle, const VideoParams &params, const void* data, int linesize) override;

  virtual void DownloadFromTexture(const QVariant &handle, const VideoParams &params, void* data, int linesize) override;

  virtual void Flush() override;

  virtual Color GetPixelFromTexture(olive::Texture *texture, const QPointF &pt) override;

protected:
  virtual void Blit(QVariant shader,
                    olive::ShaderJob job,
                    olive::Texture* destination,
                    olive::VideoParams destination_params,
                    bool clear_destination) override;

  virtual QVariant CreateNativeTexture(int width, int height, int depth, PixelFormat format, int channel_count, const void* data = nullptr, int linesize = 0) override;

  virtual void DestroyTexture(Texture *texture) override;

  virtual void DestroyNativeTexture(QVariant texture) override;

  virtual void DestroyInternal() override;

private:
  static GLint GetInternalFormat(PixelFormat format, int channel_layout);

  static GLenum GetPixelType(PixelFormat format);

  static GLenum GetPixelFormat(int channel_count);

  void AttachTextureAsDestination(const QVariant &texture);

  void DetachTextureAsDestination();

  void PrepareInputTexture(GLenum target, Texture::Interpolation interp);

  void ClearDestinationInternal(double r = 0.0, double g = 0.0, double b = 0.0, double a = 0.0);

  GLuint CompileShader(GLenum type, const QString &code);

  QOpenGLContext* context_;

  QOpenGLFunctions* functions_;

  QOffscreenSurface surface_;

  GLuint framebuffer_;

  struct TextureCacheKey {
    int width;
    int height;
    int depth;
    PixelFormat format;
    int channel_count;

    bool operator==(const TextureCacheKey &rhs) const
    {
      return width == rhs.width && height == rhs.height && depth == rhs.depth
          && format == rhs.format && channel_count == rhs.channel_count;
    }
  };

  QMap<GLuint, TextureCacheKey> texture_params_;

#if defined(OLIVE_ENABLE_CUDA_NVDEC)
  struct CudaGLPoolSlot {
    GLuint gl_tex_id = 0;
    cudaGraphicsResource_t cuda_resource = nullptr;
    int width = 0;
    int height = 0;
    PixelFormat format = PixelFormat::U8;
    bool in_use = false;
  };

  static const int kCudaFramePoolCapacity = 8;
  static const int kCudaFramePoolMaxWidth = 4096;
  static const int kCudaFramePoolMaxHeight = 4096;

  std::vector<CudaGLPoolSlot> cuda_frame_pool_;
  QMutex cuda_pool_mutex_;

  // Double-release detection ledger. All fields are guarded by cuda_pool_mutex_.
  // Invariant: borrowed == returned + in_use.
  int cuda_borrowed_count_ = 0;
  int cuda_returned_count_ = 0;
  int cuda_in_use_count_ = 0;

  int AcquireCudaPoolSlot(int width, int height, PixelFormat format);
  bool AllocateOrResizePoolSlot(CudaGLPoolSlot &slot, int width, int height, PixelFormat format);
  void ReleaseCudaPoolSlot(int slot_index);
  bool ReleaseCudaPoolTexture(GLuint tid);
  void DestroyCudaFramePool();
  int MarkCudaSlotBorrowed(int slot_index);
  int ReleaseCudaPoolSlotInternal(int slot_index);

  QHash<GLuint, cudaGraphicsResource_t> cuda_graphics_resources_;
#endif

  struct NativeSurfacePoolSlot {
    GLuint gl_tex_id = 0;
    int width = 0;
    int height = 0;
    // Patch 0050: slots are format-keyed. A GL_RGBA8 slot and a GL_RGBA16F slot of the
    // same dimensions must NOT be shared across streams - reuse matching now requires
    // identical format, mirroring CudaGLPoolSlot (patch 0027).
    PixelFormat format = PixelFormat::U8;
    bool in_use = false;
  };

  static const int kNativeSurfacePoolCapacity = 8;
  std::vector<NativeSurfacePoolSlot> native_surface_pool_;
  QMutex native_surface_pool_mutex_;

  // Double-release detection ledger, mirroring the CUDA pool (CR-08 / CR-12). Guarded by
  // native_surface_pool_mutex_. Invariant: borrowed == returned + in_use.
  int native_borrowed_count_ = 0;
  int native_returned_count_ = 0;
  int native_in_use_count_ = 0;

  // Patch 0050: format-aware acquisition. A slot's storage is allocated for a specific
  // internal format (GL_RGBA8 for 8-bit YUV, GL_RGBA16F for 10-bit P010), so reuse
  // requires an exact format match - same fix as the CUDA pool got in patch 0027.
  int AcquireNativeSurfaceSlot(int width, int height, GLenum target, PixelFormat format,
                               GLuint* out_texture_id = nullptr);
  void DestroyNativeSurfacePool();

  // Phase 6 (patch 0026): double-buffered Pixel Unpack Buffer ring for asynchronous
  // CPU->GPU streaming uploads. Frame N is memcpy'd into the next PBO while the GPU
  // DMA engine may still be consuming the previous one via glTexSubImage2D, hiding the
  // main-thread stall that a client-memory glTexSubImage2D would otherwise pay. Only
  // engaged for tightly-strided 2D streams; every other path keeps the old behaviour.
  static const int kUploadPboRingSize = 2;
  GLuint upload_pbos_[kUploadPboRingSize] = {0, 0};
  int upload_pbo_index_ = 0;

  bool UploadToTextureViaPbo(const VideoParams &params, const void *data, int linesize);
  void DestroyUploadPboRing();

  static const int kTextureCacheMaxSize;

};

}

#endif // OPENGLCONTEXT_H
