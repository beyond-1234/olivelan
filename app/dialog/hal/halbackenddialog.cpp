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

#include "halbackenddialog.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "codec/hal/gpu_session_advisor.h"
#include "codec/hal/hardware_pipeline_factory.h"
#include "config/config.h"

namespace olive {

HALBackendDialog::HALBackendDialog(QWidget* parent) :
  QDialog(parent)
{
  setWindowTitle(tr("HAL Backend Selection"));
  setMinimumWidth(560);

  // Patch 0079/0080 (Phase 9): probe *before* building any UI. This dialog runs before the main
  // window exists, so nothing has reported a GL vendor yet, and both the availability list and the
  // "what this session will actually use" text depend on it - a pure-NVIDIA desktop would otherwise
  // be told Tier 1 is unavailable simply because the vendor was still unknown.
  {
    auto& probe = GpuEnvironmentProbe::Instance();
    probe.Refresh();
    probe.ProbePlatformOpenGlVendor();
  }

  auto* layout = new QVBoxLayout(this);

  layout->addWidget(new QLabel(tr("Detected hardware environment (GpuEnvironmentProbe):")));
  layout->addWidget(CreateProbeInfoSection());

  layout->addWidget(CreateSelectionSection());

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
    const int idx = backend_combo_->currentIndex();
    QString backend_id = backend_combo_->itemData(idx).toString();

    // Patch 0080 (Phase 9): when the requested backend cannot work, offer the alternatives that
    // will rather than a yes/no "continue anyway?" - that confirmed nothing and named no fix.
    const QVariant avail = backend_combo_->itemData(idx, Qt::UserRole + 1);
    if (backend_id != QStringLiteral("auto") && avail.isValid() && !avail.toBool()) {
      const QString resolved = ResolveUnavailableBackend(backend_id);
      if (resolved.isEmpty()) {
        return;
      }
      backend_id = resolved;
    }

    selected_backend_ = (backend_id == QStringLiteral("auto")) ? QString() : backend_id;
    accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  PopulateProbeInfo();
  PopulateBackendCombo();

  // Patch 0090 (P2-1): the label used to be computed once and then ignored the combo. Keep it in
  // sync with the actual selection so "will actually use" cannot name Tier 2 while the user has
  // selected Tier 3 (or a forced backend that is known to be unavailable).
  connect(backend_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int) { UpdateEffectivePathLabel(); });
  UpdateEffectivePathLabel();
}

QWidget* HALBackendDialog::CreateProbeInfoSection()
{
  probe_tree_ = new QTreeWidget(this);
  probe_tree_->setColumnCount(2);
  probe_tree_->setHeaderLabels({tr("Item"), tr("Value")});
  probe_tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  probe_tree_->header()->setStretchLastSection(true);
  probe_tree_->setRootIsDecorated(true);
  return probe_tree_;
}

QWidget* HALBackendDialog::CreateSelectionSection()
{
  auto* widget = new QWidget(this);
  auto* layout = new QVBoxLayout(widget);
  layout->setContentsMargins(0, 0, 0, 0);

  layout->addWidget(new QLabel(tr("Backend to use for this session:")));

  backend_combo_ = new QComboBox(widget);
  layout->addWidget(backend_combo_);

  // Patch 0080 (Phase 9): state what this session will actually use, derived from the probe, rather
  // than leaving the user to infer it from a list of backend names. Patch 0090 (P2-1): the text is
  // filled by UpdateEffectivePathLabel() once the combo exists, and follows its current selection.
  effective_label_ = new QLabel(widget);
  effective_label_->setWordWrap(true);
  layout->addWidget(effective_label_);

  layout->addWidget(new QLabel(
      tr("The selection applies to this process only and is not saved. "
         "Automatic routing can also be forced without a dialog via -hal-backend <name> "
         "or the OLIVE_HAL_BACKEND environment variable.")));
  return widget;
}

