/***

  Olive - Non-Linear Video Editor
  Copyright (C) 2022 Olive Team

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

#include "colormanager.h"

#include <QDir>
#include <QStandardPaths>

#include "common/define.h"
#include "common/filefunctions.h"
#include "common/ociobridge.h"
#include "common/ocioutils.h"
#include "config/config.h"
#include "core.h"
#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;

namespace olive {

#define super Node

// Patch 0037: the OCIO config moved into an opaque Impl so the public header
// stays free of OpenColorIO. The process-wide default config lives here instead
// of in a static class member.
struct ColorManager::Impl
{
  OCIO::ConstConfigRcPtr config;
  QString last_error;
  bool is_custom_config{false};
};

namespace {
OCIO::ConstConfigRcPtr default_config;

struct LegacyColorSpaceAlias
{
  const char *legacy;
  const char *current;
};

// Phase 15: names written by the removed built-in engine or by the legacy
// serializers. These are aliases, not silent identity passthroughs.
constexpr LegacyColorSpaceAlias kLegacyColorSpaceAliases[] = {
  { "scene_linear", "Linear" },
  { "Linear Raw", "Linear" },
  { "sRGB", "sRGB OETF" },
  { "compositing_log", "Filmic Log Encoding" },
  { "default", "Rec.709 OETF" },
  { "data", "Non-Colour Data" },
  { "ACES - ACEScg", "ACEScg" },
  { "ACES - ACES2065-1", "ACES2065-1" },
  { "ACEScg Linear", "ACEScg" },
  { "ACES2065-1 Linear", "ACES2065-1" },
};

QString ResolveLegacyColorSpace(const QString &name, const QStringList &available)
{
  for (const LegacyColorSpaceAlias &alias : kLegacyColorSpaceAliases) {
    if (name.compare(QString::fromUtf8(alias.legacy), Qt::CaseInsensitive) == 0) {
      const QString current = QString::fromUtf8(alias.current);
      if (available.contains(current)) {
        qInfo() << "[Color] Migrating legacy colour space" << name << "to" << current;
        return current;
      }
    }
  }

  return QString();
}
}

ColorManager::ColorManager(Project *project) :
  QObject(project),
  impl_(std::make_unique<Impl>())
{
}

ColorManager::~ColorManager() = default;

void ColorManager::Init()
{
  // Set config to our built-in default
  impl_->config = ColorManagerOCIO::GetDefaultConfig();
  impl_->last_error.clear();
  impl_->is_custom_config = false;
  SetDefaultInputColorSpace(impl_->config ? QString::fromUtf8(impl_->config->getCanonicalName(OCIO::ROLE_DEFAULT)) : QString());
  project()->SetColorReferenceSpace(OCIOUtils::kReferenceSpaceSceneLinear);
}

QString ColorManager::GetLastError() const
{
  return impl_->last_error;
}

bool ColorManager::HasError() const
{
  return !impl_->last_error.isEmpty();
}

bool ColorManager::IsCustomConfig() const
{
  return impl_->is_custom_config;
}

QString ColorManager::GetConfigFilename() const
{
  return project()->GetColorConfigFilename();
}

void ColorManager::SetUpDefaultConfig()
{
  if (!qEnvironmentVariableIsEmpty("OCIO")) {
    // Attempt to set config from "OCIO" environment variable
    try {
      default_config = OCIO::Config::CreateFromEnv();

      return;
    } catch (OCIO::Exception& e) {
      qWarning() << "Failed to load config from OCIO environment variable config:" << e.what();
    }
  }

  // Extract OCIO config - kind of hacky, but it'll work
  QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath(QStringLiteral("ocioconf"));

  FileFunctions::CopyDirectory(QStringLiteral(":/ocioconf"),
                               dir,
                               true);

  qDebug() << "Extracting default OCIO config to" << dir;

  default_config = ColorManagerOCIO::CreateConfigFromFile(QDir(dir).filePath(QStringLiteral("config.ocio")));
}

void ColorManager::SetConfigFilename(const QString &filename)
{
  project()->SetColorConfigFilename(filename);
}

QStringList ColorManager::ListAvailableDisplays()
{
  QStringList displays;

  int number_of_displays = impl_->config->getNumDisplays();

  for (int i=0;i<number_of_displays;i++) {
    displays.append(impl_->config->getDisplay(i));
  }

  return displays;
}

QString ColorManager::GetDefaultDisplay()
{
  return impl_->config->getDefaultDisplay();
}

QStringList ColorManager::ListAvailableViews(QString display)
{
  QStringList views;

  int number_of_views = impl_->config->getNumViews(display.toUtf8());

  for (int i=0;i<number_of_views;i++) {
    views.append(impl_->config->getView(display.toUtf8(), i));
  }

  return views;
}

QString ColorManager::GetDefaultView(const QString &display)
{
  return impl_->config->getDefaultView(display.toUtf8());
}

QStringList ColorManager::ListAvailableLooks()
{
  QStringList looks;

  int number_of_looks = impl_->config->getNumLooks();

  for (int i=0;i<number_of_looks;i++) {
    looks.append(impl_->config->getLookNameByIndex(i));
  }

  return looks;
}

QStringList ColorManager::ListAvailableColorspaces() const
{
  return ColorManagerOCIO::ListAvailableColorspaces(impl_->config);
}

QString ColorManager::GetDefaultInputColorSpace() const
{
  return project()->GetDefaultInputColorSpace();
}

void ColorManager::SetDefaultInputColorSpace(const QString &s)
{
  project()->SetDefaultInputColorSpace(s);
}

QString ColorManager::GetReferenceColorSpace() const
{
  return project()->GetColorReferenceSpace();
}

QString ColorManager::GetCompliantColorSpace(const QString &s)
{
  // An empty transform means "use the project default input space". This is
  // distinct from an unknown name and must not produce an error/warning.
  if (s.isEmpty()) {
    return GetDefaultInputColorSpace();
  }

  const QStringList available = ListAvailableColorspaces();

  if (available.contains(s)) {
    return s;
  }

  const QString migrated = ResolveLegacyColorSpace(s, available);
  if (!migrated.isEmpty()) {
    return migrated;
  }

  // Keep the requested name instead of substituting the project default. A
  // downstream ColorProcessor will then fail loudly (and the UI shows its
  // error path) rather than displaying a different image under the wrong
  // colour-space label.
  qWarning() << "[Color] Unsupported colour space" << s
             << "- keeping the name so the conversion fails explicitly";
  return s;
}

ColorTransform ColorManager::GetCompliantColorSpace(const ColorTransform &transform, bool force_display)
{
  if (transform.is_display() || force_display) {
    // Get display information
    QString display = transform.display();
    QString view = transform.view();
    QString look = transform.look();

    // Check if display still exists in config
    if (!ListAvailableDisplays().contains(display)) {
      qWarning() << "[Color] Unsupported display" << display
                 << "- falling back to default" << GetDefaultDisplay();
      display = GetDefaultDisplay();
    }

    // Check if view still exists in display
    if (!ListAvailableViews(display).contains(view)) {
      qWarning() << "[Color] Unsupported view" << view << "for display" << display
                 << "- falling back to default" << GetDefaultView(display);
      view = GetDefaultView(display);
    }

    // Check if looks still exists
    if (!ListAvailableLooks().contains(look)) {
      if (!look.isEmpty()) {
        qWarning() << "[Color] Unsupported look" << look << "- clearing look";
      }
      look.clear();
    }

    return ColorTransform(display, view, look);

  } else {

    const QString output = GetCompliantColorSpace(transform.output());
    return ColorTransform(output);

  }
}

ColorTransform ColorManager::GetRestorableColorTransform(const ColorTransform &transform)
{
  const QStringList available = ListAvailableColorspaces();

  if (!transform.is_display()) {
    const QString requested = transform.output();
    if (requested.isEmpty()) {
      return ColorTransform(GetDefaultInputColorSpace());
    }

    if (available.contains(requested)) {
      return ColorTransform(requested);
    }

    const QString migrated = ResolveLegacyColorSpace(requested, available);
    if (!migrated.isEmpty()) {
      return ColorTransform(migrated);
    }

    qWarning() << "[Color] Cannot restore removed colour space" << requested
               << "- using project default";

    QString fallback = GetDefaultInputColorSpace();
    if (!available.contains(fallback)) {
      const QString fallback_alias = ResolveLegacyColorSpace(fallback, available);
      fallback = !fallback_alias.isEmpty()
          ? fallback_alias
          : (available.isEmpty() ? GetReferenceColorSpace() : available.first());
    }
    return ColorTransform(fallback);
  }

  QString display = transform.display();
  if (display.isEmpty() || !ListAvailableDisplays().contains(display)) {
    qWarning() << "[Color] Cannot restore removed display" << display
               << "- using default" << GetDefaultDisplay();
    display = GetDefaultDisplay();
  }

  QString view = transform.view();
  if (view.isEmpty() || !ListAvailableViews(display).contains(view)) {
    qWarning() << "[Color] Cannot restore removed view" << view
               << "for display" << display << "- using default" << GetDefaultView(display);
    view = GetDefaultView(display);
  }

  QString look = transform.look();
  if (!look.isEmpty() && !ListAvailableLooks().contains(look)) {
    qWarning() << "[Color] Cannot restore removed look" << look << "- clearing look";
    look.clear();
  }

  return ColorTransform(display, view, look);
}

void ColorManager::GetDefaultLumaCoefs(double *rgb) const
{
  impl_->config->getDefaultLumaCoefs(rgb);
}

Project *ColorManager::project() const
{
  return static_cast<Project*>(parent());
}

bool ColorManager::UpdateConfigFromFilename()
{
  const QString config_filename = GetConfigFilename().trimmed();
  const QString old_default_cs = GetDefaultInputColorSpace();

  if (config_filename.isEmpty()) {
    impl_->config = ColorManagerOCIO::GetDefaultConfig();
    impl_->last_error.clear();
    impl_->is_custom_config = false;

    QStringList available_cs = ListAvailableColorspaces();
    QString new_default = available_cs.contains(old_default_cs)
        ? old_default_cs
        : (impl_->config ? QString::fromUtf8(impl_->config->getCanonicalName(OCIO::ROLE_DEFAULT)) : QString());
    if (new_default.isEmpty() && !available_cs.isEmpty()) {
      new_default = available_cs.first();
    }
    SetDefaultInputColorSpace(new_default);

    emit ConfigChanged(QString());
    return true;
  }

  // Resolve relative path against project directory if project filename is known
  QString resolved_path = config_filename;
  if (QFileInfo(resolved_path).isRelative() && project() && !project()->filename().isEmpty()) {
    const QString candidate = QFileInfo(project()->filename()).dir().filePath(resolved_path);
    if (QFileInfo::exists(candidate)) {
      resolved_path = candidate;
    }
  }

  if (!QFileInfo::exists(resolved_path)) {
    impl_->last_error = tr("OpenColorIO configuration file does not exist: %1").arg(config_filename);
    qWarning() << "[ColorManager]" << impl_->last_error;
    if (!impl_->config) {
      impl_->config = ColorManagerOCIO::GetDefaultConfig();
    }
    emit ConfigLoadFailed(config_filename, impl_->last_error);
    return false;
  }

  try {
    OCIO::ConstConfigRcPtr new_config = ColorManagerOCIO::CreateConfigFromFile(resolved_path);
    if (!new_config) {
      throw OCIO::Exception("Failed to instantiate OpenColorIO configuration object.");
    }
    new_config->validate();

    impl_->config = new_config;
    impl_->last_error.clear();
    impl_->is_custom_config = true;

    QStringList available_cs = ListAvailableColorspaces();
    QString new_default = available_cs.contains(old_default_cs)
        ? old_default_cs
        : QString::fromUtf8(impl_->config->getCanonicalName(OCIO::ROLE_DEFAULT));
    if (new_default.isEmpty() && !available_cs.isEmpty()) {
      new_default = available_cs.first();
    }
    SetDefaultInputColorSpace(new_default);

    emit ConfigChanged(config_filename);
    return true;
  } catch (const OCIO::Exception& e) {
    impl_->last_error = QString::fromUtf8(e.what());
    qWarning() << "[ColorManager] Failed to load OpenColorIO config" << config_filename << ":" << impl_->last_error;
    if (!impl_->config) {
      impl_->config = ColorManagerOCIO::GetDefaultConfig();
    }
    emit ConfigLoadFailed(config_filename, impl_->last_error);
    return false;
  }
}

// Patch 0037: OCIO bridge implementations (see common/ociobridge.h).

OCIO::ConstConfigRcPtr ColorManagerOCIO::GetConfig(const ColorManager *manager)
{
  return manager->impl_->config;
}

OCIO::ConstConfigRcPtr ColorManagerOCIO::CreateConfigFromFile(const QString &filename)
{
  return OCIO::Config::CreateFromFile(filename.toUtf8());
}

OCIO::ConstConfigRcPtr ColorManagerOCIO::GetDefaultConfig()
{
  return default_config;
}

QStringList ColorManagerOCIO::ListAvailableColorspaces(OCIO::ConstConfigRcPtr config)
{
  QStringList spaces;

  if (config) {
    int number_of_colorspaces = config->getNumColorSpaces();

    for (int i=0;i<number_of_colorspaces;i++) {
      spaces.append(config->getColorSpaceNameByIndex(i));
    }
  }

  return spaces;
}



}
