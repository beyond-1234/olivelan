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
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QDebug>

#ifdef OLIVE_HAVE_QZIPREADER
#include <QtCore/private/qzipreader_p.h>
#endif

namespace olive {

namespace {

constexpr qint64 kMaximumImportedPackageBytes = 64LL * 1024LL * 1024LL;
constexpr int kMaximumImportedPackageEntries = 2048;

bool IsSafeArchivePath(const QString &path)
{
  if (path.isEmpty() || path.contains(QLatin1Char('\\')) || QDir::isAbsolutePath(path)) {
    return false;
  }

  const QString cleaned = QDir::cleanPath(path);
  if (cleaned == QStringLiteral("..") || cleaned.startsWith(QStringLiteral("../"))) {
    return false;
  }

  // Reject any parent segment, even when it is normalized away (for example
  // "packaged/../escape.qss"). Archive entries must stay under the extraction
  // root rather than relying on cleanPath().
  const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
  return !parts.contains(QStringLiteral(".."));
}

bool HasThemeStyleFile(const QString &directory)
{
  return QFileInfo(directory + QStringLiteral("/style.qss")).isFile()
      || QFileInfo(directory + QStringLiteral("/style.css")).isFile();
}

bool RemovePath(const QString &path)
{
  const QFileInfo info(path);
  if (!info.exists() && !info.isSymLink()) {
    return true;
  }
  if (info.isDir() && !info.isSymLink()) {
    return QDir(path).removeRecursively();
  }
  return QFile::remove(path);
}

bool CopyDirectoryRecursively(const QString &source, const QString &destination)
{
  QDir source_dir(source);
  if (!source_dir.exists()) {
    return false;
  }

  if (!QDir().mkpath(destination)) {
    return false;
  }

  QDirIterator it(source, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
                  QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString source_path = it.next();
    const QFileInfo info(source_path);
    const QString relative_path = source_dir.relativeFilePath(source_path);
    const QString destination_path = destination + QLatin1Char('/') + relative_path;

    if (info.isSymLink()) {
      continue;
    }

    if (info.isDir()) {
      if (!QDir().mkpath(destination_path)) {
        return false;
      }
    } else if (info.isFile()) {
      if (!QDir().mkpath(QFileInfo(destination_path).path())) {
        return false;
      }
      QFile::remove(destination_path);
      if (!QFile::copy(source_path, destination_path)) {
        return false;
      }
    }
  }

  return true;
}

QHash<QString, QColor> PaletteTokens(const QPalette &palette)
{
  QHash<QString, QColor> tokens;
  tokens.insert(QStringLiteral("color.canvas.base"), palette.window().color());
  tokens.insert(QStringLiteral("color.card.fill"), palette.alternateBase().color());
  tokens.insert(QStringLiteral("color.border.hairline"), palette.button().color());
  tokens.insert(QStringLiteral("color.accent.primary"), palette.highlight().color());
  tokens.insert(QStringLiteral("color.text.primary"), palette.text().color());
  return tokens;
}

bool ParsePaletteFile(const QString &path, QPalette *palette)
{
  if (!QFileInfo::exists(path)) {
    return false;
  }

  QSettings probe(path, QSettings::IniFormat);
  if (!path.startsWith(QLatin1Char(':'))) {
    if (probe.status() != QSettings::NoError) {
      return false;
    }

    // QPalette's default constructor already has valid black/white colors, so
    // merely checking isValid() cannot detect a palette that omitted a role.
    // Require the four roles the rest of the UI relies on.
    probe.beginGroup(QStringLiteral("All"));
    const QStringList keys = probe.childKeys();
    probe.endGroup();
    const QStringList required_roles = {
      QStringLiteral("Window"),
      QStringLiteral("Text"),
      QStringLiteral("Base"),
      QStringLiteral("Highlight")
    };
    for (const QString &role : required_roles) {
      if (!keys.contains(role, Qt::CaseInsensitive)) {
        return false;
      }
    }
  }

  *palette = StyleManager::ParsePalette(path);
  if (!palette->color(QPalette::Window).isValid()
      || !palette->color(QPalette::Text).isValid()
      || !palette->color(QPalette::Base).isValid()
      || !palette->color(QPalette::Highlight).isValid()) {
    return false;
  }
  return true;
}

} // namespace

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

