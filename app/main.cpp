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

/** \mainpage Olive Video Editor - Code Documentation
 *
 * This documentation is a primarily a developer resource. For information on using Olive, visit the website
 * https://www.olivevideoeditor.org/
 *
 * Use the navigation above to find documentation on classes or source files.
 */

extern "C" {
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
}

#include <csignal>

#include <QApplication>
#include <QCommandLineParser>
#include <QFontDatabase>
#include <QMessageBox>
#include <QSurfaceFormat>
#include <QTranslator>

#include <kddockwidgets/KDDockWidgets.h>

#include "core.h"
#include "common/commandlineparser.h"
#include "common/debug.h"
#include "codec/hal/gpu_session_advisor.h"
#include "codec/hal/hardware_pipeline_factory.h"
#include "config/config.h"
#include "dialog/hal/halbackenddialog.h"
#include "node/project/serializer/serializer.h"
#include "bootstrap/graphicsfallback.h"
#include "version.h"

#ifdef _WIN32
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <Windows.h>
#endif

#ifdef USE_CRASHPAD
#include "common/crashpadinterface.h"
#endif // USE_CRASHPAD

int decompress_project(const QString &project)
{
  if (project.isEmpty()) {
    printf("%s\n", QCoreApplication::translate("main", "No project filename set to decompress").toUtf8().constData());
    return 1;
  }

  QFile project_file(project);
  if (!project_file.open(QFile::ReadOnly)) {
    printf("%s\n", QCoreApplication::translate("main", "Failed to open file \"%1\"").arg(project).toUtf8().constData());
    return 1;
  }

  printf("%s\n", QCoreApplication::translate("main", "Decompressing project...").toUtf8().constData());

  if (!olive::ProjectSerializer::CheckCompressedID(&project_file)) {
    printf("%s\n", QCoreApplication::translate("main", "Failed to decompress, project may be corrupt").toUtf8().constData());
    return 1;
  }

  QByteArray b = project_file.readAll();

  project_file.close();

  QByteArray decompressed = qUncompress(b);

  if (decompressed.isEmpty()) {
    printf("%s\n", QCoreApplication::translate("main", "Failed to decompress, project may be corrupt").toUtf8().constData());
    return 1;
  }

  QFileInfo info(project);

  QString filename;
  QString append;
  int append_num = 0;
  do {
    filename = info.dir().filePath(info.completeBaseName().append(append).append(QStringLiteral(".ovexml")));
    append_num++;
    append = QStringLiteral("-%1").arg(append_num);
  } while(QFileInfo::exists(filename));

  printf("%s\n", QCoreApplication::translate("main", "Outputting to file \"%1\"").arg(filename).toUtf8().constData());

  QFile out(filename);
  if (!out.open(QFile::WriteOnly)) {
    printf("%s\n", QCoreApplication::translate("main", "Failed to open output file \"%1\"").arg(filename).toUtf8().constData());
    return 1;
  }

  out.write(decompressed);
  out.close();

  printf("%s\n", QCoreApplication::translate("main", "Decompressed successfully").toUtf8().constData());
  return 0;
}

