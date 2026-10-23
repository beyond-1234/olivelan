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

#ifndef GRAPHICSFALLBACK_H
#define GRAPHICSFALLBACK_H

#include <QString>

namespace olive {

/**
 * @brief Startup-only graphics capability and fallback policy.
 *
 * This module deliberately has no dependency on Core, Project, Node or UI
 * business objects. It only answers:
 *
 *   - can the current Qt platform create an OpenGL 3.2 context?
 *   - if not, should we relaunch with XWayland or software OpenGL?
 *
 * A QApplication must already exist before calling EnsureUsableOpenGL().
 */
class GraphicsFallback {
public:
  enum class Result {
    Ready,
    Relaunching,
    Unavailable
  };

  /**
   * @brief Try the current Qt platform, then XWayland, then software OpenGL.
   *
   * A failed attempt relaunches the process with a marker in
   * OLIVE_GL_FALLBACK so the next process tries the next step exactly once.
   * The caller must return immediately when Relaunching is returned.
   */
  static Result EnsureUsableOpenGL();

  /**
   * @brief Probe the current platform without relaunching.
   */
  static bool CanCreateOpenGLContext();

  /**
   * @brief Human-readable last probe error.
   */
  static QString LastError();

private:
  static Result Relaunch(const QString &fallback_mode,
                         const QString &platform,
                         bool software_gl);
};

} // namespace olive

#endif // GRAPHICSFALLBACK_H
