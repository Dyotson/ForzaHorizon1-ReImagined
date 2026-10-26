// forza - VR through OpenXR; see xr.h and docs/vr.md (#333).
//
// Threads: Prepare and the Vulkan creation hooks run where the SDK sets up graphics; the
// session, its frame loop and the copies into the headset's swapchain run on the "Forza OpenXR"
// thread; the camera hooks call CockpitView/RememberPosedView on the main loop thread and
// OnRenderView/TagSwap on the render thread. The OpenXR thread shares the device's one graphics
// queue with the GPU plugin and the presenter: it takes the device's queue lock around every
// call the XR_KHR_vulkan_enable2 spec lists as using it (xrBeginFrame, xrEndFrame,
// xrAcquireSwapchainImage, xrReleaseSwapchainImage) and around its own submissions.

#include "xr.h"

#include "guest_memory.h"
#include "sdk_import.h"
#include "vr_head_pose_math.h"
#include "xr_frame_tags.h"
#include "xr_math.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/vulkan/provider.h>

#ifdef _WIN32
#include <windows.h>
#define XR_USE_PLATFORM_WIN32
#else
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#define XR_USE_TIMESPEC
#endif
#define XR_NO_PROTOTYPES
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

REXCVAR_DEFINE_STRING(vr_mode, "off", "VR",
                      "VR headset through OpenXR: off, screen (the game on a virtual screen) or "
                      "cockpit (head-tracked in-car view, mono); needs an OpenXR runtime")
    .allowed({"off", "screen", "cockpit"})
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(vr_seat_height_cm, 0.0, "VR",
                      "Cockpit mode: raises (positive) or lowers the eye from the driver's seat, cm")
    .range(-50, 50);
REXCVAR_DEFINE_STRING(vr_recenter_key, "Home", "VR",
                      "Key that recentres the VR view on the head (both sticks held for a second "
                      "do the same)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(vr_head_back_limit_cm, 8.0, "VR",
                      "Cockpit: how far the eye may move back from where it was recentred, cm; "
                      "turning round moves the head back, and further back the eye would enter "
                      "the seat (100: no limit)")
    .range(0, 100);
REXCVAR_DEFINE_DOUBLE(vr_fov_margin, 5.0, "VR",
                      "Cockpit mode: degrees drawn beyond the headset's field of view on each side, "
                      "so reprojection between game frames does not show the image's edge")
    .range(0, 30)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(vr_runtime_timeout, 20, "VR",
                     "Seconds to wait for the OpenXR runtime at start before playing flat")
    .range(1, 300)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// 2.2 m at 2.5 m: 47 degrees wide, 27 tall. The corners are 24 degrees from the middle, within
// an eye turn without turning the head, like a cinema screen from its recommended seats (THX:
// 36-40 degrees). Version 1's 3.2 m filled 65 degrees: menus and the HUD's corners needed head
// turns.
REXCVAR_DEFINE_DOUBLE(vr_screen_width, 2.2, "VR", "Virtual screen width, metres").range(0.5, 20);
REXCVAR_DEFINE_DOUBLE(vr_screen_distance, 2.5, "VR", "Virtual screen distance, metres")
    .range(0.5, 20);
