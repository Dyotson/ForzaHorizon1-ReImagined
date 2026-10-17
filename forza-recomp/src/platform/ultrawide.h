#pragma once
#include <cstdint>
#include <functional>
namespace forza::ultrawide {
using Presentation = std::function<void(bool, double, double, double, double, double)>;
void Prepare(uint32_t width, uint32_t height);
void Initialize(uint32_t width, uint32_t height, Presentation presentation);
void Resize(uint32_t width, uint32_t height);
void Frame();
double Projection(bool gameplay, bool map, uint32_t width, uint32_t height, double aspect);
}
