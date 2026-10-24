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

#include "testutil.h"

#include "common/footagerelink.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

namespace olive {

namespace {

bool CreateEmptyFile(const QString &path)
{
  QDir().mkpath(QFileInfo(path).path());

  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write("test") == 4;
}

}

OLIVE_ADD_TEST(FootageRelinkSearchPaths)
{
  QTemporaryDir root;
  OLIVE_ASSERT(root.isValid());

  const QString root_path = root.path();
  const QString sub_clip = root_path + QStringLiteral("/Sub/Clip.MP4");
  const QString other_clip = root_path + QStringLiteral("/Other/Clip.MP4");
  const QString unique_clip = root_path + QStringLiteral("/Unique.mov");

  OLIVE_ASSERT(CreateEmptyFile(sub_clip));
  OLIVE_ASSERT(CreateEmptyFile(other_clip));
  OLIVE_ASSERT(CreateEmptyFile(unique_clip));

  const FootageRelinkSearch search(root_path);
  OLIVE_ASSERT(search.IsValid());
  OLIVE_ASSERT(search.FileCount() == 3);

  // The longest matching suffix must win when the filename is not unique.
  OLIVE_ASSERT(search.Find(QStringLiteral("/old/Sub/Clip.MP4")) == sub_clip);
  OLIVE_ASSERT(search.Find(QStringLiteral("/old/Other/Clip.MP4")) == other_clip);

  // A bare filename with two candidates is ambiguous and must not be linked.
  OLIVE_ASSERT(search.Find(QStringLiteral("/old/Clip.MP4")).isEmpty());

  // Filename matching is case-insensitive.
  OLIVE_ASSERT(search.Find(QStringLiteral("/old/unique.mov")) == unique_clip);

  // Windows separators/drive prefixes are accepted.
  OLIVE_ASSERT(search.Find(QStringLiteral("C:\\old\\Sub\\Clip.MP4")) == sub_clip);

  OLIVE_ASSERT(search.Find(QStringLiteral("/old/Missing.mov")).isEmpty());

  const FootageRelinkSearch invalid_search(root_path + QStringLiteral("/does-not-exist"));
  OLIVE_ASSERT(!invalid_search.IsValid());
  OLIVE_ASSERT(invalid_search.Find(unique_clip).isEmpty());

  OLIVE_TEST_END;
}

}