REXCVAR_DEFINE_BOOL(vr_trace, false, "VR",
                    "Log the head pose, the game camera, the field of view drawn and the latency "
                    "about 10 times a second (lines \"VR trace:\")")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace forza::xr {
namespace {

using rex::ui::vulkan::VulkanDevice;

// SDK patch 0390 (src/ui/vulkan/vulkan_creation_hooks.h in the SDK) and 0391
// (src/ui/vulkan/vulkan_presenter.cpp): no public header, the same layouts here.
extern "C" {
struct RexVulkanCreationHooks {
  uint32_t size;
  void* user;
  VkResult (*create_instance)(void* user, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                              const VkInstanceCreateInfo* create_info, VkInstance* instance);
  VkPhysicalDevice (*physical_device)(void* user, VkInstance instance);
  VkResult (*create_device)(void* user, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                            VkPhysicalDevice physical_device, const VkDeviceCreateInfo* create_info,
                            VkDevice* device);
};
struct RexGuestOutputFrame {
  uint32_t size;
  uint32_t handle;
  uint64_t serial;
  uint32_t tag;
  uint32_t width;
  uint32_t height;
  VkImage image;
  VkImageView view;
};
}
using SetVulkanCreationHooksFn = void (*)(const RexVulkanCreationHooks*);
using SetPadsWithoutFocusFn = void (*)(int);  // patch 0392 (src/input/input_system.cpp)
using SetGuestOutputExportFn = void (*)(int);
using AcquireGuestOutputFrameFn = int (*)(RexGuestOutputFrame*, uint64_t);
using ReleaseGuestOutputFrameFn = void (*)(uint32_t);

#ifdef _WIN32
constexpr const char* kLoaderName = "openxr_loader.dll";
#else
constexpr const char* kLoaderName = "libopenxr_loader.so.1";
#endif

enum class Mode { kOff, kScreen, kCockpit };

struct Api {
  PFN_xrGetInstanceProcAddr GetInstanceProcAddr = nullptr;
  PFN_xrEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties = nullptr;
  PFN_xrCreateInstance CreateInstance = nullptr;
  PFN_xrDestroyInstance DestroyInstance = nullptr;
  PFN_xrResultToString ResultToString = nullptr;
  PFN_xrGetInstanceProperties GetInstanceProperties = nullptr;
  PFN_xrGetSystem GetSystem = nullptr;
  PFN_xrGetSystemProperties GetSystemProperties = nullptr;
  PFN_xrPollEvent PollEvent = nullptr;
  PFN_xrCreateSession CreateSession = nullptr;
  PFN_xrDestroySession DestroySession = nullptr;
  PFN_xrBeginSession BeginSession = nullptr;
  PFN_xrEndSession EndSession = nullptr;
  PFN_xrRequestExitSession RequestExitSession = nullptr;
  PFN_xrCreateReferenceSpace CreateReferenceSpace = nullptr;
  PFN_xrDestroySpace DestroySpace = nullptr;
  PFN_xrLocateSpace LocateSpace = nullptr;
  PFN_xrLocateViews LocateViews = nullptr;
  PFN_xrEnumerateViewConfigurationViews EnumerateViewConfigurationViews = nullptr;
  PFN_xrEnumerateEnvironmentBlendModes EnumerateEnvironmentBlendModes = nullptr;
  PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats = nullptr;
  PFN_xrCreateSwapchain CreateSwapchain = nullptr;
  PFN_xrDestroySwapchain DestroySwapchain = nullptr;
  PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
  PFN_xrAcquireSwapchainImage AcquireSwapchainImage = nullptr;
  PFN_xrWaitSwapchainImage WaitSwapchainImage = nullptr;
  PFN_xrReleaseSwapchainImage ReleaseSwapchainImage = nullptr;
  PFN_xrWaitFrame WaitFrame = nullptr;
  PFN_xrBeginFrame BeginFrame = nullptr;
  PFN_xrEndFrame EndFrame = nullptr;
  PFN_xrGetVulkanGraphicsRequirements2KHR GetVulkanGraphicsRequirements2KHR = nullptr;
  PFN_xrCreateVulkanInstanceKHR CreateVulkanInstanceKHR = nullptr;
  PFN_xrCreateVulkanDeviceKHR CreateVulkanDeviceKHR = nullptr;
  PFN_xrGetVulkanGraphicsDevice2KHR GetVulkanGraphicsDevice2KHR = nullptr;
#ifdef _WIN32
  PFN_xrConvertWin32PerformanceCounterToTimeKHR ConvertTime = nullptr;
#else
  PFN_xrConvertTimespecTimeToTimeKHR ConvertTime = nullptr;
#endif
};

// Device functions the SDK's table may not have (vkCmdBlitImage, vkCmdCopyImage), all loaded here.
struct Vk {
  PFN_vkCreateCommandPool CreateCommandPool = nullptr;
  PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
  PFN_vkResetCommandPool ResetCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
  PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  PFN_vkCmdBlitImage CmdBlitImage = nullptr;
  PFN_vkCmdCopyImage CmdCopyImage = nullptr;
  PFN_vkQueueSubmit QueueSubmit = nullptr;
  PFN_vkCreateFence CreateFence = nullptr;
  PFN_vkDestroyFence DestroyFence = nullptr;
  PFN_vkWaitForFences WaitForFences = nullptr;
  PFN_vkResetFences ResetFences = nullptr;
  PFN_vkGetFenceStatus GetFenceStatus = nullptr;
  PFN_vkCreateImage CreateImage = nullptr;
  PFN_vkDestroyImage DestroyImage = nullptr;
  PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
  PFN_vkAllocateMemory AllocateMemory = nullptr;
  PFN_vkFreeMemory FreeMemory = nullptr;
  PFN_vkBindImageMemory BindImageMemory = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
  PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
};

// One copy of a guest output into the headset's swapchain, in flight until its fence signals.
struct CopySlot {
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer buffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  uint32_t hold = 0;  // the guest output frame held until the copy is done
  bool busy = false;
};

struct State {
  Mode mode = Mode::kOff;
#ifdef _WIN32
  HMODULE loader = nullptr;
#else
  void* loader = nullptr;
#endif
  Api xr;
  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  std::string runtime_name;
  bool prepared = false;
  bool started = false;

  // Vulkan created through the runtime (the creation hooks).
  VkInstance vk_instance = VK_NULL_HANDLE;
  VkPhysicalDevice vk_physical = VK_NULL_HANDLE;
  VkDevice vk_device = VK_NULL_HANDLE;
  bool vk_failed = false;
  PFN_vkGetInstanceProcAddr vk_get_instance_proc_addr = nullptr;

  SetGuestOutputExportFn set_export = nullptr;
  AcquireGuestOutputFrameFn acquire_frame = nullptr;
  ReleaseGuestOutputFrameFn release_frame = nullptr;
  SetPadsWithoutFocusFn set_pads_without_focus = nullptr;

  // The session (OpenXR thread; the spaces are also read by CockpitView under pose_mutex).
  VulkanDevice* device = nullptr;
  Vk vk;
  std::thread thread;
  std::atomic<bool> stop{false};
  std::atomic<bool> finished{false};  // the thread has left Run
  XrSession session = XR_NULL_HANDLE;
  XrSpace local = XR_NULL_HANDLE;
  XrSpace view = XR_NULL_HANDLE;
  XrSessionState state = XR_SESSION_STATE_UNKNOWN;
  bool running = false;  // between xrBeginSession and xrEndSession
  int frame_failures = 0;  // xrWaitFrame/xrBeginFrame/xrEndFrame failures in a row
  XrEnvironmentBlendMode blend = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  int64_t swapchain_format = 0;
  VkFormat staging_format = VK_FORMAT_UNDEFINED;  // the 8-bit UNORM twin of the sRGB format

  // The swapchain for the game's picture, sized as the guest output.
  XrSwapchain swapchain = XR_NULL_HANDLE;
  std::vector<VkImage> swapchain_images;
  bool image_acquired = false;  // a timed-out wait must resume this same acquisition
  uint32_t acquired_index = 0;
  uint32_t width = 0, height = 0;
  uint32_t failed_width = 0, failed_height = 0;
  VkImage staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_memory = VK_NULL_HANDLE;
  CopySlot slots[3];
  uint32_t next_slot = 0;
  bool have_image = false;  // a copy was released into the swapchain
  uint64_t last_serial = 0;
  FrameTag shown_tag;  // the tag of the picture in the swapchain (id 0: untagged)
  XrExtent2Di shown_extent{};  // the picture's part of the swapchain image (smaller for the screen)
  bool linear_blit = false;  // the guest output's format can be filtered: the screen is downscaled

  // Cockpit state shared with the camera hooks.
  std::atomic<bool> cockpit_active{false};
  std::atomic<bool> recenter{true};
  std::mutex pose_mutex;  // the spaces while CockpitView locates, the origin, the frustum
  Pose origin;
  bool origin_set = false;
  CockpitFrustum frustum;  // what the game draws the cockpit with (vertical 0: not known yet)
  std::atomic<int64_t> wait_display_time{0}, wait_period{0}, wait_steady_ns{0};
  // Camera built to picture shown: the head pose is predicted that far ahead. A streamed headset
  // (WiVRn) shows a picture 70-95 ms after the game built its camera, so the cap is 100 ms.
  LatencyEstimate latency{40'000'000, 5'000'000, 100'000'000};  // OpenXR thread only
  std::atomic<int64_t> latency_ns{40'000'000};
  // The last cockpit frame's latency in two parts: camera built to the picture reaching the OpenXR
  // thread (the game's own pipeline), and from there to its display time (the runtime's).
  std::atomic<int64_t> last_game_ns{0}, last_runtime_ns{0};
  int64_t stat_game_ns = 0, stat_runtime_ns = 0;  // sums over the stats window
  uint32_t stat_latency_samples = 0;

  // vr_trace: what the camera hook saw last (main loop thread), logged by the OpenXR thread.
  std::atomic<bool> trace{false};
  std::atomic<uint32_t> trace_camera{0};
  std::atomic<const char*> trace_camera_name{nullptr};
  std::atomic<float> trace_look{0};
  std::atomic<bool> trace_posed{false};
  std::atomic<int64_t> trace_view_ns{0};  // steady clock of the last view built
  int64_t trace_next_ns = 0;

  std::mutex tags_mutex;
  TagRing<64> tags;
  ViewMatcher<8> views;
  SwapTagger swap_tagger;
  std::atomic<uint32_t> next_tag{1};

  // Statistics for the periodic log line.
  std::atomic<uint32_t> stat_swaps{0}, stat_tagged_swaps{0}, stat_extra_views{0}, stat_repeat_views{0};
  uint32_t stat_frames = 0, stat_projection = 0, stat_quad = 0, stat_copies = 0;
  int64_t stat_end_frame_max_ns = 0;
  std::chrono::steady_clock::time_point stat_since;
};

State g;

int64_t SteadyNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string ResultName(XrResult r) {
  if (g.xr.ResultToString && g.instance != XR_NULL_HANDLE) {
    char text[XR_MAX_RESULT_STRING_SIZE] = {};
    if (XR_SUCCEEDED(g.xr.ResultToString(g.instance, r, text))) return text;
  }
  switch (r) {
    case XR_ERROR_RUNTIME_UNAVAILABLE:
      return "XR_ERROR_RUNTIME_UNAVAILABLE";
    case XR_ERROR_RUNTIME_FAILURE:
      return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_FORM_FACTOR_UNAVAILABLE:
      return "XR_ERROR_FORM_FACTOR_UNAVAILABLE";
    case XR_ERROR_EXTENSION_NOT_PRESENT:
      return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case XR_ERROR_API_VERSION_UNSUPPORTED:
      return "XR_ERROR_API_VERSION_UNSUPPORTED";
    case XR_ERROR_INSTANCE_LOST:
      return "XR_ERROR_INSTANCE_LOST";
    default:
      return "XrResult " + std::to_string(int(r));
  }
}

void* LoaderSymbol(const char* name) {
#ifdef _WIN32
  return g.loader ? reinterpret_cast<void*>(GetProcAddress(g.loader, name)) : nullptr;
#else
  return g.loader ? dlsym(g.loader, name) : nullptr;
#endif
}

bool LoadLoader() {
#ifdef _WIN32
  g.loader = LoadLibraryA(kLoaderName);
#else
  g.loader = dlopen(kLoaderName, RTLD_NOW | RTLD_LOCAL);
#endif
  if (!g.loader) return false;
  g.xr.GetInstanceProcAddr =
      reinterpret_cast<PFN_xrGetInstanceProcAddr>(LoaderSymbol("xrGetInstanceProcAddr"));
  return g.xr.GetInstanceProcAddr != nullptr;
}

template <typename T>
bool Load(XrInstance instance, const char* name, T& out) {
  PFN_xrVoidFunction f = nullptr;
  if (XR_FAILED(g.xr.GetInstanceProcAddr(instance, name, &f)) || !f) return false;
  out = reinterpret_cast<T>(f);
  return true;
}

// Logs why VR is off and ends the instance Prepare made (only before the Vulkan hooks are
// registered). The loader stays loaded: a runtime may leave threads in its library.
void FlatBeforeHooks(const std::string& why) {
  REXLOG_WARN("VR: {} - playing flat", why);
  if (g.instance != XR_NULL_HANDLE && g.xr.DestroyInstance) g.xr.DestroyInstance(g.instance);
  g.instance = XR_NULL_HANDLE;
  g.mode = Mode::kOff;
}

// --- Vulkan creation through the runtime (SDK patch 0390) ------------------------------------

VkResult HookCreateInstance(void*, PFN_vkGetInstanceProcAddr gipa, const VkInstanceCreateInfo* info,
                            VkInstance* out) {
  g.vk_get_instance_proc_addr = gipa;
  if (!g.vk_failed) {
    XrVulkanInstanceCreateInfoKHR create{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    create.systemId = g.system;
    create.pfnGetInstanceProcAddr = gipa;
    create.vulkanCreateInfo = info;
    VkResult vk = VK_SUCCESS;
    const XrResult r = g.xr.CreateVulkanInstanceKHR(g.instance, &create, out, &vk);
    if (XR_SUCCEEDED(r)) {
      if (vk == VK_SUCCESS) g.vk_instance = *out;
      return vk;  // a Vulkan error (a missing layer): the SDK retries through here
    }
    REXLOG_WARN("VR: xrCreateVulkanInstanceKHR failed ({}) - playing flat", ResultName(r));
    g.vk_failed = true;
  }
  auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
  const VkResult vk = create ? create(info, nullptr, out) : VK_ERROR_INITIALIZATION_FAILED;
  if (vk == VK_SUCCESS) g.vk_instance = *out;
  return vk;
}

VkPhysicalDevice HookPhysicalDevice(void*, VkInstance instance) {
  if (g.vk_failed || instance != g.vk_instance) return VK_NULL_HANDLE;
  XrVulkanGraphicsDeviceGetInfoKHR info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
  info.systemId = g.system;
  info.vulkanInstance = instance;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  const XrResult r = g.xr.GetVulkanGraphicsDevice2KHR(g.instance, &info, &physical);
  if (XR_FAILED(r) || physical == VK_NULL_HANDLE) {
    REXLOG_WARN("VR: xrGetVulkanGraphicsDevice2KHR failed ({}) - playing flat", ResultName(r));
    g.vk_failed = true;
    return VK_NULL_HANDLE;
  }
  g.vk_physical = physical;
  return physical;
}

VkResult HookCreateDevice(void*, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice physical,
                          const VkDeviceCreateInfo* info, VkDevice* out) {
  if (!g.vk_failed && physical == g.vk_physical && physical != VK_NULL_HANDLE) {
    XrVulkanDeviceCreateInfoKHR create{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    create.systemId = g.system;
    create.pfnGetInstanceProcAddr = gipa;
    create.vulkanPhysicalDevice = physical;
    create.vulkanCreateInfo = info;
    VkResult vk = VK_SUCCESS;
    const XrResult r = g.xr.CreateVulkanDeviceKHR(g.instance, &create, out, &vk);
    if (XR_SUCCEEDED(r) && vk == VK_SUCCESS) {
      g.vk_device = *out;
      return VK_SUCCESS;
    }
    REXLOG_WARN("VR: xrCreateVulkanDeviceKHR failed ({}, VkResult {}) - playing flat",
                ResultName(r), int(vk));
    g.vk_failed = true;
    if (XR_SUCCEEDED(r)) return vk;
  }
  auto create =
      reinterpret_cast<PFN_vkCreateDevice>(gipa(g.vk_instance, "vkCreateDevice"));
  return create ? create(physical, info, nullptr, out) : VK_ERROR_INITIALIZATION_FAILED;
}

// --- The session ------------------------------------------------------------------------------

XrTime NowXrTime() {
  if (g.xr.ConvertTime) {
    XrTime t = 0;
#ifdef _WIN32
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    if (XR_SUCCEEDED(g.xr.ConvertTime(g.instance, &counter, &t))) return t;
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (XR_SUCCEEDED(g.xr.ConvertTime(g.instance, &ts, &t))) return t;
#endif
  }
  // Without the conversion extension: the last xrWaitFrame's display time is about two frames
  // after it returned.
  return g.wait_display_time.load() - 2 * g.wait_period.load() + (SteadyNs() - g.wait_steady_ns.load());
}

Pose ToPose(const XrPosef& p) {
  return {{p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w},
          {p.position.x, p.position.y, p.position.z}};
}
XrPosef ToXr(const Pose& p) {
  XrPosef out;
  out.orientation = {p.q.x, p.q.y, p.q.z, p.q.w};
  out.position = {p.p.x, p.p.y, p.p.z};
  return out;
}

// The head (VIEW space) in LOCAL at `time`; false without a tracked orientation.
bool LocateHead(XrTime time, Pose& out) {
  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(g.xr.LocateSpace(g.view, g.local, time, &location))) return false;
  if (!(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;
  out = ToPose(location.pose);
  if (!(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) out.p = {};
  return true;
}

bool LoadDeviceFunctions() {
  const auto& ifn = g.device->vulkan_instance()->functions();
  const VkDevice device = g.device->device();
  const VkInstance instance = g.device->vulkan_instance()->instance();
  bool ok = true;
#define FORZA_VK_DEVICE(name)                                                         \
  ok &= (g.vk.name = reinterpret_cast<PFN_vk##name>(ifn.vkGetDeviceProcAddr(device,   \
                                                                             "vk" #name))) != nullptr;
#define FORZA_VK_INSTANCE(name)                                                        \
  ok &= (g.vk.name = reinterpret_cast<PFN_vk##name>(ifn.vkGetInstanceProcAddr(instance, \
                                                                               "vk" #name))) != nullptr;
  FORZA_VK_DEVICE(CreateCommandPool)
  FORZA_VK_DEVICE(DestroyCommandPool)
  FORZA_VK_DEVICE(ResetCommandPool)
  FORZA_VK_DEVICE(AllocateCommandBuffers)
  FORZA_VK_DEVICE(BeginCommandBuffer)
  FORZA_VK_DEVICE(EndCommandBuffer)
  FORZA_VK_DEVICE(CmdPipelineBarrier)
  FORZA_VK_DEVICE(CmdBlitImage)
  FORZA_VK_DEVICE(CmdCopyImage)
  FORZA_VK_DEVICE(QueueSubmit)
  FORZA_VK_DEVICE(CreateFence)
  FORZA_VK_DEVICE(DestroyFence)
  FORZA_VK_DEVICE(WaitForFences)
  FORZA_VK_DEVICE(ResetFences)
  FORZA_VK_DEVICE(GetFenceStatus)
  FORZA_VK_DEVICE(CreateImage)
  FORZA_VK_DEVICE(DestroyImage)
  FORZA_VK_DEVICE(GetImageMemoryRequirements)
  FORZA_VK_DEVICE(AllocateMemory)
  FORZA_VK_DEVICE(FreeMemory)
  FORZA_VK_DEVICE(BindImageMemory)
  FORZA_VK_INSTANCE(GetPhysicalDeviceMemoryProperties)
  FORZA_VK_INSTANCE(GetPhysicalDeviceFormatProperties)
#undef FORZA_VK_DEVICE
#undef FORZA_VK_INSTANCE
  return ok;
}

bool CreateCopySlots() {
  const VkDevice device = g.device->device();
  for (CopySlot& slot : g.slots) {
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = g.device->queue_family_graphics_compute();
    if (g.vk.CreateCommandPool(device, &pool_info, nullptr, &slot.pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = slot.pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (g.vk.AllocateCommandBuffers(device, &alloc, &slot.buffer) != VK_SUCCESS) return false;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (g.vk.CreateFence(device, &fence_info, nullptr, &slot.fence) != VK_SUCCESS) return false;
  }
  return true;
}

// Waits for a slot's copy (up to `timeout_ns`) and drops its hold on the guest output.
bool FinishSlot(CopySlot& slot, uint64_t timeout_ns) {
  if (!slot.busy) return true;
  const VkDevice device = g.device->device();
  const VkResult waited = g.vk.WaitForFences(device, 1, &slot.fence, VK_TRUE, timeout_ns);
  if (waited != VK_SUCCESS && waited != VK_ERROR_DEVICE_LOST) return false;
  // Vulkan defines device loss as completion for resource lifetime purposes.
  // It permits cleanup, but never another copy on this device.
  if (waited == VK_ERROR_DEVICE_LOST) g.stop = true;
  else if (g.vk.ResetFences(device, 1, &slot.fence) != VK_SUCCESS) g.stop = true;
  if (slot.hold) g.release_frame(slot.hold);
  slot.hold = 0;
  slot.busy = false;
  return true;
}
bool FinishAllSlots() {
  bool finished = true;
  for (CopySlot& slot : g.slots) finished &= FinishSlot(slot, 100'000'000);
  return finished;
}
// Copies whose fence has signalled.
void ReapSlots() {
  for (CopySlot& slot : g.slots) {
    if (slot.busy && g.vk.GetFenceStatus(g.device->device(), slot.fence) == VK_SUCCESS) {
      FinishSlot(slot, 0);
    }
  }
}

bool DestroySwapchain() {
  // A timeout is not completion. Retain the swapchain, staging image, pools,
  // fences and guest-image holds until every submitted copy has finished.
  if (!FinishAllSlots()) return false;
  const VkDevice device = g.device->device();
  if (g.swapchain != XR_NULL_HANDLE) g.xr.DestroySwapchain(g.swapchain);
  g.swapchain = XR_NULL_HANDLE;
  g.swapchain_images.clear();
  g.image_acquired = false;
  if (g.staging != VK_NULL_HANDLE) g.vk.DestroyImage(device, g.staging, nullptr);
  if (g.staging_memory != VK_NULL_HANDLE) g.vk.FreeMemory(device, g.staging_memory, nullptr);
  g.staging = VK_NULL_HANDLE;
  g.staging_memory = VK_NULL_HANDLE;
  g.width = g.height = 0;
  g.have_image = false;
  return true;
}

// The cockpit view's size and its pixel density in the middle of the picture (the guest output's
// size: 16:9 at the render scale), once both are known.
void LogCockpitView() {
  if (g.mode != Mode::kCockpit || !g.width || !g.height) return;
  CockpitFrustum f;
  {
    std::lock_guard lock(g.pose_mutex);
    f = g.frustum;
  }
  if (f.vertical <= 0) return;
  constexpr float kToDeg = 57.29578f;
  REXLOG_INFO("VR: the cockpit view is drawn {:.0f} deg wide x {:.0f} tall around an axis {:.1f} deg "
              "{} and {:.1f} {} of the head; at {}x{} that is {:.1f} x {:.1f} pixels per degree in the "
              "middle",
              2 * f.HalfHorizontal() * kToDeg, f.vertical * kToDeg, std::fabs(f.pitch) * kToDeg,
              f.pitch < 0 ? "down" : "up", std::fabs(f.yaw) * kToDeg, f.yaw < 0 ? "right" : "left",
              g.width, g.height, CentrePixelsPerDegree(float(g.width), f.HalfHorizontal()),
              CentrePixelsPerDegree(float(g.height), f.vertical / 2));
}

bool CreateSwapchain(uint32_t width, uint32_t height) {
  if (!DestroySwapchain()) {
    g.stop = true;  // stop copying; session cleanup will retry the pending fences
    return false;
  }
  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  info.format = g.swapchain_format;
  info.sampleCount = 1;
  info.width = width;
  info.height = height;
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;
  XrResult r;
  {
    const auto queue = g.device->AcquireQueue(g.device->queue_family_graphics_compute(), 0);
    r = g.xr.CreateSwapchain(g.session, &info, &g.swapchain);
  }
  if (XR_FAILED(r)) {
    REXLOG_WARN("VR: xrCreateSwapchain {}x{} failed ({})", width, height, ResultName(r));
    g.swapchain = XR_NULL_HANDLE;
    return false;
  }
  uint32_t count = 0;
  g.xr.EnumerateSwapchainImages(g.swapchain, 0, &count, nullptr);
  std::vector<XrSwapchainImageVulkan2KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
  if (XR_FAILED(g.xr.EnumerateSwapchainImages(
          g.swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())))) {
    return false;
  }
  for (const auto& image : images) g.swapchain_images.push_back(image.image);

  // The staging image: the guest output blitted to 8 bits, then copied bit for bit into the sRGB
  // swapchain image (the guest output already holds gamma-encoded values).
  const VkDevice device = g.device->device();
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = g.staging_format;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (g.vk.CreateImage(device, &image_info, nullptr, &g.staging) != VK_SUCCESS) return false;
  VkMemoryRequirements requirements;
  g.vk.GetImageMemoryRequirements(device, g.staging, &requirements);
  VkPhysicalDeviceMemoryProperties memory;
  g.vk.GetPhysicalDeviceMemoryProperties(g.device->physical_device(), &memory);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; ++i) {
    if ((requirements.memoryTypeBits & (1u << i)) &&
        (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      type = i;
    }
  }
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = requirements.size;
  alloc.memoryTypeIndex = type;
  if (type == UINT32_MAX || g.vk.AllocateMemory(device, &alloc, nullptr, &g.staging_memory) != VK_SUCCESS ||
      g.vk.BindImageMemory(device, g.staging, g.staging_memory, 0) != VK_SUCCESS) {
    REXLOG_WARN("VR: no memory for the {}x{} staging image", width, height);
    return false;
  }
  g.width = width;
  g.height = height;
  REXLOG_INFO("VR: swapchain {}x{}, format {}, {} images", width, height, g.swapchain_format,
              g.swapchain_images.size());
  LogCockpitView();
  return true;
}

VkImageMemoryBarrier Barrier(VkImage image, VkAccessFlags src, VkAccessFlags dst, VkImageLayout from,
                             VkImageLayout to) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = src;
  b.dstAccessMask = dst;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return b;
}

// Copies a guest output into the next swapchain image and releases it to the runtime. The frame's
// hold is dropped when the copy's fence signals. For the screen a picture drawn at render scale 2
// or more is halved (or more) on the way with a linear filter: the screen covers about 1,000 of
// the headset's pixels across, and the runtime samples its layers without mipmaps, so a full
// 2560-pixel picture would shimmer.
bool CopyFrame(const RexGuestOutputFrame& frame, bool for_screen) {
  if (g.stop) return false;
  if (frame.width != g.width || frame.height != g.height) {
    if (frame.width == g.failed_width && frame.height == g.failed_height) return false;
    if (!CreateSwapchain(frame.width, frame.height)) {
      g.failed_width = frame.width;  // tried again only for another size
      g.failed_height = frame.height;
      return false;
    }
  }
  CopySlot& slot = g.slots[g.next_slot];
  if (!FinishSlot(slot, 100'000'000)) return false;
  if (g.stop) return false;

  XrResult r;
  if (!g.image_acquired) {
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    const auto queue = g.device->AcquireQueue(g.device->queue_family_graphics_compute(), 0);
    r = g.xr.AcquireSwapchainImage(g.swapchain, &acquire, &g.acquired_index);
    if (XR_FAILED(r)) return false;
    g.image_acquired = true;
  }
  XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait.timeout = 100'000'000;
  r = g.xr.WaitSwapchainImage(g.swapchain, &wait);
  if (r != XR_SUCCESS) {
    // XR_TIMEOUT_EXPIRED is positive, but the runtime still owns the image.
    // Retry the same acquisition next frame; never copy or release it yet.
    // On session loss/error, retire it by destroying the session instead.
    if (r != XR_TIMEOUT_EXPIRED) g.stop = true;
    return false;
  }
  if (g.stop) return false;

  const VkDevice device = g.device->device();
  g.vk.ResetCommandPool(device, slot.pool, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  g.vk.BeginCommandBuffer(slot.buffer, &begin);
  const VkImage target = g.swapchain_images[g.acquired_index];
  const uint32_t divisor = for_screen && g.linear_blit ? std::max(1u, frame.width / 1280) : 1;
  const uint32_t out_width = std::max(1u, g.width / divisor), out_height = std::max(1u, g.height / divisor);
  // As the presenter's own guest output capture: the guest output leaves and returns to
  // SHADER_READ_ONLY in the fragment shader stage, which later refreshes wait for.
  VkImageMemoryBarrier to_copy[2] = {
      Barrier(frame.image, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
      Barrier(g.staging, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)};
  g.vk.CmdPipelineBarrier(slot.buffer,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, to_copy);
  VkImageBlit blit{};
  blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.srcOffsets[1] = {int32_t(frame.width), int32_t(frame.height), 1};
  blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  blit.dstOffsets[1] = {int32_t(out_width), int32_t(out_height), 1};
  g.vk.CmdBlitImage(slot.buffer, frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.staging,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                    divisor > 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
  VkImageMemoryBarrier after_blit[3] = {
      Barrier(frame.image, 0, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
      Barrier(g.staging, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
      Barrier(target, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)};
  g.vk.CmdPipelineBarrier(slot.buffer,
                          VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                          nullptr, 0, nullptr, 3, after_blit);
  VkImageCopy copy{};
  copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.extent = {out_width, out_height, 1};
  g.vk.CmdCopyImage(slot.buffer, g.staging, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  // Swapchain images go back to the runtime in COLOR_ATTACHMENT_OPTIMAL (XR_KHR_vulkan_enable2).
  VkImageMemoryBarrier release_barrier =
      Barrier(target, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                  VK_ACCESS_SHADER_READ_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  g.vk.CmdPipelineBarrier(slot.buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1,
                          &release_barrier);
  g.vk.EndCommandBuffer(slot.buffer);

  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &slot.buffer;
  VkResult submitted;
  {
    const auto queue = g.device->AcquireQueue(g.device->queue_family_graphics_compute(), 0);
    submitted = g.vk.QueueSubmit(queue.queue(), 1, &submit, slot.fence);
    if (submitted == VK_SUCCESS) {
      slot.hold = frame.handle;
      slot.busy = true;
      g.next_slot = (g.next_slot + 1) % 3;
    }
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    r = g.xr.ReleaseSwapchainImage(g.swapchain, &release);
    if (XR_SUCCEEDED(r)) g.image_acquired = false;
  }
  if (submitted != VK_SUCCESS) {
    REXLOG_WARN("VR: vkQueueSubmit of the copy failed ({})", int(submitted));
    g.stop = true;
    return false;
  }
  if (XR_FAILED(r)) {
    // Submission already owns the guest frame: keep its hold until the fence
    // completes even though the image could not be handed to the compositor.
    REXLOG_WARN("VR: xrReleaseSwapchainImage failed ({})", ResultName(r));
    g.have_image = false;
    g.stop = true;
    return true;
  }
  g.have_image = true;
  g.shown_extent = {int32_t(out_width), int32_t(out_height)};
  ++g.stat_copies;
  return true;
}

bool ChooseFormats() {
  uint32_t count = 0;
  g.xr.EnumerateSwapchainFormats(g.session, 0, &count, nullptr);
  std::vector<int64_t> formats(count);
  if (XR_FAILED(g.xr.EnumerateSwapchainFormats(g.session, count, &count, formats.data()))) return false;
  for (int64_t format : formats) {
    if (format == VK_FORMAT_R8G8B8A8_SRGB || format == VK_FORMAT_B8G8R8A8_SRGB) {
      g.swapchain_format = format;
      g.staging_format = format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM
                                                           : VK_FORMAT_B8G8R8A8_UNORM;
      break;
    }
  }
  if (!g.swapchain_format) {
    // No 8-bit sRGB format: an 8-bit UNORM one, blitted to directly (the picture looks too dark).
    for (int64_t format : formats) {
      if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM) {
        g.swapchain_format = format;
        g.staging_format = VkFormat(format);
        REXLOG_WARN("VR: the runtime offers no 8-bit sRGB swapchain format; colours will be off");
        break;
      }
    }
  }
  if (!g.swapchain_format) {
    REXLOG_WARN("VR: the runtime offers no 8-bit RGBA swapchain format");
    return false;
  }
  VkFormatProperties source, staging;
  g.vk.GetPhysicalDeviceFormatProperties(g.device->physical_device(),
                                         VK_FORMAT_A2B10G10R10_UNORM_PACK32, &source);
  g.vk.GetPhysicalDeviceFormatProperties(g.device->physical_device(), g.staging_format, &staging);
  if (!(source.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) ||
      !(staging.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
    REXLOG_WARN("VR: the GPU cannot blit the game's picture to an 8-bit image");
    return false;
  }
  g.linear_blit = (source.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
  uint32_t modes = 0;
  g.xr.EnumerateEnvironmentBlendModes(g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      0, &modes, nullptr);
  std::vector<XrEnvironmentBlendMode> blend(modes);
  if (modes && XR_SUCCEEDED(g.xr.EnumerateEnvironmentBlendModes(
                   g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, modes, &modes,
                   blend.data())) && modes) {
    g.blend = blend[0];
  }
  return true;
}

bool CreateSession() {
  XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
  binding.instance = g.device->vulkan_instance()->instance();
  binding.physicalDevice = g.device->physical_device();
  binding.device = g.device->device();
  binding.queueFamilyIndex = g.device->queue_family_graphics_compute();
  binding.queueIndex = 0;
  XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
  info.next = &binding;
  info.systemId = g.system;
  XrResult r;
  {
    const auto queue = g.device->AcquireQueue(g.device->queue_family_graphics_compute(), 0);
    r = g.xr.CreateSession(g.instance, &info, &g.session);
  }
  if (XR_FAILED(r)) {
    REXLOG_WARN("VR: xrCreateSession failed ({}) - playing flat", ResultName(r));
    g.session = XR_NULL_HANDLE;
    return false;
  }
  XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  space.poseInReferenceSpace.orientation.w = 1;
  space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  if (XR_FAILED(r = g.xr.CreateReferenceSpace(g.session, &space, &g.local))) {
    REXLOG_WARN("VR: no LOCAL space ({}) - playing flat", ResultName(r));
    return false;
  }
  space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  if (XR_FAILED(r = g.xr.CreateReferenceSpace(g.session, &space, &g.view))) {
    REXLOG_WARN("VR: no VIEW space ({}) - playing flat", ResultName(r));
    return false;
  }
  if (!ChooseFormats()) {
    REXLOG_WARN("VR: no usable swapchain format - playing flat");
    return false;
  }
  uint32_t count = 0;
  g.xr.EnumerateViewConfigurationViews(g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                       0, &count, nullptr);
  std::vector<XrViewConfigurationView> views(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  if (count && XR_SUCCEEDED(g.xr.EnumerateViewConfigurationViews(
                   g.instance, g.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count,
                   views.data()))) {
    REXLOG_INFO("VR: session created; the headset recommends {}x{} per eye",
                views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight);
  }
  return true;
}

// While a session runs the player looks at the headset and the desktop window usually has no
// focus: pads and wheels keep working then (patch 0392; the keyboard stays focus-bound).
void PadsWithoutFocus(bool on) {
  if (g.set_pads_without_focus) g.set_pads_without_focus(on ? 1 : 0);
}

bool DestroySession() {
  g.cockpit_active = false;
  PadsWithoutFocus(false);
  {
    std::lock_guard lock(g.pose_mutex);  // no CockpitView is locating now
    if (g.session != XR_NULL_HANDLE && !DestroySwapchain()) return false;
    if (g.view != XR_NULL_HANDLE) g.xr.DestroySpace(g.view);
    if (g.local != XR_NULL_HANDLE) g.xr.DestroySpace(g.local);
    g.view = g.local = XR_NULL_HANDLE;
  }
  if (g.session != XR_NULL_HANDLE) {
    const auto queue = g.device->AcquireQueue(g.device->queue_family_graphics_compute(), 0);
    g.xr.DestroySession(g.session);
  }
  g.session = XR_NULL_HANDLE;
  g.running = false;
  if (g.set_export) g.set_export(0);
  for (CopySlot& slot : g.slots) {
    if (slot.fence) g.vk.DestroyFence(g.device->device(), slot.fence, nullptr);
    if (slot.pool) g.vk.DestroyCommandPool(g.device->device(), slot.pool, nullptr);
    slot = {};
  }
  return true;
}

void FinishSession() {
  bool warned = false;
  while (!DestroySession()) {
    if (!warned) REXLOG_WARN("VR: waiting for pending GPU copies before destroying session resources");
    warned = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  g.finished = true;
}

const char* StateName(XrSessionState s) {
  switch (s) {
    case XR_SESSION_STATE_IDLE: return "idle";
    case XR_SESSION_STATE_READY: return "ready";
    case XR_SESSION_STATE_SYNCHRONIZED: return "synchronized";
    case XR_SESSION_STATE_VISIBLE: return "visible";
    case XR_SESSION_STATE_FOCUSED: return "focused";
    case XR_SESSION_STATE_STOPPING: return "stopping";
    case XR_SESSION_STATE_LOSS_PENDING: return "loss pending";
    case XR_SESSION_STATE_EXITING: return "exiting";
    default: return "unknown";
  }
}

// Handles the runtime's events; false when the session is over (exiting or lost).
bool PollEvents() {
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (g.xr.PollEvent(g.instance, &event) == XR_SUCCESS) {
    switch (event.type) {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
        const auto& changed = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
        g.state = changed.state;
        REXLOG_INFO("VR: session {}", StateName(g.state));
        if (g.state == XR_SESSION_STATE_READY) {
          XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
          begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          const XrResult r = g.xr.BeginSession(g.session, &begin);
          if (XR_FAILED(r)) {
            REXLOG_WARN("VR: xrBeginSession failed ({}) - playing flat", ResultName(r));
            return false;
          }
          g.running = true;
          g.recenter = true;
          g.stat_since = std::chrono::steady_clock::now();
          PadsWithoutFocus(true);
        } else if (g.state == XR_SESSION_STATE_STOPPING) {
          g.cockpit_active = false;
          g.running = false;
          PadsWithoutFocus(false);
          g.xr.EndSession(g.session);
        } else if (g.state == XR_SESSION_STATE_EXITING || g.state == XR_SESSION_STATE_LOSS_PENDING) {
          return false;
        }
        break;
      }
      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
        REXLOG_WARN("VR: the OpenXR runtime is going away");
        return false;
      case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
        REXLOG_INFO("VR: the runtime recentred its space");
        g.recenter = true;
        break;
      default:
        break;
    }
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }
  return true;
}

void LogStats() {
  const auto now = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(now - g.stat_since).count();
  if (seconds < 10) return;
  const uint32_t swaps = g.stat_swaps.exchange(0), tagged = g.stat_tagged_swaps.exchange(0);
  const uint32_t extra = g.stat_extra_views.exchange(0), repeat = g.stat_repeat_views.exchange(0);
  // Latency is measured on cockpit frames only (a tagged picture has the time its camera was
  // built): without any in this window there is nothing to report.
  std::string latency = "latency -";
  if (g.stat_latency_samples) {
    const double n = g.stat_latency_samples;
    latency = fmt::format("latency {:.1f} ms (game {:.1f} + runtime {:.1f}; predicting {:.1f})",
                          g.latency.Raw() / 1e6, g.stat_game_ns / n / 1e6,
                          g.stat_runtime_ns / n / 1e6, g.latency.Value() / 1e6);
  }
  REXLOG_INFO(
      "VR: {:.0f} Hz headset, {:.1f} game frames/s copied; shown as cockpit {} / screen {} "
      "frames; swaps tagged {}/{} (dropped views {}, repeated {}); {}; xrEndFrame max {:.1f} ms",
      g.stat_frames / seconds, g.stat_copies / seconds, g.stat_projection, g.stat_quad, tagged,
      swaps, extra, repeat, latency, g.stat_end_frame_max_ns / 1e6);
  g.stat_frames = g.stat_projection = g.stat_quad = g.stat_copies = 0;
  g.stat_end_frame_max_ns = 0;
  g.stat_game_ns = g.stat_runtime_ns = 0;
  g.stat_latency_samples = 0;
  g.stat_since = now;
}

// vr_trace, about 10 times a second: the head relative to the recentred pose (yaw left, pitch up,
// roll left in degrees; x right, y up, z forward in cm, as the game's camera takes them), the
// game camera and whether the head pose went into it, what the headset shows, the field of view
// the shown picture was drawn with, and the last cockpit frame's latency.
void Trace(XrTime display_time, Layer layer) {
  const int64_t now = SteadyNs();
  if (now < g.trace_next_ns) return;
  g.trace_next_ns = now + 100'000'000;
  std::string head = "head not tracked";
  {
    std::lock_guard lock(g.pose_mutex);
    Pose located;
    if (g.origin_set && LocateHead(display_time, located)) {
      const Pose rel = Relative(g.origin, located);
      float yaw, pitch, roll;
      YawPitchRoll(rel.q, yaw, pitch, roll);
      // + 0.0f: no "-0" in the log.
      head = fmt::format("head yaw {:.0f} pitch {:.0f} roll {:.0f} deg, x {:.1f} y {:.1f} z {:.1f} cm",
                         std::round(yaw * 57.2958f) + 0.0f, std::round(pitch * 57.2958f) + 0.0f,
                         std::round(roll * 57.2958f) + 0.0f, std::round(rel.p.x * 1000) / 10 + 0.0f,
                         std::round(rel.p.y * 1000) / 10 + 0.0f, std::round(-rel.p.z * 1000) / 10 + 0.0f);
    }
  }
  const int64_t view_age = now - g.trace_view_ns.load();
  std::string camera;
  if (!g.trace_view_ns.load() || view_age > 500'000'000) {
    camera = "no game view";
  } else {
    const char* name = g.trace_camera_name.load();
    camera = fmt::format("camera {} {:08X} look {:.0f} deg, {}", name ? name : "?", g.trace_camera.load(),
                         std::round(g.trace_look.load() * 57.2958f) + 0.0f,
                         g.trace_posed.load() ? "posed" : "not posed");
  }
  std::string shown = layer == Layer::kProjection ? "cockpit" : layer == Layer::kQuad ? "screen" : "nothing";
  if (layer == Layer::kProjection) {
    const CockpitFrustum& f = g.shown_tag.frustum;
    shown += fmt::format(" {:.0f}x{:.0f} deg pitch {:.1f}", 2 * f.HalfHorizontal() * 57.2958f,
                         f.vertical * 57.2958f, f.pitch * 57.2958f);
  }
  const int64_t game_ns = g.last_game_ns.load(), runtime_ns = g.last_runtime_ns.load();
  const std::string latency =
      game_ns || runtime_ns ? fmt::format("latency {:.0f} ms (game {:.0f} + runtime {:.0f})",
                                          (game_ns + runtime_ns) / 1e6, game_ns / 1e6, runtime_ns / 1e6)
                            : "latency -";
  REXLOG_INFO("VR trace: {}; {}; shows {}; {}", head, camera, shown, latency);
}

// One headset frame: wait, take the newest game picture, submit the layer.
bool Frame() {
  XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO};
  XrFrameState frame_state{XR_TYPE_FRAME_STATE};
  XrResult r = g.xr.WaitFrame(g.session, &wait, &frame_state);
  if (XR_FAILED(r)) {
    if (!g.frame_failures++) REXLOG_WARN("VR: xrWaitFrame failed ({})", ResultName(r));
    return r != XR_ERROR_SESSION_LOST && r != XR_ERROR_INSTANCE_LOST;
  }
  g.wait_display_time = frame_state.predictedDisplayTime;
  g.wait_period = frame_state.predictedDisplayPeriod;
  g.wait_steady_ns = SteadyNs();
  {
    const auto queue = g.device->AcquireQueue(g.device->queue_family_graphics_compute(), 0);
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO};
    r = g.xr.BeginFrame(g.session, &begin);
  }
  if (XR_FAILED(r)) {
    if (!g.frame_failures++) REXLOG_WARN("VR: xrBeginFrame failed ({})", ResultName(r));
    return r != XR_ERROR_SESSION_LOST && r != XR_ERROR_INSTANCE_LOST;
  }
  ++g.stat_frames;

  // The eyes' fields of view and orientations relative to the head: what the cockpit view has
  // to cover.
  XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
  locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate.displayTime = frame_state.predictedDisplayTime;
  locate.space = g.view;
  XrViewState view_state{XR_TYPE_VIEW_STATE};
  XrView eyes[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
  uint32_t eye_count = 0;
  if (g.mode == Mode::kCockpit &&
      XR_SUCCEEDED(g.xr.LocateViews(g.session, &locate, &view_state, 2, &eye_count, eyes)) &&
      eye_count == 2) {
    const Fov fovs[2] = {
        {eyes[0].fov.angleLeft, eyes[0].fov.angleRight, eyes[0].fov.angleUp, eyes[0].fov.angleDown},
        {eyes[1].fov.angleLeft, eyes[1].fov.angleRight, eyes[1].fov.angleUp, eyes[1].fov.angleDown}};
    const bool oriented = view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT;
    const Quat rotation[2] = {oriented ? ToPose(eyes[0].pose).q : Quat{},
                              oriented ? ToPose(eyes[1].pose).q : Quat{}};
    const float margin = float(REXCVAR_GET(vr_fov_margin)) * 3.14159265f / 180.0f;
    const CockpitFrustum f = CoverEyes(fovs, rotation, margin, 1.5f, 2.2f);
    bool changed;
    {
      std::lock_guard lock(g.pose_mutex);
      changed = std::fabs(f.vertical - g.frustum.vertical) > 0.005f ||
                std::fabs(f.aspect - g.frustum.aspect) > 0.005f ||
                std::fabs(f.pitch - g.frustum.pitch) > 0.005f || std::fabs(f.yaw - g.frustum.yaw) > 0.005f;
      if (changed) g.frustum = f;
    }
    if (changed) {
      REXLOG_INFO("VR: eye fields of view L {:.0f}/{:.0f}/{:.0f}/{:.0f}, R {:.0f}/{:.0f}/{:.0f}/{:.0f} deg",
                  fovs[0].left * 57.2958f, fovs[0].right * 57.2958f, fovs[0].up * 57.2958f,
                  fovs[0].down * 57.2958f, fovs[1].left * 57.2958f, fovs[1].right * 57.2958f,
                  fovs[1].up * 57.2958f, fovs[1].down * 57.2958f);
      LogCockpitView();
    }
  }

  if (g.recenter.load()) {
    Pose head;
    if (LocateHead(frame_state.predictedDisplayTime, head)) {
      {
        std::lock_guard lock(g.pose_mutex);
        g.origin = RecenterOrigin(head);
        g.origin_set = true;
      }
      g.recenter = false;
      REXLOG_INFO("VR: recentred (head at {:.2f} {:.2f} {:.2f} m)", head.p.x, head.p.y, head.p.z);
    }
  }
  g.cockpit_active = !g.stop && g.mode == Mode::kCockpit && g.origin_set &&
                     (g.state == XR_SESSION_STATE_VISIBLE || g.state == XR_SESSION_STATE_FOCUSED);

  if (frame_state.shouldRender) {
    RexGuestOutputFrame frame{sizeof(RexGuestOutputFrame)};
    if (g.acquire_frame(&frame, g.last_serial)) {
      g.last_serial = frame.serial;
      FrameTag tag;
      {
        std::lock_guard lock(g.tags_mutex);
        if (const FrameTag* found = g.tags.Find(frame.tag)) tag = *found;
      }
      const bool cockpit = ChooseLayer(true, g.mode == Mode::kCockpit, tag.id ? &tag : nullptr,
                                       frame_state.predictedDisplayTime) == Layer::kProjection;
      if (CopyFrame(frame, !cockpit)) {
        if (tag.id && tag.id != g.shown_tag.id &&
            frame_state.predictedDisplayTime - tag.build_time <= kMaxTagAgeNs) {
          // Built to shown, and its two parts: built to here (the game's frames in flight, the
          // render thread, the GPU, this thread's pick-up) and here to the display time (the
          // runtime's compositor and, for WiVRn, encoding, the network and the headset).
          const int64_t now = NowXrTime();
          g.latency.Add(frame_state.predictedDisplayTime - tag.build_time);
          g.latency_ns = g.latency.Value();
          g.last_game_ns = now - tag.build_time;
          g.last_runtime_ns = frame_state.predictedDisplayTime - now;
          g.stat_game_ns += now - tag.build_time;
          g.stat_runtime_ns += frame_state.predictedDisplayTime - now;
          ++g.stat_latency_samples;
        }
        g.shown_tag = tag;
      } else {
        g.release_frame(frame.handle);
      }
    }
  }
  ReapSlots();

  XrCompositionLayerProjectionView views[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                               {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  const XrCompositionLayerBaseHeader* layers[1] = {};
  uint32_t layer_count = 0;
  const XrRect2Di rect{{0, 0}, g.shown_extent};
  const Layer layer = frame_state.shouldRender
                          ? ChooseLayer(g.have_image, g.mode == Mode::kCockpit,
                                        g.shown_tag.id ? &g.shown_tag : nullptr,
                                        frame_state.predictedDisplayTime)
                          : Layer::kNone;
  if (layer == Layer::kProjection) {
    // Mono: both eyes get the picture as drawn from the head pose (turned to the drawn axis) and
    // the field of view it was rendered with.
    const Fov fov = g.shown_tag.frustum.ImageFov();
    for (auto& view : views) {
      view.pose = ToXr(g.shown_tag.head);
      view.fov = {fov.left, fov.right, fov.up, fov.down};
      view.subImage = {g.swapchain, rect, 0};
    }
    projection.space = g.local;
    projection.viewCount = 2;
    projection.views = views;
    layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    ++g.stat_projection;
  } else if (layer == Layer::kQuad) {
    Pose origin;
    {
      std::lock_guard lock(g.pose_mutex);
      origin = g.origin;
    }
    const float distance = float(REXCVAR_GET(vr_screen_distance));
    const float width = float(REXCVAR_GET(vr_screen_width));
    quad.space = g.local;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage = {g.swapchain, rect, 0};
    quad.pose = ToXr(Compose(origin, {Quat{}, Vec3{0, 0, -distance}}));
    quad.size = {width, width * float(g.shown_extent.height) / float(std::max(g.shown_extent.width, 1))};
    layers[layer_count++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    ++g.stat_quad;
  }
  XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
  end.displayTime = frame_state.predictedDisplayTime;
  end.environmentBlendMode = g.blend;
  end.layerCount = layer_count;
  end.layers = layers;
  const int64_t end_start = SteadyNs();
  {
    const auto queue = g.device->AcquireQueue(g.device->queue_family_graphics_compute(), 0);
    r = g.xr.EndFrame(g.session, &end);
  }
  g.stat_end_frame_max_ns = std::max(g.stat_end_frame_max_ns, SteadyNs() - end_start);
  if (XR_FAILED(r)) {
    if (!g.frame_failures++) REXLOG_WARN("VR: xrEndFrame failed ({})", ResultName(r));
    return r != XR_ERROR_SESSION_LOST && r != XR_ERROR_INSTANCE_LOST;
  }
  g.frame_failures = 0;
  LogStats();
  if (g.trace) Trace(frame_state.predictedDisplayTime, layer);
  return true;
}

void Run() {
#if defined(__APPLE__)
  pthread_setname_np("Forza OpenXR");  // macOS names only the calling thread
#elif !defined(_WIN32)
  pthread_setname_np(pthread_self(), "Forza OpenXR");
#endif
  if (!LoadDeviceFunctions() || !CreateCopySlots()) {
    REXLOG_WARN("VR: Vulkan functions or command buffers missing - playing flat");
    FinishSession();
    return;
  }
  if (!CreateSession()) {
    FinishSession();
    return;
  }
  bool exit_requested = false;
  const int64_t never = INT64_MAX;
  int64_t exit_deadline = never;
  while (true) {
    if (g.stop && !exit_requested) {
      exit_requested = true;
      exit_deadline = SteadyNs() + 1'000'000'000;
      if (g.running) g.xr.RequestExitSession(g.session);
    }
    if (exit_requested && (!g.running || SteadyNs() > exit_deadline)) break;
    if (!PollEvents()) {
      if (!exit_requested) REXLOG_WARN("VR: the session ended ({}) - playing flat", StateName(g.state));
      break;
    }
    if (!g.running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    if (!Frame()) {
      REXLOG_WARN("VR: the session was lost - playing flat");
      break;
    }
    if (g.frame_failures > 100) {
      REXLOG_WARN("VR: the runtime keeps refusing frames - playing flat");
      break;
    }
  }
  FinishSession();
  REXLOG_INFO("VR: OpenXR thread stopped");
}

Mode RequestedMode() {
  const std::string mode = REXCVAR_GET(vr_mode);
  if (mode == "screen") return Mode::kScreen;
  if (mode == "cockpit") return Mode::kCockpit;
  return Mode::kOff;
}

int64_t pad_held_since = 0;  // the pad polling thread only
bool pad_fired = false;

// What the probe of the runtime found, handed from its thread to Prepare.
struct ProbeResult {
  std::mutex mutex;
  std::condition_variable done_cv;
  bool done = false;
  bool abandoned = false;  // Prepare stopped waiting: the probe cleans up after itself
  std::string error;  // empty: a headset is there
  Api xr;
  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system = XR_NULL_SYSTEM_ID;
  std::string runtime_name, system_name;
  uint32_t vendor = 0;
  XrVersion vulkan_min = 0, vulkan_max = 0;
};

// Instance, headset and Vulkan requirements. On its own thread: WiVRn's xrCreateInstance waits
// for a headset to connect, however long that takes.
std::string RunProbe(ProbeResult& p) {
  Api& xr = p.xr;
  uint32_t count = 0;
  XrResult r = xr.EnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
  if (XR_FAILED(r)) {
    return "no OpenXR runtime is installed or active (" + ResultName(r) +
           "): install WiVRn, Monado or SteamVR and start it before the game";
  }
  std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
  xr.EnumerateInstanceExtensionProperties(nullptr, count, &count, extensions.data());
  bool vulkan2 = false, convert_time = false;
#ifdef _WIN32
  const char* time_extension = XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME;
#else
  const char* time_extension = XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME;
#endif
  for (const auto& e : extensions) {
    vulkan2 |= std::strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0;
    convert_time |= std::strcmp(e.extensionName, time_extension) == 0;
  }
  if (!vulkan2) return "the OpenXR runtime has no XR_KHR_vulkan_enable2";
  std::vector<const char*> enabled{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
  if (convert_time) enabled.push_back(time_extension);
  XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strncpy(create.applicationInfo.applicationName, "Forza Horizon 1 ReImagined",
               XR_MAX_APPLICATION_NAME_SIZE - 1);
  create.applicationInfo.applicationVersion = 1;
  std::strncpy(create.applicationInfo.engineName, "ReXGlue", XR_MAX_ENGINE_NAME_SIZE - 1);
  create.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  create.enabledExtensionCount = uint32_t(enabled.size());
  create.enabledExtensionNames = enabled.data();
  r = xr.CreateInstance(&create, &p.instance);
  if (XR_FAILED(r)) {
    p.instance = XR_NULL_HANDLE;
    return "the OpenXR runtime did not start (" + ResultName(r) +
           "): start its server (WiVRn, Monado, SteamVR) and connect the headset before the game";
  }
  bool ok = true;
#define FORZA_XR(name) ok &= Load(p.instance, "xr" #name, xr.name);
  FORZA_XR(DestroyInstance)
  FORZA_XR(ResultToString)
  FORZA_XR(GetInstanceProperties)
  FORZA_XR(GetSystem)
  FORZA_XR(GetSystemProperties)
  FORZA_XR(PollEvent)
  FORZA_XR(CreateSession)
  FORZA_XR(DestroySession)
  FORZA_XR(BeginSession)
  FORZA_XR(EndSession)
  FORZA_XR(RequestExitSession)
  FORZA_XR(CreateReferenceSpace)
  FORZA_XR(DestroySpace)
  FORZA_XR(LocateSpace)
  FORZA_XR(LocateViews)
  FORZA_XR(EnumerateViewConfigurationViews)
  FORZA_XR(EnumerateEnvironmentBlendModes)
  FORZA_XR(EnumerateSwapchainFormats)
  FORZA_XR(CreateSwapchain)
  FORZA_XR(DestroySwapchain)
  FORZA_XR(EnumerateSwapchainImages)
  FORZA_XR(AcquireSwapchainImage)
  FORZA_XR(WaitSwapchainImage)
  FORZA_XR(ReleaseSwapchainImage)
  FORZA_XR(WaitFrame)
  FORZA_XR(BeginFrame)
  FORZA_XR(EndFrame)
  FORZA_XR(GetVulkanGraphicsRequirements2KHR)
  FORZA_XR(CreateVulkanInstanceKHR)
  FORZA_XR(CreateVulkanDeviceKHR)
  FORZA_XR(GetVulkanGraphicsDevice2KHR)
#undef FORZA_XR
  if (!ok) return "the OpenXR runtime lacks core or Vulkan functions";
#ifdef _WIN32
  if (convert_time) Load(p.instance, "xrConvertWin32PerformanceCounterToTimeKHR", xr.ConvertTime);
#else
  if (convert_time) Load(p.instance, "xrConvertTimespecTimeToTimeKHR", xr.ConvertTime);
#endif
  XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
  xr.GetInstanceProperties(p.instance, &properties);
  p.runtime_name = properties.runtimeName;
  REXLOG_INFO("VR: OpenXR runtime {} {}.{}.{}", properties.runtimeName,
              XR_VERSION_MAJOR(properties.runtimeVersion), XR_VERSION_MINOR(properties.runtimeVersion),
              XR_VERSION_PATCH(properties.runtimeVersion));
  XrSystemGetInfo system{XR_TYPE_SYSTEM_GET_INFO};
  system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  r = xr.GetSystem(p.instance, &system, &p.system);
  if (XR_FAILED(r)) {
    return p.runtime_name + " has no headset (" + ResultName(r) +
           "): connect it before starting the game";
  }
  XrSystemProperties system_properties{XR_TYPE_SYSTEM_PROPERTIES};
  xr.GetSystemProperties(p.instance, p.system, &system_properties);
  p.system_name = system_properties.systemName;
  p.vendor = system_properties.vendorId;
  XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
  r = xr.GetVulkanGraphicsRequirements2KHR(p.instance, p.system, &requirements);
  if (XR_FAILED(r)) return "xrGetVulkanGraphicsRequirements2KHR failed (" + ResultName(r) + ")";
  p.vulkan_min = requirements.minApiVersionSupported;
  p.vulkan_max = requirements.maxApiVersionSupported;
  return {};
}

void ProbeThread(std::shared_ptr<ProbeResult> p) {
  const auto start = std::chrono::steady_clock::now();
  std::string error = RunProbe(*p);
  std::lock_guard lock(p->mutex);
  p->error = std::move(error);
  p->done = true;
  if (p->abandoned) {
    REXLOG_WARN("VR: the OpenXR runtime answered after {:.0f} s, too late; the game stays flat",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    if (p->instance != XR_NULL_HANDLE && p->xr.DestroyInstance) p->xr.DestroyInstance(p->instance);
    p->instance = XR_NULL_HANDLE;
  }
  p->done_cv.notify_all();
}

}  // namespace

void Prepare() {
  if (g.prepared || g.mode != Mode::kOff) return;
  g.mode = RequestedMode();
  if (g.mode == Mode::kOff) return;
  REXLOG_INFO("VR: {} mode requested", g.mode == Mode::kCockpit ? "cockpit" : "screen");
  if (!LoadLoader()) {
    FlatBeforeHooks(std::string("no OpenXR loader (") + kLoaderName + ")");
    return;
  }
  if (!Load(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties",
            g.xr.EnumerateInstanceExtensionProperties) ||
      !Load(XR_NULL_HANDLE, "xrCreateInstance", g.xr.CreateInstance)) {
    FlatBeforeHooks("the OpenXR loader lacks its global functions");
    return;
  }
  auto probe = std::make_shared<ProbeResult>();
  probe->xr = g.xr;
  std::thread(ProbeThread, probe).detach();
  const int timeout = int(REXCVAR_GET(vr_runtime_timeout));
  {
    std::unique_lock lock(probe->mutex);
    if (!probe->done_cv.wait_for(lock, std::chrono::seconds(1), [&] { return probe->done; })) {
      REXLOG_INFO("VR: waiting up to {} s for the OpenXR runtime", timeout);
      probe->done_cv.wait_for(lock, std::chrono::seconds(std::max(timeout - 1, 0)),
                              [&] { return probe->done; });
    }
    if (!probe->done) {
      probe->abandoned = true;
      // The loader stays loaded: the probe's thread is still inside it.
      REXLOG_WARN("VR: the OpenXR runtime did not answer in {} s (WiVRn waits until the headset "
                  "connects: connect it before starting the game) - playing flat",
                  timeout);
      g.mode = Mode::kOff;
      return;
    }
  }
  g.xr = probe->xr;
  g.instance = probe->instance;
  g.system = probe->system;
  g.runtime_name = probe->runtime_name;
  if (!probe->error.empty()) {
    FlatBeforeHooks(probe->error);
    return;
  }
  auto set_hooks =
      reinterpret_cast<SetVulkanCreationHooksFn>(forza::SdkSymbol("rexui_SetVulkanCreationHooks"));
  if (!set_hooks || !forza::SdkSymbol("rexui_AcquireGuestOutputFrame")) {
    FlatBeforeHooks("this SDK build has no OpenXR support (patches 0390 and 0391)");
    return;
  }
  RexVulkanCreationHooks hooks{};
  hooks.size = sizeof(hooks);
  hooks.create_instance = HookCreateInstance;
  hooks.physical_device = HookPhysicalDevice;
  hooks.create_device = HookCreateDevice;
  set_hooks(&hooks);
  g.prepared = true;
  REXLOG_INFO("VR: headset {} (vendor {:04X}); Vulkan {}.{} to {}.{}; the runtime picks the GPU",
              probe->system_name, probe->vendor, XR_VERSION_MAJOR(probe->vulkan_min),
              XR_VERSION_MINOR(probe->vulkan_min), XR_VERSION_MAJOR(probe->vulkan_max),
              XR_VERSION_MINOR(probe->vulkan_max));
}

void Start(rex::system::IGraphicsSystem* graphics) {
  if (!g.prepared || g.started) return;
  rex::ui::GraphicsProvider* provider = graphics ? graphics->provider() : nullptr;
  if (!provider) return;  // not yet: called again once it exists
  g.started = true;
  if (!std::strstr(typeid(*provider).name(), "VulkanProvider")) {
    REXLOG_WARN("VR: needs the Vulkan renderer (gpu_backend=vulkan) - playing flat");
    return;
  }
  VulkanDevice* device = static_cast<rex::ui::vulkan::VulkanProvider*>(provider)->vulkan_device();
  if (g.vk_failed || !device || device->device() != g.vk_device) {
    REXLOG_WARN("VR: the Vulkan device was not created through the OpenXR runtime - playing flat");
    return;
  }
  g.set_export = reinterpret_cast<SetGuestOutputExportFn>(forza::SdkSymbol("rexui_SetGuestOutputExport"));
  g.acquire_frame =
      reinterpret_cast<AcquireGuestOutputFrameFn>(forza::SdkSymbol("rexui_AcquireGuestOutputFrame"));
  g.release_frame =
      reinterpret_cast<ReleaseGuestOutputFrameFn>(forza::SdkSymbol("rexui_ReleaseGuestOutputFrame"));
  if (!g.set_export || !g.acquire_frame || !g.release_frame) {
    REXLOG_WARN("VR: this SDK build cannot hand the picture to OpenXR (patch 0391) - playing flat");
    return;
  }
  g.set_pads_without_focus =
      reinterpret_cast<SetPadsWithoutFocusFn>(forza::SdkSymbol("rexinput_SetPadsWithoutFocus"));
  if (!g.set_pads_without_focus) {
    REXLOG_WARN("VR: this SDK build drops pad input while the game window has no focus (patch 0392): "
                "click the game window before putting the headset on");
  }
  g.trace = REXCVAR_GET(vr_trace);
  g.device = device;
  g.set_export(1);
  if (const std::string key = REXCVAR_GET(vr_recenter_key); !key.empty()) {
    rex::ui::RegisterBind("bind_vr_recenter", key, "Recentre the VR view", [] { Recenter(); });
  }
  REXLOG_INFO("VR: starting the session on {} ({} to recentre, or hold both sticks)",
              device->properties().deviceName, REXCVAR_GET(vr_recenter_key));
  g.thread = std::thread(Run);
}

void Shutdown() {
  if (!g.thread.joinable()) return;
  g.stop = true;
  g.cockpit_active = false;
  // The thread requests exit when its current runtime call returns. This is an
  // app-close hook, before the SDK destroys the presenter and its Vulkan device.
  // Never detach: a runtime call may still resume and use those SDK resources.
  for (int i = 0; i < 300 && !g.finished; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (!g.finished) {
    REXLOG_WARN("VR: still waiting for OpenXR shutdown; a stalled runtime can prevent the game from closing safely");
  }
  g.thread.join();
  if (g.instance != XR_NULL_HANDLE) g.xr.DestroyInstance(g.instance);
  g.instance = XR_NULL_HANDLE;
}

bool CockpitActive() { return g.cockpit_active.load(std::memory_order_relaxed); }

bool CockpitView(vr::HeadTransform& head, float& vertical_fov, float& aspect, uint32_t& tag) {
  if (!CockpitActive()) return false;
  FrameTag t;
  {
    std::lock_guard lock(g.pose_mutex);
    if (g.local == XR_NULL_HANDLE || !g.origin_set || g.frustum.vertical <= 0) return false;
    t.frustum = g.frustum;
    t.build_time = NowXrTime();
    t.target_time = t.build_time + g.latency_ns.load();
    Pose located;
    if (!LocateHead(t.target_time, located)) return false;
    // The camera looks along the frustum's axis: the head turned by its offset, in the game (the
    // recentred head) and in the layer (the head in LOCAL) alike, from the eye held in front of
    // the seat.
    const Pose rel = LimitBack(Relative(g.origin, located), float(REXCVAR_GET(vr_head_back_limit_cm)) / 100.0f);
    t.head = TurnView(Compose(g.origin, rel), t.frustum);
    head = ToGameHead(TurnView(rel, t.frustum), float(REXCVAR_GET(vr_seat_height_cm)) / 100.0f);
  }
  t.id = g.next_tag.fetch_add(1);
  if (!t.id) t.id = g.next_tag.fetch_add(1);
  {
    std::lock_guard lock(g.tags_mutex);
    g.tags.Put(t);
  }
  vertical_fov = t.frustum.vertical;
  aspect = t.frustum.aspect;
  tag = t.id;
  return true;
}

bool Tracing() { return g.trace.load(std::memory_order_relaxed); }

void NoteView(uint32_t camera_vtable, const char* camera_name, float look_angle, bool posed) {
  g.trace_camera.store(camera_vtable, std::memory_order_relaxed);
  g.trace_camera_name.store(camera_name, std::memory_order_relaxed);
  g.trace_look.store(look_angle, std::memory_order_relaxed);
  g.trace_posed.store(posed, std::memory_order_relaxed);
  g.trace_view_ns.store(SteadyNs(), std::memory_order_relaxed);
}

void RememberPosedView(uint32_t tag, const uint8_t matrix[64]) {
  std::lock_guard lock(g.tags_mutex);
  g.views.Remember(tag, matrix);
}

void OnRenderView(uint32_t camera, const uint8_t matrix[64]) {
  std::lock_guard lock(g.tags_mutex);
  g.swap_tagger.OnView(camera, g.views.Match(matrix));
}

void TagSwap(uint8_t* base, uint32_t packet) {
  if (!g.started || !g.device) return;
  uint32_t views = 0, tag;
  {
    std::lock_guard lock(g.tags_mutex);
    tag = g.swap_tagger.TakeForSwap(&views);
  }
  ++g.stat_swaps;
  if (views > 1) g.stat_extra_views += views - 1;
  if (!views && tag) ++g.stat_repeat_views;
  if (!tag || !packet) return;
  // The packet is big-endian guest memory.
  uint8_t* p = forza::GuestPtr(base, packet);
  uint32_t dwords[64];
  for (int i = 0; i < 64; ++i) {
    const uint8_t* q = p + 4 * i;
    dwords[i] = uint32_t(q[0]) << 24 | uint32_t(q[1]) << 16 | uint32_t(q[2]) << 8 | q[3];
  }
  constexpr uint32_t kSwapSignature = 'S' << 24 | 'W' << 16 | 'A' << 8 | 'P';
  if (!TagSwapPacket(dwords, kSwapSignature, tag)) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) REXLOG_WARN("VR: unexpected swap packet layout; frames stay untagged");
    return;
  }
  for (size_t i : {kXeSwapHeaderIndex, kXeSwapTagIndex}) {
    uint8_t* q = p + 4 * i;
    q[0] = uint8_t(dwords[i] >> 24), q[1] = uint8_t(dwords[i] >> 16);
    q[2] = uint8_t(dwords[i] >> 8), q[3] = uint8_t(dwords[i]);
  }
  ++g.stat_tagged_swaps;
}

void Recenter() {
  if (g.started && g.device) {
    g.recenter = true;
    REXLOG_INFO("VR: recentre requested");
  }
}

void OnPad(uint16_t buttons) {
  if (!g.started || !g.device) return;
  constexpr uint16_t kSticks = 0x0040 | 0x0080;  // left and right thumbstick presses
  if ((buttons & kSticks) != kSticks) {
    pad_held_since = 0;
    pad_fired = false;
    return;
  }
  const int64_t now = SteadyNs();
  if (!pad_held_since) pad_held_since = now;
  if (!pad_fired && now - pad_held_since >= 1'000'000'000) {
    pad_fired = true;
    Recenter();
  }
}

}  // namespace forza::xr
