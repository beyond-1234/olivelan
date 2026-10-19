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
#include <QVariant>
#include <algorithm>

namespace olive {

namespace {

QString CurrentOs()
{
#if defined(Q_OS_MACOS) || defined(Q_OS_MAC)
  return QStringLiteral("MACOS");
#elif defined(Q_OS_WIN)
  return QStringLiteral("WINDOWS");
#else
  return QStringLiteral("LINUX");
#endif
}

struct ConditionalFrame {
  bool parent_active;
  bool condition;
  bool else_seen;
};

QVariant ParseLiteralValue(const QString &value, const QVariantMap &tokens)
{
  const QString trimmed = value.trimmed();

  static const QRegularExpression token_ref(QStringLiteral("^@([A-Za-z0-9_.-]+)$"));
  static const QRegularExpression var_ref(
      QStringLiteral("^var\\(\\s*--([A-Za-z0-9_.-]+)\\s*\\)$"));

  QRegularExpressionMatch ref_match = token_ref.match(trimmed);
  if (!ref_match.hasMatch()) {
    ref_match = var_ref.match(trimmed);
  }
  if (ref_match.hasMatch()) {
    const QString key = ref_match.captured(1);
    if (tokens.contains(key)) {
      return tokens.value(key);
    }
  }

  const QColor color(trimmed);
  if (color.isValid()) {
    return color;
  }

  bool ok = false;
  const int int_value = trimmed.toInt(&ok);
  if (ok) {
    return int_value;
  }

  const double double_value = trimmed.toDouble(&ok);
  if (ok) {
    return double_value;
  }

  return trimmed;
}

QString ColorToCssImpl(const QColor &color)
{
  if (color.alpha() == 255) {
    return color.name(QColor::HexRgb);
  }

  return QStringLiteral("rgba(%1, %2, %3, %4)")
      .arg(color.red())
      .arg(color.green())
      .arg(color.blue())
      .arg(color.alphaF(), 0, 'f', 3);
}

bool IsInsideString(const QString &text, int index)
{
  QChar quote;
  bool escaped = false;

  for (int i = 0; i < index; ++i) {
    const QChar ch = text.at(i);
    if (quote.isNull()) {
      if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
        quote = ch;
      }
    } else if (escaped) {
      escaped = false;
    } else if (ch == QLatin1Char('\\')) {
      escaped = true;
    } else if (ch == quote) {
      quote = QChar();
    }
  }

  return !quote.isNull();
}

bool TokenToCss(const QVariant &value, QString *out)
{
  switch (value.metaType().id()) {
  case QMetaType::QColor:
    *out = ColorToCssImpl(value.value<QColor>());
    return true;
  case QMetaType::Int:
    *out = QString::number(value.toInt());
    return true;
  case QMetaType::Double:
    *out = QString::number(value.toDouble(), 'g', 12);
    return true;
  case QMetaType::QString:
    *out = value.toString();
    return true;
  default:
    return false;
  }
}

} // namespace

