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
#include "capcutstyleconfig.h"
#include "metrostyleconfig.h"
#include "ui/icons/icons.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPalette>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QDebug>

#include <cmath>

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

void ReadIntCapability(const QJsonObject &obj, const QString &key, int minimum, int maximum,
                       int *target)
{
  if (!obj.contains(key)) {
    return;
  }

  const QJsonValue value = obj.value(key);
  if (!value.isDouble()) {
    qWarning() << "[ThemeManager] Ignoring non-numeric capability" << key;
    return;
  }

  const int number = value.toInt(-1);
  if (number < minimum || number > maximum) {
    qWarning() << "[ThemeManager] Ignoring out-of-range capability" << key << ":" << number;
    return;
  }

  *target = number;
}

void ReadDoubleCapability(const QJsonObject &obj, const QString &key, double minimum,
                          double maximum, double *target)
{
  if (!obj.contains(key)) {
    return;
  }

  const QJsonValue value = obj.value(key);
  if (!value.isDouble()) {
    qWarning() << "[ThemeManager] Ignoring non-numeric capability" << key;
    return;
  }

  const double number = value.toDouble();
  if (!std::isfinite(number) || number < minimum || number > maximum) {
    qWarning() << "[ThemeManager] Ignoring out-of-range capability" << key << ":" << number;
    return;
  }

  *target = number;
}

void ReadBoolCapability(const QJsonObject &obj, const QString &key, bool *target)
{
  if (!obj.contains(key)) {
    return;
  }

  const QJsonValue value = obj.value(key);
  if (!value.isBool()) {
    qWarning() << "[ThemeManager] Ignoring non-boolean capability" << key;
    return;
  }

  *target = value.toBool();
}

void ReadCapabilities(const QJsonObject &manifest, ThemeInfo *info)
{
  ThemeCapabilities &caps = info->capabilities;

  const QJsonObject workspace = manifest.value(QStringLiteral("workspace")).toObject();
  if (!workspace.isEmpty()) {
    const QString layout = workspace.value(QStringLiteral("layout")).toString().trimmed();
    if (!layout.isEmpty()) {
      if (layout.compare(QStringLiteral("standard"), Qt::CaseInsensitive) == 0) {
        caps.workspace_layout = ThemeCapabilities::WorkspaceLayout::kStandard;
      } else if (layout.compare(QStringLiteral("threeColumn"), Qt::CaseInsensitive) == 0
                 || layout.compare(QStringLiteral("three-column"), Qt::CaseInsensitive) == 0
                 || layout.compare(QStringLiteral("three_column"), Qt::CaseInsensitive) == 0) {
        caps.workspace_layout = ThemeCapabilities::WorkspaceLayout::kThreeColumn;
      } else {
        qWarning() << "[ThemeManager] Unknown workspace layout" << layout
                   << "for theme" << info->id;
      }
    }

    ReadBoolCapability(workspace, QStringLiteral("toolPanel"), &caps.show_tool_panel);
    ReadBoolCapability(workspace, QStringLiteral("nodePanel"), &caps.show_node_panel);
    ReadBoolCapability(workspace, QStringLiteral("footageViewer"), &caps.show_footage_viewer);
    ReadBoolCapability(workspace, QStringLiteral("emptyInspectorWhenUnselected"),
                       &caps.empty_inspector_when_unselected);

    ReadIntCapability(workspace, QStringLiteral("projectWidth"), 0, 2000,
                      &caps.project_panel_width);
    ReadIntCapability(workspace, QStringLiteral("inspectorWidth"), 0, 2000,
                      &caps.inspector_panel_width);
    ReadIntCapability(workspace, QStringLiteral("timelineHeightPercent"), 0, 90,
                      &caps.timeline_height_percent);
    ReadIntCapability(workspace, QStringLiteral("projectIconSize"), 0, 512,
                      &caps.project_icon_size);
    ReadDoubleCapability(workspace, QStringLiteral("projectIconHeightRatio"), 0.0, 2.0,
                         &caps.project_icon_height_ratio);

    const QString project_view =
        workspace.value(QStringLiteral("projectView")).toString().trimmed().toLower();
    if (!project_view.isEmpty()) {
      if (project_view == QStringLiteral("tree") || project_view == QStringLiteral("list")
          || project_view == QStringLiteral("icon")) {
        caps.project_view = project_view;
      } else {
        qWarning() << "[ThemeManager] Unknown project view" << project_view
                   << "for theme" << info->id;
      }
    }
  }

  const QJsonObject fonts = manifest.value(QStringLiteral("fonts")).toObject();
  const QJsonValue ui_fonts = fonts.value(QStringLiteral("ui"));
  if (ui_fonts.isArray()) {
    for (const QJsonValue &value : ui_fonts.toArray()) {
      if (value.isString()) {
        const QString family = value.toString().trimmed();
        if (!family.isEmpty() && !caps.ui_font_families.contains(family, Qt::CaseInsensitive)) {
          caps.ui_font_families.append(family);
        }
      }
    }
  } else if (ui_fonts.isString()) {
    // Comma-separated form keeps hand-written manifests convenient.
    for (const QString &family : ui_fonts.toString().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
      const QString trimmed = family.trimmed();
      if (!trimmed.isEmpty() && !caps.ui_font_families.contains(trimmed, Qt::CaseInsensitive)) {
        caps.ui_font_families.append(trimmed);
      }
    }
  }

  const QJsonObject tokens = manifest.value(QStringLiteral("tokens")).toObject();
  ReadBoolCapability(tokens, QStringLiteral("inheritPalette"), &caps.inherit_palette_tokens);
}

