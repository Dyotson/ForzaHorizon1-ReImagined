// Achievement events wait for a native HUD control; no ImGui dialog is registered.
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace rex::system { class AchievementManager; }
namespace forza::achievement_hud {
struct Notice {
  uint32_t id = 0, gamerscore = 0;
  std::string name;
  uint64_t revision = 0;
  int64_t remaining = 4500;
};

// The notification callback can run on any thread. Only the guest UI thread
// starts/pauses display time, so loading screens and higher-priority notices
// cannot consume an unlock before the player has a chance to see it.
class Queue {
 public:
  void Push(uint32_t id, std::string name, uint32_t gamerscore) {
    std::lock_guard lock(mutex_);
    if (!pending_.insert(id).second) return;
    notices_.push_back({id, gamerscore, std::move(name), ++revision_, 4500});
  }
  std::optional<Notice> Read(int64_t now) {
    std::lock_guard lock(mutex_);
    if (notices_.empty()) return std::nullopt;
    const auto left = Remaining(now);
    if (left <= 0) {
      pending_.erase(notices_.front().id);
      notices_.pop_front();
      showing_ = false;
      return std::nullopt;  // allow HIDE before the next queued unlock
    }
    auto notice = notices_.front();
    notice.remaining = left;
    return notice;
  }
  void Resume(int64_t now) {
    std::lock_guard lock(mutex_);
    if (showing_ || notices_.empty()) return;
    showing_ = true;
    since_ = now;
  }
  void Pause(int64_t now) {
    std::lock_guard lock(mutex_);
    if (showing_ && !notices_.empty()) notices_.front().remaining = Remaining(now);
    showing_ = false;
  }
  void Clear() {
    std::lock_guard lock(mutex_);
    notices_.clear();
    pending_.clear();
    showing_ = false;
  }
 private:
  int64_t Remaining(int64_t now) const {
    return notices_.front().remaining - (showing_ ? std::max<int64_t>(0, now - since_) : 0);
  }
  std::mutex mutex_;
  std::deque<Notice> notices_;
  std::unordered_set<uint32_t> pending_;
  uint64_t revision_ = 0;
  int64_t since_ = 0;
  bool showing_ = false;
};
Queue& Notifications();
}  // namespace forza::achievement_hud

namespace forza {
uint64_t InstallAchievementHud(rex::system::AchievementManager& manager);
void RemoveAchievementHud(rex::system::AchievementManager& manager, uint64_t listener);
}  // namespace forza
