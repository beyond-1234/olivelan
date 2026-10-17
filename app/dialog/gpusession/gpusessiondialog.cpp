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

#include "gpusessiondialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QRadioButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "config/config.h"
#include "codec/hal/gpu_environment_probe.h"

namespace olive {

GpuSessionDialog::GpuSessionDialog(QWidget* parent) :
  QDialog(parent)
{
  setWindowTitle(tr("GPU Session Selection"));
  setMinimumWidth(620);

  // Patch 0090 (P2-1): take one probe snapshot before building *any* text. Previously the advice
  // block was constructed first and PopulateProbeInfo() refreshed the probe afterwards, so the
  // recommendation and the probe tree in the same dialog could describe two different machines.
  GpuEnvironmentProbe::Instance().Refresh();

  auto* layout = new QVBoxLayout(this);

  layout->addWidget(new QLabel(tr("Detected display session and GPU topology:")));
  layout->addWidget(CreateProbeInfoSection());

  layout->addWidget(CreateSelectionSection());
  layout->addWidget(CreateAdviceSection());

  dont_show_again_ = new QCheckBox(tr("Don't ask again"), this);
  layout->addWidget(dont_show_again_);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
    const auto mode = xwayland_radio_->isChecked() ? GpuSessionAdvisor::Mode::XwaylandPrime
                                                   : GpuSessionAdvisor::Mode::NativeSession;

    if (dont_show_again_->isChecked()) {
      OLIVE_CONFIG("GpuSessionPromptDisabled") = true;
      Config::Save();
    }

    if (mode == GpuSessionAdvisor::ConfiguredMode()) {
      // Nothing to change, so nothing to restart for.
      accept();
      return;
    }

    // Patch 0082 (Phase 9): a session change needs a new process, because the QPA platform is
    // bound when QApplication is constructed. Restart into it now rather than leaving the user to
    // do it by hand.
    if (!GpuSessionAdvisor::RestartWithSession(mode)) {
      QMessageBox::warning(this, tr("Could not restart"),
          tr("The session preference was saved, but Olive could not restart itself. "
             "It will take effect the next time you start Olive."),
          QMessageBox::Ok);
    }

    accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  PopulateProbeInfo();
}

QWidget* GpuSessionDialog::CreateProbeInfoSection()
{
  probe_tree_ = new QTreeWidget(this);
  probe_tree_->setColumnCount(2);
  probe_tree_->setHeaderLabels({tr("Item"), tr("Value")});
  probe_tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  probe_tree_->header()->setStretchLastSection(true);
  probe_tree_->setRootIsDecorated(false);
  return probe_tree_;
}

QWidget* GpuSessionDialog::CreateSelectionSection()
{
  // The choice is applied by restarting immediately (patch 0082), so don't label it "next start".
  auto* group = new QGroupBox(tr("Display session"), this);
  auto* layout = new QVBoxLayout(group);

  native_radio_ = new QRadioButton(tr("Native session (recommended default)"), group);
  xwayland_radio_ = new QRadioButton(tr("Xwayland + PRIME offload (enables Tier 1 zero-copy)"),
                                     group);
  layout->addWidget(native_radio_);
  layout->addWidget(xwayland_radio_);

  if (GpuSessionAdvisor::ConfiguredMode() == GpuSessionAdvisor::Mode::XwaylandPrime) {
    xwayland_radio_->setChecked(true);
  } else {
    native_radio_->setChecked(true);
  }

  // Xwayland can't be selected when it isn't reachable - forcing xcb there would stop Olive from
  // starting at all.
  if (!GpuSessionAdvisor::IsXwaylandAvailable()) {
    xwayland_radio_->setEnabled(false);
    xwayland_radio_->setToolTip(tr("No reachable X display was found on this system."));
    native_radio_->setChecked(true);
  }

  return group;
}

QWidget* GpuSessionDialog::CreateAdviceSection()
{
  auto* group = new QGroupBox(tr("Recommendation"), this);
  auto* layout = new QVBoxLayout(group);

  auto* label = new QLabel(GpuSessionAdvisor::DescribeAdvice(), group);
  label->setWordWrap(true);
  layout->addWidget(label);

  return group;
}

void GpuSessionDialog::PopulateProbeInfo()
{
  auto& probe = GpuEnvironmentProbe::Instance();

  // The probe was refreshed once in the constructor, before the advice text was built. Do not
  // refresh again here: that would make the tree newer than the recommendation it explains
  // (patch 0090 / P2-1).

  auto add_item = [this](QTreeWidgetItem* parent, const QString& key, const QString& value) {
    auto* item = new QTreeWidgetItem(parent);
    item->setText(0, key);
    item->setText(1, value);
    return item;
  };

  auto* session = new QTreeWidgetItem(probe_tree_, {tr("Session")});
  add_item(session, tr("Wayland session"),
           GpuSessionAdvisor::IsWaylandSession() ? tr("Yes") : tr("No"));
  add_item(session, tr("Xwayland reachable"),
           GpuSessionAdvisor::IsXwaylandAvailable() ? tr("Yes") : tr("No"));
  add_item(session, tr("Configured mode"),
           GpuSessionAdvisor::ConfiguredMode() == GpuSessionAdvisor::Mode::XwaylandPrime
               ? tr("Xwayland + PRIME")
               : tr("Native"));

  auto* gpu = new QTreeWidgetItem(probe_tree_, {tr("GPU")});
  add_item(gpu, tr("Primary GPU"), QString::fromStdString(probe.GetPrimaryGpuName()));
  add_item(gpu, tr("Hybrid topology"), probe.IsHybridTopology() ? tr("Yes") : tr("No"));
  const std::string gl_vendor = probe.GetOpenGlVendor();
  add_item(gpu, tr("Active OpenGL vendor"),
           gl_vendor.empty() ? tr("Unknown (no live GL context yet)")
                             : QString::fromStdString(gl_vendor));

  auto* tiers = new QTreeWidgetItem(probe_tree_, {tr("Decode path on this session")});
  add_item(tiers, QStringLiteral("Tier 1 (CUDA-NVDEC + CUDA-GL interop)"),
           probe.IsCudaGlInteropBlocked()
               ? tr("Unavailable - the OpenGL context is not on the NVIDIA GPU")
               : (probe.HasNvdecSupport() ? tr("Available") : tr("Unavailable")));
  add_item(tiers, QStringLiteral("Tier 2 (VA-API DMA-BUF)"),
           probe.SupportsVaapiDmaBuf() ? tr("Available") : tr("Unavailable"));
  add_item(tiers, QStringLiteral("Tier 3 (Reinforced CPU decode)"), tr("Available"));

  probe_tree_->expandAll();
}

} // namespace olive
