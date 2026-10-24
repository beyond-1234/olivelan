#ifndef REINFORCED_SOFTWARE_PIPELINE_H
#define REINFORCED_SOFTWARE_PIPELINE_H

#include "video_hardware_pipeline.h"

struct AVCodecContext;

namespace olive {

/**
 * @brief Global CPU decode thread budget for Tier 3 software pipelines.
 *
 * Guarantees that multiple concurrent software video decoders (e.g. 4K HDR10 footage,
 * multicam tracks, thumbnail generators) do not oversubscribe the host CPU and starve
 * the real-time audio callback or GUI main thread.
 *
 * Always preserves at least 2 cores for the operating system, audio mixing and OpenGL upload.
 */
class Tier3ThreadBudget {
public:
  static int Acquire(int width, int height, bool is_primary);
  static void Release(int threads);
  static int CurrentAllocated();
  static void ResetForTests();
};

class ReinforcedSoftwarePipeline : public IVideoHardwarePipeline {
public:
  explicit ReinforcedSoftwarePipeline(Renderer* renderer = nullptr);
  virtual ~ReinforcedSoftwarePipeline() override;

  virtual bool ProbeAndInitialize(AVCodecContext* codec_ctx, int max_w, int max_h) override;
  virtual bool ProcessHardwareFrame(AVFrame* hw_frame, const VideoParams& params, HardwareDecodedFrame& out_frame) override;
  virtual void Close() override;

  virtual HardwareBackendType GetType() const override { return HardwareBackendType::SoftwareFallback; }
  virtual const char* GetBackendName() const override { return "Reinforced CPU Decode + Native YUV Shader Upload (Tier 3)"; }
  virtual AVBufferRef* GetHwDeviceContext() const override { return nullptr; }

  virtual void SetRenderer(Renderer* renderer) override { renderer_ = renderer; }
  virtual void UpdatePriority(bool is_primary) override;

  int GetAllocatedThreads() const { return allocated_threads_; }

private:
  Renderer* renderer_{nullptr};
  AVCodecContext* codec_ctx_{nullptr};
  int allocated_threads_{0};
  int max_width_{0};
  int max_height_{0};
  bool is_primary_{true};
};

} // namespace olive

#endif // REINFORCED_SOFTWARE_PIPELINE_H
