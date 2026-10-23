#pragma once
#include <cstdint>

namespace forza::ambient_leaderboard {
// Speed traps and speed zones (CAmbientChallengeLeaderboard, #288). At a trap the game reads
// GetRows(type 1 or 2, view 5 "Friends & Club", board, from 1, 200 rows) into the object: the
// rows at +0x58 (200 of 0xC0 bytes, the score float at +0x58 of a row), their count at +0x9718
// and the player's own row at +0x9658 (its first word, the position). A new score is placed by
// walking up from the player's position (or from below the last row when they have none) while
// it beats the row above: row[place - 2]. Turn 10's Friends & Club was a small group the player
// belonged to, so the position always fell within the rows. A position past them (a server that
// ranks everyone and sends the top 200, with the player at 713) indexes beyond the array, and
// the game read unmapped memory there.
constexpr int32_t kMaxRows = 200;

struct Start {
  int32_t place;  // 1-based place the walk starts from
  bool clamped;   // the game's own value would have read past the rows
};

// The game's start (position, or the row count + 1 when the player has no row; 32-bit
// arithmetic as the PowerPC code), kept at most one place below the rows held.
inline Start StartPlace(int32_t position, int32_t rows) {
  const int32_t game = position != 0 ? position : int32_t(uint32_t(rows) + 1u);
  const int32_t held = rows < 0 ? 0 : rows > kMaxRows ? kMaxRows : rows;
  if (game > held + 1) return {held + 1, true};
  return {game, false};
}

// The place a new score takes: from `start` up while it beats the row above. score_of(i)
// is the score of row i (0-based). The comparison is the game's: stop unless strictly greater
// (a NaN stops at once).
template <typename ScoreOf>
int32_t NewPlace(int32_t start, double score, ScoreOf score_of) {
  int32_t place = start;
  while (place > 1 && score > double(score_of(place - 2))) --place;
  return place;
}

}  // namespace forza::ambient_leaderboard
