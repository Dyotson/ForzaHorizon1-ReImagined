#pragma once

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace forza::graphics_menu {
// Native Mac apps use a flat cvar TOML file. Preserve unrelated settings and
// comments, and write defaults explicitly so selecting Off replaces an old mode.
inline std::string RewriteGraphicsConfig(
    const std::string& original,
    const std::vector<std::pair<std::string, std::string>>& updates) {
  std::istringstream input(original);
  std::string result, line;
  while (std::getline(input, line)) {
    const auto equal = line.find('=');
    auto key = line.substr(0, equal);
    const auto first = key.find_first_not_of(" \t\r");
    if (first != std::string::npos)
      key = key.substr(first, key.find_last_not_of(" \t\r") - first + 1);
    bool replaced = false;
    if (equal != std::string::npos)
      for (const auto& update : updates) replaced |= key == update.first;
    if (!replaced) result += line + "\n";
  }
  for (const auto& [name, value] : updates)
    result += name + " = " + value + "\n";
  return result;
}
}  // namespace forza::graphics_menu
