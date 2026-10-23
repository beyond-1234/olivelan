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

#include "render/graphicsfallback.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QProcess>
#include <QProcessEnvironment>

#if defined(Q_OS_UNIX)
#include <unistd.h>
#include <vector>
#endif

namespace olive {

namespace {

QString g_last_error;

QString FallbackProgram()
{
  const QString appimage = qEnvironmentVariable("APPIMAGE");
  if (!appimage.isEmpty()) {
    return appimage;
  }

  return QCoreApplication::applicationFilePath();
}

}

bool GraphicsFallback::CanCreateOpenGLContext()
{
  QOffscreenSurface surface;
  surface.create();
  if (!surface.isValid()) {
    g_last_error = QStringLiteral("Failed to create an offscreen surface");
    return false;
  }

  QOpenGLContext context;
  context.setFormat(surface.format());
  if (!context.create()) {
    g_last_error = QStringLiteral("Failed to create an OpenGL context");
    return false;
  }

  if (!context.makeCurrent(&surface)) {
    g_last_error = QStringLiteral("Failed to make the OpenGL context current");
    return false;
  }

  context.doneCurrent();
  surface.destroy();
  return true;
}

QString GraphicsFallback::LastError()
{
  return g_last_error;
}

GraphicsFallback::Result GraphicsFallback::Relaunch(const QString &fallback_mode,
                                                    const QString &platform,
                                                    bool software_gl)
{
  const QString program = FallbackProgram();
  if (program.isEmpty() || !QFileInfo(program).isExecutable()) {
    g_last_error = QStringLiteral("Cannot relaunch: application executable is unavailable");
    return Result::Unavailable;
  }

  qputenv("OLIVE_GL_FALLBACK", fallback_mode.toUtf8());

  if (!platform.isEmpty()) {
    qputenv("QT_QPA_PLATFORM", platform.toUtf8());
  }

  if (software_gl) {
    qputenv("LIBGL_ALWAYS_SOFTWARE", "1");
    qputenv("MESA_LOADER_DRIVER_OVERRIDE", "llvmpipe");
    // Tier 3 is CPU decode. Keep it explicit so a software-GL session never
    // attempts a hardware decoder it cannot use.
    qputenv("OLIVE_HAL_BACKEND", "software");
  }

  const QStringList arguments = QCoreApplication::arguments().mid(1);
  qInfo() << "[Graphics] Relaunching with fallback" << fallback_mode
          << "platform" << (platform.isEmpty() ? QStringLiteral("<current>") : platform)
          << "software_gl" << software_gl;

#if defined(Q_OS_UNIX)
  // Preserve the terminal/stdout/stderr and PID-visible process. A detached
  // QProcess would make the new fallback process lose the user's terminal
  // logs, which makes field diagnostics impossible.
  const QByteArray program_utf8 = program.toLocal8Bit();
  std::vector<QByteArray> argv_storage;
  argv_storage.reserve(arguments.size() + 1);
  argv_storage.push_back(program_utf8);
  for (const QString &argument : arguments) {
    argv_storage.push_back(argument.toLocal8Bit());
  }

  std::vector<char*> argv;
  argv.reserve(argv_storage.size() + 1);
  for (QByteArray &argument : argv_storage) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);

  execv(program_utf8.constData(), argv.data());
  g_last_error = QStringLiteral("execv failed while applying graphics fallback");
  return Result::Unavailable;
#else
  if (!QProcess::startDetached(program, arguments)) {
    g_last_error = QStringLiteral("Failed to relaunch the application");
    return Result::Unavailable;
  }

  return Result::Relaunching;
#endif
}

GraphicsFallback::Result GraphicsFallback::EnsureUsableOpenGL()
{
  if (CanCreateOpenGLContext()) {
    qInfo() << "[Graphics] OpenGL context ready on platform"
            << QGuiApplication::platformName();
    return Result::Ready;
  }

  const QString fallback = qEnvironmentVariable("OLIVE_GL_FALLBACK");
  const QString platform = QGuiApplication::platformName();
  const bool has_display = !qEnvironmentVariableIsEmpty("DISPLAY");

  // Wayland without a working EGL context is the common hybrid/NVIDIA failure
  // mode. XWayland is the least invasive fallback because it can still use the
  // host's GL driver and leaves hardware decode available when possible.
  if (fallback.isEmpty() && platform == QStringLiteral("wayland") && has_display) {
    return Relaunch(QStringLiteral("xwayland"), QStringLiteral("xcb"), false);
  }

  // Software GL is a diagnostic/emergency path. It keeps the UI and Viewer
  // usable with Tier 3 CPU decode, at the cost of performance.
  if (fallback != QStringLiteral("software")) {
    return Relaunch(QStringLiteral("software"),
                    has_display ? QStringLiteral("xcb") : QString(),
                    true);
  }

  g_last_error = QStringLiteral("OpenGL remains unavailable after XWayland and software fallback");
  return Result::Unavailable;
}

} // namespace olive
