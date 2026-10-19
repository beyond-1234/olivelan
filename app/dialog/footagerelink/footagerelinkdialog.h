/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2019  Olive Team

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

#ifndef FOOTAGERELINKDIALOG_H
#define FOOTAGERELINKDIALOG_H

#include <QDialog>
#include <QStringList>
#include <QVector>

#include "node/project/footage/footage.h"

class QLabel;
class QListWidget;

namespace olive {

class UndoStack;

class FootageRelinkDialog : public QDialog
{
  Q_OBJECT
public:
  /**
   * @brief Prompt for missing footage and let the user choose a media folder.
   *
   * When `undo_stack` is provided (the project is already open), accepted
   * changes are pushed as one undo command. During project load it is null and
   * the paths are applied directly.
   */
  FootageRelinkDialog(const QVector<Footage*>& footage, QWidget* parent = nullptr,
                      UndoStack* undo_stack = nullptr);

protected:
  virtual void accept() override;

private:
  void SelectFolder();
  void RefreshMissingList();
  bool IsFound(int index) const;
  int MissingCount() const;

  QListWidget* missing_list_;
  QLabel* status_label_;

  QVector<Footage*> footage_;
  QStringList original_paths_;
  QStringList new_paths_;
  QString last_search_dir_;
  UndoStack* undo_stack_;

};

}

#endif // FOOTAGERELINKDIALOG_H
