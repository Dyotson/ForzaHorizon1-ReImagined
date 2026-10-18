#pragma once
#include "geometry.h"
#include "vulkan_dispatch.h"
#include <optional>
#include <vector>

#define FORZA_NATIVE_GEOMETRY_VULKAN_FUNCTIONS(X) \
  X(vkCreateGraphicsPipelines) \
  X(vkCmdBindIndexBuffer) \
  X(vkCmdDrawIndexed) \
  X(vkCmdPushConstants) \
  X(vkCmdSetViewport) \
  X(vkCmdSetScissor) \
  X(vkCmdSetStencilReference)

namespace forza::native_renderer {
struct GeometryVulkanDispatch : VulkanDispatch {
#define FORZA_NATIVE_GEOMETRY_DECLARE(name) PFN_##name name = nullptr;
  FORZA_NATIVE_GEOMETRY_VULKAN_FUNCTIONS(FORZA_NATIVE_GEOMETRY_DECLARE)
#undef FORZA_NATIVE_GEOMETRY_DECLARE
  bool Valid() const {
    if (!VulkanDispatch::Valid()) return false;
#define FORZA_NATIVE_GEOMETRY_REQUIRE(name) if (!name) return false;
    FORZA_NATIVE_GEOMETRY_VULKAN_FUNCTIONS(FORZA_NATIVE_GEOMETRY_REQUIRE)
#undef FORZA_NATIVE_GEOMETRY_REQUIRE
    return true;
  }
};

// An authored rigid-mesh depth/stencil draw. It owns immutable vertex/index
// snapshots per slot. The caller owns a compatible, single-sample depth/stencil
// render pass with no color targets, its attachments, layouts and lifetime.
// Prepare is producer-only; Record consumes an immutable token inside that pass
// on the recording thread. Uploads precede queue submission. Slots may be reused
// only after waited completion fences. Drain recording jobs and GPU work before
// destruction; this pass never waits on the externally owned device.
// A prepared token belongs to its reserved submission. Do not retain it for
// later recording after that submission has completed and its slot is reused.
class GeometryPass {
 public:
  struct Config {
    VkRenderPass render_pass = VK_NULL_HANDLE;
    uint32_t slots = 0, vertex_capacity = 0, index_capacity = 0;
    bool triangle_strip = true, reverse_depth = true;
    bool full_index_uint32 = false; // caller enabled fullDrawIndexUint32 on this device
  };
  struct Mesh {
    const void* vertices = nullptr;
    uint32_t vertex_bytes = 0;
    const void* indices = nullptr;
    uint32_t index_bytes = 0, index_width = 2;
  };
  struct Raster {
    VkViewport viewport{};
    VkRect2D scissor{};
    uint32_t stencil_reference = 0;
  };
  class Dispatch {
   public:
    Dispatch() = default;
   private:
    friend class GeometryPass;
    const GeometryPass* owner = nullptr;
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    VkBuffer indices = VK_NULL_HANDLE;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    uint32_t count = 0;
    GeometryParameters parameters;
    Raster raster;
  };
  GeometryPass() = default;
  ~GeometryPass();
  GeometryPass(const GeometryPass&) = delete;
  GeometryPass& operator=(const GeometryPass&) = delete;
  void Initialize(const GeometryVulkanDispatch& functions, VkPhysicalDevice physical, VkDevice device,
                  const Config& config, const std::vector<uint32_t>& vertex_shader);
  std::optional<Dispatch> Prepare(uint32_t slot, const Mesh& mesh, const GeometryParameters& parameters,
      const Raster& raster, uint64_t submission, uint64_t completed_submission);
  void Record(VkCommandBuffer command, const Dispatch& dispatch) const;
 private:
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
  };
  struct Frame {
    Buffer vertices, indices;
    VkDescriptorSet descriptors = VK_NULL_HANDLE;
    uint64_t submission = 0;
  };
  void Allocate(Buffer& buffer, uint32_t bytes, VkBufferUsageFlags usage);
  GeometryVulkanDispatch functions_;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  Config config_;
  VkPhysicalDeviceLimits limits_{};
  bool initialized_ = false;
  std::vector<Frame> frames_;
  uint64_t last_submission_ = 0, last_completed_ = 0;
};
}  // namespace forza::native_renderer
