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

  return QStringLiteral("session=%1 xwayland=%2 hybrid=%3 nvidia=%4 gl=%5 tier1=%6 mode=%7")
      .arg(IsWaylandSession() ? QStringLiteral("wayland") : QStringLiteral("x11"))
      .arg(IsXwaylandAvailable() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(probe.IsHybridTopology() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(HasNvidiaDevice() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(gl_vendor, tier1,
           ConfiguredMode() == Mode::XwaylandPrime ? QStringLiteral("xwayland+prime")
                                                   : QStringLiteral("native"));
}

QString GpuSessionAdvisor::DescribeAdvice()
{
  return QCoreApplication::translate("GpuSessionAdvisor",
    "Best option, and it needs no software switch: make the discrete GPU drive the display.\n"
    "  - Set the firmware/MUX graphics mode to discrete-only (often \"Discrete Graphics\" or\n"
    "    \"dGPU only\" in the BIOS), or connect the monitor to an output wired to the discrete GPU.\n"
    "  - The compositor then runs on the discrete GPU, so the OpenGL context and CUDA end up on the\n"
    "    same card: you keep the native session AND get Tier 1 (CUDA-GL zero-copy).\n"
    "\n"
    "Otherwise pick one:\n"
    "  - Native session (default) - keeps every native compositor feature. On a hybrid GPU the\n"
    "    decode runs on the display GPU (Tier 2), because Tier 1 is structurally impossible when the\n"
    "    compositor owns the display GPU. Note: on Mesa/radeonsi the Tier 2 zero-copy import is\n"
    "    currently not functional, so those frames take a GPU->CPU readback.\n"
    "  - Xwayland + PRIME - switches to an X11 (xcb) session so the OpenGL context is placed on the\n"
    "    NVIDIA GPU, which makes Tier 1 reachable. Costs the native session (per-monitor fractional\n"
    "    scaling, native input methods).\n"
    "\n"
    "Either change takes effect the next time Olive starts.");
}

} // namespace olive
