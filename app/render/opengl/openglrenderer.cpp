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

#include "openglrenderer.h"

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>

#if defined(OLIVE_ENABLE_CUDA_NVDEC)
extern "C" {
#include <libavutil/hwcontext.h>
}
#include "cuda_nv12_to_rgba.h"
#endif

#if defined(__APPLE__)
#include <CoreVideo/CoreVideo.h>
#include <IOSurface/IOSurface.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/CGLCurrent.h>
#endif

#include "config/config.h"

namespace olive {

const int OpenGLRenderer::kTextureCacheMaxSize = 5000;

const QVector<GLfloat> blit_vertices = {
  -1.0f, -1.0f, 0.0f,
  1.0f, -1.0f, 0.0f,
  1.0f, 1.0f, 0.0f,

  -1.0f, -1.0f, 0.0f,
  -1.0f, 1.0f, 0.0f,
  1.0f, 1.0f, 0.0f
};

const QVector<GLfloat> blit_texcoords = {
  0.0f, 0.0f,
  1.0f, 0.0f,
  1.0f, 1.0f,

  0.0f, 0.0f,
  0.0f, 1.0f,
  1.0f, 1.0f
};

class ErrorPrinter {
public:
  ErrorPrinter(const char* name, QOpenGLFunctions* f)
  {
    GLuint err = f->glGetError();
    if (err > 0)
      qDebug() << name << "entered with" << err;

    name_ = name;
    functions_ = f;
  }

  ~ErrorPrinter()
  {
    GLuint err = functions_->glGetError();
    if (err > 0)
      qDebug() << name_ << "exited with" << err;
  }

private:
  const char* name_;

  QOpenGLFunctions* functions_;

};

#define PRINT_GL_ERRORS ErrorPrinter __e(__FUNCTION__, functions_)

#define GL_PREAMBLE \
  //QMutexLocker __l(&global_opengl_mutex);

//QMutex global_opengl_mutex;

OpenGLRenderer::OpenGLRenderer(QObject* parent) :
  Renderer(parent),
  context_(nullptr),
  framebuffer_(0)
{
}

OpenGLRenderer::~OpenGLRenderer()
{
  Destroy();
  PostDestroy();
}

void OpenGLRenderer::Init(QOpenGLContext *existing_ctx)
{
  if (context_) {
    qCritical() << "Can't initialize already initialized OpenGLRenderer";
    return;
  }

  context_ = existing_ctx;
}

bool OpenGLRenderer::Init()
{
  GL_PREAMBLE;

  if (context_) {
    qCritical() << "Can't initialize already initialized OpenGLRenderer";
    return false;
  }

  surface_.create();

  context_ = new QOpenGLContext(this);
  context_->setShareContext(QOpenGLContext::globalShareContext());
  if (!context_->create()) {
    qCritical() << "Failed to create OpenGL context";
    return false;
  }

  context_->moveToThread(this->thread());

  return true;
}

void OpenGLRenderer::PostDestroy()
{
  // Destroy surface if we created it
  if (surface_.isValid()) {
    surface_.destroy();
  }
}

void OpenGLRenderer::PostInit()
{
  GL_PREAMBLE;

  // Make context current on that surface
  if (context_->parent() == this && !context_->makeCurrent(&surface_)) {
    qCritical() << "Failed to makeCurrent() on offscreen surface in thread" << thread();
    return;
  }

  functions_ = context_->functions();

  // Store OpenGL functions instance
  functions_->glBlendFunc(GL_ONE, GL_ZERO);

  // Set up framebuffer used for various things
  functions_->glGenFramebuffers(1, &framebuffer_);
}

void OpenGLRenderer::DestroyInternal()
{
  if (context_) {
    GL_PREAMBLE;

#if defined(OLIVE_ENABLE_CUDA_NVDEC)
    DestroyCudaFramePool();
    for (cudaGraphicsResource_t resource : cuda_graphics_resources_) {
      cudaGraphicsUnregisterResource(resource);
    }
    cuda_graphics_resources_.clear();
#endif

    DestroyNativeSurfacePool();

    DestroyUploadPboRing();

    // Delete framebuffer
    functions_->glDeleteFramebuffers(1, &framebuffer_);
    framebuffer_ = 0;

    // Delete context if it belongs to us
    if (context_->parent() == this) {
      delete context_;
    }
    context_ = nullptr;
  }
}

void OpenGLRenderer::ClearDestination(Texture *texture, double r, double g, double b, double a)
{
  GL_PREAMBLE;

  if (texture) {
    AttachTextureAsDestination(texture->id());
  }

  ClearDestinationInternal(r, g, b, a);

  if (texture) {
    DetachTextureAsDestination();
  }
}

QVariant OpenGLRenderer::CreateNativeTexture(int width, int height, int depth, PixelFormat format, int channel_count, const void *data, int linesize)
{
  GL_PREAMBLE;

  bool is_3d = depth > 1;

  // Generate new texture
  GLuint texture;
  functions_->glGenTextures(1, &texture);
  texture_params_.insert(texture, {width, height, depth, format, channel_count});

  functions_->glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize);

  GLenum target_current = is_3d ? GL_TEXTURE_BINDING_3D : GL_TEXTURE_BINDING_2D;
  GLenum target = is_3d ? GL_TEXTURE_3D : GL_TEXTURE_2D;

  GLint current_tex;
  functions_->glGetIntegerv(target_current, &current_tex);

  functions_->glBindTexture(target, texture);

  if (is_3d) {
    context_->extraFunctions()->glTexImage3D(target, 0, GetInternalFormat(format, channel_count),
                                             width, height, depth, 0, GetPixelFormat(channel_count),
                                             GetPixelType(format), data);
  } else {
    functions_->glTexImage2D(target, 0, GetInternalFormat(format, channel_count),
                             width, height, 0, GetPixelFormat(channel_count),
                             GetPixelType(format), data);
  }

  functions_->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

  functions_->glBindTexture(target, current_tex);

  return texture;
}

void OpenGLRenderer::AttachTextureAsDestination(const QVariant &texture)
{
  PRINT_GL_ERRORS;

  functions_->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
  functions_->glFramebufferTexture2D(GL_FRAMEBUFFER,
                                     GL_COLOR_ATTACHMENT0,
                                     GL_TEXTURE_2D,
                                     texture.value<GLuint>(),
                                     0);
}

