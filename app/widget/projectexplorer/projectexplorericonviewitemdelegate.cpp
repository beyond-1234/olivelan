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

#include "projectexplorericonviewitemdelegate.h"

#include <QPainter>

#include "common/qtutils.h"
#include "projectthumbnailcache.h"
#include "projectviewmodel.h"
#include "ui/style/metrostyleconfig.h"

namespace olive {

namespace {

QColor BadgeColor(const QString &badge)
{
  if (badge == QStringLiteral("4K")) {
    return metro::kAccentColor;
  } else if (badge == QStringLiteral("10bit")) {
    return QColor(0x87, 0x64, 0xB8);
  } else if (badge == QStringLiteral("60FPS")) {
    return metro::kTrackVideo;
  } else if (badge == QStringLiteral("AUDIO")) {
    return metro::kTrackAudio;
  }

  return metro::kBorderColor;
}

}

ProjectExplorerIconViewItemDelegate::ProjectExplorerIconViewItemDelegate(ProjectThumbnailCache *thumbnail_cache, QObject *parent) :
  QStyledItemDelegate(parent),
  thumbnail_cache_(thumbnail_cache)
{
}

QSize ProjectExplorerIconViewItemDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const
{
  Q_UNUSED(option)

  // 16:9 cover + a metadata bar. Note that ProjectExplorer sets a (square) grid size on the icon
  // view, so the actual item rect is the grid cell; paint() lays the tile out inside `option.rect`.
  return QSize(176, 128);
}

void ProjectExplorerIconViewItemDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
  const bool selected = option.state & QStyle::State_Selected;
  const QFontMetrics fm = painter->fontMetrics();

  const int bar_height = fm.height() + 6;

  const QRect cell = option.rect;
  const int thumb_height = qBound(0, cell.width() * 9 / 16, qMax(0, cell.height() - bar_height));
  const QRect tile(cell.x(), cell.y(), cell.width(), thumb_height + bar_height);

  if (tile.width() <= 2 || tile.height() <= 2) {
    return;
  }

  const QRect thumb_rect(tile.x(), tile.y(), tile.width(), thumb_height);
  const QRect bar_rect(tile.x(), tile.y() + thumb_height, tile.width(), bar_height);

  // Tile background (doubles as the placeholder colour while a thumbnail is being decoded).
  painter->fillRect(tile, metro::kTileBg);

  // ---- Thumbnail, or placeholder + async request ----
  const QString key = index.data(ProjectViewModel::kThumbnailKeyRole).toString();
  QPixmap thumb;

  if (!key.isEmpty()) {
    thumb = thumbnail_cache_->Get(key);

    if (thumb.isNull()) {
      // Not ready yet: request an asynchronous decode and draw a placeholder for now.
      thumbnail_cache_->Request(key, key);
    }
  }

  if (!thumb.isNull()) {
    painter->drawPixmap(thumb_rect, thumb.scaled(thumb_rect.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
  } else {
    QIcon ico = index.data(Qt::DecorationRole).value<QIcon>();
    if (!ico.isNull() && thumb_height > 0) {
      const QSize ico_size = ico.actualSize(thumb_rect.size());
      const QRect ico_rect(thumb_rect.x() + (thumb_rect.width() - ico_size.width()) / 2,
                           thumb_rect.y() + (thumb_rect.height() - ico_size.height()) / 2,
                           ico_size.width(),
                           ico_size.height());
      painter->drawPixmap(ico_rect, ico.pixmap(ico_size));
    }
  }

  // ---- Badges, overlaid on the top-left of the thumbnail ----
  const QStringList badges = index.data(ProjectViewModel::kBadgeRole).toStringList();
  if (!badges.isEmpty() && thumb_height > fm.height()) {
    const QFont original_font = painter->font();

    QFont badge_font = original_font;
    badge_font.setPointSizeF(qMax(6.0, badge_font.pointSizeF() * 0.75));

    const QFontMetrics badge_fm(badge_font);

    int x = thumb_rect.x() + 4;
    const int y = thumb_rect.y() + 4;

    painter->setFont(badge_font);

    for (const QString &badge : badges) {
      const int w = badge_fm.horizontalAdvance(badge) + 8;
      const QRect badge_rect(x, y, w, badge_fm.height());

      if (badge_rect.right() > thumb_rect.right() - 2) {
        break;
      }

      painter->fillRect(badge_rect, BadgeColor(badge));
      painter->setPen(Qt::white);
      painter->drawText(badge_rect, Qt::AlignCenter, badge);

      x += w + 4;
    }

    painter->setFont(original_font);
  }

  // ---- Metadata bar: name (left) + duration (right) ----
  painter->fillRect(bar_rect, metro::kCanvasBg);

  {
    const QString duration = index.data(ProjectViewModel::kDurationRole).toString();
    const QRect text_rect = bar_rect.adjusted(4, 0, -4, 0);
    const int duration_width = QtUtils::QFontMetricsWidth(fm, duration);

    int name_width = text_rect.width();

    painter->setPen(option.palette.text().color());

    if (duration_width > 0 && duration_width < text_rect.width() / 2) {
      painter->drawText(text_rect, static_cast<int>(Qt::AlignRight | Qt::AlignVCenter), duration);
      name_width -= duration_width + 4;
    }

    painter->drawText(text_rect,
                      static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter),
                      fm.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, name_width));
  }

  // ---- Border / selection frame (Metro) ----
  painter->setBrush(Qt::NoBrush);

  if (selected) {
    painter->setPen(QPen(metro::kAccentColor, metro::kBorderWidth * 2));
  } else {
    painter->setPen(QPen(metro::kBorderColor, metro::kBorderWidth));
  }

  painter->drawRect(tile.adjusted(0, 0, -1, -1));
}

}
