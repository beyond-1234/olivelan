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
#include <QTreeWidget>

#include "node/project/footage/footage.h"

namespace olive {

class UndoStack;

class FootageRelinkDialog : public QDialog
{
  Q_OBJECT
public:
  /**
   * @brief Relink missing footage.
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
  void UpdateFootageItem(int index);
  void BrowseForFootage(int index);
  void SearchFolder();
  void RelinkFromFile(int index, const QString &path);
  void RelinkSiblingsFromFile(int index, const QString &path);
  bool IsFound(int index) const;
  int FirstUnresolved() const;

  QTreeWidget* table_;

  QVector<Footage*> footage_;
  QStringList original_paths_;
  QStringList new_paths_;
  QString last_search_dir_;
  UndoStack* undo_stack_;

};

}

#endif // FOOTAGERELINKDIALOG_H
