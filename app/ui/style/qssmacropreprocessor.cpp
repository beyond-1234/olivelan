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

#include "qssmacropreprocessor.h"

#include <QRegularExpression>
#include <QStringList>
#include <algorithm>

namespace olive {

QString QssMacroPreprocessor::Process(const QString &source_qss, const QHash<QString, QColor> &tokens)
{
  QString result = source_qss;

  // 1. Handle OS Conditionals: #ifdef MACOS / #ifdef WINDOWS / #ifdef LINUX
#if defined(Q_OS_MAC)
  const QString current_os = QStringLiteral("MACOS");
#elif defined(Q_OS_WIN)
  const QString current_os = QStringLiteral("WINDOWS");
#else
  const QString current_os = QStringLiteral("LINUX");
#endif

  QRegularExpression ifdef_regex(QStringLiteral("#ifdef\\s+([A-Z]+)\\s*\\n(.*?)#endif"),
                                 QRegularExpression::DotMatchesEverythingOption);
  QRegularExpressionMatchIterator if_it = ifdef_regex.globalMatch(result);
  
  // Replace conditional blocks from end to start to maintain indices
  QStringList matches;
  QList<int> starts, lengths;
  while (if_it.hasNext()) {
    QRegularExpressionMatch match = if_it.next();
    QString target_os = match.captured(1);
    QString block = match.captured(2);
    QString replacement = (target_os == current_os) ? block : QString();
    matches.append(replacement);
    starts.append(match.capturedStart());
    lengths.append(match.capturedLength());
  }
  for (int i = starts.size() - 1; i >= 0; --i) {
    result.replace(starts[i], lengths[i], matches[i]);
  }

  // 2. Handle Color Functions: @token.alpha(0.5), @token.lighten(10%)
  // Regex: @([a-zA-Z0-9_.-]+)\\.([a-zA-Z]+)\\(([^)]+)\\)
  QRegularExpression func_regex(QStringLiteral("@([a-zA-Z0-9_.-]+)\\.([a-zA-Z]+)\\(([^)]+)\\)"));
  QRegularExpressionMatchIterator func_it = func_regex.globalMatch(result);
  QList<QPair<QRegularExpressionMatch, QString>> func_replacements;

  while (func_it.hasNext()) {
    QRegularExpressionMatch m = func_it.next();
    QString token_key = m.captured(1);
    QString func_name = m.captured(2);
    QString arg = m.captured(3).trimmed();

    if (tokens.contains(token_key)) {
      QString eval_color = EvaluateColorFunction(tokens.value(token_key), func_name, arg);
      func_replacements.append(qMakePair(m, eval_color));
    }
  }
  for (int i = func_replacements.size() - 1; i >= 0; --i) {
    const auto &pair = func_replacements[i];
    result.replace(pair.first.capturedStart(), pair.first.capturedLength(), pair.second);
  }

  // 3. Handle Direct Tokens: @token_name
  // Regex: @([a-zA-Z0-9_.-]+)
  QRegularExpression token_regex(QStringLiteral("@([a-zA-Z0-9_.-]+)"));
  QRegularExpressionMatchIterator tok_it = token_regex.globalMatch(result);
  QList<QPair<QRegularExpressionMatch, QString>> tok_replacements;

  while (tok_it.hasNext()) {
    QRegularExpressionMatch m = tok_it.next();
    QString token_key = m.captured(1);
    if (tokens.contains(token_key)) {
      tok_replacements.append(qMakePair(m, tokens.value(token_key).name()));
    }
  }
  for (int i = tok_replacements.size() - 1; i >= 0; --i) {
    const auto &pair = tok_replacements[i];
    result.replace(pair.first.capturedStart(), pair.first.capturedLength(), pair.second);
  }

  return result;
}

QString QssMacroPreprocessor::EvaluateColorFunction(const QColor &base_color, const QString &func_name, const QString &arg)
{
  if (func_name.compare(QStringLiteral("alpha"), Qt::CaseInsensitive) == 0) {
    double a = arg.toDouble();
    a = std::clamp(a, 0.0, 1.0);
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(base_color.red())
        .arg(base_color.green())
        .arg(base_color.blue())
        .arg(a);
  } else if (func_name.compare(QStringLiteral("lighten"), Qt::CaseInsensitive) == 0) {
    QString clean_arg = arg;
    int percent = clean_arg.remove('%').trimmed().toInt();
    return base_color.lighter(100 + percent).name();
  } else if (func_name.compare(QStringLiteral("darken"), Qt::CaseInsensitive) == 0) {
    QString clean_arg = arg;
    int percent = clean_arg.remove('%').trimmed().toInt();
    return base_color.darker(100 + percent).name();
  }

  return base_color.name();
}

} // namespace olive
