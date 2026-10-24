// forza - mods (#149): the folders a player's mods live in, and the archive the game reads in
// place of the title update's media.zip, built from that archive and the mods' files.
//
// The game merges its archives into one tree by priority: every zip of
// game:\media\zipmanifest.xml (95 for media\*.zip, 80 for cars and string tables), the DLC
// zips (Media\DLCZips\<name>_pri_<n>.zip) and, above them all, update:\media.zip, mounted at
// game:\media\ with priority 100000 (TU4 sub_82581DB8 and its caller). A member
// "stringtables/en/main.str" of that archive replaces game:\media\stringtables\en\main.str,
// which the disc keeps in media\stringtables\en.zip. So a mod file at
// <mod>/media/stringtables/en/main.str only has to become a member of update:\media.zip.
//
// The archive here: the update's own bytes up to its central directory, unchanged (every
// original member keeps its offset), then each mod file as a stored member (method 0: a local
// header and the file's bytes, CRC-32 of the file), then a new central directory: the
// original records verbatim except those a mod replaces, then the mods' records, and the end
// record. Like the update's own records, a mod's record carries the extra field 0x1123 with
// the absolute offset of its data, which the game reads instead of the local header.
// The game finds update:\media.zip's directory through the end record (its zipmanifest.xml
// lists only the disc's archives), so nothing else changes.
//
// No runtime in here: forza-recomp/tests/mod_archive_test.cpp. The runtime part (the
// archive put in the place of update:\media.zip, reads) is src/kernel/mod_overlay.cpp.

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace forza::mods {

// --- CRC-32 (the zip polynomial) -----------------------------------------------------------

inline uint32_t Crc32(const void* data, size_t size, uint32_t crc = 0) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  const auto* p = static_cast<const uint8_t*>(data);
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

// --- zip records ---------------------------------------------------------------------------

inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline void Put16(std::string& s, uint16_t v) {
  s.push_back(char(v & 0xFF));
  s.push_back(char(v >> 8));
}
inline void Put32(std::string& s, uint32_t v) {
  for (int i = 0; i < 4; ++i) s.push_back(char((v >> (8 * i)) & 0xFF));
}

constexpr uint32_t kLocalSignature = 0x04034B50;
constexpr uint32_t kCentralSignature = 0x02014B50;
constexpr uint32_t kEndSignature = 0x06054B50;
constexpr uint16_t kDataOffsetField = 0x1123;  // the game's extra field: absolute data offset
constexpr uint64_t kMaxArchive = 0xFFFFFFFFull;  // no Zip64 in the game's reader
// The game's zip index (TU4 sub_82D22528, the base game's the same) keeps a member's compressed
// size as csize << 6 and its size as usize << 5 (the method in the low bits) in 32 bits: a
// stored member has at most 2^26 - 1 bytes. Its directory parser copies each name into a
// 416-byte stack frame without a length check: names stay at 255 bytes, as on the game's discs.
constexpr uint64_t kMaxMember = (uint64_t(1) << 26) - 1;
constexpr size_t kMaxName = 255;
// A bigger file can only take the place of a loose file of the game (read straight from disk).
constexpr uint64_t kMaxModFile = 0x7FFFFFFFull;

struct ZipEntry {
  std::string name;    // as stored
  std::string record;  // the whole central directory record, verbatim
  uint16_t method = 0;
  uint32_t crc = 0, compressed = 0, size = 0, local_offset = 0;
};

struct ZipDirectory {
  uint64_t directory_offset = 0, directory_size = 0;
  std::vector<ZipEntry> entries;
};

// Reads `size` bytes at `offset`; false when fewer were there.
using ReadAt = std::function<bool(uint64_t offset, void* buffer, size_t size)>;

// The end record (searched in the last 64 KB + 22 bytes, a comment may follow it) and the
// central directory it points at. Multi-disk and Zip64 archives are refused.
inline bool ReadZipDirectory(const ReadAt& read, uint64_t file_size, ZipDirectory* out,
                             std::string* error) {
  *out = {};
  if (file_size < 22) {
    *error = "too small for a zip archive";
    return false;
  }
  const uint64_t tail_size = std::min<uint64_t>(file_size, 22 + 0xFFFF);
  std::string tail(size_t(tail_size), '\0');
  if (!read(file_size - tail_size, tail.data(), tail.size())) {
    *error = "could not read the end of the archive";
    return false;
  }
  const auto* t = reinterpret_cast<const uint8_t*>(tail.data());
  for (int64_t i = int64_t(tail_size) - 22; i >= 0; --i) {
    if (Le32(t + i) != kEndSignature) continue;
    const uint8_t* e = t + i;
    const uint16_t disk = Le16(e + 4), directory_disk = Le16(e + 6);
    const uint16_t here = Le16(e + 8), total = Le16(e + 10);
    const uint32_t directory_size = Le32(e + 12), directory_offset = Le32(e + 16);
    if (disk != 0 || directory_disk != 0 || here != total) {
      *error = "multi-disk archives are not supported";
      return false;
    }
    if (directory_offset == 0xFFFFFFFFu || total == 0xFFFF) {
      *error = "Zip64 archives are not supported";
      return false;
    }
    if (uint64_t(directory_offset) + directory_size > file_size - tail_size + uint64_t(i)) {
      *error = "the central directory is outside the archive";
      return false;
    }
    std::string directory(directory_size, '\0');
    if (directory_size && !read(directory_offset, directory.data(), directory.size())) {
      *error = "could not read the central directory";
      return false;
    }
    const auto* d = reinterpret_cast<const uint8_t*>(directory.data());
    size_t at = 0;
    for (uint32_t n = 0; n < total; ++n) {
      if (at + 46 > directory.size() || Le32(d + at) != kCentralSignature) {
        *error = "damaged central directory record " + std::to_string(n);
        return false;
      }
      const size_t length = 46 + size_t(Le16(d + at + 28)) + Le16(d + at + 30) + Le16(d + at + 32);
      if (at + length > directory.size()) {
        *error = "damaged central directory record " + std::to_string(n);
        return false;
      }
      ZipEntry entry;
      entry.method = Le16(d + at + 10);
      entry.crc = Le32(d + at + 16);
      entry.compressed = Le32(d + at + 20);
      entry.size = Le32(d + at + 24);
      entry.local_offset = Le32(d + at + 42);
      entry.name.assign(directory.data() + at + 46, Le16(d + at + 28));
      entry.record.assign(directory.data() + at, length);
      out->entries.push_back(std::move(entry));
      at += length;
    }
    out->directory_offset = directory_offset;
    out->directory_size = directory_size;
    return true;
  }
  *error = "no end of central directory record";
  return false;
}

// The names the game's archives use: forward slashes, lower case ("Stringtables\EN\Main.str"
// becomes "stringtables/en/main.str"). Lookups in the game are not case-sensitive, and TU4's
// media.zip stores every name in lower case.
inline std::string GamePath(std::string_view path) {
  std::string out;
  out.reserve(path.size());
  for (char c : path) {
    if (c == '\\') c = '/';
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (c == '/' && (out.empty() || out.back() == '/')) continue;
    out.push_back(c);
  }
  return out;
}

// --- the archive's bytes -------------------------------------------------------------------

struct Segment {
  uint64_t offset = 0, length = 0;  // where in the archive
  int32_t source = -1;              // a host file (index), or -1: bytes in Layout::blobs[blob]
  uint32_t blob = 0;
  uint64_t source_offset = 0;       // where in that file or blob
};

struct Layout {
  uint64_t size = 0;
  std::vector<Segment> segments;  // in order, no gaps
  std::vector<std::string> blobs;

  void AddBytes(std::string bytes) {
    if (bytes.empty()) return;
    Segment s;
    s.offset = size;
    s.length = bytes.size();
    s.blob = uint32_t(blobs.size());
    blobs.push_back(std::move(bytes));
    segments.push_back(s);
    size += s.length;
  }
  void AddFile(int32_t source, uint64_t source_offset, uint64_t length) {
    if (!length) return;
    Segment s;
    s.offset = size;
    s.length = length;
    s.source = source;
    s.source_offset = source_offset;
    segments.push_back(s);
    size += length;
  }
  // The segment holding byte `offset`, or segments.size().
  size_t Find(uint64_t offset) const {
    auto it = std::upper_bound(segments.begin(), segments.end(), offset,
                               [](uint64_t o, const Segment& s) { return o < s.offset; });
    if (it == segments.begin()) return segments.size();
    --it;
    return offset < it->offset + it->length ? size_t(it - segments.begin()) : segments.size();
  }
};

// Reads host file `source` at `offset` into `buffer`; returns the bytes read (fewer at its end).
using SourceRead = std::function<size_t(int32_t source, uint64_t offset, uint8_t* buffer,
                                        size_t size)>;

// Copies up to `size` bytes at `offset`; returns how many (fewer only at the archive's end).
// `touched(index)` is told each segment the read used; bytes a host file no longer has (it
// shrank) read as zeros and `short_source(source)` is told.
inline size_t ReadLayout(const Layout& layout, uint64_t offset, uint8_t* buffer, size_t size,
                         const SourceRead& read_source,
                         const std::function<void(size_t)>& touched = {},
                         const std::function<void(int32_t)>& short_source = {}) {
  if (offset >= layout.size) return 0;
  size = size_t(std::min<uint64_t>(size, layout.size - offset));
  size_t done = 0;
  size_t index = layout.Find(offset);
  while (done < size && index < layout.segments.size()) {
    const Segment& s = layout.segments[index];
    const uint64_t within = offset + done - s.offset;
    const size_t n = size_t(std::min<uint64_t>(size - done, s.length - within));
    if (touched) touched(index);
    if (s.source < 0) {
      std::copy_n(layout.blobs[s.blob].data() + s.source_offset + within, n, buffer + done);
    } else {
      size_t got = read_source(s.source, s.source_offset + within, buffer + done, n);
      if (got < n) {
        std::fill(buffer + done + got, buffer + done + n, uint8_t(0));
        if (short_source) short_source(s.source);
      }
    }
    done += n;
    ++index;
  }
  return done;
}

struct AddedFile {
  std::string name;  // GamePath, relative to game:\media\ (the archive's root)
  uint32_t size = 0, crc = 0;
  int32_t source = 0;  // host file index
};

// The archive: `base` (host file `base_source`, read with ReadZipDirectory; null for none) up
// to its central directory, the files as stored members, and the new directory. replaced[i]
// says whether files[i] took the place of one of the base's members (matched without case).
inline bool BuildArchive(const ZipDirectory* base, int32_t base_source,
                         const std::vector<AddedFile>& files, Layout* out,
                         std::vector<bool>* replaced, std::string* error) {
  *out = {};
  replaced->assign(files.size(), false);
  std::map<std::string, size_t> by_name;  // GamePath -> file index
  for (size_t i = 0; i < files.size(); ++i) {
    if (files[i].name.empty() || files[i].name.size() > kMaxName) {
      *error = "bad member name: " + files[i].name;
      return false;
    }
    if (files[i].size > kMaxMember) {
      *error = files[i].name + " is over 64 MB, more than the game's archive holds";
      return false;
    }
    if (!by_name.emplace(GamePath(files[i].name), i).second) {
      *error = "two files named " + files[i].name;
      return false;
    }
  }
  std::vector<std::string> names(files.size());
  for (size_t i = 0; i < files.size(); ++i) names[i] = files[i].name;
  if (base) {
    out->AddFile(base_source, 0, base->directory_offset);
    for (const ZipEntry& e : base->entries) {
      auto it = by_name.find(GamePath(e.name));
      if (it == by_name.end()) continue;
      (*replaced)[it->second] = true;
      names[it->second] = e.name;  // the base's spelling
    }
  }
  struct Placed {
    uint32_t local, data;
  };
  std::vector<Placed> placed(files.size());
  constexpr uint16_t kVersion = 10;  // 1.0: stored, as the update's own records
  constexpr uint16_t kTime = 0, kDate = 0x21;  // 1980-01-01 00:00
  for (size_t i = 0; i < files.size(); ++i) {
    const AddedFile& f = files[i];
    std::string local;
    Put32(local, kLocalSignature);
    Put16(local, kVersion);
    Put16(local, 0);  // flags
    Put16(local, 0);  // method: stored
    Put16(local, kTime);
    Put16(local, kDate);
    Put32(local, f.crc);
    Put32(local, f.size);
    Put32(local, f.size);
    Put16(local, uint16_t(names[i].size()));
    Put16(local, 0);  // extra
    local += names[i];
    placed[i].local = uint32_t(out->size);
    placed[i].data = uint32_t(out->size + local.size());
    if (out->size + local.size() + f.size > kMaxArchive) {
      *error = "the archive would pass 4 GB, more than the game's zip reader handles";
      return false;
    }
    out->AddBytes(std::move(local));
    out->AddFile(f.source, 0, f.size);
  }
  std::string directory;
  size_t count = 0;
  if (base) {
    for (const ZipEntry& e : base->entries) {
      if (by_name.count(GamePath(e.name))) continue;
      directory += e.record;
      ++count;
    }
  }
  for (size_t i = 0; i < files.size(); ++i) {
    const AddedFile& f = files[i];
    Put32(directory, kCentralSignature);
    Put16(directory, kVersion);  // made by: 1.0, MS-DOS
    Put16(directory, kVersion);
    Put16(directory, 0);
    Put16(directory, 0);
    Put16(directory, kTime);
    Put16(directory, kDate);
    Put32(directory, f.crc);
    Put32(directory, f.size);
    Put32(directory, f.size);
    Put16(directory, uint16_t(names[i].size()));
    Put16(directory, 8);  // extra: the data offset field
    Put16(directory, 0);  // comment
    Put16(directory, 0);  // disk
    Put16(directory, 0);  // internal attributes
    Put32(directory, 0);  // external attributes
    Put32(directory, placed[i].local);
    directory += names[i];
    Put16(directory, kDataOffsetField);
    Put16(directory, 4);
    Put32(directory, placed[i].data);
    ++count;
  }
  if (count >= 0xFFFF) {
    *error = "more than 65534 members, more than the game's zip reader handles";
    return false;
  }
  const uint64_t directory_offset = out->size;
  if (directory_offset + directory.size() + 22 > kMaxArchive) {
    *error = "the archive would pass 4 GB, more than the game's zip reader handles";
    return false;
  }
  std::string end;
  Put32(end, kEndSignature);
  Put16(end, 0);
  Put16(end, 0);
  Put16(end, uint16_t(count));
  Put16(end, uint16_t(count));
  Put32(end, uint32_t(directory.size()));
  Put32(end, uint32_t(directory_offset));
  Put16(end, 0);
  out->AddBytes(std::move(directory));
  out->AddBytes(std::move(end));
  return true;
}

// --- host files -----------------------------------------------------------------------------

// Open handles of the files an archive reads: at most `capacity` of the unpinned ones at once,
// the least recently used closed first (a reader still using one keeps it until it is done:
// handles are shared), so a mod of thousands of files does not run the process out of file
// descriptors (1024 on many Linux desktops). A failed open is tried again on the next read.
template <typename Handle>
class HandleCache {
 public:
  using Opener = std::function<std::unique_ptr<Handle>(size_t index)>;

  HandleCache(std::vector<bool> pinned, size_t capacity, Opener open)
      : pinned_(std::move(pinned)), capacity_(capacity ? capacity : 1), open_(std::move(open)),
        handles_(pinned_.size()), where_(pinned_.size(), lru_.end()) {}

  // The handle of file `index`, or null when it cannot be opened (`*opened_now` says whether
  // this call opened it).
  std::shared_ptr<Handle> Get(size_t index, bool* opened_now = nullptr) {
    std::lock_guard lock(mutex_);
    if (opened_now) *opened_now = false;
    if (index >= handles_.size()) return nullptr;
    if (handles_[index]) {
      if (!pinned_[index]) lru_.splice(lru_.begin(), lru_, where_[index]);
      return handles_[index];
    }
    std::unique_ptr<Handle> handle = open_(index);
    if (!handle) return nullptr;
    handles_[index] = std::shared_ptr<Handle>(std::move(handle));
    if (opened_now) *opened_now = true;
    if (!pinned_[index]) {
      lru_.push_front(index);
      where_[index] = lru_.begin();
      while (lru_.size() > capacity_) {
        const size_t oldest = lru_.back();
        lru_.pop_back();
        where_[oldest] = lru_.end();
        handles_[oldest].reset();
      }
    }
    return handles_[index];
  }

  size_t open_count() {
    std::lock_guard lock(mutex_);
    size_t n = 0;
    for (const auto& h : handles_) n += h != nullptr;
    return n;
  }

 private:
  std::mutex mutex_;
  std::vector<bool> pinned_;
  size_t capacity_;
  Opener open_;
  std::vector<std::shared_ptr<Handle>> handles_;
  std::list<size_t> lru_;  // unpinned open handles, most recently used first
  std::vector<std::list<size_t>::iterator> where_;
};

// --- mod folders ---------------------------------------------------------------------------

// mod.toml: name, version, author, description (all optional). A small subset of TOML:
// `key = "text"` or `key = 'text'` lines, # comments; other lines are ignored.
struct Manifest {
  std::string name, version, author, description;
};

inline Manifest ParseManifest(std::string_view text) {
  Manifest m;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(at, end - at);
    at = end + 1;
    auto trim = [](std::string_view s) {
      while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
      while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
      return s;
    };
    line = trim(line);
    if (line.size() >= 3 && line.substr(0, 3) == "\xEF\xBB\xBF") line = trim(line.substr(3));
    if (line.empty() || line[0] == '#' || line[0] == '[') continue;
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) continue;
    std::string_view key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
    if (value.size() < 2 || (value[0] != '"' && value[0] != '\'')) continue;
    const char quote = value[0];
    std::string parsed;
    bool closed = false;
    for (size_t i = 1; i < value.size(); ++i) {
      char c = value[i];
      if (c == quote) {
        closed = true;
        break;
      }
      if (c == '\\' && quote == '"' && i + 1 < value.size()) {
        char n = value[++i];
        parsed.push_back(n == 'n' ? '\n' : n == 't' ? '\t' : n);
        continue;
      }
      parsed.push_back(c);
    }
    if (!closed) continue;
    if (key == "name") m.name = parsed;
    else if (key == "version") m.version = parsed;
    else if (key == "author") m.author = parsed;
    else if (key == "description") m.description = parsed;
  }
  return m;
}

