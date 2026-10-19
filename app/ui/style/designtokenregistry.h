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

#ifndef DESIGNTOKENREGISTRY_H
#define DESIGNTOKENREGISTRY_H

#include <QColor>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>

namespace olive {

/**
 * @brief Central design token registry that bridges external theme files (JSON/QSS)
 *        with C++ QPainter-drawn widgets (Timeline clips, NodeGraph, Waveforms, etc.).
 */
class DesignTokenRegistry : public QObject {
  Q_OBJECT

public:
  static DesignTokenRegistry* instance();

  void ResetToDefaults();

  void SetColor(const QString &key, const QColor &color);
  void SetColors(const QHash<QString, QColor> &colors);
  QColor GetColor(const QString &key, const QColor &fallback = QColor()) const;

  void SetInt(const QString &key, int value);
  int GetInt(const QString &key, int fallback = 0) const;

  void SetDouble(const QString &key, double value);
  double GetDouble(const QString &key, double fallback = 0.0) const;

  void SetString(const QString &key, const QString &value);
  QString GetString(const QString &key, const QString &fallback = QString()) const;

  bool LoadFromJsonString(const QString &json_str);
  bool LoadFromJsonFile(const QString &file_path);

  const QHash<QString, QColor>& colors() const { return colors_; }

  /**
   * @brief Flattened color/integer/double/string tokens for the QSS preprocessor.
   */
  QVariantMap AllTokens() const;

signals:
  void TokensChanged();

private:
  DesignTokenRegistry(QObject *parent = nullptr);
  ~DesignTokenRegistry() override = default;

  QHash<QString, QColor> colors_;
  QHash<QString, int> ints_;
  QHash<QString, double> doubles_;
  QHash<QString, QString> strings_;
};

} // namespace olive

#endif // DESIGNTOKENREGISTRY_H
