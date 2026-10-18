#include "velocity_pass.h"

#include <cstring>
#include <string>

namespace forza::native_renderer {
namespace {
void Require(VkResult result, const char* operation) {
  if (result != VK_SUCCESS)
    throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageLayout old_layout,
    VkImageLayout new_layout, VkAccessFlags src, VkAccessFlags dst) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.image = image;
  barrier.oldLayout = old_layout;
  barrier.newLayout = new_layout;
  barrier.srcAccessMask = src;
  barrier.dstAccessMask = dst;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return barrier;
}
}  // namespace

VelocityPass::~VelocityPass() {
  if (!device_) return;
  // The owner drains recording jobs and GPU submissions before destruction.
  // Waiting on an externally owned device here would race other queue users.
  if (pipeline_) functions_.vkDestroyPipeline(device_, pipeline_, nullptr);
  if (pipeline_layout_) functions_.vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
  if (descriptor_pool_) functions_.vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
  if (descriptor_layout_) functions_.vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
  if (sampler_) functions_.vkDestroySampler(device_, sampler_, nullptr);
  for (auto& frame : frames_) {
    for (auto& image : frame.images) {
      if (image.view) functions_.vkDestroyImageView(device_, image.view, nullptr);
      if (image.image) functions_.vkDestroyImage(device_, image.image, nullptr);
      if (image.memory) functions_.vkFreeMemory(device_, image.memory, nullptr);
    }
    if (frame.mapped) functions_.vkUnmapMemory(device_, frame.memory);
    if (frame.constants) functions_.vkDestroyBuffer(device_, frame.constants, nullptr);
    if (frame.memory) functions_.vkFreeMemory(device_, frame.memory, nullptr);
  }
}

uint32_t VelocityPass::MemoryType(uint32_t bits, VkMemoryPropertyFlags required) const {
  VkPhysicalDeviceMemoryProperties properties;
  functions_.vkGetPhysicalDeviceMemoryProperties(physical_, &properties);
  for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & required) == required)
      return i;
  throw std::runtime_error("No compatible native velocity memory type");
}

void VelocityPass::Initialize(const VulkanDispatch& dispatch, VkPhysicalDevice physical, VkDevice device,
    uint32_t width, uint32_t height, uint32_t slots, const std::vector<uint32_t>& spirv) {
  if (device_ || !dispatch.Valid() || !physical || !device || !width || !height || !slots || slots > 16 ||
      spirv.empty() || spirv.front() != 0x07230203)
    throw std::invalid_argument("Invalid native velocity configuration");
  functions_ = dispatch;
  VkPhysicalDeviceProperties properties;
  functions_.vkGetPhysicalDeviceProperties(physical, &properties);
  if (width > 32768 || height > 32768 || width > properties.limits.maxImageDimension2D ||
      height > properties.limits.maxImageDimension2D)
    throw std::invalid_argument("Native velocity extent exceeds device limits");
  physical_ = physical; device_ = device; width_ = width; height_ = height;
  frames_.resize(slots);

  const VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  descriptors.bindingCount = 5; descriptors.pBindings = bindings;
  Require(functions_.vkCreateDescriptorSetLayout(device_, &descriptors, nullptr, &descriptor_layout_), "descriptor layout");
  const VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, slots},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 * slots},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, slots}};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = slots; pool.poolSizeCount = 3; pool.pPoolSizes = sizes;
  Require(functions_.vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_), "descriptor pool");
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount = 1; layout.pSetLayouts = &descriptor_layout_;
  Require(functions_.vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_), "pipeline layout");
  VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module.codeSize = spirv.size() * sizeof(uint32_t); module.pCode = spirv.data();
  VkShaderModule shader;
  Require(functions_.vkCreateShaderModule(device_, &module, nullptr, &shader), "shader module");
  VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline.layout = pipeline_layout_;
  pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  pipeline.stage.module = shader; pipeline.stage.pName = "main";
  const auto result = functions_.vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_);
  functions_.vkDestroyShaderModule(device_, shader, nullptr);
  Require(result, "compute pipeline");
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
  sampler.maxLod = 0;
  sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  Require(functions_.vkCreateSampler(device_, &sampler, nullptr, &sampler_), "point sampler");

  for (auto& frame : frames_) {
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptor_pool_; allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &descriptor_layout_;
    Require(functions_.vkAllocateDescriptorSets(device_, &allocate, &frame.descriptors), "descriptor set");
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = sizeof(VelocityParameters); buffer.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    Require(functions_.vkCreateBuffer(device_, &buffer, nullptr, &frame.constants), "constant buffer");
    VkMemoryRequirements requirements;
    functions_.vkGetBufferMemoryRequirements(device_, frame.constants, &requirements);
    VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    memory.allocationSize = requirements.size;
    memory.memoryTypeIndex = MemoryType(requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Require(functions_.vkAllocateMemory(device_, &memory, nullptr, &frame.memory), "constant memory");
    Require(functions_.vkBindBufferMemory(device_, frame.constants, frame.memory, 0), "constant binding");
    Require(functions_.vkMapMemory(device_, frame.memory, 0, sizeof(VelocityParameters), 0, &frame.mapped), "constant mapping");
    constexpr VkFormat formats[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
                                    VK_FORMAT_R8G8B8A8_UNORM};
    for (uint32_t index = 0; index < 3; ++index) {
      auto& image = frame.images[index]; image.format = formats[index];
      VkFormatProperties format;
      functions_.vkGetPhysicalDeviceFormatProperties(physical_, image.format, &format);
      constexpr auto required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
          VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
      if ((format.optimalTilingFeatures & required) != required)
        throw std::runtime_error("Native velocity storage format unsupported");
      VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      info.imageType = VK_IMAGE_TYPE_2D; info.format = image.format;
      info.extent = {width, height, 1}; info.mipLevels = info.arrayLayers = 1;
      info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
      info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      Require(functions_.vkCreateImage(device_, &info, nullptr, &image.image), "native attachment");
      functions_.vkGetImageMemoryRequirements(device_, image.image, &requirements);
      memory.allocationSize = requirements.size;
      memory.memoryTypeIndex = MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      Require(functions_.vkAllocateMemory(device_, &memory, nullptr, &image.memory), "attachment memory");
      Require(functions_.vkBindImageMemory(device_, image.image, image.memory, 0), "attachment binding");
      VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.image = image.image; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = image.format;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      Require(functions_.vkCreateImageView(device_, &view, nullptr, &image.view), "attachment view");
    }
  }
}