int main(int argc, char *argv[])
{
  // Set up debug handler
  qInstallMessageHandler(olive::DebugHandler);

  // Set application metadata
  QCoreApplication::setOrganizationName("olivelan.org");
  QCoreApplication::setOrganizationDomain("olivelan.org");
  QCoreApplication::setApplicationName("Olivelan");
  QGuiApplication::setDesktopFileName("org.olivelan.Olivelan");
  QCoreApplication::setApplicationVersion(olive::kAppVersionLong);

  // Patch 0078 (Phase 9): resolve the display session before anything else. The QPA platform is
  // bound when QApplication is constructed (see below), so a stored preference has to be turned
  // into environment variables here or not at all. Config::Load() depends on the organization /
  // application names set above, and its error paths are safe to reach before Core exists.
  olive::Config::Load();
  olive::GpuSessionAdvisor::ApplySessionPreference();


  //
  // Parse command line arguments
  //

  QVector<QString> args;
#if defined(_WIN32) && defined(UNICODE)
  int wargc;
  LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  args.resize(wargc);
  for (int i=0; i<wargc; i++) {
    args[i] = QString::fromWCharArray(wargv[i]);
  }
  LocalFree(wargv);
#else
  args.resize(argc);
  for (int i=0; i<argc; i++) {
    args[i] = QString::fromLocal8Bit(argv[i]);
  }
#endif

  olive::Core::CoreParams startup_params;

  CommandLineParser parser;

  // Our options
  auto help_option =
      parser.AddOption({QStringLiteral("h"), QStringLiteral("-help")},
                       QCoreApplication::translate("main", "Show this help text"));

  auto version_option =
      parser.AddOption({QStringLiteral("v"), QStringLiteral("-version")},
                       QCoreApplication::translate("main", "Show application version"));

  auto fullscreen_option =
      parser.AddOption({QStringLiteral("f"), QStringLiteral("-fullscreen")},
                       QCoreApplication::translate("main", "Start in full-screen mode"));

  auto export_option =
      parser.AddOption({QStringLiteral("x"), QStringLiteral("-export")},
                       QCoreApplication::translate("main", "Export only (No GUI)"));

  auto ts_option =
      parser.AddOption({QStringLiteral("-ts")},
                       QCoreApplication::translate("main", "Override language with file"),
                       true,
                       QCoreApplication::translate("main", "qm-file"));

  auto decompress_option =
      parser.AddOption({QStringLiteral("d"), QStringLiteral("-decompress")},
                       QCoreApplication::translate("main", "Decompress project file (No GUI)"));

  auto hal_backend_option =
      parser.AddOption({QStringLiteral("hal-backend"), QStringLiteral("-hal-backend")},
                       QCoreApplication::translate("main",
                          "Force a HAL decode backend for this session "
                          "(auto|cuda|videotoolbox|vaapi|software)"),
                       true,
                       QCoreApplication::translate("main", "backend"));

  auto hal_dialog_option =
      parser.AddOption({QStringLiteral("hal-dialog"), QStringLiteral("-hal-dialog")},
                       QCoreApplication::translate("main",
                          "Show the HAL backend selection dialog with probe results on startup"));

#ifdef _WIN32
  auto console_option =
      parser.AddOption({QStringLiteral("c"), QStringLiteral("-console")},
                       QCoreApplication::translate("main", "Launch with debug console"));
#endif // _WIN32

  auto project_argument =
      parser.AddPositionalArgument(QStringLiteral("project"),
                                   QCoreApplication::translate("main", "Project to open on startup"));


  // Qt options re-implemented (add to this as necessary)
  //
  // Because we don't use QCommandLineParser, we must filter out Qt's arguments ourselves. Here,
  // we create them so they're recognized, but never use and also hide them in the "help" text.
  parser.AddOption({QStringLiteral("platform")}, QString(), true, QString(), true);
  parser.AddOption({QStringLiteral("platformpluginpath")}, QString(), true, QString(), true);
  parser.AddOption({QStringLiteral("platformtheme")}, QString(), true, QString(), true);
  parser.AddOption({QStringLiteral("plugin")}, QString(), true, QString(), true);
  parser.AddOption({QStringLiteral("qmljsdebugger")}, QString(), true, QString(), true);
  parser.AddOption({QStringLiteral("qwindowgeometry")}, QString(), true, QString(), true);
  parser.AddOption({QStringLiteral("qwindowicon")}, QString(), true, QString(), true);
  parser.AddOption({QStringLiteral("qwindowtitle")}, QString(), true, QString(), true);
  parser.AddOption({QStringLiteral("reverse")}, QString(), false, QString(), true);
  parser.AddOption({QStringLiteral("session")}, QString(), true, QString(), true);
  parser.AddOption({QStringLiteral("style")}, QString(), true, QString(), true);
  parser.AddOption({QStringLiteral("stylesheet")}, QString(), true, QString(), true);
  parser.AddOption({QStringLiteral("widgetcount")}, QString(), false, QString(), true);

  // Hidden crash option for debugging the crash handling
  auto crash_option = parser.AddOption({QStringLiteral("-crash")}, QString(), true, QString(), true);

  // Patch 0095: hidden continuous-playback benchmark. Not part of the user-facing CLI.
  auto autoplay_option = parser.AddOption({QStringLiteral("-autoplay")}, QString(), true,
                                          QStringLiteral("seconds"), true);

  parser.Process(args);

  if (help_option->IsSet()) {
    // Show help
    parser.PrintHelp(argv[0]);
    return 0;
  }

  if (version_option->IsSet()) {
    // Print version
    printf("%s\n", QCoreApplication::applicationVersion().toUtf8().constData());
    return 0;
  }

  if (decompress_option->IsSet()) {
    return decompress_project(project_argument->GetSetting());
  }

  if (export_option->IsSet()) {
    startup_params.set_run_mode(olive::Core::CoreParams::kHeadlessExport);
  }

  if (ts_option->IsSet()) {
    if (ts_option->GetSetting().isEmpty()) {
      qWarning() << "--ts was set but no translation file was provided";
    } else {
      startup_params.set_startup_language(ts_option->GetSetting());
    }
  }

  if (crash_option->IsSet()) {
    startup_params.set_crash_on_startup(true);
  }

  if (autoplay_option->IsSet()) {
    bool ok = false;
    const int seconds = autoplay_option->GetSetting().toInt(&ok);
    if (ok && seconds > 0) {
      startup_params.set_autoplay_seconds(seconds);
    } else {
      qWarning() << "--autoplay expects a positive number of seconds";
    }
  }

  // HAL backend forcing (phase5-07). Environment variable is the lowest-priority source;
  // -hal-backend overrides it, and the -hal-dialog selection overrides both.
  {
    const QByteArray env_backend = qgetenv("OLIVE_HAL_BACKEND");
    if (!env_backend.isEmpty()) {
      olive::HardwarePipelineFactory::SetForcedBackend(QString::fromLocal8Bit(env_backend));
    }
    if (hal_backend_option->IsSet()) {
      olive::HardwarePipelineFactory::SetForcedBackend(hal_backend_option->GetSetting());
    }
  }

  startup_params.set_fullscreen(fullscreen_option->IsSet());

  startup_params.set_startup_project(project_argument->GetSetting());

  // Set OpenGL display profile
  QSurfaceFormat format;

  // Tries to cover all bases. If drivers don't support 3.2, they should fallback to the closest
  // alternative. Unfortunately Qt doesn't support 3.0-3.1 without DeprecatedFunctions, so we
  // declare that too. We also force Qt to not use ANGLE because I've had a lot of problems with it
  // so far.
  //
  // https://bugreports.qt.io/browse/QTBUG-46140
  QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
  format.setVersion(3, 2);
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setRenderableType(QSurfaceFormat::OpenGL);

  format.setDepthBufferSize(24);
  QSurfaceFormat::setDefaultFormat(format);

  // Enable application automatically using higher resolution images from icons
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
  QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif

  QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

  // Create application instance
  std::unique_ptr<QCoreApplication> a;

  if (startup_params.run_mode() == olive::Core::CoreParams::kRunNormal) {
#ifdef _WIN32
    // Since Olive is linked with the console subsystem (for better POSIX compatibility), a console
    // is created by default. If the user didn't request one, we free it here.
    if (!console_option->IsSet()) {
      FreeConsole();
    }
#endif // _WIN32

    a.reset(new QApplication(argc, argv));

    // Patch 0078 (Phase 9): record the platform that was actually selected, so the session choice
    // can be asserted from a log rather than guessed.
    qInfo() << "[GPU Session] QApplication platform:" << QGuiApplication::platformName();

    // Graphics capability is independent of Core/Project/Node state. Probe it
    // before any MainWindow or render thread exists.
    const olive::GraphicsFallback::Result graphics_result =
        olive::GraphicsFallback::EnsureUsableOpenGL();
    if (graphics_result == olive::GraphicsFallback::Result::Relaunching) {
      return 0;
    }
    if (graphics_result == olive::GraphicsFallback::Result::Unavailable) {
      const QString message = QCoreApplication::translate(
          "main",
          "Olive could not create an OpenGL context. The viewer cannot start.\n\n"
          "%1\n\n"
          "Try an X11/XWayland session or install/update the GPU driver. "
          "The diagnostics log contains more details.")
          .arg(olive::GraphicsFallback::LastError());
      qCritical().noquote() << message;
      QMessageBox::critical(nullptr,
                            QCoreApplication::translate("main", "OpenGL unavailable"),
                            message);
      return 1;
    }

    // Initialize the default UI font stack. Inter is registered so theme
    // manifests can opt into it, but it is not forced globally: the embedded
    // Source Han Sans font remains the default family for themes that do not
    // declare a font stack.
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/Inter.ttf"));

    QStringList ui_families;
    int cjk_font_id = QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/DefaultSans.otf"));
    if (cjk_font_id != -1) {
      ui_families.append(QFontDatabase::applicationFontFamilies(cjk_font_id));
    } else {
      // Fallback geometric sans Chinese font stack
#if defined(Q_OS_WIN)
      ui_families << QStringLiteral("Source Han Sans SC") << QStringLiteral("Microsoft YaHei UI") << QStringLiteral("SimHei");
#elif defined(Q_OS_MAC)
      ui_families << QStringLiteral("Source Han Sans SC") << QStringLiteral("PingFang SC") << QStringLiteral("Heiti SC");
#else
      ui_families << QStringLiteral("Source Han Sans SC") << QStringLiteral("Noto Sans CJK SC") << QStringLiteral("WenQuanYi Micro Hei");
#endif
    }

    ui_families.removeDuplicates();
    QFont app_font = QApplication::font();
    if (!ui_families.isEmpty()) {
      app_font.setFamilies(ui_families);
    }
    QApplication::setFont(app_font);

    // KDDockWidgets 2.x requires a frontend to be selected before any MainWindow/DockWidget is
    // created. Olive only links the QtWidgets frontend (see ext/CMakeLists.txt).
    KDDockWidgets::initFrontend(KDDockWidgets::FrontendType::QtWidgets);
  } else {
    a.reset(new QCoreApplication(argc, argv));
  }

#ifdef _WIN32
  // On Windows, users seem to frequently run into a crash caused by their graphics driver not
  // supporting framebuffers, which we require. I personally have only been able to recreate this
  // when no driver is installed (e.g. when using the Microsoft Basic Display Adapter). Whether
  // that's true for all users or not is still up in the air, but what we do know is it's a driver
  // issue and users should know what to do rather than simply receive a cryptic crash report.
  QOpenGLContext ctx;
  ctx.create();
  QOffscreenSurface surface;
  surface.create();
  ctx.makeCurrent(&surface);
  bool has_proc_address = wglGetProcAddress("glGenFramebuffers");
  std::string gpu_vendor = reinterpret_cast<const char*>(ctx.functions()->glGetString(GL_VENDOR));
  std::string gpu_renderer = reinterpret_cast<const char*>(ctx.functions()->glGetString(GL_RENDERER));
  std::string gpu_version = reinterpret_cast<const char*>(ctx.functions()->glGetString(GL_VERSION));
  ctx.doneCurrent();
  surface.destroy();

  if (!has_proc_address) {
    QString msg = QCoreApplication::translate("main",
      "Your computer's graphics driver does not appear to support framebuffers. "
      "This most likely means either your graphics driver is not up-to-date or your graphics card is too old to run Olive.\n\n"
      "Please update your graphics driver to the latest version and try again.\n\n"
      "Current driver information: %1 %2 %3").arg(QString::fromStdString(gpu_vendor), QString::fromStdString(gpu_renderer), QString::fromStdString(gpu_version));

    if (dynamic_cast<QGuiApplication*>(a.get())) {
      QMessageBox::critical(nullptr, QString(), msg);
    } else {
      qCritical().noquote() << msg;
    }

    return 1;
  }
#endif

  // Register FFmpeg codecs and filters (deprecated in 4.0+)
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
  av_register_all();
#endif
#if LIBAVFILTER_VERSION_INT < AV_VERSION_INT(7, 14, 100)
  avfilter_register_all();
#endif

  // HAL backend selection dialog (phase5-07). GUI mode only, shown before Core
  // startup so the forced backend applies to every decoder instance. Cancelled
  // dialog keeps whatever was set from -hal-backend / OLIVE_HAL_BACKEND.
  //
  // Patch 0081 (Phase 9): the translator is normally installed by Core::SetStartupLocale(), which
  // runs inside Core::Start() - i.e. *after* this dialog. Without installing one here too, this
  // dialog is the one piece of UI that could never be translated. Declared at this scope so it
  // outlives the dialog; Core later installs its own, which takes precedence in Qt's
  // reverse-order translator lookup.
  QTranslator early_translator;

  if (startup_params.run_mode() == olive::Core::CoreParams::kRunNormal
      && hal_dialog_option->IsSet()
      && dynamic_cast<QApplication*>(a.get())) {
    bool early_loaded = false;
    if (!startup_params.startup_language().isEmpty()) {
      early_loaded = early_translator.load(startup_params.startup_language());
    } else {
      QString locale = olive::Config::Current()[QStringLiteral("Language")].toString();
      if (locale.isEmpty()) {
        locale = QStringLiteral("zh_CN");
      }
      early_loaded = early_translator.load(QStringLiteral(":/ts/%1").arg(locale));
    }

    if (early_loaded) {
      QCoreApplication::installTranslator(&early_translator);
    }

    olive::HALBackendDialog hal_dialog;
    if (hal_dialog.exec() == QDialog::Accepted) {
      olive::HardwarePipelineFactory::SetForcedBackend(hal_dialog.SelectedBackend());
    }
  }

  // Patch 0082 (Phase 9): the user asked to switch display session, which needs a new process. The
  // replacement is already running, so stop here instead of initialising Core behind its back.
  if (olive::GpuSessionAdvisor::RestartLaunched()) {
    return 0;
  }

  // Enable Google Crashpad if compiled with it
#ifdef USE_CRASHPAD
  if (!InitializeCrashpad()) {
    qWarning() << "Failed to initialize Crashpad handler";
  }
#endif // USE_CRASHPAD

  // Start core
  olive::Core c(startup_params);

  c.Start();

  int ret = a->exec();

  // Clear core memory
  c.Stop();

  return ret;
}
