/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2024 Olive Team

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

#include "projectexplorericonview.h"

#include "projectviewmodel.h"
#include "ui/style/designtokenregistry.h"

namespace olive {

ProjectExplorerIconView::ProjectExplorerIconView(QWidget *parent) :
  ProjectExplorerListViewBase(parent),
  thumbnail_cache_(this),
  delegate_(&thumbnail_cache_)
{
  setViewMode(QListView::IconMode);

  setItemDelegate(&delegate_);

  connect(&thumbnail_cache_, &ProjectThumbnailCache::Ready, this, &ProjectExplorerIconView::ThumbnailReady);
  connect(DesignTokenRegistry::instance(), &DesignTokenRegistry::TokensChanged, this,
          [this]() { viewport()->update(); });
}

void ProjectExplorerIconView::ThumbnailReady(const QString &key)
{
  // Repaint just the tile(s) that use this key; fall back to the whole viewport if none matched
  // (e.g. the item is currently in another folder).
  QAbstractItemModel *m = model();
  const QModelIndex parent = rootIndex();
  bool updated = false;

  if (m) {
    for (int i = 0; i < m->rowCount(parent); i++) {
      const QModelIndex index = m->index(i, ProjectViewModel::kName, parent);

      if (index.data(ProjectViewModel::kThumbnailKeyRole).toString() == key) {
        update(index);
        updated = true;
      }
    }
  }

  if (!updated) {
    viewport()->update();
  }
}

}
