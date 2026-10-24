#ifndef HAL_SESSION_STATE_H
#define HAL_SESSION_STATE_H

#include <functional>
#include <mutex>

#include <QSet>
#include <QString>

namespace olive {

/**
 * @brief Process-wide HAL selection/session state.
 *
 * HardwarePipelineFactory used to own this state, mixing a creational factory with mutable
 * process policy, failure caches, callbacks and status telemetry. Keeping it in a dedicated
 * object makes the factory's remaining responsibility explicit and gives tests one place to
 * reset state.
 *
 * The state is intentionally process-wide because a forced backend and its failure cache are
 * launch-session policy, not properties of a single pipeline instance.
 */
using HalFallbackHandler = std::function<void(const QString&)>;

class HalSessionState {
public:
  static HalSessionState& Instance();

  void SetForcedBackend(const QString& name);
  QString ForcedBackend() const;

  /// @brief Snapshot of failure keys used by the per-stream routing decision.
  QSet<QString> FailedKeys() const;
  bool HasFailedKey(const QString& key) const;

  /// @brief Record a failure only if the user has not selected a different backend meanwhile.
  void RecordFailure(const QString& key, const QString& expected_backend);

  void SetFallbackHandler(HalFallbackHandler handler);
  void NotifyFallback(const QString& message);

  void RecordSelection(int tier, const QString& backend);
  int LastSelectedTier() const;
  QString LastSelectedBackend() const;

  /// @brief Reset all mutable process state. Intended for tests only.
  void ResetForTests();

private:
  HalSessionState() = default;

  mutable std::mutex mutex_;
  QString forced_backend_;
  QSet<QString> failed_keys_;
  QSet<QString> notified_fallbacks_;
  HalFallbackHandler fallback_handler_;
  int last_selected_tier_ = 0;
  QString last_selected_backend_;
};

} // namespace olive

#endif // HAL_SESSION_STATE_H
