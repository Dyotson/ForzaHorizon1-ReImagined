// forza - Title Update 4's car view with indexed joins, and the time of nested queries
// (src/hooks/database_query.cpp, #167). No runtime in here: forza-recomp/tests/car_view_test.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace forza::car_view {

// Title Update 4's CREATE VIEW Restrictions_Data_Car statement: its length and FNV-1a hash.
constexpr std::string_view kViewPrefix = "CREATE VIEW Restrictions_Data_Car AS ";
constexpr size_t kGameViewLength = 818;
constexpr uint64_t kGameViewHash = 0xd7e6492f06fbc0e8ull;

// The same view (columns, rows) with joins SQLite 3.6.19 can do by index, then the indexes.
// The view comes first: an index that cannot be made ends the string, the view is made.
constexpr char kIndexedView[] =
    "CREATE VIEW Restrictions_Data_Car AS SELECT Data_Car.*, "
    "Springs.IsStock AS IsStock, Springs.Level AS Level, Springs.Ordinal AS Ordinal, "
    "Tyres.IsStock AS IsStock, Tyres.Level AS Level, Tyres.Ordinal AS Ordinal, "
    "Transmission.IsStock AS IsStock, Transmission.Level AS Level, "
    "Drivetrain.Ordinal AS Ordinal, Data_Car.BaseCost AS CarValue, "
    "CASE WHEN Springs.IsStock = 1 AND Tyres.IsStock = 1 AND Transmission.IsStock = 1 "
    "THEN 1 ELSE 0 END AS IsRally, "
    "CASE WHEN Springs.Level = 4 AND Tyres.Level = 5 AND Transmission.Level = 4 "
    "THEN 1 ELSE 0 END AS CouldRally "
    "FROM Data_Car "
    "LEFT JOIN List_UpgradeSpringDamper AS Springs "
    "ON Springs.Ordinal = Data_Car.ModelId AND Springs.Level = 4 "
    "LEFT JOIN List_UpgradeTireCompound AS Tyres "
    "ON Tyres.Ordinal = Data_Car.ModelId AND Tyres.Level = 5 "
    "LEFT JOIN List_UpgradeDrivetrain AS Drivetrain "
    "ON Drivetrain.Ordinal = Data_Car.ModelId AND EXISTS (SELECT 1 FROM "
    "List_UpgradeDrivetrainTransmission AS t WHERE t.DrivetrainID = Drivetrain.DrivetrainID "
    "AND t.Level = 4) "
    "LEFT JOIN List_UpgradeDrivetrainTransmission AS Transmission "
    "ON Transmission.DrivetrainID = Drivetrain.DrivetrainID AND Transmission.Level = 4 "
    "WHERE Data_Car.IsDrivable = 1 OR IsDrivableCar(Data_Car.Id);"
    "CREATE INDEX IF NOT EXISTS forza_SpringDamper_Ordinal ON List_UpgradeSpringDamper (Ordinal);"
    "CREATE INDEX IF NOT EXISTS forza_TireCompound_Ordinal ON List_UpgradeTireCompound (Ordinal);"
    "CREATE INDEX IF NOT EXISTS forza_Drivetrain_Ordinal ON List_UpgradeDrivetrain (Ordinal);"
    "CREATE INDEX IF NOT EXISTS forza_Transmission_DrivetrainID "
    "ON List_UpgradeDrivetrainTransmission (DrivetrainID)";

// The view's own statement in kIndexedView, up to its ';': the text SQLite keeps in
// sqlite_master and the game runs again when it rebuilds its database (its vtable+88,
// TU4 sub_82CD4C50: the tables are copied without their indexes, each view's stored SQL
// goes through the query method).
constexpr size_t kViewStatementLength = std::string_view(kIndexedView).find(';');

constexpr uint64_t Fnv1a(const char* s, size_t n) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ uint8_t(s[i])) * 0x100000001b3ull;
  return h;
}

enum class Statement { kOther, kGameView, kIndexedView };

// What a query string (NUL-terminated) is: the game's CREATE VIEW (by prefix, length and
// hash), our view's statement as stored (alone, or followed by ';' and anything), or another.
// Both views are run as kIndexedView, so the indexes are made whenever the view is.
inline Statement Classify(const char* s) {
  if (!s || std::strncmp(s, kViewPrefix.data(), kViewPrefix.size()) != 0) {
    return Statement::kOther;
  }
  if (std::strncmp(s, kIndexedView, kViewStatementLength) == 0 &&
      (s[kViewStatementLength] == '\0' || s[kViewStatementLength] == ';')) {
    return Statement::kIndexedView;
  }
  size_t n = 0;
  while (n <= kGameViewLength && s[n]) ++n;
  if (n == kGameViewLength && Fnv1a(s, n) == kGameViewHash) return Statement::kGameView;
  return Statement::kOther;
}

// Time spent in the statements of queries, when the copy routines can call each other (the
// cached copy, TU4 sub_82CD8BA8, runs the plain one, sub_82CD8778, on a miss): only the
// outermost call counts. One per thread; a query's share is the growth of total() over it.
class NestedTimer {
 public:
  void Enter(int64_t now) {
    if (depth_++ == 0) start_ = now;
  }
  void Exit(int64_t now) {
    if (depth_ > 0 && --depth_ == 0) total_ += now - start_;
  }
  int64_t total() const { return total_; }

 private:
  int depth_ = 0;
  int64_t start_ = 0, total_ = 0;
};

}  // namespace forza::car_view
