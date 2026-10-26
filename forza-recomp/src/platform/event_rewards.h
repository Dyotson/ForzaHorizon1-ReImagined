#pragma once
#include "unicorn_eligibility.h"
#include <cstddef>
#include <cstdint>

namespace forza {
enum class EventPermission { Unrestricted, Allowed, Denied, Pending, Unavailable, EnrollmentRequired };

// Time is supplied by the caller, allowing stale/reordered packet tests without
// sleeping. Access is synchronized by the network adapter, not the guest thread.
struct EventRewardCache {
  uint16_t epoch = 0;
  uint32_t peer = 0, revision = 0;
  uint64_t pending = 0, requested = 0, received = 0, activated = 0;
  uint8_t mask = 0;
  bool required = false, enrolled = false, has_reply = false;

  static uint64_t Read(const uint8_t* p, size_t count) {
    uint64_t result = 0;
    for (size_t i = 0; i < count; ++i) result = (result << 8) | p[i];
    return result;
  }
  void Session(uint16_t new_epoch, uint32_t new_peer, uint64_t now) {
    epoch = new_epoch; peer = new_peer; revision = 0;
    mask = 0; enrolled = false; has_reply = false; pending = 0;
    activated = now;  // preserve a known requirement across a restart
  }
  void Request(uint64_t nonce, uint64_t now) { pending = nonce; requested = now; }
  bool Receive(const uint8_t* body, size_t size, uint64_t now) {
    if (size == 4 && body[0] == 0) {
      if (Read(body + 1, 2) != epoch || body[3] > 1) return false;
      if (body[3] && !required) { required = true; activated = now; }
      // Only a fresh nonce-bound response may relax a known requirement.
      return true;
    }
    if (size != 22 || body[0] != 2 || !pending || Read(body + 1, 8) != pending ||
        Read(body + 13, 2) != epoch || Read(body + 15, 4) != peer ||
        body[19] > 1 || body[20] > 1 || body[21] > 127 ||
        Read(body + 9, 4) < revision || now < requested || now - requested > 5000) return false;
    revision = uint32_t(Read(body + 9, 4));
    enrolled = body[19] != 0;
    if (!required && body[20]) activated = now;
    required = body[20] != 0; mask = body[21];
    received = now; has_reply = true; pending = 0;
    return true;
  }
  EventPermission Permission(uint32_t model, uint64_t now) const {
    if (!required || !unicorn::IsUnicorn(model)) return EventPermission::Unrestricted;
    if (!has_reply) return now - activated > 12000 ? EventPermission::Unavailable : EventPermission::Pending;
    if (now < received || now - received > 12000) return EventPermission::Unavailable;
    if (!enrolled) return EventPermission::EnrollmentRequired;
    constexpr uint32_t models[]{320,326,330,1082,1197,1233,1488};
    for (unsigned i = 0; i < 7; ++i)
      if (models[i] == model) return mask & (1 << i) ? EventPermission::Allowed : EventPermission::Denied;
    return EventPermission::Unrestricted;
  }
};
}  // namespace forza
