#ifndef VIDEO_HARDWARE_PIPELINE_H
#define VIDEO_HARDWARE_PIPELINE_H

#include <cstdint>
#include <memory>
#include <string>

#include "render/texture.h"

struct AVCodecContext;
struct AVFrame;
struct AVBufferRef;

namespace olive {

class Renderer;

enum class HardwareBackendType {
  CudaNvdec,          // Tier 1: Dedicated NVDEC + CUDA resident path (RTX/GTX)
  VaapiDmaBuf,        // Tier 2: Linux AMD/Intel native lightweight DMA-BUF
  D3D11Interop,       // Tier 2: Windows AMD/Intel native D3D11 shared texture
  VideoToolboxInterop,// Tier 2: macOS AMD/Intel/Apple Silicon native VideoToolbox + IOSurface
  SoftwareFallback,   // Tier 3: Olive native reinforced CPU decode + texture upload (zero external dependency)
  LibPlaceboFallback  // Optional / Future plugin: libplacebo HDR tonemapper placeholder
};

/**
 * @brief Result of processing one hardware frame.
 *
 * Carries an owning TexturePtr rather than a bare GL name on purpose: the pool slot behind a
 * texture is returned exactly once, by Texture::~Texture() -> Renderer::DestroyTexture. If the
 * pipeline handed out a naked `uint32_t` and the caller built its own Texture from it, two
 * Texture objects would own the same slot and release it twice. Transferring the object keeps
 * ownership unambiguous.
 */
struct HardwareDecodedFrame {
  TexturePtr texture;        // Owning handle to the resulting OpenGL texture
  int64_t pts{0};            // Presentation timestamp
  int width{0};              // Active video frame width
  int height{0};             // Active video frame height
  int coded_width{0};        // HW buffer padded width
  int coded_height{0};       // HW buffer padded height
  int pool_slot_index{-1};   // Internal frame pool slot index (-1 if non-pooled)
};

class IVideoHardwarePipeline {
public:
  virtual ~IVideoHardwarePipeline() = default;

  /**
   * @brief Probe format, hardware context, and allocate necessary hardware resources.
   * @param codec_ctx FFmpeg AVCodecContext containing stream properties.
   * @param max_w Bounding box width.
   * @param max_h Bounding box height.
   * @return true if hardware pipeline successfully took over the stream, false otherwise.
   */
  virtual bool ProbeAndInitialize(AVCodecContext* codec_ctx, int max_w, int max_h) = 0;

  /**
   * @brief Process an incoming hardware AVFrame into an owning OpenGL texture (< 1ms target).
   *
   * The caller supplies VideoParams because only the decoder holds the stream context needed
   * to derive the pixel aspect ratio; the pipeline supplies the texture itself.
   *
   * @param hw_frame Hardware AVFrame produced by avcodec_receive_frame.
   * @param params   Output texture parameters (dimensions, pixel aspect ratio, divider, format).
   * @param out_frame Populated with the owning TexturePtr and frame metadata on success.
   * @return true if mapping succeeded, false otherwise.
   */
  virtual bool ProcessHardwareFrame(AVFrame* hw_frame, const VideoParams& params, HardwareDecodedFrame& out_frame) = 0;

  /**
   * @brief Tear down hardware pipeline, unregister OpenGL interop resources and release quota.
   */
  virtual void Close() = 0;

  /**
   * @brief Return the active hardware backend type.
   */
  virtual HardwareBackendType GetType() const = 0;

  /**
   * @brief Return a human-readable name for telemetry and logging.
   */
  virtual const char* GetBackendName() const = 0;

  /**
   * @brief Dynamically adjust slot priority (primary vs secondary track) based on active viewport area.
   */
  virtual void UpdatePriority(bool is_primary) { (void)is_primary; }

  /**
   * @brief Attach the render context that owns the live OpenGL context for this thread.
   *
   * This is an EXECUTION-TIME injection, deliberately separate from construction/probing:
   * the decoder is created during footage import, when no viewport renderer exists yet.
   * Only when a frame is actually pulled for display (RetrieveVideoInternal) is there a
   * live, thread-current renderer to bind. Pipelines that do not need it may ignore it.
   *
   * Takes the generic Renderer so callers need no backend-specific cast; a pipeline that
   * requires OpenGL performs the narrowing itself, where the requirement actually lives.
   */
  virtual void SetRenderer(Renderer* renderer) { (void)renderer; }

  /**
   * @brief Return the active AVBufferRef hardware device context, or nullptr.
   */
  virtual AVBufferRef* GetHwDeviceContext() const = 0;
};

} // namespace olive

#endif // VIDEO_HARDWARE_PIPELINE_H
