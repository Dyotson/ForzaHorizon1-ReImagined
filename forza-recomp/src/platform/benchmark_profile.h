#pragma once
#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
namespace forza::benchmark {
// Benchmark folders add a timestamp below an already deep DLC installation.
// CreateHardLinkW needs extended paths even when ordinary directory creation works.
inline std::filesystem::path BenchmarkFilePath(const std::filesystem::path &path) {
#ifdef _WIN32
  auto native = std::filesystem::absolute(path).lexically_normal().make_preferred().wstring();
  if (native.starts_with(L"\\\\?\\")) return native;
  if (native.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + native.substr(2);
  return L"\\\\?\\" + native;
#else
  return path;
#endif
}

// Only immutable installed DLC is hard-linked. Writable saves use CopyProfiles.
inline void LinkPackages(const std::filesystem::path &source,
                         const std::filesystem::path &destination) {
  namespace fs = std::filesystem;
  const auto from = BenchmarkFilePath(source), to = BenchmarkFilePath(destination);
  const auto packages = from / "0000000000000000" / "4D5309C9" / "00000002";
  if (!fs::is_directory(packages)) return;
  for (const auto &item : fs::recursive_directory_iterator(packages)) {
    if (item.is_symlink())
      throw std::runtime_error("DLC package folder contains a symbolic link");
    const auto target = to / item.path().lexically_relative(from);
    if (item.is_directory())
      fs::create_directories(target);
    else if (item.is_regular_file()) {
      fs::create_directories(target.parent_path());
      fs::create_hard_link(item.path(), target);
    }
  }
}

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
