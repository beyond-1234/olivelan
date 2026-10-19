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

#include "designtokenregistry.h"
#include "metrostyleconfig.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

namespace olive {

DesignTokenRegistry* DesignTokenRegistry::instance()
{
  static DesignTokenRegistry registry;
  return &registry;
}

DesignTokenRegistry::DesignTokenRegistry(QObject *parent)
  : QObject(parent)
{
  ResetToDefaults();
}

void DesignTokenRegistry::ResetToDefaults()
{
  colors_.clear();
  ints_.clear();
  doubles_.clear();
  strings_.clear();

  // Populate from base Metro v2.0 design tokens
  colors_.insert(QStringLiteral("color.canvas.base"), metro::kCanvasBg);
  colors_.insert(QStringLiteral("color.card.fill"), metro::kTileBg);
  colors_.insert(QStringLiteral("color.border.hairline"), metro::kBorderColor);
  colors_.insert(QStringLiteral("color.accent.primary"), metro::kAccentColor);
  colors_.insert(QStringLiteral("color.track.video"), metro::kTrackVideo);
  colors_.insert(QStringLiteral("color.track.audio"), metro::kTrackAudio);

  // Default strokes & geometries
  ints_.insert(QStringLiteral("stroke.hairline.width"), metro::kBorderWidth);
  ints_.insert(QStringLiteral("corner.radius.base"), metro::kCornerRadius);

  emit TokensChanged();
}

void DesignTokenRegistry::SetColor(const QString &key, const QColor &color)
{
  if (colors_.value(key) != color) {
    colors_.insert(key, color);
    emit TokensChanged();
  }
}

QColor DesignTokenRegistry::GetColor(const QString &key, const QColor &fallback) const
{
  return colors_.value(key, fallback);
}

void DesignTokenRegistry::SetInt(const QString &key, int value)
{
  if (ints_.value(key) != value) {
    ints_.insert(key, value);
    emit TokensChanged();
  }
}

int DesignTokenRegistry::GetInt(const QString &key, int fallback) const
{
  return ints_.value(key, fallback);
}

void DesignTokenRegistry::SetDouble(const QString &key, double value)
{
  if (doubles_.value(key) != value) {
    doubles_.insert(key, value);
    emit TokensChanged();
  }
}

double DesignTokenRegistry::GetDouble(const QString &key, double fallback) const
{
  return doubles_.value(key, fallback);
}

void DesignTokenRegistry::SetString(const QString &key, const QString &value)
{
  if (strings_.value(key) != value) {
    strings_.insert(key, value);
    emit TokensChanged();
  }
}

QString DesignTokenRegistry::GetString(const QString &key, const QString &fallback) const
{
  return strings_.value(key, fallback);
}

bool DesignTokenRegistry::LoadFromJsonString(const QString &json_str)
{
  QJsonParseError parse_err;
  QJsonDocument doc = QJsonDocument::fromJson(json_str.toUtf8(), &parse_err);
  if (parse_err.error != QJsonParseError::NoError || !doc.isObject()) {
    qWarning() << "[DesignTokenRegistry] Failed to parse token JSON:" << parse_err.errorString();
    return false;
  }

  QJsonObject obj = doc.object();
  bool modified = false;

  for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
    const QString &key = it.key();
    const QJsonValue &val = it.value();

    if (val.isString()) {
      QString str_val = val.toString();
      if (str_val.startsWith('#') || str_val.startsWith("rgb", Qt::CaseInsensitive)) {
        QColor col(str_val);
        if (col.isValid()) {
          colors_.insert(key, col);
          modified = true;
          continue;
        }
      }
      strings_.insert(key, str_val);
      modified = true;
    } else if (val.isDouble()) {
      double d = val.toDouble();
      if (static_cast<double>(static_cast<int>(d)) == d) {
        ints_.insert(key, static_cast<int>(d));
      } else {
        doubles_.insert(key, d);
      }
      modified = true;
    }
  }

  if (modified) {
    emit TokensChanged();
  }
  return true;
}

bool DesignTokenRegistry::LoadFromJsonFile(const QString &file_path)
{
  QFile file(file_path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    return false;
  }

  return LoadFromJsonString(QString::fromUtf8(file.readAll()));
}

} // namespace olive
