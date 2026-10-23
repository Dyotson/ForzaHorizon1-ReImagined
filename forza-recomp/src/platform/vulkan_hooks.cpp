// forza - hooks in the Vulkan device's function table; see vulkan_hooks.h.
//
// The SDK loads every Vulkan entry point into VulkanDevice::Functions, and its presenter
// and the GPU plugin call through that table, so swapping a pointer there wraps a call
// without patching the SDK.

#include "vulkan_hooks.h"

#include "../debug/frame_stats.h"
#include "../hooks/frame_time.h"

#include <rex/logging.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/vulkan/provider.h>

#include <cstring>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#else
#include <dlfcn.h>
#endif

namespace forza {
namespace {

using rex::ui::vulkan::VulkanDevice;

const VulkanDevice* g_device = nullptr;
PFN_vkCreateSwapchainKHR g_create_swapchain = nullptr;
PFN_vkQueuePresentKHR g_queue_present = nullptr;
PFN_vkCreateGraphicsPipelines g_create_graphics_pipelines = nullptr;
PFN_vkCreateComputePipelines g_create_compute_pipelines = nullptr;

const char* PresentModeName(VkPresentModeKHR mode) {
  switch (mode) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR:
      return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR:
      return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR:
      return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR:
      return "FIFO_RELAXED";
    case VK_PRESENT_MODE_FIFO_LATEST_READY_KHR:
      return "FIFO_LATEST_READY";
    default:
      return nullptr;
  }
}

std::string PresentModeString(VkPresentModeKHR mode) {
  const char* name = PresentModeName(mode);
  return name ? name : std::to_string(uint32_t(mode));
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchain(VkDevice device,
                                               const VkSwapchainCreateInfoKHR* create_info,
                                               const VkAllocationCallbacks* allocator,
                                               VkSwapchainKHR* swapchain) {
  const auto& ifn = g_device->vulkan_instance()->functions();
  std::string offered;
  uint32_t count = 0;
  if (ifn.vkGetPhysicalDeviceSurfacePresentModesKHR(
          g_device->physical_device(), create_info->surface, &count, nullptr) == VK_SUCCESS) {
    std::vector<VkPresentModeKHR> modes(count);
    if (ifn.vkGetPhysicalDeviceSurfacePresentModesKHR(g_device->physical_device(),
                                                      create_info->surface, &count,
                                                      modes.data()) == VK_SUCCESS) {
      modes.resize(count);
      for (VkPresentModeKHR mode : modes) {
        offered += (offered.empty() ? "" : " ") + PresentModeString(mode);
      }
    }
  }
  REXLOG_INFO(
      "Vulkan swapchain {}x{}: present mode {} (the surface offers {}; vulkan_allow_present_mode_* "
      "settings choose, docs/perf.md)",
      create_info->imageExtent.width, create_info->imageExtent.height,
      PresentModeString(create_info->presentMode), offered.empty() ? "?" : offered);
  return g_create_swapchain(device, create_info, allocator, swapchain);
}

// Keep guest pipeline counts separate from the presenter's output and overlay pipelines.
// The host count also exposes repeated output-pipeline creation regressions (SDK patch 0045).
bool FromGpuPlugin(void* return_address) {
#ifdef _WIN32
  HMODULE module = nullptr;
  char name[MAX_PATH];
  if (!GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          static_cast<LPCSTR>(return_address), &module) ||
      !GetModuleFileNameA(module, name, sizeof(name))) {
    return true;
  }
  return std::strstr(name, "rexgpu-xenos") != nullptr;
#else
  Dl_info info;
  if (!dladdr(return_address, &info) || !info.dli_fname) return true;
  return std::strstr(info.dli_fname, "rexgpu-xenos") != nullptr;
#endif
}

// Waits at least `ns` nanoseconds, subject to scheduler latency: nanosleep on POSIX, a high-resolution
// waitable timer on Windows (10 1803 and later; without one it does not wait).
void PreciseWait(int64_t ns) {
  if (ns <= 0) return;
#ifdef _WIN32
  static thread_local HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (!timer) return;
  LARGE_INTEGER due;
  due.QuadPart = -(ns / 100);  // relative, in 100 ns units
  if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
    WaitForSingleObject(timer, INFINITE);
  }
#else
  std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
#endif
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresent(VkQueue queue, const VkPresentInfoKHR* present_info) {
  PreciseWait(frame_time::PresentDelayNs());  // 20 and 40 fps: evenly spaced frames
  perf::OnHostPresent();
  return g_queue_present(queue, present_info);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(
    VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo* create_infos, const VkAllocationCallbacks* allocator,
    VkPipeline* pipelines) {
  if (FromGpuPlugin(__builtin_return_address(0))) {
    perf::OnPipelinesCreated(count);
  } else {
    static std::atomic<uint64_t> host_pipelines{0};
    const auto previous = host_pipelines.fetch_add(count, std::memory_order_relaxed);
    const auto total = previous + count;
    if (total <= 10 || previous / 100 != total / 100) {
      REXLOG_INFO("PERF: host graphics pipelines created={}", total);
    }
  }
  return g_create_graphics_pipelines(device, cache, count, create_infos, allocator, pipelines);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(
    VkDevice device, VkPipelineCache cache, uint32_t count,
    const VkComputePipelineCreateInfo* create_infos, const VkAllocationCallbacks* allocator,
    VkPipeline* pipelines) {
  if (FromGpuPlugin(__builtin_return_address(0))) perf::OnPipelinesCreated(count);
  return g_create_compute_pipelines(device, cache, count, create_infos, allocator, pipelines);
}

}  // namespace

bool InstallVulkanHooks(rex::system::IGraphicsSystem* graphics) {
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  if (g_device) return true;
  rex::ui::GraphicsProvider* provider = graphics ? graphics->provider() : nullptr;
  // typeid instead of dynamic_cast: no dependency on the SDK exporting the type's RTTI.
  if (!provider || !std::strstr(typeid(*provider).name(), "VulkanProvider")) return false;
  VulkanDevice* device = static_cast<rex::ui::vulkan::VulkanProvider*>(provider)->vulkan_device();
  if (!device) return false;
  // The table is filled once when the device is created and only read afterwards.
  auto& dfn = const_cast<VulkanDevice::Functions&>(device->functions());
  if (!dfn.vkCreateSwapchainKHR || !dfn.vkQueuePresentKHR) return false;
  g_device = device;
  g_create_swapchain = dfn.vkCreateSwapchainKHR;
  dfn.vkCreateSwapchainKHR = CreateSwapchain;
  g_queue_present = dfn.vkQueuePresentKHR;
  dfn.vkQueuePresentKHR = QueuePresent;  // frame statistics and the present pacing
  if (perf::Enabled()) {
    g_create_graphics_pipelines = dfn.vkCreateGraphicsPipelines;
    g_create_compute_pipelines = dfn.vkCreateComputePipelines;
    dfn.vkCreateGraphicsPipelines = CreateGraphicsPipelines;
    dfn.vkCreateComputePipelines = CreateComputePipelines;
  }
  return true;
}

}  // namespace forza
