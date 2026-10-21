#pragma once
#include <cstdint>

namespace forza::telemetry {
// Raw, post-overlay player-one inputs. These are not remapped game actions.
void Input(bool connected, uint8_t left, uint8_t right, uint16_t buttons, int16_t steer);
void Physics(uint8_t* base);
}
