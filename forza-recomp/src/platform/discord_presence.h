// Optional local Discord Rich Presence. No network services or profile identifiers.
#pragma once
#include <cstdint>

namespace forza::discord {
void Initialize();
void Shutdown();
// XGI's primary-user context updates, also observed while playing offline.
void Context(uint32_t id, uint32_t value);
// Session-local member counts, never the server's global online population.
void Party(uint32_t count, uint32_t capacity);
}  // namespace forza::discord
