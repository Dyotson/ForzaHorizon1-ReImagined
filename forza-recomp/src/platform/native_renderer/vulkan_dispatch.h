#pragma once
#include <vulkan/vulkan.h>

// The renderer can use the game's existing Vulkan loader without opening a
// second loader or relying on global Vulkan symbols inside the runtime SDK.
#define FORZA_NATIVE_VULKAN_FUNCTIONS(X) \
  X(vkAllocateDescriptorSets) \
  X(vkAllocateMemory) \
  X(vkBindBufferMemory) \
  X(vkBindImageMemory) \
  X(vkCmdBindDescriptorSets) \
  X(vkCmdBindPipeline) \
  X(vkCmdDispatch) \
  X(vkCmdPipelineBarrier) \
  X(vkCreateBuffer) \
  X(vkCreateComputePipelines) \
  X(vkCreateDescriptorPool) \
  X(vkCreateDescriptorSetLayout) \
  X(vkCreateImage) \
  X(vkCreateImageView) \
  X(vkCreatePipelineLayout) \
  X(vkCreateSampler) \
  X(vkCreateShaderModule) \
  X(vkDestroyBuffer) \
  X(vkDestroyDescriptorPool) \
  X(vkDestroyDescriptorSetLayout) \
  X(vkDestroyImage) \
  X(vkDestroyImageView) \
  X(vkDestroyPipeline) \
  X(vkDestroyPipelineLayout) \
  X(vkDestroySampler) \
  X(vkDestroyShaderModule) \
  X(vkDeviceWaitIdle) \
  X(vkFreeMemory) \
  X(vkGetBufferMemoryRequirements) \
  X(vkGetImageMemoryRequirements) \
  X(vkGetPhysicalDeviceFormatProperties) \
  X(vkGetPhysicalDeviceMemoryProperties) \
  X(vkGetPhysicalDeviceProperties) \
  X(vkMapMemory) \
  X(vkUnmapMemory) \
  X(vkUpdateDescriptorSets)

namespace forza::native_renderer {
struct VulkanDispatch {
#define FORZA_NATIVE_DECLARE(name) PFN_##name name = nullptr;
  FORZA_NATIVE_VULKAN_FUNCTIONS(FORZA_NATIVE_DECLARE)
#undef FORZA_NATIVE_DECLARE
  bool Valid() const {
#define FORZA_NATIVE_REQUIRE(name) if (!name) return false;
    FORZA_NATIVE_VULKAN_FUNCTIONS(FORZA_NATIVE_REQUIRE)
#undef FORZA_NATIVE_REQUIRE
    return true;
  }
};
}  // namespace forza::native_renderer
