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

#ifndef OLIVE_CRASHHANDLER_H
#define OLIVE_CRASHHANDLER_H

#include <QString>

class QWidget;

namespace olive {

/**
 * @brief Minimal, fully offline crash handling.
 *
 * Responsibilities:
 * - write a rolling log file under the application data directory;
 * - record fatal signals (SIGSEGV/SIGABRT/...) to a small marker file;
 * - on the next start, show a dialog with the captured reason and the log path.
 *
 * There is deliberately **no** networking, symbol collection or automatic
 * upload: nothing ever leaves the machine. The log and marker files can be
 * inspected (or attached to a bug report) manually by the user.
 */
class CrashHandler {
public:
  /// Install the log file, session marker and fatal signal handlers.
  static void Install();

  /// Remove the session marker; called on a clean shutdown.
  static void MarkCleanShutdown();

  /// Show the pending crash report (if any) and clear it. GUI mode only.
  static void ShowPendingCrashReport(QWidget *parent = nullptr);

  /// Absolute path of the rolling log file.
  static QString LogFilePath();

  /// Directory holding the log and crash marker files.
  static QString DataDirectory();

private:
  static void HandleFatalSignal(int signal_number);
};

} // namespace olive

#endif // OLIVE_CRASHHANDLER_H
