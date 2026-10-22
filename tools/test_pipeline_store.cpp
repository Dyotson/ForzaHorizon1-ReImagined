// Pipeline store loading and compaction (patch 0102). No game files, GPU or Vulkan SDK required.
// Records have the layout of the SDK's PipelineStoredDescription (a hash, then a 58-byte
// description); a test hash replaces XXH3, and a marker byte stands for what the device rejects.
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS  // fopen, as the game's stdio storage files
#endif
#include <rex/graphics/pipeline_store_file.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
using namespace rex::graphics::pipeline_store_file;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)

#pragma pack(push, 1)
struct Record {
  uint64_t hash;
  uint8_t description[58];
};
#pragma pack(pop)
static_assert(sizeof(Record) == 66, "the Clang/GCC record size");

namespace {
const uint32_t kApi = 0;
const Versions kVersions{0x20260228, 0x20261018};

uint64_t Hash(const uint8_t* p, size_t n) {
  uint64_t h = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * UINT64_C(1099511628211);
  return h;
}
Record Make(uint32_t id, bool supported = true) {
  Record r{};
  std::memcpy(r.description, &id, 4);
  r.description[4] = supported ? 0 : 0xFF;
  r.hash = Hash(r.description, sizeof(r.description));
  return r;
}
uint32_t Id(const Record& r) { uint32_t id; std::memcpy(&id, r.description, 4); return id; }
bool Valid(const Record& r) { return Hash(r.description, sizeof(r.description)) == r.hash; }
bool Supported(const Record& r) { return r.description[4] != 0xFF; }
std::FILE* Open(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
  return _wfopen(path.c_str(), std::wstring(mode, mode + std::strlen(mode)).c_str());
#else
  return std::fopen(path.c_str(), mode);
#endif
}

std::vector<uint8_t> LegacyHeader(uint32_t api, uint32_t version) {
  const uint32_t words[3] = {kMagic, api, SwapBytes(version)};
  return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(words),
                              reinterpret_cast<const uint8_t*>(words) + 12);
}
std::vector<uint8_t> Header2(uint32_t api, Versions v) {
  const auto h = EncodeHeader(api, v);
  return std::vector<uint8_t>(h.begin(), h.end());
}
void WriteFile(const std::filesystem::path& path, std::vector<uint8_t> bytes,
               const std::vector<Record>& records, size_t extra = 0) {
  for (const Record& r : records) {
    const auto* p = reinterpret_cast<const uint8_t*>(&r);
    bytes.insert(bytes.end(), p, p + sizeof(r));
  }
  bytes.insert(bytes.end(), extra, uint8_t(0x5A));
  std::ofstream(path, std::ios::binary | std::ios::trunc)
      .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}
std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}
std::vector<uint32_t> Ids(const std::vector<Record>& records) {
  std::vector<uint32_t> ids;
  for (const Record& r : records) ids.push_back(Id(r));
  return ids;
}

struct Startup {
  Header header = Header::kNone;
  LoadResult<Record> load;
  bool rewritten = false;
};
// What InitializeShaderStorage does with the store: open it for appending, read it, and
// rewrite it when anything was left out. Returns with the file still open in `file`.
Startup RunStartup(const std::filesystem::path& path, std::FILE*& file, Versions v = kVersions,
                   uint32_t api = kApi) {
  Startup s;
  file = Open(path, "a+b");
  CHECK(file);
  s.header = ReadHeader(file, api, v);
  if (s.header != Header::kNone) s.load = LoadRecords<Record>(file, Valid, Supported);
  if (s.load.NeedsRewrite(s.header)) {
    CHECK(Rewrite(path, file, api, v, s.load.kept, Open));
    s.rewritten = true;
  }
  CHECK(file);
  return s;
}
Startup RunStartup(const std::filesystem::path& path, Versions v = kVersions, uint32_t api = kApi) {
  std::FILE* file = nullptr;
  Startup s = RunStartup(path, file, v, api);
  std::fclose(file);
  return s;
}
std::vector<uint8_t> Expected(const std::vector<Record>& records) {
  std::vector<uint8_t> bytes = Header2(kApi, kVersions);
  for (const Record& r : records) {
    const auto* p = reinterpret_cast<const uint8_t*>(&r);
    bytes.insert(bytes.end(), p, p + sizeof(r));
  }
  return bytes;
}
}  // namespace