// The forza_mods setting: folder names in load order, separated by '|' (a character no
// Windows file name has). "*" alone is every folder in the mods folder, by name.
inline std::vector<std::string> SplitModList(std::string_view text) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at <= text.size()) {
    size_t end = text.find('|', at);
    if (end == std::string_view::npos) end = text.size();
    // Names are taken as they are (a folder may start or end with a space).
    std::string_view item = text.substr(at, end - at);
    if (!item.empty() && std::find(out.begin(), out.end(), item) == out.end()) out.emplace_back(item);
    at = end + 1;
  }
  return out;
}

struct ModFile {
  std::string name;  // GamePath relative to game:\media\, e.g. "stringtables/en/main.str"
  std::filesystem::path path;
  uint32_t size = 0, crc = 0;
  bool loose_only = false;  // over kMaxMember: only replaces a loose file (no CRC computed)
};

struct ScannedMod {
  std::string folder;  // its folder's name: the name in forza_mods
  Manifest manifest;
  std::vector<ModFile> files;   // sorted by name
  std::vector<std::string> ignored;  // files left out, "path: why"
  std::string error;            // set: the whole mod is skipped
  std::string Title() const {
    std::string t = manifest.name.empty() ? folder : manifest.name;
    if (!manifest.version.empty()) t += " " + manifest.version;
    if (!manifest.author.empty()) t += " by " + manifest.author;
    return t;
  }
};

