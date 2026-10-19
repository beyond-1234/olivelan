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

#include "testutil.h"

#include "ui/style/designtokenregistry.h"
#include "ui/style/qssmacropreprocessor.h"
#include "ui/style/thememanager.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QVariantMap>

#ifdef OLIVE_HAVE_QZIPREADER
#include <QtCore/private/qzipwriter_p.h>
#endif

namespace olive {

namespace {

QVariantMap TestTokens()
{
  QVariantMap tokens;
  tokens.insert(QStringLiteral("color.accent.primary"), QColor(0x00, 0x78, 0xD4));
  tokens.insert(QStringLiteral("color.card.fill"), QColor(0x1C, 0x1C, 0x22));
  tokens.insert(QStringLiteral("stroke.hairline.width"), 1);
  return tokens;
}

QString Preprocess(const QString &source, QList<QssMacroPreprocessor::Diagnostic> *diagnostics)
{
  return QssMacroPreprocessor::Process(source, TestTokens(), diagnostics);
}

}

OLIVE_ADD_TEST(ThemeQssTokenSubstitution)
{
  QString out = Preprocess(
      QStringLiteral("QWidget { color: @color.accent.primary; border: @stroke.hairline.width; }"),
      nullptr);
  OLIVE_ASSERT(out.contains(QStringLiteral("#0078d4")));
  OLIVE_ASSERT(out.contains(QStringLiteral("border: 1;")));
  OLIVE_ASSERT(!out.contains(QLatin1Char('@')));
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ThemeQssColorFunctions)
{
  QString out = Preprocess(
      QStringLiteral("QWidget { background: @color.accent.primary.alpha(30%); }"),
      nullptr);
  OLIVE_ASSERT(out.contains(QStringLiteral("rgba(0, 120, 212, 0.300)")));

  out = Preprocess(
      QStringLiteral("QWidget { border-color: @color.accent.primary.lighten(20%); }"),
      nullptr);
  OLIVE_ASSERT(!out.contains(QStringLiteral("lighten")));
  OLIVE_ASSERT(!out.contains(QLatin1Char('@')));
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ThemeQssLocalDeclarationsAndVar)
{
  QString out = Preprocess(
      QStringLiteral("@accent: #ff0000;\n"
                     "QWidget { color: @accent; background: var(--accent); }"),
      nullptr);
  OLIVE_ASSERT(out.contains(QStringLiteral("color: #ff0000")));
  OLIVE_ASSERT(out.contains(QStringLiteral("background: #ff0000")));
  OLIVE_ASSERT(!out.contains(QStringLiteral("@accent")));
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ThemeQssNestedConditionals)
{
  QString out = Preprocess(
      QStringLiteral("#ifdef __OLIVE_TEST_NEVER_DEFINED__\n"
                     "A { color: red; }\n"
                     "#ifdef MACOS\n"
                     "B { color: blue; }\n"
                     "#else\n"
                     "C { color: green; }\n"
                     "#endif\n"
                     "#else\n"
                     "D { color: white; }\n"
                     "#endif\n"),
      nullptr);
  OLIVE_ASSERT(out.contains(QStringLiteral("D { color: white; }")));
  OLIVE_ASSERT(!out.contains(QStringLiteral("A { color: red; }")));
  OLIVE_ASSERT(!out.contains(QStringLiteral("B { color: blue; }")));
  OLIVE_ASSERT(!out.contains(QStringLiteral("C { color: green; }")));
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ThemeQssDiagnostics)
{
  QList<QssMacroPreprocessor::Diagnostic> diagnostics;
  Preprocess(QStringLiteral("QWidget { color: red;"), &diagnostics);
  OLIVE_ASSERT(!diagnostics.isEmpty());
  OLIVE_ASSERT(diagnostics.constFirst().line == 1);

  diagnostics.clear();
  Preprocess(QStringLiteral("QWidget { color: @missing; }"), &diagnostics);
  OLIVE_ASSERT(!diagnostics.isEmpty());
  OLIVE_ASSERT(diagnostics.constFirst().message.contains(QStringLiteral("missing")));

  diagnostics.clear();
  Preprocess(QStringLiteral("QWidget { font-family: \"A@B\"; }"), &diagnostics);
  OLIVE_ASSERT(diagnostics.isEmpty());
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ThemeTokenRegistry)
{
  DesignTokenRegistry *registry = DesignTokenRegistry::instance();
  registry->ResetToDefaults();

  const QVariantMap tokens = registry->AllTokens();
  OLIVE_ASSERT(tokens.contains(QStringLiteral("color.accent.primary")));
  OLIVE_ASSERT(tokens.contains(QStringLiteral("stroke.hairline.width")));
  OLIVE_ASSERT(tokens.value(QStringLiteral("color.accent.primary")).metaType().id()
               == QMetaType::QColor);

  OLIVE_ASSERT(registry->LoadFromJsonString(
      QStringLiteral("{\"color.accent.primary\":\"#112233\","
                     "\"stroke.hairline.width\":3}")));
  OLIVE_ASSERT(registry->GetColor(QStringLiteral("color.accent.primary"))
               == QColor(0x11, 0x22, 0x33));
  OLIVE_ASSERT(registry->GetInt(QStringLiteral("stroke.hairline.width")) == 3);

  registry->ResetToDefaults();
  OLIVE_ASSERT(registry->GetColor(QStringLiteral("color.accent.primary"))
               == QColor(0x00, 0x78, 0xD4));
  OLIVE_TEST_END;
}

OLIVE_ADD_TEST(ThemeImportPackages)
{
  int argc = 1;
  char app_name[] = "theme-tests";
  char *argv[] = {app_name, nullptr};
  QCoreApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("OliveTest"));
  QCoreApplication::setApplicationName(QStringLiteral("OliveThemeTest"));
  QStandardPaths::setTestModeEnabled(true);

