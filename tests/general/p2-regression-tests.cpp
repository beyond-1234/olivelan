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

/**
 * Phase 9 / patch 0093: regression gates for the P2 fixes.
 *
 * These are deliberately behavioural rather than source-text checks. The 0088/0089 fixes and the
 * HAL routing code had no tests at all, and the existing hal-honesty test greps for wording, which
 * cannot catch a logic regression.
 *
 * Covered:
 *   - ProjectThumbnailCache negative-result cache and per-key in-flight dedupe (0088/P1-1).
 *   - kDurationRole video/audio/still/invalid-duration cases (0088/P1-1).
 *   - GpuEnvironmentProbe OpenGL-vendor precedence under concurrent writers (P2-9).
 *   - GpuSessionAdvisor::ConfiguredMode environment override (0078).
 *   - HardwarePipelineFactory forced-backend get/set under the new mutex (P2-2).
 *   - EGLint is 32-bit when EGL headers are available (P2-8 / patch 0087 ABI).
 */

#include "testutil.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#if __has_include(<EGL/egl.h>)
#include <EGL/egl.h>
#endif

#include "codec/hal/gpu_environment_probe.h"
#include "codec/hal/vaapi_dmabuf_pipeline.h"
#include "common/ffmpegutils.h"
#include "core.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/mastering_display_metadata.h>
}
#include "codec/hal/gpu_session_advisor.h"
#include "codec/hal/hardware_pipeline_factory.h"
#include "node/color/colormanager/colormanager.h"
#include "node/project.h"
#include "node/project/folder/folder.h"
#include "node/project/footage/footage.h"
#include "olive/core/render/audioparams.h"
#include "render/rendermanager.h"
#include "render/renderticket.h"
#include "render/videoparams.h"
#include "widget/projectexplorer/projectthumbnailcache.h"
#include "widget/projectexplorer/projectviewmodel.h"

namespace olive {

/**
 * @brief Minimal Renderer whose PostInit() always fails.
 *
 * Used by RendererFailureDrainsTickets to verify the failure path without requiring a real
 * OpenGL context. The fake implements only the pure virtual seam; no GL call is made.
 */
class FailingRenderer : public Renderer {
public:
  bool Init() override { return true; }
  void PostDestroy() override {}
  bool PostInit() override
  {
    SetLastError(QStringLiteral("injected renderer failure"));
    return false;
  }
  void ClearDestination(Texture*, double, double, double, double) override {}
  QVariant CreateNativeShader(ShaderCode) override { return {}; }
  void DestroyNativeShader(QVariant) override {}
  void UploadToTexture(const QVariant&, const VideoParams&, const void*, int) override {}
  void DownloadFromTexture(const QVariant&, const VideoParams&, void*, int) override {}
  void Flush() override {}
  Color GetPixelFromTexture(Texture*, const QPointF&) override { return {}; }
  int GetGpuShaderLanguage() const override { return 0; }

protected:
  void Blit(QVariant, ShaderJob, Texture*, VideoParams, bool) override {}
  QVariant CreateNativeTexture(int, int, int, PixelFormat, int, const void*, int) override
  {
    return {};
  }
  void DestroyNativeTexture(QVariant) override {}
  void DestroyInternal() override {}
};

/**
 * @brief Test seam for ProjectThumbnailCache's private state machine.
 *
 * The class is a friend in projectthumbnailcache.h. Keeping this helper local means the production
 * API does not grow test-only methods, while the test can still assert negative-cache and FIFO
 * behaviour deterministically (without starting real FFmpeg decodes).
 */
class ProjectThumbnailCacheTest {
public:
  static bool IsInFlight(const ProjectThumbnailCache& cache, const QString& key)
  {
    return cache.in_flight_.contains(key);
  }

  static bool IsFailed(const ProjectThumbnailCache& cache, const QString& key)
  {
    return cache.failed_.contains(key);
  }

