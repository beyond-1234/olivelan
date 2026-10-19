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

#include "thememanager.h"
#include "designtokenregistry.h"
#include "qssmacropreprocessor.h"
#include "style.h"
#include "metrostyleconfig.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QSettings>
#include <QTimer>
#include <QDebug>

namespace olive {

ThemeManager* ThemeManager::instance()
{
  static ThemeManager mgr;
  return &mgr;
}

ThemeManager::ThemeManager(QObject *parent)
  : QObject(parent),
    current_theme_id_(QString::fromLatin1(metro::kThemeName))
{
  connect(&file_watcher_, &QFileSystemWatcher::fileChanged, this, &ThemeManager::OnFileChanged);
}

void ThemeManager::Initialize()
{
  ScanThemeDirectories();
}

QString ThemeManager::GetThemesDirectory() const
{
  QString app_data = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QDir dir(app_data);
  dir.mkpath(QStringLiteral("themes"));
  return dir.filePath(QStringLiteral("themes"));
}

void ThemeManager::ScanThemeDirectories()
{
  themes_.clear();

  // 1. Built-in themes
  ThemeInfo metro_theme;
  metro_theme.id = QString::fromLatin1(metro::kThemeName);
  metro_theme.name = QString::fromLatin1(metro::kThemeDisplayName);
  metro_theme.author = QStringLiteral("Olive Team");
  metro_theme.version = QStringLiteral("2.0");
  metro_theme.is_builtin = true;
  themes_.insert(metro_theme.id, metro_theme);

  ThemeInfo dark_theme;
  dark_theme.id = QStringLiteral("olive-dark");
  dark_theme.name = QStringLiteral("Olive Dark");
  dark_theme.author = QStringLiteral("Olive Team");
  dark_theme.version = QStringLiteral("1.0");
  dark_theme.is_builtin = true;
  themes_.insert(dark_theme.id, dark_theme);

  ThemeInfo light_theme;
  light_theme.id = QStringLiteral("olive-light");
  light_theme.name = QStringLiteral("Olive Light");
  light_theme.author = QStringLiteral("Olive Team");
  light_theme.version = QStringLiteral("1.0");
  light_theme.is_builtin = true;
  themes_.insert(light_theme.id, light_theme);

  // 2. Scan external user themes directory
  QDir themes_dir(GetThemesDirectory());
  QFileInfoList entries = themes_dir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot);

  for (const QFileInfo &entry : entries) {
    if (entry.isDir()) {
      QString manifest_path = entry.filePath() + QStringLiteral("/manifest.json");
      ThemeInfo info;
      info.id = entry.fileName();
      info.path = entry.filePath();
      info.name = entry.fileName();
      info.is_builtin = false;

      if (QFile::exists(manifest_path)) {
        QFile f(manifest_path);
        if (f.open(QIODevice::ReadOnly)) {
          QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
          if (doc.isObject()) {
            QJsonObject obj = doc.object();
            if (obj.contains("name")) info.name = obj.value("name").toString();
            if (obj.contains("author")) info.author = obj.value("author").toString();
            if (obj.contains("version")) info.version = obj.value("version").toString();
          }
        }
      }
      themes_.insert(info.id, info);
    } else if (entry.isFile() && entry.suffix().compare(QStringLiteral("qss"), Qt::CaseInsensitive) == 0) {
      // Community raw .qss single file support!
      ThemeInfo qss_theme;
      qss_theme.id = entry.baseName();
      qss_theme.name = entry.baseName();
      qss_theme.path = entry.filePath();
      qss_theme.author = QStringLiteral("Community");
      qss_theme.version = QStringLiteral("1.0");
      qss_theme.is_builtin = false;
      qss_theme.is_pure_qss = true;
      themes_.insert(qss_theme.id, qss_theme);
    }
  }

  emit ThemeListUpdated();
}

QStringList ThemeManager::GetAvailableThemeIds() const
{
  return themes_.keys();
}

ThemeInfo ThemeManager::GetThemeInfo(const QString &theme_id) const
{
  return themes_.value(theme_id);
}

bool ThemeManager::ApplyTheme(const QString &theme_id)
{
  if (!themes_.contains(theme_id)) {
    qWarning() << "[ThemeManager] Requested theme not found:" << theme_id;
    return false;
  }

  const ThemeInfo &info = themes_.value(theme_id);
  current_theme_id_ = theme_id;

  // Clear file watcher
  if (!file_watcher_.files().isEmpty()) {
    file_watcher_.removePaths(file_watcher_.files());
  }

  if (info.is_builtin) {
    DesignTokenRegistry::instance()->ResetToDefaults();
    StyleManager::SetStyle(info.id);
  } else {
    // 1. Load Tokens if present (tokens.json)
    if (!info.is_pure_qss) {
      QString tokens_path = info.path + QStringLiteral("/tokens.json");
      if (QFile::exists(tokens_path)) {
        DesignTokenRegistry::instance()->LoadFromJsonFile(tokens_path);
        if (hot_reload_enabled_) {
          file_watcher_.addPath(tokens_path);
        }
      }
    }

    // 2. Load Palette
    QString palette_path = info.is_pure_qss ? QStringLiteral(":/style/olive-metro/palette.ini")
                                            : info.path + QStringLiteral("/palette.ini");
    if (!QFile::exists(palette_path)) {
      palette_path = QStringLiteral(":/style/olive-metro/palette.ini");
    }
    QPalette custom_palette = StyleManager::ParsePalette(palette_path);
    qApp->setPalette(custom_palette);

    // 3. Load & Preprocess QSS
    QString raw_qss;
    QString qss_file = info.is_pure_qss ? info.path : info.path + QStringLiteral("/style.qss");
    if (!QFile::exists(qss_file) && !info.is_pure_qss) {
      qss_file = info.path + QStringLiteral("/style.css");
    }

    QFile f(qss_file);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
      raw_qss = QString::fromUtf8(f.readAll());
      if (hot_reload_enabled_) {
        file_watcher_.addPath(qss_file);
      }
    }

    QString compiled_qss = QssMacroPreprocessor::Process(raw_qss, DesignTokenRegistry::instance()->colors());
    qApp->setStyleSheet(compiled_qss);
  }

  emit ThemeChanged(theme_id);
  return true;
}

void ThemeManager::OnFileChanged(const QString &path)
{
  qInfo() << "[ThemeManager] Hot-reload triggered by file change:" << path;
  // Re-apply current theme with debounce
  QTimer::singleShot(50, this, [this]() {
    ApplyTheme(current_theme_id_);
  });
}

void ThemeManager::SetHotReloadEnabled(bool enabled)
{
  hot_reload_enabled_ = enabled;
  if (!enabled && !file_watcher_.files().isEmpty()) {
    file_watcher_.removePaths(file_watcher_.files());
  }
}

bool ThemeManager::ImportThemePackage(const QString &source_file_or_dir)
{
  QFileInfo fi(source_file_or_dir);
  if (!fi.exists()) {
    return false;
  }

  QString target_dir = GetThemesDirectory();

  if (fi.isFile() && fi.suffix().compare(QStringLiteral("qss"), Qt::CaseInsensitive) == 0) {
    // Copy single .qss into themes directory
    QString dest = target_dir + QStringLiteral("/") + fi.fileName();
    if (QFile::exists(dest)) QFile::remove(dest);
    if (QFile::copy(fi.filePath(), dest)) {
      ScanThemeDirectories();
      return true;
    }
  }

  return false;
}

} // namespace olive
