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

#ifndef GPU_SESSION_ADVISOR_H
#define GPU_SESSION_ADVISOR_H

#include <QString>

namespace olive {

/**
 * @brief Patch 0078 (Phase 9): resolves which display session to run in.
 *
 * Zero-copy hardware decode requires the decode device to be the presentation device. On a hybrid
 * laptop the compositor owns the iGPU, so the OpenGL context can never sit on the NVIDIA GPU there
 * and Tier 1 (CUDA-GL interop) is structurally impossible. Running as an X11 client instead lets
 * PRIME place the GL context on the NVIDIA GPU, which makes Tier 1 reachable at the cost of the
 * native Wayland session.
 *
 * The QPA platform is bound when QApplication is constructed, so the stored preference can only be
 * applied from main() *before* that - which is what ApplySessionPreference() is for.
 */
class GpuSessionAdvisor {
public:
  enum class Mode {
    NativeSession = 0,
    XwaylandPrime = 1,
  };

  /// @brief True when this process is running under a Wayland session.
  static bool IsWaylandSession();

  /// @brief True when an X display is advertised *and* actually reachable.
  ///
  /// Forcing QT_QPA_PLATFORM=xcb without a reachable display makes Qt abort during startup, so this
  /// is a hard prerequisite for Mode::XwaylandPrime.
  static bool IsXwaylandAvailable();

  /// @brief True when the two modes would actually behave differently.
  ///
  /// Only a Wayland session on a hybrid machine with an NVIDIA GPU has a real choice; everywhere
  /// else both modes end up on the same GPU.
  static bool HasChoice();

  /// @brief Session requested by OLIVE_GPU_SESSION, else the stored preference.
  static Mode ConfiguredMode();

  /// @brief True when OLIVE_GPU_SESSION pins the session, i.e. don't ask the user.
  static bool IsSessionForcedByEnvironment();

  /**
   * @brief Applies the session preference to the environment.
   *
   * MUST be called before QApplication is constructed. Never exports the PRIME variables on their
   * own: under native Wayland they pull GL onto the dGPU while the compositor stays on the iGPU,
   * which makes the whole window fail to present (measured blank white client area, patch 0076).
   */
  static void ApplySessionPreference();

  /// @brief One-line classification, for the log and the dialog.
  static QString DescribeEnvironment();

  /// @brief User-facing explanation of the options, including the discrete-GPU-direct advice.
  static QString DescribeAdvice();
};

} // namespace olive

#endif // GPU_SESSION_ADVISOR_H
