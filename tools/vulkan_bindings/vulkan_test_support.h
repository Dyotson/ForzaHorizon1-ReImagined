#pragma once
#include <vulkan/vulkan.h>
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)
#define VK_CHECK(call) CHECK((call) == VK_SUCCESS)

struct Context {
  std::atomic<uint32_t> validation_errors{0};
  VkInstance instance{};
  VkDebugUtilsMessengerEXT messenger{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  VkCommandPool pool{};
  VkCommandBuffer command{};
  VkFence fence{};
  bool full_index_uint32 = false, dynamic_rendering = false;
  ~Context() {
    if (device) {
      vkDeviceWaitIdle(device);
      if (fence) vkDestroyFence(device, fence, nullptr);
      if (pool) vkDestroyCommandPool(device, pool, nullptr);
      vkDestroyDevice(device, nullptr);
    }
    if (messenger) {
      auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
      destroy(instance, messenger, nullptr);
    }
    if (instance) vkDestroyInstance(instance, nullptr);
  }
  static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
      VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* message, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
      ++static_cast<Context*>(user)->validation_errors;
    std::cerr << "Vulkan validation: " << message->pMessage << '\n';
    return VK_FALSE;
  }
  bool Initialize(bool validation, bool graphics = false, uint32_t api_version = VK_API_VERSION_1_1) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Native renderer GPU validation";
    app.apiVersion = api_version;
    VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &app;
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = Validation; debug.pUserData = this;
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const char* extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT features_info{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    features_info.enabledValidationFeatureCount = 1; features_info.pEnabledValidationFeatures = &sync;
    features_info.pNext = &debug;
    if (validation) {
      create.enabledLayerCount = 1; create.ppEnabledLayerNames = &layer;
      create.enabledExtensionCount = 2; create.ppEnabledExtensionNames = extensions;
      create.pNext = &features_info;
    }
    if (vkCreateInstance(&create, nullptr, &instance) != VK_SUCCESS) return false;
    if (validation) {
      auto make = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
          vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
      CHECK(make);
      VK_CHECK(make(instance, &debug, nullptr, &messenger));
      std::cout << "Vulkan synchronization validation enabled\n";
    }
    uint32_t count = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
    std::vector<VkPhysicalDevice> devices(count);
    VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
    uint32_t family = 0;
    for (auto candidate : devices) {
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, nullptr);
      std::vector<VkQueueFamilyProperties> families(count);
      vkGetPhysicalDeviceQueueFamilyProperties(candidate, &count, families.data());
      for (uint32_t i = 0; i < count; ++i)
        if ((families[i].queueFlags & (VK_QUEUE_COMPUTE_BIT | (graphics ? VK_QUEUE_GRAPHICS_BIT : 0))) ==
            (VK_QUEUE_COMPUTE_BIT | (graphics ? VK_QUEUE_GRAPHICS_BIT : 0))) {
          physical = candidate; family = i; break;
        }
      if (physical) break;
    }
    if (!physical) return false;
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(physical, &properties);
    std::cout << "GPU: " << properties.deviceName << ", driver " << properties.driverVersion << '\n';
    float priority = 1;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = family; queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures features{};
    if (graphics) {
      VkPhysicalDeviceFeatures available;
      vkGetPhysicalDeviceFeatures(physical, &available);
      features.fullDrawIndexUint32 = available.fullDrawIndexUint32;
      full_index_uint32 = available.fullDrawIndexUint32;
    }
    VkPhysicalDeviceDynamicRenderingFeatures dynamic{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
    if (api_version >= VK_API_VERSION_1_3 && properties.apiVersion >= VK_API_VERSION_1_3) {
      VkPhysicalDeviceFeatures2 available{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      available.pNext = &dynamic;
      vkGetPhysicalDeviceFeatures2(physical, &available);
      dynamic_rendering = dynamic.dynamicRendering;
    }
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = dynamic_rendering ? &dynamic : nullptr;
    device_info.queueCreateInfoCount = 1; device_info.pQueueCreateInfos = &queue_info;
    device_info.pEnabledFeatures = &features;
    VK_CHECK(vkCreateDevice(physical, &device_info, nullptr, &device));
    vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo command_pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    command_pool.queueFamilyIndex = family;
    command_pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(device, &command_pool, nullptr, &pool));
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = pool; allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device, &allocate, &command));
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(device, &fence_info, nullptr, &fence));
    return true;
  }
  uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags) return i;
    throw std::runtime_error("Missing test memory type");
  }
  void Begin() {
    VK_CHECK(vkResetCommandBuffer(command, 0));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(command, &begin));
  }
  void Submit() {
    VK_CHECK(vkEndCommandBuffer(command));
    VK_CHECK(vkResetFences(device, 1, &fence));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
  }
};

struct Buffer {
  Context& context;
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  void* mapped = nullptr;
  Buffer(Context& c, VkDeviceSize bytes, VkBufferUsageFlags usage) : context(c) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = bytes; info.usage = usage;
    VK_CHECK(vkCreateBuffer(c.device, &info, nullptr, &buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(c.device, buffer, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = c.MemoryType(requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(c.device, &allocation, nullptr, &memory));
    VK_CHECK(vkBindBufferMemory(c.device, buffer, memory, 0));
    VK_CHECK(vkMapMemory(c.device, memory, 0, bytes, 0, &mapped));
  }
  ~Buffer() {
    if (mapped) vkUnmapMemory(context.device, memory);
    vkDestroyBuffer(context.device, buffer, nullptr);
    vkFreeMemory(context.device, memory, nullptr);
  }
};
