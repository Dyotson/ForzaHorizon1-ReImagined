// Speed trap and zone leaderboards (#288): where a new score is placed among the rows of a
// GetRows answer, and that it never reads past the 200 rows the game holds. Build from the
// repository root:
//   g++ -std=c++20 -Wall -Wextra -Werror -Iforza-recomp/src forza-recomp/tests/ambient_leaderboard_test.cpp
//       -o ambient-leaderboard-test && ./ambient-leaderboard-test
#include "platform/ambient_leaderboard.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace lb = forza::ambient_leaderboard;

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

// The game's walk over `rows` (best first), recording every row it reads.
struct Board {
  std::vector<float> rows;
  std::vector<int32_t> read;
  int32_t Place(int32_t position, double score) {
    read.clear();
    const lb::Start start = lb::StartPlace(position, int32_t(rows.size()));
    return lb::NewPlace(start.place, score, [&](int32_t i) {
      read.push_back(i);
      return rows.at(size_t(i));
    });
  }
  bool ReadInside() const {
    for (int32_t i : read)
      if (i < 0 || i >= int32_t(rows.size())) return false;
    return true;
  }
};

std::vector<float> Speeds(int n) {  // 60, 59.9, ... m/s
  std::vector<float> v;
  for (int i = 0; i < n; ++i) v.push_back(60.0f - 0.1f * float(i));
  return v;
}
}  // namespace

int main() {
  // The game's start where it is sound: the player's position, or below the last row.
  Check(lb::StartPlace(2, 3).place == 2 && !lb::StartPlace(2, 3).clamped, "own position");
  Check(lb::StartPlace(0, 3).place == 4 && !lb::StartPlace(0, 3).clamped, "no row: after the rows");
  Check(lb::StartPlace(4, 3).place == 4 && !lb::StartPlace(4, 3).clamped, "right after the rows");
  Check(lb::StartPlace(201, 200).place == 201 && !lb::StartPlace(201, 200).clamped, "201 of 200");
  Check(lb::StartPlace(0, 0).place == 1 && !lb::StartPlace(0, 0).clamped, "empty board");
  Check(lb::StartPlace(-5, 3).place == -5 && !lb::StartPlace(-5, 3).clamped, "negative: no walk");

  // The crash: place 713 with the top 200 rows (row 711 read; 0x215F0 past the object).
  Check(lb::StartPlace(713, 200).place == 201 && lb::StartPlace(713, 200).clamped, "713 of 200");
  Check(lb::StartPlace(713, 50).place == 51 && lb::StartPlace(713, 50).clamped, "713 of 50");
  // A count beyond the array, with and without a row of the player's.
  Check(lb::StartPlace(0, 712).place == 201 && lb::StartPlace(0, 712).clamped, "712 rows, no row");
  Check(lb::StartPlace(300, 712).place == 201 && lb::StartPlace(300, 712).clamped, "712 rows");
  Check(lb::StartPlace(0, INT32_MAX).place == INT32_MIN, "the game's 32-bit wrap: no walk");
  Check(lb::StartPlace(5, -1).place == 1 && lb::StartPlace(5, -1).clamped, "negative count");

  Board top{Speeds(200), {}};
  // Below everyone (60 .. 40.1 m/s): stays after the last row, reads only that row.
  Check(top.Place(713, 30.0) == 201 && top.read.size() == 1 && top.read[0] == 199,
        "slow: after the rows");
  // Fast enough for the top 200: walks up from the last row to its place.
  Check(top.Place(713, 55.05) == 51 && top.ReadInside(), "into the rows");
  Check(top.Place(713, 99.0) == 1 && top.ReadInside(), "to the top");
  // The game's own cases are unchanged.
  Board small{{52.5f, 45.088f, 30.25f}, {}};
  Check(small.Place(2, 50.0) == 2, "not past the row above");
  Check(small.Place(2, 53.0) == 1, "past the row above");
  Check(small.Place(0, 40.0) == 3, "no row: below 45.088");
  Check(small.Place(1, 99.0) == 1 && small.read.empty(), "first: nothing to read");
  Check(small.Place(3, 45.088) == 3, "a tie does not move up");
  Check(small.Place(3, std::nan("")) == 3, "NaN stops at once");
  Check(small.Place(713, 46.0) == 2 && small.ReadInside(), "713 with 3 rows");

  if (failures) return EXIT_FAILURE;
  std::printf("ambient leaderboard: all checks passed\n");
  return EXIT_SUCCESS;
}