  reload_timer_.setSingleShot(true);
  reload_timer_.setInterval(20);
  connect(&reload_timer_, &QTimer::timeout, this, [this]() {
    if (!current_theme_id_.isEmpty()) {
      ApplyTheme(current_theme_id_);
    }
  });
}

void ThemeManager::Initialize(const QString &saved_theme_id)
{
  ScanThemeDirectories();

  QString target = saved_theme_id;
  if (target.isEmpty() || !FindTheme(target)) {
    target = StyleManager::GetStyle();
  }
  if (target.isEmpty() || !FindTheme(target)) {
    target = QString::fromLatin1(StyleManager::kDefaultStyle);
  }

  if (!ApplyTheme(target)) {
    qWarning() << "[ThemeManager] Failed to restore theme" << target << "- falling back to default";
    const QString default_id = QString::fromLatin1(StyleManager::kDefaultStyle);
    if (current_theme_id_ != default_id) {
      ApplyTheme(default_id);
    }
  }
}

void ThemeManager::RefreshThemes()
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

  // Built-in themes are always first and always use a stable order.
  {
    ThemeInfo metro_theme;
    metro_theme.id = QString::fromLatin1(metro::kThemeName);
    metro_theme.name = QString::fromLatin1(metro::kThemeDisplayName);
    metro_theme.author = QStringLiteral("Olive Team");
    metro_theme.version = QStringLiteral("2.0");
    metro_theme.is_builtin = true;
    themes_.append(metro_theme);
  }
  {
    ThemeInfo dark_theme;
    dark_theme.id = QStringLiteral("olive-dark");
    dark_theme.name = QStringLiteral("Olive Dark");
    dark_theme.author = QStringLiteral("Olive Team");
    dark_theme.version = QStringLiteral("1.0");
    dark_theme.is_builtin = true;
    themes_.append(dark_theme);
  }
  {
    ThemeInfo light_theme;
    light_theme.id = QStringLiteral("olive-light");
    light_theme.name = QStringLiteral("Olive Light");
    light_theme.author = QStringLiteral("Olive Team");
    light_theme.version = QStringLiteral("1.0");
    light_theme.is_builtin = true;
    themes_.append(light_theme);
  }

  // Scan external user themes in a deterministic order.
  QDir themes_dir(GetThemesDirectory());
  const QFileInfoList entries = themes_dir.entryInfoList(
      QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable,
      QDir::Name | QDir::IgnoreCase);

  for (const QFileInfo &entry : entries) {
    if (entry.isDir()) {
      const QString id = entry.fileName();
      if (FindTheme(id)) {
        qWarning() << "[ThemeManager] Skipping theme with duplicate id:" << id;
        continue;
      }
      if (!HasThemeStyleFile(entry.filePath())) {
        qWarning() << "[ThemeManager] Skipping theme directory without style.qss/style.css:" << entry.filePath();
        continue;
      }

      ThemeInfo info;
      info.id = id;
      info.path = entry.filePath();
      info.name = id;
      info.is_builtin = false;

      const QString manifest_path = entry.filePath() + QStringLiteral("/manifest.json");
      if (QFileInfo::exists(manifest_path)) {
        QFile manifest(manifest_path);
        if (manifest.open(QIODevice::ReadOnly)) {
          QJsonParseError parse_error;
          const QJsonDocument doc = QJsonDocument::fromJson(manifest.readAll(), &parse_error);
          if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning() << "[ThemeManager] Invalid manifest for" << id << ":"
                       << parse_error.errorString();
          } else {
            const QJsonObject obj = doc.object();
            const QString name = obj.value(QStringLiteral("name")).toString();
            const QString author = obj.value(QStringLiteral("author")).toString();
            const QString version = obj.value(QStringLiteral("version")).toString();
            if (!name.isEmpty()) {
              info.name = name;
            }
            if (!author.isEmpty()) {
              info.author = author;
            }
            if (!version.isEmpty()) {
              info.version = version;
            }
          }
        }
      }

      themes_.append(info);
    } else if (entry.isFile()
               && entry.suffix().compare(QStringLiteral("qss"), Qt::CaseInsensitive) == 0) {
      const QString id = entry.completeBaseName();
      if (FindTheme(id)) {
        qWarning() << "[ThemeManager] Skipping QSS theme with duplicate id:" << id;
        continue;
      }

      ThemeInfo qss_theme;
      qss_theme.id = id;
      qss_theme.name = id;
      qss_theme.path = entry.filePath();
      qss_theme.author = QStringLiteral("Community");
      qss_theme.version = QStringLiteral("1.0");
      qss_theme.is_builtin = false;
      qss_theme.is_pure_qss = true;
      themes_.append(qss_theme);
    }
  }

  emit ThemeListUpdated();
}

