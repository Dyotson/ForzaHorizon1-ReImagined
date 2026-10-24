#include "../src/platform/native_renderer/geometry_pass.h"
#include "native_gpu_test_support.h"
#include <fstream>
#include <functional>
#include <memory>
using namespace forza::native_renderer;

struct DepthTarget {
  Context& context;
  uint32_t width, height;
  VkImage image{};
  VkDeviceMemory memory{};
  VkImageView view{};
  VkRenderPass render_pass{};
  VkFramebuffer framebuffer{};
  struct Result { std::vector<float> depth; std::vector<uint8_t> stencil; };
  DepthTarget(Context& c,uint32_t w,uint32_t h):context(c),width(w),height(h) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType=VK_IMAGE_TYPE_2D;info.format=VK_FORMAT_D32_SFLOAT_S8_UINT;
    info.extent={w,h,1};info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;
    info.tiling=VK_IMAGE_TILING_OPTIMAL;
    info.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vkCreateImage(c.device,&info,nullptr,&image));
    VkMemoryRequirements requirements;vkGetImageMemoryRequirements(c.device,image,&requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize=requirements.size;allocation.memoryTypeIndex=c.MemoryType(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(c.device,&allocation,nullptr,&memory));VK_CHECK(vkBindImageMemory(c.device,image,memory,0));
    VkImageViewCreateInfo image_view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    image_view.image=image;image_view.viewType=VK_IMAGE_VIEW_TYPE_2D;image_view.format=info.format;
    image_view.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
    VK_CHECK(vkCreateImageView(c.device,&image_view,nullptr,&view));
    VkAttachmentDescription attachment{};attachment.format=info.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp=attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp=attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference depth{0,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.pDepthStencilAttachment=&depth;
    VkSubpassDependency dependencies[2]{};
    const VkPipelineStageFlags stages=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcSubpass=VK_SUBPASS_EXTERNAL;dependencies[0].dstSubpass=0;
    dependencies[0].srcStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;dependencies[0].dstStageMask=stages;
    dependencies[0].srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    dependencies[0].dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass=0;dependencies[1].dstSubpass=VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask=stages;dependencies[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;dependencies[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass.attachmentCount=1;pass.pAttachments=&attachment;pass.subpassCount=1;pass.pSubpasses=&subpass;
    pass.dependencyCount=2;pass.pDependencies=dependencies;
    VK_CHECK(vkCreateRenderPass(c.device,&pass,nullptr,&render_pass));
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass=render_pass;fb.attachmentCount=1;fb.pAttachments=&view;fb.width=w;fb.height=h;fb.layers=1;
    VK_CHECK(vkCreateFramebuffer(c.device,&fb,nullptr,&framebuffer));
  }
  ~DepthTarget() {
    vkDestroyFramebuffer(context.device,framebuffer,nullptr);vkDestroyRenderPass(context.device,render_pass,nullptr);
    vkDestroyImageView(context.device,view,nullptr);vkDestroyImage(context.device,image,nullptr);vkFreeMemory(context.device,memory,nullptr);
  }
  Result Render(const GeometryPass& pass,const std::vector<GeometryPass::Dispatch>& draws,bool reverse,
      const std::function<void(VkCommandBuffer)>& uploads = {}) {
    const size_t pixels=size_t(width)*height;
    const VkDeviceSize stencil_offset=(pixels*4+15)&~VkDeviceSize(15);
    Buffer readback(context,stencil_offset+pixels,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    context.Begin();
    if(uploads)uploads(context.command);
    else for(const auto& draw:draws)pass.RecordUploads(context.command,draw);
    VkClearValue clear{};clear.depthStencil={reverse?0.0f:1.0f,0xa5};
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    begin.renderPass=render_pass;begin.framebuffer=framebuffer;begin.renderArea.extent={width,height};
    begin.clearValueCount=1;begin.pClearValues=&clear;
    if(context.dynamic_rendering) {
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
      barrier.dstAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      barrier.image=image;barrier.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
      vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
          0,0,nullptr,0,nullptr,1,&barrier);
      VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      attachment.imageView=view;attachment.imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
      attachment.clearValue=clear;
      VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
      rendering.renderArea.extent={width,height};rendering.layerCount=1;
      rendering.pDepthAttachment=rendering.pStencilAttachment=&attachment;
      vkCmdBeginRendering(context.command,&rendering);
    } else vkCmdBeginRenderPass(context.command,&begin,VK_SUBPASS_CONTENTS_INLINE);
    for(const auto& draw:draws)pass.Record(context.command,draw);
    if(context.dynamic_rendering) {
      vkCmdEndRendering(context.command);
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
      barrier.oldLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
      barrier.image=image;barrier.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
      vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    } else vkCmdEndRenderPass(context.command);
    VkBufferImageCopy copies[2]{};
    copies[0].imageSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};copies[0].imageExtent={width,height,1};
    copies[1].bufferOffset=stencil_offset;copies[1].imageSubresource={VK_IMAGE_ASPECT_STENCIL_BIT,0,0,1};copies[1].imageExtent=copies[0].imageExtent;
    vkCmdCopyImageToBuffer(context.command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,2,copies);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer=readback.buffer;barrier.size=VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(context.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&barrier,0,nullptr);
    context.Submit();
    Result result;result.depth.resize(pixels);result.stencil.resize(pixels);
    std::memcpy(result.depth.data(),readback.mapped,pixels*4);
    std::memcpy(result.stencil.data(),static_cast<const uint8_t*>(readback.mapped)+stencil_offset,pixels);
    return result;
  }
};

GeometryVulkanDispatch Functions() {
  GeometryVulkanDispatch result;
#define SET_FUNCTION(name) result.name=name;
  FORZA_NATIVE_VULKAN_FUNCTIONS(SET_FUNCTION)
  FORZA_NATIVE_GEOMETRY_VULKAN_FUNCTIONS(SET_FUNCTION)
#undef SET_FUNCTION
  return result;
}

void Run(Context& context,const std::vector<uint32_t>& shader,uint32_t width,uint32_t height,bool reverse,bool reversed_viewport=false) {
  DepthTarget target(context,width+13,height+7);
  GeometryPass pass;
  GeometryPass::Config config{target.render_pass,2,4096,4096,true,reverse,context.full_index_uint32,context.dynamic_rendering};
  pass.Initialize(Functions(),context.physical,context.device,config,shader);
  uint64_t completed=0,submission=1;
  for(uint32_t vertex_endian=0;vertex_endian<4;++vertex_endian)
    for(uint32_t index_width:{2u,4u}) for(uint32_t index_endian=0;index_endian<4;++index_endian) {
      if(index_width==2 && index_endian>=2)continue;
      if(index_width==4 && index_endian==2 && !context.full_index_uint32)continue;
      GeometryParameters p;p.layout={7,vertex_endian,3,index_endian};
      std::vector<uint32_t> vertices(21,0xdeadbeef);
      const std::array<std::array<int16_t,2>,3> xy{{{-26214,-26214},{26214,-26214},{0,26214}}};
      auto pack=[&](int16_t z) {
        for(uint32_t i=0;i<3;++i) {
          vertices[i*7]=GeometryEndian((uint32_t(uint16_t(xy[i][0]))<<16)|uint16_t(xy[i][1]),vertex_endian);
          vertices[i*7+1]=GeometryEndian((uint32_t(uint16_t(z))<<16)|32767,vertex_endian);
        }
      };
      const uint32_t order[]={0,1,2,UINT32_MAX,2,1,0};
      std::vector<uint8_t> indices(7*index_width);
      for(uint32_t i=0;i<7;++i) {
        const uint32_t raw=order[i]==UINT32_MAX?UINT32_MAX:GeometryEndian(order[i],index_endian);
        std::memcpy(indices.data()+i*index_width,&raw,index_width);
      }
      GeometryPass::Mesh mesh{vertices.data(),uint32_t(vertices.size()*4),indices.data(),uint32_t(indices.size()),index_width};
      GeometryPass::Raster raster{{0,0,float(width),float(height),0,1},{{2,2},{width-4,height-4}},9};
      if(reversed_viewport) {raster.viewport.minDepth=0.5f;raster.viewport.maxDepth=0;}
      pack(reverse!=reversed_viewport?8192:24575);
      const auto first=pass.Prepare(0,mesh,p,raster,submission,completed);CHECK(first);
      CHECK(!pass.Prepare(0,mesh,p,raster,submission,completed));
      pack(reverse!=reversed_viewport?24575:8192);raster.stencil_reference=42;
      raster.scissor={{int32_t(width/3),int32_t(height/3)},{width/3,height/3}};
      const auto second=pass.Prepare(1,mesh,p,raster,submission,completed);CHECK(second);
      // Later caller edits cannot alter an already prepared draw.
      vertices.assign(vertices.size(),0);indices.assign(indices.size(),0);p.origin_scale={9,9,9,9};
      const auto result=target.Render(pass,{*first,*second},reverse);
      const auto depth_value=[&](int16_t z) {return raster.viewport.minDepth+
          GeometrySnorm(z)*(raster.viewport.maxDepth-raster.viewport.minDepth);};
      const float near=depth_value(reverse!=reversed_viewport?24575:8192),
          far=depth_value(reverse!=reversed_viewport?8192:24575);
      const auto center=size_t(height/2)*target.width+width/2;
      CHECK(result.stencil[center]==42 && std::abs(result.depth[center]-near)<1e-6f);
      const auto outer=size_t(height/4)*target.width+width/2;
      CHECK(result.stencil[outer]==9 && std::abs(result.depth[outer]-far)<1e-6f);
      size_t drawn=0;
      for(uint32_t y=0;y<target.height;++y) for(uint32_t x=0;x<target.width;++x) {
        const auto i=size_t(y)*target.width+x;const auto stencil=result.stencil[i];
        if(stencil==9 || stencil==42) {++drawn;CHECK(std::abs(result.depth[i]-(stencil==9?far:near))<1e-6f);}
        else CHECK(stencil==0xa5 && result.depth[i]==(reverse?0.0f:1.0f));
        if(x<2 || y<2 || x>=width-2 || y>=height-2)CHECK(stencil==0xa5);
      }
      CHECK(drawn>100);completed=submission++;
      p=GeometryParameters{};p.layout={7,vertex_endian,3,index_endian};
      mesh.vertices=nullptr;CHECK(!pass.Prepare(0,mesh,p,raster,submission,completed));mesh.vertices=vertices.data();
      const auto valid_viewport=raster.viewport;
      raster.viewport.minDepth=1.01f;CHECK(!pass.Prepare(0,mesh,p,raster,submission,completed));
      raster.viewport=valid_viewport;raster.viewport.maxDepth=-0.01f;
      CHECK(!pass.Prepare(0,mesh,p,raster,submission,completed));raster.viewport=valid_viewport;
      auto invalid=p;invalid.layout[2]=4;CHECK(!pass.Prepare(0,mesh,invalid,raster,submission,completed));
      raster.scissor.extent.width=UINT32_MAX;CHECK(!pass.Prepare(0,mesh,p,raster,submission,completed));
      raster.scissor.extent.width=width/3;
      const uint32_t invalid_index=GeometryEndian(3,index_endian);
      std::memcpy(indices.data(),&invalid_index,index_width);
      CHECK(!pass.Prepare(0,mesh,p,raster,submission,completed));
    }
  bool rejected=false;try {pass.Record(context.command,GeometryPass::Dispatch{});}catch(const std::invalid_argument&){rejected=true;}CHECK(rejected);
  std::cout<<"Native geometry "<<width<<'x'<<height<<", reverse="<<reverse<<", reversed viewport="<<reversed_viewport<<": depth, stencil, scissor, snapshots and retired slots passed\n";
}

void RunGpuSnapshots(Context& context,const std::vector<uint32_t>& shader) {
  constexpr uint32_t width=73,height=41;
  DepthTarget target(context,width,height);
  GeometryPass pass;
  pass.Initialize(Functions(),context.physical,context.device,
      {target.render_pass,2,4096,4096,true,true,context.full_index_uint32,context.dynamic_rendering},shader);
  Buffer source(context,256,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  Buffer first_upload(context,256,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  Buffer second_upload(context,256,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  std::memset(source.mapped,0,256);
  std::memset(first_upload.mapped,0x57,256);std::memset(second_upload.mapped,0xa3,256);
  GeometryParameters p;p.layout={7,2,3,1};
  std::vector<uint32_t> far_vertices(21,0),near_vertices(21,0);
  const std::array<std::array<int16_t,2>,3> xy{{{-26214,-26214},{26214,-26214},{0,26214}}};
  for(uint32_t i=0;i<3;++i) {
    far_vertices[i*7]=near_vertices[i*7]=GeometryEndian((uint32_t(uint16_t(xy[i][0]))<<16)|uint16_t(xy[i][1]),2);
    far_vertices[i*7+1]=GeometryEndian((8192u<<16)|32767,2);
    near_vertices[i*7+1]=GeometryEndian((24575u<<16)|32767,2);
  }
  std::vector<uint16_t> indices;
  for(uint32_t i:{0u,1u,2u,0xffffu,2u,1u,0u})indices.push_back(uint16_t(GeometryEndian(i,1)));
  std::memcpy(static_cast<uint8_t*>(first_upload.mapped)+4,far_vertices.data(),84);
  std::memcpy(static_cast<uint8_t*>(second_upload.mapped)+4,near_vertices.data(),84);
  for(void* mapped:{first_upload.mapped,second_upload.mapped})
    std::memcpy(static_cast<uint8_t*>(mapped)+90,indices.data(),14);
  GeometryPass::GpuMesh gpu{{source.buffer,256,4,84},{source.buffer,256,90,14},2};
  GeometryPass::Raster raster{{0,0,float(width),float(height),0,1},{{2,2},{width-4,height-4}},9};
  auto invalid=gpu;invalid.vertices.offset=5;CHECK(!pass.PrepareGpu(0,invalid,p,raster,1,0));
  invalid=gpu;invalid.indices.offset=88;invalid.indices.buffer_bytes=102;
  CHECK(!pass.PrepareGpu(0,invalid,p,raster,1,0)); // logical bytes fit, rounded copy does not
  invalid=gpu;invalid.vertices.buffer_bytes=87;CHECK(!pass.PrepareGpu(0,invalid,p,raster,1,0));
  invalid=gpu;invalid.indices.offset=UINT64_MAX-1;CHECK(!pass.PrepareGpu(0,invalid,p,raster,1,0));
  const auto first=pass.PrepareGpu(0,gpu,p,raster,1,0);CHECK(first);
  CHECK(!pass.PrepareGpu(0,gpu,p,raster,1,0));
  raster.stencil_reference=42;raster.scissor={{width/3,height/3},{width/3,height/3}};
  const auto second=pass.PrepareGpu(1,gpu,p,raster,1,0);CHECK(second);
  const auto result=target.Render(pass,{*first,*second},true,[&](VkCommandBuffer command) {
    VkBufferMemoryBarrier writable{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    writable.buffer=source.buffer;writable.size=VK_WHOLE_SIZE;
    writable.srcQueueFamilyIndex=writable.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    writable.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;writable.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&writable,0,nullptr);
    VkBufferCopy copy{0,0,256};
    vkCmdCopyBuffer(command,first_upload.buffer,source.buffer,1,&copy);
    pass.RecordUploads(command,*first);
    // The source owner orders its next write after the snapshot read.
    writable.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&writable,0,nullptr);
    vkCmdCopyBuffer(command,second_upload.buffer,source.buffer,1,&copy);
    pass.RecordUploads(command,*second);
    vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&writable,0,nullptr);
    vkCmdFillBuffer(command,source.buffer,0,VK_WHOLE_SIZE,0);
  });
  GeometryPass::Mesh cpu{far_vertices.data(),84,indices.data(),14,2};
  raster.scissor={{2,2},{width-4,height-4}};raster.stencil_reference=9;
  const auto reference_first=pass.Prepare(0,cpu,p,raster,2,1);CHECK(reference_first);
  cpu.vertices=near_vertices.data();raster.stencil_reference=42;raster.scissor={{width/3,height/3},{width/3,height/3}};
  const auto reference_second=pass.Prepare(1,cpu,p,raster,2,1);CHECK(reference_second);
  const auto reference=target.Render(pass,{*reference_first,*reference_second},true);
  CHECK(std::memcmp(result.depth.data(),reference.depth.data(),result.depth.size()*4)==0);
  CHECK(result.stencil==reference.stencil);
  CHECK(result.stencil[(height/2)*width+width/2]==42);
  CHECK(result.stencil[(height/4)*width+width/2]==9);
  std::cout<<"GPU geometry snapshots: distinct GPU-written versions, later source overwrite, unaligned indices and retired-slot reuse passed\n";
}

int main(int argc,char** argv) {
  if(argc<2 || argc>4)return 2;
  bool validation=false,dynamic=false;
  for(int i=2;i<argc;++i) {
    const std::string option=argv[i];
    if(option=="--validation")validation=true;
    else if(option=="--dynamic-rendering")dynamic=true;
    else return 2;
  }
  try {
    std::ifstream file(argv[1],std::ios::binary|std::ios::ate);CHECK(file.good());
    const auto bytes=file.tellg();CHECK(bytes>0 && bytes%4==0);std::vector<uint32_t> shader(size_t(bytes)/4);
    file.seekg(0);file.read(reinterpret_cast<char*>(shader.data()),bytes);CHECK(file.good());
    Context context;if(!context.Initialize(validation,true,dynamic?VK_API_VERSION_1_3:VK_API_VERSION_1_1)) {
      if(validation)throw std::runtime_error("Could not start requested geometry validation");
      std::cout<<"No compatible graphics device\n";return 77;
    }
    if(dynamic && !context.dynamic_rendering) {std::cout<<"No Vulkan 1.3 dynamic rendering support\n";return 77;}
    std::cout<<"Dynamic rendering: "<<dynamic<<'\n';
    VkFormatProperties properties;vkGetPhysicalDeviceFormatProperties(context.physical,VK_FORMAT_D32_SFLOAT_S8_UINT,&properties);
    if(!(properties.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))return 77;
    for(bool reverse:{false,true})Run(context,shader,73,41,reverse);
    for(bool reverse:{false,true})Run(context,shader,73,41,reverse,true);
    Run(context,shader,2560,1440,true);
    Run(context,shader,2560,1440,true,true);
    RunGpuSnapshots(context,shader);
    CHECK(context.validation_errors==0);std::cout<<"Native geometry GPU checks passed\n";return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
