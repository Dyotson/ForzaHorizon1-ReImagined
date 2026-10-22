// The shader prebuild's pipeline list (patch 0112, docs/shader_prebuild.md). No game files, GPU or
// Vulkan SDK required. Description is a copy of the SDK's VulkanPipelineCache::PipelineDescription
// declarations (packed bit fields, enums of uint32_t): ToDescription must produce exactly the bytes
// of the descriptions the game stored, in this compiler's layout (58 bytes with Clang/GCC, 60 with
// MSVC and clang-cl), from the same compiler-independent list.
#include <rex/graphics/pipeline_prebuild_file.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace rex::graphics::pipeline_prebuild_file;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("line " + std::to_string(__LINE__) + ": " #x); } while (false)

namespace {
enum class E : uint32_t {};  // stands for the SDK's enum types (all with uint32_t underneath)
union RenderPassKey {
  struct {
    E msaa_samples : 2;
    uint32_t depth_and_color_used : 5;
    E depth_format : 1;
    E color_0_view_format : 4;
    E color_1_view_format : 4;
    E color_2_view_format : 4;
    E color_3_view_format : 4;
    uint32_t color_rts_use_transfer_formats : 1;
  };
  uint32_t key = 0;
};
#pragma pack(push, 1)
struct RenderTarget {
  E src_color_blend_factor : 4;
  E dst_color_blend_factor : 4;
  E color_blend_op : 3;
  E src_alpha_blend_factor : 4;
  E dst_alpha_blend_factor : 4;
  E alpha_blend_op : 3;
  uint32_t color_write_mask : 4;
};
struct Description {
  uint64_t vertex_shader_hash;
  uint64_t vertex_shader_modification;
  uint64_t pixel_shader_hash;
  uint64_t pixel_shader_modification;
  RenderPassKey render_pass_key;
  E geometry_shader : 2;
  E primitive_topology : 3;
  uint32_t primitive_restart : 1;
  E tessellation_mode : 2;
  uint32_t depth_clamp_enable : 1;
  E polygon_mode : 2;
  uint32_t cull_front : 1;
  uint32_t cull_back : 1;
  uint32_t front_face_clockwise : 1;
  uint32_t rasterizer_discard : 1;
  uint32_t depth_write_enable : 1;
  E depth_compare_op : 3;
  uint32_t stencil_test_enable : 1;
  E stencil_front_fail_op : 3;
  E stencil_front_pass_op : 3;
  E stencil_front_depth_fail_op : 3;
  E stencil_front_compare_op : 3;
  E stencil_back_fail_op : 3;
  E stencil_back_pass_op : 3;
  E stencil_back_depth_fail_op : 3;
  E stencil_back_compare_op : 3;
  uint32_t sample_rate_shading : 1;
  RenderTarget render_targets[4];
};
#pragma pack(pop)

std::vector<uint8_t> Hex(const char* s) {
  std::vector<uint8_t> out;
  for (; s[0] && s[1]; s += 2) out.push_back(uint8_t(std::stoi(std::string(s, 2), nullptr, 16)));
  return out;
}

// A pipeline the game stored on Linux (RADV): depth test, stencil, alpha blending into render
// target 0. Its fields in list order, and its stored description in both layouts
// (tools/pipeline_list.py encode_description).
Entry Sample() {
  Entry e;
  e.vertex_shader_hash = 0x9B5EE39E84345522;
  e.vertex_shader_modification = 0x3;
  e.pixel_shader_hash = 0x8CDF6BDE45CB52DD;
  e.pixel_shader_modification = 0x0000400000000003;
  e.flags = kPartitionMain;
  const uint8_t fields[kFieldCount] = {
      1, 3, 1, 0, 0, 0, 0, 0,                                      // render pass key
      0, 3, 0, 0, 0, 0, 0, 1, 1, 0, 0, 7, 1, 0, 0, 0, 2, 0, 0, 0, 2, 0,  // state
      6, 7, 0, 6, 7, 0, 7,                                         // render target 0
  };
  std::memcpy(e.fields.data(), fields, kFieldCount);
  return e;
}
const char* kClangBytes =
    "225534849ee35e9b0300000000000000dd52cb45de6bdf8c03000000004000008d0000000c300f40000476b0c3"
    "01000000000000000000000000";
const char* kMsvcBytes =
    "225534849ee35e9b0300000000000000dd52cb45de6bdf8c03000000004000008d0000000c300f400004000076"
    "b0c301000000000000000000000000";

// The bytes tools/pipeline_list.py writes for [Sample()] (fbo, versions 20260228/20261018): the
// Python and C++ encoders must agree (launcher/test_shader_prebuild.py checks the same bytes).
const char* kListBytes =
    "5845504c010000000000000028022620181026203a0000000100000060000000b180d814877653ac22553484"
    "9ee35e9b0300000000000000dd52cb45de6bdf8c030000000040000001000000010301000000000000030000"
    "0000000101000007010000000200000002000607000607000700000000000000000000000000000000000000"
    "00000000";

const Versions kVersions{0x20260228, 0x20261018};

void TestDescriptionLayout() {
  Description d;
  std::memset(static_cast<void*>(&d), 0, sizeof(d));
  ToDescription(Sample(), d);
  const auto expected = Hex(sizeof(Description) == 58 ? kClangBytes : kMsvcBytes);
  CHECK(sizeof(Description) == 58 || sizeof(Description) == 60);
  CHECK(expected.size() == sizeof(Description));
  CHECK(std::memcmp(&d, expected.data(), sizeof(d)) == 0);
  std::cout << "description layout: " << sizeof(Description) << " bytes, as stored by the game\n";
}

void TestRoundTrip() {
  std::vector<Entry> entries{Sample()};
  Entry b = Sample();
  b.pixel_shader_hash = 0;
  b.flags = kPartitionUi | kPartitionScene;
  entries.push_back(b);
  const auto bytes = Encode(entries, 0, kVersions);
  CHECK(bytes.size() == kHeaderSize + 2 * kRecordSize);
  const ParseResult r = Parse(bytes.data(), bytes.size(), 0, kVersions);
  CHECK(r.status == Status::kOk);
  CHECK(r.entries.size() == 2 && r.invalid == 0);
  CHECK(r.entries[0].fields == entries[0].fields);
  CHECK(r.entries[0].pixel_shader_modification == 0x0000400000000003);
  CHECK(r.entries[1].pixel_shader_hash == 0 && r.entries[1].flags == (kPartitionUi | kPartitionScene));
  // The Python encoder's bytes.
  const auto python = Hex(kListBytes);
  CHECK(python == Encode({Sample()}, 0, kVersions));
  const ParseResult p = Parse(python.data(), python.size(), 0, kVersions);
  CHECK(p.status == Status::kOk && p.entries.size() == 1 && p.entries[0].fields == Sample().fields);
}

void TestRefused() {
  const auto good = Encode({Sample()}, 0, kVersions);
  CHECK(Parse(good.data(), good.size(), 1, kVersions).status == Status::kOtherApi);
  CHECK(Parse(good.data(), good.size(), 0, {0x20260228, 0x20261019}).status ==
        Status::kOtherVersions);
  CHECK(Parse(good.data(), good.size(), 0, {0x20260301, 0x20261018}).status ==
        Status::kOtherVersions);
  auto damaged = good;
  damaged[kHeaderSize + 40] ^= 1;
  CHECK(Parse(damaged.data(), damaged.size(), 0, kVersions).status == Status::kDamaged);
  CHECK(Parse(good.data(), good.size() - 1, 0, kVersions).status == Status::kDamaged);
  CHECK(Parse(good.data(), 12, 0, kVersions).status == Status::kDamaged);
  auto magic = good;
  magic[0] = 'Y';
  CHECK(Parse(magic.data(), magic.size(), 0, kVersions).status == Status::kDamaged);

  // Values outside an enum, an unknown flag or no vertex shader: the record is skipped.
  std::vector<Entry> entries;
  for (int i = 0; i < 5; ++i) entries.push_back(Sample());
  entries[0].fields[kRtSrcColorBlendFactor] = 15;  // 4 bits, 15 blend factors
  entries[1].fields[kPolygonMode] = 3;
  entries[2].flags |= 1u << 20;
  entries[3].vertex_shader_hash = 0;
  entries[4].fields[kFirstRenderTargetField + 3 * kRenderTargetFieldCount + 2] = 5;  // RT 3 op
  entries.push_back(Sample());
  const auto bytes = Encode(entries, 0, kVersions);
  const ParseResult r = Parse(bytes.data(), bytes.size(), 0, kVersions);
  CHECK(r.status == Status::kOk && r.entries.size() == 1 && r.invalid == 5);
}

void TestSelect() {
  Entry main = Sample();  // main
  Entry ui = Sample();
  ui.flags = kPartitionUi;
  ui.vertex_shader_hash = 2;
  Entry both = Sample();
  both.flags = kPartitionMain | kPartitionScene;
  both.vertex_shader_hash = 3;
  Entry s8 = Sample();  // the render pass key keeps the guest's D24S8 on every device
  s8.vertex_shader_hash = 4;
  s8.fields[kDepthFormat] = 0;
  Entry raster = Sample();
  raster.vertex_shader_hash = 6;
  raster.pixel_shader_modification |= UINT64_C(1) << 63;
  const std::vector<Entry> list{main, ui, both, s8, raster, main};

  Target target;
  target.unused_modification_bits = UINT64_C(1) << 63;
  SelectResult r = Select(list, target);
  // main once, both, s8; the raster variant is never used here; ui is another partition.
  CHECK(r.entries.size() == 3 && r.other_partition == 1 && r.unused_variant == 1);
  CHECK(r.entries[0].vertex_shader_hash == main.vertex_shader_hash);
  CHECK(r.entries[2].vertex_shader_hash == 4 && r.entries[2].fields[kDepthFormat] == 0);
  for (const Entry& e : r.entries) CHECK(e.flags == 0);

  // A scaled raster uses both variants.
  target.unused_modification_bits = 0;
  r = Select(list, target);
  CHECK(r.entries.size() == 4 && r.unused_variant == 0);

  Target scene;
  scene.partition = kPartitionScene;
  r = Select(list, scene);
  CHECK(r.entries.size() == 1 && r.entries[0].vertex_shader_hash == 3);
  Target ui_target;
  ui_target.partition = kPartitionUi;
  r = Select(list, ui_target);
  CHECK(r.entries.size() == 1 && r.entries[0].vertex_shader_hash == 2);
}
}  // namespace

int main() {
  try {
    TestDescriptionLayout();
    TestRoundTrip();
    TestRefused();
    TestSelect();
  } catch (const std::exception& e) {
    std::cerr << "FAILED: " << e.what() << "\n";
    return 1;
  }
  std::cout << "pipeline prebuild list: all checks passed\n";
  return 0;
}
