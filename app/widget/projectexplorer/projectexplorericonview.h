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

#ifndef PROJECTEXPLORERICONVIEW_H
#define PROJECTEXPLORERICONVIEW_H

#include "projectexplorerlistviewbase.h"
#include "projectexplorericonviewitemdelegate.h"
#include "projectthumbnailcache.h"

namespace olive {

/**
 * @brief The view widget used when ProjectExplorer is in Icon View
 *
 * Draws Metro/Fluent tile cards. Thumbnails are produced asynchronously by an owned ThumbnailCache;
 * when one becomes ready the affected tile is repainted.
 */
class ProjectExplorerIconView : public ProjectExplorerListViewBase
{
  Q_OBJECT
public:
  ProjectExplorerIconView(QWidget* parent);

private slots:
  void ThumbnailReady(const QString &key);

private:
  // Declared before the delegate: the delegate takes a pointer to the cache.
  ProjectThumbnailCache thumbnail_cache_;

  ProjectExplorerIconViewItemDelegate delegate_;
};

}

#endif // PROJECTEXPLORERICONVIEW_H
