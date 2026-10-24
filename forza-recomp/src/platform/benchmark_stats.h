#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

namespace forza::benchmark {
struct Statistics {
  size_t frames = 0, stalls50 = 0, stalls100 = 0;
  double seconds = 0, fps = 0, low1 = 0, p99_ms = 0, worst_ms = 0;
};
// VdSwap intervals, not averages of instantaneous FPS. 1% low is the reciprocal
// of the mean of the slowest ceil(N/100) intervals, including genuine hitches.
inline Statistics Summarize(std::vector<double> ms) {
  ms.erase(std::remove_if(ms.begin(), ms.end(),
                          [](double n) { return !std::isfinite(n) || n <= 0; }),
           ms.end());
  Statistics s;
  s.frames = ms.size();
  if (ms.empty())
    return s;
  std::sort(ms.begin(), ms.end());
  const double total = std::accumulate(ms.begin(), ms.end(), 0.0);
  s.seconds = total / 1000;
  s.fps = 1000 * ms.size() / total;
  const size_t slow = std::max<size_t>(1, (ms.size() + 99) / 100);
  const double tail = std::accumulate(ms.end() - slow, ms.end(), 0.0);
  s.low1 = 1000 * slow / tail;
  s.p99_ms = ms[(99 * ms.size() + 99) / 100 - 1];
  s.worst_ms = ms.back();
  s.stalls50 = size_t(ms.end() - std::upper_bound(ms.begin(), ms.end(), 50));
  s.stalls100 = size_t(ms.end() - std::upper_bound(ms.begin(), ms.end(), 100));
  return s;
}
} // namespace forza::benchmark
