// SDK achievement notifications are delivered to the game's native HUD queue.
#include "achievement_toast.h"
#include <rex/logging.h>
#include <rex/system/achievement_manager.h>

namespace forza::achievement_hud {
Queue& Notifications() {
  static Queue queue;
  return queue;
}
}  // namespace forza::achievement_hud

namespace forza {
uint64_t InstallAchievementHud(rex::system::AchievementManager& manager) {
  return manager.RegisterNotificationCallback([](const rex::system::AchievementEvent& event) {
    const auto& achievement = event.achievement;
    achievement_hud::Notifications().Push(achievement.id,
        achievement.label.empty() ? "Achievement unlocked" : achievement.label,
        achievement.gamerscore);
    REXLOG_INFO("ACHIEVEMENT HUD: queued {}: {} ({} G)",
                achievement.id, achievement.label, achievement.gamerscore);
  });
}
void RemoveAchievementHud(rex::system::AchievementManager& manager, uint64_t listener) {
  manager.UnregisterCallback(listener);
  achievement_hud::Notifications().Clear();
}
}  // namespace forza