  static int FailedCount(const ProjectThumbnailCache& cache)
  {
    return cache.failed_.size();
  }

  static void Complete(ProjectThumbnailCache& cache, const QString& key, const QImage& image)
  {
    cache.OnWorkerDone(key, image);
  }
};

OLIVE_ADD_TEST(ProjectThumbnailNegativeCache)
{
  // Negative results are remembered, so a repaint cannot queue the same doomed FFmpeg open forever.
  {
    ProjectThumbnailCache cache;
    const QString key = QStringLiteral("audio-only.mp3");

    ProjectThumbnailCacheTest::Complete(cache, key, QImage());
    if (!ProjectThumbnailCacheTest::IsFailed(cache, key)) {
      std::cout << " - null image was not negative-cached";
      return __LINE__;
    }

    // A cached negative key must not be enqueued again.
    cache.Request(key, QStringLiteral("/definitely/not/a/real/file.mp3"));
    if (ProjectThumbnailCacheTest::IsInFlight(cache, key)) {
      std::cout << " - Request() re-queued a negative-cached key";
      return __LINE__;
    }
  }

  // Repeated Request() calls for the same key while a worker is running must be coalesced.
  {
    ProjectThumbnailCache cache;
    const QString key = QStringLiteral("in-flight.mov");

    cache.Request(key, QStringLiteral("/definitely/not/a/real/file.mov"));
    cache.Request(key, QStringLiteral("/definitely/not/a/real/file.mov"));

    if (!ProjectThumbnailCacheTest::IsInFlight(cache, key)) {
      std::cout << " - first Request() did not mark the key in flight";
      return __LINE__;
    }

    // Two requests must still produce one in-flight entry (the set cannot show a count, but a
    // second distinct key proves the set is not simply short-circuited by a global busy flag).
    if (ProjectThumbnailCacheTest::IsInFlight(cache, QStringLiteral("other.mov"))) {
      std::cout << " - unrelated key was reported in flight";
      return __LINE__;
    }
  }

  // The negative cache is bounded: after kMaxEntries distinct failures the oldest is evicted, so a
  // file that becomes readable later is retried eventually.
  {
    ProjectThumbnailCache cache;

    for (int i = 0; i < ProjectThumbnailCache::kMaxEntries + 1; i++) {
      ProjectThumbnailCacheTest::Complete(cache, QStringLiteral("fail-%1").arg(i), QImage());
    }

    if (ProjectThumbnailCacheTest::FailedCount(cache) != ProjectThumbnailCache::kMaxEntries) {
      std::cout << " - negative cache exceeded its bound: "
                << ProjectThumbnailCacheTest::FailedCount(cache);
      return __LINE__;
    }

    if (ProjectThumbnailCacheTest::IsFailed(cache, QStringLiteral("fail-0"))) {
      std::cout << " - oldest negative entry was not evicted";
      return __LINE__;
    }

    if (!ProjectThumbnailCacheTest::IsFailed(cache,
                                             QStringLiteral("fail-%1")
                                                 .arg(ProjectThumbnailCache::kMaxEntries))) {
      std::cout << " - newest negative entry is missing";
      return __LINE__;
    }
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ProjectViewModelDurationRole)
{
  // Footage creates a QTimer and the project emits queued signals; both need an event dispatcher.
  int argc = 1;
  char application_name[] = "p2-regression-tests";
  char* argv[] = { application_name, nullptr };
  std::unique_ptr<QCoreApplication> application;
  if (!QCoreApplication::instance()) {
    application = std::make_unique<QCoreApplication>(argc, argv);
  }

  // Project's ColorManager always expects an OCIO config in this build.
  ColorManager::SetUpDefaultConfig();

  Project project;
  project.Initialize();
  Footage* footage = new Footage();
  footage->setParent(&project);
  // Footage::Clear() (called by the constructor) leaves the stream arrays empty; real imports
  // resize them from the probe. Resize to one element so SetVideoParams()/SetAudioParams() have
  // somewhere to write (ViewerOutput::k*ParamsInput and Node::InputArrayResize are public).
  footage->InputArrayResize(Footage::kVideoParamsInput, 1);
  footage->InputArrayResize(Footage::kAudioParamsInput, 1);
  {
    struct AddChild : FolderAddChild {
      using FolderAddChild::FolderAddChild;
      using FolderAddChild::redo;
    } add(project.root(), footage);
    add.redo();
  }

  ProjectViewModel model(nullptr);
  model.set_project(&project);

  if (model.rowCount() != 1) {
    std::cout << " - unexpected project row count: " << model.rowCount();
    return __LINE__;
  }

  const QModelIndex index = model.index(0, ProjectViewModel::kName);
  if (!index.isValid()) {
    std::cout << " - could not obtain the footage index";
    return __LINE__;
  }

  // The third argument is the stream time base (frame duration), not the frame rate.
  VideoParams video(1920, 1080, rational(1, 25), PixelFormat::U8,
                    VideoParams::kRGBAChannelCount);
  video.set_time_base(rational(1, 25));
  video.set_enabled(true);

  // 250 frames at 25 fps = ten seconds.
  video.set_duration(250);
  footage->SetVideoParams(video);

  const QString video_duration = model.data(index, ProjectViewModel::kDurationRole).toString();
  if (video_duration != QStringLiteral("00:00:10:00")) {
    std::cout << " - video duration was " << video_duration.toStdString()
              << ", expected 00:00:10:00";
    return __LINE__;
  }

  // Audio-only media has no frame rate; the role must fall back to seconds.
  video.set_enabled(false);
  footage->SetVideoParams(video);

  AudioParams audio(48000, AudioParams::kSupportedChannelLayouts.front(), SampleFormat::S16);
  audio.set_enabled(true);
  audio.set_duration(96000);
  footage->SetAudioParams(audio);

  const QString audio_duration = model.data(index, ProjectViewModel::kDurationRole).toString();
  if (audio_duration != QStringLiteral("00:00:02.000")) {
    std::cout << " - audio duration was " << audio_duration.toStdString()
              << ", expected 00:00:02.000";
    return __LINE__;
  }

  // A still has no meaningful duration even if its container reports one.
  audio.set_enabled(false);
  footage->SetAudioParams(audio);

  video.set_enabled(true);
  video.set_video_type(VideoParams::kVideoTypeStill);
  video.set_duration(250);
  footage->SetVideoParams(video);

  if (!model.data(index, ProjectViewModel::kDurationRole).toString().isEmpty()) {
    std::cout << " - still image reported a duration: "
              << model.data(index, ProjectViewModel::kDurationRole).toString().toStdString();
    return __LINE__;
  }

  // AV_NOPTS_VALUE/unknown durations become 0 upstream and must not be formatted as timecode.
  video.set_video_type(VideoParams::kVideoTypeVideo);
  video.set_duration(0);
  footage->SetVideoParams(video);

  if (!model.data(index, ProjectViewModel::kDurationRole).toString().isEmpty()) {
    std::cout << " - zero duration reported a duration";
    return __LINE__;
  }

  // Project owns the HDR metadata decision (not ColorManager). Verify that a metadata change
  // invalidates the cache and updates HasHdrFootage() immediately.
  video.set_video_type(VideoParams::kVideoTypeVideo);
  video.set_duration(250);
  video.set_enabled(true);
  video.set_color_trc(VideoParams::kTransferSmpte2084);
  footage->SetVideoParams(video);
  if (!project.HasHdrFootage()) {
    std::cout << " - HDR footage was not detected after PQ metadata was set";
    return __LINE__;
  }

  video.set_color_trc(VideoParams::kTransferBt709);
  footage->SetVideoParams(video);
  if (project.HasHdrFootage()) {
    std::cout << " - HDR cache was not invalidated after the stream became SDR";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(GpuOpenGlVendorPrecedence)
{
  auto& probe = GpuEnvironmentProbe::Instance();

  // A platform-level (offscreen) reading is only a stand-in.
  probe.RecordOpenGlVendor("P2 Platform Vendor", false);

  // A render-context reading is authoritative and supersedes it.
  probe.RecordOpenGlVendor("P2 Render Vendor", true);
  if (probe.GetOpenGlVendor() != "P2 Render Vendor") {
    std::cout << " - render-context vendor did not supersede the platform vendor";
    return __LINE__;
  }

  // A later platform-level reading must not displace the render-context one.
  probe.RecordOpenGlVendor("P2 Later Platform Vendor", false);
  if (probe.GetOpenGlVendor() != "P2 Render Vendor") {
    std::cout << " - a later platform vendor overwrote the render-context vendor";
    return __LINE__;
  }

  if (probe.IsOpenGlOnNvidia()) {
    std::cout << " - a non-NVIDIA vendor was classified as NVIDIA";
    return __LINE__;
  }

  // Exercise the mutex from several threads. The strings are not NVIDIA, so RecordOpenGlVendor()
  // takes the write/read path instead of the "already authoritative" early return.
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; i++) {
    threads.emplace_back([&probe, i]() {
      const QByteArray vendor = QStringLiteral("P2 Concurrent Vendor %1").arg(i).toLatin1();
      probe.RecordOpenGlVendor(vendor.constData(), true);
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  if (probe.GetOpenGlVendor().empty()) {
    std::cout << " - vendor became empty after concurrent records";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(GpuSessionModeEnvironmentPrecedence)
{
  const QByteArray old_value = qgetenv("OLIVE_GPU_SESSION");

  qputenv("OLIVE_GPU_SESSION", "xwayland");
  if (GpuSessionAdvisor::ConfiguredMode() != GpuSessionAdvisor::Mode::XwaylandPrime) {
    std::cout << " - OLIVE_GPU_SESSION=xwayland did not select Xwayland";
    return __LINE__;
  }

  qputenv("OLIVE_GPU_SESSION", "native");
  if (GpuSessionAdvisor::ConfiguredMode() != GpuSessionAdvisor::Mode::NativeSession) {
    std::cout << " - OLIVE_GPU_SESSION=native did not select the native session";
    return __LINE__;
  }

  if (old_value.isEmpty()) {
    qunsetenv("OLIVE_GPU_SESSION");
  } else {
    qputenv("OLIVE_GPU_SESSION", old_value);
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(HardwarePipelineForcedBackendState)
{
  const QString old_value = HardwarePipelineFactory::GetForcedBackend();

  HardwarePipelineFactory::SetForcedBackend(QStringLiteral("VAAPI"));
  if (HardwarePipelineFactory::GetForcedBackend() != QStringLiteral("vaapi")) {
    std::cout << " - forced backend was not normalized/stored";
    return __LINE__;
  }

  HardwarePipelineFactory::SetForcedBackend(QString());
  if (!HardwarePipelineFactory::GetForcedBackend().isEmpty()) {
    std::cout << " - clearing the forced backend did not take effect";
    return __LINE__;
  }

  HardwarePipelineFactory::SetForcedBackend(old_value);

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ForcedBackendFallbackNotification)
{
  // Patch 0097: an unknown/unavailable forced backend must reach the UI once, not only qWarning().
  const QString old_backend = HardwarePipelineFactory::GetForcedBackend();
  QStringList notices;
  HardwarePipelineFactory::SetFallbackHandler([&notices](const QString& message) {
    notices.append(message);
  });
  HardwarePipelineFactory::SetForcedBackend(QStringLiteral("definitely-not-a-backend"));

  AVCodecContext* codec = avcodec_alloc_context3(nullptr);
  codec->codec_id = AV_CODEC_ID_PRORES;
  codec->width = 1920;
  codec->height = 1080;
  codec->pix_fmt = AV_PIX_FMT_YUV422P10LE;

  // ProRes takes the CPU pre-filter after the forced backend fails, so this test does not need a
  // GPU or a display. Two calls also pin the duplicate-suppression behavior.
  auto first = HardwarePipelineFactory::CreatePipeline(codec, 1920, 1080, nullptr);
  auto second = HardwarePipelineFactory::CreatePipeline(codec, 1920, 1080, nullptr);

  if (!first || !second) {
    std::cout << " - CPU fallback pipeline was not created";
    avcodec_free_context(&codec);
    HardwarePipelineFactory::SetForcedBackend(old_backend);
    HardwarePipelineFactory::SetFallbackHandler({});
    return __LINE__;
  }
  if (notices.size() != 1 || !notices.first().contains(QStringLiteral("unknown forced backend"))) {
    std::cout << " - fallback notification was missing or duplicated: " << notices.size();
    avcodec_free_context(&codec);
    HardwarePipelineFactory::SetForcedBackend(old_backend);
    HardwarePipelineFactory::SetFallbackHandler({});
    return __LINE__;
  }

  first.reset();
  second.reset();

  avcodec_free_context(&codec);
  HardwarePipelineFactory::SetForcedBackend(old_backend);
  HardwarePipelineFactory::SetFallbackHandler({});

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(EglintAbiIs32Bit)
{
#if __has_include(<EGL/egl.h>)
  // Patch 0087: eglCreateImageKHR() reads EGLint attributes at a 4-byte stride. A pointer-sized
  // EGLint array made Mesa read past every key and return EGL_BAD_PARAMETER. Pin the ABI.
  static_assert(sizeof(EGLint) == 4, "EGLint must be 32-bit (patch 0087 ABI contract)");
  OLIVE_TEST_END;
#else
  std::cout << " (EGL headers not installed; ABI static_assert skipped)";
  OLIVE_TEST_END;
#endif
}

OLIVE_ADD_TEST(GpuVaapiImportStatus)
{
  // Patch 0094: the UI must distinguish "VA-API device exists" from "a real frame actually
  // imported". The probe records both outcomes and never lets a later Refresh() erase them.
  auto& probe = GpuEnvironmentProbe::Instance();
  probe.ResetVaapiImportStatus();

  if (probe.GetVaapiImportStatus() != VaapiImportStatus::Unknown) {
    std::cout << " - reset status was not Unknown";
    return __LINE__;
  }

  probe.RecordVaapiImportResult(false);
  if (probe.GetVaapiImportStatus() != VaapiImportStatus::Failed) {
    std::cout << " - a failed import did not produce Failed";
    return __LINE__;
  }

  probe.RecordVaapiImportResult(true);
  if (probe.GetVaapiImportStatus() != VaapiImportStatus::Mixed) {
    std::cout << " - success after failure did not produce Mixed";
    return __LINE__;
  }

  probe.ResetVaapiImportStatus();
  probe.RecordVaapiImportResult(true);
  if (probe.GetVaapiImportStatus() != VaapiImportStatus::Verified) {
    std::cout << " - a verified import did not produce Verified";
    return __LINE__;
  }

  // Repeated failures must not overflow the bitmask or change the state.
  for (int i = 0; i < 32; i++) {
    probe.RecordVaapiImportResult(false);
  }
  if (probe.GetVaapiImportStatus() != VaapiImportStatus::Mixed) {
    std::cout << " - repeated failures changed the mixed state";
    return __LINE__;
  }

  probe.ResetVaapiImportStatus();

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(CoreAutoplayParamDefault)
{
  // Patch 0095: autoplay is opt-in, and a zero/negative value must never start a benchmark by
  // accident. The actual playback path is exercised by the remote GUI benchmark.
  Core::CoreParams params;
  if (params.autoplay_seconds() != 0) {
    std::cout << " - autoplay was enabled by default";
    return __LINE__;
  }

  params.set_autoplay_seconds(7);
  if (params.autoplay_seconds() != 7) {
    std::cout << " - autoplay seconds were not stored";
    return __LINE__;
  }

  params.set_autoplay_seconds(0);
  if (params.autoplay_seconds() != 0) {
    std::cout << " - autoplay could not be disabled";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(VaapiFrameColorMetadata)
{
  // Patch 0096: the hardware path must use the decoded frame's colour tags, not only the
  // VideoParams serialized into the project. A 10-bit HDR re-import can otherwise keep an
  // earlier 8-bit/SDR matrix and range while still producing 10-bit planes.
  VideoParams params(1920, 1080, rational(1, 25), PixelFormat::U8,
                     VideoParams::kRGBAChannelCount);
  params.set_color_space(VideoParams::kColorSpaceBt709);
  params.set_color_range(VideoParams::kColorRangeLimited);
  params.set_color_primaries(VideoParams::kColorPrimariesBt709);
  params.set_color_trc(VideoParams::kTransferBt709);
  params.set_mastering_peak_nits(203.0);

  AVFrame* frame = av_frame_alloc();
  frame->colorspace = AVCOL_SPC_BT2020_NCL;
  frame->color_primaries = AVCOL_PRI_BT2020;
  frame->color_trc = AVCOL_TRC_SMPTE2084;
  frame->color_range = AVCOL_RANGE_JPEG;

  AVFrameSideData* side_data = av_frame_new_side_data(
      frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA,
      sizeof(AVMasteringDisplayMetadata));
  if (!side_data) {
    std::cout << " - could not allocate mastering-display side data";
    av_frame_free(&frame);
    return __LINE__;
  }
  memset(side_data->data, 0, side_data->size);
  auto* metadata = reinterpret_cast<AVMasteringDisplayMetadata*>(side_data->data);
  metadata->has_luminance = 1;
  metadata->max_luminance.num = 1000;
  metadata->max_luminance.den = 1;

  const VideoParams effective = VaapiDmaBufPipeline::ResolveColorMetadata(params, frame);
  if (effective.color_space() != VideoParams::kColorSpaceBt2020Ncl) {
    std::cout << " - frame matrix did not override the cached matrix";
    av_frame_free(&frame);
    return __LINE__;
  }
  if (effective.color_range() != VideoParams::kColorRangeFull) {
    std::cout << " - frame range did not override the cached range";
    av_frame_free(&frame);
    return __LINE__;
  }
  if (effective.color_primaries() != VideoParams::kColorPrimariesBt2020) {
    std::cout << " - frame primaries did not override the cached primaries";
    av_frame_free(&frame);
    return __LINE__;
  }
  if (effective.color_trc() != VideoParams::kTransferSmpte2084) {
    std::cout << " - frame transfer did not override the cached transfer";
    av_frame_free(&frame);
    return __LINE__;
  }
  if (effective.colorspace() != QStringLiteral("Rec.2020 PQ")) {
    std::cout << " - PQ frame did not select the Rec.2020 PQ display space";
    av_frame_free(&frame);
    return __LINE__;
  }
  if (effective.mastering_peak_nits() != 1000.0) {
    std::cout << " - frame mastering peak was not applied";
    av_frame_free(&frame);
    return __LINE__;
  }

  // A frame that carries no colour description must leave the cached values alone.
  AVFrame* unspecified = av_frame_alloc();
  const VideoParams fallback = VaapiDmaBufPipeline::ResolveColorMetadata(params, unspecified);
  if (fallback.color_space() != params.color_space()
      || fallback.color_range() != params.color_range()
      || fallback.color_trc() != params.color_trc()
      || fallback.mastering_peak_nits() != params.mastering_peak_nits()) {
    std::cout << " - unspecified frame metadata clobbered the cached values";
    av_frame_free(&unspecified);
    av_frame_free(&frame);
    return __LINE__;
  }

  av_frame_free(&unspecified);
  av_frame_free(&frame);

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(FrameColorRangePriority)
{
  // Patch 0104: the decoder must not overwrite an explicit AVFrame range with the project's
  // cached VideoParams. Only an unspecified frame range falls back to the project value.
  if (FFmpegUtils::ResolveColorRange(AVCOL_RANGE_MPEG, VideoParams::kColorRangeFull)
      != AVCOL_RANGE_MPEG) {
    std::cout << " - limited decoded range was overwritten by a full-range project";
    return __LINE__;
  }
  if (FFmpegUtils::ResolveColorRange(AVCOL_RANGE_JPEG, VideoParams::kColorRangeLimited)
      != AVCOL_RANGE_JPEG) {
    std::cout << " - full decoded range was overwritten by a limited-range project";
    return __LINE__;
  }
  if (FFmpegUtils::ResolveColorRange(AVCOL_RANGE_UNSPECIFIED, VideoParams::kColorRangeFull)
      != AVCOL_RANGE_JPEG) {
    std::cout << " - unspecified range did not fall back to the full-range project value";
    return __LINE__;
  }
  if (FFmpegUtils::ResolveColorRange(AVCOL_RANGE_UNSPECIFIED, VideoParams::kColorRangeLimited)
      != AVCOL_RANGE_MPEG) {
    std::cout << " - unspecified range did not fall back to the limited-range project value";
    return __LINE__;
  }
  if (FFmpegUtils::ResolveColorRange(AVCOL_RANGE_UNSPECIFIED, VideoParams::kColorRangeAuto)
      != AVCOL_RANGE_MPEG) {
    std::cout << " - unspecified range with kColorRangeAuto did not fall back to limited";
    return __LINE__;
  }

  // Patch 0105: explicit user override must win even if the frame specifies range
  if (FFmpegUtils::ResolveColorRange(AVCOL_RANGE_MPEG, VideoParams::kColorRangeFull, true)
      != AVCOL_RANGE_JPEG) {
    std::cout << " - user override to full range did not override limited decoded frame";
    return __LINE__;
  }
  if (FFmpegUtils::ResolveColorRange(AVCOL_RANGE_JPEG, VideoParams::kColorRangeLimited, true)
      != AVCOL_RANGE_MPEG) {
    std::cout << " - user override to limited range did not override full decoded frame";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ColorRangeOverridePersistence)
{
  // Patch 0113: the user's explicit range choice must survive save/load, and an
  // imported range must not masquerade as an override (otherwise 0104 regresses).
  VideoParams saved;
  saved.set_color_range(VideoParams::kColorRangeFull);
  saved.set_color_range_overridden(true);

  QString xml;
  {
    QXmlStreamWriter w(&xml);
    w.writeStartElement(QStringLiteral("video"));
    saved.Save(&w);
    w.writeEndElement();
  }

  VideoParams loaded;
  {
    QXmlStreamReader r(xml);
    while (!r.atEnd() && !(r.isStartElement() && r.name() == QStringLiteral("video"))) {
      r.readNext();
    }
    loaded.Load(&r);
  }

  if (loaded.color_range() != VideoParams::kColorRangeFull || !loaded.color_range_overridden()) {
    std::cout << " - explicit range override was not round-tripped";
    return __LINE__;
  }

  // A file written before this flag existed has no <colorrangeoverride>; it must be
  // treated as "no override" so the decoded frame wins (the 0104 semantics).
  const QString legacy = QStringLiteral("<video><colorrange>1</colorrange></video>");
  VideoParams legacy_loaded;
  {
    QXmlStreamReader r(legacy);
    while (!r.atEnd() && !(r.isStartElement() && r.name() == QStringLiteral("video"))) {
      r.readNext();
    }
    legacy_loaded.Load(&r);
  }

  if (legacy_loaded.color_range() != VideoParams::kColorRangeFull
      || legacy_loaded.color_range_overridden()) {
    std::cout << " - legacy stream range was misread as a user override";
    return __LINE__;
  }

  const VideoParams defaults;
  if (defaults.color_range() != VideoParams::kColorRangeAuto
      || defaults.color_range_overridden()) {
    std::cout << " - default VideoParams should be auto range without an override";
    return __LINE__;
  }

  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(StreamFallbackContextPrecedence)
{
  // Patch 0116: pin the decision the decoder relies on. A forced backend must be
  // retried when neither cache knows about it, skipped when either the per-stream
  // context or the process-wide (backend+codec+format+size) cache recorded it, and
  // never suppressed for an unrelated backend or a different stream shape.
  AVCodecContext* codec = avcodec_alloc_context3(nullptr);
  codec->codec_id = AV_CODEC_ID_PRORES;
  codec->width = 1920;
  codec->height = 1080;
  codec->pix_fmt = AV_PIX_FMT_YUV422P10LE;

  const QString backend = QStringLiteral("test-forced-backend");
  const QString other_backend = QStringLiteral("test-other-backend");
  QSet<QString> global;
  StreamFallbackContext stream_ctx;

  if (HardwarePipelineFactory::IsForcedBackendAlreadyFailed(backend, global, nullptr, codec)) {
    std::cout << " - an unattempted backend was reported as failed";
    avcodec_free_context(&codec);
    return __LINE__;
  }

  stream_ctx.failed_backends.insert(backend);
  if (!HardwarePipelineFactory::IsForcedBackendAlreadyFailed(backend, global, &stream_ctx, codec)) {
    std::cout << " - the per-stream context did not suppress its recorded backend";
    avcodec_free_context(&codec);
    return __LINE__;
  }
  if (HardwarePipelineFactory::IsForcedBackendAlreadyFailed(other_backend, global, &stream_ctx, codec)) {
    std::cout << " - the per-stream context suppressed an unrelated backend";
    avcodec_free_context(&codec);
    return __LINE__;
  }

  global.insert(HardwarePipelineFactory::ForcedFailureKey(backend, codec));
  if (!HardwarePipelineFactory::IsForcedBackendAlreadyFailed(backend, global, nullptr, codec)) {
    std::cout << " - the global cache did not suppress a recorded key";
    avcodec_free_context(&codec);
    return __LINE__;
  }

  codec->width = 1280;
  if (HardwarePipelineFactory::IsForcedBackendAlreadyFailed(backend, global, nullptr, codec)) {
    std::cout << " - the global cache ignored the frame dimensions in the key";
    avcodec_free_context(&codec);
    return __LINE__;
  }

  avcodec_free_context(&codec);
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(RendererFailureDrainsTickets)
{
  // R-5: a failed render thread must not leave tickets permanently unresolved. Both a ticket
  // queued before the failure and one submitted afterwards must finish exactly once.
  DecoderCache decoder_cache;
  ShaderCache shader_cache;
  auto* renderer = new FailingRenderer();
  RenderThread thread(renderer, &decoder_cache, &shader_cache);

  RenderTicketPtr before = std::make_shared<RenderTicket>();
  thread.AddTicket(before);
  thread.start();
  thread.wait();

  if (!thread.IsFailed()) {
    std::cout << " - render thread did not expose its failed state";
    delete renderer;
    return __LINE__;
  }
  if (before->GetFinishCount() != 1) {
    std::cout << " - ticket queued before renderer failure was not finished exactly once";
    delete renderer;
    return __LINE__;
  }

  RenderTicketPtr after = std::make_shared<RenderTicket>();
  thread.AddTicket(after);
  if (after->GetFinishCount() != 1) {
    std::cout << " - ticket submitted after renderer failure was not completed";
    delete renderer;
    return __LINE__;
  }

  delete renderer;
  OLIVE_TEST_END;
}

} // namespace olive
