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

#include <cmath>
#include <limits>

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
  colors_.insert(QStringLiteral("color.text.primary"), QColor(0xF0, 0xF0, 0xF2));
  colors_.insert(QStringLiteral("color.text.on-accent"), QColor(Qt::white));
  colors_.insert(QStringLiteral("color.badge.10bit"), QColor(0x87, 0x64, 0xB8));

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

void DesignTokenRegistry::SetColors(const QHash<QString, QColor> &colors)
{
  bool changed = false;

  for (auto it = colors.constBegin(); it != colors.constEnd(); ++it) {
    if (colors_.value(it.key()) != it.value()) {
      colors_.insert(it.key(), it.value());
      changed = true;
    }
  }

  if (changed) {
    emit TokensChanged();
  }
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

QVariantMap DesignTokenRegistry::AllTokens() const
{
  QVariantMap tokens;

  for (auto it = colors_.constBegin(); it != colors_.constEnd(); ++it) {
    tokens.insert(it.key(), it.value());
  }
  for (auto it = ints_.constBegin(); it != ints_.constEnd(); ++it) {
    tokens.insert(it.key(), it.value());
  }
  for (auto it = doubles_.constBegin(); it != doubles_.constEnd(); ++it) {
    tokens.insert(it.key(), it.value());
  }
  for (auto it = strings_.constBegin(); it != strings_.constEnd(); ++it) {
    tokens.insert(it.key(), it.value());
  }

  return tokens;
}

bool DesignTokenRegistry::LoadFromJsonString(const QString &json_str)
{
  QJsonParseError parse_err;
  const QJsonDocument doc = QJsonDocument::fromJson(json_str.toUtf8(), &parse_err);
  if (parse_err.error != QJsonParseError::NoError || !doc.isObject()) {
    qWarning() << "[DesignTokenRegistry] Failed to parse token JSON:" << parse_err.errorString();
    return false;
  }

  // Apply into copies first so one bad value cannot leave a half-updated
  // registry behind (the caller may have already changed the palette).
  QHash<QString, QColor> colors = colors_;
  QHash<QString, int> ints = ints_;
  QHash<QString, double> doubles = doubles_;
  QHash<QString, QString> strings = strings_;

  const QJsonObject obj = doc.object();
  for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
    const QString &key = it.key();
    const QJsonValue &val = it.value();

    // A key must have exactly one type. Drop any previous value of another
    // type so switching a token from color to string does not leave stale data.
    colors.remove(key);
    ints.remove(key);
    doubles.remove(key);
    strings.remove(key);

    if (val.isString()) {
      const QString str_val = val.toString();
      const bool looks_like_color = str_val.startsWith(QLatin1Char('#'))
          || str_val.startsWith(QStringLiteral("rgb"), Qt::CaseInsensitive)
          || str_val.startsWith(QStringLiteral("hsl"), Qt::CaseInsensitive);

      if (looks_like_color) {
        const QColor col(str_val);
        if (!col.isValid()) {
          qWarning() << "[DesignTokenRegistry] Invalid color token" << key << ":" << str_val;
          return false;
        }
        colors.insert(key, col);
      } else {
        strings.insert(key, str_val);
      }
    } else if (val.isDouble()) {
      const double d = val.toDouble();
      if (!std::isfinite(d)) {
        qWarning() << "[DesignTokenRegistry] Non-finite number token" << key;
        return false;
      }
      if (d >= std::numeric_limits<int>::min() && d <= std::numeric_limits<int>::max()
          && std::floor(d) == d) {
        ints.insert(key, static_cast<int>(d));
      } else {
        doubles.insert(key, d);
      }
    } else {
      qWarning() << "[DesignTokenRegistry] Unsupported token value for" << key
                 << "(expected color/number/string)";
      return false;
    }
  }

  if (colors != colors_ || ints != ints_ || doubles != doubles_ || strings != strings_) {
    colors_ = colors;
    ints_ = ints;
    doubles_ = doubles;
    strings_ = strings;
    emit TokensChanged();
  }

  return true;
}

bool DesignTokenRegistry::LoadFromJsonFile(const QString &file_path)
{
  QFile file(file_path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qWarning() << "[DesignTokenRegistry] Cannot read token file:" << file_path;
    return false;
  }

  return LoadFromJsonString(QString::fromUtf8(file.readAll()));
}

} // namespace olive
