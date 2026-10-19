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

#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>

namespace olive {

struct ThemeInfo {
  QString id;
  QString name;
  QString author;
  QString version;
  QString path;
  bool is_builtin{false};
  bool is_pure_qss{false};
};

/**
 * @brief Dynamic Theme Lifecycle & Hot-Reload Manager.
 *        Scans custom directories, watches for changes, and dynamically compiles QSS.
 */
class ThemeManager : public QObject {
  Q_OBJECT

public:
  static ThemeManager* instance();

  void Initialize();

  QStringList GetAvailableThemeIds() const;
  ThemeInfo GetThemeInfo(const QString &theme_id) const;

  bool ApplyTheme(const QString &theme_id);
  const QString& CurrentThemeId() const { return current_theme_id_; }

  QString GetThemesDirectory() const;
  bool ImportThemePackage(const QString &source_file_or_dir);

  void SetHotReloadEnabled(bool enabled);
  bool IsHotReloadEnabled() const { return hot_reload_enabled_; }

signals:
  void ThemeChanged(const QString &theme_id);
  void ThemeListUpdated();

private slots:
  void OnFileChanged(const QString &path);

private:
  ThemeManager(QObject *parent = nullptr);
  ~ThemeManager() override = default;

  void ScanThemeDirectories();
  bool LoadThemeFiles(const ThemeInfo &info, QString &out_qss, QString &out_palette_ini);

  QString current_theme_id_;
  QHash<QString, ThemeInfo> themes_;
  QFileSystemWatcher file_watcher_;
  bool hot_reload_enabled_{true};
};

} // namespace olive

#endif // THEMEMANAGER_H
