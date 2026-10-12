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

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "codec/hal/hardware_pipeline_factory.h"

namespace olive {

HALBackendDialog::HALBackendDialog(QWidget* parent) :
  QDialog(parent)
{
  setWindowTitle(tr("HAL Backend Selection"));
  setMinimumWidth(560);

  auto* layout = new QVBoxLayout(this);

  layout->addWidget(new QLabel(tr("Detected hardware environment (GpuEnvironmentProbe):")));
  layout->addWidget(CreateProbeInfoSection());

  layout->addWidget(CreateSelectionSection());

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
    const int idx = backend_combo_->currentIndex();
    const QString data = backend_combo_->itemData(idx).toString();

    // Warn (but still allow) forcing a backend the probe considers unavailable.
    const QVariant avail = backend_combo_->itemData(idx, Qt::UserRole + 1);
    if (data != QStringLiteral("auto") && avail.isValid() && !avail.toBool()) {
      const auto answer = QMessageBox::warning(this, tr("Backend unavailable"),
          tr("The probe did not detect support for this backend on this system. "
             "Forcing it may fail at initialization and fall back to automatic routing. "
             "Continue anyway?"),
          QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
      if (answer != QMessageBox::Yes) {
        return;
      }
    }

    selected_backend_ = (data == QStringLiteral("auto")) ? QString() : data;
    accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  PopulateProbeInfo();
  PopulateBackendCombo();
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

  layout->addWidget(new QLabel(
      tr("The selection applies to this process only and is not saved. "
         "Automatic routing can also be forced without a dialog via -hal-backend <name> "
         "or the OLIVE_HAL_BACKEND environment variable.")));
  return widget;
}

void HALBackendDialog::PopulateProbeInfo()
{
  auto& probe = GpuEnvironmentProbe::Instance();
  probe.Refresh();

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
  add_item(backends, QStringLiteral("CUDA-NVDEC (Tier 1)"), probe.HasNvdecSupport() ? tr("Available") : tr("Unavailable"));
  add_item(backends, QStringLiteral("VideoToolbox (Tier 2, macOS)"), probe.SupportsVideoToolbox() ? tr("Available") : tr("Unavailable"));
  add_item(backends, QStringLiteral("VA-API DMA-BUF (Tier 2, Linux)"), probe.SupportsVaapiDmaBuf() ? tr("Available") : tr("Unavailable"));
  add_item(backends, QStringLiteral("D3D11 Interop (Tier 2, Windows)"), probe.SupportsD3D11() ? tr("Available") : tr("Unavailable"));
  add_item(backends, QStringLiteral("libplacebo (Plugin)"), probe.SupportsLibPlacebo() ? tr("Available") : tr("Unavailable"));
  add_item(backends, QStringLiteral("Reinforced Software (Tier 3)"), tr("Available"));

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
  auto& probe = GpuEnvironmentProbe::Instance();

  auto add_backend = [this](const QString& display, const QString& data, bool available, const QString& reason = QString()) {
    QString label = display;
    if (!available) {
      label += QStringLiteral(" (") + (reason.isEmpty() ? tr("detected unavailable") : reason) + QStringLiteral(")");
    }
    backend_combo_->addItem(label, data);
    const int idx = backend_combo_->count() - 1;
    backend_combo_->setItemData(idx, available, Qt::UserRole + 1);
  };

  backend_combo_->addItem(tr("Auto (dynamic probe)"), QStringLiteral("auto"));

#if defined(OLIVE_ENABLE_CUDA_NVDEC)
  add_backend(QStringLiteral("CUDA-NVDEC (Tier 1)"), QStringLiteral("cuda"), probe.HasNvdecSupport());
#endif
#if defined(__APPLE__)
  add_backend(QStringLiteral("VideoToolbox (Tier 2)"), QStringLiteral("videotoolbox"), probe.SupportsVideoToolbox());
#endif
#if defined(__linux__) && defined(OLIVE_ENABLE_VAAPI_DMABUF)
  add_backend(QStringLiteral("VA-API DMA-BUF (Tier 2)"), QStringLiteral("vaapi"), probe.SupportsVaapiDmaBuf());
#endif
#if defined(_WIN32) && defined(OLIVE_ENABLE_D3D11_INTEROP)
  add_backend(QStringLiteral("D3D11 Interop (Tier 2)"), QStringLiteral("d3d11"), probe.SupportsD3D11());
#endif
  add_backend(QStringLiteral("Reinforced Software (Tier 3)"), QStringLiteral("software"), true);
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
