#include "../src/platform/native_renderer/vertex_lit.h"
#include "native_gpu_test_support.h"
#include <fstream>
#include <string>
using namespace forza::native_renderer;

// Own a complete offscreen material target, including its MSAA resolve. No SDK
// shader ABI, guest memory, original-game shaders or textures are involved.
struct MaterialTarget {
  Context& c;
  uint32_t width, height, sample_count;
  VkImage images[2]{};
  VkDeviceMemory memory[2]{};
  VkImageView views[2]{};
  VkRenderPass render_pass{};
  VkFramebuffer framebuffer{};
  VkDescriptorSetLayout descriptors{};
  VkDescriptorPool pool{};
  VkDescriptorSet set{};
  VkPipelineLayout layout{};
  VkPipeline pipeline{};
  Buffer vertices, parameters, readback;
  const std::array<float,4> sentinel{-8,-4,-2,-1};

  MaterialTarget(Context& context,uint32_t w,uint32_t h,uint32_t samples,
      const std::vector<uint32_t>& vs,const std::vector<uint32_t>& fs)
      :c(context),width(w),height(h),sample_count(samples),
       vertices(c,96,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT),
       parameters(c,sizeof(VertexLitParameters),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT),
       readback(c,size_t(w)*h*16,VK_BUFFER_USAGE_TRANSFER_DST_BIT) {
    const uint32_t attachments=samples==1?1:2;
    VkAttachmentDescription descriptions[2]{};
    for(uint32_t i=0;i<attachments;++i) {
      const auto count=i==0?VkSampleCountFlagBits(samples):VK_SAMPLE_COUNT_1_BIT;
      VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      image.imageType=VK_IMAGE_TYPE_2D;image.format=VK_FORMAT_R32G32B32A32_SFLOAT;
      image.extent={w,h,1};image.mipLevels=image.arrayLayers=1;image.samples=count;
      image.tiling=VK_IMAGE_TILING_OPTIMAL;
      image.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      VK_CHECK(vkCreateImage(c.device,&image,nullptr,&images[i]));
      VkMemoryRequirements requirements;vkGetImageMemoryRequirements(c.device,images[i],&requirements);
      VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      allocation.allocationSize=requirements.size;
      allocation.memoryTypeIndex=c.MemoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      VK_CHECK(vkAllocateMemory(c.device,&allocation,nullptr,&memory[i]));
      VK_CHECK(vkBindImageMemory(c.device,images[i],memory[i],0));
      VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view.image=images[i];view.viewType=VK_IMAGE_VIEW_TYPE_2D;view.format=image.format;
      view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
      VK_CHECK(vkCreateImageView(c.device,&view,nullptr,&views[i]));
      auto& description=descriptions[i];description.format=image.format;description.samples=count;
      description.loadOp=i==0?VK_ATTACHMENT_LOAD_OP_CLEAR:VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      description.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
      description.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
      description.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference resolve{1,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount=1;subpass.pColorAttachments=&color;
    if(samples!=1)subpass.pResolveAttachments=&resolve;
    VkSubpassDependency dependencies[2]{};
    dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;
    dependencies[0].srcStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    dependencies[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount=attachments;pass.pAttachments=descriptions;
    pass.subpassCount=1;pass.pSubpasses=&subpass;pass.dependencyCount=2;pass.pDependencies=dependencies;
    VK_CHECK(vkCreateRenderPass(c.device,&pass,nullptr,&render_pass));
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass=render_pass;fb.attachmentCount=attachments;fb.pAttachments=views;
    fb.width=w;fb.height=h;fb.layers=1;
    VK_CHECK(vkCreateFramebuffer(c.device,&fb,nullptr,&framebuffer));
    VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo descriptor_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptor_info.bindingCount=1;descriptor_info.pBindings=&binding;
    VK_CHECK(vkCreateDescriptorSetLayout(c.device,&descriptor_info,nullptr,&descriptors));
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets=1;pool_info.poolSizeCount=1;pool_info.pPoolSizes=&pool_size;
    VK_CHECK(vkCreateDescriptorPool(c.device,&pool_info,nullptr,&pool));
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool=pool;allocate.descriptorSetCount=1;allocate.pSetLayouts=&descriptors;
    VK_CHECK(vkAllocateDescriptorSets(c.device,&allocate,&set));
    VkDescriptorBufferInfo uniform{parameters.buffer,0,sizeof(VertexLitParameters)};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet=set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;write.pBufferInfo=&uniform;
    vkUpdateDescriptorSets(c.device,1,&write,0,nullptr);
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount=1;layout_info.pSetLayouts=&descriptors;
    VK_CHECK(vkCreatePipelineLayout(c.device,&layout_info,nullptr,&layout));
    VkShaderModule modules[2]{};
    VkPipelineShaderStageCreateInfo stages[2]{};
    for(uint32_t i=0;i<2;++i) {
      const auto& code=i==0?vs:fs;
      VkShaderModuleCreateInfo shader{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      shader.codeSize=code.size()*4;shader.pCode=code.data();
      VK_CHECK(vkCreateShaderModule(c.device,&shader,nullptr,&modules[i]));
      stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      stages[i].stage=i==0?VK_SHADER_STAGE_VERTEX_BIT:VK_SHADER_STAGE_FRAGMENT_BIT;
      stages[i].module=modules[i];stages[i].pName="main";
    }
    VkVertexInputBindingDescription vertex_binding{0,32,VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[]={{0,0,VK_FORMAT_R32G32B32A32_SFLOAT,0},{1,0,VK_FORMAT_R32G32B32A32_SFLOAT,16}};
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount=1;vertex_input.pVertexBindingDescriptions=&vertex_binding;
    vertex_input.vertexAttributeDescriptionCount=2;vertex_input.pVertexAttributeDescriptions=attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0,0,float(w),float(h),0,1};
    VkRect2D scissor{{2,3},{w-7,h-8}};
    VkPipelineViewportStateCreateInfo viewport_info{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_info.viewportCount=viewport_info.scissorCount=1;viewport_info.pViewports=&viewport;viewport_info.pScissors=&scissor;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.lineWidth=1;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples=VkSampleCountFlagBits(samples);
    VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=15;
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount=1;blending.pAttachments=&blend;
    VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount=2;pipeline_info.pStages=stages;pipeline_info.pVertexInputState=&vertex_input;
    pipeline_info.pInputAssemblyState=&assembly;pipeline_info.pViewportState=&viewport_info;
    pipeline_info.pRasterizationState=&raster;pipeline_info.pMultisampleState=&multisample;
    pipeline_info.pColorBlendState=&blending;pipeline_info.layout=layout;pipeline_info.renderPass=render_pass;
    const auto result=vkCreateGraphicsPipelines(c.device,VK_NULL_HANDLE,1,&pipeline_info,nullptr,&pipeline);
    for(auto module:modules)vkDestroyShaderModule(c.device,module,nullptr);
    VK_CHECK(result);
  }
  ~MaterialTarget() {
    vkDestroyPipeline(c.device,pipeline,nullptr);vkDestroyPipelineLayout(c.device,layout,nullptr);
    vkDestroyDescriptorPool(c.device,pool,nullptr);vkDestroyDescriptorSetLayout(c.device,descriptors,nullptr);
    vkDestroyFramebuffer(c.device,framebuffer,nullptr);vkDestroyRenderPass(c.device,render_pass,nullptr);
    for(uint32_t i=0;i<(sample_count==1?1u:2u);++i) {
      vkDestroyImageView(c.device,views[i],nullptr);vkDestroyImage(c.device,images[i],nullptr);vkFreeMemory(c.device,memory[i],nullptr);
    }
  }
  void Check(const VertexLitParameters& p) {
    CHECK(Valid(p) && p.output_rules[1]==sample_count);
    const float triangle[]={-1,-1,0.5f,1, 0.25f,1,4,1,
                             3,-1,0.5f,1, 0.25f,1,4,1,
                            -1, 3,0.5f,1, 0.25f,1,4,1};
    std::memcpy(vertices.mapped,triangle,sizeof(triangle));
    std::memcpy(parameters.mapped,&p,sizeof(p));
    c.Begin();
    VkClearValue clear{};std::memcpy(clear.color.float32,sentinel.data(),16);
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass=render_pass;begin.framebuffer=framebuffer;begin.renderArea.extent={width,height};
    begin.clearValueCount=1;begin.pClearValues=&clear;
    vkCmdBeginRenderPass(c.command,&begin,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    const VkDeviceSize offset=0;vkCmdBindVertexBuffers(c.command,0,1,&vertices.buffer,&offset);
    vkCmdBindDescriptorSets(c.command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);
    vkCmdDraw(c.command,3,1,0,0);
    vkCmdEndRenderPass(c.command);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={width,height,1};
    vkCmdCopyImageToBuffer(c.command,images[sample_count==1?0:1],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&copy);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer=readback.buffer;barrier.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&barrier,0,nullptr);
    c.Submit();
    const auto color=ShadeVertexLit({0.25f,1,4},p);
    const auto* pixels=static_cast<const float*>(readback.mapped);
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x) {
      float fraction=0;
      if(x>=2 && x<width-5 && y>=3 && y<height-5 &&
          VertexLitAlphaPass(p.material[0],p.output_color[1],p.output_rules[0])) {
        const uint32_t mask=VertexLitCoverage(p.material[0],x,y,sample_count,p.output_rules[2]);
        for(uint32_t sample=0;sample<sample_count;++sample)if(mask&(1u<<sample))fraction+=1.0f/sample_count;
      }
      for(uint32_t channel=0;channel<4;++channel) {
        const float expected=color[channel]*fraction+sentinel[channel]*(1-fraction);
        const float actual=pixels[(size_t(y)*width+x)*4+channel];
        if(!std::isfinite(actual) || std::abs(actual-expected)>0.000005f) {
          std::cerr<<"Material mismatch "<<x<<','<<y<<" channel "<<channel<<": "<<actual<<" expected "<<expected<<'\n';
          throw std::runtime_error("Native material GPU output differs");
        }
      }
    }
  }
};

std::vector<uint32_t> Shader(const char* path) {
  std::ifstream file(path,std::ios::binary|std::ios::ate);CHECK(file.good());
  const auto bytes=file.tellg();CHECK(bytes>0 && bytes%4==0);
  std::vector<uint32_t> result(size_t(bytes)/4);file.seekg(0);
  file.read(reinterpret_cast<char*>(result.data()),bytes);CHECK(file.good());return result;
}

int main(int argc,char** argv) {
  if(argc<3 || argc>4)return 2;
  const bool validation=argc==4 && std::string(argv[3])=="--validation";
  if(argc==4 && !validation)return 2;
  try {
    const auto vs=Shader(argv[1]),fs=Shader(argv[2]);
    Context c;if(!c.Initialize(validation,true)) {
      if(validation)throw std::runtime_error("Could not start requested material validation");
      return 77;
    }
    VkImageFormatProperties format{};
    if(vkGetPhysicalDeviceImageFormatProperties(c.physical,VK_FORMAT_R32G32B32A32_SFLOAT,VK_IMAGE_TYPE_2D,
        VK_IMAGE_TILING_OPTIMAL,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,0,&format)!=VK_SUCCESS)return 77;
    for(uint32_t samples:{1u,4u}) {
      if(!(format.sampleCounts&samples)) {std::cout<<"Material MSAA "<<samples<<" unsupported\n";continue;}
      MaterialTarget target(c,73,41,samples,vs,fs);
      VertexLitParameters p;p.output_rules[1]=samples;target.Check(p);
      p.fog={1,4,9,1};p.output_color[0]=2;p.material[0]=0.25f;target.Check(p);
      p.fog={1,4,9,0.5f};p.modulation={-1,4,9,0};target.Check(p);
      p.output_rules[3]=1;target.Check(p);
      for(uint32_t comparison=0;comparison<8;++comparison)for(float alpha:{0.25f,0.5f,0.75f}) {
        p=VertexLitParameters{};p.output_rules={comparison,samples,0,0};
        p.output_color[1]=0.5f;p.material[0]=alpha;target.Check(p);
      }
      for(float alpha:{0.0f,0.24f,0.25f,0.5f,0.75f,1.0f})for(uint32_t pattern:{256u,483u}) {
        p=VertexLitParameters{};p.output_rules={7,samples,pattern,0};p.material[0]=alpha;target.Check(p);
      }
      std::cout<<"Native material 73x41 MSAA "<<samples<<": color, fog, alpha, sample coverage and untouched padding passed\n";
    }
    { MaterialTarget target(c,2560,1440,1,vs,fs);target.Check(VertexLitParameters{}); }
    CHECK(c.validation_errors==0);
    std::cout<<"Native material 2560x1440 output passed; zero validation errors\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
