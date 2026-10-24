#pragma once

namespace forza::wheel {
// Tells the SDK's racing wheel driver whether the player is driving (#256, #304), so the game's wheel
// forces fade out in the pause menu, rewinds, replays and menus. Called on every pad poll.
void UpdateDriving();
}
