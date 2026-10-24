#include "../src/platform/benchmark_stats.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
using forza::benchmark::Summarize;
int main() {
  auto empty = Summarize({});
  assert(empty.frames == 0 && empty.fps == 0);
  auto sixty = Summarize(std::vector<double>(600, 1000.0 / 60));
  assert(std::abs(sixty.fps - 60) < 1e-8 && std::abs(sixty.low1 - 60) < 1e-8);
  // One real 200 ms stall must stay in the result and dominate the 1% low.
  std::vector<double> samples(99, 10);
  samples.push_back(200);
  auto hitch = Summarize(samples);
  assert(hitch.frames == 100 && hitch.low1 == 5 && hitch.p99_ms == 10);
  assert(hitch.worst_ms == 200 && hitch.stalls50 == 1 && hitch.stalls100 == 1);
  assert(std::abs(hitch.fps - 100000.0 / 1190) < 1e-8);
  // FPS weights time: averaging 100 FPS and 10 FPS would incorrectly give 55.
  auto mixed = Summarize({10, 100});
  assert(std::abs(mixed.fps - 2000.0 / 110) < 1e-8);
  auto invalid = Summarize({0, -1, std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity(), 20});
  assert(invalid.frames == 1 && invalid.fps == 50);
  auto boundary = Summarize({50, 100, 101});
  assert(boundary.stalls50 == 2 && boundary.stalls100 == 1);
  auto round = Summarize(std::vector<double>(101, 20));
  assert(round.low1 == 50);
  std::cout << "Benchmark statistics passed\n";
}