QString QssMacroPreprocessor::Process(const QString &source_qss,
                                      const QVariantMap &tokens,
                                      QList<Diagnostic> *diagnostics)
{
  auto add_diagnostic = [diagnostics](int line, const QString &message) {
    if (diagnostics) {
      diagnostics->append({line, message});
    }
  };

  // ---------------------------------------------------------------------------
  // 1. Conditional blocks. This is a line-based parser rather than a regular
  //    expression so nested blocks and #else are handled correctly.
  // ---------------------------------------------------------------------------
  const QString os = CurrentOs();
  const QRegularExpression ifdef_regex(QStringLiteral("^#ifdef\\s+([A-Za-z0-9_]+)\\s*$"));
  const QRegularExpression ifndef_regex(QStringLiteral("^#ifndef\\s+([A-Za-z0-9_]+)\\s*$"));

  QStringList input_lines = source_qss.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
  QStringList processed_lines;
  processed_lines.reserve(input_lines.size());

  QList<ConditionalFrame> stack;
  bool active = true;

  for (int i = 0; i < input_lines.size(); ++i) {
    QString line = input_lines.at(i);
    if (line.endsWith(QLatin1Char('\r'))) {
      line.chop(1);
    }

    const QString trimmed = line.trimmed();
    QRegularExpressionMatch match = ifdef_regex.match(trimmed);
    bool is_if = match.hasMatch();
    bool expect_true = true;
    if (!is_if) {
      match = ifndef_regex.match(trimmed);
      is_if = match.hasMatch();
      expect_true = false;
    }

    if (is_if) {
      const bool condition = (match.captured(1) == os) == expect_true;
      stack.append({active, condition, false});
      active = active && condition;
      processed_lines.append(QString());
      continue;
    }

    if (trimmed == QStringLiteral("#else")) {
      if (stack.isEmpty()) {
        add_diagnostic(i + 1, QStringLiteral("#else without #ifdef/#ifndef"));
      } else {
        ConditionalFrame &frame = stack.last();
        if (frame.else_seen) {
          add_diagnostic(i + 1, QStringLiteral("duplicate #else"));
        }
        frame.else_seen = true;
        active = frame.parent_active && !frame.condition;
      }
      processed_lines.append(QString());
      continue;
    }

    if (trimmed == QStringLiteral("#endif")) {
      if (stack.isEmpty()) {
        add_diagnostic(i + 1, QStringLiteral("#endif without #ifdef/#ifndef"));
      } else {
        const ConditionalFrame frame = stack.takeLast();
        active = frame.parent_active;
      }
      processed_lines.append(QString());
      continue;
    }

    if (trimmed.startsWith(QStringLiteral("#if"))) {
      add_diagnostic(i + 1, QStringLiteral("unsupported preprocessor directive: %1").arg(trimmed));
      processed_lines.append(QString());
      continue;
    }

    processed_lines.append(active ? line : QString());
  }

  if (!stack.isEmpty()) {
    add_diagnostic(input_lines.size(), QStringLiteral("unterminated #ifdef/#ifndef block"));
  }

  QString result = processed_lines.join(QLatin1Char('\n'));

  // ---------------------------------------------------------------------------
  // 2. Local declarations: @name: value; and --name: value;
  //    These override tokens supplied by the theme registry.
  // ---------------------------------------------------------------------------
  QVariantMap local_tokens = tokens;
  static const QRegularExpression declaration_regex(
      QStringLiteral("^\\s*(?:--|@)([A-Za-z0-9_.-]+)\\s*:\\s*(.*?)\\s*;\\s*$"));

  QStringList declaration_lines = result.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
  for (int i = 0; i < declaration_lines.size(); ++i) {
    const QRegularExpressionMatch match = declaration_regex.match(declaration_lines.at(i));
    if (!match.hasMatch()) {
      continue;
    }

    const QString key = match.captured(1);
    const QString raw_value = match.captured(2).trimmed();
    if (raw_value.isEmpty()) {
      add_diagnostic(i + 1, QStringLiteral("empty declaration for token '%1'").arg(key));
    } else {
      local_tokens.insert(key, ParseLiteralValue(raw_value, local_tokens));
    }

    declaration_lines[i].clear();
  }
  result = declaration_lines.join(QLatin1Char('\n'));

  // Resolve declarations that refer to other declarations (a small fixed number
  // of passes is enough for a theme file and avoids unbounded recursion).
  for (int pass = 0; pass < 10; ++pass) {
    bool changed = false;
    for (auto it = local_tokens.begin(); it != local_tokens.end(); ++it) {
      if (it.value().metaType().id() != QMetaType::QString) {
        continue;
      }

      const QVariant resolved = ParseLiteralValue(it.value().toString(), local_tokens);
      if (resolved.metaType().id() != QMetaType::QString || resolved.toString() != it.value().toString()) {
        it.value() = resolved;
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
  }

  // ---------------------------------------------------------------------------
  // 3. Validate braces/strings and strip comments. Comments are replaced by
  //    spaces (newlines are kept) so source line numbers remain stable.
  // ---------------------------------------------------------------------------
  QString cleaned;
  cleaned.reserve(result.size());

  bool in_comment = false;
  int comment_start = 0;
  QChar quote;
  bool escaped = false;
  int brace_depth = 0;
  int negative_depth_line = 0;

  for (int i = 0; i < result.size(); ++i) {
    const QChar ch = result.at(i);

    if (in_comment) {
      if (ch == QLatin1Char('*') && i + 1 < result.size() && result.at(i + 1) == QLatin1Char('/')) {
        cleaned.append(QStringLiteral("  "));
        ++i;
        in_comment = false;
      } else if (ch == QLatin1Char('\n')) {
        cleaned.append(ch);
      } else {
        cleaned.append(QLatin1Char(' '));
      }
      continue;
    }

    if (!quote.isNull()) {
      cleaned.append(ch);
      if (escaped) {
        escaped = false;
      } else if (ch == QLatin1Char('\\')) {
        escaped = true;
      } else if (ch == quote) {
        quote = QChar();
      }
      continue;
    }

    if (ch == QLatin1Char('/') && i + 1 < result.size() && result.at(i + 1) == QLatin1Char('*')) {
      in_comment = true;
      comment_start = i;
      cleaned.append(QStringLiteral("  "));
      ++i;
      continue;
    }

    if (ch == QLatin1Char('"') || ch == QLatin1Char('\'')) {
      quote = ch;
      cleaned.append(ch);
      continue;
    }

    if (ch == QLatin1Char('{')) {
      ++brace_depth;
    } else if (ch == QLatin1Char('}')) {
      --brace_depth;
      if (brace_depth < 0) {
        negative_depth_line = LineForIndex(result, i);
        add_diagnostic(negative_depth_line, QStringLiteral("unexpected '}'"));
        brace_depth = 0;
      }
    }

    cleaned.append(ch);
  }

  if (in_comment) {
    add_diagnostic(LineForIndex(result, comment_start), QStringLiteral("unterminated comment"));
  }
  if (!quote.isNull()) {
    add_diagnostic(LineForIndex(result, result.size()), QStringLiteral("unterminated string"));
  }
  if (brace_depth != 0) {
    add_diagnostic(LineForIndex(result, result.size()), QStringLiteral("unbalanced braces"));
  }

  result = cleaned;

  // ---------------------------------------------------------------------------
  // 4. Color functions: @color.alpha(...) / .lighten(...) / .darken(...)
  // ---------------------------------------------------------------------------
  struct Replacement {
    int start;
    int length;
    QString text;
  };

  auto apply_replacements = [&result](QList<Replacement> &replacements) {
    std::sort(replacements.begin(), replacements.end(),
              [](const Replacement &a, const Replacement &b) { return a.start > b.start; });
    for (const Replacement &replacement : replacements) {
      result.replace(replacement.start, replacement.length, replacement.text);
    }
    replacements.clear();
  };

  {
    static const QRegularExpression function_regex(
        QStringLiteral("@([A-Za-z0-9_.-]+)\\.([A-Za-z]+)\\(([^()]*)\\)"));
    QRegularExpressionMatchIterator it = function_regex.globalMatch(result);
    QList<Replacement> replacements;

    while (it.hasNext()) {
      const QRegularExpressionMatch match = it.next();
      const int line = LineForIndex(result, match.capturedStart());
      if (IsInsideString(result, static_cast<int>(match.capturedStart()))) {
        continue;
      }
      const QString key = match.captured(1);

      if (!local_tokens.contains(key)) {
        add_diagnostic(line, QStringLiteral("unknown token '@%1'").arg(key));
        continue;
      }

      const QVariant value = local_tokens.value(key);
      if (value.metaType().id() != QMetaType::QColor) {
        add_diagnostic(line, QStringLiteral("token '@%1' is not a color").arg(key));
        continue;
      }

      QColor color;
      QString error;
      if (!EvaluateColorFunction(value.value<QColor>(), match.captured(2), match.captured(3),
                                 &color, &error)) {
        add_diagnostic(line, error);
        continue;
      }

      replacements.append({static_cast<int>(match.capturedStart()), static_cast<int>(match.capturedLength()), ColorToCss(color)});
    }

    apply_replacements(replacements);
  }

  // ---------------------------------------------------------------------------
  // 5. var(--token) references.
  // ---------------------------------------------------------------------------
  {
    static const QRegularExpression var_regex(
        QStringLiteral("var\\(\\s*--([A-Za-z0-9_.-]+)\\s*\\)"));
    QRegularExpressionMatchIterator it = var_regex.globalMatch(result);
    QList<Replacement> replacements;

    while (it.hasNext()) {
      const QRegularExpressionMatch match = it.next();
      const int line = LineForIndex(result, match.capturedStart());
      if (IsInsideString(result, static_cast<int>(match.capturedStart()))) {
        continue;
      }
      const QString key = match.captured(1);

      if (!local_tokens.contains(key)) {
        add_diagnostic(line, QStringLiteral("unknown token '--%1'").arg(key));
        continue;
      }

      QString css;
      if (!TokenToCss(local_tokens.value(key), &css)) {
        add_diagnostic(line, QStringLiteral("unsupported token type for '--%1'").arg(key));
        continue;
      }

      replacements.append({static_cast<int>(match.capturedStart()), static_cast<int>(match.capturedLength()), css});
    }

    apply_replacements(replacements);
  }

  // ---------------------------------------------------------------------------
  // 6. Direct @token references.
  // ---------------------------------------------------------------------------
  {
    static const QRegularExpression token_regex(QStringLiteral("@([A-Za-z0-9_.-]+)"));
    QRegularExpressionMatchIterator it = token_regex.globalMatch(result);
    QList<Replacement> replacements;

    while (it.hasNext()) {
      const QRegularExpressionMatch match = it.next();
      const int line = LineForIndex(result, match.capturedStart());
      if (IsInsideString(result, static_cast<int>(match.capturedStart()))) {
        continue;
      }
      const QString key = match.captured(1);

      if (!local_tokens.contains(key)) {
        add_diagnostic(line, QStringLiteral("unknown token '@%1'").arg(key));
        continue;
      }

      QString css;
      if (!TokenToCss(local_tokens.value(key), &css)) {
        add_diagnostic(line, QStringLiteral("unsupported token type for '@%1'").arg(key));
        continue;
      }

      replacements.append({static_cast<int>(match.capturedStart()), static_cast<int>(match.capturedLength()), css});
    }

    apply_replacements(replacements);
  }

  return result;
}

bool QssMacroPreprocessor::EvaluateColorFunction(const QColor &base_color,
                                                 const QString &func_name,
                                                 const QString &arg,
                                                 QColor *out_color,
                                                 QString *error)
{
  QString name = func_name.trimmed();
  QString value = arg.trimmed();

  if (name.compare(QStringLiteral("alpha"), Qt::CaseInsensitive) == 0) {
    bool ok = false;
    double alpha;
    if (value.endsWith(QLatin1Char('%'))) {
      alpha = value.left(value.size() - 1).toDouble(&ok) / 100.0;
    } else {
      alpha = value.toDouble(&ok);
    }

    if (!ok) {
      *error = QStringLiteral("invalid alpha value '%1'").arg(arg);
      return false;
    }

    *out_color = base_color;
    out_color->setAlphaF(std::clamp(alpha, 0.0, 1.0));
    return true;
  }

  if (name.compare(QStringLiteral("lighten"), Qt::CaseInsensitive) == 0
      || name.compare(QStringLiteral("darken"), Qt::CaseInsensitive) == 0) {
    QString percent_text = value;
    if (percent_text.endsWith(QLatin1Char('%'))) {
      percent_text.chop(1);
    }

    bool ok = false;
    const int percent = percent_text.toInt(&ok);
    if (!ok) {
      *error = QStringLiteral("invalid %1 percentage '%2'").arg(name, arg);
      return false;
    }

    if (name.compare(QStringLiteral("lighten"), Qt::CaseInsensitive) == 0) {
      *out_color = base_color.lighter(100 + percent);
    } else {
      *out_color = base_color.darker(100 + percent);
    }
    return true;
  }

  *error = QStringLiteral("unknown color function '%1'").arg(func_name);
  return false;
}

QString QssMacroPreprocessor::ColorToCss(const QColor &color)
{
  return ColorToCssImpl(color);
}

int QssMacroPreprocessor::LineForIndex(const QString &text, int index)
{
  const int bounded_index = std::clamp(index, 0, static_cast<int>(text.size()));
  return 1 + text.left(bounded_index).count(QLatin1Char('\n'));
}

} // namespace olive
