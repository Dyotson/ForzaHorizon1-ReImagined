#include "../src/platform/benchmark_profile.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
using forza::benchmark::CopyProfiles;
int main() {
  namespace fs = std::filesystem;
  auto root =
      fs::temp_directory_path() /
      ("forza-profile-test-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  auto source = root / "source", copied = root / "copy";
  auto relative =
      fs::path("B13EBABEB13EBABE") / "4D5309C9" / "00000001" / "ForzaProfile";
  fs::create_directories((source / relative).parent_path());
  std::ofstream(source / relative) << "original";
  fs::create_directories(source / "0000000000000000");
  std::ofstream(source / "0000000000000000" / "not-a-save") << "package";
  std::ofstream(source / "launcher.json") << "not a profile";
  assert(CopyProfiles(source, copied) == 1);
  assert(!fs::exists(copied / "launcher.json") &&
         !fs::exists(copied / "0000000000000000"));
  std::ofstream(copied / relative) << "benchmark progress";
  std::string bytes;
  std::ifstream(source / relative) >> bytes;
  assert(bytes == "original");
  bool rejected = false;
  try {
    CopyProfiles(source, copied);
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  assert(rejected);
  fs::create_directories(root / "empty");
  rejected = false;
  try {
    CopyProfiles(root / "empty", root / "empty-copy");
  } catch (const std::runtime_error &) {
    rejected = true;
  }
  assert(rejected);
  fs::remove_all(root);
  std::cout << "Benchmark save isolation passed\n";
}