std::optional<VelocityPass::Dispatch> VelocityPass::Prepare(uint32_t slot,
    VkImageView packed_depth, const VelocityParameters& parameters,
    uint64_t submission, uint64_t completed_submission) {
  auto& frame = frames_.at(slot);
  if (!packed_depth || !Valid(parameters) || submission <= completed_submission ||
      submission < last_submission_ || completed_submission < last_completed_ ||
      frame.submission > completed_submission)
    return {};
  std::memcpy(frame.mapped, &parameters, sizeof(parameters));
  const VkDescriptorImageInfo images[] = {
      {sampler_, packed_depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, frame.images[0].view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, frame.images[1].view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, frame.images[2].view, VK_IMAGE_LAYOUT_GENERAL}};
  const VkDescriptorBufferInfo buffer{frame.constants, 0, sizeof(parameters)};
  VkWriteDescriptorSet writes[5]{};
  for (uint32_t i = 0; i < 5; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = frame.descriptors; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
    writes[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
        i == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    if (i == 4) writes[i].pBufferInfo = &buffer; else writes[i].pImageInfo = &images[i];
  }
  functions_.vkUpdateDescriptorSets(device_, 5, writes, 0, nullptr);
  Dispatch dispatch;
  dispatch.descriptors = frame.descriptors;
  dispatch.first_use = !frame.initialized;
  for (uint32_t i = 0; i < 3; ++i) dispatch.images[i] = frame.images[i].image;
  frame.initialized = true;
  frame.submission = submission;
  last_submission_ = submission;
  last_completed_ = completed_submission;
  return dispatch;
}

void VelocityPass::Record(VkCommandBuffer command, const Dispatch& dispatch) const {
  if (!command || !dispatch.descriptors)
    throw std::invalid_argument("Native velocity dispatch must be prepared before recording");
  VkImageMemoryBarrier barriers[3];
  for (uint32_t i = 0; i < 3; ++i)
    barriers[i] = ImageBarrier(dispatch.images[i],
        dispatch.first_use ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL, dispatch.first_use ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_SHADER_WRITE_BIT);
  functions_.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0, 0, nullptr, 0, nullptr, 3, barriers);
  functions_.vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
  functions_.vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_,
      0, 1, &dispatch.descriptors, 0, nullptr);
  functions_.vkCmdDispatch(command, (width_ + 7) / 8, (height_ + 7) / 8, 1);
  for (uint32_t i = 0; i < 3; ++i)
    barriers[i] = ImageBarrier(dispatch.images[i], VK_IMAGE_LAYOUT_GENERAL,
        VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
  functions_.vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
      0, 0, nullptr, 0, nullptr, 3, barriers);
}

const VelocityPass::Output& VelocityPass::output(uint32_t slot, uint32_t attachment) const {
  return frames_.at(slot).images.at(attachment);
}

}  // namespace forza::native_renderer
