// forza - the game's database queries: Title Update 4's car view with indexed joins, and a log
// line for every slow query (#167).
//
// The game keeps its data (cars, upgrades, events, the garage) in an in-memory SQLite 3.6.19
// database and runs its queries synchronously on whichever thread asks, the main loop
// included. Every query string goes through one method that locks the database (a critical
// section at +12), runs each statement of the string and copies all rows into a recordset
// (sub_82C19130, TU4 sub_82CD8FD8; the copy is sub_82C188D0 / sub_82C18D00).
//
// Title Update 4 (the Rally Expansion) creates the view Restrictions_Data_Car at start: every
// car (Data_Car) with IsRally / CouldRally, LEFT JOINed to three derived tables (the level 4
// springs, the level 5 tyres, the level 4 transmissions with their drivetrain). SQLite 3.6.19
// has no automatic indexes: it materializes each derived table and scans it for every car, so
// a query of the view costs cars x (springs + tyres + transmissions) row visits: 0.13 s with
// all DLC (251 cars) on a Ryzen 7 7800X3D, 0.3 s on the PC of #167. The main loop runs
// "SELECT * FROM Restrictions_Data_Car WHERE IsCarInstalled(ModelId) ORDER BY ModelId" twice
// after each load into free roam (fast travel, the end of an event, joining a session), so the
// first seconds of driving had two or three frames that long; an event's sign-up screens run
// three more on another thread, holding the database lock the main loop waits for. The disc
// version has no such view.
//
// The view is created instead with the same columns (names, order, declared types) and rows
// from joins on the tables themselves: the springs and tyres with their level in the ON
// clause; the transmissions through the car's drivetrains that have a level 4 transmission
// (EXISTS), then those transmissions. Each row of the game's view is one (drivetrain,
// transmission) pair, or NULLs when the car has none; the drivetrain join keeps exactly the
// drivetrains with a pair, so the rows are the same, NULLs included (tools/check_car_view.py
// compares both on your game files, docs/perf.md). Indexes on the joined columns, created after
// the view, make each join a lookup: 17 ms instead of 131 for the main loop's query.
// FORZA_GAME_CAR_VIEW=1 keeps the game's view.
//
// A query that takes 50 ms or more logs (at most 100 lines a session)
//   FORZA_SQL: 131 ms (lock 0 ms) on thread F800007C: SELECT * FROM Restrictions_Data_Car ...
// and FORZA_SQL_LOG=1 logs every query with its time.

#include "../platform/guest_addr.h"
#include "../platform/guest_memory.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xthread.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

extern "C" REX_FUNC(__imp__sub_82C19130);
extern "C" REX_FUNC(__imp__sub_82C188D0);
extern "C" REX_FUNC(__imp__sub_82C18D00);

namespace {

constexpr int64_t kSlowUs = 50000;
constexpr int kMaxLines = 100;
std::atomic<int> lines{0};
thread_local int64_t run_us = 0;  // the statements' time inside the current query

bool EnvOn(const char* name) {
  const char* v = std::getenv(name);
  return v && *v && *v != '0';
}

bool LogAll() {
  static const bool on = EnvOn("FORZA_SQL_LOG");
  return on;
}

int64_t NowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string Sql(uint8_t* base, uint32_t address) {
  if (!address) return "(none)";
  const char* s = reinterpret_cast<const char*>(forza::GuestPtr(base, address));
  const size_t n = strnlen(s, 400);
  std::string out(s, n);
  for (char& c : out)
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  if (n == 400) out += "...";
  return out;
}

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

bool IsGameCarView(uint8_t* base, uint32_t address) {
  if (!address) return false;
  const char* s = reinterpret_cast<const char*>(forza::GuestPtr(base, address));
  if (std::strncmp(s, kViewPrefix.data(), kViewPrefix.size()) != 0) return false;
  if (strnlen(s, kGameViewLength + 1) != kGameViewLength) return false;
  uint64_t h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < kGameViewLength; ++i) h = (h ^ uint8_t(s[i])) * 0x100000001b3ull;
  return h == kGameViewHash;
}

// The guest copy of kIndexedView (0 when the game's view is kept or no memory was had).
uint32_t IndexedViewSql() {
  static const uint32_t address = [] {
    if (EnvOn("FORZA_GAME_CAR_VIEW")) {
      REXLOG_INFO("Restrictions_Data_Car: the game's view (FORZA_GAME_CAR_VIEW)");
      return 0u;
    }
    const uint32_t a = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(kIndexedView));
    if (!a) return 0u;
    std::memcpy(REX_KERNEL_MEMORY()->TranslateVirtual(a), kIndexedView, sizeof(kIndexedView));
    REXLOG_INFO("Restrictions_Data_Car: the car view joins by index (#167; FORZA_GAME_CAR_VIEW=1 "
                "keeps the game's)");
    return a;
  }();
  return address;
}

template <void (*Run)(PPCContext&, uint8_t*)>
void Timed(PPCContext& ctx, uint8_t* base) {
  const int64_t t0 = NowUs();
  Run(ctx, base);
  run_us += NowUs() - t0;
}

}  // namespace

// Database::Query(result*, database*, const char* sql)
extern "C" REX_FUNC(sub_82C19130) {
  if (IsGameCarView(base, ctx.r5.u32)) {
    if (const uint32_t view = IndexedViewSql()) ctx.r5.u64 = view;
  }
  const uint32_t sql = ctx.r5.u32;
  run_us = 0;
  const int64_t t0 = NowUs();
  __imp__sub_82C19130(ctx, base);
  const int64_t us = NowUs() - t0;
  const bool all = LogAll();
  if (us < kSlowUs && !all) return;
  if (!all && lines.fetch_add(1, std::memory_order_relaxed) >= kMaxLines) return;
  const uint32_t thread = rex::system::XThread::IsInThread()
                              ? rex::system::XThread::GetCurrentThreadHandle()
                              : 0;
  const std::string text = Sql(base, sql);
  if (us >= kSlowUs) {
    REXLOG_WARN("FORZA_SQL: {} ms (lock {} ms) on thread {:08X}: {}", us / 1000,
                (us - run_us) / 1000, thread, text);
  } else {
    REXLOG_INFO("FORZA_SQL: {:.2f} ms on thread {:08X}: {}", us / 1000.0, thread, text);
  }
}

extern "C" REX_FUNC(sub_82C188D0) { Timed<__imp__sub_82C188D0>(ctx, base); }
extern "C" REX_FUNC(sub_82C18D00) { Timed<__imp__sub_82C18D00>(ctx, base); }
