#pragma once
#include <string>
#include <string_view>

namespace forza::graphics_menu {
inline bool LiveOutputSupported(std::string_view value) {
#ifdef __APPLE__
  if (value == "720p") return true;
#endif
  return value == "1080p" || value == "1440p" || value == "4k";
}
inline std::string CurrentOutput(std::string value) {
  return value.empty() ? "720p" : value;
}
inline std::string OutputLabel(std::string_view value) {
  if (value == "720p") return "1280 x 720";
  if (value == "1080p") return "1920 x 1080";
  if (value == "1440p") return "2560 x 1440";
  if (value == "4k") return "3840 x 2160";
  return std::string(value);
}
}  // namespace forza::graphics_menu
