// forza - Custom Race: replay a won festival or street event with your own settings.
//
// On a career event's title card (CCareerEventDescription, TU4 only), once the event has
// been won, SELECT opens a Custom Race page built on the game's own difficulty-screen
// layout (like the in-game Graphics menu): car class, laps (circuit races), opponents
// (1-7) and time of day. A confirms: the event's database rows are shadowed for the
// race by TEMP tables (Events, Races, EventParticipants, EventRestrictions) and the
// game's own car selection opens, filtered to the chosen class, then the normal career
// race loads. B returns to the title card and cancels. See src/hooks/custom_race.cpp.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct PPCContext;

namespace forza::custom_race {

// --- Game-independent model (unit-testable) -----------------------------------------

struct ClassChoice {
  uint32_t id;        // CarClasses.Id / Data_Car.ClassID / Events.TargetClass
  const char* label;  // CarClasses code (CLASS_E ... CLASS_R1)
};
inline constexpr std::array<ClassChoice, 9> kClasses{{{1, "E"},
                                                      {2, "D"},
                                                      {3, "C"},
                                                      {4, "B"},
                                                      {5, "A"},
                                                      {6, "S"},
                                                      {7, "R3"},
                                                      {8, "R2"},
                                                      {9, "R1"}}};

struct TimeChoice {
  uint32_t seconds;  // Events.TimeOfDayStart (seconds after midnight); 0 keeps the event's
  const char* label;
};
inline constexpr std::array<TimeChoice, 7> kTimes{{{0, "EVENT DEFAULT"},
                                                   {23400, "DAWN"},
                                                   {32400, "MORNING"},
                                                   {45000, "MIDDAY"},
                                                   {55800, "AFTERNOON"},
                                                   {70200, "SUNSET"},
                                                   {86100, "MIDNIGHT"}}};

inline constexpr uint32_t kMinOpponents = 1, kMaxOpponents = 7;
inline constexpr uint32_t kMinLaps = 1, kMaxLaps = 10;

// CareerTypeId of the events this works on: 1 festival point-to-point, 2 festival circuit,
// 4 street race (point-to-point with traffic).
inline bool SupportedType(uint32_t career_type) {
  return career_type == 1 || career_type == 2 || career_type == 4;
}
inline bool IsCircuit(uint32_t career_type) { return career_type == 2; }

// What the event itself says (read when the title card binds).
struct EventInfo {
  uint32_t id = 0;
  uint32_t career_type = 0;
  uint32_t target_class = 4;
  uint32_t laps = 1;
  uint32_t drivers = 7;
  uint32_t time_of_day = 0;
};

// The player's choices, as indices into the option lists above / plain numbers.
struct Settings {
  uint32_t event = 0;
  uint32_t class_id = 4;
  uint32_t laps = 1;
  uint32_t opponents = 7;
  uint32_t time_of_day = 0;  // seconds, 0 = event default
};

inline uint32_t Clamp(uint32_t v, uint32_t lo, uint32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

inline Settings Defaults(const EventInfo& e) {
  Settings s;
  s.event = e.id;
  s.class_id = Clamp(e.target_class, kClasses.front().id, kClasses.back().id);
  s.laps = Clamp(e.laps, kMinLaps, kMaxLaps);
  s.opponents = Clamp(e.drivers, kMinOpponents, kMaxOpponents);
  s.time_of_day = 0;
  return s;
}

// The SQL that shadows the event for the race (run in order, one statement per call) and
// the SQL that removes the shadow again. TEMP tables are searched before the main
// database, so every query the game makes about the event (title card, car selection's
// class filter, the race setup, the CPU grid) sees the custom values; other events are
// copied unchanged. Written for the game's SQLite 3.6.19 (no CTEs).
std::vector<std::string> ArmStatements(const Settings& s, bool circuit);
std::vector<std::string> DisarmStatements();

// --- Hooks (src/hooks/custom_race.cpp, TU4 only; no-ops in the disc build) ----------

// The difficulty-style screen the Graphics menu also uses (src/hooks/graphics_menu.cpp
// owns its hooks and offers each call here first).
bool TryBind(PPCContext& ctx, uint8_t* base);
bool TryNotify(PPCContext& ctx, uint8_t* base);
bool TryDestroy(PPCContext& ctx, uint8_t* base);
bool TryInput(PPCContext& ctx, uint8_t* base);
// Every main-loop frame (graphics_menu::Tick).
void Tick(PPCContext& ctx, uint8_t* base);
}  // namespace forza::custom_race

namespace forza {
// Player 1's pad (kernel/input_autopress.cpp): watches SELECT for the title card and
// returns the buttons the game should see (adds the SELECT press that hands the
// confirmed Custom Race to the title card's input dispatcher).
uint16_t CustomRacePad(uint16_t buttons);
}  // namespace forza