void OpenGLRenderer::DetachTextureAsDestination()
{
  functions_->glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void OpenGLRenderer::DestroyNativeTexture(QVariant texture)
{
  GLuint t = texture.value<GLuint>();

  if (t > 0) {
#if defined(OLIVE_ENABLE_CUDA_NVDEC)
    auto resource = cuda_graphics_resources_.find(t);
    if (resource != cuda_graphics_resources_.end()) {
      cudaGraphicsUnregisterResource(resource.value());
      cuda_graphics_resources_.erase(resource);
    }
#endif
    functions_->glDeleteTextures(1, &t);
  }
}

void OpenGLRenderer::DestroyTexture(Texture *texture)
{
  if (texture) {
    const GLuint tid = texture->id().value<GLuint>();
#if defined(OLIVE_ENABLE_CUDA_NVDEC)
    if (ReleaseCudaPoolTexture(tid)) {
      return;
    }
#endif
    if (ReleaseNativeSurfacePoolTexture(tid)) {
      return;
    }
  }
  Renderer::DestroyTexture(texture);
}

#if defined(OLIVE_ENABLE_CUDA_NVDEC)
int OpenGLRenderer::AcquireCudaPoolSlot(int width, int height, PixelFormat format)
{
  QMutexLocker locker(&cuda_pool_mutex_);

  if (width <= 0 || height <= 0 || width > kCudaFramePoolMaxWidth || height > kCudaFramePoolMaxHeight) {
    qWarning() << "[CUDA-GL-Pool] Requested dimensions exceed bounding box:" << width << "x" << height;
    return -1;
  }

  // 1. Exact match pass: find a free slot with identical resolution *and* precision
  for (size_t i = 0; i < cuda_frame_pool_.size(); ++i) {
    auto &slot = cuda_frame_pool_[i];
    if (!slot.in_use && slot.width == width && slot.height == height && slot.format == format
        && slot.gl_tex_id != 0 && slot.cuda_resource != nullptr) {
      if (!MarkCudaSlotBorrowed(static_cast<int>(i))) {
        return -1;
      }
      return static_cast<int>(i);
    }
  }

  // 2. Reuse pass: resize an existing idle slot
  for (size_t i = 0; i < cuda_frame_pool_.size(); ++i) {
    auto &slot = cuda_frame_pool_[i];
    if (!slot.in_use) {
      if (!AllocateOrResizePoolSlot(slot, width, height, format)) {
        return -1;
      }
      if (!MarkCudaSlotBorrowed(static_cast<int>(i))) {
        return -1;
      }
      return static_cast<int>(i);
    }
  }

  // 3. Growth pass: append a new slot if under capacity
  if (static_cast<int>(cuda_frame_pool_.size()) < kCudaFramePoolCapacity) {
    CudaGLPoolSlot new_slot;
    if (AllocateOrResizePoolSlot(new_slot, width, height, format)) {
      cuda_frame_pool_.push_back(new_slot);
      int new_index = static_cast<int>(cuda_frame_pool_.size() - 1);
      if (!MarkCudaSlotBorrowed(new_index)) {
        return -1;
      }
      return new_index;
    }
  }

  if (!cuda_pool_capacity_warned_) {
    cuda_pool_capacity_warned_ = true;
    qWarning() << "[CUDA-GL-Pool] Pool capacity reached (" << cuda_frame_pool_.size() << "slots all in use)";
  }
  return -1;
}

bool OpenGLRenderer::AllocateOrResizePoolSlot(CudaGLPoolSlot &slot, int width, int height, PixelFormat format)
{
  if (slot.cuda_resource) {
    cudaGraphicsUnregisterResource(slot.cuda_resource);
    slot.cuda_resource = nullptr;
  }
  if (slot.gl_tex_id) {
    functions_->glDeleteTextures(1, &slot.gl_tex_id);
    slot.gl_tex_id = 0;
  }

  // Patch 0027: 8-bit SDR streams keep the cheap GL_RGBA8 slot; 10-bit (P010) streams are
  // promoted to GL_RGBA16F so the extra two bits survive instead of being truncated.
  GLint internal_format = GL_RGBA8;
  GLenum upload_type = GL_UNSIGNED_BYTE;
  if (format == PixelFormat::F16 || format == PixelFormat::F32) {
    internal_format = GL_RGBA16F;
    upload_type = GL_HALF_FLOAT;
  }

  functions_->glGenTextures(1, &slot.gl_tex_id);
  functions_->glBindTexture(GL_TEXTURE_2D, slot.gl_tex_id);
  functions_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  functions_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  functions_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  functions_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  functions_->glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, GL_RGBA, upload_type, nullptr);
  functions_->glBindTexture(GL_TEXTURE_2D, 0);

  const cudaError_t reg_err = cudaGraphicsGLRegisterImage(&slot.cuda_resource,
                                                          slot.gl_tex_id,
                                                          GL_TEXTURE_2D,
                                                          cudaGraphicsRegisterFlagsSurfaceLoadStore);
  if (reg_err != cudaSuccess) {
    if (!cuda_pool_registration_failed_) {
      cuda_pool_registration_failed_ = true;
      qWarning() << "[CUDA-GL-Pool] Failed to register texture with CUDA:" << cudaGetErrorString(reg_err)
                 << "- if this is a hybrid-GPU laptop, the OpenGL context may be on the iGPU while"
                 << "CUDA/NVDEC is on the NVIDIA GPU (CUDA-GL interop cannot work across GPUs)."
                 << "Use the GPU session selection to move GL onto the NVIDIA GPU, or pick a"
                 << "different backend in the HAL backend dialog."
                 << "(This will not be repeated for every frame.)";
    }
    if (slot.gl_tex_id) {
      functions_->glDeleteTextures(1, &slot.gl_tex_id);
      slot.gl_tex_id = 0;
    }
    return false;
  }

  cuda_pool_registration_failed_ = false;

  slot.width = width;
  slot.height = height;
  slot.format = format;
  return true;
}

int OpenGLRenderer::MarkCudaSlotBorrowed(int slot_index)
{
  // Caller must hold cuda_pool_mutex_. Returns false if the slot was already checked out,
  // which would indicate pool bookkeeping corruption.
  if (slot_index < 0 || slot_index >= static_cast<int>(cuda_frame_pool_.size())) {
    return false;
  }
  CudaGLPoolSlot &slot = cuda_frame_pool_[slot_index];
  if (slot.in_use) {
    qCritical() << "[CUDA-GL-Pool] Slot" << slot_index << "borrowed while already in use"
                << "- pool bookkeeping corruption";
    return false;
  }
  slot.in_use = true;
  cuda_borrowed_count_++;
  cuda_in_use_count_++;
  // The pool is serving frames again, so the next saturation is worth reporting anew.
  cuda_pool_capacity_warned_ = false;
  return true;
}

int OpenGLRenderer::ReleaseCudaPoolSlotInternal(int slot_index)
{
  // Caller must hold cuda_pool_mutex_. Single choke point for returning a slot to the pool.
  // This is the double-release tripwire: a slot released while already free means two owners
  // believed they held it. Assertions are unusable here (Release builds define NDEBUG), so the
  // detector reports through qCritical and refuses the release rather than corrupting the pool.
  if (slot_index < 0 || slot_index >= static_cast<int>(cuda_frame_pool_.size())) {
    return false;
  }

  CudaGLPoolSlot &slot = cuda_frame_pool_[slot_index];
  if (!slot.in_use) {
    qCritical() << "[CUDA-GL-Pool] DOUBLE RELEASE on slot" << slot_index
                << "(tex" << slot.gl_tex_id << ") - ignoring to preserve pool integrity";
    return false;
  }

  slot.in_use = false;
  cuda_returned_count_++;
  cuda_in_use_count_--;

  if (cuda_borrowed_count_ != cuda_returned_count_ + cuda_in_use_count_) {
    qCritical() << "[CUDA-GL-Pool] Ledger invariant violated: borrowed"
                << cuda_borrowed_count_ << "!= returned" << cuda_returned_count_
                << "+ in_use" << cuda_in_use_count_;
  }

  return true;
}

void OpenGLRenderer::ReleaseCudaPoolSlot(int slot_index)
{
  QMutexLocker locker(&cuda_pool_mutex_);
  ReleaseCudaPoolSlotInternal(slot_index);
}

bool OpenGLRenderer::ReleaseCudaPoolTexture(GLuint tid)
{
  QMutexLocker locker(&cuda_pool_mutex_);

  // Probe semantics: return true only if this texture belongs to our pool. A miss is the
  // ordinary path for every non-pooled texture, so it must stay silent and side-effect free.
  for (size_t i = 0; i < cuda_frame_pool_.size(); ++i) {
    if (cuda_frame_pool_[i].gl_tex_id == tid) {
      ReleaseCudaPoolSlotInternal(static_cast<int>(i));
      return true;
    }
  }
  return false;
}

