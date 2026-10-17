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

#ifndef GPUSESSIONDIALOG_H
#define GPUSESSIONDIALOG_H

#include <QDialog>

#include "codec/hal/gpu_session_advisor.h"

class QCheckBox;
class QRadioButton;
class QTreeWidget;

namespace olive {

/**
 * @brief Patch 0078 (Phase 9): asks which display session to use.
 *
 * Shown at startup whenever the choice is meaningful (hybrid GPU + Wayland). The QPA platform can
 * only be chosen before QApplication exists, so the selection is persisted and applied on the next
 * launch rather than immediately.
 */
class GpuSessionDialog : public QDialog {
  Q_OBJECT
public:
  GpuSessionDialog(QWidget* parent = nullptr);

private:
  QWidget* CreateProbeInfoSection();
  QWidget* CreateSelectionSection();
  QWidget* CreateAdviceSection();

  void PopulateProbeInfo();

  QTreeWidget* probe_tree_ = nullptr;
  QRadioButton* native_radio_ = nullptr;
  QRadioButton* xwayland_radio_ = nullptr;
  QCheckBox* dont_show_again_ = nullptr;
};

} // namespace olive

#endif // GPUSESSIONDIALOG_H
