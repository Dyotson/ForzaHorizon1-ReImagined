// Exercise the Vulkan ABI used by the dynamic guest-constants experiment.
// This checks GPU data and synchronization, not application-cache hit rates.
#include "vulkan_test_support.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>

int main(int argc, char** argv) try {
  CHECK(argc >= 2);
  Context c;
  CHECK(c.Initialize(argc > 2 && std::strcmp(argv[2], "--validation") == 0));
  VkPhysicalDeviceProperties properties;
  vkGetPhysicalDeviceProperties(c.physical, &properties);
  CHECK(properties.limits.maxDescriptorSetUniformBuffersDynamic >= 5);
  const uint32_t alignment = uint32_t(std::max<VkDeviceSize>(
      32, properties.limits.minUniformBufferOffsetAlignment));
  constexpr uint32_t slices = 32, draws = 200;
  Buffer a(c, alignment*slices, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  Buffer b(c, alignment*slices, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  Buffer out(c, draws*sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  std::array<VkDescriptorSetLayoutBinding,5> bindings{};
  for(uint32_t i=0;i<5;++i) bindings[i] = {i,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,
                                        VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
  VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  layout_info.bindingCount=5;layout_info.pBindings=bindings.data();
  VkDescriptorSetLayout constants_layout, output_layout;
  VK_CHECK(vkCreateDescriptorSetLayout(c.device,&layout_info,nullptr,&constants_layout));
  VkDescriptorSetLayoutBinding output_binding{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,
                                              VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
  layout_info.bindingCount=1;layout_info.pBindings=&output_binding;
  VK_CHECK(vkCreateDescriptorSetLayout(c.device,&layout_info,nullptr,&output_layout));
  VkDescriptorSetLayout layouts[]={constants_layout,output_layout};
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,4};
  VkPipelineLayoutCreateInfo pipeline_layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pipeline_layout_info.setLayoutCount=2;pipeline_layout_info.pSetLayouts=layouts;
  pipeline_layout_info.pushConstantRangeCount=1;pipeline_layout_info.pPushConstantRanges=&push;
  VkPipelineLayout pipeline_layout;
  VK_CHECK(vkCreatePipelineLayout(c.device,&pipeline_layout_info,nullptr,&pipeline_layout));
  std::ifstream file(argv[1],std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(file)),{});
  CHECK(!bytes.empty() && bytes.size()%4==0);
  std::vector<uint32_t> code(bytes.size()/4);std::memcpy(code.data(),bytes.data(),bytes.size());
  VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module_info.codeSize=bytes.size();module_info.pCode=code.data();
  VkShaderModule shader;VK_CHECK(vkCreateShaderModule(c.device,&module_info,nullptr,&shader));
  VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.layout=pipeline_layout;
  pipeline_info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  pipeline_info.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;pipeline_info.stage.module=shader;
  pipeline_info.stage.pName="main";
  VkPipeline pipeline;
  VK_CHECK(vkCreateComputePipelines(c.device,VK_NULL_HANDLE,1,&pipeline_info,nullptr,&pipeline));
  VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,15},
                               {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets=4;pool_info.poolSizeCount=2;pool_info.pPoolSizes=sizes;
  VkDescriptorPool pool;VK_CHECK(vkCreateDescriptorPool(c.device,&pool_info,nullptr,&pool));
  VkDescriptorSetLayout set_layouts[]={constants_layout,constants_layout,constants_layout,output_layout};
  VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocation.descriptorPool=pool;allocation.descriptorSetCount=4;allocation.pSetLayouts=set_layouts;
  VkDescriptorSet sets[4];VK_CHECK(vkAllocateDescriptorSets(c.device,&allocation,sets));
  for(uint32_t variant=0;variant<3;++variant) {
    std::array<VkDescriptorBufferInfo,5> infos;
    std::array<VkWriteDescriptorSet,5> writes{};
    for(uint32_t i=0;i<5;++i) {
      infos[i]={variant==1 && i==2 ? b.buffer:a.buffer,0,variant==2?32u:16u};
      writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=sets[variant];
      writes[i].dstBinding=i;writes[i].descriptorCount=1;
      writes[i].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
      writes[i].pBufferInfo=&infos[i];
    }
    vkUpdateDescriptorSets(c.device,5,writes.data(),0,nullptr);
  }
  VkDescriptorBufferInfo output_info{out.buffer,0,draws*sizeof(uint32_t)};
  VkWriteDescriptorSet output_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  output_write.dstSet=sets[3];output_write.descriptorCount=1;
  output_write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;output_write.pBufferInfo=&output_info;
  vkUpdateDescriptorSets(c.device,1,&output_write,0,nullptr);
  for(uint32_t frame=0;frame<8;++frame) {
    for(uint32_t slice=0;slice<slices;++slice) {
      *reinterpret_cast<uint32_t*>(static_cast<char*>(a.mapped)+slice*alignment)=1000*frame+slice;
      *reinterpret_cast<uint32_t*>(static_cast<char*>(b.mapped)+slice*alignment)=1000*frame+slice+100;
    }
    c.Begin();vkCmdBindPipeline(c.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
    std::array<uint32_t,draws> expected{};
    for(uint32_t draw=0;draw<draws;++draw) {
      const uint32_t variant=draw%3;
      uint32_t offsets[5];
      for(uint32_t i=0;i<5;++i) {
        const uint32_t slice=(draw*3+i)%slices;offsets[i]=slice*alignment;
        expected[draw]+=1000*frame+slice+(variant==1 && i==2?100:0);
      }
      VkDescriptorSet bound[]={sets[variant],sets[3]};
      vkCmdBindDescriptorSets(c.command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,
                             0,2,bound,5,offsets);
      std::fill(std::begin(offsets),std::end(offsets),UINT32_MAX);
      vkCmdPushConstants(c.command,pipeline_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&draw);
      vkCmdDispatch(c.command,1,1,1);
      VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
      vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    }
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
                         0,1,&host,0,nullptr,0,nullptr);
    c.Submit();
    for(uint32_t draw=0;draw<draws;++draw) CHECK(static_cast<uint32_t*>(out.mapped)[draw]==expected[draw]);
  }
  vkDestroyDescriptorPool(c.device,pool,nullptr);vkDestroyPipeline(c.device,pipeline,nullptr);
  vkDestroyShaderModule(c.device,shader,nullptr);vkDestroyPipelineLayout(c.device,pipeline_layout,nullptr);
  vkDestroyDescriptorSetLayout(c.device,output_layout,nullptr);
  vkDestroyDescriptorSetLayout(c.device,constants_layout,nullptr);
  CHECK(c.validation_errors==0);
  std::cout<<"1600 GPU dispatches matched across five dynamic bindings, buffer/range variants and eight fenced frames\n";
  return 0;
} catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
