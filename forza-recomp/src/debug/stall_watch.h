// forza - log long frames and where the main loop and render threads were during them
// (src/debug/stall_watch.cpp, #187). Always on.

#pragma once

namespace forza::stall {

// Starts the watchdog thread (once).
void Start();

// The render thread handed a frame to VdSwap (src/debug/frame_dump.cpp).
void OnGuestSwap();

// The main loop took its frame time (src/hooks/frame_time.cpp).
void OnMainFrame();

}  // namespace forza::stall
