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

  const QString program = QCoreApplication::applicationFilePath();
  const QStringList arguments = QCoreApplication::arguments().mid(1);

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
    // Native session: deliberately export NOTHING. Setting __NV_PRIME_RENDER_OFFLOAD on its own
    // pulls the GL context onto the dGPU while the compositor stays on the iGPU, and the window
    // then fails to present at all (measured as a blank white client area, patch 0076).
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

  QString gl_vendor = probe.GetOpenGlVendor().empty()
                          ? QStringLiteral("unknown")
                          : QString::fromStdString(probe.GetOpenGlVendor());

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
  assessment.tier1_possible =
      assessment.nvidia_present && probe.HasNvdecSupport() && !probe.IsCudaGlInteropBlocked();
  assessment.tier2_possible = probe.SupportsVaapiDmaBuf();
  assessment.nvidia_has_display = probe.HasConnectedDisplayOn(GpuVendor::Nvidia);
  assessment.display_gpu = probe.GetOpenGlVendor().empty()
                               ? QStringLiteral("unknown")
                               : QString::fromStdString(probe.GetOpenGlVendor());
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
    lines << Tr("NVDEC: %1").arg(GpuEnvironmentProbe::Instance().HasNvdecSupport()
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
  lines << Tr("Tier 2: %1")
               .arg(assessment.tier2_possible ? Tr("available (VA-API)") : Tr("unavailable"));

  return lines.join(QStringLiteral("\n"));
}

QString GpuSessionAdvisor::DescribeEffectivePath(const Assessment& assessment)
{
  if (assessment.tier1_possible) {
    return Tr("Tier 1 - CUDA-NVDEC decode with CUDA-GL same-GPU zero-copy. Already the best path "
              "on this machine.");
  }

  if (assessment.tier2_possible) {
    // Deliberately no per-driver claim here. Whether the dmabuf import succeeds depends on the
    // stream *and* the driver, and patch 0087 showed that a driver we had written off
    // (Mesa/radeonsi) does establish zero-copy once the EGLint attrib array is passed with the
    // correct ABI. The runtime probe tree and the [VAAPI-HAL] log carry the actual import status;
    // the only thing this sentence must promise is that decoding runs on the display GPU.
    return Tr("Tier 2 - VA-API decodes on the display GPU (%1), using less CPU than Tier 3.")
        .arg(assessment.display_gpu);
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
                    "connected (%1). Make that screen the primary one, or move the Olive window "
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
    actions << Tr("This machine has no NVIDIA GPU, so Tier 1 does not apply; the remaining two "
                  "options are the only ones.");
  }

  if (assessment.wayland_session) {
    if (assessment.xwayland_available && assessment.nvidia_present) {
      actions << Tr("[Second best] Xwayland + PRIME: switch to X11 (xcb) so the OpenGL context "
                    "lands on the discrete GPU, which makes Tier 1 reachable. The cost is the "
                    "native session (per-monitor fractional scaling, native input methods). "
                    "Switching restarts Olive into the new session.");
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
  blocks << Tr("This session will use:") + QStringLiteral("\n  ") + DescribeEffectivePath(assessment);
  blocks << Tr("Options:") + QStringLiteral("\n  ")
                + RecommendedActions(assessment).join(QStringLiteral("\n  "));
  return blocks.join(QStringLiteral("\n\n"));
}

} // namespace olive
