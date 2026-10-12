#ifndef VIDEO_HARDWARE_PIPELINE_H
#define VIDEO_HARDWARE_PIPELINE_H

#include <cstdint>
#include <memory>
#include <string>

struct AVCodecContext;
struct AVFrame;
struct AVBufferRef;

namespace olive {

enum class HardwareBackendType {
  CudaNvdec,          // Tier 1: Dedicated NVDEC + CUDA resident path (RTX/GTX)
  VaapiDmaBuf,        // Tier 2: Linux AMD/Intel native lightweight DMA-BUF
  D3D11Interop,       // Tier 2: Windows AMD/Intel native D3D11 shared texture
  LibPlaceboFallback, // Tier 3: Cross-adapter / HDR tone mapping / generic fallback
  SoftwareFallback    // Software decode (CPU) fallback
};

struct HardwareDecodedFrame {
  uint32_t gl_texture_id{0}; // Output OpenGL texture identifier
  float uv_scale_x{1.0f};     // Sub-rectangle normalized U boundary in bounding box
  float uv_scale_y{1.0f};     // Sub-rectangle normalized V boundary in bounding box
  int64_t pts{0};            // Presentation timestamp
  int width{0};              // Active video frame width
  int height{0};             // Active video frame height
  int coded_width{0};        // HW buffer padded width
  int coded_height{0};       // HW buffer padded height
  int pool_slot_index{-1};   // Internal frame pool slot index (-1 if non-pooled)
  void* backend_cookie{nullptr};
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
   * @brief Process an incoming hardware AVFrame into a standard OpenGL texture (< 1ms target).
   * @param hw_frame Hardware AVFrame produced by avcodec_receive_frame.
   * @param out_frame Out parameter populated with the GL texture ID and UV normalization scale.
   * @return true if mapping succeeded, false otherwise.
   */
  virtual bool ProcessHardwareFrame(AVFrame* hw_frame, HardwareDecodedFrame& out_frame) = 0;

  /**
   * @brief Return the hardware frame slot / unmap texture for reuse.
   */
  virtual void ReleaseFrame(const HardwareDecodedFrame& frame) = 0;

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
   * @brief Return the active AVBufferRef hardware device context, or nullptr.
   */
  virtual AVBufferRef* GetHwDeviceContext() const = 0;
};

} // namespace olive

#endif // VIDEO_HARDWARE_PIPELINE_H
