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
#include <QStringList>
#include <QVector>

#include "gpu_environment_probe.h"

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

  /// @brief Patch 0082 (Phase 9): true when this process was launched by RestartWithSession().
  static bool WasLaunchedByRestart();

  /**
   * @brief Patch 0082 (Phase 9): saves the preference, relaunches Olive with it applied, and asks
   *        this process to exit.
   *
   * A session change needs a new process because the QPA platform is bound when QApplication is
   * constructed. Launching the replacement here means the choice takes effect immediately instead
   * of at some unspecified next start.
   *
   * @return true if the replacement process was started; the caller must then let this one exit.
   */
  static bool RestartWithSession(Mode mode);

  /// @brief True once RestartWithSession() has launched the replacement process.
  static bool RestartLaunched();

  /**
   * @brief Applies the session preference to the environment.
   *
   * MUST be called before QApplication is constructed. Never exports the PRIME variables on their
   * own: under native Wayland they pull GL onto the dGPU while the compositor stays on the iGPU,
   * which makes the whole window fail to present (measured blank white client area, patch 0076).
   */
  static void ApplySessionPreference();

  /// @brief One-line classification, for the log.
  static QString DescribeEnvironment();

  /**
   * @brief Which platform-native path Tier 2 resolves to.
   *
   * Tier 2 is not one backend: macOS uses VideoToolbox while Linux uses VA-API DMA-BUF. The
   * recommendation must name the one this build can actually select, otherwise a working macOS
   * path is reported as unavailable and auto routing is described as Tier 3.
   */
  enum class Tier2Backend {
    None,
    VaapiDmaBuf,
    VideoToolbox
  };

  /**
   * @brief Patch 0080 (Phase 9): what the probe actually found on this machine.
   *
   * The advice has to come from this rather than from a fixed paragraph - "use Tier 3" is wrong
   * when Tier 2 is available, and "switch the display to the discrete GPU" is not actionable
   * without knowing whether the dGPU has a connected output at all.
   */
  struct Assessment {
    bool wayland_session{false};
    bool xwayland_available{false};
    bool hybrid{false};
    bool nvidia_present{false};
    bool tier1_possible{false};
    bool cuda_gl_interop_blocked{false};
    bool tier2_possible{false};
    Tier2Backend tier2_backend{Tier2Backend::None};
    VaapiImportStatus tier2_import_status{VaapiImportStatus::Unknown};
    bool nvidia_has_display{false};
    bool nvdec_available{false};
    QString display_gpu;          // vendor driving the display, "unknown" if not yet known
    QString nvidia_name;          // e.g. "NVIDIA GeForce RTX 4060 Laptop GPU"
    QString connected_displays;   // e.g. "card1-eDP-1 (AMD)"
  };

  static Assessment Assess();

  /**
   * @brief One decode tier and the reason for its state, for the HAL status panel.
   *
   * `available` is a capability answer, not a promise that every stream will use the tier:
   * Tier 2 VA-API import is per-stream and Tier 3 is always the final fallback. `state`
   * is the short user-facing label, while `reason` explains the evidence behind it.
   */
  struct TierStatus {
    int tier{0};
    QString backend;
    bool available{false};
    QString state;
    QString reason;
  };

  /// @brief Tier 1/2/3 status with reasons, derived from one probe snapshot.
  static QVector<TierStatus> DescribeTiers(const Assessment& assessment);

  /// @brief The detected facts, one per line, for the UI.
  static QString DescribeSituation(const Assessment& assessment);

  /**
   * @brief Which decode tier this session is expected to use, and what that costs.
   *
   * Says Tier 2 when Tier 2 is available - the honest answer is whatever the factory will pick.
   */
  static QString DescribeEffectivePath(const Assessment& assessment);

  /// @brief Ranked, actionable options derived from the assessment.
  static QStringList RecommendedActions(const Assessment& assessment);

  /// @brief Situation + effective path + actions, as one block of text.
  static QString DescribeAdvice();
};

} // namespace olive

#endif // GPU_SESSION_ADVISOR_H
