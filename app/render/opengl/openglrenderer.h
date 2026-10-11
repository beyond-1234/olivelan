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
    bool in_use = false;
  };

  static const int kCudaFramePoolCapacity = 8;
  static const int kCudaFramePoolMaxWidth = 4096;
  static const int kCudaFramePoolMaxHeight = 4096;

  std::vector<CudaGLPoolSlot> cuda_frame_pool_;
  QMutex cuda_pool_mutex_;

  int AcquireCudaPoolSlot(int width, int height);
  bool AllocateOrResizePoolSlot(CudaGLPoolSlot &slot, int width, int height);
  void ReleaseCudaPoolSlot(int slot_index);
  bool ReleaseCudaPoolTexture(GLuint tid);
  void DestroyCudaFramePool();

  QHash<GLuint, cudaGraphicsResource_t> cuda_graphics_resources_;
#endif

  static const int kTextureCacheMaxSize;

};

}

#endif // OPENGLCONTEXT_H
