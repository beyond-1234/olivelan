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

#ifndef FOOTAGERELINK_H
#define FOOTAGERELINK_H

#include <QHash>
#include <QString>
#include <QStringList>

namespace olive {

/**
 * @brief Index of files under a directory used to resolve missing footage.
 *
 * Media is frequently moved together with its project, or a project is moved
 * between machines with a different root (for example /home/user vs
 * /var/home/user). In those cases the absolute filename stored in the project
 * is stale, but the filename and its trailing directory components still
 * identify the file. This class builds one recursive index per search root and
 * resolves original paths by the longest matching path suffix.
 */
class FootageRelinkSearch
{
public:
  explicit FootageRelinkSearch(const QString &search_root);

  bool IsValid() const { return valid_; }

  int FileCount() const { return file_count_; }

  /**
   * @brief Find the best replacement for an original filename.
   *
   * Returns an empty string when the file is absent, when several files share
   * the same trailing path (ambiguous), or when the search root is invalid.
   * A `C:\` prefix and both slash directions are accepted.
   */
  QString Find(const QString &original_path) const;

private:
  QString root_;
  QHash<QString, QStringList> files_by_name_;
  int file_count_ = 0;
  bool valid_ = false;
};

}

#endif // FOOTAGERELINK_H