void OpenGLRenderer::DestroyCudaFramePool()
{
  QMutexLocker locker(&cuda_pool_mutex_);

  // Teardown checkpoint: outstanding borrows here mean textures outlived the pool.
  if (cuda_in_use_count_ != 0) {
    qWarning() << "[CUDA-GL-Pool] Destroying pool with" << cuda_in_use_count_
               << "slot(s) still checked out";
  }

  for (auto &slot : cuda_frame_pool_) {
    if (slot.cuda_resource) {
      cudaGraphicsUnregisterResource(slot.cuda_resource);
      slot.cuda_resource = nullptr;
    }
    if (slot.gl_tex_id) {
      functions_->glDeleteTextures(1, &slot.gl_tex_id);
      slot.gl_tex_id = 0;
    }
    slot.in_use = false;
  }
  cuda_frame_pool_.clear();

  // Pool is gone, so the ledger restarts from zero.
  cuda_borrowed_count_ = 0;
  cuda_returned_count_ = 0;
  cuda_in_use_count_ = 0;
}

TexturePtr OpenGLRenderer::CreateTextureFromCudaFrame(const VideoParams &params, const AVFrame *frame)
{
  if (!frame || frame->format != AV_PIX_FMT_CUDA || !frame->hw_frames_ctx || params.is_3d()) {
    return nullptr;
  }

  // cudaGraphicsGLRegisterImage/Map require the texture's GL context to be current on this thread.
  // Without it the interop calls are undefined behaviour and crash, so fall back to the CPU path.
  QOpenGLContext *current_context = QOpenGLContext::currentContext();
  if (current_context != context_) {
    static bool context_warning_emitted = false;
    if (!context_warning_emitted) {
      context_warning_emitted = true;
      qWarning() << "[CUDA-GL] Renderer context is not current on this thread; falling back to"
                 << "software frame transfer. current:" << current_context << "expected:" << context_;
    }
    return nullptr;
  }

  const auto *frames_context = reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data);
  if (!frames_context || (frames_context->sw_format != AV_PIX_FMT_NV12 && frames_context->sw_format != AV_PIX_FMT_P010)) {
    return nullptr;
  }

  // Patch 0027: 10-bit P010 sources get a GL_RGBA16F slot and the full-precision kernel so
  // the two extra bits are preserved instead of being truncated at the kernel exit.
  const bool is_10bit = (frames_context->sw_format == AV_PIX_FMT_P010);
  const PixelFormat slot_format = is_10bit ? PixelFormat::F16 : PixelFormat::U8;

  const int slot_index = AcquireCudaPoolSlot(frame->width, frame->height, slot_format);
  if (slot_index < 0) {
    return nullptr;
  }

  cudaGraphicsResource_t resource = cuda_frame_pool_[slot_index].cuda_resource;
  const GLuint texture_id = cuda_frame_pool_[slot_index].gl_tex_id;

  cudaError_t error = cudaGraphicsMapResources(1, &resource, 0);
  if (error != cudaSuccess) {
    qWarning() << "[CUDA-GL] Texture map failed:" << cudaGetErrorString(error);
    ReleaseCudaPoolSlot(slot_index);
    return nullptr;
  }

  cudaArray_t mapped_array = nullptr;
  error = cudaGraphicsSubResourceGetMappedArray(&mapped_array, resource, 0, 0);
  cudaSurfaceObject_t surface = 0;
  if (error == cudaSuccess) {
    cudaResourceDesc resource_description{};
    resource_description.resType = cudaResourceTypeArray;
    resource_description.res.array.array = mapped_array;
    error = cudaCreateSurfaceObject(&surface, &resource_description);
  }

  if (error == cudaSuccess) {
    if (is_10bit) {
      error = OliveLaunchP010ToRgba16fSurface(reinterpret_cast<const uint16_t*>(frame->data[0]),
                                              frame->linesize[0],
                                              reinterpret_cast<const uint16_t*>(frame->data[1]),
                                              frame->linesize[1],
                                              surface,
                                              frame->width,
                                              frame->height,
                                              frame->colorspace,
                                              frame->color_range == AVCOL_RANGE_JPEG);
    } else {
      error = OliveLaunchNv12ToRgbaSurface(frame->data[0],
                                           frame->linesize[0],
                                           frame->data[1],
                                           frame->linesize[1],
                                           surface,
                                           frame->width,
                                           frame->height,
                                           frame->colorspace,
                                           frame->color_range == AVCOL_RANGE_JPEG);
    }
  }
  if (error == cudaSuccess) {
    error = cudaDeviceSynchronize();
  }
  if (surface) {
    cudaDestroySurfaceObject(surface);
  }

  const cudaError_t unmap_error = cudaGraphicsUnmapResources(1, &resource, 0);
  if (error == cudaSuccess) {
    error = unmap_error;
  }
  if (error != cudaSuccess) {
    qWarning() << "[CUDA-GL] NV12 texture conversion failed:" << cudaGetErrorString(error);
    ReleaseCudaPoolSlot(slot_index);
    return nullptr;
  }

  // Return Texture with this as renderer so IsDummy() is false and Texture::~Texture() routes to DestroyTexture
  VideoParams texture_params = params;
  if (is_10bit) {
    texture_params.set_format(PixelFormat::F16);
  }
  return std::make_shared<Texture>(this, QVariant::fromValue(texture_id), texture_params);
}
#endif

int OpenGLRenderer::AcquireNativeSurfaceSlot(int width, int height, GLenum target, PixelFormat format,
                                             GLuint* out_texture_id)
{
  QMutexLocker locker(&native_surface_pool_mutex_);
  if (width <= 0 || height <= 0) return -1;

  size_t chosen = 0;
  bool found = false;

  for (size_t i = 0; i < native_surface_pool_.size(); ++i) {
    auto &slot = native_surface_pool_[i];
    if (!slot.in_use && slot.width == width && slot.height == height && slot.format == format) {
      chosen = i;
      found = true;
      break;
    }
  }

  // Patch 0050: internal-format selection mirrors AllocateOrResizePoolSlot (patch 0027):
  // 8-bit YUV lands in the cheap GL_RGBA8 slot; 10-bit (P010) promotes to GL_RGBA16F so
  // the two extra bits survive. A slot allocated for one format is never rebound to another.
  GLint internal_format = GL_RGBA8;
  GLenum upload_type = GL_UNSIGNED_BYTE;
  if (format == PixelFormat::F16 || format == PixelFormat::F32) {
    internal_format = GL_RGBA16F;
    upload_type = GL_HALF_FLOAT;
  }

  if (!found) {
    for (size_t i = 0; i < native_surface_pool_.size(); ++i) {
      auto &slot = native_surface_pool_[i];
      if (!slot.in_use) {
        if (slot.gl_tex_id) {
          functions_->glDeleteTextures(1, &slot.gl_tex_id);
          slot.gl_tex_id = 0;
        }
        functions_->glGenTextures(1, &slot.gl_tex_id);
        functions_->glBindTexture(target, slot.gl_tex_id);
        functions_->glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        functions_->glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        functions_->glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        functions_->glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        functions_->glTexImage2D(target, 0, internal_format, width, height, 0, GL_RGBA, upload_type, nullptr);
        functions_->glBindTexture(target, 0);
        slot.width = width;
        slot.height = height;
        slot.format = format;
        slot.in_use = true;
        chosen = i;
        found = true;
        break;
      }
    }
  }

  if (!found && static_cast<int>(native_surface_pool_.size()) < kNativeSurfacePoolCapacity) {
    NativeSurfacePoolSlot new_slot;
    functions_->glGenTextures(1, &new_slot.gl_tex_id);
    functions_->glBindTexture(target, new_slot.gl_tex_id);
    functions_->glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    functions_->glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    functions_->glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    functions_->glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    functions_->glTexImage2D(target, 0, internal_format, width, height, 0, GL_RGBA, upload_type, nullptr);
    functions_->glBindTexture(target, 0);
    new_slot.width = width;
    new_slot.height = height;
    // F-3 fix (HAL/Phase6-8 review): the newly created slot must record its pixel format,
    // otherwise the match at the top of this function (slot.format == format) can never hit
    // it and every 16-bit borrow would allocate a fresh slot instead of reusing this one.
    new_slot.format = format;
    new_slot.in_use = true;
    native_surface_pool_.push_back(new_slot);
    chosen = native_surface_pool_.size() - 1;
    found = true;
  }

  if (!found) return -1;

  // Emit the texture id while still holding the lock (CR-13: callers must not read the pool vector
  // outside the mutex).
  if (out_texture_id) {
    *out_texture_id = native_surface_pool_[chosen].gl_tex_id;
  }

  // Ledger bookkeeping, aligned with the CUDA pool's double-release guard (CR-12).
  native_borrowed_count_++;
  native_in_use_count_++;

  return static_cast<int>(chosen);
}

