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
#include <memory>
#include <thread>
#include <vector>

#include <QCoreApplication>
#include <QImage>
#include <QString>

#if __has_include(<EGL/egl.h>)
#include <EGL/egl.h>
#endif

#include "codec/hal/gpu_environment_probe.h"
#include "core.h"
#include "codec/hal/gpu_session_advisor.h"
#include "codec/hal/hardware_pipeline_factory.h"
#include "node/color/colormanager/colormanager.h"
#include "node/project.h"
#include "node/project/folder/folder.h"
#include "node/project/footage/footage.h"
#include "olive/core/render/audioparams.h"
#include "render/videoparams.h"
#include "widget/projectexplorer/projectthumbnailcache.h"
#include "widget/projectexplorer/projectviewmodel.h"

namespace olive {

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

#ifdef OLIVE_ENABLE_OCIO
  // Project's ColorManager expects an OCIO config to exist in this build (see color-role-tests).
  ColorManager::SetUpDefaultConfig();
#endif

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

} // namespace olive
