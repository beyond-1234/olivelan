#include "hal_session_state.h"

#include <utility>

namespace olive {

HalSessionState& HalSessionState::Instance()
{
  static HalSessionState state;
  return state;
}

void HalSessionState::SetForcedBackend(const QString& name)
{
  std::lock_guard<std::mutex> lock(mutex_);
  forced_backend_ = name.toLower().trimmed();
  // Explicit (re)selection re-arms attempts even for a backend that failed earlier.
  failed_keys_.clear();
}

QString HalSessionState::ForcedBackend() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return forced_backend_;
}

QSet<QString> HalSessionState::FailedKeys() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return failed_keys_;
}

bool HalSessionState::HasFailedKey(const QString& key) const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return failed_keys_.contains(key);
}

void HalSessionState::RecordFailure(const QString& key, const QString& expected_backend)
{
  std::lock_guard<std::mutex> lock(mutex_);

  // Only record the failure if the user has not selected a different backend while this
  // initialization was running.
  if (forced_backend_ == expected_backend) {
    failed_keys_.insert(key);
  }
}

void HalSessionState::SetFallbackHandler(HalFallbackHandler handler)
{
  std::lock_guard<std::mutex> lock(mutex_);
  fallback_handler_ = std::move(handler);
}

void HalSessionState::NotifyFallback(const QString& message)
{
  HalFallbackHandler handler;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (notified_fallbacks_.contains(message)) {
      return;
    }
    notified_fallbacks_.insert(message);
    handler = fallback_handler_;
  }

  // Call outside the lock: a GUI handler may queue work that comes back through this state.
  if (handler) {
    handler(message);
  }
}

void HalSessionState::RecordSelection(int tier, const QString& backend)
{
  std::lock_guard<std::mutex> lock(mutex_);
  last_selected_tier_ = tier;
  last_selected_backend_ = backend;
}

int HalSessionState::LastSelectedTier() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return last_selected_tier_;
}

QString HalSessionState::LastSelectedBackend() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return last_selected_backend_;
}

void HalSessionState::ResetForTests()
{
  std::lock_guard<std::mutex> lock(mutex_);
  forced_backend_.clear();
  failed_keys_.clear();
  notified_fallbacks_.clear();
  fallback_handler_ = {};
  last_selected_tier_ = 0;
  last_selected_backend_.clear();
}

} // namespace olive