bool OpenGLRenderer::ReleaseNativeSurfacePoolTexture(GLuint tid)
{
  QMutexLocker locker(&native_surface_pool_mutex_);

  for (auto &slot : native_surface_pool_) {
    if (slot.gl_tex_id == tid) {
      // Double-release tripwire, mirroring ReleaseCudaPoolSlotInternal (CR-08 / CR-12).
      if (!slot.in_use) {
        qCritical() << "[NativeSurface-Pool] DOUBLE RELEASE on texture" << tid
                    << "- ignoring to preserve pool integrity";
        return false;
      }
      slot.in_use = false;
      native_returned_count_++;
      native_in_use_count_--;
      if (native_borrowed_count_ != native_returned_count_ + native_in_use_count_) {
        qCritical() << "[NativeSurface-Pool] Ledger invariant violated: borrowed"
                    << native_borrowed_count_ << "!= returned" << native_returned_count_
                    << "+ in_use" << native_in_use_count_;
      }
      return true;
    }
  }
  return false;
}

void OpenGLRenderer::DestroyNativeSurfacePool()
{
  QMutexLocker locker(&native_surface_pool_mutex_);

  if (native_in_use_count_ != 0) {
    qWarning() << "[NativeSurface-Pool] Destroying pool with" << native_in_use_count_
               << "slot(s) still checked out";
  }

  for (auto &slot : native_surface_pool_) {
    if (slot.gl_tex_id && context_) {
      functions_->glDeleteTextures(1, &slot.gl_tex_id);
      slot.gl_tex_id = 0;
    }
    slot.in_use = false;
  }
  native_surface_pool_.clear();

  native_borrowed_count_ = 0;
  native_returned_count_ = 0;
  native_in_use_count_ = 0;
}

TexturePtr OpenGLRenderer::CreateTextureFromIOSurface(const VideoParams &params, void* iosurface, int width, int height)
{
#if !defined(__APPLE__)
  Q_UNUSED(params);
  Q_UNUSED(iosurface);
  Q_UNUSED(width);
  Q_UNUSED(height);
  return nullptr;
#else
  if (!iosurface || width <= 0 || height <= 0) return nullptr;

  CGLContextObj cgl_ctx = CGLGetCurrentContext();
  if (!cgl_ctx) return nullptr;

  // IOSurface can only be bound as GL_TEXTURE_RECTANGLE (CGLTexImageIOSurface2D has no 2D target).
  // Bind it to a scratch RECTANGLE texture, then blit into a pool-resident GL_TEXTURE_2D so the
  // result is compatible with Olive's normalized-UV samplers (fixes CR-10: delivering RECTANGLE
  // straight to the node graph would sample the wrong region / black screen).
  GLuint rect_tex = 0;
  glGenTextures(1, &rect_tex);
  glBindTexture(GL_TEXTURE_RECTANGLE, rect_tex);
  CGLError cgl_err = CGLTexImageIOSurface2D(cgl_ctx,
                                           GL_TEXTURE_RECTANGLE,
                                           GL_RGBA,
                                           width,
                                           height,
                                           GL_BGRA,
                                           GL_UNSIGNED_INT_8_8_8_8_REV,
                                           reinterpret_cast<IOSurfaceRef>(iosurface),
                                           0);
  glBindTexture(GL_TEXTURE_RECTANGLE, 0);
  if (cgl_err != kCGLNoError) {
    qWarning() << "[VideoToolbox-Pool] CGLTexImageIOSurface2D failed:" << cgl_err;
    glDeleteTextures(1, &rect_tex);
    return nullptr;
  }

  GLuint target_2d = 0;
  const int slot_index = AcquireNativeSurfaceSlot(width, height, GL_TEXTURE_2D, params.format(), &target_2d);
  if (slot_index < 0) {
    glDeleteTextures(1, &rect_tex);
    return nullptr;
  }

  auto* xf = context_->extraFunctions();
  if (xf) {
    GLuint fbo[2] = {0, 0};
    xf->glGenFramebuffers(2, fbo);
    xf->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[0]);
    xf->glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, rect_tex, 0);
    xf->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[1]);
    xf->glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target_2d, 0);
    xf->glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    xf->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    xf->glDeleteFramebuffers(2, fbo);
  } else {
    qWarning() << "[VideoToolbox-Pool] extraFunctions() unavailable; cannot blit RECTANGLE to 2D";
    ReleaseNativeSurfacePoolTexture(target_2d);
    glDeleteTextures(1, &rect_tex);
    return nullptr;
  }

  glDeleteTextures(1, &rect_tex);
  return std::make_shared<Texture>(this, QVariant::fromValue(target_2d), params);
#endif
}

// Patch 0050 (item #5), corrected by the HAL/Phase6-8 review (F-1): fragment shader for
// the two-plane VA-API import composite. Mirrors :/shaders/yuv2rgb.frag, but takes a
// single packed-UV texture (GL_RG8/GL_RG16 from a GR88/GR32/GR1616 EGLImage) instead of
// separate U and V planes.
//
// The luma/chroma textures arrive as UNORM (GL_R8/R16, GL_RG8/RG16), so the GL hardware
// has already normalised *every* depth to [0,1]. A 10-bit sample stored MSB-aligned in a
// 16-bit container (val16 = val10 << 6) reads back as s = val10*64/65535 ~= val10/1023,
// i.e. the normalised 10-bit domain. There is therefore NO integer-domain rescale: the
// pre-fix `yuv *= 65535/64` applied the domain transform a second time and produced
// ~1024x over-bright output (the review's F-1). Limited range is expanded per bit depth
// so the 10-bit black level sits at 64/1023 and white at 940/1023, matching the CPU
// YUV->RGB path and Tier 1 (CUDA).
static const char* kVaapiYuvToRgbFrag = R"(

uniform sampler2D ove_y_tex;
uniform sampler2D ove_uv_tex;

uniform int bits_per_pixel;
uniform bool full_range;

uniform float yuv_crv;
uniform float yuv_cgu;
uniform float yuv_cgv;
uniform float yuv_cbu;

in vec2 ove_texcoord;
out vec4 frag_color;

