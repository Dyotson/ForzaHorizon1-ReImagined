// Mods (src/platform/mod_archive.h, #149): the archive built from the title update's
// media.zip and the mods' files, the mod folders and their load order.
// Standalone: c++ -std=c++20 -I forza-recomp/src forza-recomp/tests/mod_archive_test.cpp -o mod_archive_test
// MOD_ARCHIVE_OUT=file.zip writes the test's overlay archive there (for a check with other zip tools).
#include "platform/mod_archive.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace m = forza::mods;
namespace fs = std::filesystem;

namespace {

int failures = 0;
#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK(%s)\n", __FILE__, __LINE__, #c); \
      ++failures;                                                   \
    }                                                               \
  } while (0)

// Host files of a test: index -> bytes.
struct Sources {
  std::vector<std::string> files;
  size_t shrink_to = std::string::npos;  // file 1 reads as if this long (it shrank)
  m::SourceRead Reader() const {
    return [this](int32_t source, uint64_t offset, uint8_t* buffer, size_t size) -> size_t {
      std::string data = files.at(size_t(source));
      if (source == 1 && shrink_to != std::string::npos) data.resize(shrink_to);
      if (offset >= data.size()) return 0;
      size_t n = std::min<size_t>(size, data.size() - size_t(offset));
      std::memcpy(buffer, data.data() + offset, n);
      return n;
    };
  }
};

std::string Materialize(const m::Layout& layout, const Sources& sources) {
  std::string out(size_t(layout.size), '\0');
  size_t n = m::ReadLayout(layout, 0, reinterpret_cast<uint8_t*>(out.data()), out.size(),
                           sources.Reader());
  CHECK(n == out.size());
  return out;
}

m::ReadAt ReaderOf(const std::string& bytes) {
  return [&bytes](uint64_t offset, void* buffer, size_t size) {
    if (offset + size > bytes.size()) return false;
    std::memcpy(buffer, bytes.data() + offset, size);
    return true;
  };
}

// The data of member `e` through the game's extra field (as the game reads it), checked
// against the local header and the CRC.
std::string MemberData(const std::string& zip, const m::ZipEntry& e) {
  const auto* r = reinterpret_cast<const uint8_t*>(e.record.data());
  size_t at = 46 + m::Le16(r + 28);
  const size_t extra_end = at + m::Le16(r + 30);
  uint32_t data_offset = 0;
  bool found = false;
  while (at + 4 <= extra_end) {
    uint16_t tag = m::Le16(r + at), len = m::Le16(r + at + 2);
    if (tag == m::kDataOffsetField && len == 4) {
      data_offset = m::Le32(r + at + 4);
      found = true;
    }
    at += 4 + len;
  }
  CHECK(found);
  const auto* z = reinterpret_cast<const uint8_t*>(zip.data());
  CHECK(m::Le32(z + e.local_offset) == m::kLocalSignature);
  const uint32_t local_data =
      e.local_offset + 30 + m::Le16(z + e.local_offset + 26) + m::Le16(z + e.local_offset + 28);
  CHECK(local_data == data_offset);
  std::string data = zip.substr(data_offset, e.compressed);
  if (e.method == 0) CHECK(m::Crc32(data.data(), data.size()) == e.crc);
  return data;
}

const m::ZipEntry* Find(const m::ZipDirectory& d, const std::string& name) {
  for (const auto& e : d.entries)
    if (e.name == name) return &e;
  return nullptr;
}

void TestCrc() {
  CHECK(m::Crc32("123456789", 9) == 0xCBF43926u);
  // In pieces, as ScanMod reads a file.
  CHECK(m::Crc32("6789", 4, m::Crc32("12345", 5)) == 0xCBF43926u);
  CHECK(m::Crc32("", 0) == 0);
}

void TestGamePath() {
  CHECK(m::GamePath("Stringtables\\EN\\Main.str") == "stringtables/en/main.str");
  CHECK(m::GamePath("db//gamedb.slt") == "db/gamedb.slt");
}

