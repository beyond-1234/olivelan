/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2026 Olive Team

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

#include "footagerelink.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>

namespace olive {

namespace {

const int kMaximumIndexedFiles = 200000;

QStringList SplitPath(QString path)
{
  path.replace(QLatin1Char('\\'), QLatin1Char('/'));

  QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);

  // A Windows drive letter is not a directory component.
  if (!parts.isEmpty() && parts.first().size() == 2 && parts.first().at(1) == QLatin1Char(':')) {
    parts.removeFirst();
  }

  return parts;
}

int TrailingComponentScore(const QStringList &original, const QStringList &candidate)
{
  int original_index = original.size() - 1;
  int candidate_index = candidate.size() - 1;
  int score = 0;

  while (original_index >= 0 && candidate_index >= 0) {
    if (original.at(original_index).compare(candidate.at(candidate_index), Qt::CaseInsensitive) != 0) {
      break;
    }

    ++score;
    --original_index;
    --candidate_index;
  }

  return score;
}

}

FootageRelinkSearch::FootageRelinkSearch(const QString &search_root)
{
  const QFileInfo root_info(search_root);

  if (!root_info.exists() || !root_info.isDir()) {
    return;
  }

  root_ = root_info.absoluteFilePath();
  valid_ = true;

  QDirIterator it(root_, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
  while (it.hasNext() && file_count_ < kMaximumIndexedFiles) {
    const QString path = it.next();
    files_by_name_[QFileInfo(path).fileName().toLower()].append(path);
    ++file_count_;
  }
}

QString FootageRelinkSearch::Find(const QString &original_path) const
{
  if (!valid_) {
    return QString();
  }

  QString normalized_path = original_path;
  normalized_path.replace(QLatin1Char('\\'), QLatin1Char('/'));

  const QString filename = QFileInfo(normalized_path).fileName();
  if (filename.isEmpty()) {
    return QString();
  }

  const QStringList candidates = files_by_name_.value(filename.toLower());
  if (candidates.isEmpty()) {
    return QString();
  }

  // A unique filename is always safe. With several candidates, require at
  // least one matching parent directory so we do not link the wrong clip.
  if (candidates.size() == 1) {
    return candidates.first();
  }

  const QStringList original_parts = SplitPath(original_path);
  QString best;
  int best_score = 0;
  bool tied = false;

  for (const QString &candidate : candidates) {
    const QStringList candidate_parts = SplitPath(QDir(root_).relativeFilePath(candidate));
    const int score = TrailingComponentScore(original_parts, candidate_parts);

    if (score > best_score) {
      best = candidate;
      best_score = score;
      tied = false;
    } else if (score == best_score && !best.isEmpty()) {
      tied = true;
    }
  }

  if (best.isEmpty() || tied || best_score < 2) {
    return QString();
  }

  return best;
}

}