void main()
{
  vec3 yuv;
  yuv.r = texture(ove_y_tex, ove_texcoord).r;
  vec2 uv = texture(ove_uv_tex, ove_texcoord).rg;
  yuv.g = uv.r;
  yuv.b = uv.g;

  // The UNORM samples are already normalised to [0,1]; only the limited-range expansion
  // differs per bit depth. Chroma is recentred on the mid code value of the depth.
  float luma_black;
  float luma_scale;
  if (bits_per_pixel == 8) {
    luma_black = 16.0/255.0;    // limited-range black (16 of 255)
    luma_scale = 255.0/219.0;   // 16..235 -> 0..1
    yuv.gb -= (128.0/255.0);
  } else if (bits_per_pixel == 10) {
    luma_black = 64.0/1023.0;   // limited-range black (64 of 1023)
    luma_scale = 1023.0/876.0;  // 64..940 -> 0..1
    yuv.gb -= (512.0/1023.0);
  } else if (bits_per_pixel == 12) {
    luma_black = 256.0/4095.0;  // limited-range black (256 of 4095)
    luma_scale = 4095.0/3504.0; // 256..3760 -> 0..1
    yuv.gb -= (2048.0/4095.0);
  } else {
    luma_black = 0.0;
    luma_scale = 1.0;
  }

  // Limited-range luma -> full range (undone below when the source is full range).
  yuv.r = (yuv.r - luma_black) * luma_scale;

  vec4 rgba;
  rgba.r = yuv.r + yuv_crv * yuv.b;
  rgba.g = yuv.r - yuv_cgu * yuv.g - yuv_cgv * yuv.b;
  rgba.b = yuv.r + yuv_cbu * yuv.g;

  if (full_range) {
    rgba.rgb = rgba.rgb / luma_scale + luma_black;
  }

  rgba.a = 1.0;
  frag_color = rgba;
}

)";

TexturePtr OpenGLRenderer::CompositeTextureFromVaapiPlanes(const VideoParams &params,
                                                           void* egl_image_y, void* egl_image_uv,
                                                           int width, int height,
                                                           int bits_per_pixel, bool full_range,
                                                           float yuv_crv, float yuv_cbu,
                                                           float yuv_cgu, float yuv_cgv)
{
#if !defined(__linux__)
  Q_UNUSED(params);
  Q_UNUSED(egl_image_y);
  Q_UNUSED(egl_image_uv);
  Q_UNUSED(width);
  Q_UNUSED(height);
  Q_UNUSED(bits_per_pixel);
  Q_UNUSED(full_range);
  Q_UNUSED(yuv_crv);
  Q_UNUSED(yuv_cbu);
  Q_UNUSED(yuv_cgu);
  Q_UNUSED(yuv_cgv);
  return nullptr;
#else
  if (!egl_image_y || !egl_image_uv || width <= 0 || height <= 0) return nullptr;

  // Resolve pointers per-call (CR-11: a static cache would freeze nullptrs if the first
  // call had no current GL context).
  QOpenGLContext* ctx = QOpenGLContext::currentContext();
  if (!ctx) {
    qWarning() << "[VAAPI-HAL] No current OpenGL context while compositing VA-API planes";
    return nullptr;
  }
  typedef void (*PFNGLEGLIMAGETARGETTEXTURE2DOESPROC_)(unsigned int, void*);
  auto pfn_glEGLImageTargetTexture2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC_>(
      ctx->getProcAddress("glEGLImageTargetTexture2DOES"));
  if (!pfn_glEGLImageTargetTexture2DOES) {
    qWarning() << "[VAAPI-HAL] glEGLImageTargetTexture2DOES not available; cannot composite planes";
    return nullptr;
  }

  // Scratch textures bound to the two EGLImages. Created per call and deleted below -
  // they are views over driver-owned memory, never stored.
  GLuint y_tex = 0, uv_tex = 0;
  functions_->glGenTextures(1, &y_tex);
  functions_->glBindTexture(GL_TEXTURE_2D, y_tex);
  functions_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  functions_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  functions_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  functions_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  pfn_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, egl_image_y);

  functions_->glGenTextures(1, &uv_tex);
  functions_->glBindTexture(GL_TEXTURE_2D, uv_tex);
  functions_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  functions_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  functions_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  functions_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  pfn_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, egl_image_uv);
  functions_->glBindTexture(GL_TEXTURE_2D, 0);
  if (GLenum err = functions_->glGetError(); err != GL_NO_ERROR) {
    qWarning() << "[VAAPI-HAL] EGLImage plane binding failed, GL error 0x" << Qt::hex << err << Qt::dec;
    functions_->glDeleteTextures(1, &y_tex);
    functions_->glDeleteTextures(1, &uv_tex);
    return nullptr;
  }

  // Compile the composite shader once per renderer process.
  static QVariant vaapi_yuv_shader;
  if (vaapi_yuv_shader.isNull()) {
    vaapi_yuv_shader = CreateNativeShader(ShaderCode(QString::fromUtf8(kVaapiYuvToRgbFrag)));
    if (vaapi_yuv_shader.isNull()) {
      qCritical() << "[VAAPI-HAL] Failed to compile VA-API two-plane YUV->RGB shader";
      functions_->glDeleteTextures(1, &y_tex);
      functions_->glDeleteTextures(1, &uv_tex);
      return nullptr;
    }
  }

  // Destination: pooled slot, GL_RGBA16F for 10-bit sources (patch 0050/0027 symmetry),
  // GL_RGBA8 for 8-bit. The slot outlives the DMA-BUF fds, which the pipeline may close
  // as soon as this function returns.
  VideoParams dst_params = params;
  if (bits_per_pixel > 8) {
    dst_params.set_format(PixelFormat::F16);
  }
  dst_params.set_channel_count(VideoParams::kRGBAChannelCount);

  TexturePtr dst = CreateTexture(dst_params);
  if (!dst) {
    qWarning() << "[VAAPI-HAL] Failed to allocate composite destination texture";
    functions_->glDeleteTextures(1, &y_tex);
    functions_->glDeleteTextures(1, &uv_tex);
    return nullptr;
  }

  // Half-size chroma texture params (420 subsampling, matching the GR*/P010 layout).
  VideoParams uv_params = dst_params;
  uv_params.set_width(width / 2);
  uv_params.set_height(height / 2);
  uv_params.set_channel_count(2);

  VideoParams y_params = dst_params;
  y_params.set_channel_count(1);
  TexturePtr y_view = std::make_shared<Texture>(this, QVariant::fromValue(y_tex), y_params);
  TexturePtr uv_view = std::make_shared<Texture>(this, QVariant::fromValue(uv_tex), uv_params);

  ShaderJob job;
  job.Insert(QStringLiteral("ove_y_tex"), NodeValue(NodeValue::kTexture, QVariant::fromValue(y_view)));
  job.Insert(QStringLiteral("ove_uv_tex"), NodeValue(NodeValue::kTexture, QVariant::fromValue(uv_view)));
  job.Insert(QStringLiteral("bits_per_pixel"), NodeValue(NodeValue::kInt, bits_per_pixel));
  job.Insert(QStringLiteral("full_range"), NodeValue(NodeValue::kBoolean, full_range));
  job.Insert(QStringLiteral("yuv_crv"), NodeValue(NodeValue::kFloat, yuv_crv));
  job.Insert(QStringLiteral("yuv_cbu"), NodeValue(NodeValue::kFloat, yuv_cbu));
  job.Insert(QStringLiteral("yuv_cgu"), NodeValue(NodeValue::kFloat, yuv_cgu));
  job.Insert(QStringLiteral("yuv_cgv"), NodeValue(NodeValue::kFloat, yuv_cgv));

  // Same default matrix the node pipeline passes to every other Blit call.
  QMatrix4x4 identity;
  job.Insert(QStringLiteral("ove_mvpmat"), NodeValue(NodeValue::kMatrix, identity));

  BlitToTexture(vaapi_yuv_shader, job, dst.get(), false);

  functions_->glDeleteTextures(1, &y_tex);
  functions_->glDeleteTextures(1, &uv_tex);

  return dst;
