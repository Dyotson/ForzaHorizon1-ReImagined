// Optional hardware test. Uses only synthetic fixtures, never game data.
#include "../src/platform/native_renderer/velocity_pass.h"
#include "../src/platform/native_renderer/resolve.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <atomic>
#include <memory>

using namespace forza::native_renderer;
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
  bool Initialize(bool validation) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Native velocity GPU validation";
    app.apiVersion = VK_API_VERSION_1_1;
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
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
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
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
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

struct Input {
  Context& context;
  VkImage image{};
  VkImageView view{};
  VkDeviceMemory memory{};
  uint32_t width = 37, height = 19;
  bool swizzled = false;
  std::vector<std::array<uint8_t, 4>> pixels;
  explicit Input(Context& c, uint32_t w = 37, uint32_t h = 19, bool bgra = false, bool storage = false)
      : context(c), width(w), height(h), swizzled(bgra), pixels(width * height) {
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x)
        pixels[y * width + x] = {uint8_t((x * 17) & 255), uint8_t((y * 37) & 255),
            uint8_t((x + y * width) & 255), uint8_t(224 + (x + y) % 32)};
    if (width > 1 && height > 1) pixels[width + 1] = {0, 0, 6, 240};
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D; info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {width, height, 1}; info.mipLevels = info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (storage) info.usage |= VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VK_CHECK(vkCreateImage(c.device, &info, nullptr, &image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(c.device, image, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = c.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(c.device, &allocate, nullptr, &memory));
    VK_CHECK(vkBindImageMemory(c.device, image, memory, 0));
    VkImageViewCreateInfo image_view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    image_view.image = image;
    image_view.viewType = storage ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    image_view.format = info.format;
    if (swizzled)
      image_view.components = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G,
          VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A};
    image_view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(c.device, &image_view, nullptr, &view));
    Buffer upload(c, pixels.size() * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(upload.mapped, pixels.data(), pixels.size() * 4);
    c.Begin();
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.image = image; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange = image_view.subresourceRange;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(c.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = info.extent;
    vkCmdCopyBufferToImage(c.command, upload.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.oldLayout = barrier.newLayout;
    barrier.newLayout = storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(c.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
    c.Submit();
  }
  ~Input() {
    vkDestroyImageView(context.device, view, nullptr);
    vkDestroyImage(context.device, image, nullptr);
    vkFreeMemory(context.device, memory, nullptr);
  }
  std::array<uint8_t, 4> Read(uint32_t x, uint32_t y) const {
    auto sample = pixels[y * width + x];
    if (swizzled) std::swap(sample[0], sample[2]);
    return sample;
  }
};

void Run(Context& context, const Input& input, const std::vector<uint32_t>& shader,
    uint32_t width, uint32_t height, bool rotated, bool singular, bool borrowed = false) {
  const size_t pixels = size_t(width) * height;
  // A four-channel float texel needs a sixteen-byte-aligned copy offset even
  // when the preceding single-channel image has an odd number of pixels.
  const VkDeviceSize motion_offset = (pixels * 4 + 15) & ~VkDeviceSize(15);
  const VkDeviceSize offsets[] = {0, motion_offset, motion_offset + pixels * 16};
  Buffer readback(context, offsets[2] + pixels * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  std::unique_ptr<Input> target;
  std::unique_ptr<Buffer> padding;
  VelocityPass::Output external;
  if (borrowed) {
    target = std::make_unique<Input>(context, width + 13, height + 7, false, true);
    padding = std::make_unique<Buffer>(context, target->pixels.size() * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    external = {target->image, target->view, VK_FORMAT_R8G8B8A8_UNORM};
  }
  VelocityPass pass;
  VulkanDispatch dispatch;
#define FORZA_NATIVE_BIND(name) dispatch.name = &name;
  FORZA_NATIVE_VULKAN_FUNCTIONS(FORZA_NATIVE_BIND)
#undef FORZA_NATIVE_BIND
  pass.Initialize(dispatch, context.physical, context.device, width, height, 2, shader, !borrowed);
  CHECK(bool(pass.output(0, 2).image) == !borrowed);
  VelocityParameters parameters;
  parameters.projection = {1.0002f, -1.0002f, 0, 0};
  for (uint32_t object = 0; object < 43; ++object) {
    for (uint32_t row = 0; row < 4; ++row)
      parameters.previous[object * 4 + row][row] = 1;
    parameters.previous[object * 4][3] = float(int(object % 3) - 1) * 0.037f;
    parameters.previous[object * 4 + 1][3] = float(int(object % 5) - 2) * 0.029f;
    if (singular) parameters.previous[object * 4 + 3] = {};
  }
  for (uint32_t row = 0; row < 4; ++row) parameters.previous[33 * 4 + row] = {};
  parameters.previous[2 * 4][2] = std::numeric_limits<float>::quiet_NaN();
  if (rotated) {
    parameters.uv_transform = {-1, -1, 1, 1};
    parameters.motion_scale = {-0.5f, 0.5f, 0, 0};
  }
  for (uint32_t repeat = 0; repeat < 3; ++repeat) {
    context.Begin();
    const auto slot = repeat % 2;
    auto invalid = parameters;
    invalid.projection[0] = std::numeric_limits<float>::quiet_NaN();
    const uint64_t submission = repeat + 1, completed = repeat;
    const auto* output = borrowed ? &external : nullptr;
    CHECK(!pass.Prepare(slot, input.view, invalid, submission, completed, output));
    CHECK(!pass.Prepare(slot, input.view, parameters, completed, completed, output));
    if (borrowed) {
      CHECK(!pass.Prepare(slot, input.view, parameters, submission, completed));
      auto wrong_format = external; wrong_format.format = VK_FORMAT_R32_SFLOAT;
      CHECK(!pass.Prepare(slot, input.view, parameters, submission, completed, &wrong_format));
      auto missing_view = external; missing_view.view = VK_NULL_HANDLE;
      CHECK(!pass.Prepare(slot, input.view, parameters, submission, completed, &missing_view));
    }
    const auto dispatch = pass.Prepare(slot, input.view, parameters, submission, completed, output);
    CHECK(dispatch.has_value());
    CHECK(dispatch->external_blur == borrowed);
    auto mismatched = *dispatch;
    mismatched.external_blur = !borrowed;
    bool rejected = false;
    try { pass.Record(context.command, mismatched); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    CHECK(!pass.Prepare(slot, input.view, parameters, submission + 1, completed, output));
    VkImageMemoryBarrier external_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    if (borrowed) {
      external_barrier.image = target->image;
      external_barrier.oldLayout = external_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      external_barrier.srcQueueFamilyIndex = external_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      external_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      external_barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      external_barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      vkCmdPipelineBarrier(context.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
          0, 0, nullptr, 0, nullptr, 1, &external_barrier);
    }
    pass.Record(context.command, *dispatch);
    if (borrowed) {
      external_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      external_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier(context.command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          0, 0, nullptr, 0, nullptr, 1, &external_barrier);
      VkBufferImageCopy copy{};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {target->width, target->height, 1};
      vkCmdCopyImageToBuffer(context.command, target->image, VK_IMAGE_LAYOUT_GENERAL, padding->buffer, 1, &copy);
    }
    // Recorded/enqueued work is not completed work. Its slot remains reserved.
    CHECK(!pass.Prepare(slot, input.view, parameters, submission + 1, completed, output));
    for (uint32_t i = 0; i < 3; ++i) {
      VkBufferImageCopy copy{};
      copy.bufferOffset = offsets[i];
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {width, height, 1};
      vkCmdCopyImageToBuffer(context.command, dispatch->images[i],
          VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &copy);
    }
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = readback.buffer; barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(context.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 0, nullptr, 1, &barrier, 0, nullptr);
    if (borrowed) {
      barrier.buffer = padding->buffer;
      vkCmdPipelineBarrier(context.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
          0, 0, nullptr, 1, &barrier, 0, nullptr);
    }
    context.Submit();
    if (borrowed) {
      const auto* retained = static_cast<const uint8_t*>(padding->mapped);
      for (uint32_t y = 0; y < target->height; ++y)
        for (uint32_t x = 0; x < target->width; ++x)
          if (x >= width || y >= height)
            CHECK(std::memcmp(retained + (size_t(y) * target->width + x) * 4,
                target->pixels[size_t(y) * target->width + x].data(), 4) == 0);
    }
    const auto* depth = static_cast<const float*>(readback.mapped);
    const auto* motion = reinterpret_cast<const float*>(static_cast<const uint8_t*>(readback.mapped) + offsets[1]);
    const auto* blur = static_cast<const uint8_t*>(readback.mapped) + offsets[2];
    float depth_error = 0, motion_error = 0;
    int blur_error = 0;
    for (uint32_t y = 0; y < height; ++y)
      for (uint32_t x = 0; x < width; ++x) {
        const float u = (float(x) + 0.5f) / width, v = (float(y) + 0.5f) / height;
        const uint64_t center_x = rotated ? 2ull * (width - x) - 1 : 2ull * x + 1;
        const uint64_t center_y = rotated ? 2ull * (height - y) - 1 : 2ull * y + 1;
        const auto ix = uint32_t(center_x * input.width / (2ull * width));
        const auto iy = uint32_t(center_y * input.height / (2ull * height));
        const auto expected = EvaluateVelocity(parameters, input.Read(ix, iy), u, v, width, height);
        const size_t p = size_t(y) * width + x;
        CHECK(std::isfinite(depth[p]) && std::isfinite(motion[p * 4]) && std::isfinite(motion[p * 4 + 1]));
        depth_error = std::max(depth_error, std::abs(depth[p] - expected.depth));
        for (uint32_t c = 0; c < 2; ++c)
          motion_error = std::max(motion_error, std::abs(motion[p * 4 + c] - expected.pixels[c]));
        for (uint32_t c = 0; c < 4; ++c)
          blur_error = std::max(blur_error, std::abs(int(blur[p * 4 + c]) - int(std::lround(expected.blur[c] * 255))));
      }
    std::cout << width << 'x' << height << ", rotated=" << rotated << ", singular=" << singular << ", borrowed=" << borrowed
        << ", slot=" << slot << ": errors depth=" << depth_error << ", motion=" << motion_error
        << " pixels, blur=" << blur_error << "/255\n";
    CHECK(depth_error < 0.000002f);
    CHECK(motion_error < 0.002f);
    CHECK(blur_error <= 1);
  }
}

struct ResolveGpu {
  Context& context;
  VkDescriptorSetLayout sets[2]{};
  VkDescriptorPool pool{};
  VkPipelineLayout layout{};
  VkPipeline pipeline{};
  VkDescriptorSet descriptors[2]{};
  explicit ResolveGpu(Context& c) : context(c) {}
  ~ResolveGpu() {
    if (pipeline) vkDestroyPipeline(context.device,pipeline,nullptr);
    if (layout) vkDestroyPipelineLayout(context.device,layout,nullptr);
    if (pool) vkDestroyDescriptorPool(context.device,pool,nullptr);
    for(auto set:sets) if(set) vkDestroyDescriptorSetLayout(context.device,set,nullptr);
  }
  void Initialize(const std::vector<uint32_t>& shader) {
    const VkDescriptorType types[]={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE};
    for(uint32_t i=0;i<2;++i) {
      VkDescriptorSetLayoutBinding binding{0,types[i],1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
      VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      info.bindingCount=1;info.pBindings=&binding;
      VK_CHECK(vkCreateDescriptorSetLayout(context.device,&info,nullptr,&sets[i]));
    }
    VkDescriptorPoolSize sizes[]={{types[0],1},{types[1],1}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets=2;pool_info.poolSizeCount=2;pool_info.pPoolSizes=sizes;
    VK_CHECK(vkCreateDescriptorPool(context.device,&pool_info,nullptr,&pool));
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool=pool;allocate.descriptorSetCount=2;allocate.pSetLayouts=sets;
    VK_CHECK(vkAllocateDescriptorSets(context.device,&allocate,descriptors));
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(ResolveParameters)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount=2;layout_info.pSetLayouts=sets;
    layout_info.pushConstantRangeCount=1;layout_info.pPushConstantRanges=&range;
    VK_CHECK(vkCreatePipelineLayout(context.device,&layout_info,nullptr,&layout));
    VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    module_info.codeSize=shader.size()*4;module_info.pCode=shader.data();VkShaderModule module;
    VK_CHECK(vkCreateShaderModule(context.device,&module_info,nullptr,&module));
    VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    info.layout=layout;info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;info.stage.module=module;info.stage.pName="main";
    const auto result=vkCreateComputePipelines(context.device,VK_NULL_HANDLE,1,&info,nullptr,&pipeline);
    vkDestroyShaderModule(context.device,module,nullptr);VK_CHECK(result);
  }
};

void RunResolve(Context& context,const std::vector<uint32_t>& shader) {
  ResolveGpu gpu(context);gpu.Initialize(shader);
  auto run=[&](uint32_t sx,uint32_t sy,uint32_t endian,bool swap,uint32_t width,uint32_t height,bool fill) {
    ResolveParameters p;
    p.source={3,5,width,height};p.destination={8,8,((width/sx+8+31)/32)*32,32};
    p.layout={sx,sy,endian,uint32_t(swap)};
    p.bounds={uint32_t(ResolveBufferBytes(p)),width+8,height+8,uint32_t(fill)};CHECK(Valid(p));
    Input input(context,p.bounds[1],p.bounds[2],endian%2!=0);
    struct View {
      VkDevice device;VkImageView value{};
      ~View(){if(value)vkDestroyImageView(device,value,nullptr);}
    } view{context.device};
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image=input.image;view_info.viewType=VK_IMAGE_VIEW_TYPE_2D;view_info.format=VK_FORMAT_R8G8B8A8_UNORM;
    if(input.swizzled) view_info.components={VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_G,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_A};
    view_info.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VK_CHECK(vkCreateImageView(context.device,&view_info,nullptr,&view.value));
    Buffer output(context,p.bounds[0],VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memset(output.mapped,0xA7,p.bounds[0]);
    std::vector<uint32_t> expected(p.bounds[0]/4,0xA7A7A7A7);
    for(uint32_t y=0;y<height;++y) for(uint32_t q=0;q<width/4;++q) {
      std::array<std::array<uint8_t,4>,4> texels;
      for(uint32_t lane=0;lane<4;++lane) {
        const auto xy=ResolveSourcePixel(p,q*4+lane,y);
        texels[lane]=input.Read(xy[0],xy[1]);
      }
      const auto encoded=EncodeResolveQuad(texels,endian,swap);
      std::memcpy(expected.data()+ResolveQuadOffset(p,q,y)/4,encoded.data(),16);
    }
    VkDescriptorBufferInfo buffer_info{output.buffer,0,p.bounds[0]};
    VkDescriptorImageInfo image_info{VK_NULL_HANDLE,view.value,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet writes[2]{};
    for(uint32_t i=0;i<2;++i) {
      writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=gpu.descriptors[i];
      writes[i].descriptorCount=1;
    }
    writes[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[0].pBufferInfo=&buffer_info;
    writes[1].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;writes[1].pImageInfo=&image_info;
    vkUpdateDescriptorSets(context.device,2,writes,0,nullptr);
    context.Begin();
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.buffer=output.buffer;barrier.size=VK_WHOLE_SIZE;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,0,nullptr,1,&barrier,0,nullptr);
    vkCmdBindPipeline(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,gpu.pipeline);
    vkCmdBindDescriptorSets(context.command,VK_PIPELINE_BIND_POINT_COMPUTE,gpu.layout,0,2,gpu.descriptors,0,nullptr);
    vkCmdPushConstants(context.command,gpu.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(p),&p);
    vkCmdDispatch(context.command,(width/4+7)/8,(height+7)/8,1);
    barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        0,0,nullptr,1,&barrier,0,nullptr);
    context.Submit();
    CHECK(std::memcmp(expected.data(),output.mapped,p.bounds[0])==0);
    std::cout<<"Resolve "<<width<<'x'<<height<<", scale="<<sx<<'x'<<sy<<", endian="<<endian
             <<", swap="<<swap<<", half-pixel fill="<<fill<<": exact output and preserved padding\n";
  };
  for(const auto& scale:std::array<std::array<uint32_t,2>,7>{{{1,1},{2,2},{3,2},{7,3},{4,6},{5,7},{1,4}}})
    for(uint32_t endian=0;endian<6;++endian) for(bool swap:{false,true}) for(bool fill:{false,true})
      run(scale[0],scale[1],endian,swap,32*scale[0],24*scale[1],fill);
  for(bool fill:{false,true}) run(2,2,2,true,2560,1440,fill);
}

int main(int argc, char** argv) {
  if (argc < 2 || argc > 4) return 2;
  bool validation=false,resolve_only=false;
  for(int i=2;i<argc;++i) {
    if(std::string(argv[i])=="--validation")validation=true;
    else if(std::string(argv[i])=="--resolve-only")resolve_only=true;
    else return 2;
  }
  try {
    std::ifstream file(argv[1], std::ios::binary | std::ios::ate);
    CHECK(file.good());
    const auto bytes = file.tellg();
    CHECK(bytes > 0 && bytes % 4 == 0);
    std::vector<uint32_t> shader(size_t(bytes) / 4);
    file.seekg(0); file.read(reinterpret_cast<char*>(shader.data()), bytes);
    CHECK(file.good());
    Context context;
    if (!context.Initialize(validation)) {
      if (validation) throw std::runtime_error("Could not start requested Vulkan validation");
      std::cout << "No compatible Vulkan device\n"; return 77;
    }
    if(resolve_only) {
      RunResolve(context,shader);CHECK(context.validation_errors==0);
      std::cout<<"Native resolve GPU checks passed\n";return 0;
    }
    Input input(context);
    Run(context, input, shader, 73, 41, false, false);
    Run(context, input, shader, 73, 41, true, false);
    Run(context, input, shader, 73, 41, true, true);
    Run(context, input, shader, 2560, 1440, false, false);
    Run(context, input, shader, 2560, 1440, true, false);
    Input swizzled(context, 146, 82, true);
    Run(context, swizzled, shader, 73, 41, false, false);
    Run(context, swizzled, shader, 73, 41, true, false);
    Run(context, input, shader, 73, 41, false, false, true);
    Run(context, swizzled, shader, 73, 41, true, true, true);
    Run(context, input, shader, 2560, 1440, false, false, true);
    CHECK(context.validation_errors == 0);
    std::cout << "Native velocity GPU checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
