/***

  Olivelan - Non-Linear Video Editor
  Copyright (C) 2022-2026 Olivelan Contributors

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

#include "gpu_session_advisor.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>

#include "config/config.h"
#include "codec/hal/gpu_environment_probe.h"

namespace olive {

namespace {

bool HasNvidiaDevice()
{
  auto& probe = GpuEnvironmentProbe::Instance();

  if (probe.GetPrimaryVendor() == GpuVendor::Nvidia) {
    return true;
  }

  for (const auto& device : probe.GetDetectedDevices()) {
    if (device.vendor == GpuVendor::Nvidia) {
      return true;
    }
  }

  return false;
}

/// @brief Screen number out of a DISPLAY value (":0", ":0.0", "host:1").
int ScreenFromDisplay(const QByteArray& display, bool* ok)
{
  const int colon = display.lastIndexOf(':');
  if (colon < 0) {
    *ok = false;
    return 0;
  }

  QByteArray tail = display.mid(colon + 1);
  const int dot = tail.indexOf('.');
  if (dot >= 0) {
    tail = tail.left(dot);
  }

  return tail.toInt(ok);
}

bool XSocketIsReachable()
{
  const QByteArray display = qgetenv("DISPLAY");
  if (display.isEmpty()) {
    return false;
  }

  bool ok = false;
  const int screen = ScreenFromDisplay(display, &ok);
  if (!ok) {
    return false;
  }

  return QFileInfo::exists(QStringLiteral("/tmp/.X11-unix/X%1").arg(screen));
}

} // namespace

namespace {

/// @brief One-shot marker handed to the process we spawn, so it does not ask the same question again.
const char* const kRestartedEnv = "OLIVE_GPU_SESSION_RESTARTED";

bool g_restart_launched = false;

/// @brief Shorthand for the translation context of everything in this file.
QString Tr(const char* text)
{
  return QCoreApplication::translate("GpuSessionAdvisor", text);
}

QString VendorLabel(GpuVendor vendor)
{
  switch (vendor) {
  case GpuVendor::Nvidia: return QStringLiteral("NVIDIA");
  case GpuVendor::Amd: return QStringLiteral("AMD");
  case GpuVendor::Intel: return QStringLiteral("Intel");
  case GpuVendor::Apple: return QStringLiteral("Apple");
  default: return Tr("unknown");
  }
}

QString NvidiaDeviceName()
{
  for (const auto& device : GpuEnvironmentProbe::Instance().GetDetectedDevices()) {
    if (device.vendor == GpuVendor::Nvidia && !device.device_name.empty()) {
      return QString::fromStdString(device.device_name);
    }
  }
  return QStringLiteral("NVIDIA GPU");
}

QString ConnectedDisplaySummary()
{
  QStringList parts;
  for (const auto& display : GpuEnvironmentProbe::Instance().GetConnectedDisplays()) {
    parts << QStringLiteral("%1 (%2)").arg(QString::fromStdString(display.connector),
                                           VendorLabel(display.gpu));
  }
  return parts.isEmpty() ? Tr("(none detected)") : parts.join(QStringLiteral(", "));
}

} // namespace

bool GpuSessionAdvisor::IsWaylandSession()
{
  if (!qgetenv("WAYLAND_DISPLAY").isEmpty()) {
    return true;
  }

  return qgetenv("XDG_SESSION_TYPE") == "wayland";
}

bool GpuSessionAdvisor::IsXwaylandAvailable()
{
  return !qgetenv("DISPLAY").isEmpty() && XSocketIsReachable();
}

bool GpuSessionAdvisor::HasChoice()
{
  // Anywhere but a hybrid Wayland session both modes put GL on the same GPU, so prompting would
  // only be noise.
  return IsWaylandSession() && GpuEnvironmentProbe::Instance().IsHybridTopology() && HasNvidiaDevice();
}

GpuSessionAdvisor::Mode GpuSessionAdvisor::ConfiguredMode()
{
  // OLIVE_GPU_SESSION overrides the stored preference; same layering idea as OLIVE_HAL_BACKEND.
  const QByteArray requested = qgetenv("OLIVE_GPU_SESSION").trimmed().toLower();
  if (requested == "xwayland" || requested == "xcb") {
    return Mode::XwaylandPrime;
  }
  if (requested == "native" || requested == "wayland") {
    return Mode::NativeSession;
  }

  return (OLIVE_CONFIG("GpuSessionMode").toInt() == 1) ? Mode::XwaylandPrime : Mode::NativeSession;
}

bool GpuSessionAdvisor::IsSessionForcedByEnvironment()
{
  const QByteArray requested = qgetenv("OLIVE_GPU_SESSION").trimmed().toLower();
  return requested == "xwayland" || requested == "xcb" || requested == "native" || requested == "wayland";
}

bool GpuSessionAdvisor::WasLaunchedByRestart()
{
  return !qgetenv(kRestartedEnv).isEmpty();
}

bool GpuSessionAdvisor::RestartLaunched()
{
  return g_restart_launched;
}

bool GpuSessionAdvisor::RestartWithSession(Mode mode)
{
  if (mode == Mode::XwaylandPrime && !IsXwaylandAvailable()) {
    qWarning() << "[GPU Session] Cannot restart into Xwayland: no reachable X display.";
    return false;
  }

  OLIVE_CONFIG("GpuSessionMode") = static_cast<int>(mode);
  Config::Save();

  // The replacement inherits our environment, so apply the choice to ourselves first.
  if (mode == Mode::XwaylandPrime) {
    qputenv("QT_QPA_PLATFORM", "xcb");
    qputenv("__NV_PRIME_RENDER_OFFLOAD", "1");
    qputenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia");
  } else {
    // Back to the native session: remove exactly what the other branch adds. Leaving PRIME set
    // without xcb is what blanks the window under native Wayland.
    qunsetenv("QT_QPA_PLATFORM");
    qunsetenv("__NV_PRIME_RENDER_OFFLOAD");
    qunsetenv("__GLX_VENDOR_LIBRARY_NAME");
  }

  // One-shot marker: the replacement was started because the user just answered this question, so
  // it must not ask it again immediately.
  qputenv(kRestartedEnv, "1");

  const QString appimage = qEnvironmentVariable("APPIMAGE");
  const QString program = !appimage.isEmpty() ? appimage : QCoreApplication::applicationFilePath();
  const QStringList arguments = QCoreApplication::arguments().mid(1);

  // Restore the original host LD_LIBRARY_PATH before restarting to prevent environment poisoning
  const QByteArray orig_ld = qgetenv("OLIVELAN_ORIGINAL_LD_LIBRARY_PATH");
  if (!orig_ld.isEmpty()) {
    qputenv("LD_LIBRARY_PATH", orig_ld);
  } else if (!appimage.isEmpty()) {
    qunsetenv("LD_LIBRARY_PATH");
  }

  if (!QProcess::startDetached(program, arguments)) {
    qWarning() << "[GPU Session] Failed to restart" << program
               << "- the preference is saved and will apply on the next start.";
    qunsetenv(kRestartedEnv);
    return false;
  }

  qInfo() << "[GPU Session] Restarted as" << program << "with the session preference applied.";
  g_restart_launched = true;

  // No effect when the event loop has not started yet (the HAL backend dialog path runs before
  // Core); main() checks RestartLaunched() for that case.
  QCoreApplication::quit();

  return true;
}

void GpuSessionAdvisor::ApplySessionPreference()
{
  // An explicitly set platform wins. Scripts, the test harness and `-platform <name>` must not be
  // silently overridden by a stored preference.
  const QByteArray explicit_platform = qgetenv("QT_QPA_PLATFORM");
  if (!explicit_platform.isEmpty()) {
    qInfo() << "[GPU Session] QT_QPA_PLATFORM is already set to" << explicit_platform
            << "- leaving the session untouched.";
    return;
  }

  if (ConfiguredMode() != Mode::XwaylandPrime) {
    // Native session: deliberately export NOTHING.
    // Critical defect note (Patch 0099): Setting __NV_PRIME_RENDER_OFFLOAD on its own under native
    // Wayland pulls the Qt GL context onto the dGPU while KWin/Mutter compositor stays on the iGPU.
    // This fails presentation completely:
    //   - On single internal display: window fails to present at all (solid blank white client area, patch 0076).
    //   - On external display connected to dGPU (e.g. HDMI on card0): window exhibits corrupted presentation,
    //     becoming a transparent/ghost buffer that continuously redraws any underlying screen contents
    //     (measured & confirmed on RTX 4060 + 780M / KDE Plasma Wayland).
    // Therefore, under native Wayland, PRIME offload MUST NEVER be automatically enabled regardless
    // of whether an external monitor is plugged into the discrete GPU. To use Tier 1 on hybrid
    // hardware, the session must be cleanly switched to X11/Xwayland.
    qInfo() << "[GPU Session]" << DescribeEnvironment();
    return;
  }

  if (!IsXwaylandAvailable()) {
    qWarning() << "[GPU Session] Xwayland was requested but no reachable X display was found"
               << "(DISPLAY=" << qgetenv("DISPLAY") << "); staying on the native session.";
    return;
  }

  // Under PRIME the xcb GLX context is created on the NVIDIA GPU, which is exactly the same-GPU
  // requirement that CUDA-GL interop (Tier 1) has.
  qputenv("QT_QPA_PLATFORM", "xcb");
  qputenv("__NV_PRIME_RENDER_OFFLOAD", "1");
  qputenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia");
  qInfo() << "[GPU Session] Xwayland + PRIME offload requested for this launch:"
          << "QT_QPA_PLATFORM=xcb, __NV_PRIME_RENDER_OFFLOAD=1, __GLX_VENDOR_LIBRARY_NAME=nvidia.";
}

QString GpuSessionAdvisor::DescribeEnvironment()
{
  auto& probe = GpuEnvironmentProbe::Instance();

  const std::string gl_vendor_raw = probe.GetOpenGlVendor();
  const QString gl_vendor = gl_vendor_raw.empty()
                                ? QStringLiteral("unknown")
                                : QString::fromStdString(gl_vendor_raw);

  QString tier1;
  if (probe.IsCudaGlInteropBlocked()) {
    tier1 = QStringLiteral("blocked(gl-not-nvidia)");
  } else if (probe.HasNvdecSupport()) {
    tier1 = QStringLiteral("possible");
  } else {
    tier1 = QStringLiteral("unavailable");
  }

  return QStringLiteral("session=%1 xwayland=%2 hybrid=%3 nvidia=%4 gl=%5 tier1=%6 nvdisp=%7 mode=%8")
      .arg(IsWaylandSession() ? QStringLiteral("wayland") : QStringLiteral("x11"))
      .arg(IsXwaylandAvailable() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(probe.IsHybridTopology() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(HasNvidiaDevice() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(gl_vendor, tier1)
      .arg(probe.HasConnectedDisplayOn(GpuVendor::Nvidia) ? QStringLiteral("yes")
                                                          : QStringLiteral("no"))
      .arg(ConfiguredMode() == Mode::XwaylandPrime ? QStringLiteral("xwayland+prime")
                                                   : QStringLiteral("native"));
}

GpuSessionAdvisor::Assessment GpuSessionAdvisor::Assess()
{
  auto& probe = GpuEnvironmentProbe::Instance();

  Assessment assessment;
  assessment.wayland_session = IsWaylandSession();
  assessment.xwayland_available = IsXwaylandAvailable();
  assessment.hybrid = probe.IsHybridTopology();
  assessment.nvidia_present = HasNvidiaDevice();
  // Tier 1 needs NVDEC *and* the GL context on the same NVIDIA GPU - the second half is what a
  // hybrid Wayland session cannot provide.
  assessment.cuda_gl_interop_blocked = probe.IsCudaGlInteropBlocked();
  assessment.tier1_possible =
      assessment.nvidia_present && probe.HasNvdecSupport() && !assessment.cuda_gl_interop_blocked;

  // Tier 2 is platform-native, not always VA-API. Check VideoToolbox first so macOS reports the
  // backend that HardwarePipelineFactory actually auto-selects there.
  if (probe.SupportsVideoToolbox()) {
    assessment.tier2_backend = Tier2Backend::VideoToolbox;
  } else if (probe.SupportsVaapiDmaBuf()) {
    assessment.tier2_backend = Tier2Backend::VaapiDmaBuf;
    assessment.tier2_import_status = probe.GetVaapiImportStatus();
  }

  // Patch 0094: routing is still per-stream (a 10-bit failure must not disable 8-bit streams), so
  // tier2_possible remains "the backend exists". The VA-API import status is reported separately
  // instead of turning device presence into a zero-copy promise.
  assessment.tier2_possible = assessment.tier2_backend != Tier2Backend::None;
  assessment.nvidia_has_display = probe.HasConnectedDisplayOn(GpuVendor::Nvidia);
  assessment.nvdec_available = probe.HasNvdecSupport();
  const std::string gl_vendor_raw = probe.GetOpenGlVendor();
  assessment.display_gpu = gl_vendor_raw.empty()
                               ? QStringLiteral("unknown")
                               : QString::fromStdString(gl_vendor_raw);
  assessment.nvidia_name = NvidiaDeviceName();
  assessment.connected_displays = ConnectedDisplaySummary();
  return assessment;
}

QString GpuSessionAdvisor::DescribeSituation(const Assessment& assessment)
{
  QStringList lines;
  lines << Tr("Session: %1 (Xwayland %2)")
               .arg(assessment.wayland_session ? Tr("native Wayland") : Tr("X11"),
                    assessment.xwayland_available ? Tr("reachable") : Tr("not reachable"));
  lines << Tr("Display (OpenGL) GPU: %1").arg(assessment.display_gpu);
  lines << Tr("Connected displays: %1").arg(assessment.connected_displays);

  if (assessment.nvidia_present) {
    lines << Tr("Discrete GPU: %1").arg(assessment.nvidia_name);
    lines << Tr("NVDEC: %1").arg(assessment.nvdec_available
                                     ? Tr("available")
                                     : Tr("unavailable"));
    lines << Tr("Connected display on the discrete GPU: %1")
                 .arg(assessment.nvidia_has_display ? Tr("yes") : Tr("no"));
  } else {
    lines << Tr("Discrete GPU: no NVIDIA GPU detected");
  }

  lines << Tr("Tier 1: %1")
               .arg(assessment.tier1_possible
                        ? Tr("available")
                        : Tr("unavailable - CUDA-GL interop needs the OpenGL context and CUDA on "
                             "the same GPU"));
  QString tier2_description;
  switch (assessment.tier2_backend) {
  case Tier2Backend::None:
    tier2_description = Tr("unavailable");
    break;
  case Tier2Backend::VideoToolbox:
    tier2_description = Tr("available (VideoToolbox, macOS)");
    break;
  case Tier2Backend::VaapiDmaBuf:
    switch (assessment.tier2_import_status) {
    case VaapiImportStatus::Unknown:
      tier2_description = Tr("device present; import not yet verified");
      break;
    case VaapiImportStatus::Verified:
      tier2_description = Tr("available (VA-API, EGL import verified)");
      break;
    case VaapiImportStatus::Failed:
      tier2_description = Tr("degraded - import failed for at least one stream; that stream used CPU readback");
      break;
    case VaapiImportStatus::Mixed:
      tier2_description = Tr("partial - some streams verified, others read back");
      break;
    }
    break;
  }
  lines << Tr("Tier 2: %1").arg(tier2_description);

  return lines.join(QStringLiteral("\n"));
}

QVector<GpuSessionAdvisor::TierStatus> GpuSessionAdvisor::DescribeTiers(
    const Assessment& assessment)
{
  QVector<TierStatus> tiers;

  TierStatus tier1;
  tier1.tier = 1;
  tier1.backend = QStringLiteral("CUDA-NVDEC");
#if defined(OLIVE_ENABLE_CUDA_NVDEC)
  if (!assessment.nvidia_present) {
    tier1.state = Tr("Unavailable");
    tier1.reason = Tr("No NVIDIA GPU was detected.");
  } else if (!assessment.nvdec_available) {
    tier1.state = Tr("Unavailable");
    tier1.reason = Tr("An NVIDIA GPU is present, but NVDEC support was not detected.");
  } else if (assessment.cuda_gl_interop_blocked) {
    tier1.state = Tr("Unavailable");
    tier1.reason = Tr("The OpenGL context is on %1, not on the NVIDIA GPU. CUDA-GL interop "
                      "requires both on the same GPU.")
                       .arg(assessment.display_gpu);
  } else {
    tier1.available = true;
    tier1.state = Tr("Available");
    tier1.reason = Tr("NVDEC and the OpenGL context are on the same NVIDIA GPU.");
  }
#else
  tier1.state = Tr("Unavailable");
  tier1.reason = Tr("CUDA-NVDEC is not compiled into this build.");
#endif
  tiers.push_back(tier1);

  TierStatus tier2;
  tier2.tier = 2;
#if defined(__APPLE__)
  tier2.backend = QStringLiteral("VideoToolbox");
  if (assessment.tier2_backend == Tier2Backend::VideoToolbox) {
    tier2.available = true;
    tier2.state = Tr("Available");
    tier2.reason = Tr("VideoToolbox hardware decode with IOSurface import is available.");
  } else {
    tier2.state = Tr("Unavailable");
    tier2.reason = Tr("VideoToolbox is not available in this build or on this Mac.");
  }
#elif defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
  tier2.backend = QStringLiteral("VA-API DMA-BUF");
  if (!assessment.tier2_possible) {
    tier2.state = Tr("Unavailable");
    tier2.reason = Tr("No usable VA-API DMA-BUF device was created for this display session.");
  } else {
    switch (assessment.tier2_import_status) {
    case VaapiImportStatus::Unknown:
      tier2.available = true;
      tier2.state = Tr("Unverified");
      tier2.reason = Tr("The VA-API device is present; EGL import will be verified on the first "
                        "decoded frame.");
      break;
    case VaapiImportStatus::Verified:
      tier2.available = true;
      tier2.state = Tr("Available");
      tier2.reason = Tr("VA-API DMA-BUF EGL import has been verified for this session.");
      break;
    case VaapiImportStatus::Mixed:
      tier2.available = true;
      tier2.state = Tr("Partial");
      tier2.reason = Tr("Some streams imported through DMA-BUF; others fell back to CPU readback.");
      break;
    case VaapiImportStatus::Failed:
      tier2.available = true;
      tier2.state = Tr("Degraded");
      tier2.reason = Tr("A real VA-API DMA-BUF import failed; affected streams use CPU readback.");
      break;
    }
  }
#else
  tier2.backend = Tr("Platform native");
  tier2.state = Tr("Unavailable");
  tier2.reason = Tr("No Tier 2 hardware backend is available for this platform.");
#endif
  tiers.push_back(tier2);

  TierStatus tier3;
  tier3.tier = 3;
  tier3.backend = QStringLiteral("Reinforced CPU Decode + Native YUV Upload");
  tier3.available = true;
  tier3.state = Tr("Available");
  tier3.reason = Tr("CPU decode with PBO upload is always available as the final fallback.");
  tiers.push_back(tier3);

  return tiers;
}

QString GpuSessionAdvisor::DescribeEffectivePath(const Assessment& assessment)
{
  if (assessment.tier1_possible) {
    return Tr("Tier 1 - CUDA-NVDEC decode with CUDA-GL same-GPU zero-copy. Already the best path "
              "on this machine.");
  }

  if (assessment.tier2_possible) {
    if (assessment.tier2_backend == Tier2Backend::VideoToolbox) {
      return Tr("Tier 2 - VideoToolbox hardware decode with GPU-side YUV compositing. This is what "
                "automatic routing selects on this Mac.");
    }

    // Patch 0094: say what has actually been proven. Before the first frame only device creation is
    // known; after a real transfer the probe reports whether EGLImage import succeeded, failed, or
    // varied by stream. Never turn device presence into a zero-copy promise.
    QString import_state;
    switch (assessment.tier2_import_status) {
    case VaapiImportStatus::Unknown:
      import_state = Tr("The device is present; EGL import will be verified when the first frame "
                        "is decoded.");
      break;
    case VaapiImportStatus::Verified:
      import_state = Tr("EGL import has been verified for this session.");
      break;
    case VaapiImportStatus::Mixed:
      import_state = Tr("Some streams imported successfully; others fall back to CPU readback.");
      break;
    case VaapiImportStatus::Failed:
      import_state = Tr("A real EGL import attempt has failed, so that stream used CPU readback; "
                        "other streams are still attempted.");
      break;
    }
    return Tr("Tier 2 - VA-API decodes on the display GPU (%1), using less CPU than Tier 3. %2")
        .arg(assessment.display_gpu, import_state);
  }

  return Tr("Tier 3 - reinforced CPU decode plus PBO upload. The fallback when no hardware decode "
            "is available.");
}

QStringList GpuSessionAdvisor::RecommendedActions(const Assessment& assessment)
{
  QStringList actions;

  if (assessment.tier1_possible) {
    actions << Tr("No change needed: Tier 1 (zero-copy) is available on this machine.");
    return actions;
  }

  if (assessment.nvidia_present) {
    if (assessment.nvidia_has_display) {
      actions << Tr("[Best] Discrete GPU direct: a display on the discrete GPU is already "
                    "connected (%1). Make that screen the primary one, or move the Olivelan window "
                    "onto it, and the OpenGL context lands on the discrete GPU - keeping the "
                    "native session and getting Tier 1.")
                    .arg(assessment.connected_displays);
    } else {
      actions << Tr("[Best] Discrete GPU direct: the discrete GPU (%1) currently has no connected "
                    "display - the display is driven by %2, so the OpenGL context cannot land on "
                    "the discrete GPU. This needs a firmware/MUX switch to discrete-only graphics, "
                    "or a monitor connected to the discrete GPU's output. After that the OpenGL "
                    "context and CUDA share a card: native session kept, Tier 1 available.")
                    .arg(assessment.nvidia_name, assessment.display_gpu);
    }
  } else {
    if (assessment.tier2_possible) {
      actions << Tr("This machine has no NVIDIA GPU, so Tier 1 does not apply. Tier 2 (%1) is the "
                    "best available path and is selected automatically.")
                     .arg(assessment.tier2_backend == Tier2Backend::VideoToolbox
                              ? QStringLiteral("VideoToolbox")
                              : QStringLiteral("VA-API"));
    } else {
      actions << Tr("This machine has no NVIDIA GPU, so Tier 1 does not apply; the remaining two "
                    "options are the only ones.");
    }
  }

  if (assessment.wayland_session) {
    if (assessment.xwayland_available && assessment.nvidia_present) {
      actions << Tr("[Second best] Xwayland + PRIME: switch to X11 (xcb) so the OpenGL context "
                    "lands on the discrete GPU, which makes Tier 1 reachable. The cost is the "
                    "native session (per-monitor fractional scaling, native input methods). "
                    "Switching restarts Olivelan into the new session.");
    } else if (!assessment.xwayland_available) {
      actions << Tr("Xwayland is not available (no reachable X display), so that option cannot be "
                    "selected on this machine.");
    }
  }

  actions << Tr("[Keep current] Leave the session as it is and use the path shown above "
                "(%1).").arg(assessment.tier1_possible
                                 ? QStringLiteral("Tier 1")
                                 : (assessment.tier2_possible ? QStringLiteral("Tier 2")
                                                              : QStringLiteral("Tier 3")));

  return actions;
}

QString GpuSessionAdvisor::DescribeAdvice()
{
  const Assessment assessment = Assess();

  QStringList blocks;
  blocks << DescribeSituation(assessment);
  blocks << Tr("Expected path:") + QStringLiteral("\n  ") + DescribeEffectivePath(assessment);
  blocks << Tr("Options:") + QStringLiteral("\n  ")
                + RecommendedActions(assessment).join(QStringLiteral("\n  "));
  return blocks.join(QStringLiteral("\n\n"));
}

} // namespace olive