QString HALBackendDialog::ResolveUnavailableBackend(const QString& requested)
{
  auto& probe = GpuEnvironmentProbe::Instance();
  const bool interop_blocked = probe.IsCudaGlInteropBlocked();

  QMessageBox box(this);
  box.setIcon(QMessageBox::Warning);
  box.setWindowTitle(tr("Backend unavailable"));

  if (requested == QStringLiteral("cuda") && interop_blocked) {
    box.setText(tr("CUDA-NVDEC (Tier 1) cannot work in this session: CUDA-GL interop requires the "
                   "OpenGL context and CUDA to be on the same NVIDIA GPU, but the current OpenGL "
                   "context is on \"%1\". The frame pool will fail to register its textures and "
                   "this stream will produce no picture.")
                    .arg(QString::fromStdString(probe.GetOpenGlVendor())));
  } else {
    box.setText(tr("The probe reports that backend \"%1\" is unavailable on this system.")
                    .arg(requested));
  }

  box.setInformativeText(GpuSessionAdvisor::DescribeAdvice());

  // Offer what will actually work, best first.
  const bool tier2_usable = probe.SupportsVaapiDmaBuf();
  QPushButton* use_best_tier = box.addButton(
      tier2_usable ? tr("Use Tier 2 (VA-API hardware decode)") : tr("Use Tier 3 (CPU decode)"),
      QMessageBox::AcceptRole);

  const GpuSessionAdvisor::Assessment assessment = GpuSessionAdvisor::Assess();
  QPushButton* switch_session = nullptr;
  if (assessment.wayland_session && assessment.xwayland_available && assessment.nvidia_present) {
    // No "(next start)" suffix: picking this restarts Olive straight into the new session (0082).
    switch_session = box.addButton(tr("Switch to Xwayland + PRIME"), QMessageBox::ActionRole);
  }

  QPushButton* force = box.addButton(tr("Force %1 anyway").arg(requested),
                                     QMessageBox::DestructiveRole);
  box.addButton(QMessageBox::Cancel);
  box.exec();

  if (box.clickedButton() == use_best_tier) {
    return tier2_usable ? QStringLiteral("vaapi") : QStringLiteral("software");
  }

  if (switch_session && box.clickedButton() == switch_session) {
    // Patch 0082 (Phase 9): restart straight into the new session rather than saving and asking the
    // user to do it themselves. Closing the dialog lets main() see RestartLaunched() and exit.
    if (!GpuSessionAdvisor::RestartWithSession(GpuSessionAdvisor::Mode::XwaylandPrime)) {
      QMessageBox::warning(this, tr("Could not restart"),
          tr("The session preference was saved, but Olive could not restart itself. "
             "It will take effect the next time you start Olive."),
          QMessageBox::Ok);
    } else {
      reject();
    }
    return QString();
  }

  if (box.clickedButton() == force) {
    return requested;
  }

  return QString();
}