QStringList ThemeManager::GetAvailableThemeIds() const
{
  QStringList ids;
  ids.reserve(themes_.size());
  for (const ThemeInfo &info : themes_) {
    ids.append(info.id);
  }
  return ids;
}

ThemeInfo ThemeManager::GetThemeInfo(const QString &theme_id) const
{
  if (const ThemeInfo *info = FindTheme(theme_id)) {
    return *info;
  }
  return ThemeInfo();
}

const ThemeInfo *ThemeManager::FindTheme(const QString &theme_id) const
{
  for (const ThemeInfo &info : themes_) {
    if (info.id == theme_id) {
      return &info;
    }
  }
  return nullptr;
}

bool ThemeManager::ApplyTheme(const QString &theme_id)
{
  if (themes_.isEmpty()) {
    ScanThemeDirectories();
  }

  const ThemeInfo *info = FindTheme(theme_id);
  if (!info) {
    qWarning() << "[ThemeManager] Requested theme not found:" << theme_id;
    return false;
  }

  if (!file_watcher_.files().isEmpty()) {
    file_watcher_.removePaths(file_watcher_.files());
  }
  reload_timer_.stop();

  if (info->is_builtin) {
    StyleManager::SetStyle(info->id);
    DesignTokenRegistry::instance()->ResetToDefaults();
    if (info->id != QString::fromLatin1(metro::kThemeName)) {
      DesignTokenRegistry::instance()->SetColors(PaletteTokens(qApp->palette()));
    }

    current_theme_id_ = info->id;
    StyleManager::SetCurrentStyleId(info->id);
    qInfo() << "[ThemeManager] Applied built-in theme" << info->id;

    emit ThemeChanged(info->id);
    return true;
  }

  QString error;
  if (!LoadExternalTheme(*info, &error)) {
    qWarning() << "[ThemeManager] Failed to apply theme" << info->id << ":" << error;
    FallbackToDefault();
    return false;
  }

  current_theme_id_ = info->id;
  StyleManager::SetCurrentStyleId(info->id);
  qInfo() << "[ThemeManager] Applied theme" << info->id << "from" << info->path;

  emit ThemeChanged(info->id);
  return true;
}

bool ThemeManager::LoadExternalTheme(const ThemeInfo &info, QString *error)
{
  // 1. Resolve the stylesheet first so an unreadable theme never mutates the UI.
  QString qss_path;
  if (info.is_pure_qss) {
    qss_path = info.path;
  } else {
    qss_path = info.path + QStringLiteral("/style.qss");
    if (!QFileInfo(qss_path).isFile()) {
      qss_path = info.path + QStringLiteral("/style.css");
    }
  }

  QFile qss_file(qss_path);
  if (!QFileInfo(qss_path).isFile() || !qss_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    *error = QStringLiteral("cannot read stylesheet '%1'").arg(qss_path);
    return false;
  }

  const QString raw_qss = QString::fromUtf8(qss_file.readAll());
  qss_file.close();
  if (raw_qss.trimmed().isEmpty()) {
    *error = QStringLiteral("stylesheet '%1' is empty").arg(qss_path);
    return false;
  }

  // 2. Resolve the palette. A malformed palette is rejected rather than silently
  //    replacing the application palette with defaults.
  QString palette_path = info.is_pure_qss
      ? QStringLiteral(":/style/olive-metro/palette.ini")
      : info.path + QStringLiteral("/palette.ini");
  if (!QFileInfo::exists(palette_path)) {
    palette_path = QStringLiteral(":/style/olive-metro/palette.ini");
  }

  QPalette palette;
  if (!ParsePaletteFile(palette_path, &palette)) {
    *error = QStringLiteral("cannot parse palette '%1'").arg(palette_path);
    return false;
  }

  DesignTokenRegistry::instance()->ResetToDefaults();
  if (!info.is_pure_qss) {
    DesignTokenRegistry::instance()->SetColors(PaletteTokens(palette));
  }
  qApp->setPalette(palette);

  // 3. Load optional design tokens after the palette-derived defaults so theme
  //    tokens always win.
  QString tokens_path;
  if (!info.is_pure_qss) {
    tokens_path = info.path + QStringLiteral("/tokens.json");
    if (QFileInfo::exists(tokens_path)
        && !DesignTokenRegistry::instance()->LoadFromJsonFile(tokens_path)) {
      *error = QStringLiteral("cannot parse tokens '%1'").arg(tokens_path);
      return false;
    }
  }

  // 4. Preprocess and validate the stylesheet before committing it.
  QList<QssMacroPreprocessor::Diagnostic> diagnostics;
  const QString compiled_qss = QssMacroPreprocessor::Process(
      raw_qss, DesignTokenRegistry::instance()->AllTokens(), &diagnostics);

  if (!diagnostics.isEmpty()) {
    const QssMacroPreprocessor::Diagnostic &first = diagnostics.constFirst();
    *error = QStringLiteral("stylesheet error at line %1: %2")
                 .arg(first.line)
                 .arg(first.message);
    return false;
  }

  qApp->setStyleSheet(compiled_qss);

  // 5. Watch the files that can be hot-reloaded.
  if (hot_reload_enabled_) {
    if (!tokens_path.isEmpty()) {
      file_watcher_.addPath(tokens_path);
    }
    if (QFileInfo(qss_path).isFile()) {
      file_watcher_.addPath(qss_path);
    }
    if (palette_path.startsWith(QLatin1Char(':')) == false && QFileInfo::exists(palette_path)) {
      file_watcher_.addPath(palette_path);
    }
  }

  return true;
}

