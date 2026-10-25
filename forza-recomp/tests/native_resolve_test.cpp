#include "../src/platform/native_renderer/resolve.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace forza::native_renderer;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
int main() {
  try {
    CHECK(Tiled32Offset(0,0,64)==0);
    CHECK(Tiled32Offset(4,0,64)==32);
    CHECK(Tiled32Offset(8,0,64)==64);
    CHECK(Tiled32Offset(0,1,64)==16);
    CHECK(Tiled32Offset(0,2,64)==256);
    CHECK(Tiled32Offset(0,8,64)==1152);
    CHECK(Tiled32Offset(0,16,64)==2048);
    CHECK(Tiled32Offset(32,0,64)==4096);
    CHECK(Tiled32Offset(0,32,64)==8192);
    // Independently check byte permutations, including cross-word swaps.
    std::array<std::array<uint8_t,4>,4> input{};
    for(uint32_t i=0;i<16;++i) input[i/4][i%4]=uint8_t(i);
    const uint32_t groups[]={1,2,4,4,8,16};
    for(uint32_t endian=0;endian<6;++endian) {
      const auto encoded=EncodeResolveQuad(input,endian,false);
      for(uint32_t i=0;i<16;++i) {
        const uint32_t group=groups[endian];
        uint32_t original=(i/group)*group+group-1-i%group;
        if(endian==3) original=i^2;
        CHECK(uint8_t(encoded[i/4]>>((i%4)*8))==original);
      }
    }
    for(uint32_t sx=1;sx<=7;++sx) for(uint32_t sy=1;sy<=7;++sy) {
      ResolveParameters p;
      p.source={3,5,32*sx,24*sy};p.destination={8,8,64,32};p.layout={sx,sy,2,1};
      p.bounds={uint32_t(ResolveBufferBytes(p)),p.source[2]+8,p.source[3]+8,0};
      CHECK(Valid(p));
      std::set<uint64_t> addresses;
      for(uint32_t y=0;y<p.source[3];++y) for(uint32_t q=0;q<p.source[2]/4;++q) {
        const auto address=ResolveQuadOffset(p,q,y);
        CHECK(address%16==0 && address+16<=p.bounds[0]);
        CHECK(addresses.insert(address).second);
      }
      auto invalid=p;invalid.bounds[0]-=16;CHECK(!Valid(invalid));
      invalid=p;invalid.source[0]=invalid.bounds[1];CHECK(!Valid(invalid));
      invalid=p;invalid.layout[0]=0;CHECK(!Valid(invalid));
      invalid=p;invalid.layout[2]=6;CHECK(!Valid(invalid));
      invalid=p;invalid.destination[0]=1;CHECK(!Valid(invalid));
      invalid=p;invalid.bounds[3]=2;CHECK(!Valid(invalid));
      // Edge filling is relative to the source rectangle, including at odd
      // scales. Interior texels and the destination mapping are unaffected.
      p.bounds[3]=1;CHECK(Valid(p));
      CHECK(ResolveSourcePixel(p,0,0)==(std::array<uint32_t,2>{3+sx/2,5+sy/2}));
      CHECK(ResolveSourcePixel(p,10,0)==(std::array<uint32_t,2>{13,5+sy/2}));
      CHECK(ResolveSourcePixel(p,0,12)==(std::array<uint32_t,2>{3+sx/2,17}));
      CHECK(ResolveSourcePixel(p,10,12)==(std::array<uint32_t,2>{13,17}));
      p.bounds[3]=0;
      CHECK(ResolveSourcePixel(p,0,0)==(std::array<uint32_t,2>{3,5}));
    }
    std::cout<<"Native RGBA8 resolve layout checks passed\n";
    return 0;
  } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
