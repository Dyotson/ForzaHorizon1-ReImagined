#pragma once

#include "menu_model.h"
#include <array>
#include <optional>
#include <string_view>

// Original menu definitions. Coordinates use a 1280 x 720 design canvas;
// the renderer maps them into its viewport. IDs are persistent selection keys,
// not indices into a serialized game scene or translated instruction stream.
namespace forza::menu {

inline constexpr double kPauseCanvasWidth = 1280;
inline constexpr double kPauseCanvasHeight = 720;

enum class PauseAction {
  WorldMap, SettingsPage, ChangeCar, FestivalProgress, PhotoMode, MyProfile,
  Garage, Autoshow, Difficulty, Graphics, Audio, Controls, Hud, Video,
};

struct PauseActionBinding {
  std::string_view id;
  PauseAction action;
};

inline constexpr std::array kPauseActions{
    PauseActionBinding{"world_map", PauseAction::WorldMap},
    PauseActionBinding{"settings_page", PauseAction::SettingsPage},
    PauseActionBinding{"change_car", PauseAction::ChangeCar},
    PauseActionBinding{"festival_progress", PauseAction::FestivalProgress},
    PauseActionBinding{"photo_mode", PauseAction::PhotoMode},
    PauseActionBinding{"my_profile", PauseAction::MyProfile},
    PauseActionBinding{"garage", PauseAction::Garage},
    PauseActionBinding{"autoshow", PauseAction::Autoshow},
    PauseActionBinding{"difficulty", PauseAction::Difficulty},
    PauseActionBinding{"graphics", PauseAction::Graphics},
    PauseActionBinding{"audio", PauseAction::Audio},
    PauseActionBinding{"controls", PauseAction::Controls},
    PauseActionBinding{"hud", PauseAction::Hud},
    PauseActionBinding{"video", PauseAction::Video},
};

inline std::optional<PauseAction> FindPauseAction(std::string_view id) {
  for (const auto& binding : kPauseActions)
    if (binding.id == id) return binding.action;
  return std::nullopt;
}

// The native adapter derives these from FH1's live action model. Unknown
// availability fails closed; changing layout cannot unlock a game action.
struct PauseCapabilities {
  bool world_map = false;
  bool photo_mode = false;
  bool my_profile = false;
  bool garage = false;
  bool autoshow = false;
};

inline std::vector<Page> MakePausePages(const PauseCapabilities& caps = {}) {
  return {
      {"home", "HOME", {
          {"world_map", "WORLD MAP", {64, 166, 560, 282}, "world_map", true, caps.world_map},
          {"my_cars", "MY CARS", {640, 166, 280, 282}, "change_car", true, caps.world_map},
          {"photo_mode", "PHOTO MODE", {936, 166, 280, 133}, "photo_mode", true, caps.photo_mode},
          {"my_profile", "MY PROFILE", {936, 315, 280, 133}, "my_profile", true, caps.my_profile},
          {"festival_progress", "FESTIVAL PROGRESS", {64, 464, 560, 106}, "festival_progress"},
          {"settings", "SETTINGS", {640, 464, 576, 106}, "settings_page"},
      }},
      {"cars", "CARS", {
          {"current_car", "CURRENT CAR", {64, 166, 560, 404}, ""},
          {"change_car", "CHANGE CAR", {640, 166, 576, 124}, "change_car", true, caps.world_map},
          {"garage", "GO TO GARAGE", {640, 306, 576, 124}, "garage", true, caps.world_map && caps.garage},
          {"autoshow", "GO TO AUTOSHOW", {640, 446, 576, 124}, "autoshow", true, caps.world_map && caps.autoshow},
      }, true, caps.world_map},
      {"settings", "SETTINGS", {
          {"difficulty", "DIFFICULTY", {64, 166, 374, 194}, "difficulty"},
          {"graphics", "GRAPHICS", {454, 166, 372, 194}, "graphics"},
          {"audio", "AUDIO", {842, 166, 374, 194}, "audio"},
          {"controls", "CONTROLS", {64, 376, 374, 194}, "controls"},
          {"hud", "HUD", {454, 376, 372, 194}, "hud"},
          {"video", "VIDEO", {842, 376, 374, 194}, "video"},
      }},
  };
}

} // namespace forza::menu
