#pragma once
#include "geometry_pass.h"
#include "world_color.h"

namespace forza::native_renderer {
// A complete world-color finishing pass. Input images are imported read-only,
// with their shader-visible swizzles already applied. Scene/noise/bloom are
// single-mip 2D-array views; grading is a single-mip 3D view. All are visible to
// graphics in SHADER_READ_ONLY_OPTIMAL. The exposure view contains one float.
// The caller imports a single-sample color attachment and preserves its padding,
// transitions and lifetime. Depth/stencil are retained without testing/writing.
// Source quads are snapshotted on the GPU at their original position in the
// command stream. Parameters/descriptors/quad slots live through waited fences.
// Drain recording and GPU work before destruction. No device-idle wait is added.
class WorldColorPass {
 public:
  struct Config {
    VkRenderPass render_pass=VK_NULL_HANDLE;
    VkFormat color_format=VK_FORMAT_UNDEFINED,depth_format=VK_FORMAT_UNDEFINED;
    uint32_t slots=0;
    bool dynamic_rendering=false;
  };
  struct Images {VkImageView exposure{},scene{},noise{},bloom{},grading{};};
  struct Source {VkBuffer buffer{};VkDeviceSize offset=0,buffer_bytes=0;};
  struct Dispatch {
    const WorldColorPass* owner=nullptr;
    VkDescriptorSet descriptors{};
    VkBuffer source{},quad{};
    VkDeviceSize offset=0;
    VkViewport viewport{};
    VkRect2D scissor{};
  };
  WorldColorPass()=default;
  ~WorldColorPass();
  WorldColorPass(const WorldColorPass&)=delete;
  WorldColorPass& operator=(const WorldColorPass&)=delete;
  void Initialize(const GeometryVulkanDispatch&,VkPhysicalDevice,VkDevice,
      const Config&,const std::vector<uint32_t>& vertex,const std::vector<uint32_t>& fragment);
  std::optional<Dispatch> Prepare(uint32_t slot,const Source&,const Images&,
      const WorldColorParameters&,VkViewport,VkRect2D,uint64_t submission,uint64_t completed);
  void Upload(VkCommandBuffer,const Dispatch&) const;
  void Draw(VkCommandBuffer,const Dispatch&) const;
 private:
  struct Buffer {VkBuffer buffer{};VkDeviceMemory memory{};void* mapped=nullptr;};
  struct Frame {Buffer quad,parameters;VkDescriptorSet descriptors{};uint64_t submission=0;};
  void Allocate(Buffer&,uint32_t,VkBufferUsageFlags);
  GeometryVulkanDispatch f_;
  VkPhysicalDevice physical_{};VkDevice device_{};
  VkPhysicalDeviceLimits limits_{};
  VkDescriptorSetLayout descriptors_{};VkDescriptorPool pool_{};
  VkPipelineLayout layout_{};VkPipeline pipeline_{};
  VkSampler linear_{},point_{};
  Buffer indices_;
  std::vector<Frame> frames_;
  uint64_t last_submission_=0,last_completed_=0;
};
} // namespace forza::native_renderer
