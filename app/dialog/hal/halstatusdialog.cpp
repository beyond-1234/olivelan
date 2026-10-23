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
#include <QTreeWidget>
#include <QVBoxLayout>

#include "codec/hal/gpu_session_advisor.h"
#include "codec/hal/hardware_pipeline_factory.h"

namespace olive {

HALStatusDialog::HALStatusDialog(QWidget* parent) :
  QDialog(parent)
{
  setWindowTitle(tr("HAL Status"));
  setMinimumSize(760, 380);

  // Do not Refresh() here: this panel can be opened during playback, while decoder threads are
  // reading the probe. The probe is populated at startup and its runtime fields (VA-API import
  // status) update atomically; re-running device detection from the GUI would race those readers.
  const GpuSessionAdvisor::Assessment assessment = GpuSessionAdvisor::Assess();

  auto* layout = new QVBoxLayout(this);

  layout->addWidget(new QLabel(
      tr("Decode path status for this session. Reasons come from the live GPU probe; "
         "Tier 2 is verified per stream and Tier 3 is always the final fallback."),
      this));

  auto* tiers = new QTreeWidget(this);
  tiers->setColumnCount(3);
  tiers->setHeaderLabels({tr("Tier"), tr("Status"), tr("Reason")});
  tiers->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  tiers->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  tiers->header()->setStretchLastSection(true);
  tiers->setRootIsDecorated(false);

  const QVector<GpuSessionAdvisor::TierStatus> statuses = GpuSessionAdvisor::DescribeTiers(assessment);
  for (const GpuSessionAdvisor::TierStatus& status : statuses) {
    auto* item = new QTreeWidgetItem(tiers);
    item->setText(0, tr("Tier %1 - %2").arg(status.tier).arg(status.backend));
    item->setText(1, status.state);
    item->setText(2, status.reason);
    item->setToolTip(2, status.reason);

    if (!status.available) {
      QPalette palette = tiers->palette();
      palette.setColor(QPalette::Text, QColor(190, 90, 90));
      item->setForeground(1, palette.text());
    }
  }
  tiers->resizeColumnToContents(0);
  layout->addWidget(tiers);

  auto* expected = new QLabel(
      tr("Expected routing: %1").arg(GpuSessionAdvisor::DescribeEffectivePath(assessment)),
      this);
  expected->setWordWrap(true);
  layout->addWidget(expected);

  const QString forced = HardwarePipelineFactory::GetForcedBackend();
  auto* forced_label = new QLabel(
      forced.isEmpty() || forced == QStringLiteral("auto")
          ? tr("Forced backend: automatic routing")
          : tr("Forced backend: %1 (this process only)").arg(forced),
      this);
  forced_label->setWordWrap(true);
  layout->addWidget(forced_label);

  const int selected_tier = HardwarePipelineFactory::GetLastSelectedTier();
  const QString selected_backend = HardwarePipelineFactory::GetLastSelectedBackend();
  auto* selected_label = new QLabel(
      selected_tier > 0
          ? tr("Last frame selected: Tier %1 - %2").arg(selected_tier).arg(selected_backend)
          : tr("Last frame selected: no frame has selected a backend yet."),
      this);
  selected_label->setWordWrap(true);
  layout->addWidget(selected_label);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);
}

} // namespace olive