void TestArchive() {
  // A base archive made by BuildArchive itself (as the update's: stored members with the
  // data offset field), then the overlay on it.
  Sources src;
  src.files = {"", "<a>original</a>", "second file", "gamedb bytes"};
  std::vector<m::AddedFile> base_files = {
      {"Gamemodes/Autoshows.xml", 15, m::Crc32(src.files[1].data(), 15), 1},
      {"stringtables/en/main.str", 11, m::Crc32(src.files[2].data(), 11), 2},
      {"db/gamedb.slt", 12, m::Crc32(src.files[3].data(), 12), 3},
  };
  m::Layout base_layout;
  std::vector<bool> replaced;
  std::string error;
  CHECK(m::BuildArchive(nullptr, 0, base_files, &base_layout, &replaced, &error));
  const std::string base = Materialize(base_layout, src);
  m::ZipDirectory base_dir;
  CHECK(m::ReadZipDirectory(ReaderOf(base), base.size(), &base_dir, &error));
  CHECK(base_dir.entries.size() == 3);
  CHECK(MemberData(base, *Find(base_dir, "db/gamedb.slt")) == "gamedb bytes");

  // The overlay: the base is source 0, mod files 4 and 5.
  Sources over;
  over.files = {base, "", "", "", "<a>modded</a>!", "new"};
  std::vector<m::AddedFile> mod_files = {
      {"gamemodes/autoshows.xml", 14, m::Crc32(over.files[4].data(), 14), 4},
      {"cars/new_file.xml", 3, m::Crc32("new", 3), 5},
  };
  m::Layout layout;
  CHECK(m::BuildArchive(&base_dir, 0, mod_files, &layout, &replaced, &error));
  CHECK(replaced.size() == 2 && replaced[0] && !replaced[1]);
  const std::string zip = Materialize(layout, over);
  // The base's bytes before its directory are unchanged, at the same offsets.
  CHECK(zip.compare(0, base_dir.directory_offset, base, 0, base_dir.directory_offset) == 0);
  m::ZipDirectory dir;
  CHECK(m::ReadZipDirectory(ReaderOf(zip), zip.size(), &dir, &error));
  CHECK(dir.entries.size() == 4);
  // Untouched members: their records verbatim.
  CHECK(Find(dir, "db/gamedb.slt")->record == Find(base_dir, "db/gamedb.slt")->record);
  CHECK(MemberData(zip, *Find(dir, "db/gamedb.slt")) == "gamedb bytes");
  CHECK(MemberData(zip, *Find(dir, "stringtables/en/main.str")) == "second file");
  // The replaced member keeps the base's spelling and holds the mod's bytes, stored.
  CHECK(Find(dir, "gamemodes/autoshows.xml") == nullptr);
  const m::ZipEntry* swapped = Find(dir, "Gamemodes/Autoshows.xml");
  CHECK(swapped && swapped->method == 0 && swapped->size == 14 && swapped->compressed == 14);
  CHECK(swapped && MemberData(zip, *swapped) == "<a>modded</a>!");
  CHECK(Find(dir, "cars/new_file.xml") && MemberData(zip, *Find(dir, "cars/new_file.xml")) == "new");
  if (const char* out = std::getenv("MOD_ARCHIVE_OUT")) std::ofstream(out, std::ios::binary) << zip;

  // Reads across segment edges match the whole.
  for (uint64_t at = 0; at < zip.size(); at += 7) {
    uint8_t buffer[23];
    size_t n = m::ReadLayout(layout, at, buffer, sizeof(buffer), over.Reader());
    CHECK(n == std::min<size_t>(sizeof(buffer), zip.size() - at));
    CHECK(std::memcmp(buffer, zip.data() + at, n) == 0);
  }
  uint8_t byte;
  CHECK(m::ReadLayout(layout, zip.size(), &byte, 1, over.Reader()) == 0);

  // Two files with one name are refused.
  std::vector<m::AddedFile> twice = {mod_files[0], mod_files[0]};
  twice[1].name = "GAMEMODES/autoshows.XML";
  CHECK(!m::BuildArchive(&base_dir, 0, twice, &layout, &replaced, &error));
}

