#pragma once
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
namespace forza::benchmark {
// Copy profile bytes; never redirect a writable save back to the source. DLC
// packages are immutable inputs and remain outside this profile-copy helper.
inline size_t CopyProfiles(const std::filesystem::path &source,
                           const std::filesystem::path &destination) {
  namespace fs = std::filesystem;
  if (fs::exists(destination))
    throw std::runtime_error("Benchmark destination already exists");
  size_t copied = 0;
  fs::create_directories(destination);
  for (const auto &owner : fs::directory_iterator(source)) {
    const auto name = owner.path().filename().string();
    if (name.size() != 16 || name == "0000000000000000" ||
        name.find_first_not_of("0123456789ABCDEFabcdef") != std::string::npos)
      continue;
    if (owner.is_symlink())
      throw std::runtime_error("Benchmark profile must be a real directory");
    if (!owner.is_directory())
      continue;
    for (const auto &item : fs::recursive_directory_iterator(owner.path())) {
      if (item.is_symlink())
        throw std::runtime_error("Benchmark profile contains a symbolic link");
      const auto target = destination / item.path().lexically_relative(source);
      if (item.is_directory())
        fs::create_directories(target);
      else if (item.is_regular_file()) {
        fs::create_directories(target.parent_path());
        fs::copy_file(item.path(), target);
        ++copied;
      }
    }
  }
  if (!copied)
    throw std::runtime_error("Play and save once before running the benchmark");
  return copied;
}
} // namespace forza::benchmark
