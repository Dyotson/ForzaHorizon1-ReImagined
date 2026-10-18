#include "../src/platform/native_renderer/velocity.h"
#include <iostream>
#include <limits>

using namespace forza::native_renderer;
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

int main() {
  VelocityParameters parameters;
  parameters.projection = {1.0002f, -1.0002f, 0, 0};
  for (uint32_t object = 0; object < 43; ++object)
    for (uint32_t row = 0; row < 4; ++row)
      parameters.previous[object * 4 + row][row] = 1;
  CHECK(Valid(parameters));
  CHECK(DecodeDepth({0, 0, 0, 240}) == 1);
  CHECK(DecodeDepth({0, 0, 0, 224}) == 0.5f);
  CHECK(DecodeDepth({0, 0, 0, 232}) == 0.75f);
  const auto still = EvaluateVelocity(parameters, {0, 0, 128, 232}, 0.25f, 0.75f, 2560, 1440);
  CHECK(std::abs(still.pixels[0]) < 0.0001f && std::abs(still.pixels[1]) < 0.0001f);
  CHECK(std::abs(still.blur[0] - 127.0f / 255) < 0.0001f);
  CHECK(still.blur[3] == 0);
  parameters.previous[0][3] = 0.05f;
  parameters.previous[1][3] = -0.025f;
  const auto camera = EvaluateVelocity(parameters, {0, 0, 255, 232}, 0.25f, 0.75f, 2560, 1440);
  CHECK(std::abs(camera.pixels[0] - 64) < 0.001f);
  CHECK(std::abs(camera.pixels[1] - 18) < 0.001f);
  const auto same = EvaluateVelocity(parameters, {0, 0, 128, 232}, 0.25f, 0.75f, 2560, 1440);
  CHECK(same.pixels == camera.pixels);
  parameters.previous[5 * 4][3] = -0.2f;
  const auto car = EvaluateVelocity(parameters, {0, 0, 15, 232}, 0.25f, 0.75f, 2560, 1440);
  CHECK(std::abs(car.pixels[0] + 256) < 0.001f);
  CHECK(car.blur[3] == 1);
  // The blur limit must never clamp the temporal motion vector.
  CHECK(car.blur[0] > 0.99f && car.pixels[0] < -200);
  parameters.previous[42 * 4][3] = 0.1f;
  const auto high_id = EvaluateVelocity(parameters, {0, 0, 127, 232}, 0.25f, 0.75f, 2560, 1440);
  CHECK(std::abs(high_id.pixels[0] - 128) < 0.001f);
  parameters.previous[3] = {};
  const auto bad_w = EvaluateVelocity(parameters, {0, 0, 128, 232}, 0.25f, 0.75f, 2560, 1440);
  CHECK(bad_w.pixels[0] == 0 && bad_w.pixels[1] == 0);
  for (uint32_t row = 0; row < 4; ++row) parameters.previous[33 * 4 + row] = {};
  const auto zero_position = EvaluateVelocity(parameters, {0, 0, 99, 232}, 0.25f, 0.75f, 2560, 1440);
  CHECK(zero_position.pixels[0] == 0 && zero_position.pixels[1] == 0);
  CHECK(std::abs(zero_position.blur[0] - (127.0f / 255 - 0.5f / std::sqrt(2.0f))) < 0.0001f);
  CHECK(zero_position.blur[2] > 0.7f);
  parameters.previous[0] = {1, 0, 0.2f, 0};
  parameters.previous[3] = {0, 0, 0, 1};
  const auto outside_depth = EvaluateVelocity(parameters, {0, 0, 128, 248}, 0.25f, 0.75f, 2560, 1440);
  CHECK(outside_depth.depth == 1);
  CHECK(std::abs(outside_depth.pixels[0] + 128) < 0.001f);
  parameters.previous[8][2] = std::numeric_limits<float>::quiet_NaN();
  CHECK(Valid(parameters));
  const auto unused_nan = EvaluateVelocity(parameters, {0, 0, 6, 240}, 0.25f, 0.75f, 2560, 1440);
  CHECK(std::isfinite(unused_nan.pixels[0]));
  CHECK(unused_nan.blur[0] != 0);
  const auto invalid_nan = EvaluateVelocity(parameters, {0, 0, 6, 232}, 0.25f, 0.75f, 2560, 1440);
  CHECK(invalid_nan.pixels[0] == 0 && invalid_nan.pixels[1] == 0);
  CHECK(invalid_nan.blur == Float4({0, 0, 0, 1}));
  parameters.previous[8][2] = 0;
  CHECK(Valid(parameters));
  parameters.uv_transform = {0.5f, 1, 0, 0};
  CHECK(!Valid(parameters));
  parameters.uv_transform = {1, 1, 0, 0};
  CHECK(Valid(parameters));
  parameters.blur[3] = 0;
  CHECK(!Valid(parameters));
  std::cout << "Native velocity reference checks passed\n";
}
