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
#include <QList>
#include <QString>
#include <QVariantMap>

namespace olive {

/**
 * @brief Lightweight macro preprocessor for QSS stylesheets.
 *
 * Supports:
 *  - @token / var(--token) interpolation for colors, numbers and strings
 *  - local @name: value; / --name: value; declarations
 *  - color operators (@color.alpha(0.3), .lighten(10%), .darken(10%))
 *  - nested #ifdef / #ifndef / #else / #endif blocks
 *
 * All failures are reported as Diagnostics with a source line number so the theme
 * manager can reject a broken theme and fall back to the default style instead of
 * applying a partially valid stylesheet.
 */
class QssMacroPreprocessor {
public:
  struct Diagnostic {
    int line = 0;
    QString message;
  };

  static QString Process(const QString &source_qss,
                         const QVariantMap &tokens,
                         QList<Diagnostic> *diagnostics = nullptr);

private:
  static bool EvaluateColorFunction(const QColor &base_color,
                                    const QString &func_name,
                                    const QString &arg,
                                    QColor *out_color,
                                    QString *error);

  static QString ColorToCss(const QColor &color);

  static int LineForIndex(const QString &text, int index);
};

} // namespace olive

#endif // QSSMACROPREPROCESSOR_H
