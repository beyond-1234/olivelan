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
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

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
 *
 * ThemeManager is the single source of truth for the active theme. Built-in
 * styles are still rendered by StyleManager, while external themes are loaded
 * and validated here before being committed to QApplication.
 */
class ThemeManager : public QObject {
  Q_OBJECT

public:
  static ThemeManager* instance();

  /**
   * @brief Scan theme directories and restore the saved theme.
   * @param saved_theme_id Theme id from the application config. If it is empty
   *        or cannot be found, the current StyleManager theme is used.
   */
  void Initialize(const QString &saved_theme_id = QString());

  /**
   * @brief Rescan the user theme directory without changing the active theme.
   */
  void RefreshThemes();

  QStringList GetAvailableThemeIds() const;
  const QList<ThemeInfo> &GetAvailableThemes() const { return themes_; }
  ThemeInfo GetThemeInfo(const QString &theme_id) const;

  bool ApplyTheme(const QString &theme_id);
  const QString &CurrentThemeId() const { return current_theme_id_; }

  QString GetThemesDirectory() const;

  /**
   * @brief Import a .qss file, a theme directory, or an .olivetheme/.zip package.
   * @param overwrite Replace an existing theme with the same id.
   */
  bool ImportThemePackage(const QString &source_file_or_dir, bool overwrite = false);

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
  const ThemeInfo *FindTheme(const QString &theme_id) const;

  bool LoadExternalTheme(const ThemeInfo &info, QString *error);
  bool ImportThemeDirectory(const QString &source_dir, const QString &id, bool overwrite);
  bool ImportZipPackage(const QString &source_file, const QString &id, bool overwrite);
  bool FallbackToDefault();

  QString current_theme_id_;
  QList<ThemeInfo> themes_;
  QFileSystemWatcher file_watcher_;
  QTimer reload_timer_;
  bool hot_reload_enabled_{true};
};

} // namespace olive

#endif // THEMEMANAGER_H
