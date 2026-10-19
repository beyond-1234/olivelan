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

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QVBoxLayout>

#include "common/footagerelink.h"
#include "node/nodeundo.h"
#include "undo/undostack.h"

namespace olive {

FootageRelinkDialog::FootageRelinkDialog(const QVector<Footage *> &footage, QWidget* parent,
                                         UndoStack* undo_stack) :
  QDialog(parent),
  table_(nullptr),
  footage_(footage),
  undo_stack_(undo_stack)
{
  QVBoxLayout* layout = new QVBoxLayout(this);

  QLabel* description = new QLabel(
      tr("The following files could not be found. Use Browse for a single file, or "
         "Search Folder to match all missing files under a new media directory. "
         "Clips whose files stay missing will remain offline."));
  description->setWordWrap(true);
  layout->addWidget(description);

  table_ = new QTreeWidget();
  table_->setColumnCount(4);
  table_->setHeaderLabels({tr("Footage"), tr("Path"), tr("Status"), tr("Actions")});
  table_->setRootIsDecorated(false);
  table_->setSelectionBehavior(QAbstractItemView::SelectRows);
  table_->setWordWrap(false);
  table_->header()->setSectionsMovable(false);
  table_->header()->setSectionResizeMode(1, QHeaderView::Stretch);
  table_->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  table_->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
  table_->header()->setStretchLastSection(false);
  table_->setColumnWidth(0, 220);

  for (int i = 0; i < footage.size(); i++) {
    Footage* f = footage.at(i);
    const QString original = f->filename();
    const QString absolute = QFileInfo(original).isFile() ? QFileInfo(original).absoluteFilePath() : QString();

    original_paths_.append(original);
    new_paths_.append(absolute);

    QTreeWidgetItem* item = new QTreeWidgetItem();
    item->setIcon(0, f->data(Node::ICON).value<QIcon>());
    item->setText(0, f->GetLabel());

    QWidget* item_actions = new QWidget();
    QHBoxLayout* item_actions_layout = new QHBoxLayout(item_actions);
    item_actions_layout->setContentsMargins(0, 0, 0, 0);
    QPushButton* item_browse_btn = new QPushButton(tr("Browse..."));
    connect(item_browse_btn, &QPushButton::clicked, this, [this, i]() {
      BrowseForFootage(i);
    });
    item_actions_layout->addWidget(item_browse_btn);

    table_->addTopLevelItem(item);
    table_->setItemWidget(item, 3, item_actions);

    UpdateFootageItem(i);
  }

  connect(table_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
    const int index = table_->indexOfTopLevelItem(item);
    if (index >= 0) {
      BrowseForFootage(index);
    }
  });

  layout->addWidget(table_);

  QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                                   this);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Apply"));
  QPushButton* search_button = buttons->addButton(tr("Search Folder..."),
                                                  QDialogButtonBox::ActionRole);
  connect(search_button, &QPushButton::clicked, this, &FootageRelinkDialog::SearchFolder);
  connect(buttons, &QDialogButtonBox::accepted, this, &FootageRelinkDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &FootageRelinkDialog::reject);
  layout->addWidget(buttons);

  resize(980, 480);
  setWindowTitle(tr("Relink Footage"));
}

bool FootageRelinkDialog::IsFound(int index) const
{
  if (index < 0 || index >= new_paths_.size()) {
    return false;
  }

  const QString &path = new_paths_.at(index);
  return !path.isEmpty() && QFileInfo(path).isFile();
}

int FootageRelinkDialog::FirstUnresolved() const
{
  for (int i = 0; i < footage_.size(); i++) {
    if (!IsFound(i)) {
      return i;
    }
  }

  return -1;
}

void FootageRelinkDialog::UpdateFootageItem(int index)
{
  Footage* f = footage_.at(index);
  QTreeWidgetItem* item = table_->topLevelItem(index);
  if (!item) {
    return;
  }

  const QString &original = original_paths_.at(index);
  const QString &replacement = new_paths_.at(index);
  const bool found = IsFound(index);

  QString path_text = original;
  if (found && replacement != original) {
    path_text.append(QStringLiteral(" -> "));
    path_text.append(replacement);
  }

  item->setIcon(0, f->data(Node::ICON).value<QIcon>());
  item->setText(0, f->GetLabel());
  item->setText(1, path_text);
  item->setToolTip(1, path_text);
  item->setText(2, found ? tr("Found") : tr("Missing"));

  const QPalette palette = table_->palette();
  item->setForeground(2, found ? palette.color(QPalette::Text)
                              : palette.color(QPalette::Disabled, QPalette::Text));
  item->setToolTip(2, replacement);
}