bool ThemeManager::FallbackToDefault()
{
  const QString default_id = QString::fromLatin1(StyleManager::kDefaultStyle);

  if (current_theme_id_ == default_id && FindTheme(default_id)) {
    // The default theme is already active; make sure its palette/tokens are sane.
    StyleManager::SetStyle(default_id);
    DesignTokenRegistry::instance()->ResetToDefaults();
    StyleManager::SetCurrentStyleId(default_id);
    emit ThemeChanged(default_id);
    return true;
  }

  return ApplyTheme(default_id);
}

void ThemeManager::OnFileChanged(const QString &path)
{
  qInfo() << "[ThemeManager] Hot-reload triggered by file change:" << path;
  reload_timer_.start();
}

void ThemeManager::SetHotReloadEnabled(bool enabled)
{
  hot_reload_enabled_ = enabled;

  if (!enabled) {
    reload_timer_.stop();
    if (!file_watcher_.files().isEmpty()) {
      file_watcher_.removePaths(file_watcher_.files());
    }
  } else if (!current_theme_id_.isEmpty()) {
    ApplyTheme(current_theme_id_);
  }
}

bool ThemeManager::ImportThemePackage(const QString &source_file_or_dir, bool overwrite)
{
  const QFileInfo source(source_file_or_dir);
  if (!source.exists()) {
    return false;
  }

  if (source.isDir()) {
    return ImportThemeDirectory(source.filePath(), source.fileName(), overwrite);
  }

  if (!source.isFile()) {
    return false;
  }

  const QString suffix = source.suffix().toLower();

  if (suffix == QStringLiteral("qss")) {
    const QString id = source.completeBaseName();
    const QString destination = GetThemesDirectory() + QLatin1Char('/') + source.fileName();

    if (source.canonicalFilePath() == QFileInfo(destination).canonicalFilePath()) {
      RefreshThemes();
      return true;
    }
    if (QFileInfo::exists(destination) && !overwrite) {
      qWarning() << "[ThemeManager] Theme already exists:" << destination;
      return false;
    }

    QFile input(source.filePath());
    if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) {
      return false;
    }

    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) {
      return false;
    }
    output.write(input.readAll());
    if (!output.commit()) {
      qWarning() << "[ThemeManager] Failed to write theme:" << destination;
      return false;
    }

    RefreshThemes();
    return true;
  }

  if (suffix == QStringLiteral("olivetheme") || suffix == QStringLiteral("zip")) {
    return ImportZipPackage(source.filePath(), source.completeBaseName(), overwrite);
  }

  return false;
}

