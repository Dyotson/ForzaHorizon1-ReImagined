#include "../src/platform/native_renderer/world_color.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace forza::native_renderer;
#define CHECK(x) do{if(!(x))throw std::runtime_error(#x);}while(false)
int main(){try{
  WorldColorParameters p;
  CHECK(!Valid(p));
  p.view[2]={0,.25f,.5f,1};
  p.view[5]={1,.5f,.25f,0};p.view[7]={.125f,.25f,.125f,0};
  p.exposure_bias_scale[2]=1;p.color[12][1]=.02f;
  CHECK(Valid(p));
  for(uint32_t e=0;e<4;++e){p.rules[0]=e;CHECK(Valid(p));}
  p.rules[0]=4;CHECK(!Valid(p));p.rules[0]=0;
  p.rules[1]=2;CHECK(!Valid(p));p.rules[1]=0;
  for(uint32_t index:{2u,3u}){p.rules[index]=1;CHECK(!Valid(p));p.rules[index]=0;}
  p.color[7][3]=std::numeric_limits<float>::infinity();CHECK(!Valid(p));p.color[7][3]=0;
  p.view[2][1]=p.view[2][2];CHECK(!Valid(p));p.view[2][1]=.25f;
  p.view[2][2]=p.view[2][3];CHECK(!Valid(p));p.view[2][2]=.5f;
  p.ndc_offset[0]=std::numeric_limits<float>::quiet_NaN();CHECK(!Valid(p));p.ndc_offset[0]=0;
  for(auto* field:{&p.scene_noise_bias,&p.bloom_lut_bias,&p.input_scale,&p.exposure_bias_scale}){
    auto old=(*field)[1];(*field)[1]=std::numeric_limits<float>::quiet_NaN();CHECK(!Valid(p));(*field)[1]=old;
  }
  CHECK(Valid(p));std::cout<<"World-color parameter bounds and region snapshots validated\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
