// forza - hooks in the Vulkan device's function table (the SDK presenter and the GPU plugin
// call Vulkan through it).
//
// Always: every swapchain creation logs the present mode it uses and the ones the surface
// offers, and at 20 and 40 fps each guest frame's host present is paced
// (frame_time::PresentDelayNs). With RECOMP_PERF (src/debug/frame_stats.h): host presents
// and pipeline creations are timed and counted.

#pragma once

namespace rex::system {
class IGraphicsSystem;
}

namespace forza {

// Installs the hooks once the graphics system has its Vulkan provider; true when they are
// in place (also when already installed). A no-op returning false for other backends.
bool InstallVulkanHooks(rex::system::IGraphicsSystem* graphics);

}  // namespace forza
