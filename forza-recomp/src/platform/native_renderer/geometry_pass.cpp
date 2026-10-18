#include "geometry_pass.h"
#include <cstring>
#include <stdexcept>
#include <string>

namespace forza::native_renderer {
namespace {
void Check(VkResult result, const char* operation) {
  if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation)+": "+std::to_string(result));
}
}

GeometryPass::~GeometryPass() {
  if (!device_) return;
  if (pipeline_) functions_.vkDestroyPipeline(device_,pipeline_,nullptr);
  if (pipeline_layout_) functions_.vkDestroyPipelineLayout(device_,pipeline_layout_,nullptr);
  if (descriptor_pool_) functions_.vkDestroyDescriptorPool(device_,descriptor_pool_,nullptr);
  if (descriptor_layout_) functions_.vkDestroyDescriptorSetLayout(device_,descriptor_layout_,nullptr);
  for (auto& frame : frames_) for (auto* buffer : {&frame.vertices,&frame.indices}) {
    if (buffer->mapped) functions_.vkUnmapMemory(device_,buffer->memory);
    if (buffer->buffer) functions_.vkDestroyBuffer(device_,buffer->buffer,nullptr);
    if (buffer->memory) functions_.vkFreeMemory(device_,buffer->memory,nullptr);
  }
}

