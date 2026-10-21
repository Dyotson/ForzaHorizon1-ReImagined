#include "../src/platform/macos_graphics_config.h"
#include <cassert>

int main() {
  using forza::graphics_menu::RewriteGraphicsConfig;
  const auto updated = RewriteGraphicsConfig(
      "# Keep my paths and queue\ngame_data_root = \"/Games/Forza\"\n"
      "vulkan_max_frames_in_flight = 1\n fh1_upscaler = \"metalfx_temporal\"\n"
      "resolution_scale = 2 # previous output\nfuture_option = true\n",
      {{"fh1_upscaler", "\"off\""}, {"resolution_scale", "1"},
       {"forza_performance_overlay", "\"graph\""}});
  assert(updated.find("# Keep my paths and queue\n") == 0);
  assert(updated.find("game_data_root = \"/Games/Forza\"\n") != std::string::npos);
  assert(updated.find("vulkan_max_frames_in_flight = 1\n") != std::string::npos);
  assert(updated.find("future_option = true\n") != std::string::npos);
  assert(updated.find("metalfx_temporal") == std::string::npos);
  assert(updated.find("resolution_scale = 2") == std::string::npos);
  assert(updated.find("fh1_upscaler = \"off\"\n") != std::string::npos);
  assert(updated.find("forza_performance_overlay = \"graph\"\n") != std::string::npos);
  assert(RewriteGraphicsConfig(updated, {{"fh1_upscaler", "\"off\""},
      {"resolution_scale", "1"}, {"forza_performance_overlay", "\"graph\""}}) == updated);
}