void HALBackendDialog::PopulateProbeInfo()
{
  auto& probe = GpuEnvironmentProbe::Instance();

  auto add_item = [this](QTreeWidgetItem* parent, const QString& key, const QString& value) {
    auto* item = new QTreeWidgetItem(parent);
    item->setText(0, key);
    item->setText(1, value);
    return item;
  };

  auto* env = new QTreeWidgetItem(probe_tree_, {tr("Environment")});
  add_item(env, tr("Primary GPU"), QString::fromStdString(probe.GetPrimaryGpuName()));
  add_item(env, tr("Vendor"), VendorToString(probe.GetPrimaryVendor()));
  add_item(env, tr("Total VRAM"), tr("%1 MB").arg(probe.GetTotalVramMb()));
  add_item(env, tr("Display server"), DisplayServerToString(probe.GetDisplayServer()));
  add_item(env, tr("Hybrid topology"), probe.IsHybridTopology() ? tr("Yes") : tr("No"));

  auto* devices = new QTreeWidgetItem(probe_tree_, {tr("Detected devices")});
  for (const auto& dev : probe.GetDetectedDevices()) {
    auto* dev_item = add_item(devices, QString::fromStdString(dev.device_name),
                              tr("%1 MB VRAM%2").arg(dev.vram_total_bytes / (1024 * 1024))
                                  .arg(dev.is_primary_display ? tr(", primary display") : QString()));
    add_item(dev_item, tr("Vendor"), VendorToString(dev.vendor));
    if (!dev.driver_version.empty()) {
      add_item(dev_item, tr("Driver"), QString::fromStdString(dev.driver_version));
    }
  }

  auto* backends = new QTreeWidgetItem(probe_tree_, {tr("Backend availability")});
  add_item(backends, QStringLiteral("CUDA-NVDEC (Tier 1)"),
           probe.IsCudaGlInteropBlocked()
               ? tr("Unavailable - the OpenGL context is not on the NVIDIA GPU")
               : (probe.HasNvdecSupport() ? tr("Available") : tr("Unavailable")));
  add_item(backends, QStringLiteral("VideoToolbox (Tier 2, macOS)"), probe.SupportsVideoToolbox() ? tr("Available") : tr("Unavailable"));
  QString vaapi_status;
  if (!probe.SupportsVaapiDmaBuf()) {
    vaapi_status = tr("Unavailable");
  } else {
    switch (probe.GetVaapiImportStatus()) {
    case VaapiImportStatus::Unknown:
      vaapi_status = tr("Device available - import not yet verified");
      break;
    case VaapiImportStatus::Verified:
      vaapi_status = tr("Available - EGL import verified");
      break;
    case VaapiImportStatus::Failed:
      vaapi_status = tr("Degraded - import failed for at least one stream; that stream used CPU readback");
      break;
    case VaapiImportStatus::Mixed:
      vaapi_status = tr("Partial - some streams verified, others read back");
      break;
    }
  }
  add_item(backends, QStringLiteral("VA-API DMA-BUF (Tier 2, Linux)"), vaapi_status);
  add_item(backends, QStringLiteral("D3D11 Interop (Tier 2, Windows)"), probe.SupportsD3D11() ? tr("Available") : tr("Unavailable"));
  add_item(backends, QStringLiteral("Reinforced CPU Decode + Native YUV Upload (Tier 3)"), tr("Available"));

  auto* cap = new QTreeWidgetItem(probe_tree_, {tr("Decode capability matrix")});
  add_item(cap, tr("H.264"), probe.SupportsStream(AV_CODEC_ID_H264, AV_PIX_FMT_YUV420P) ? tr("Yes") : tr("No"));
  add_item(cap, tr("HEVC 8-bit"), probe.SupportsStream(AV_CODEC_ID_HEVC, AV_PIX_FMT_YUV420P) ? tr("Yes") : tr("No"));
  add_item(cap, tr("HEVC 10-bit"), probe.SupportsStream(AV_CODEC_ID_HEVC, AV_PIX_FMT_YUV420P10LE) ? tr("Yes") : tr("No"));
  add_item(cap, tr("VP9"), probe.SupportsStream(AV_CODEC_ID_VP9, AV_PIX_FMT_YUV420P) ? tr("Yes") : tr("No"));
  add_item(cap, tr("AV1 8-bit"), probe.SupportsStream(AV_CODEC_ID_AV1, AV_PIX_FMT_YUV420P) ? tr("Yes") : tr("No"));
  add_item(cap, tr("AV1 10-bit"), probe.SupportsStream(AV_CODEC_ID_AV1, AV_PIX_FMT_YUV420P10LE) ? tr("Yes") : tr("No"));

  probe_tree_->expandAll();
}

