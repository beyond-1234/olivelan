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
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include "common/footagerelink.h"
#include "node/nodeundo.h"
#include "undo/undostack.h"

namespace olive {

FootageRelinkDialog::FootageRelinkDialog(const QVector<Footage *> &footage, QWidget* parent,
                                         UndoStack* undo_stack) :
  QDialog(parent),
  file_list_(nullptr),
  status_label_(nullptr),
  browse_button_(nullptr),
  footage_(footage),
  undo_stack_(undo_stack)
{
  setWindowTitle(tr("Missing Media"));

  QVBoxLayout* layout = new QVBoxLayout(this);

  QLabel* description = new QLabel(
      tr("The following media files could not be found. Select Folder... scans the chosen "
         "folder and its subfolders, matching filenames and relative folder paths. Browse "
         "File... replaces only the selected item. A folder match overrides an individual "
         "selection; if the folder has no match, the individual selection is kept."));
  description->setWordWrap(true);
  layout->addWidget(description);

  file_list_ = new QTreeWidget();
  file_list_->setColumnCount(2);
  file_list_->setHeaderLabels({tr("Media"), tr("Status")});
  file_list_->setRootIsDecorated(false);
  file_list_->setSelectionMode(QAbstractItemView::SingleSelection);
  file_list_->setSelectionBehavior(QAbstractItemView::SelectRows);
  file_list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  file_list_->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  file_list_->header()->setStretchLastSection(false);
  layout->addWidget(file_list_, 1);

  status_label_ = new QLabel();
  status_label_->setWordWrap(true);
  layout->addWidget(status_label_);

  QDialogButtonBox* buttons = new QDialogButtonBox(this);
  QPushButton* select_folder_button = buttons->addButton(
      tr("Select Folder..."), QDialogButtonBox::ActionRole);
  select_folder_button->setDefault(true);
  browse_button_ = buttons->addButton(tr("Browse File..."), QDialogButtonBox::ActionRole);
  QPushButton* apply_button = buttons->addButton(
      tr("Apply"), QDialogButtonBox::AcceptRole);
  buttons->addButton(QDialogButtonBox::Cancel);

  connect(select_folder_button, &QPushButton::clicked,
          this, &FootageRelinkDialog::SelectFolder);
  connect(browse_button_, &QPushButton::clicked,
          this, &FootageRelinkDialog::BrowseFile);
  connect(apply_button, &QPushButton::clicked,
          this, &FootageRelinkDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &FootageRelinkDialog::reject);
  connect(file_list_, &QTreeWidget::itemSelectionChanged,
          this, &FootageRelinkDialog::UpdateButtons);
  connect(file_list_, &QTreeWidget::itemDoubleClicked,
          this, &FootageRelinkDialog::BrowseFile);
  layout->addWidget(buttons);

  for (Footage* f : footage_) {
    const QString original = f->filename();
    original_paths_.append(original);
    new_paths_.append(QFileInfo(original).isFile()
                          ? QFileInfo(original).absoluteFilePath()
                          : QString());
  }

  RefreshList();

  if (file_list_->topLevelItemCount() > 0) {
    file_list_->setCurrentItem(file_list_->topLevelItem(0));
  }

  resize(900, 500);
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

int FootageRelinkDialog::SelectedIndex() const
{
  QTreeWidgetItem* item = file_list_->currentItem();
  return item ? file_list_->indexOfTopLevelItem(item) : -1;
}

void FootageRelinkDialog::UpdateButtons()
{
  browse_button_->setEnabled(SelectedIndex() >= 0);
}

void FootageRelinkDialog::RefreshList()
{
  file_list_->clear();

  const int missing = MissingCount();

  for (int i = 0; i < footage_.size(); i++) {
    const QString original = original_paths_.at(i).isEmpty()
        ? tr("(unknown path)") : original_paths_.at(i);
    const bool found = IsFound(i);

    QTreeWidgetItem* item = new QTreeWidgetItem();
    item->setText(0, original);
    item->setToolTip(0, original);
    item->setText(1, found ? tr("Found") : tr("Missing"));
    item->setToolTip(1, found ? tr("Using: %1").arg(new_paths_.at(i)) : QString());
    file_list_->addTopLevelItem(item);
  }

  if (missing == 0) {
    status_label_->setText(tr("All %1 file(s) are online.").arg(footage_.size()));
  } else {
    status_label_->setText(
        tr("%1 of %2 file(s) are still missing. Use Select Folder... for a recursive "
           "match, or select an item and use Browse File....").arg(missing).arg(footage_.size()));
  }

  UpdateButtons();
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

  int batch_matches = 0;

  // Folder matches intentionally override existing individual selections.
  // If the folder has no match for an item, its individual selection is kept.
  for (int i = 0; i < footage_.size(); i++) {
    const QString match = search.Find(original_paths_.at(i));
    if (!match.isEmpty()) {
      new_paths_[i] = match;
      ++batch_matches;
    }
  }

  RefreshList();

  if (MissingCount() == 0) {
    accept();
    return;
  }

  if (batch_matches == 0) {
    QMessageBox::information(
        this, tr("Missing Media"),
        tr("No matching media files were found in \"%1\" or its subfolders.").arg(search_dir));
  }
}

void FootageRelinkDialog::BrowseFile()
{
  const int index = SelectedIndex();
  if (index < 0) {
    return;
  }

  QString start_dir;

  if (IsFound(index)) {
    start_dir = QFileInfo(new_paths_.at(index)).absolutePath();
  } else {
    QString normalized_original = original_paths_.at(index);
    normalized_original.replace(QLatin1Char('\\'), QLatin1Char('/'));
    start_dir = QFileInfo(normalized_original).absolutePath();
  }

  if (!QFileInfo(start_dir).isDir()) {
    start_dir = last_search_dir_;
  }
  if (!QFileInfo(start_dir).isDir()) {
    start_dir = QDir::homePath();
  }

  const QString file = QFileDialog::getOpenFileName(
      this, tr("Select Media File for \"%1\"").arg(footage_.at(index)->GetLabel()),
      start_dir, tr("All Files (*.*)"));
  if (file.isEmpty()) {
    return;
  }

  new_paths_[index] = QFileInfo(file).absoluteFilePath();
  RefreshList();

  if (index < file_list_->topLevelItemCount()) {
    file_list_->setCurrentItem(file_list_->topLevelItem(index));
  }

  if (MissingCount() == 0) {
    accept();
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
