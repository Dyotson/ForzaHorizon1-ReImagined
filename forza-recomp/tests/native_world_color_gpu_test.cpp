#include "../src/platform/native_renderer/world_color_pass.h"
#include "../src/platform/native_renderer/vertex_lit.h"
#include "native_gpu_test_support.h"
#include <fstream>
#include <memory>
#include <algorithm>
#include <string>
namespace nr=forza::native_renderer;
std::vector<uint32_t> Shader(const char* path){
 std::ifstream f(path,std::ios::binary|std::ios::ate);CHECK(f);size_t n=size_t(f.tellg());CHECK(n%4==0);
 std::vector<uint32_t> code(n/4);f.seekg(0);CHECK(f.read((char*)code.data(),n));return code;
}
#include <limits>
struct Image {
 Context& c;VkImage image{};VkImageView view{};VkDeviceMemory memory{};uint32_t w,h,d;VkFormat format;
 Image(Context& c,uint32_t w,uint32_t h,uint32_t d,VkFormat format,bool target=false,VkComponentMapping swizzle={}):c(c),w(w),h(h),d(d),format(format){
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.imageType=d>1?VK_IMAGE_TYPE_3D:VK_IMAGE_TYPE_2D;info.format=format;info.extent={w,h,d};info.mipLevels=info.arrayLayers=1;info.samples=VK_SAMPLE_COUNT_1_BIT;
  info.usage=target?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT:VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VK_CHECK(vkCreateImage(c.device,&info,nullptr,&image));VkMemoryRequirements r;vkGetImageMemoryRequirements(c.device,image,&r);
  VkMemoryAllocateInfo m{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};m.allocationSize=r.size;m.memoryTypeIndex=c.MemoryType(r.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VK_CHECK(vkAllocateMemory(c.device,&m,nullptr,&memory));VK_CHECK(vkBindImageMemory(c.device,image,memory,0));
  VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=image;v.format=format;v.viewType=d>1?VK_IMAGE_VIEW_TYPE_3D:target?VK_IMAGE_VIEW_TYPE_2D:VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  v.components=swizzle;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VK_CHECK(vkCreateImageView(c.device,&v,nullptr,&view));
 }
 ~Image(){vkDestroyImageView(c.device,view,nullptr);vkDestroyImage(c.device,image,nullptr);vkFreeMemory(c.device,memory,nullptr);}
 void Load(const std::vector<uint8_t>& b){Buffer upload(c,b.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT);std::memcpy(upload.mapped,b.data(),b.size());c.Begin();
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.image=image;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
  VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={w,h,d};vkCmdCopyBufferToImage(c.command,upload.buffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
  barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_VERTEX_SHADER_BIT|VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);c.Submit();
 }
};
nr::GeometryVulkanDispatch Functions(){nr::GeometryVulkanDispatch f;
#define LOAD(name) f.name=&name;
 FORZA_NATIVE_VULKAN_FUNCTIONS(LOAD)
 FORZA_NATIVE_GEOMETRY_VULKAN_FUNCTIONS(LOAD)
#undef LOAD
 return f;
}

nr::WorldColorParameters Parameters(){
 nr::WorldColorParameters p;
 p.view[0]={1,0,0,1};p.view[1]={1,0,0,0};p.view[2]={0,.25f,.5f,1};p.view[3]={-1,-.5f,0,1};
 p.view[4]={0,.5f,0,0};p.view[5]={1,.5f,.25f,1};p.view[6]={0,0,0,0};p.view[7]={.125f,.25f,.125f,0};
 p.view[8]={-.5f,.5f,1.5f,1};p.view[9]={.1f,.2f,.3f,.4f};p.view[10]={1,0,0,0};
 p.color[0]={1,0,0,1};p.color[1]={0,0,0,1};p.color[2]={.1f,0,0,.5f};
 p.color[4]={.9f,0,0,0};p.color[5]={.03f,0,0,0};p.color[6]={1,0,0,0};
 p.color[7]={1,0,0,0};p.color[8]={0,1,0,0};p.color[9]={0,0,1,0};
 p.color[10]={.25f,0,.7f,0};p.color[11]={0,.7f,.04f,0};p.color[12]={.025f,.02f,.001f,.03f};
 p.color[13]={-.1f,1,0,0};p.color[14]={.5f,-.5f,1,0};p.color[15]={-.1f,.25f,-.25f,.2f};
 p.input_scale={1,1,1,1};p.exposure_bias_scale={0,0,1,0};CHECK(nr::Valid(p));return p;
}
std::vector<uint8_t> Pixels(uint32_t count,std::array<uint8_t,4> value){
 std::vector<uint8_t> bytes(count*4);for(uint32_t i=0;i<count;++i)std::memcpy(bytes.data()+i*4,value.data(),4);return bytes;
}
int main(int argc,char** argv){
 if(argc<3)return 2;bool validation=false,dynamic=false;std::string view_probe;
 for(int i=3;i<argc;++i){std::string arg=argv[i];validation|=arg=="--validation";dynamic|=arg=="--dynamic-rendering";
   if(arg.rfind("--view-probe=",0)==0)view_probe=arg.substr(13);}
 try{
  Context c;if(!c.Initialize(validation,true,dynamic?VK_API_VERSION_1_3:VK_API_VERSION_1_1))return 77;
  if(dynamic && !c.dynamic_rendering)return 77;
  Image exposure(c,2,2,1,VK_FORMAT_R32_SFLOAT),scene(c,2,2,1,VK_FORMAT_R8G8B8A8_UNORM),
      noise(c,2,2,1,VK_FORMAT_R8G8B8A8_UNORM),bloom(c,2,2,1,VK_FORMAT_R8G8B8A8_UNORM),lut(c,2,2,2,VK_FORMAT_R8G8B8A8_UNORM);
  std::vector<uint8_t> adapted(16);for(int i=0;i<4;++i){float value=.375f;std::memcpy(adapted.data()+i*4,&value,4);}exposure.Load(adapted);
  scene.Load(Pixels(4,{64,128,192,255}));noise.Load(Pixels(4,{128,128,128,255}));bloom.Load(Pixels(4,{0,0,0,0}));lut.Load(Pixels(8,{36,81,144,255}));
  constexpr uint32_t width=73,height=41;
  Image output(c,width,height,1,VK_FORMAT_R32G32B32A32_SFLOAT,true);
  VkAttachmentDescription attachment{};attachment.format=output.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=1;subpass.pColorAttachments=&color;
  VkSubpassDependency dep[2]{};dep[0].srcSubpass=VK_SUBPASS_EXTERNAL;dep[0].dstSubpass=0;dep[0].srcStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;dep[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dep[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep[1].srcSubpass=0;dep[1].dstSubpass=VK_SUBPASS_EXTERNAL;dep[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dep[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;dep[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;dep[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&attachment;rp.subpassCount=1;rp.pSubpasses=&subpass;rp.dependencyCount=2;rp.pDependencies=dep;VkRenderPass render_pass{};VK_CHECK(vkCreateRenderPass(c.device,&rp,nullptr,&render_pass));
  VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=render_pass;fb.attachmentCount=1;fb.pAttachments=&output.view;fb.width=width;fb.height=height;fb.layers=1;VkFramebuffer framebuffer{};VK_CHECK(vkCreateFramebuffer(c.device,&fb,nullptr,&framebuffer));
  {
   nr::WorldColorPass pass;nr::WorldColorPass::Config config{render_pass,output.format,VK_FORMAT_UNDEFINED,2,dynamic};
   pass.Initialize(Functions(),c.physical,c.device,config,Shader(argv[1]),Shader(argv[2]));
   Buffer source(c,48,VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT),upload(c,96,VK_BUFFER_USAGE_TRANSFER_SRC_BIT),readback(c,width*height*16,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
   nr::WorldColorPass::Images images{exposure.view,scene.view,noise.view,bloom.view,lut.view};
   for(uint64_t submission=1;submission<=4;++submission){
    const uint32_t endian=uint32_t(submission-1);
    const float a[12]={-1,-1,0,0,1,-1,1,0,-1,1,0,1};
    const float b[12]={-1,-1,0,0,0,-1,1,0,-1,1,0,1};
    for(uint32_t i=0;i<24;++i){uint32_t bits;const float value=i<12?a[i]:b[i-12];std::memcpy(&bits,&value,4);bits=nr::GeometryEndian(bits,endian);std::memcpy((uint8_t*)upload.mapped+i*4,&bits,4);}
    auto first=Parameters(),second=first;first.rules[0]=second.rules[0]=endian;
    second.color[2]={0,.1f,0,.5f};first.rules[1]=second.rules[1]=submission==4?1:0;
    const VkViewport top{2,3,64,16,0,1},bottom{2,19,64,16,0,1};const VkRect2D clip_top{{3,4},{62,14}},clip_bottom{{3,20},{62,14}};
    auto one=pass.Prepare(0,{source.buffer,0,48},images,first,top,clip_top,submission,submission-1);CHECK(one);
    CHECK(!pass.Prepare(0,{source.buffer,0,48},images,second,top,clip_top,submission,submission-1));
    CHECK(!pass.Prepare(1,{source.buffer,4,48},images,second,bottom,clip_bottom,submission,submission-1));
    auto invalid=second;invalid.color[0][0]=std::numeric_limits<float>::quiet_NaN();CHECK(!pass.Prepare(1,{source.buffer,0,48},images,invalid,bottom,clip_bottom,submission,submission-1));
    auto two=pass.Prepare(1,{source.buffer,0,48},images,second,bottom,clip_bottom,submission,submission-1);CHECK(two);
    c.Begin();
    VkMemoryBarrier write{VK_STRUCTURE_TYPE_MEMORY_BARRIER};write.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;write.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    VkBufferCopy copy{0,0,48};vkCmdCopyBuffer(c.command,upload.buffer,source.buffer,1,&copy);pass.Upload(c.command,*one);
    vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&write,0,nullptr,0,nullptr);
    copy.srcOffset=48;vkCmdCopyBuffer(c.command,upload.buffer,source.buffer,1,&copy);pass.Upload(c.command,*two);
    vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&write,0,nullptr,0,nullptr);
    vkCmdFillBuffer(c.command,source.buffer,0,48,0);
    VkClearValue clear{};clear.color.float32[0]=clear.color.float32[1]=clear.color.float32[2]=-1;clear.color.float32[3]=-1;
    if(dynamic){
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;barrier.image=output.image;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
      vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&barrier);
      VkRenderingAttachmentInfo attachment_info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};attachment_info.imageView=output.view;attachment_info.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;attachment_info.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment_info.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment_info.clearValue=clear;
      VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};info.renderArea={{0,0},{width,height}};info.layerCount=1;info.colorAttachmentCount=1;info.pColorAttachments=&attachment_info;vkCmdBeginRendering(c.command,&info);
      pass.Draw(c.command,*one);pass.Draw(c.command,*two);vkCmdEndRendering(c.command);
      barrier.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    }else{
      VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=render_pass;begin.framebuffer=framebuffer;begin.renderArea={{0,0},{width,height}};begin.clearValueCount=1;begin.pClearValues=&clear;
      vkCmdBeginRenderPass(c.command,&begin,VK_SUBPASS_CONTENTS_INLINE);pass.Draw(c.command,*one);pass.Draw(c.command,*two);vkCmdEndRenderPass(c.command);
    }
    VkBufferImageCopy read{};read.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};read.imageExtent={width,height,1};vkCmdCopyImageToBuffer(c.command,output.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&read);c.Submit();
    auto* pixels=(const float*)readback.mapped;
    for(uint32_t y=0;y<height;++y)for(uint32_t x=0;x<width;++x){
      const bool covered_top=x>=3 && x<65 && y>=4 && y<18;
      const bool covered_bottom=x>=3 && x<34 && y>=20 && y<34;
      for(uint32_t component=0;component<4;++component){float expected=-1;
        if(covered_top || covered_bottom){expected=component==3?1:std::sqrt(float(std::array<uint8_t,3>{36,81,144}[component])/255);
          if(component<3 && ((covered_top && component==0)||(covered_bottom && component==1)))expected+=(std::min(1.f,expected+.1f)-expected)*.5f;
          if(component<3 && first.rules[1])expected=nr::VertexLitPwlGamma(expected);
        }
        CHECK(std::abs(pixels[(y*width+x)*4+component]-expected)<0.000003f);
      }
    }
   }
  }
  if(!view_probe.empty()){
   nr::WorldColorPass probe;nr::WorldColorPass::Config config{render_pass,output.format,VK_FORMAT_UNDEFINED,3,dynamic};
   probe.Initialize(Functions(),c.physical,c.device,config,Shader(argv[1]),Shader(view_probe.c_str()));
   std::vector<std::unique_ptr<Image>> exposure_images;
   const float values[]={.125f,.375f,.75f};const float expected[]={-.75f,-.25f,.5f};
   Buffer source(c,48,VK_BUFFER_USAGE_TRANSFER_SRC_BIT),readback(c,width*height*16,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
   const float vertices[]={-1,-1,0,0,1,-1,1,0,-1,1,0,1};std::memcpy(source.mapped,vertices,sizeof(vertices));
   std::vector<nr::WorldColorPass::Dispatch> draws;
   for(uint32_t i=0;i<3;++i){
    auto image=std::make_unique<Image>(c,2,2,1,VK_FORMAT_R32_SFLOAT);
    std::vector<uint8_t> data(16);for(uint32_t j=0;j<4;++j)std::memcpy(data.data()+j*4,&values[i],4);image->Load(data);
    nr::WorldColorPass::Images images{image->view,scene.view,noise.view,bloom.view,lut.view};
    auto draw=probe.Prepare(i,{source.buffer,0,48},images,Parameters(),{2,float(2+i*12),64,10,0,1},{{2,int(2+i*12)},{64,10}},5,4);
    CHECK(draw);draws.push_back(*draw);exposure_images.push_back(std::move(image));
   }
   c.Begin();for(auto& draw:draws)probe.Upload(c.command,draw);
   VkClearValue clear{};
   // A compatible traditional pass exercises the same immutable view contract;
   // the main cases above separately cover the dynamic rendering path.
   if(dynamic){
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;barrier.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;barrier.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;barrier.image=output.image;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,0,nullptr,0,nullptr,1,&barrier);
    VkRenderingAttachmentInfo attachment_info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};attachment_info.imageView=output.view;attachment_info.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;attachment_info.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment_info.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment_info.clearValue=clear;
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};info.renderArea={{0,0},{width,height}};info.layerCount=1;info.colorAttachmentCount=1;info.pColorAttachments=&attachment_info;vkCmdBeginRendering(c.command,&info);
    for(auto& draw:draws)probe.Draw(c.command,draw);vkCmdEndRendering(c.command);
    barrier.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;barrier.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(c.command,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
   }else{
    VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};begin.renderPass=render_pass;begin.framebuffer=framebuffer;begin.renderArea={{0,0},{width,height}};begin.clearValueCount=1;begin.pClearValues=&clear;
    vkCmdBeginRenderPass(c.command,&begin,VK_SUBPASS_CONTENTS_INLINE);for(auto& draw:draws)probe.Draw(c.command,draw);vkCmdEndRenderPass(c.command);
   }
   VkBufferImageCopy read{};read.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};read.imageExtent={width,height,1};vkCmdCopyImageToBuffer(c.command,output.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback.buffer,1,&read);c.Submit();
   const float* pixels=(const float*)readback.mapped;
   for(uint32_t i=0;i<3;++i){const float* value=pixels+((6+i*12)*width+10)*4;
    CHECK(std::abs(value[2]-.75f)<.000003f);CHECK(std::abs(value[3]-expected[i])<.000003f);}
   std::cout<<"Low, middle and high exposure ranges match independently calculated view values\n";
  }
  vkDestroyFramebuffer(c.device,framebuffer,nullptr);vkDestroyRenderPass(c.device,render_pass,nullptr);CHECK(c.validation_errors==0);
  std::cout<<"World-color regions, GPU snapshots, endian formats, padding, PWL output and fence retirement passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
