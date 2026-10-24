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

#ifndef MAINSTATUSBAR_H
#define MAINSTATUSBAR_H

#include <QProgressBar>
#include <QStatusBar>

#include "task/taskmanager.h"

class QLabel;

namespace olive {

/**
 * @brief Shows abbreviated information from a TaskManager object
 */
class MainStatusBar : public QStatusBar
{
  Q_OBJECT
public:
  MainStatusBar(QWidget* parent = nullptr);

  void ConnectTaskManager(TaskManager* manager);

  /**
   * @brief Show the sticky render-thread failure state.
   *
   * Unlike showMessage(), this is a permanent widget: the viewer can be blank for the rest
   * of the session, so a 15-second transient message would disappear and leave no clue.
   * The full reason is available as the tooltip; the visible text stays short enough for
   * the status bar.
   */
  void SetRendererFailure(const QString& reason);

  void ClearRendererFailure();

  void SetColorConfigFailure(const QString& filename, const QString& reason);

  void ClearColorConfigFailure();

signals:
  void DoubleClicked();

protected:
  virtual void mouseDoubleClickEvent(QMouseEvent* e) override;

private slots:
  void UpdateStatus();

  void SetProgressBarValue(double d);

  void ConnectedTaskDeleted();

private:
  TaskManager* manager_;

  QProgressBar* bar_;

  QLabel* renderer_failure_;

  QLabel* color_config_failure_;

  Task* connected_task_;

};

}

#endif // MAINSTATUSBAR_H