  ThemeManager *manager = ThemeManager::instance();
  const QString themes_dir = manager->GetThemesDirectory();
  const QString source_root = QDir::tempPath() + QStringLiteral("/olive-theme-import-test");

  QDir(themes_dir).removeRecursively();
  QDir(source_root).removeRecursively();

  const QString source_theme = source_root + QStringLiteral("/folder-theme");
  OLIVE_ASSERT(QDir().mkpath(source_theme));
  {
    QFile style(source_theme + QStringLiteral("/style.qss"));
    OLIVE_ASSERT(style.open(QIODevice::WriteOnly | QIODevice::Text));
    style.write("QWidget { color: #ffffff; }");
  }
  OLIVE_ASSERT(manager->ImportThemePackage(source_theme));
  OLIVE_ASSERT(QFileInfo::exists(themes_dir + QStringLiteral("/folder-theme/style.qss")));

  const QString source_qss = source_root + QStringLiteral("/single.qss");
  {
    QFile style(source_qss);
    OLIVE_ASSERT(style.open(QIODevice::WriteOnly | QIODevice::Text));
    style.write("QWidget { color: #ffffff; }");
  }
  OLIVE_ASSERT(manager->ImportThemePackage(source_qss));
  OLIVE_ASSERT(QFileInfo::exists(themes_dir + QStringLiteral("/single.qss")));

#ifdef OLIVE_HAVE_QZIPREADER
  const QString zip_path = source_root + QStringLiteral("/packaged.olivetheme");
  {
    QZipWriter writer(zip_path);
    writer.addFile(QStringLiteral("packaged/style.qss"),
                   QByteArray("QWidget { color: #ffffff; }"));
    writer.addFile(QStringLiteral("packaged/manifest.json"),
                   QByteArray("{\"name\":\"Packaged\"}"));
  }
  OLIVE_ASSERT(manager->ImportThemePackage(zip_path));
  OLIVE_ASSERT(QFileInfo::exists(themes_dir + QStringLiteral("/packaged/style.qss")));

  const QString unsafe_zip = source_root + QStringLiteral("/unsafe.olivetheme");
  {
    QZipWriter writer(unsafe_zip);
    writer.addFile(QStringLiteral("packaged/style.qss"),
                   QByteArray("QWidget { color: #ffffff; }"));
    writer.addFile(QStringLiteral("packaged/../escape.qss"),
                   QByteArray("QWidget { color: red; }"));
  }
  OLIVE_ASSERT(!manager->ImportThemePackage(unsafe_zip));
  OLIVE_ASSERT(!QFileInfo::exists(source_root + QStringLiteral("/escape.qss")));
#endif

  QDir(themes_dir).removeRecursively();
  QDir(source_root).removeRecursively();
  QStandardPaths::setTestModeEnabled(false);
  OLIVE_TEST_END;
}

}
