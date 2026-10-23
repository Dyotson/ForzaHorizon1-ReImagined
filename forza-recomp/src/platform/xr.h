// forza - VR through OpenXR (docs/vr.md, #333): a session on the headset next to the desktop
// window, which keeps showing the game.
//
// vr_mode = off (default): nothing here runs and no OpenXR library is loaded.
// vr_mode = screen: the game's picture on a virtual screen (an OpenXR quad layer, 3.2 x 1.8 m
//   at 2.5 m in front of the seat), redrawn at the headset's rate.
// vr_mode = cockpit: as screen, but in the in-car view the head pose turns and moves the camera
//   inside the car (hooks/vr_head_pose.cpp) and the picture becomes a projection layer drawn
//   for the pose it was rendered with (the runtime reprojects head motion between game frames).
//   Mono: both eyes see the same image. Other cameras, menus and loading stay on the screen.
//
// The OpenXR runtime creates the Vulkan instance and device and picks the GPU (SDK patch 0390);
// the newest guest output reaches this thread with the tag of the frame it shows (SDK patch
// 0391). Any failure (no loader, no runtime, no headset, a lost session) logs one line that
// starts with "VR:" and the game goes on flat.
#pragma once

#include <cstdint>

namespace rex::system {
class IGraphicsSystem;
}
namespace forza::vr {
struct HeadTransform;
}

namespace forza::xr {

// OnPreSetup, before the GPU plugin creates its Vulkan provider: loads the loader, creates the
// instance, finds the headset and registers the Vulkan creation hooks.
void Prepare();
// Once the graphics provider exists: starts the session and its thread if the Vulkan device was
// created through the runtime. Safe to call more than once.
void Start(rex::system::IGraphicsSystem* graphics);
// Ends the session and joins the thread (window close and shutdown).
void Shutdown();

// Cockpit mode with a running session: the camera hook takes its pose from the headset.
bool CockpitActive();
// Main loop thread, for each in-car view built: the head transform (recentred, with the seat
// offset) and the vertical field of view to draw with, and a new frame tag. False: no pose.
bool CockpitView(vr::HeadTransform& head, float& vertical_fov, uint32_t& tag);
// Main loop thread: the posed view matrix (64 guest bytes) that carries `tag`.
void RememberPosedView(uint32_t tag, const uint8_t matrix[64]);
// Render thread: a view matrix reached the render-side camera (CPresentationCamera at guest
// address `camera`).
void OnRenderView(uint32_t camera, const uint8_t matrix[64]);
// Render thread, at VdSwap after the SDK wrote the swap packet at guest address `packet`:
// puts the tag of the frame being swapped into it.
void TagSwap(uint8_t* base, uint32_t packet);

// Recentres the cockpit view and the screen on where the head is now.
void Recenter();
// Player 1's pad buttons, every poll: both sticks pressed for a second recentre.
void OnPad(uint16_t buttons);

}  // namespace forza::xr