// UTF-8 text of a path, and a path from UTF-8 text (std::string on every standard: u8string is
// std::u8string since C++20).
inline std::string Utf8(const std::filesystem::path& p) {
  const auto s = p.u8string();
  return std::string(s.begin(), s.end());
}
inline std::string Utf8Generic(const std::filesystem::path& p) {
  const auto s = p.generic_u8string();
  return std::string(s.begin(), s.end());
}
inline std::filesystem::path PathOf(const std::string& utf8) {
#if defined(__cpp_char8_t)
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
  return std::filesystem::u8path(utf8);
#endif
}

inline std::string Lower(std::string s) {
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return s;
}

// Reads a mod folder: mod.toml, then every file under media/ (its CRC-32 too). A file that
// cannot be read, or one over 2 GB, skips the whole mod (error); files the game would not find
// (outside media/, non-ASCII names, paths over 255 bytes) and whole archives are left out
// (ignored) and the rest loads. A file over 64 MB is marked loose_only: the caller keeps it
// only if it replaces a loose file of the game. Links are followed; names are the paths under
// media/ as listed, not where the links lead.
inline ScannedMod ScanMod(const std::filesystem::path& mods_dir, const std::string& folder) {
  namespace fs = std::filesystem;
  ScannedMod mod;
  mod.folder = folder;
  const fs::path root = mods_dir / PathOf(folder);
  std::error_code ec;
  if (!fs::is_directory(root, ec)) {
    mod.error = "folder not found: " + Utf8(root);
    return mod;
  }
  const fs::path manifest = root / "mod.toml";
  if (fs::exists(manifest, ec)) {
    std::ifstream in(manifest, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in.good() && !in.eof()) {
      mod.error = "cannot read mod.toml";
      return mod;
    }
    mod.manifest = ParseManifest(text);
  }
  const fs::path media = root / "media";
  if (!fs::is_directory(media, ec)) {
    mod.error = "no media folder: put the files under " + folder + "/media/, as the game's paths are";
    return mod;
  }
  fs::directory_iterator top(root, ec);
  for (; !ec && top != fs::directory_iterator(); top.increment(ec)) {
    const std::string n = Lower(Utf8(top->path().filename()));
    if (n == "media" || n == "mod.toml") continue;
    std::error_code dir_ec;
    if (top->is_directory(dir_ec)) mod.ignored.push_back(Utf8(top->path().filename()) + "/: outside media/, the game does not see it");
  }
  ec.clear();
  fs::recursive_directory_iterator it(media, fs::directory_options::follow_directory_symlink, ec);
  if (ec) {
    mod.error = "cannot list media/: " + ec.message();
    return mod;
  }
  std::vector<char> buffer(1 << 16);
  for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      mod.error = "cannot list media/: " + ec.message();
      return mod;
    }
    const fs::directory_entry& entry = *it;
    if (entry.is_directory(ec)) continue;
    // lexically_relative: fs::relative would resolve links and name a linked file by its
    // target ("../../work/x").
    const fs::path rel_path = entry.path().lexically_relative(media);
    const std::string relative = Utf8Generic(rel_path);
    const std::string shown = "media/" + relative;
    bool outside = relative.empty();
    for (const auto& part : rel_path) outside = outside || part == ".." || part == ".";
    if (outside) {
      mod.ignored.push_back(Utf8(entry.path()) + ": not under media/");
      continue;
    }
    if (!entry.is_regular_file(ec)) {
      ec.clear();
      mod.ignored.push_back(shown + ": not a regular file");
      continue;
    }
    bool ascii = true;
    for (unsigned char c : relative)
      if (c < 0x20 || c > 0x7E || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
          c == '>' || c == '|')
        ascii = false;
    if (!ascii) {
      mod.ignored.push_back(shown + ": the game's paths are plain ASCII");
      continue;
    }
    const std::string name = GamePath(relative);
    if (name.size() > kMaxName) {
      mod.ignored.push_back(shown + ": its path is over 255 characters, longer than the game's");
      continue;
    }
    if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".zip") == 0) {
      mod.ignored.push_back(shown + ": whole archives are not replaced; put the files from inside "
                            "it under media/" + name.substr(0, name.size() - 4) + "/ instead");
      continue;
    }
    const uintmax_t size = entry.file_size(ec);
    if (ec) {
      mod.error = "cannot read " + shown + ": " + ec.message();
      return mod;
    }
    if (size > kMaxModFile) {
      mod.error = shown + " is over 2 GB, too big for the game";
      return mod;
    }
    const bool loose_only = size > kMaxMember;
    std::FILE* f = nullptr;
