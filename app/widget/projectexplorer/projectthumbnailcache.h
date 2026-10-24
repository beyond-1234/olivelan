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

#ifndef PROJECTTHUMBNAILCACHE_H
#define PROJECTTHUMBNAILCACHE_H

#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QString>
#include <QThreadPool>

#include "common/define.h"

namespace olive {

class ProjectThumbnailWorker;

/**
 * @brief Asynchronous LRU cache of Project Panel tile thumbnails.
 *
 * (Named distinctly from render::ThumbnailCache, which is a FrameHashCache subclass used by the
 * timeline.)
 *
 * The Project Panel's tile delegate must never block on media I/O while painting. This cache lets
 * it fetch a thumbnail synchronously (`Get`) and fall back to a placeholder + an asynchronous
 * decode request (`Request`) on a miss. When a decode finishes, `Ready` is emitted so the view can
 * repaint just that tile.
 *
 * Thumbnails are keyed by an opaque string (the Footage filename), so two Footage items pointing
 * at the same file share one decode.
 */
class ProjectThumbnailCache : public QObject
{
  Q_OBJECT
public:
  enum {
    kThumbnailWidth = 160,
    kThumbnailHeight = 90,
    kMaxEntries = 256
  };

  explicit ProjectThumbnailCache(QObject *parent = nullptr);
  ~ProjectThumbnailCache() override;

  /**
   * @brief Returns the cached thumbnail for `key`, or a null pixmap if it isn't ready yet.
   *
   * A successful lookup marks the entry as most-recently-used.
   */
  QPixmap Get(const QString &key) const;

  /**
   * @brief Ensures a thumbnail for `key`/`filename` is queued for asynchronous decoding.
   *
   * Does nothing if the thumbnail is already cached or a decode is already in flight. Safe to call
   * repeatedly (e.g. once per repaint).
   */
  void Request(const QString &key, const QString &filename);

signals:
  void Ready(const QString &key);

private:
  friend class ProjectThumbnailWorker;
  // Test seam for the P2-8 regression suite (patch 0093): lets the negative-cache/FIFO state
  // machine be exercised without starting real FFmpeg decodes.
  friend class ProjectThumbnailCacheTest;

  /// Delivers a finished decode. Always runs on the GUI thread.
  void OnWorkerDone(const QString &key, const QImage &image);

  void Insert(const QString &key, const QPixmap &pixmap);

  QHash<QString, QPixmap> cache_;

  /// Least-recently-used order; front is most recently used.
  mutable QList<QString> lru_;

  /// Keys with a decode currently running in the thread pool.
  QSet<QString> in_flight_;

  /**
   * @brief Keys whose decode produced no image (audio-only, unreadable, unsupported codec).
   *
   * Negative results are cached like successes: without this, the delegate re-queues a full
   * FFmpeg open on every repaint for a clip that can never have a thumbnail. Bounded by the same
   * limit as the LRU (FIFO eviction), so a file that becomes readable later is retried eventually.
   */
  QSet<QString> failed_;

  /// FIFO order for `failed_`, oldest first.
  QList<QString> failed_order_;

  /**
   * @brief Private decode pool (patch 0092 / P2-4).
   *
   * Kept separate from QThreadPool::globalInstance() so destroying the cache only waits for
   * thumbnail decodes, not for unrelated global-pool work (and so thumbnail concurrency is
   * bounded instead of consuming every core).
   */
  QThreadPool pool_;
};

}

#endif // PROJECTTHUMBNAILCACHE_H