int main() {
  const auto temporary = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path());
  auto root = temporary / ("fh1-pipeline-store-test-" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  bool created = false;
  auto cleanup = [&] {
    if (created && std::filesystem::weakly_canonical(root).parent_path() == temporary)
      std::filesystem::remove_all(root);
  };
  try {
    CHECK(std::filesystem::create_directory(root)); created = true;
    const auto path = root / "4D5309C9.fbo.vk.xpso";

    // Valid records with rejected ones among them, the last records included. Every valid
    // record survives, in order, and the file holds exactly those.
    std::vector<Record> records;
    for (uint32_t i = 0; i < 12; ++i) records.push_back(Make(i, i != 2 && i != 5 && i != 7 && i != 10));
    WriteFile(path, Header2(kApi, kVersions), records);
    auto s = RunStartup(path);
    CHECK(s.header == Header::kCurrent && s.rewritten);
    CHECK((Ids(s.load.kept) == std::vector<uint32_t>{0, 1, 3, 4, 6, 8, 9, 11}));
    CHECK((Ids(s.load.unsupported) == std::vector<uint32_t>{2, 5, 7, 10}));
    CHECK(s.load.corrupted == 0 && !s.load.partial);
    CHECK(ReadFile(path) == Expected(s.load.kept));
    // The old truncation to header + kept count kept records 0-7: the rejected 2, 5 and 7
    // stayed, and the newest valid ones (8, 9, 11) were cut. They are all here now.
    // A second start reads the same records and leaves the file alone.
    auto again = RunStartup(path);
    CHECK(again.header == Header::kCurrent && !again.rewritten);
    CHECK(Ids(again.load.kept) == Ids(s.load.kept));

    // Pipelines appended after the rewrite follow the kept records.
    {
      std::FILE* file = nullptr;
      RunStartup(path, file);
      const Record added = Make(12);
      CHECK(std::fwrite(&added, sizeof(added), 1, file) == 1);
      std::fclose(file);
      auto after = RunStartup(path);
      CHECK((Ids(after.load.kept) == std::vector<uint32_t>{0, 1, 3, 4, 6, 8, 9, 11, 12}));
      CHECK(!after.rewritten);
    }

    // A record failing its hash ends the store: the valid ones before it stay, the rest goes.
    records.clear();
    for (uint32_t i = 0; i < 10; ++i) records.push_back(Make(i, i != 3));
    records[6].hash ^= 1;
    WriteFile(path, Header2(kApi, kVersions), records);
    s = RunStartup(path);
    CHECK((Ids(s.load.kept) == std::vector<uint32_t>{0, 1, 2, 4, 5}));
    CHECK(s.load.corrupted == 4 && s.load.unsupported.size() == 1 && s.rewritten);
    CHECK(ReadFile(path) == Expected(s.load.kept));

    // A partial record at the end (a write cut short) is dropped.
    records.clear();
    for (uint32_t i = 0; i < 5; ++i) records.push_back(Make(i));
    WriteFile(path, Header2(kApi, kVersions), records, 30);
    s = RunStartup(path);
    CHECK(s.load.partial && s.rewritten && Ids(s.load.kept).size() == 5);
    CHECK(ReadFile(path) == Expected(records));

    // The original 12-byte header, written with the same versions, is kept and converted.
    records.clear();
    for (uint32_t i = 0; i < 7; ++i) records.push_back(Make(i, i != 4));
    WriteFile(path, LegacyHeader(kApi, 0x20261018), records);
    s = RunStartup(path);
    CHECK(s.header == Header::kLegacy && s.rewritten);
    CHECK((Ids(s.load.kept) == std::vector<uint32_t>{0, 1, 2, 3, 5, 6}));
    CHECK(ReadFile(path) == Expected(s.load.kept));
    CHECK(RunStartup(path).header == Header::kCurrent);

    // ... but not one written with other versions, nor one of the other render path.
    WriteFile(path, LegacyHeader(kApi, 0x20261017), records);
    s = RunStartup(path);
    CHECK(s.header == Header::kNone && s.load.kept.empty() && ReadFile(path) == Expected({}));
    WriteFile(path, LegacyHeader(1, 0x20261018), records);
    CHECK(RunStartup(path).header == Header::kNone);
    WriteFile(path, Header2(1, kVersions), records);
    CHECK(RunStartup(path).header == Header::kNone);

    // Each version counts on its own: raising the smaller one (the description version here)
    // leaves the larger one, all the original header had, unchanged.
    WriteFile(path, Header2(kApi, kVersions), records);
    const Versions raised{0x20260301, kVersions.modification};
    CHECK(std::max(raised.description, raised.modification) ==
          std::max(kVersions.description, kVersions.modification));
    s = RunStartup(path, raised);
    CHECK(s.header == Header::kNone && s.load.kept.empty());
    WriteFile(path, Header2(kApi, kVersions), records);
    CHECK(RunStartup(path, Versions{kVersions.description, 0x20261101}).header == Header::kNone);

    // A new store gets a header and nothing else.
    std::filesystem::remove(path);
    s = RunStartup(path);
    CHECK(s.header == Header::kNone && s.rewritten && ReadFile(path) == Expected({}));
    CHECK(RunStartup(path).header == Header::kCurrent);

    // When the temporary file cannot be made, the store stays as it was and stays open.
    records.clear();
    for (uint32_t i = 0; i < 4; ++i) records.push_back(Make(i, i != 1));
    WriteFile(path, Header2(kApi, kVersions), records);
    const auto before = ReadFile(path);
    std::filesystem::create_directory(root / "4D5309C9.fbo.vk.xpso.new");
    {
      std::FILE* file = Open(path, "a+b");
      CHECK(ReadHeader(file, kApi, kVersions) == Header::kCurrent);
      auto load = LoadRecords<Record>(file, Valid, Supported);
      CHECK(load.NeedsRewrite(Header::kCurrent));
      std::FILE* const original = file;
      CHECK(!Rewrite(path, file, kApi, kVersions, load.kept, Open));
      CHECK(file == original);
      CHECK(ReadHeader(file, kApi, kVersions) == Header::kCurrent);
      std::fclose(file);
    }
    CHECK(ReadFile(path) == before);
    std::filesystem::remove(root / "4D5309C9.fbo.vk.xpso.new");

    // Many records, one in three rejected: all others kept in order.
    records.clear();
    std::vector<uint32_t> expected;
    for (uint32_t i = 0; i < 3000; ++i) {
      records.push_back(Make(i, i % 3 != 1));
      if (i % 3 != 1) expected.push_back(i);
    }
    WriteFile(path, Header2(kApi, kVersions), records);
    s = RunStartup(path);
    CHECK(Ids(s.load.kept) == expected && s.load.unsupported.size() == 1000);
    CHECK(Ids(RunStartup(path).load.kept) == expected);

    cleanup();
    std::cout << "Pipeline store: rejected records dropped, every valid record kept in order, "
                 "versions checked separately, legacy header converted, rewrite failure safe\n";
    return 0;
  } catch (const std::exception& e) {
    cleanup();
    std::cerr << e.what() << '\n';
    return 1;
  }
}
