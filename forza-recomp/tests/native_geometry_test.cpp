#include "../src/platform/native_renderer/geometry.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace forza::native_renderer;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
int main() {
  try {
    GeometryParameters p; p.layout[2]=1; CHECK(Valid(p));
    CHECK(GeometrySnorm(0)==0); CHECK(GeometrySnorm(32767)==1);
    CHECK(GeometrySnorm(0x8000)==-1); CHECK(GeometrySnorm(0xffff)<0);
    CHECK(GeometryEndian(0x11223344,0)==0x11223344);
    CHECK(GeometryEndian(0x11223344,1)==0x22114433);
    CHECK(GeometryEndian(0x11223344,2)==0x44332211);
    CHECK(GeometryEndian(0x11223344,3)==0x33441122);
    // Closed-form corners check unpacking, quantization, scale and translation.
    const std::array<uint32_t,2> packed{0x7fff8000,0x00007fff};
    for(uint32_t endian=0;endian<4;++endian) {
      p.layout[1]=endian;
      const auto position=GeometryPosition(p,{GeometryEndian(packed[0],endian),GeometryEndian(packed[1],endian)});
      CHECK(position==(GeometryVector{1,-1,0,1}));
    }
    p.layout[1]=0; p.origin_scale={4,5,6,2};
    p.clip_columns={{{2,0,0,0},{0,3,0,0},{0,0,4,0},{1,2,3,1}}};
    p.ndc_scale={1,-1,1,0};p.ndc_offset={0.25f,0.5f,0,0};
    CHECK(GeometryPosition(p,packed)==(GeometryVector{13.25f,-10.5f,27,1}));
    auto bad=p;bad.layout[0]=1;CHECK(!Valid(bad));
    bad=p;bad.layout[2]=0;CHECK(!Valid(bad));
    bad=p;bad.layout[3]=4;CHECK(!Valid(bad));
    bad=p;bad.ndc_offset[3]=1;CHECK(!Valid(bad));
    bad=p;bad.clip_columns[0][0]=std::numeric_limits<float>::quiet_NaN();CHECK(!Valid(bad));
    bad=p;bad.origin_scale[3]=std::numeric_limits<float>::max();CHECK(!Valid(bad));
    std::cout<<"Native rigid geometry checks passed\n";return 0;
  } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