void GeometryPass::Allocate(Buffer& buffer, uint32_t bytes, VkBufferUsageFlags usage) {
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size=bytes;info.usage=usage;
  Check(functions_.vkCreateBuffer(device_,&info,nullptr,&buffer.buffer),"geometry buffer");
  VkMemoryRequirements requirements;
  functions_.vkGetBufferMemoryRequirements(device_,buffer.buffer,&requirements);
  VkPhysicalDeviceMemoryProperties memory;
  functions_.vkGetPhysicalDeviceMemoryProperties(physical_,&memory);
  uint32_t type=memory.memoryTypeCount;
  constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for(uint32_t i=0;i<memory.memoryTypeCount;++i)
    if((requirements.memoryTypeBits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags) {type=i;break;}
  if(type==memory.memoryTypeCount)throw std::runtime_error("No coherent geometry upload memory");
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=type;
  Check(functions_.vkAllocateMemory(device_,&allocation,nullptr,&buffer.memory),"geometry memory");
  Check(functions_.vkBindBufferMemory(device_,buffer.buffer,buffer.memory,0),"geometry binding");
  Check(functions_.vkMapMemory(device_,buffer.memory,0,bytes,0,&buffer.mapped),"geometry mapping");
}

void GeometryPass::Initialize(const GeometryVulkanDispatch& functions, VkPhysicalDevice physical,
    VkDevice device, const Config& config, const std::vector<uint32_t>& shader) {
  if(device_ || !functions.Valid() || !physical || !device || !config.render_pass || !config.slots ||
      config.slots>64 || config.vertex_capacity<8 || config.vertex_capacity%4 ||
      config.vertex_capacity>16*1024*1024 || config.index_capacity<6 || config.index_capacity%4 ||
      config.index_capacity>16*1024*1024 || shader.empty() || shader[0]!=0x07230203)
    throw std::invalid_argument("Invalid native geometry configuration");
  VkPhysicalDeviceProperties properties;
  functions.vkGetPhysicalDeviceProperties(physical,&properties);
  if(properties.limits.maxPushConstantsSize<sizeof(GeometryParameters) ||
      config.vertex_capacity>properties.limits.maxStorageBufferRange)
    throw std::invalid_argument("Native geometry exceeds device limits");
  functions_=functions;physical_=physical;device_=device;config_=config;limits_=properties.limits;
  frames_.resize(config.slots);
  VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr};
  VkDescriptorSetLayoutCreateInfo descriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  descriptor.bindingCount=1;descriptor.pBindings=&binding;
  Check(functions_.vkCreateDescriptorSetLayout(device_,&descriptor,nullptr,&descriptor_layout_),"geometry descriptors");
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,config.slots};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets=config.slots;pool.poolSizeCount=1;pool.pPoolSizes=&size;
  Check(functions_.vkCreateDescriptorPool(device_,&pool,nullptr,&descriptor_pool_),"geometry descriptor pool");
  VkPushConstantRange constants{VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(GeometryParameters)};
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout.setLayoutCount=1;layout.pSetLayouts=&descriptor_layout_;
  layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&constants;
  Check(functions_.vkCreatePipelineLayout(device_,&layout,nullptr,&pipeline_layout_),"geometry pipeline layout");
  VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module_info.codeSize=shader.size()*4;module_info.pCode=shader.data();VkShaderModule module;
  Check(functions_.vkCreateShaderModule(device_,&module_info,nullptr,&module),"geometry vertex shader");
  VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stage.stage=VK_SHADER_STAGE_VERTEX_BIT;stage.module=module;stage.pName="main";
  VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology=config.triangle_strip?VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  assembly.primitiveRestartEnable=config.triangle_strip;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount=viewport.scissorCount=1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;
  raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
  VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depth.depthTestEnable=depth.depthWriteEnable=VK_TRUE;
  depth.depthCompareOp=config.reverse_depth?VK_COMPARE_OP_GREATER_OR_EQUAL:VK_COMPARE_OP_LESS_OR_EQUAL;
  depth.stencilTestEnable=VK_TRUE;
  depth.front={VK_STENCIL_OP_KEEP,VK_STENCIL_OP_REPLACE,VK_STENCIL_OP_KEEP,VK_COMPARE_OP_ALWAYS,255,255,0};
  depth.back=depth.front;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  const VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR,VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount=3;dynamic.pDynamicStates=states;
  VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipeline.stageCount=1;pipeline.pStages=&stage;pipeline.pVertexInputState=&input;
  pipeline.pInputAssemblyState=&assembly;pipeline.pViewportState=&viewport;
  pipeline.pRasterizationState=&raster;pipeline.pMultisampleState=&samples;
  pipeline.pDepthStencilState=&depth;pipeline.pColorBlendState=&blend;pipeline.pDynamicState=&dynamic;
  pipeline.layout=pipeline_layout_;pipeline.renderPass=config.render_pass;
  const auto result=functions_.vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&pipeline,nullptr,&pipeline_);
  functions_.vkDestroyShaderModule(device_,module,nullptr);Check(result,"geometry graphics pipeline");
  for(auto& frame:frames_) {
    Allocate(frame.vertices,config.vertex_capacity,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    Allocate(frame.indices,config.index_capacity,VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool=descriptor_pool_;allocate.descriptorSetCount=1;allocate.pSetLayouts=&descriptor_layout_;
    Check(functions_.vkAllocateDescriptorSets(device_,&allocate,&frame.descriptors),"geometry descriptor allocation");
    VkDescriptorBufferInfo buffer{frame.vertices.buffer,0,config.vertex_capacity};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet=frame.descriptors;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo=&buffer;functions_.vkUpdateDescriptorSets(device_,1,&write,0,nullptr);
  }
  initialized_=true;
}

std::optional<GeometryPass::Dispatch> GeometryPass::Prepare(uint32_t slot, const Mesh& mesh,
    const GeometryParameters& parameters, const Raster& raster, uint64_t submission, uint64_t completed) {
  if(!initialized_ || slot>=frames_.size() || !Valid(parameters) || !mesh.vertices || !mesh.indices ||
      !mesh.vertex_bytes || mesh.vertex_bytes>config_.vertex_capacity ||
      uint64_t(parameters.layout[0])*parameters.layout[2]*4!=mesh.vertex_bytes ||
      (mesh.index_width!=2 && mesh.index_width!=4) || mesh.index_bytes<3*mesh.index_width ||
      mesh.index_bytes>config_.index_capacity || mesh.index_bytes%mesh.index_width ||
      !submission || completed>=submission || submission<last_submission_ || completed<last_completed_ ||
      frames_[slot].submission>completed || raster.stencil_reference>255 ||
      !std::isfinite(raster.viewport.x) || !std::isfinite(raster.viewport.y) ||
      !std::isfinite(raster.viewport.width) || !std::isfinite(raster.viewport.height) ||
      raster.viewport.width<=0 || raster.viewport.width>32768 || raster.viewport.height<=0 || raster.viewport.height>32768 ||
      raster.viewport.width>limits_.maxViewportDimensions[0] || raster.viewport.height>limits_.maxViewportDimensions[1] ||
      raster.viewport.x<limits_.viewportBoundsRange[0] || raster.viewport.y<limits_.viewportBoundsRange[0] ||
      raster.viewport.x+raster.viewport.width>limits_.viewportBoundsRange[1] ||
      raster.viewport.y+raster.viewport.height>limits_.viewportBoundsRange[1] ||
      !(raster.viewport.minDepth>=0 && raster.viewport.maxDepth<=1 && raster.viewport.minDepth<=raster.viewport.maxDepth) ||
      raster.scissor.offset.x<0 || raster.scissor.offset.y<0 || !raster.scissor.extent.width || !raster.scissor.extent.height ||
      uint64_t(raster.scissor.offset.x)+raster.scissor.extent.width>INT32_MAX ||
      uint64_t(raster.scissor.offset.y)+raster.scissor.extent.height>INT32_MAX)
    return {};
  for(uint32_t offset=0;offset<mesh.index_bytes;offset+=mesh.index_width) {
    uint32_t index=0;std::memcpy(&index,static_cast<const uint8_t*>(mesh.indices)+offset,mesh.index_width);
    if(config_.triangle_strip && index==(mesh.index_width==2?0xffffu:0xffffffffu))continue;
    if(mesh.index_width==4 && !config_.full_index_uint32 && index>0xffffff)return {};
    index=GeometryEndian(index,parameters.layout[3]);
    if(index>=parameters.layout[2])return {};
  }
  auto& frame=frames_[slot];
  std::memcpy(frame.vertices.mapped,mesh.vertices,mesh.vertex_bytes);
  std::memcpy(frame.indices.mapped,mesh.indices,mesh.index_bytes);
  frame.submission=submission;last_submission_=submission;last_completed_=completed;
  Dispatch draw;draw.owner=this;draw.descriptors=frame.descriptors;draw.indices=frame.indices.buffer;
  draw.index_type=mesh.index_width==2?VK_INDEX_TYPE_UINT16:VK_INDEX_TYPE_UINT32;
  draw.count=mesh.index_bytes/mesh.index_width;draw.parameters=parameters;draw.raster=raster;
  return draw;
}

void GeometryPass::Record(VkCommandBuffer command, const Dispatch& draw) const {
  if(!command || draw.owner!=this)throw std::invalid_argument("Invalid native geometry dispatch");
  functions_.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_);
  functions_.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout_,0,1,&draw.descriptors,0,nullptr);
  functions_.vkCmdPushConstants(command,pipeline_layout_,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(draw.parameters),&draw.parameters);
  functions_.vkCmdSetViewport(command,0,1,&draw.raster.viewport);
  functions_.vkCmdSetScissor(command,0,1,&draw.raster.scissor);
  functions_.vkCmdSetStencilReference(command,VK_STENCIL_FACE_FRONT_AND_BACK,draw.raster.stencil_reference);
  functions_.vkCmdBindIndexBuffer(command,draw.indices,0,draw.index_type);
  functions_.vkCmdDrawIndexed(command,draw.count,1,0,0,0);
}
}  // namespace forza::native_renderer