void ReadManifest(const QString &theme_directory, ThemeInfo *info)
{
  const QString manifest_path = theme_directory + QStringLiteral("/manifest.json");
  if (!QFileInfo::exists(manifest_path)) {
    return;
  }

  QFile manifest(manifest_path);
  if (!manifest.open(QIODevice::ReadOnly)) {
    qWarning() << "[ThemeManager] Cannot read manifest:" << manifest_path;
    return;
  }

  QJsonParseError parse_error;
  const QJsonDocument doc = QJsonDocument::fromJson(manifest.readAll(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
    qWarning() << "[ThemeManager] Invalid manifest for" << info->id << ":"
               << parse_error.errorString();
    return;
  }

  const QJsonObject obj = doc.object();
  const QString name = obj.value(QStringLiteral("name")).toString();
  const QString author = obj.value(QStringLiteral("author")).toString();
  const QString version = obj.value(QStringLiteral("version")).toString();
  if (!name.isEmpty()) {
    info->name = name;
  }
  if (!author.isEmpty()) {
    info->author = author;
  }
  if (!version.isEmpty()) {
    info->version = version;
  }

  ReadCapabilities(obj, info);
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
    current_theme_id_(QString::fromLatin1(StyleManager::kDefaultStyle))
{
  // Capture the application-wide fallback stack before any theme manifest is
  // applied. GUI-less tests construct ThemeManager with a QCoreApplication.
  if (QApplication *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
    default_font_families_ = app->font().families();
  }

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

  // Built-in themes are always first and use a stable order. Their resources
  // live under the same :/style/<id>/ convention as the legacy StyleManager,
  // so the rest of this file can treat built-ins and external folders alike.
  const auto add_builtin = [this](const char *id, const char *display_name,
                                  const char *author, const char *version) {
    ThemeInfo info;
    info.id = QString::fromLatin1(id);
    info.name = QString::fromLatin1(display_name);
    info.author = QString::fromLatin1(author);
    info.version = QString::fromLatin1(version);
    info.path = QStringLiteral(":/style/%1").arg(info.id);
    info.is_builtin = true;
    ReadManifest(info.path, &info);
    themes_.append(info);
  };

  add_builtin(capcut::kThemeName, capcut::kThemeDisplayName, "Olive Team", "1.0");
  add_builtin(metro::kThemeName, metro::kThemeDisplayName, "Olive Team", "2.0");
  add_builtin("olive-dark", "Olive Dark", "Olive Team", "1.0");
  add_builtin("olive-light", "Olive Light", "Olive Team", "1.0");

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

      ReadManifest(entry.filePath(), &info);

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
  last_error_.clear();

  if (themes_.isEmpty()) {
    ScanThemeDirectories();
  }

  const ThemeInfo *info = FindTheme(theme_id);
  if (!info) {
    return Fail(QStringLiteral("theme '%1' was not found").arg(theme_id));
  }

  if (!file_watcher_.files().isEmpty()) {
    file_watcher_.removePaths(file_watcher_.files());
  }
  reload_timer_.stop();

  QString error;
  if (!ApplyThemeResources(*info, &error)) {
    qWarning() << "[ThemeManager] Failed to apply theme" << info->id << ":" << error;
    last_error_ = error;

    // Never recurse when the default itself is broken. For any other theme,
    // fall back to the known-good built-in and keep the original error visible.
    if (info->id != QString::fromLatin1(StyleManager::kDefaultStyle)) {
      FallbackToDefault();
      last_error_ = error;
    }
    return false;
  }

  const QString previous_id = current_theme_id_;
  const ThemeCapabilities previous_capabilities = current_capabilities_;

  current_theme_id_ = info->id;
  current_capabilities_ = info->capabilities;
  StyleManager::SetCurrentStyleId(info->id);
  ApplyThemeFont(*info);

  if (info->is_builtin) {
    qInfo() << "[ThemeManager] Applied built-in theme" << info->id;
  } else {
    qInfo() << "[ThemeManager] Applied theme" << info->id << "from" << info->path;
  }

  // Hot reloads of QSS/tokens/palette should repaint immediately, but they
  // must not rebuild the workspace. Only an id or capability change does that.
  if (previous_id != info->id || previous_capabilities != info->capabilities) {
    emit ThemeChanged(info->id);
  }

  return true;
}

bool ThemeManager::ApplyThemeResources(const ThemeInfo &info, QString *error)
{
  const QString resource_root = info.is_builtin
      ? QStringLiteral(":/style/%1").arg(info.id)
      : info.path;

  // 1. Resolve the stylesheet first so an unreadable theme never mutates the UI.
  QString qss_path;
  if (info.is_pure_qss) {
    qss_path = info.path;
  } else {
    qss_path = resource_root + QStringLiteral("/style.qss");
    if (!QFileInfo(qss_path).isFile()) {
      qss_path = resource_root + QStringLiteral("/style.css");
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

  // 2. Resolve the palette. Built-in and external folders use the same
  //    convention; pure QSS files keep the Metro fallback.
  QString palette_path = QStringLiteral(":/style/olive-metro/palette.ini");
  if (!info.is_pure_qss) {
    palette_path = resource_root + QStringLiteral("/palette.ini");
    if (!QFileInfo::exists(palette_path)) {
      palette_path = QStringLiteral(":/style/olive-metro/palette.ini");
    }
  }

  QPalette palette;
  if (!ParsePaletteFile(palette_path, &palette)) {
    *error = QStringLiteral("cannot parse palette '%1'").arg(palette_path);
    return false;
  }

  // 3. Load optional design tokens after the palette-derived defaults so theme
  //    tokens always win.
  DesignTokenRegistry *registry = DesignTokenRegistry::instance();
  registry->ResetToDefaults();
  if (!info.is_pure_qss && info.capabilities.inherit_palette_tokens) {
    registry->SetColors(PaletteTokens(palette));
  }

  QString tokens_path;
  if (!info.is_pure_qss) {
    tokens_path = resource_root + QStringLiteral("/tokens.json");
    if (QFileInfo::exists(tokens_path)
        && !registry->LoadFromJsonFile(tokens_path)) {
      *error = QStringLiteral("cannot parse tokens '%1'").arg(tokens_path);
      return false;
    }
  }

  // 4. Preprocess and validate the stylesheet before committing it.
  QList<QssMacroPreprocessor::Diagnostic> diagnostics;
  const QString compiled_qss = QssMacroPreprocessor::Process(
      raw_qss, registry->AllTokens(), &diagnostics);

  if (!diagnostics.isEmpty()) {
    const QssMacroPreprocessor::Diagnostic &first = diagnostics.constFirst();
    *error = QStringLiteral("stylesheet error at line %1: %2")
                 .arg(first.line)
                 .arg(first.message);
    return false;
  }

  // 5. Built-in themes own their icon set; external themes intentionally keep
  //    the current icons because packages cannot ship executable icon logic.
  if (info.is_builtin) {
    icon::LoadAll(resource_root);
  }

  qApp->setPalette(palette);
  qApp->setStyleSheet(compiled_qss);

  // 6. Watch the files that can be hot-reloaded. Built-in resources live in
  //    the resource bundle and never need watching.
  if (!info.is_builtin && hot_reload_enabled_) {
    if (QFileInfo(qss_path).isFile() && !qss_path.startsWith(QLatin1Char(':'))) {
      file_watcher_.addPath(qss_path);
    }
    if (!tokens_path.isEmpty() && QFileInfo::exists(tokens_path)
        && !tokens_path.startsWith(QLatin1Char(':'))) {
      file_watcher_.addPath(tokens_path);
    }
    if (!palette_path.startsWith(QLatin1Char(':')) && QFileInfo::exists(palette_path)) {
      file_watcher_.addPath(palette_path);
    }
    if (!info.is_pure_qss) {
      const QString manifest_path = info.path + QStringLiteral("/manifest.json");
      if (QFileInfo::exists(manifest_path)) {
        file_watcher_.addPath(manifest_path);
      }
    }
  }

  return true;
}

void ThemeManager::ApplyThemeFont(const ThemeInfo &info)
{
  QApplication *app = qobject_cast<QApplication *>(QCoreApplication::instance());
  if (!app) {
    return;
  }

  QStringList families = info.capabilities.ui_font_families;
  if (families.isEmpty()) {
    families = default_font_families_;
  }

  // The manifest can add a Latin face in front of the standard CJK stack, but
  // it must never remove the fallback glyph coverage provided by the app font.
  for (const QString &fallback : default_font_families_) {
    if (!families.contains(fallback, Qt::CaseInsensitive)) {
      families.append(fallback);
    }
  }

  if (families.isEmpty()) {
    return;
  }

  QFont font = app->font();
  font.setFamilies(families);
  app->setFont(font);
}

bool ThemeManager::FallbackToDefault()
{
  return ApplyTheme(QString::fromLatin1(StyleManager::kDefaultStyle));
}

bool ThemeManager::Fail(const QString &message)
{
  last_error_ = message;
  qWarning().noquote() << "[ThemeManager]" << message;
  return false;
}

void ThemeManager::OnFileChanged(const QString &path)
{
  qInfo() << "[ThemeManager] Hot-reload triggered by file change:" << path;

  // Capabilities and fonts live in the manifest. Re-read the theme list so the
  // pending ApplyTheme() sees the new values. QSS/tokens/palette-only changes
  // skip this and therefore do not rebuild the workspace.
  if (QFileInfo(path).fileName() == QStringLiteral("manifest.json")) {
    ScanThemeDirectories();
  }

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
  last_error_.clear();

  const QFileInfo source(source_file_or_dir);
  if (!source.exists()) {
    return Fail(QStringLiteral("source does not exist: '%1'").arg(source_file_or_dir));
  }

  if (source.isDir()) {
    return ImportThemeDirectory(source.filePath(), source.fileName(), overwrite);
  }

  if (!source.isFile()) {
    return Fail(QStringLiteral("source is not a file or directory: '%1'").arg(source_file_or_dir));
  }

  const QString suffix = source.suffix().toLower();

  if (suffix == QStringLiteral("qss")) {
    const QString id = source.completeBaseName();
    if (id.isEmpty()) {
      return Fail(QStringLiteral("stylesheet has no usable file name: '%1'").arg(source.filePath()));
    }

    const QString destination = GetThemesDirectory() + QLatin1Char('/') + source.fileName();

    if (source.canonicalFilePath() == QFileInfo(destination).canonicalFilePath()) {
      RefreshThemes();
      return true;
    }
    if (QFileInfo::exists(destination) && !overwrite) {
      return Fail(QStringLiteral("theme '%1' already exists; enable overwrite to replace it").arg(id));
    }

    QFile input(source.filePath());
    if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) {
      return Fail(QStringLiteral("cannot read '%1'").arg(source.filePath()));
    }

    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Text)) {
      return Fail(QStringLiteral("cannot write '%1'").arg(destination));
    }
    const QByteArray data = input.readAll();
    if (output.write(data) != data.size() || !output.commit()) {
      return Fail(QStringLiteral("failed to save '%1'").arg(destination));
    }

    RefreshThemes();
    return true;
  }

  if (suffix == QStringLiteral("olivetheme") || suffix == QStringLiteral("zip")) {
    return ImportZipPackage(source.filePath(), source.completeBaseName(), overwrite);
  }

  return Fail(QStringLiteral("unsupported theme package type: '.%1'").arg(suffix));
}

bool ThemeManager::ImportThemeDirectory(const QString &source_dir,
                                        const QString &id,
                                        bool overwrite)
{
  last_error_.clear();

  if (id.isEmpty()) {
    return Fail(QStringLiteral("theme directory has no usable name: '%1'").arg(source_dir));
  }
  if (!HasThemeStyleFile(source_dir)) {
    return Fail(QStringLiteral("theme directory must contain style.qss or style.css: '%1'").arg(source_dir));
  }

  const QString target_dir = GetThemesDirectory();
  const QString destination = target_dir + QLatin1Char('/') + id;
  const QString source_canonical = QFileInfo(source_dir).canonicalFilePath();
  const QString target_canonical = QFileInfo(target_dir).canonicalFilePath();

  // Importing the themes directory itself (or one of its ancestors) would copy
  // the staging directory into itself. Reject that shape outright.
  if (source_canonical == target_canonical
      || (!source_canonical.isEmpty() && target_canonical.startsWith(source_canonical + QLatin1Char('/')))) {
    return Fail(QStringLiteral("refusing to import a directory that contains the themes directory: '%1'")
                    .arg(source_dir));
  }

  if (QFileInfo(source_dir).canonicalFilePath() == QFileInfo(destination).canonicalFilePath()) {
    RefreshThemes();
    return true;
  }

  if (QFileInfo::exists(destination) && !overwrite) {
    return Fail(QStringLiteral("theme '%1' already exists; enable overwrite to replace it").arg(id));
  }

  const QString staging = target_dir + QStringLiteral("/.") + id + QStringLiteral(".import");
  RemovePath(staging);

  if (!CopyDirectoryRecursively(source_dir, staging)) {
    RemovePath(staging);
    return Fail(QStringLiteral("failed to copy theme files from '%1'").arg(source_dir));
  }

  if (QFileInfo::exists(destination)) {
    RemovePath(destination);
  }

  if (!QDir().rename(staging, destination)) {
    RemovePath(staging);
    return Fail(QStringLiteral("failed to move theme into place: '%1'").arg(destination));
  }

  RefreshThemes();
  return true;
}

bool ThemeManager::ImportZipPackage(const QString &source_file,
                                    const QString &id,
                                    bool overwrite)
{
  last_error_.clear();

#ifdef OLIVE_HAVE_QZIPREADER
  QZipReader zip(source_file);
  if (!zip.isReadable()) {
    return Fail(QStringLiteral("cannot read theme package: '%1'").arg(source_file));
  }

  const QList<QZipReader::FileInfo> entries = zip.fileInfoList();
  if (entries.isEmpty()) {
    return Fail(QStringLiteral("theme package is empty: '%1'").arg(source_file));
  }
  if (entries.size() > kMaximumImportedPackageEntries) {
    return Fail(QStringLiteral("theme package contains too many entries (%1 > %2)")
                    .arg(entries.size())
                    .arg(kMaximumImportedPackageEntries));
  }

  qint64 total_bytes = 0;
  for (const QZipReader::FileInfo &entry : entries) {
    if (entry.isDir) {
      continue;
    }
    if (entry.isSymLink || !IsSafeArchivePath(entry.filePath)) {
      return Fail(QStringLiteral("rejecting unsafe archive entry: '%1'").arg(entry.filePath));
    }
    total_bytes += entry.size;
  }

  if (total_bytes > kMaximumImportedPackageBytes) {
    return Fail(QStringLiteral("theme package is too large (%1 bytes)").arg(total_bytes));
  }

  QTemporaryDir temporary(GetThemesDirectory() + QStringLiteral("/.olivetheme-XXXXXX"));
  if (!temporary.isValid()) {
    return Fail(QStringLiteral("cannot create a temporary directory for theme extraction"));
  }

  qint64 extracted_bytes = 0;
  for (const QZipReader::FileInfo &entry : entries) {
    const QString destination = temporary.filePath(entry.filePath);

    if (entry.isDir) {
      if (!QDir().mkpath(destination)) {
        return Fail(QStringLiteral("cannot create archive directory: '%1'").arg(entry.filePath));
      }
      continue;
    }

    if (!QDir().mkpath(QFileInfo(destination).path())) {
      return Fail(QStringLiteral("cannot create archive path: '%1'").arg(entry.filePath));
    }

    const QByteArray data = zip.fileData(entry.filePath);
    extracted_bytes += data.size();
    if (extracted_bytes > kMaximumImportedPackageBytes) {
      return Fail(QStringLiteral("theme package expands beyond the size limit"));
    }

    QSaveFile output(destination);
    if (!output.open(QIODevice::WriteOnly)
        || output.write(data) != data.size()
        || !output.commit()) {
      return Fail(QStringLiteral("cannot extract archive entry: '%1'").arg(entry.filePath));
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
          return Fail(QStringLiteral("theme package has more than one root theme"));
        }
        root = entry.filePath();
      }
    }
  }

  if (root.isEmpty()) {
    return Fail(QStringLiteral("theme package has no style.qss/style.css"));
  }

  return ImportThemeDirectory(root, id, overwrite);
#else
  Q_UNUSED(source_file)
  Q_UNUSED(id)
  Q_UNUSED(overwrite)
  return Fail(QStringLiteral("this build has no Qt ZipReader support; extract the package manually"));
#endif
}

} // namespace olive
