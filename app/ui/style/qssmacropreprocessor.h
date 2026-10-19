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

#ifndef QSSMACROPREPROCESSOR_H
#define QSSMACROPREPROCESSOR_H

#include <QColor>
#include <QHash>
#include <QString>

namespace olive {

/**
 * @brief Lightweight macro preprocessor for QSS stylesheets.
 *        Supports token replacement (@token), color operators (@color.alpha(0.3)),
 *        and OS conditional blocks (#ifdef MACOS ... #endif).
 */
class QssMacroPreprocessor {
public:
  static QString Process(const QString &source_qss, const QHash<QString, QColor> &tokens);

private:
  static QString EvaluateColorFunction(const QColor &base_color, const QString &func_name, const QString &arg);
};

} // namespace olive

#endif // QSSMACROPREPROCESSOR_H
