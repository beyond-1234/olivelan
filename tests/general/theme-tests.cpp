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

bool WriteTextFile(const QString &path, const QByteArray &contents)
{
  QDir().mkpath(QFileInfo(path).path());
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::Text)
      && file.write(contents) == contents.size();
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

OLIVE_ADD_TEST(ThemeQssEdgeCases)
{
  QList<QssMacroPreprocessor::Diagnostic> diagnostics;

  QString out = Preprocess(
      QStringLiteral("#ifndef __OLIVE_TEST_NEVER_DEFINED__\n"
                     "A { color: red; }\n"
                     "#else\n"
                     "B { color: blue; }\n"
                     "#endif\n"),
      &diagnostics);
  OLIVE_ASSERT(diagnostics.isEmpty());
  OLIVE_ASSERT(out.contains(QStringLiteral("A { color: red; }")));
  OLIVE_ASSERT(!out.contains(QStringLiteral("B { color: blue; }")));

  diagnostics.clear();
  Preprocess(QStringLiteral("QWidget { /* unterminated"), &diagnostics);
  OLIVE_ASSERT(!diagnostics.isEmpty());

  diagnostics.clear();
  Preprocess(QStringLiteral("QWidget { font-family: \"unterminated; }"), &diagnostics);
  OLIVE_ASSERT(!diagnostics.isEmpty());

  diagnostics.clear();
  Preprocess(QStringLiteral("QWidget { color: @color.accent.primary.blur(20%); }"), &diagnostics);
  OLIVE_ASSERT(!diagnostics.isEmpty());

  diagnostics.clear();
  Preprocess(QStringLiteral("QWidget { color: var(--missing); }"), &diagnostics);
  OLIVE_ASSERT(!diagnostics.isEmpty());

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

OLIVE_ADD_TEST(ThemeTokenRegistryErrors)
{
  DesignTokenRegistry *registry = DesignTokenRegistry::instance();
  registry->ResetToDefaults();
  const QColor default_accent = registry->GetColor(QStringLiteral("color.accent.primary"));

  OLIVE_ASSERT(!registry->LoadFromJsonString(
      QStringLiteral("{\"color.accent.primary\":\"#not-a-color\"}")));
  OLIVE_ASSERT(registry->GetColor(QStringLiteral("color.accent.primary")) == default_accent);

  OLIVE_ASSERT(!registry->LoadFromJsonString(QStringLiteral("{\"enabled\":true}")));
  OLIVE_ASSERT(!registry->LoadFromJsonString(
      QStringLiteral("{\"color.accent.primary\":\"#112233\",\"bad\":[]}")));
  OLIVE_ASSERT(registry->GetColor(QStringLiteral("color.accent.primary")) == default_accent);

  OLIVE_ASSERT(registry->LoadFromJsonString(
      QStringLiteral("{\"stroke.hairline.width\":3.5}")));
  OLIVE_ASSERT(registry->GetDouble(QStringLiteral("stroke.hairline.width")) == 3.5);

  registry->ResetToDefaults();
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
  OLIVE_ASSERT(WriteTextFile(source_theme + QStringLiteral("/style.qss"),
                             QByteArray("QWidget { color: #ffffff; }")));
  OLIVE_ASSERT(WriteTextFile(
      source_theme + QStringLiteral("/manifest.json"),
      QByteArray("{\"name\":\"Folder Theme\","
                 "\"workspace\":{\"layout\":\"threeColumn\","
                 "\"toolPanel\":false,\"projectView\":\"icon\","
                 "\"projectIconSize\":96,\"timelineHeightPercent\":42},"
                 "\"fonts\":{\"ui\":[\"Inter\"]}}")));
  OLIVE_ASSERT(manager->ImportThemePackage(source_theme));
  OLIVE_ASSERT(QFileInfo::exists(themes_dir + QStringLiteral("/folder-theme/style.qss")));

  const QString source_qss = source_root + QStringLiteral("/single.qss");
  OLIVE_ASSERT(WriteTextFile(source_qss, QByteArray("QWidget { color: #ffffff; }")));
  OLIVE_ASSERT(manager->ImportThemePackage(source_qss));
  OLIVE_ASSERT(QFileInfo::exists(themes_dir + QStringLiteral("/single.qss")));

  // Importing the same folder again must fail and report the reason without
  // touching the existing theme.
  OLIVE_ASSERT(!manager->ImportThemePackage(source_theme));
  OLIVE_ASSERT(manager->LastError().contains(QStringLiteral("already exists")));

  const QString no_style = source_root + QStringLiteral("/no-style");
  OLIVE_ASSERT(QDir().mkpath(no_style));
  OLIVE_ASSERT(!manager->ImportThemePackage(no_style));
  OLIVE_ASSERT(manager->LastError().contains(QStringLiteral("style.qss")));

  const QString unsupported = source_root + QStringLiteral("/note.txt");
  OLIVE_ASSERT(WriteTextFile(unsupported, QByteArray("not a theme")));
  OLIVE_ASSERT(!manager->ImportThemePackage(unsupported));
  OLIVE_ASSERT(manager->LastError().contains(QStringLiteral("unsupported")));

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

  // Scan should expose both imported and manually placed themes, and use the
  // display name from manifest.json.
  const QString named_theme = themes_dir + QStringLiteral("/named-theme");
  OLIVE_ASSERT(WriteTextFile(named_theme + QStringLiteral("/style.qss"),
                             QByteArray("QWidget { color: #ffffff; }")));
  OLIVE_ASSERT(WriteTextFile(named_theme + QStringLiteral("/manifest.json"),
                             QByteArray("{\"name\":\"Named Theme\",\"author\":\"Test\"}")));
  manager->RefreshThemes();

  const QStringList ids = manager->GetAvailableThemeIds();
  OLIVE_ASSERT(ids.contains(QStringLiteral("folder-theme")));
  OLIVE_ASSERT(ids.contains(QStringLiteral("single")));
  OLIVE_ASSERT(ids.contains(QStringLiteral("named-theme")));
  OLIVE_ASSERT(manager->GetThemeInfo(QStringLiteral("named-theme")).name
               == QStringLiteral("Named Theme"));

  // Built-in manifests expose the same declarative capabilities as external
  // themes. Consumers must not need to compare theme ids.
  const ThemeInfo metro = manager->GetThemeInfo(QStringLiteral("olive-metro"));
  OLIVE_ASSERT(metro.is_builtin);
  OLIVE_ASSERT(metro.capabilities.workspace_layout
               == ThemeCapabilities::WorkspaceLayout::kThreeColumn);
  OLIVE_ASSERT(!metro.capabilities.show_tool_panel);
  OLIVE_ASSERT(!metro.capabilities.show_node_panel);
  OLIVE_ASSERT(!metro.capabilities.show_footage_viewer);
  OLIVE_ASSERT(metro.capabilities.empty_inspector_when_unselected);
  OLIVE_ASSERT(metro.capabilities.project_view == QStringLiteral("icon"));
  OLIVE_ASSERT(metro.capabilities.project_icon_size == 160);
  OLIVE_ASSERT(metro.capabilities.project_icon_height_ratio == 0.82);
  OLIVE_ASSERT(metro.capabilities.ui_font_families.contains(QStringLiteral("Inter")));

  // External packages can opt into workspace/font behavior without shipping
  // executable code; unsupported or missing fields keep the standard layout.
  const ThemeInfo imported = manager->GetThemeInfo(QStringLiteral("folder-theme"));
  OLIVE_ASSERT(imported.capabilities.workspace_layout
               == ThemeCapabilities::WorkspaceLayout::kThreeColumn);
  OLIVE_ASSERT(!imported.capabilities.show_tool_panel);
  OLIVE_ASSERT(imported.capabilities.project_view == QStringLiteral("icon"));
  OLIVE_ASSERT(imported.capabilities.project_icon_size == 96);
  OLIVE_ASSERT(imported.capabilities.timeline_height_percent == 42);
  OLIVE_ASSERT(imported.capabilities.ui_font_families.contains(QStringLiteral("Inter")));

  QDir(themes_dir).removeRecursively();
  QDir(source_root).removeRecursively();
  QStandardPaths::setTestModeEnabled(false);
  OLIVE_TEST_END;
}

}
