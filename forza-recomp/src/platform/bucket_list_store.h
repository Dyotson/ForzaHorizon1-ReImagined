#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

namespace forza::bucket_list {
// Separate, profile-scoped prototype progress. No ForzaProfile writes.
class ProgressStore {
 public:
  explicit ProgressStore(std::filesystem::path path) : path_(std::move(path)) {}
  bool Load(uint8_t& completed) const {
    return Read(path_, completed) || Read(Suffix(".bak"), completed);
  }
  bool Save(uint8_t completed) const {
    if (completed > 7 || path_.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(path_.parent_path(), error);
    if (error) return false;
    const auto temporary = Suffix(".tmp");
    const auto backup = Suffix(".bak");
    const bool exists = std::filesystem::exists(path_, error);
    if (error || (exists && !std::filesystem::is_regular_file(path_, error))) return false;
    {
      std::ofstream out(temporary, std::ios::trunc);
      out << "fh1-bucket-list-v1 " << unsigned(completed) << '\n';
      out.close();
      if (!out) return false;
    }
    if (exists) {
      std::filesystem::remove(backup, error);
      if (error) return false;
      std::filesystem::rename(path_, backup, error);
      if (error) return false;
    }
    std::filesystem::rename(temporary, path_, error);
    if (!error) return true;
    if (exists) {
      error.clear();
      std::filesystem::rename(backup, path_, error);
    }
    return false;
  }
 private:
  std::filesystem::path Suffix(const char* suffix) const {
    auto path = path_;
    path += suffix;
    return path;
  }
  static bool Read(const std::filesystem::path& path, uint8_t& completed) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > 128) return false;
    std::ifstream in(path);
    std::string magic, extra;
    unsigned mask = 0;
    if (!(in >> magic >> mask) || magic != "fh1-bucket-list-v1" || mask > 7 || (in >> extra)) return false;
    completed = uint8_t(mask);
    return true;
  }
  std::filesystem::path path_;
};
}