#endif
}

QVariant OpenGLRenderer::CreateNativeShader(ShaderCode code)
{  GL_PREAMBLE;

  PRINT_GL_ERRORS;

  GLuint vert = CompileShader(GL_VERTEX_SHADER, code.vert_code());
  GLuint frag = CompileShader(GL_FRAGMENT_SHADER, code.frag_code());

  GLuint program = 0;

  if (frag && vert) {
    program = functions_->glCreateProgram();
    functions_->glAttachShader(program, frag);
    functions_->glAttachShader(program, vert);
    functions_->glLinkProgram(program);

    GLint success;
    functions_->glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
      qWarning() << "Failed to link OpenGL shader program";
      functions_->glDeleteProgram(program);
      program = 0;
    }
  }

  functions_->glDeleteShader(frag);
  functions_->glDeleteShader(vert);

  return program;
}

void OpenGLRenderer::DestroyNativeShader(QVariant shader)
{
  GL_PREAMBLE;

  GLuint program = shader.value<GLuint>();
  functions_->glDeleteProgram(program);
}

void OpenGLRenderer::UploadToTexture(const QVariant &handle, const VideoParams &p, const void *data, int linesize)
{
  GL_PREAMBLE;

  GLuint t = handle.value<GLuint>();

  bool is_3d = p.is_3d();

  GLenum tex_type = !is_3d ? GL_TEXTURE_2D : GL_TEXTURE_3D;
  GLenum tex_binding = !is_3d ? GL_TEXTURE_BINDING_2D : GL_TEXTURE_BINDING_3D;

  // Store currently bound texture so it can be restored later
  GLint current_tex;
  functions_->glGetIntegerv(tex_binding, &current_tex);

  functions_->glBindTexture(tex_type, t);

  functions_->glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize);

  // Phase 6 (patch 0026): stream the frame through the async PBO ring when possible.
  // On failure we transparently fall back to the original client-memory upload.
  bool uploaded = false;

  if (!is_3d) {
    uploaded = UploadToTextureViaPbo(p, data, linesize);
  }

  if (!uploaded) {
    PRINT_GL_ERRORS;

    if (!is_3d) {
      functions_->glTexSubImage2D(tex_type, 0, 0, 0,
                                  p.effective_width(), p.effective_height(),
                                  GetPixelFormat(p.channel_count()), GetPixelType(p.format()),
                                  data);
    } else {
      context_->extraFunctions()->glTexSubImage3D(tex_type, 0, 0, 0, 0,
                                                  p.effective_width(), p.effective_height(), p.effective_depth(),
                                                  GetPixelFormat(p.channel_count()), GetPixelType(p.format()),
                                                  data);
    }
  }

  functions_->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

  functions_->glBindTexture(tex_type, current_tex);
}

bool OpenGLRenderer::UploadToTextureViaPbo(const VideoParams &params, const void *data, int linesize)
{
  if (!data || !context_) {
    return false;
  }

  QOpenGLExtraFunctions* xf = context_->extraFunctions();
  if (!xf) {
    return false;
  }

  const int bytes_per_pixel = params.GetBytesPerPixel();
  const int width = params.effective_width();
  const int height = params.effective_height();

  if (bytes_per_pixel <= 0 || width <= 0 || height <= 0) {
    return false;
  }

  const GLsizeiptr payload = static_cast<GLsizeiptr>(width) * bytes_per_pixel * height;
  const int source_row_pixels = (linesize > 0) ? linesize : width;

  // Ping-pong across the ring: we fill the next PBO while the GPU may still be
  // draining the slot used by the previous frame.
  const int slot = upload_pbo_index_;
  upload_pbo_index_ = (upload_pbo_index_ + 1) % kUploadPboRingSize;

  if (upload_pbos_[slot] == 0) {
    xf->glGenBuffers(1, &upload_pbos_[slot]);
    if (upload_pbos_[slot] == 0) {
      return false;
    }
  }

  bool profiled = qEnvironmentVariableIsSet("OLIVE_PBO_PROFILE");
  QElapsedTimer timer;
  if (profiled) {
    timer.start();
  }

  xf->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, upload_pbos_[slot]);

  // Orphan the store every frame: a fresh backing allocation means the driver never
  // has to stall waiting for the previous frame's DMA before handing us a pointer.
  xf->glBufferData(GL_PIXEL_UNPACK_BUFFER, payload, nullptr, GL_STREAM_DRAW);

  void* mapped = xf->glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, payload,
                                      GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
  if (!mapped) {
    xf->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    return false;
  }

  const size_t dst_stride = static_cast<size_t>(width) * bytes_per_pixel;
  if (source_row_pixels == width) {
    std::memcpy(mapped, data, static_cast<size_t>(payload));
  } else {
    const size_t src_stride = static_cast<size_t>(source_row_pixels) * bytes_per_pixel;
    const unsigned char* src = static_cast<const unsigned char*>(data);
    unsigned char* dst = static_cast<unsigned char*>(mapped);
    for (int y = 0; y < height; ++y) {
      std::memcpy(dst + static_cast<size_t>(y) * dst_stride,
                  src + static_cast<size_t>(y) * src_stride,
                  dst_stride);
    }
  }

  xf->glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);

  // The PBO holds tightly packed rows, so the effective row length is the frame width
  // and no alignment padding applies (unlike the caller's possibly padded source).
  GLint prev_alignment = 4;
  functions_->glGetIntegerv(GL_UNPACK_ALIGNMENT, &prev_alignment);
  functions_->glPixelStorei(GL_UNPACK_ROW_LENGTH, width);
  functions_->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

  {
    PRINT_GL_ERRORS;
    functions_->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                                GetPixelFormat(params.channel_count()), GetPixelType(params.format()),
                                nullptr);
  }

  functions_->glPixelStorei(GL_UNPACK_ALIGNMENT, prev_alignment);
  functions_->glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize);

  xf->glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

  if (profiled) {
    static qint64 total_ns = 0;
    static int frame_count = 0;
    total_ns += timer.nsecsElapsed();
    ++frame_count;
    if (frame_count % 240 == 0) {
      qInfo() << "[PBO-Async] streaming upload main-thread cost avg"
              << static_cast<double>(total_ns) / 1000.0 / frame_count << "us over"
              << frame_count << "frames";
    }
  }

  return true;
}

void OpenGLRenderer::DestroyUploadPboRing()
{
  if (!functions_) {
    return;
  }

  for (int i = 0; i < kUploadPboRingSize; ++i) {
    if (upload_pbos_[i] != 0) {
      functions_->glDeleteBuffers(1, &upload_pbos_[i]);
      upload_pbos_[i] = 0;
    }
  }

  upload_pbo_index_ = 0;
}

void OpenGLRenderer::DownloadFromTexture(const QVariant &id, const VideoParams &p, void *data, int linesize)
{
  GL_PREAMBLE;

  GLint current_tex;
  functions_->glGetIntegerv(GL_TEXTURE_BINDING_2D, &current_tex);

  AttachTextureAsDestination(id);

  functions_->glPixelStorei(GL_PACK_ROW_LENGTH, linesize);

  {
    PRINT_GL_ERRORS;
    functions_->glReadPixels(0,
                             0,
                             p.effective_width(),
                             p.effective_height(),
                             GetPixelFormat(p.channel_count()),
                             GetPixelType(p.format()),
                             data);
  }

  functions_->glPixelStorei(GL_PACK_ROW_LENGTH, 0);

  DetachTextureAsDestination();

  functions_->glBindTexture(GL_TEXTURE_2D, current_tex);
}

