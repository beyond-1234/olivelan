/***

  Olivelan - Non-Linear Video Editor
  Copyright (C) 2026 Olivelan Contributors

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

#include "crashhandler.h"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QMutex>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>
#include <QtGlobal>

#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <unistd.h>


namespace olive {

namespace {

const char *kLogFileName = "olivelan.log";
const char *kSessionFileName = "session.lock";
const char *kCrashFileName = "crash-report.txt";

// Fixed-size, async-signal-safe state filled in by Install().
char g_log_path[PATH_MAX];
char g_session_path[PATH_MAX];
char g_crash_path[PATH_MAX];
char g_version[64];

QMutex g_log_mutex;
QtMessageHandler g_previous_handler = nullptr;

const char *SignalName(int signal_number)
{
  switch (signal_number) {
  case SIGSEGV: return "SIGSEGV (segmentation fault)";
  case SIGABRT: return "SIGABRT (aborted)";
  case SIGFPE:  return "SIGFPE (arithmetic exception)";
  case SIGILL:  return "SIGILL (illegal instruction)";
  case SIGBUS:  return "SIGBUS (bus error)";
  default:      return "unknown signal";
  }
}

void WriteAll(int fd, const char *data, size_t size)
{
  while (size > 0) {
    ssize_t written = ::write(fd, data, size);
    if (written <= 0) {
      return;
    }
    data += written;
    size -= static_cast<size_t>(written);
  }
}

void ChainHandler(QtMsgType type,
                  const QMessageLogContext &context,
                  const QString &message)
{
  {
    QMutexLocker locker(&g_log_mutex);

    QFile file(QString::fromLocal8Bit(g_log_path));
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
      const char *level = "INFO";
      switch (type) {
      case QtDebugMsg: level = "DEBUG"; break;
      case QtInfoMsg: level = "INFO"; break;
      case QtWarningMsg: level = "WARNING"; break;
      case QtCriticalMsg: level = "ERROR"; break;
      case QtFatalMsg: level = "FATAL"; break;
      }

      file.write(QStringLiteral("[%1] [%2] %3\n")
                     .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                          QString::fromLatin1(level),
                          message)
                     .toUtf8());
    }
  }

  if (g_previous_handler) {
    g_previous_handler(type, context, message);
  }
}

} // namespace

QString CrashHandler::DataDirectory()
{
  QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  if (dir.isEmpty()) {
    dir = QDir::homePath() + QStringLiteral("/.olivelan");
  }

  QDir().mkpath(dir);
  return dir;
}

QString CrashHandler::LogFilePath()
{
  return QDir(DataDirectory()).filePath(QString::fromLatin1(kLogFileName));
}

void CrashHandler::Install()
{
  const QByteArray log_path = LogFilePath().toLocal8Bit();

  if (log_path.size() < PATH_MAX - 1) {
    std::memset(g_log_path, 0, sizeof(g_log_path));
    std::memcpy(g_log_path, log_path.constData(), static_cast<size_t>(log_path.size()));
  }

  const QByteArray session = QDir(DataDirectory()).filePath(QString::fromLatin1(kSessionFileName)).toLocal8Bit();
  const QByteArray crash = QDir(DataDirectory()).filePath(QString::fromLatin1(kCrashFileName)).toLocal8Bit();

  if (session.size() < PATH_MAX - 1) {
    std::memset(g_session_path, 0, sizeof(g_session_path));
    std::memcpy(g_session_path, session.constData(), static_cast<size_t>(session.size()));
  }
  if (crash.size() < PATH_MAX - 1) {
    std::memset(g_crash_path, 0, sizeof(g_crash_path));
    std::memcpy(g_crash_path, crash.constData(), static_cast<size_t>(crash.size()));
  }

  const QByteArray version = QCoreApplication::applicationVersion().toLocal8Bit();
  std::memset(g_version, 0, sizeof(g_version));
  if (version.size() < static_cast<int>(sizeof(g_version))) {
    std::memcpy(g_version, version.constData(), static_cast<size_t>(version.size()));
  }

  // Session marker: created now, removed on a clean shutdown.
  {
    QFile marker(QString::fromLocal8Bit(g_session_path));
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
      marker.write(QStringLiteral("pid=%1\nstarted=%2\nversion=%3\n")
                       .arg(QCoreApplication::applicationPid())
                       .arg(QDateTime::currentDateTime().toString(Qt::ISODate),
                            QCoreApplication::applicationVersion())
                       .toUtf8());
    }
  }

  // Chain the message handler so the log file mirrors stderr output.
  g_previous_handler = qInstallMessageHandler(ChainHandler);

  // Record fatal signals, then let the OS handle them normally.
  struct sigaction action;
  std::memset(&action, 0, sizeof(action));
  action.sa_handler = &CrashHandler::HandleFatalSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESETHAND;
  sigaction(SIGSEGV, &action, nullptr);
  sigaction(SIGABRT, &action, nullptr);
  sigaction(SIGFPE, &action, nullptr);
  sigaction(SIGILL, &action, nullptr);
  sigaction(SIGBUS, &action, nullptr);
}

void CrashHandler::MarkCleanShutdown()
{
  if (g_session_path[0] != '\0') {
    QFile::remove(QString::fromLocal8Bit(g_session_path));
  }
}

void CrashHandler::HandleFatalSignal(int signal_number)
{
  // Async-signal-safe: only open/write/close and formatting into a fixed buffer.
  const int fd = ::open(g_crash_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    char buffer[512];
    const long long epoch = static_cast<long long>(std::time(nullptr));
    const int size = std::snprintf(buffer, sizeof(buffer),
                                   "reason=%s\nsignal=%d\nversion=%s\ntime_utc_epoch=%lld\nlog=%s\n",
                                   SignalName(signal_number),
                                   signal_number,
                                   g_version,
                                   epoch,
                                   g_log_path);
    if (size > 0) {
      WriteAll(fd, buffer, static_cast<size_t>(size < static_cast<int>(sizeof(buffer)) ? size : static_cast<int>(sizeof(buffer)) - 1));
    }
    ::close(fd);
  }

  std::signal(signal_number, SIG_DFL);
  std::raise(signal_number);
}

void CrashHandler::ShowPendingCrashReport(QWidget *parent)
{
  // GUI only: a headless run cannot show a dialog.
  if (!QCoreApplication::instance() || !qobject_cast<QApplication *>(QCoreApplication::instance())) {
    return;
  }

  const QString crash_path = QDir(DataDirectory()).filePath(QString::fromLatin1(kCrashFileName));
  const QString session_path = QDir(DataDirectory()).filePath(QString::fromLatin1(kSessionFileName));

  QFile crash_file(crash_path);
  const bool has_crash_file = crash_file.exists();
  const bool unclean_shutdown = QFile::exists(session_path);

  if (!has_crash_file && !unclean_shutdown) {
    return;
  }

  QString details;
  if (has_crash_file && crash_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    details = QString::fromUtf8(crash_file.readAll()).trimmed();
    crash_file.close();
  }
  if (details.isEmpty()) {
    details = QStringLiteral("reason=terminated without a clean shutdown");
  }

  if (unclean_shutdown) {
    QFile marker(session_path);
    if (marker.open(QIODevice::ReadOnly | QIODevice::Text)) {
      details += QStringLiteral("\nprevious_session=") + QString::fromUtf8(marker.readAll()).trimmed();
      marker.close();
    }
  }

  const QString log_path = LogFilePath();

  QMessageBox box(parent);
  box.setIcon(QMessageBox::Critical);
  box.setWindowTitle(QCoreApplication::translate("CrashHandler", "Olivelan Crash Report"));
  box.setText(QCoreApplication::translate("CrashHandler", "Olivelan did not shut down correctly."));
  box.setInformativeText(QCoreApplication::translate("CrashHandler",
                                "A crash report was written locally. Nothing has been uploaded.\n\n"
                                "Log file:\n%1\n\n"
                                "You can attach the log file to a bug report if you would like help.").arg(log_path));
  box.setDetailedText(details);

  QPushButton *open_folder = box.addButton(QCoreApplication::translate("CrashHandler", "Open Log Folder"), QMessageBox::ActionRole);
  box.addButton(QMessageBox::Close);
  box.setDefaultButton(QMessageBox::Close);

  box.exec();

  if (box.clickedButton() == open_folder) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(log_path).absolutePath()));
  }

  // One report per crash; remove both markers so it is not shown again.
  QFile::remove(crash_path);
  QFile::remove(session_path);
}

} // namespace olive
