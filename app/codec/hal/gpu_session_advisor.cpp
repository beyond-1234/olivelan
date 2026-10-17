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

namespace {

QString VendorLabel(GpuVendor vendor)
{
  switch (vendor) {
  case GpuVendor::Nvidia: return QStringLiteral("NVIDIA");
  case GpuVendor::Amd: return QStringLiteral("AMD");
  case GpuVendor::Intel: return QStringLiteral("Intel");
  case GpuVendor::Apple: return QStringLiteral("Apple");
  default: return QStringLiteral("unknown");
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
  return parts.isEmpty() ? QStringLiteral("(none detected)") : parts.join(QStringLiteral(", "));
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
  lines << QStringLiteral("会话：%1（Xwayland %2）")
               .arg(assessment.wayland_session ? QStringLiteral("原生 Wayland")
                                               : QStringLiteral("X11"))
               .arg(assessment.xwayland_available ? QStringLiteral("可达")
                                                  : QStringLiteral("不可达"));
  lines << QStringLiteral("显示（OpenGL）GPU：%1").arg(assessment.display_gpu);
  lines << QStringLiteral("已连接显示器：%1").arg(assessment.connected_displays);

  if (assessment.nvidia_present) {
    lines << QStringLiteral("独显：%1（NVDEC %2；%3已连接的显示器）")
                 .arg(assessment.nvidia_name,
                      GpuEnvironmentProbe::Instance().HasNvdecSupport()
                          ? QStringLiteral("可用")
                          : QStringLiteral("不可用"),
                      assessment.nvidia_has_display ? QStringLiteral("有")
                                                    : QStringLiteral("没有"));
  } else {
    lines << QStringLiteral("独显：未检测到 NVIDIA GPU");
  }

  lines << QStringLiteral("Tier 1：%1")
               .arg(assessment.tier1_possible
                        ? QStringLiteral("可用")
                        : QStringLiteral("不可用（CUDA-GL 互操作要求 GL 与 CUDA 同卡）"));
  lines << QStringLiteral("Tier 2：%1")
               .arg(assessment.tier2_possible ? QStringLiteral("可用（VA-API）")
                                              : QStringLiteral("不可用"));

  return lines.join(QStringLiteral("\n"));
}

QString GpuSessionAdvisor::DescribeEffectivePath(const Assessment& assessment)
{
  if (assessment.tier1_possible) {
    return QCoreApplication::translate("GpuSessionAdvisor",
        "Tier 1 - CUDA-NVDEC 解码，CUDA-GL 同卡零拷贝。已经是这台机器上的最优路径。");
  }

  if (assessment.tier2_possible) {
    QString text = QCoreApplication::translate("GpuSessionAdvisor",
        "Tier 2 - VA-API 在显示 GPU（%1）上硬件解码，CPU 占用低于 Tier 3。")
        .arg(assessment.display_gpu);

    if (assessment.display_gpu.contains(QStringLiteral("AMD"), Qt::CaseInsensitive)
        || assessment.display_gpu.contains(QStringLiteral("Mesa"), Qt::CaseInsensitive)) {
      // Measured limitation, not a guess: see reports/hal-zero-copy-matrix.md.
      text += QCoreApplication::translate("GpuSessionAdvisor",
          "该驱动（Mesa/radeonsi）不接受 VA-API 导出的平面格式，dmabuf 零拷贝导入无法建立，"
          "每帧会经 GPU->CPU 回读后再上传；解码本身仍在 GPU 上。");
    } else {
      text += QCoreApplication::translate("GpuSessionAdvisor",
          "dmabuf 零拷贝导入能否建立取决于驱动。");
    }
    return text;
  }

  return QCoreApplication::translate("GpuSessionAdvisor",
      "Tier 3 - 强化 CPU 解码 + PBO 上传。无硬件解码可用时的兜底路径。");
}

QStringList GpuSessionAdvisor::RecommendedActions(const Assessment& assessment)
{
  QStringList actions;

  if (assessment.tier1_possible) {
    actions << QCoreApplication::translate("GpuSessionAdvisor",
        "无需更改：Tier 1（零拷贝）在这台机器上可用。");
    return actions;
  }

  if (assessment.nvidia_present) {
    if (assessment.nvidia_has_display) {
      actions << QCoreApplication::translate("GpuSessionAdvisor",
          "[最优] 独显直连：检测到独显上已有连接的显示器（%1）。把那块屏幕设为主屏，或把 Olive "
          "窗口移过去，GL 就会落在独显上——保留原生会话且拿到 Tier 1。")
          .arg(assessment.connected_displays);
    } else {
      actions << QCoreApplication::translate("GpuSessionAdvisor",
          "[最优] 独显直连：独显（%1）当前没有已连接的显示器，显示由 %2 驱动，所以 GL 无法落在独显上。"
          "需要 BIOS/MUX 切到「独显直连/Discrete Graphics」，或把显示器接到独显的输出口。"
          "之后 GL 与 CUDA 同卡，保留原生会话且拿到 Tier 1。")
          .arg(assessment.nvidia_name, assessment.display_gpu);
    }
  } else {
    actions << QCoreApplication::translate("GpuSessionAdvisor",
        "本机没有 NVIDIA GPU，Tier 1 不适用；下面两条是仅有的选择。");
  }

  if (assessment.wayland_session) {
    if (assessment.xwayland_available && assessment.nvidia_present) {
      actions << QCoreApplication::translate("GpuSessionAdvisor",
          "[次优] Xwayland + PRIME：切到 X11(xcb)，让 GL 落到独显，从而拿到 Tier 1。"
          "代价是放弃原生会话（逐屏分数缩放、原生输入法）。下次启动生效。");
    } else if (!assessment.xwayland_available) {
      actions << QCoreApplication::translate("GpuSessionAdvisor",
          "Xwayland 不可用（没有可达的 X 显示），该路径在本机无法选择。");
    }
  }

  actions << QCoreApplication::translate("GpuSessionAdvisor",
      "[保持当前] 不更改会话，走上面「本次会话实际会走」的那条路径（%1）。")
      .arg(assessment.tier1_possible ? QStringLiteral("Tier 1")
                                     : (assessment.tier2_possible ? QStringLiteral("Tier 2")
                                                                  : QStringLiteral("Tier 3")));

  return actions;
}

QString GpuSessionAdvisor::DescribeAdvice()
{
  const Assessment assessment = Assess();

  QStringList blocks;
  blocks << DescribeSituation(assessment);
  blocks << QStringLiteral("本次会话实际会走：\n  ") + DescribeEffectivePath(assessment);
  blocks << QStringLiteral("可选的改进：\n  ") + RecommendedActions(assessment).join(QStringLiteral("\n  "));
  return blocks.join(QStringLiteral("\n\n"));
}

} // namespace olive