void TestShortSource() {
  Sources src;
  src.files = {"", "abcdef"};
  m::Layout layout;
  layout.AddBytes("hdr");
  layout.AddFile(1, 0, 6);
  src.shrink_to = 4;  // the file lost 2 bytes after the archive was built
  std::vector<size_t> touched;
  int shorts = 0;
  uint8_t buffer[9];
  size_t n = m::ReadLayout(layout, 0, buffer, 9, src.Reader(),
                           [&](size_t i) { touched.push_back(i); },
                           [&](int32_t s) { shorts += s == 1; });
  CHECK(n == 9);
  CHECK(std::memcmp(buffer, "hdrabcd\0\0", 9) == 0);
  CHECK(shorts == 1);
  CHECK((touched == std::vector<size_t>{0, 1}));
  CHECK(layout.Find(2) == 0 && layout.Find(3) == 1 && layout.Find(9) == layout.segments.size());
}

void TestManifest() {
  auto mf = m::ParseManifest(
      "\xEF\xBB\xBF# comment\r\nname = \"Title \\\"text\\\"\"\r\nversion='1.2'\n"
      "author = \"Someone\"  # trailing\n[extra]\ndescription = \"two\\nlines\"\nbroken = \"x\n");
  CHECK(mf.name == "Title \"text\"");
  CHECK(mf.version == "1.2");
  CHECK(mf.author == "Someone");
  CHECK(mf.description == "two\nlines");
  CHECK(m::ParseManifest("").name.empty());
}

void TestList() {
  CHECK((m::SplitModList("A | B|A||C ") == std::vector<std::string>{"A", "B", "C"}));
  CHECK(m::SplitModList("").empty());
}

void Write(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p, std::ios::binary) << text;
}

void TestFolders() {
  const fs::path root = fs::temp_directory_path() / ("mod_archive_test_" + std::to_string(std::rand()));
  fs::remove_all(root);
  Write(root / "First/mod.toml", "name = \"First mod\"\nversion = \"1.0\"\nauthor = \"Me\"\n");
  Write(root / "First/media/Gamemodes/Autoshows.xml", "first");
  Write(root / "First/media/db/gamedb.slt", "db");
  Write(root / "First/media/cars/abc_car_01.zip", "PK");
  Write(root / "First/extras/readme.txt", "x");
  Write(root / "Second/media/gamemodes/autoshows.xml", "second wins");
  Write(root / "Empty/mod.toml", "name = \"Nothing\"\n");
  fs::create_directories(root / "NoFiles/media/empty");

  m::ScannedMod first = m::ScanMod(root, "First");
  CHECK(first.error.empty());
  CHECK(first.Title() == "First mod 1.0 by Me");
  CHECK(first.files.size() == 2);
  CHECK(first.files[0].name == "db/gamedb.slt" && first.files[0].size == 2 &&
        first.files[0].crc == m::Crc32("db", 2));
  CHECK(first.files[1].name == "gamemodes/autoshows.xml");
  CHECK(first.ignored.size() == 2);  // the zip and extras/
  m::ScannedMod second = m::ScanMod(root, "Second");
  CHECK(second.error.empty() && second.Title() == "Second");
  CHECK(!m::ScanMod(root, "Empty").error.empty());
  CHECK(!m::ScanMod(root, "NoFiles").error.empty());
  CHECK(!m::ScanMod(root, "Missing").error.empty());

  std::vector<m::Conflict> conflicts;
  auto merged = m::MergeMods({first, second, m::ScanMod(root, "Missing")}, &conflicts);
  CHECK(merged.size() == 2);
  CHECK(conflicts.size() == 1 && conflicts[0].name == "gamemodes/autoshows.xml" &&
        conflicts[0].loser == 0 && conflicts[0].winner == 1);
  for (const auto& f : merged)
    if (f.file.name == "gamemodes/autoshows.xml") CHECK(f.mod == 1 && f.file.size == 11);
  fs::remove_all(root);
}

}  // namespace

int main() {
  TestCrc();
  TestGamePath();
  TestArchive();
  TestShortSource();
  TestManifest();
  TestList();
  TestFolders();
  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("mod archive tests passed");
  return 0;
}