void OpenGLRenderer::Flush()
{
  GL_PREAMBLE;

  if (OLIVE_CONFIG("UseGLFinish").toBool()) {
    functions_->glFinish();
  } else {
    functions_->glFlush();
  }
}

Color OpenGLRenderer::GetPixelFromTexture(Texture *texture, const QPointF &pt)
{
  AttachTextureAsDestination(texture->id());

  QByteArray data(VideoParams::GetBytesPerPixel(texture->format(), texture->channel_count()), Qt::Uninitialized);

  functions_->glReadPixels(pt.x(), pt.y(), 1, 1, GetPixelFormat(texture->channel_count()), GetPixelType(texture->format()), data.data());

  Color c = Color::fromData(data.data(), texture->format(), texture->channel_count());

  if (texture->channel_count() == VideoParams::kRGBChannelCount) {
    // No alpha channel, set to 1.0
    c.set_alpha(1.0);
  }

  DetachTextureAsDestination();

  return c;
}

struct TextureToBind {
  TexturePtr texture;
  Texture::Interpolation interpolation;
};

void OpenGLRenderer::Blit(QVariant s, ShaderJob job, Texture *destination, VideoParams destination_params, bool clear_destination)
{
  GL_PREAMBLE;

  // If this node is iterative, we'll pick up which input here
  QMap<QString, GLuint> texture_index_map;
  QVector<TextureToBind> textures_to_bind;

  GLuint shader = s.value<GLuint>();

  functions_->glUseProgram(shader);

  for (auto it=job.GetValues().constBegin(); it!=job.GetValues().constEnd(); it++) {
    // See if the shader has takes this parameter as an input
    GLint variable_location = functions_->glGetUniformLocation(shader, it.key().toUtf8().constData());

    if (variable_location == -1) {
      continue;
    }

    // This variable is used in the shader, let's set it
    const NodeValue& value = it.value();

    // Arrays are not currently supported in this system
    if (value.array()) {
      continue;
    }

    switch (value.type()) {
    case NodeValue::kInt:
      // kInt technically specifies a LongLong, but OpenGL doesn't support those. This may lead to
      // over/underflows if the number is large enough, but the likelihood of that is quite low.
      functions_->glUniform1i(variable_location, value.toInt());
      break;
    case NodeValue::kFloat:
      // kFloat technically specifies a double but as above, OpenGL doesn't support those.
      functions_->glUniform1f(variable_location, value.toDouble());
      break;
    case NodeValue::kVec2:
    {
      QVector2D v = value.toVec2();
      functions_->glUniform2fv(variable_location, 1, reinterpret_cast<const GLfloat*>(&v));
      break;
    }
    case NodeValue::kVec3:
    {
      QVector3D v = value.toVec3();
      functions_->glUniform3fv(variable_location, 1, reinterpret_cast<const GLfloat*>(&v));
      break;
    }
    case NodeValue::kVec4:
    {
      QVector4D v = value.toVec4();
      functions_->glUniform4fv(variable_location, 1, reinterpret_cast<const GLfloat*>(&v));
      break;
    }
    case NodeValue::kMatrix:
      functions_->glUniformMatrix4fv(variable_location, 1, false, value.toMatrix().constData());
      break;
    case NodeValue::kCombo:
      functions_->glUniform1i(variable_location, value.toInt());
      break;
    case NodeValue::kColor:
    {
      Color color = value.toColor();
      functions_->glUniform4f(variable_location, color.red(), color.green(), color.blue(), color.alpha());
      break;
    }
    case NodeValue::kBoolean:
      functions_->glUniform1i(variable_location, value.toBool());
      break;
    case NodeValue::kTexture:
    {
      TexturePtr texture = value.toTexture();

      // Set value to bound texture
      functions_->glUniform1i(variable_location, textures_to_bind.size());

      texture_index_map.insert(it.key(), textures_to_bind.size());

      textures_to_bind.append({texture, job.GetInterpolation(it.key())});

      // Set enable flag if shader wants it
      GLuint tex_id = texture ? texture->id().value<GLuint>() : 0;
      int enable_param_location = functions_->glGetUniformLocation(shader, QStringLiteral("%1_enabled").arg(it.key()).toUtf8().constData());
      if (enable_param_location > -1) {
        functions_->glUniform1i(enable_param_location, tex_id > 0);
      }
      break;
    }
    case NodeValue::kSamples:
    case NodeValue::kText:
    case NodeValue::kRational:
    case NodeValue::kFont:
    case NodeValue::kFile:
    case NodeValue::kVideoParams:
    case NodeValue::kAudioParams:
    case NodeValue::kSubtitleParams:
    case NodeValue::kBezier:
    case NodeValue::kBinary:
    case NodeValue::kNone:
    case NodeValue::kDataTypeCount:
      break;
    }
  }

  // Bind all textures
  for (int i=0; i<textures_to_bind.size(); i++) {
    const TextureToBind& t = textures_to_bind.at(i);
    TexturePtr texture = t.texture;

    GLuint tex_id = texture ? texture->id().value<GLuint>() : 0;

    functions_->glActiveTexture(GL_TEXTURE0 + i);

    GLenum target = (texture && texture->params().is_3d()) ? GL_TEXTURE_3D : GL_TEXTURE_2D;
    functions_->glBindTexture(target, tex_id);

    if (tex_id) {
      PrepareInputTexture(target, t.interpolation);

      if (texture->channel_count() == 1 && destination_params.channel_count() != 1) {
        // Interpret this texture as a grayscale texture
        functions_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
        functions_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
        functions_->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
      }
    }
  }

  // Ensure matrix is set, at least to identity
  GLint mvpmat_location = functions_->glGetUniformLocation(shader, "ove_mvpmat");
  if (mvpmat_location > -1) {
    functions_->glUniformMatrix4fv(mvpmat_location, 1, false, job.Get(QStringLiteral("ove_mvpmat")).toMatrix().constData());
  }

  // Set the viewport to the "physical" resolution of the destination
  functions_->glViewport(0, 0,
                         destination_params.effective_width(),
                         destination_params.effective_height());

  // Bind vertex array object
  QOpenGLVertexArrayObject vao_;
  vao_.create();
  vao_.bind();

  // Set buffers
  QOpenGLBuffer vert_vbo_;
  vert_vbo_.create();
  vert_vbo_.bind();
  // If the job has vertex coordinate overrides use them instead of the defaults.
  if (!job.GetVertexCoordinates().isEmpty()) {
    Q_ASSERT(job.GetVertexCoordinates().size() == 18);
    vert_vbo_.allocate(job.GetVertexCoordinates().constData(), job.GetVertexCoordinates().size() * sizeof(float));
  } else {
    vert_vbo_.allocate(blit_vertices.constData(), blit_vertices.size() * sizeof(GLfloat));
  }
  vert_vbo_.release();

  QOpenGLBuffer frag_vbo_;
  frag_vbo_.create();
  frag_vbo_.bind();
  frag_vbo_.allocate(blit_texcoords.constData(), blit_texcoords.size() * sizeof(GLfloat));
  frag_vbo_.release();

  GLint vertex_location = functions_->glGetAttribLocation(shader, "a_position");
  if (vertex_location != -1) {
    vert_vbo_.bind();
    functions_->glEnableVertexAttribArray(vertex_location);
    functions_->glVertexAttribPointer(vertex_location, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    vert_vbo_.release();
  }

  GLint tex_location = functions_->glGetAttribLocation(shader, "a_texcoord");
  if (tex_location != -1) {
    frag_vbo_.bind();
    functions_->glEnableVertexAttribArray(tex_location);
    functions_->glVertexAttribPointer(tex_location, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    frag_vbo_.release();
  }

  // Some shaders optimize through multiple iterations which requires ping-ponging textures
  // - If there are only two iterations, we can just create one backend texture and then the
  //   destination can be the second
  // - If there are more than two iterations, we need to ping pong back and forth between two
  //   textures. We can still use the destination as the last iteration, but we'll need textures
  //   for the iterative process.
  int real_iteration_count;
  if (job.GetIterationCount() > 1 && !job.GetIterativeInput().isEmpty()) {
    real_iteration_count = job.GetIterationCount();
  } else {
    real_iteration_count = 1;
  }

  TexturePtr output_tex, input_tex;
  if (real_iteration_count > 1) {
    // Create one texture to bounce off
    output_tex = CreateTexture(destination_params);

    if (real_iteration_count > 2) {
      // Create a second texture bounce off
      input_tex = CreateTexture(destination_params);
    }
  }

  GLint iteration_location = functions_->glGetUniformLocation(shader, "ove_iteration");
  for (int iteration=0; iteration<real_iteration_count; iteration++) {
    // Set iteration number
    if (iteration_location > -1) {
      functions_->glUniform1i(iteration_location, iteration);
    }

    if (iteration > 0) {
      // If this is not the first iteration, replace the iterative texture with the one we
      // drew into in the previous iteration (which was output_tex, now held in input_tex after swap)
      const QString &iterative_input = job.GetIterativeInput();
      functions_->glActiveTexture(GL_TEXTURE0 + texture_index_map.value(iterative_input));
      functions_->glBindTexture(GL_TEXTURE_2D, input_tex->id().value<GLuint>());

      // At this time, we only support iterating 2D textures
      PrepareInputTexture(GL_TEXTURE_2D, job.GetInterpolation(iterative_input));
    }

    // Replace iterative input
    if (iteration == real_iteration_count-1) {
      // This is the last iteration, draw to the destination
      if (destination) {
        // If we have a destination texture, draw to it
        AttachTextureAsDestination(destination->id());
      } else if (iteration > 0) {
        // Otherwise, if we were iterating before, detach texture now
        DetachTextureAsDestination();
      }

      // Clear the destination if the caller requested it
      if (clear_destination) {
        ClearDestinationInternal();
      }
    } else {
      // Always draw to output_tex, which gets swapped with input_tex every iteration
      AttachTextureAsDestination(output_tex->id());
    }

    // Blit this texture through this shader
    {
      PRINT_GL_ERRORS;
      functions_->glDrawArrays(GL_TRIANGLES, 0, blit_vertices.size() / 3);
    }

    // Swap so that the next iteration, the texture we drew now will be the input texture next
    std::swap(output_tex, input_tex);
  }

  if (destination) {
    // Reset framebuffer to default if we were drawing to a texture
    DetachTextureAsDestination();
  }

  // Release any textures we bound before
  for (int i=textures_to_bind.size()-1; i>=0; i--) {
    TexturePtr texture = textures_to_bind.at(i).texture;
    GLenum target = (texture && texture->params().is_3d()) ? GL_TEXTURE_3D : GL_TEXTURE_2D;
    functions_->glActiveTexture(GL_TEXTURE0 + i);
    functions_->glBindTexture(target, 0);
  }

  // Release shader
  functions_->glUseProgram(0);

  // Release vertex array object
  frag_vbo_.destroy();
  vert_vbo_.destroy();
  vao_.release();
  vao_.destroy();
}

GLint OpenGLRenderer::GetInternalFormat(PixelFormat format, int channel_layout)
{
  switch (format) {
  case PixelFormat::U8:
    switch (channel_layout) {
    case 1:
      return GL_R8;
    case 2:
      return GL_RG8;
    case 3:
      return GL_RGB8;
    case 4:
      return GL_RGBA8;
    }
    break;
  case PixelFormat::U16:
    switch (channel_layout) {
    case 1:
      return GL_R16;
    case 2:
      return GL_RG16;
    case 3:
      return GL_RGB16;
    case 4:
      return GL_RGBA16;
    }
    break;
  case PixelFormat::F16:
    switch (channel_layout) {
    case 1:
      return GL_R16F;
    case 2:
      return GL_RG16F;
    case 3:
      return GL_RGB16F;
    case 4:
      return GL_RGBA16F;
    }
    break;
  case PixelFormat::F32:
    switch (channel_layout) {
    case 1:
      return GL_R32F;
    case 2:
      return GL_RG32F;
    case 3:
      return GL_RGB32F;
    case 4:
      return GL_RGBA32F;
    }
    break;
  case PixelFormat::INVALID:
  case PixelFormat::COUNT:
    break;
  }

  return GL_INVALID_VALUE;
}

GLenum OpenGLRenderer::GetPixelType(PixelFormat format)
{
  switch (format) {
  case PixelFormat::U8:
    return GL_UNSIGNED_BYTE;
  case PixelFormat::U16:
    return GL_UNSIGNED_SHORT;
  case PixelFormat::F16:
    return GL_HALF_FLOAT;
  case PixelFormat::F32:
    return GL_FLOAT;

  case PixelFormat::INVALID:
  case PixelFormat::COUNT:
    break;
  }

  return GL_INVALID_VALUE;
}

GLenum OpenGLRenderer::GetPixelFormat(int channel_count)
{
  switch (channel_count) {
  case 1:
    return GL_RED;
  case 3:
    return GL_RGB;
  case 4:
    return GL_RGBA;
  default:
    return GL_INVALID_VALUE;
  }
}

void OpenGLRenderer::PrepareInputTexture(GLenum target, Texture::Interpolation interp)
{
  switch (interp) {
  case Texture::kNearest:
    functions_->glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    functions_->glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    break;
  case Texture::kLinear:
    functions_->glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    functions_->glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    break;
  case Texture::kMipmappedLinear:
    functions_->glGenerateMipmap(target);
    functions_->glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    functions_->glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    break;
  }

  functions_->glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  functions_->glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  if (target == GL_TEXTURE_3D) {
    functions_->glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
  }
}

void OpenGLRenderer::ClearDestinationInternal(double r, double g, double b, double a)
{
  functions_->glClearColor(r, g, b, a);
  functions_->glClear(GL_COLOR_BUFFER_BIT);
}

GLuint OpenGLRenderer::CompileShader(GLenum type, const QString &code)
{
  static const QString shader_preamble =
      // Use appropriate GL 3.2 shader header
      QStringLiteral("#version 150\n\n"
                     "precision highp float;\n\n");

  QString complete_code;

  if (!code.startsWith(QStringLiteral("#version"))) {
    complete_code = shader_preamble;
  }

  if (code.isEmpty()) {
    // Use default code
    if (type == GL_FRAGMENT_SHADER) {
      complete_code.append(FileFunctions::ReadFileAsString(QStringLiteral(":/shaders/default.frag")));
    } else if (type == GL_VERTEX_SHADER) {
      complete_code.append(FileFunctions::ReadFileAsString(QStringLiteral(":/shaders/default.vert")));
    }
  } else {
    complete_code.append(code);
  }

  QByteArray code_utf8 = complete_code.toUtf8();
  const char *code_cstr = code_utf8.constData();

  GLuint shader = functions_->glCreateShader(type);
  functions_->glShaderSource(shader, 1, &code_cstr, nullptr);
  functions_->glCompileShader(shader);

  GLint success;
  functions_->glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
  if (!success) {
    qWarning() << "Failed to compile OpenGL shader";

    QByteArray error_log(10240, Qt::Uninitialized);
    functions_->glGetShaderInfoLog(shader, error_log.size(), nullptr, error_log.data());
    std::cout << error_log.constData() << std::endl << code_cstr << std::endl;

    functions_->glDeleteShader(shader);
    shader = 0;
  }

  return shader;
}

}
