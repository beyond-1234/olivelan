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

#include "halstatusdialog.h"

#include <QColor>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "codec/hal/gpu_environment_probe.h"
#include "codec/hal/gpu_session_advisor.h"
#include "codec/hal/hardware_pipeline_factory.h"

namespace olive {

HALStatusDialog::HALStatusDialog(QWidget* parent) :
  QDialog(parent)
{
  setWindowTitle(tr("HAL Status"));
  setMinimumSize(760, 420);

  auto* layout = new QVBoxLayout(this);

  layout->addWidget(new QLabel(
      tr("Decode path status for this session. Reasons come from the live GPU probe; "
         "Tier 2 is verified per stream and Tier 3 is always the final fallback."),
      this));
  auto* device_note = new QLabel(
      tr("Device information is periodically snapshotted. Click Refresh to re-probe "
         "hardware topology (thread-safe); VA-API import status updates dynamically."),
      this);
  device_note->setWordWrap(true);
  layout->addWidget(device_note);

  tiers_ = new QTreeWidget(this);
  tiers_->setColumnCount(3);
  tiers_->setHeaderLabels({tr("Tier"), tr("Status"), tr("Reason")});
  tiers_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  tiers_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  tiers_->header()->setStretchLastSection(true);
  tiers_->setRootIsDecorated(false);
  layout->addWidget(tiers_);

  expected_ = new QLabel(this);
  expected_->setWordWrap(true);
  layout->addWidget(expected_);

  forced_label_ = new QLabel(this);
  forced_label_->setWordWrap(true);
  layout->addWidget(forced_label_);

  selected_label_ = new QLabel(this);
  selected_label_->setWordWrap(true);
  layout->addWidget(selected_label_);

  PopulateStatus();

  auto* buttons = new QDialogButtonBox(this);
  auto* refresh_btn = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
  buttons->addButton(QDialogButtonBox::Close);
  connect(refresh_btn, &QPushButton::clicked, this, &HALStatusDialog::PopulateStatus);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

void HALStatusDialog::PopulateStatus()
{
  if (!tiers_) {
    return;
  }

  // Probe refresh is protected by probe_mutex_
  GpuEnvironmentProbe::Instance().Refresh();

  const GpuSessionAdvisor::Assessment assessment = GpuSessionAdvisor::Assess();

  tiers_->clear();
  const QVector<GpuSessionAdvisor::TierStatus> statuses = GpuSessionAdvisor::DescribeTiers(assessment);
  for (const GpuSessionAdvisor::TierStatus& status : statuses) {
    auto* item = new QTreeWidgetItem(tiers_);
    item->setText(0, tr("Tier %1 - %2").arg(status.tier).arg(status.backend));
    item->setText(1, status.state);
    item->setText(2, status.reason);
    item->setToolTip(2, status.reason);

    if (!status.available) {
      QPalette palette = tiers_->palette();
      palette.setColor(QPalette::Text, QColor(190, 90, 90));
      item->setForeground(1, palette.text());
    }
  }
  tiers_->resizeColumnToContents(0);

  if (expected_) {
    expected_->setText(tr("Expected routing: %1").arg(GpuSessionAdvisor::DescribeEffectivePath(assessment)));
  }

  const QString forced = HardwarePipelineFactory::GetForcedBackend();
  if (forced_label_) {
    forced_label_->setText(
        forced.isEmpty() || forced == QStringLiteral("auto")
            ? tr("Forced backend: automatic routing")
            : tr("Forced backend: %1 (this process only)").arg(forced));
  }

  const int selected_tier = HardwarePipelineFactory::GetLastSelectedTier();
  const QString selected_backend = HardwarePipelineFactory::GetLastSelectedBackend();
  if (selected_label_) {
    selected_label_->setText(
        selected_tier > 0
            ? tr("Last frame selected: Tier %1 - %2").arg(selected_tier).arg(selected_backend)
            : tr("Last frame selected: no frame has selected a backend yet."));
  }
}

} // namespace olive
