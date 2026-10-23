// Title Update 4's car view with indexed joins (src/platform/car_view.h, #167): which query
// strings get the view and its indexes (the game's statement, and ours as the game runs it again
// from sqlite_master when it rebuilds its database), and the time of nested copy routines.
// tools/check_car_view.py checks the rows on the game's own files. Build from the repository root:
//   g++ -std=c++20 -Wall -Wextra -Werror -Iforza-recomp/src forza-recomp/tests/car_view_test.cpp
//       -o car-view-test && ./car-view-test
#include "platform/car_view.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace cv = forza::car_view;

namespace {
void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::exit(1);
  }
}

size_t Count(std::string_view s, std::string_view part) {
  size_t n = 0;
  for (size_t at = s.find(part); at != std::string_view::npos; at = s.find(part, at + 1)) ++n;
  return n;
}
}  // namespace

int main() {
  const std::string_view full(cv::kIndexedView);
  const std::string statement(full.substr(0, cv::kViewStatementLength));

  // The string: the view first, then only indexes that may already exist.
  Check(full.starts_with(cv::kViewPrefix), "the rewrite creates the same view");
  Check(statement.ends_with("IsDrivableCar(Data_Car.Id)"), "the view statement ends at its ';'");
  Check(Count(full, ";") == 4, "four statements after the view");
  Check(Count(full, ";CREATE INDEX IF NOT EXISTS forza_") == 4, "each one an index if missing");
  Check(Count(statement, "CREATE ") == 1, "the view statement holds no other statement");

  // What gets the view and its indexes.
  Check(cv::Classify(statement.c_str()) == cv::Statement::kIndexedView,
        "our view as SQLite stores it (no ';'), run again by a rebuild");
  Check(cv::Classify(cv::kIndexedView) == cv::Statement::kIndexedView, "our whole string");
  Check(cv::Classify((statement + ";").c_str()) == cv::Statement::kIndexedView,
        "our view with a ';'");
  Check(cv::Classify((statement + " AND 1").c_str()) == cv::Statement::kOther,
        "a longer view is not ours");
  Check(cv::Classify(statement.substr(0, statement.size() - 1).c_str()) == cv::Statement::kOther,
        "a shorter view is not ours");
  Check(cv::Classify(nullptr) == cv::Statement::kOther, "no string");
  Check(cv::Classify("SELECT * FROM Restrictions_Data_Car") == cv::Statement::kOther,
        "a query of the view");
  Check(cv::Classify(std::string(cv::kViewPrefix).c_str()) == cv::Statement::kOther,
        "the prefix alone");
  std::string other(cv::kViewPrefix);
  other.resize(cv::kGameViewLength, 'x');
  Check(cv::Classify(other.c_str()) == cv::Statement::kOther,
        "another view of the game's length (its hash differs)");

  // FNV-1a 64 (the game's statement is recognized by it; its text is not in this repository).
  Check(cv::Fnv1a("", 0) == 0xcbf29ce484222325ull, "FNV-1a of nothing");
  Check(cv::Fnv1a("a", 1) == 0xaf63dc4c8601ec8cull, "FNV-1a of a");
  Check(cv::Fnv1a("foobar", 6) == 0x85944171f73967e8ull, "FNV-1a of foobar");

  // The cached copy runs the plain one on a miss: only the outermost call counts.
  cv::NestedTimer t;
  t.Enter(100);
  t.Enter(110);
  t.Exit(150);
  t.Exit(170);
  Check(t.total() == 70, "a nested copy is not counted twice");
  t.Enter(200);
  t.Exit(205);
  Check(t.total() == 75, "the next query adds its own time");
  t.Exit(300);
  Check(t.total() == 75, "an exit without an enter changes nothing");
  t.Enter(400);
  t.Exit(410);
  Check(t.total() == 85, "and the timer still works after it");

  std::puts("car view: all checks passed");
  return 0;
}