void FootageRelinkDialog::BrowseForFootage(int index)
{
  if (index < 0 || index >= footage_.size()) {
    return;
  }

  QString normalized_original = original_paths_.at(index);
  normalized_original.replace(QLatin1Char('\\'), QLatin1Char('/'));

  QString start_dir = QFileInfo(normalized_original).absolutePath();
  if (!QFileInfo(start_dir).isDir()) {
    start_dir = last_search_dir_;
  }
  if (start_dir.isEmpty()) {
    start_dir = QDir::homePath();
  }

  const QString new_fn = QFileDialog::getOpenFileName(
      this, tr("Relink \"%1\"").arg(footage_.at(index)->GetLabel()), start_dir,
      tr("All Files (*.*)"));

  if (!new_fn.isEmpty()) {
    RelinkFromFile(index, new_fn);
  }
}

void FootageRelinkDialog::RelinkFromFile(int index, const QString &path)
{
  if (index < 0 || index >= footage_.size() || !QFileInfo(path).isFile()) {
    return;
  }

  new_paths_[index] = QFileInfo(path).absoluteFilePath();
  RelinkSiblingsFromFile(index, path);

  for (int i = 0; i < footage_.size(); i++) {
    UpdateFootageItem(i);
  }

  if (FirstUnresolved() == -1) {
    accept();
  }
}

void FootageRelinkDialog::RelinkSiblingsFromFile(int index, const QString &path)
{
  QString normalized_original = original_paths_.at(index);
  normalized_original.replace(QLatin1Char('\\'), QLatin1Char('/'));

  const QFileInfo original_info(normalized_original);
  const QDir original_dir = original_info.dir();
  const QDir new_dir = QFileInfo(path).dir();

  for (int i = 0; i < footage_.size(); i++) {
    if (i == index || IsFound(i)) {
      continue;
    }

    QString normalized_other = original_paths_.at(i);
    normalized_other.replace(QLatin1Char('\\'), QLatin1Char('/'));

    const QString relative_to_original = original_dir.relativeFilePath(normalized_other);
    QString candidate = QDir::isAbsolutePath(relative_to_original)
        ? relative_to_original
        : new_dir.filePath(relative_to_original);
    candidate = QDir::cleanPath(candidate);

    if (!QFileInfo(candidate).isFile()) {
      candidate = new_dir.filePath(QFileInfo(normalized_other).fileName());
    }

    if (QFileInfo(candidate).isFile()) {
      new_paths_[i] = QFileInfo(candidate).absoluteFilePath();
    }
  }
}

void FootageRelinkDialog::SearchFolder()
{
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
      this, tr("Search For Missing Footage"), start_dir);
  if (search_dir.isEmpty()) {
    return;
  }

  last_search_dir_ = search_dir;

  const FootageRelinkSearch search(search_dir);
  if (!search.IsValid()) {
    QMessageBox::warning(this, tr("Relink Footage"),
                         tr("The selected search folder cannot be read."));
    return;
  }

  int unresolved_before = 0;
  for (int i = 0; i < footage_.size(); i++) {
    if (!IsFound(i)) {
      ++unresolved_before;
      const QString match = search.Find(original_paths_.at(i));
      if (!match.isEmpty()) {
        new_paths_[i] = match;
      }
    }
    UpdateFootageItem(i);
  }

  const int unresolved_after = [this]() {
    int count = 0;
    for (int i = 0; i < footage_.size(); i++) {
      if (!IsFound(i)) {
        ++count;
      }
    }
    return count;
  }();

  if (unresolved_after == 0) {
    accept();
    return;
  }

  QMessageBox::information(
      this, tr("Relink Footage"),
      tr("Found %1 of %2 missing file(s) in \"%3\". Use Browse for the remaining ones.")
          .arg(unresolved_before - unresolved_after)
          .arg(unresolved_before)
          .arg(search_dir));
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