void HALBackendDialog::PopulateBackendCombo()
{
  // Only referenced by the per-backend #if blocks below; on a build with no
  // hardware backend compiled in (e.g. CI, or a GPU-less container) none of those
  // blocks exist and the probe would otherwise be flagged unused.
  [[maybe_unused]] auto& probe = GpuEnvironmentProbe::Instance();

  auto add_backend = [this](const QString& display, const QString& backend_id, bool available, const QString& reason = QString(), const QString& detail = QString()) {
    QString label = display;
    if (!detail.isEmpty()) {
      label += QStringLiteral(" (") + detail + QStringLiteral(")");
    }
    if (!available) {
      label += QStringLiteral(" (") + (reason.isEmpty() ? tr("detected unavailable") : reason) + QStringLiteral(")");
    }
    backend_combo_->addItem(label, backend_id);
    const int idx = backend_combo_->count() - 1;
    backend_combo_->setItemData(idx, available, Qt::UserRole + 1);
    // Clean name without the status suffix, for the effective-path sentence.
    backend_combo_->setItemData(idx, display, Qt::UserRole + 2);
  };

  backend_combo_->addItem(tr("Auto (dynamic probe)"), QStringLiteral("auto"));
  backend_combo_->setItemData(backend_combo_->count() - 1, tr("Auto"), Qt::UserRole + 2);

#if defined(OLIVE_ENABLE_CUDA_NVDEC)
  // Patch 0079 (Phase 9): NVDEC being present is not enough - Tier 1 also needs the OpenGL context
  // on the same NVIDIA GPU, which is exactly what a hybrid Wayland session cannot provide.
  if (probe.IsCudaGlInteropBlocked()) {
    add_backend(QStringLiteral("CUDA-NVDEC (Tier 1)"), QStringLiteral("cuda"), false,
                tr("the OpenGL context is not on the NVIDIA GPU"));
    // Logged so a "why is Tier 1 greyed out?" report is answerable without a screenshot.
    qInfo() << "[HAL Dialog] CUDA-NVDEC listed as unavailable: the OpenGL context is on"
            << QString::fromStdString(probe.GetOpenGlVendor())
            << "and CUDA-GL interop requires it on the NVIDIA GPU.";
  } else {
    add_backend(QStringLiteral("CUDA-NVDEC (Tier 1)"), QStringLiteral("cuda"), probe.HasNvdecSupport());
  }
#endif
#if defined(__APPLE__)
  add_backend(QStringLiteral("VideoToolbox (Tier 2)"), QStringLiteral("videotoolbox"), probe.SupportsVideoToolbox());
#endif
#if defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
  {
    const bool device_available = probe.SupportsVaapiDmaBuf();
    bool available = device_available;
    QString reason;
    QString detail;

    if (!device_available) {
      reason = tr("VA-API device could not be created");
    } else {
      switch (probe.GetVaapiImportStatus()) {
      case VaapiImportStatus::Unknown:
        detail = tr("device present; import not yet verified");
        break;
      case VaapiImportStatus::Verified:
        detail = tr("EGL import verified");
        break;
      case VaapiImportStatus::Failed:
        // Routing is per-stream: a 10-bit failure must not hide a working 8-bit path.
        detail = tr("at least one stream fell back to CPU readback");
        break;
      case VaapiImportStatus::Mixed:
        detail = tr("some streams verified; others read back");
        break;
      }
    }

    add_backend(QStringLiteral("VA-API DMA-BUF (Tier 2)"), QStringLiteral("vaapi"), available,
                reason, detail);
  }
#endif
  // Patch 0049 (Phase 8): D3D11 interop is a placeholder - shown on Windows as a
  // greyed-out "Planned" entry so the roadmap is visible, but never selectable and
  // never routed to (SupportsD3D11() answers false everywhere).
#if defined(_WIN32)
  add_backend(QStringLiteral("D3D11 Interop (Tier 2)"), QStringLiteral("d3d11"),
              probe.SupportsD3D11(), tr("Planned - not implemented; Tier 3 is used instead"));
#endif
  add_backend(QStringLiteral("Reinforced CPU Decode + Native YUV Upload (Tier 3)"), QStringLiteral("software"), true);
}

void HALBackendDialog::UpdateEffectivePathLabel()
{
  if (!effective_label_ || !backend_combo_) {
    return;
  }

  const int idx = backend_combo_->currentIndex();
  const QString backend_id = backend_combo_->itemData(idx).toString();

  if (backend_id.isEmpty() || backend_id == QStringLiteral("auto")) {
    effective_label_->setText(
        tr("Will actually use: %1").arg(
            GpuSessionAdvisor::DescribeEffectivePath(GpuSessionAdvisor::Assess())));
    return;
  }

  const QString display = backend_combo_->itemData(idx, Qt::UserRole + 2).toString();
  const QVariant available = backend_combo_->itemData(idx, Qt::UserRole + 1);

  QString text = tr("Will attempt: %1 (forced for this process only). If it fails to initialize, "
                    "Olive falls back to automatic routing.")
                     .arg(display);

  if (available.isValid() && !available.toBool()) {
    text += QStringLiteral(" ") +
            tr("The probe marks it unavailable here, so OK will offer a working alternative first.");
  }

  effective_label_->setText(text);
}

QString HALBackendDialog::VendorToString(GpuVendor vendor)
{
  switch (vendor) {
  case GpuVendor::Nvidia: return QStringLiteral("NVIDIA");
  case GpuVendor::Amd: return QStringLiteral("AMD");
  case GpuVendor::Intel: return QStringLiteral("Intel");
  case GpuVendor::Apple: return QStringLiteral("Apple");
  default: return QObject::tr("Unknown");
  }
}

QString HALBackendDialog::DisplayServerToString(DisplayServer server)
{
  switch (server) {
  case DisplayServer::Wayland: return QStringLiteral("Wayland");
  case DisplayServer::X11: return QStringLiteral("X11");
  case DisplayServer::WindowsDWM: return QStringLiteral("Windows DWM");
  case DisplayServer::Cocoa: return QStringLiteral("macOS Cocoa");
  case DisplayServer::Headless: return QStringLiteral("Headless");
  default: return QObject::tr("Unknown");
  }
}

} // namespace olive
