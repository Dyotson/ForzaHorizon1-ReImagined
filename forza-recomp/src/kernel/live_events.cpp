// Community reward snapshots use Forza Live's existing UDP trust boundary.
#include "live.h"
#include <chrono>
#include <mutex>
#include <random>

namespace forza {
namespace {
std::mutex event_mutex;
EventRewardCache cache;
uint64_t token = 0, last_sent = 0;
uint64_t Now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}
void Append(std::vector<uint8_t>& out, uint64_t value) {
  for (unsigned i = 8; i > 0; --i) out.push_back(uint8_t(value >> ((i - 1) * 8)));
}
}

void LiveEventsSession(uint16_t epoch, uint32_t peer, uint64_t registration_token) {
  std::lock_guard<std::mutex> lock(event_mutex);
  cache.Session(epoch, peer, Now());
  token = registration_token; last_sent = 0;
}
void LiveEventsMessage(const uint8_t* body, size_t size) {
  std::lock_guard<std::mutex> lock(event_mutex);
  cache.Receive(body, size, Now());
}
void LiveEventsTick() {
  std::vector<uint8_t> request;
  {
    std::lock_guard<std::mutex> lock(event_mutex);
    const auto now = Now();
    if (!cache.peer || (last_sent && now - last_sent < 3000)) return;
    std::random_device random;
    uint64_t nonce = uint64_t(random()) << 32 | random();
    if (!nonce) nonce = 1;
    cache.Request(nonce, now);
    request.push_back(1); Append(request, token); Append(request, nonce);
    last_sent = now;
  }
  LiveSendEvents(request);
}
EventPermission LiveEventPermission(uint32_t model) {
  std::lock_guard<std::mutex> lock(event_mutex);
  return cache.Permission(model, Now());
}
}  // namespace forza
