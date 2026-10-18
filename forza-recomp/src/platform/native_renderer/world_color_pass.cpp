#include "world_color_pass.h"
#include <cstring>
#include <stdexcept>
#include <string>
namespace forza::native_renderer {
namespace {
void Require(VkResult value,const char* operation) {
  if(value!=VK_SUCCESS)throw std::runtime_error(std::string("World color ")+operation+": "+std::to_string(value));
}
}
WorldColorPass::~WorldColorPass() {
  if(!device_)return;
  if(pipeline_)f_.vkDestroyPipeline(device_,pipeline_,nullptr);
  if(layout_)f_.vkDestroyPipelineLayout(device_,layout_,nullptr);
  if(pool_)f_.vkDestroyDescriptorPool(device_,pool_,nullptr);
  if(descriptors_)f_.vkDestroyDescriptorSetLayout(device_,descriptors_,nullptr);
  if(linear_)f_.vkDestroySampler(device_,linear_,nullptr);
  if(point_)f_.vkDestroySampler(device_,point_,nullptr);
  auto release=[&](Buffer& b){
    if(b.mapped)f_.vkUnmapMemory(device_,b.memory);
    if(b.buffer)f_.vkDestroyBuffer(device_,b.buffer,nullptr);
    if(b.memory)f_.vkFreeMemory(device_,b.memory,nullptr);
  };
  release(indices_);
  for(auto& frame:frames_){release(frame.quad);release(frame.parameters);}
}
void WorldColorPass::Allocate(Buffer& b,uint32_t bytes,VkBufferUsageFlags usage) {
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=bytes;info.usage=usage;
  Require(f_.vkCreateBuffer(device_,&info,nullptr,&b.buffer),"buffer");
  VkMemoryRequirements requirements;f_.vkGetBufferMemoryRequirements(device_,b.buffer,&requirements);
  VkPhysicalDeviceMemoryProperties memory;f_.vkGetPhysicalDeviceMemoryProperties(physical_,&memory);
  uint32_t type=memory.memoryTypeCount;
  constexpr auto flags=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for(uint32_t i=0;i<memory.memoryTypeCount;++i)
    if((requirements.memoryTypeBits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags){type=i;break;}
  if(type==memory.memoryTypeCount)throw std::runtime_error("No coherent world-color buffer memory");
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocate.allocationSize=requirements.size;allocate.memoryTypeIndex=type;
  Require(f_.vkAllocateMemory(device_,&allocate,nullptr,&b.memory),"memory");
  Require(f_.vkBindBufferMemory(device_,b.buffer,b.memory,0),"buffer binding");
  Require(f_.vkMapMemory(device_,b.memory,0,bytes,0,&b.mapped),"mapping");
}
void WorldColorPass::Initialize(const GeometryVulkanDispatch& functions,VkPhysicalDevice physical,
    VkDevice device,const Config& config,const std::vector<uint32_t>& vertex,const std::vector<uint32_t>& fragment) {
  if(device_ || !functions.Valid() || !physical || !device || !config.slots || config.slots>64 ||
      (!config.render_pass && !config.dynamic_rendering) || config.color_format==VK_FORMAT_UNDEFINED ||
      vertex.empty() || fragment.empty() || vertex[0]!=0x07230203 || fragment[0]!=0x07230203)
    throw std::invalid_argument("Invalid world-color configuration");
  f_=functions;physical_=physical;device_=device;
  VkPhysicalDeviceProperties properties;f_.vkGetPhysicalDeviceProperties(physical,&properties);limits_=properties.limits;
  if(limits_.maxUniformBufferRange<sizeof(WorldColorParameters))throw std::invalid_argument("World-color uniform limit");
  frames_.resize(config.slots);
  VkDescriptorSetLayoutBinding bindings[8]{};
  for(uint32_t i=0;i<8;++i){bindings[i].binding=i;bindings[i].descriptorCount=1;
    bindings[i].descriptorType=i==0?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:i==1?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[i].stageFlags=i==0 || i==2?VK_SHADER_STAGE_VERTEX_BIT:i==1?VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo descriptor{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};descriptor.bindingCount=8;descriptor.pBindings=bindings;
  Require(f_.vkCreateDescriptorSetLayout(device,&descriptor,nullptr,&descriptors_),"descriptor layout");
  VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,config.slots},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,config.slots},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,6*config.slots}};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=config.slots;pool.poolSizeCount=3;pool.pPoolSizes=sizes;
  Require(f_.vkCreateDescriptorPool(device,&pool,nullptr,&pool_),"descriptor pool");
  VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=1;layout.pSetLayouts=&descriptors_;
  Require(f_.vkCreatePipelineLayout(device,&layout,nullptr,&layout_),"pipeline layout");
  VkShaderModule modules[2]{};VkPipelineShaderStageCreateInfo stages[2]{};
  try {
    for(uint32_t i=0;i<2;++i){const auto& code=i?fragment:vertex;
      VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};module.codeSize=code.size()*4;module.pCode=code.data();
      Require(f_.vkCreateShaderModule(device,&module,nullptr,&modules[i]),"shader");
      stages[i]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};stages[i].stage=i?VK_SHADER_STAGE_FRAGMENT_BIT:VK_SHADER_STAGE_VERTEX_BIT;
      stages[i].module=modules[i];stages[i].pName="main";
    }
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};viewport.viewportCount=viewport.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState attachment{};attachment.colorWriteMask=15;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=1;blend.pAttachments=&attachment;
    const VkDynamicState states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=2;dynamic.pDynamicStates=states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.colorAttachmentCount=1;rendering.pColorAttachmentFormats=&config.color_format;
    rendering.depthAttachmentFormat=rendering.stencilAttachmentFormat=config.depth_format;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pipeline.stageCount=2;pipeline.pStages=stages;
    pipeline.pVertexInputState=&input;pipeline.pInputAssemblyState=&assembly;pipeline.pViewportState=&viewport;pipeline.pRasterizationState=&raster;
    pipeline.pMultisampleState=&samples;pipeline.pDepthStencilState=&depth;pipeline.pColorBlendState=&blend;pipeline.pDynamicState=&dynamic;pipeline.layout=layout_;
    pipeline.renderPass=config.dynamic_rendering?VK_NULL_HANDLE:config.render_pass;pipeline.pNext=config.dynamic_rendering?&rendering:nullptr;
    Require(f_.vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pipeline,nullptr,&pipeline_),"graphics pipeline");
  }catch(...){for(auto m:modules)if(m)f_.vkDestroyShaderModule(device,m,nullptr);throw;}
  for(auto m:modules)f_.vkDestroyShaderModule(device,m,nullptr);
  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR;
  Require(f_.vkCreateSampler(device,&sampler,nullptr,&linear_),"linear sampler");
  sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;
  Require(f_.vkCreateSampler(device,&sampler,nullptr,&point_),"point sampler");
  Allocate(indices_,12,VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  const uint16_t indices[]={0,1,2,1,3,2};std::memcpy(indices_.mapped,indices,sizeof(indices));
  for(auto& frame:frames_){
    Allocate(frame.quad,48,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    Allocate(frame.parameters,sizeof(WorldColorParameters),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};set.descriptorPool=pool_;set.descriptorSetCount=1;set.pSetLayouts=&descriptors_;
    Require(f_.vkAllocateDescriptorSets(device,&set,&frame.descriptors),"descriptor set");
  }
}
std::optional<WorldColorPass::Dispatch> WorldColorPass::Prepare(uint32_t slot,const Source& source,
    const Images& images,const WorldColorParameters& p,VkViewport viewport,VkRect2D scissor,
    uint64_t submission,uint64_t completed) {
  if(!pipeline_ || slot>=frames_.size() || !source.buffer || source.offset%4 || source.offset>source.buffer_bytes ||
      source.buffer_bytes-source.offset<48 || !images.exposure || !images.scene || !images.noise || !images.bloom || !images.grading ||
      !Valid(p) || submission<=completed || submission<last_submission_ || completed<last_completed_ || frames_[slot].submission>completed ||
      !std::isfinite(viewport.x) || !std::isfinite(viewport.y) || !std::isfinite(viewport.width) || !std::isfinite(viewport.height) ||
      viewport.width<=0 || viewport.height<=0 || viewport.width>limits_.maxViewportDimensions[0] || viewport.height>limits_.maxViewportDimensions[1] ||
      viewport.x<limits_.viewportBoundsRange[0] || viewport.y<limits_.viewportBoundsRange[0] ||
      viewport.x+viewport.width>limits_.viewportBoundsRange[1] || viewport.y+viewport.height>limits_.viewportBoundsRange[1] ||
      !(viewport.minDepth>=0 && viewport.minDepth<=1 && viewport.maxDepth>=0 && viewport.maxDepth<=1) ||
      scissor.offset.x<0 || scissor.offset.y<0 || !scissor.extent.width || !scissor.extent.height ||
      uint64_t(scissor.offset.x)+scissor.extent.width>INT32_MAX || uint64_t(scissor.offset.y)+scissor.extent.height>INT32_MAX)return {};
  auto& frame=frames_[slot];
  std::memcpy(frame.parameters.mapped,&p,sizeof(p));
  const VkDescriptorBufferInfo buffers[]={{frame.quad.buffer,0,48},{frame.parameters.buffer,0,sizeof(p)}};
  const VkImageView views[]={images.exposure,images.scene,images.scene,images.noise,images.bloom,images.grading};
  VkDescriptorImageInfo descriptors[6]{};VkWriteDescriptorSet writes[8]{};
  for(uint32_t i=0;i<8;++i){
    writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=frame.descriptors;writes[i].dstBinding=i;writes[i].descriptorCount=1;
    if(i<2){writes[i].descriptorType=i?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&buffers[i];}
    else {descriptors[i-2]={i==2 || i==4?point_:linear_,views[i-2],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      writes[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[i].pImageInfo=&descriptors[i-2];}
  }
  f_.vkUpdateDescriptorSets(device_,8,writes,0,nullptr);
  frame.submission=submission;last_submission_=submission;last_completed_=completed;
  return Dispatch{this,frame.descriptors,source.buffer,frame.quad.buffer,source.offset,viewport,scissor};
}
void WorldColorPass::Upload(VkCommandBuffer command,const Dispatch& draw) const {
  VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
  f_.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
  VkBufferCopy copy{draw.offset,0,48};f_.vkCmdCopyBuffer(command,draw.source,draw.quad,1,&copy);
  barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
  f_.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
}
void WorldColorPass::Draw(VkCommandBuffer command,const Dispatch& draw) const {
  f_.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_);
  f_.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&draw.descriptors,0,nullptr);
  f_.vkCmdBindIndexBuffer(command,indices_.buffer,0,VK_INDEX_TYPE_UINT16);
  f_.vkCmdSetViewport(command,0,1,&draw.viewport);f_.vkCmdSetScissor(command,0,1,&draw.scissor);
  f_.vkCmdDrawIndexed(command,6,1,0,0,0);
}
} // namespace forza::native_renderer
