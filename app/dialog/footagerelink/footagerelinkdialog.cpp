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

#include "footagerelinkdialog.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "common/footagerelink.h"
#include "node/nodeundo.h"
#include "undo/undostack.h"

namespace olive {

FootageRelinkDialog::FootageRelinkDialog(const QVector<Footage *> &footage, QWidget* parent,
                                         UndoStack* undo_stack) :
  QDialog(parent),
  missing_list_(nullptr),
  status_label_(nullptr),
  footage_(footage),
  undo_stack_(undo_stack)
{
  setWindowTitle(tr("Missing Media"));

  QVBoxLayout* layout = new QVBoxLayout(this);

  QLabel* description = new QLabel(
      tr("The following media files could not be found. Select the folder that now contains "
         "them. Olive will scan that folder and all of its subfolders, matching files by "
         "filename and relative folder path."));
  description->setWordWrap(true);
  layout->addWidget(description);

  layout->addWidget(new QLabel(tr("Missing files:")));

  missing_list_ = new QListWidget();
  missing_list_->setSelectionMode(QAbstractItemView::NoSelection);
  layout->addWidget(missing_list_, 1);

  status_label_ = new QLabel();
  status_label_->setWordWrap(true);
  layout->addWidget(status_label_);

  QDialogButtonBox* buttons = new QDialogButtonBox(this);
  QPushButton* select_folder_button = buttons->addButton(
      tr("Select Folder..."), QDialogButtonBox::ActionRole);
  select_folder_button->setDefault(true);
  QPushButton* apply_button = buttons->addButton(
      tr("Apply"), QDialogButtonBox::AcceptRole);
  buttons->addButton(QDialogButtonBox::Cancel);

  connect(select_folder_button, &QPushButton::clicked,
          this, &FootageRelinkDialog::SelectFolder);
  connect(apply_button, &QPushButton::clicked,
          this, &FootageRelinkDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &FootageRelinkDialog::reject);
  layout->addWidget(buttons);

  for (Footage* f : footage_) {
    const QString original = f->filename();
    original_paths_.append(original);
    new_paths_.append(QFileInfo(original).isFile()
                          ? QFileInfo(original).absoluteFilePath()
                          : QString());
  }

  RefreshMissingList();

  resize(760, 460);
}

bool FootageRelinkDialog::IsFound(int index) const
{
  if (index < 0 || index >= new_paths_.size()) {
    return false;
  }

  const QString &path = new_paths_.at(index);
  return !path.isEmpty() && QFileInfo(path).isFile();
}

int FootageRelinkDialog::MissingCount() const
{
  int missing = 0;

  for (int i = 0; i < footage_.size(); i++) {
    if (!IsFound(i)) {
      ++missing;
    }
  }

  return missing;
}

void FootageRelinkDialog::RefreshMissingList()
{
  missing_list_->clear();

  const int missing = MissingCount();

  for (int i = 0; i < footage_.size(); i++) {
    if (!IsFound(i)) {
      const QString path = original_paths_.at(i).isEmpty()
          ? tr("(unknown path)") : original_paths_.at(i);
      QListWidgetItem* item = new QListWidgetItem(path);
      item->setToolTip(path);
      missing_list_->addItem(item);
    }
  }

  if (missing == 0) {
    status_label_->setText(tr("All %1 file(s) are online.").arg(footage_.size()));
  } else {
    status_label_->setText(
        tr("%1 of %2 file(s) are still missing. Select the folder that contains them, "
           "or apply with the remaining files offline.").arg(missing).arg(footage_.size()));
  }
}

void FootageRelinkDialog::SelectFolder()
{
  if (MissingCount() == 0) {
    accept();
    return;
  }

  QString start_dir = last_search_dir_;

  if (start_dir.isEmpty() && !original_paths_.isEmpty()) {
    QString normalized_original = original_paths_.first();
    normalized_original.replace(QLatin1Char('\\'), QLatin1Char('/'));
    start_dir = QFileInfo(normalized_original).absolutePath();
  }

  if (!QFileInfo(start_dir).isDir()) {
    start_dir = QDir::homePath();
  }

  const QString search_dir = QFileDialog::getExistingDirectory(
      this, tr("Select Media Folder"), start_dir);
  if (search_dir.isEmpty()) {
    return;
  }

  last_search_dir_ = search_dir;

  const FootageRelinkSearch search(search_dir);
  if (!search.IsValid()) {
    QMessageBox::warning(this, tr("Missing Media"),
                         tr("The selected folder cannot be read."));
    return;
  }

  const int missing_before = MissingCount();

  for (int i = 0; i < footage_.size(); i++) {
    if (IsFound(i)) {
      continue;
    }

    const QString match = search.Find(original_paths_.at(i));
    if (!match.isEmpty()) {
      new_paths_[i] = match;
    }
  }

  RefreshMissingList();

  if (MissingCount() == 0) {
    accept();
    return;
  }

  if (MissingCount() == missing_before) {
    QMessageBox::information(
        this, tr("Missing Media"),
        tr("No matching media files were found in \"%1\" or its subfolders.").arg(search_dir));
  }
}

void FootageRelinkDialog::accept()
{
  MultiUndoCommand* command = undo_stack_ ? new MultiUndoCommand() : nullptr;
  int changed = 0;

  for (int i = 0; i < footage_.size(); i++) {
    if (!IsFound(i) || new_paths_.at(i) == original_paths_.at(i)) {
      continue;
    }

    Footage* f = footage_.at(i);
    const QString &new_path = new_paths_.at(i);

    if (command) {
      command->add_child(new NodeParamSetStandardValueCommand(
          NodeKeyframeTrackReference(NodeInput(f, Footage::kFilenameInput)),
          new_path, original_paths_.at(i)));
    } else {
      f->set_filename(new_path);
    }

    ++changed;
  }

  if (command) {
    if (changed > 0) {
      undo_stack_->push(command, tr("Relinked Footage"));
    } else {
      delete command;
    }
  }

  QDialog::accept();
}

}