#ifdef _WIN32
    f = _wfopen(entry.path().c_str(), L"rb");
#else
    f = std::fopen(entry.path().c_str(), "rb");
#endif
    if (!f) {
      mod.error = "cannot read " + shown;
      return mod;
    }
    uint32_t crc = 0;
    uintmax_t total = loose_only ? size : 0;
    for (; !loose_only;) {
      const size_t n = std::fread(buffer.data(), 1, buffer.size(), f);
      crc = Crc32(buffer.data(), n, crc);
      total += n;
      if (n < buffer.size()) break;
    }
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed || total != size) {
      mod.error = "cannot read " + shown + (failed ? "" : " (its size changed while it was read)");
      return mod;
    }
    mod.files.push_back({name, entry.path(), uint32_t(size), crc, loose_only});
  }
  std::sort(mod.files.begin(), mod.files.end(),
            [](const ModFile& a, const ModFile& b) { return a.name < b.name; });
  for (size_t i = 1; i < mod.files.size(); ++i) {
    if (mod.files[i].name == mod.files[i - 1].name) {
      mod.error = "two files differ only in case: media/" + mod.files[i].name;
      return mod;
    }
  }
  if (mod.files.empty()) mod.error = "no files under media/";
  return mod;
}

struct Conflict {
  std::string name;
  size_t loser, winner;  // mod indexes
};

struct MergedFile {
  ModFile file;
  size_t mod = 0;
};

// The files of mods in load order: a later mod's file wins over an earlier one's.
inline std::vector<MergedFile> MergeMods(const std::vector<ScannedMod>& mods,
                                         std::vector<Conflict>* conflicts) {
  std::map<std::string, MergedFile> merged;
  for (size_t m = 0; m < mods.size(); ++m) {
    if (!mods[m].error.empty()) continue;
    for (const ModFile& f : mods[m].files) {
      auto [it, added] = merged.try_emplace(f.name, MergedFile{f, m});
      if (!added) {
        if (conflicts) conflicts->push_back({f.name, it->second.mod, m});
        it->second = MergedFile{f, m};
      }
    }
  }
  std::vector<MergedFile> out;
  out.reserve(merged.size());
  for (auto& [name, file] : merged) out.push_back(std::move(file));
  return out;
}

}  // namespace forza::mods