bool ThemeManager::ImportThemeDirectory(const QString &source_dir,
                                        const QString &id,
                                        bool overwrite)
{
  if (id.isEmpty() || !HasThemeStyleFile(source_dir)) {
    return false;
  }

  const QString target_dir = GetThemesDirectory();
  const QString destination = target_dir + QLatin1Char('/') + id;
  const QString source_canonical = QFileInfo(source_dir).canonicalFilePath();
  const QString target_canonical = QFileInfo(target_dir).canonicalFilePath();

  // Importing the themes directory itself (or one of its ancestors) would copy
  // the staging directory into itself. Reject that shape outright.
  if (source_canonical == target_canonical
      || (!source_canonical.isEmpty() && target_canonical.startsWith(source_canonical + QLatin1Char('/')))) {
    qWarning() << "[ThemeManager] Refusing to import a directory that contains the themes directory:"
               << source_dir;
    return false;
  }

  if (QFileInfo(source_dir).canonicalFilePath() == QFileInfo(destination).canonicalFilePath()) {
    RefreshThemes();
    return true;
  }

  if (QFileInfo::exists(destination) && !overwrite) {
    qWarning() << "[ThemeManager] Theme already exists:" << destination;
    return false;
  }

  const QString staging = target_dir + QStringLiteral("/.") + id + QStringLiteral(".import");
  RemovePath(staging);

  if (!CopyDirectoryRecursively(source_dir, staging)) {
    RemovePath(staging);
    return false;
  }

  if (QFileInfo::exists(destination)) {
    RemovePath(destination);
  }

  if (!QDir().rename(staging, destination)) {
    qWarning() << "[ThemeManager] Failed to move imported theme into place:" << destination;
    RemovePath(staging);
    return false;
  }

  RefreshThemes();
  return true;
}

bool ThemeManager::ImportZipPackage(const QString &source_file,
                                    const QString &id,
                                    bool overwrite)
{
#ifdef OLIVE_HAVE_QZIPREADER
  QZipReader zip(source_file);
  if (!zip.isReadable()) {
    qWarning() << "[ThemeManager] Unreadable theme package:" << source_file;
    return false;
  }

  const QList<QZipReader::FileInfo> entries = zip.fileInfoList();
  if (entries.isEmpty() || entries.size() > kMaximumImportedPackageEntries) {
    return false;
  }

  qint64 total_bytes = 0;
  for (const QZipReader::FileInfo &entry : entries) {
    if (entry.isDir) {
      continue;
    }
    if (entry.isSymLink || !IsSafeArchivePath(entry.filePath)) {
      qWarning() << "[ThemeManager] Rejecting unsafe archive entry:" << entry.filePath;
      return false;
    }
    total_bytes += entry.size;
  }

  if (total_bytes > kMaximumImportedPackageBytes) {
    qWarning() << "[ThemeManager] Theme package is too large:" << total_bytes;
    return false;
  }

  QTemporaryDir temporary(GetThemesDirectory() + QStringLiteral("/.olivetheme-XXXXXX"));
  if (!temporary.isValid()) {
    return false;
  }

  qint64 extracted_bytes = 0;
  for (const QZipReader::FileInfo &entry : entries) {
    const QString destination = temporary.filePath(entry.filePath);

    if (entry.isDir) {
      if (!QDir().mkpath(destination)) {
        return false;
      }
      continue;
    }

    if (!QDir().mkpath(QFileInfo(destination).path())) {
      return false;
    }

    const QByteArray data = zip.fileData(entry.filePath);
    extracted_bytes += data.size();
    if (extracted_bytes > kMaximumImportedPackageBytes) {
      qWarning() << "[ThemeManager] Theme package expands beyond the size limit";
      return false;
    }

    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)
        || output.write(data) != data.size()
        || !output.commit()) {
      return false;
    }
  }

  zip.close();

  QString root = temporary.path();
  if (!HasThemeStyleFile(root)) {
    root.clear();

    const QFileInfoList root_entries = QDir(temporary.path()).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : root_entries) {
      if (HasThemeStyleFile(entry.filePath())) {
        if (!root.isEmpty()) {
          qWarning() << "[ThemeManager] Theme package has more than one root theme";
          return false;
        }
        root = entry.filePath();
      }
    }
  }

  if (root.isEmpty()) {
    qWarning() << "[ThemeManager] Theme package has no style.qss/style.css";
    return false;
  }

  return ImportThemeDirectory(root, id, overwrite);
#else
  Q_UNUSED(source_file)
  Q_UNUSED(id)
  Q_UNUSED(overwrite)
  qWarning() << "[ThemeManager] This build has no Qt ZipReader support; extract the package manually";
  return false;
#endif
}

} // namespace olive
