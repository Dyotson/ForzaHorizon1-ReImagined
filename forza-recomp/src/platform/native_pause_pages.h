#pragma once

#include "menu_model.h"
#include "native_pause_layout.h"
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
  Garage, Autoshow, TuneCar, Difficulty, Graphics, Audio, Controls, Hud, Video,
  Friends, Chat, SocialStats, FreeRoam, CreateGame, FriendRequests, ConnectionGuide, SocialRoot,
  SessionPage, InviteFriends, LeaveSession, SessionNext,
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
    PauseActionBinding{"car_tuning", PauseAction::TuneCar},
    PauseActionBinding{"difficulty", PauseAction::Difficulty},
    PauseActionBinding{"graphics", PauseAction::Graphics},
    PauseActionBinding{"audio", PauseAction::Audio},
    PauseActionBinding{"controls", PauseAction::Controls},
    PauseActionBinding{"hud", PauseAction::Hud},
    PauseActionBinding{"video", PauseAction::Video},
    PauseActionBinding{"friends", PauseAction::Friends},
    PauseActionBinding{"chat", PauseAction::Chat},
    PauseActionBinding{"social_stats", PauseAction::SocialStats},
    PauseActionBinding{"free_roam", PauseAction::FreeRoam},
    PauseActionBinding{"create_game", PauseAction::CreateGame},
    PauseActionBinding{"friend_requests", PauseAction::FriendRequests},
    PauseActionBinding{"social_connection", PauseAction::ConnectionGuide},
    PauseActionBinding{"social_back", PauseAction::SocialRoot},
    PauseActionBinding{"social_session", PauseAction::SessionPage},
    PauseActionBinding{"session_invite", PauseAction::InviteFriends},
    PauseActionBinding{"session_leave", PauseAction::LeaveSession},
    PauseActionBinding{"session_next", PauseAction::SessionNext},
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
  bool car_tuning = false;
  bool friends = false;
  bool free_roam = false;
  bool create_game = false;
  bool invite_friends = false;
  bool leave_session = false;
  bool session_more = false;
  bool end_free_roam = false;
  bool change_car = false;
};

inline std::vector<Page> MakePausePages(const PauseCapabilities& caps = {}, bool social_help=false,
                                      bool social_session=false,bool camera_layout=false) {
  std::vector<Page> pages{
      {"home", "HOME", {
          {"world_map", "WORLD MAP", {64, 166, 560, 282}, "world_map", true, caps.world_map},
          {"my_cars", "MY CARS", {640, 166, 280, 282}, "change_car", true, caps.change_car},
          {"photo_mode", "PHOTO MODE", {936, 166, 280, 133}, "photo_mode", true, caps.photo_mode},
          {"my_profile", "MY PROFILE", {936, 315, 280, 133}, "my_profile", true, caps.my_profile},
          {"festival_progress", "FESTIVAL PROGRESS", {64, 464, 560, 106}, "festival_progress"},
          {"settings", "SETTINGS", {640, 464, 576, 106}, "settings_page"},
      }},
      // Viewing car information must not depend on the offline Change Car
      // action. Keep each action's capability check on its own tile.
      {"cars", "CARS", {
          {"current_car", "CURRENT CAR", {64, 166, 560, 404}, ""},
          {"change_car", "CHANGE CAR", {640, 166, 280, 194}, "change_car", true, caps.change_car},
          {"car_tuning", "TUNE CAR", {936, 166, 280, 194}, "car_tuning", true, caps.world_map && caps.car_tuning},
          {"garage", "GO TO GARAGE", {640, 376, 280, 194}, "garage", true, caps.world_map && caps.garage},
          {"autoshow", "GO TO AUTOSHOW", {936, 376, 280, 194}, "autoshow", true, caps.world_map && caps.autoshow},
      }},
      {"social", "SOCIAL", {
          {"friends", "FRIENDS", {64, 166, 560, 282}, "friends"},
          {"free_roam", "HORIZON WORLD", {640, 166, 280, 194}, "free_roam", true, caps.free_roam},
          {"create_game", "CREATE GAME", {936, 166, 280, 194}, "create_game", true, caps.create_game},
          {"friend_requests", "REQUESTS", {640, 376, 280, 194}, "friend_requests"},
          {"chat", "CHAT", {936, 376, 280, 194}, "chat"},
          {"social_connection", "SESSION & CONNECTION", {64, 464, 560, 106}, "social_session"},
      }},
      {"settings", "SETTINGS", {
          {"difficulty", "DIFFICULTY", {64, 166, 374, 194}, "difficulty"},
          {"graphics", "GRAPHICS", {454, 166, 372, 194}, "graphics"},
          {"audio", "AUDIO", {842, 166, 374, 194}, "audio"},
          {"controls", "CONTROLS", {64, 376, 374, 194}, "controls"},
          {"hud", "HUD", {454, 376, 372, 194}, "hud"},
          {"video", "VIDEO", {842, 376, 374, 194}, "video"},
      }},

  };
  if(social_help)pages[2].tiles={
      {"social_account","CONNECTION",{64,166,560,282},""},
      {"social_help","FORZA LIVE SETUP",{640,166,576,282},""},
      {"social_stats","CAREER STATISTICS",{64,464,560,106},"social_stats"},
      {"social_back","BACK TO SOCIAL",{640,464,576,106},"social_back"},
  };
  else if(social_session)pages[2].tiles={
      {"session_players","SESSION PLAYERS",{64,166,560,282},caps.session_more?"session_next":""},
      {"session_join","JOIN A FRIEND",{640,166,280,194},"friends",true,caps.friends},
      {"session_invite","INVITE FRIENDS",{936,166,280,194},"session_invite",true,caps.invite_friends},
      {"session_leave",caps.end_free_roam?"END FREE ROAM":"LEAVE SESSION",{640,376,280,194},"session_leave",true,caps.leave_session},
      {"social_connection","CONNECTION & HELP",{936,376,280,194},"social_connection"},
      {"social_back","BACK TO SOCIAL",{64,464,560,106},"social_back"},
  };
  if(camera_layout)native_pause_layout::CameraPages(pages);
  return pages;
}

} // namespace forza::menu
