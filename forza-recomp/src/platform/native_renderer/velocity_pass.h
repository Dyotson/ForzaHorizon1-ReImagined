#pragma once

#include "velocity.h"
#include "vulkan_dispatch.h"
#include <optional>
#include <vector>

namespace forza::native_renderer {

// An owned native compute pass. The caller supplies a compatible device/queue
// and a sampled RGBA8 2D-array view in SHADER_READ_ONLY_OPTIMAL, made visible to
// compute. Outputs remain GENERAL, visible to compute/transfer consumers.
// Prepare runs on the producer thread; Record consumes its immutable snapshot on
// the submission thread. Completion IDs must come from waited GPU fences, never
// from merely enqueuing a submission. Consumers must restore output GENERAL
// layout before slot reuse. The pass must outlive every recorded dispatch.
// A prepared dispatch must be recorded/submitted; abandoning a command stream
// requires destroying the pass after the device is idle and recreating it.
class VelocityPass {
 public:
  struct Output {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
  };
  struct Dispatch {
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    std::array<VkImage, 3> images{};
    bool first_use = false;
  };
  VelocityPass() = default;
  ~VelocityPass();
  VelocityPass(const VelocityPass&) = delete;
  VelocityPass& operator=(const VelocityPass&) = delete;

  void Initialize(const VulkanDispatch& dispatch, VkPhysicalDevice physical, VkDevice device, uint32_t width,
                  uint32_t height, uint32_t slots, const std::vector<uint32_t>& spirv);
  std::optional<Dispatch> Prepare(uint32_t slot, VkImageView packed_depth,
      const VelocityParameters& parameters, uint64_t submission, uint64_t completed_submission);
  void Record(VkCommandBuffer command, const Dispatch& dispatch) const;
  const Output& output(uint32_t slot, uint32_t attachment) const;
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

 private:
  struct Image : Output { VkDeviceMemory memory = VK_NULL_HANDLE; };
  struct Frame {
    std::array<Image, 3> images{};
    VkBuffer constants = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    uint64_t submission = 0;
    bool initialized = false;
  };
  uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags required) const;
  VulkanDispatch functions_;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t width_ = 0, height_ = 0;
  uint64_t last_submission_ = 0, last_completed_ = 0;
  VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  std::vector<Frame> frames_;
};

}  // namespace forza::native_renderer
