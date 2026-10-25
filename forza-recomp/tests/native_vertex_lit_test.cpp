#include "../src/platform/native_renderer/vertex_lit.h"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace forza::native_renderer;
#define CHECK(x) do {if(!(x))throw std::runtime_error(#x);}while(false)
int main() {
  try {
    VertexLitParameters p;CHECK(Valid(p));
    CHECK(ShadeVertexLit({0,0.25f,1},p)==(std::array<float,4>{0,0.5f,1,1}));
    p.fog={1,4,9,1};p.output_color[0]=2;p.material[0]=0.25f;
    CHECK(ShadeVertexLit({16,25,36},p)==(std::array<float,4>{2,4,6,0.5f}));
    p.fog={0,0,0,0};p.output_color[0]=1;p.modulation={-1,4,9,0};
    CHECK(ShadeVertexLit({1,1,1},p)==(std::array<float,4>{1,2,3,0.25f}));
    p=VertexLitParameters{};p.fog={0.5f,1.75f,4,0.5f};
    CHECK(ShadeVertexLit({0,0.25f,4},p)==(std::array<float,4>{0.5f,1,2,1}));
    const float nan=std::numeric_limits<float>::quiet_NaN();
    CHECK(VertexLitMultiply(0,nan)==0 && VertexLitMultiply(nan,0)==0);
    for(uint32_t comparison=0;comparison<8;++comparison) {
      CHECK(VertexLitAlphaPass(0.25f,0.5f,comparison)==bool(comparison&1));
      CHECK(VertexLitAlphaPass(0.5f,0.5f,comparison)==bool(comparison&2));
      CHECK(VertexLitAlphaPass(0.75f,0.5f,comparison)==bool(comparison&4));
      CHECK(VertexLitAlphaPass(nan,0.5f,comparison)==(comparison==5 || comparison==7));
    }
    CHECK(VertexLitCoverage(0,0,0,4,0)==UINT32_MAX);
    CHECK(VertexLitCoverage(0.24f,0,0,4,256)==0);
    CHECK(VertexLitCoverage(0.25f,0,0,4,256)==4);
    CHECK(VertexLitCoverage(0.5f,0,0,4,256)==6);
    CHECK(VertexLitCoverage(0.75f,0,0,4,256)==7);
    CHECK(VertexLitCoverage(1,0,0,4,256)==15);
    CHECK(VertexLitCoverage(0.5f,0,0,2,256)==2);
    CHECK(VertexLitCoverage(1,0,0,2,256)==3);
    CHECK(VertexLitCoverage(0.9f,0,0,1,256)==0);
    CHECK(VertexLitCoverage(0.9f,0,0,1,259)==1);
    CHECK(VertexLitPwlGamma(-1)==0 && VertexLitPwlGamma(nan)==0);
    CHECK(VertexLitPwlGamma(1)==1 && VertexLitPwlGamma(4)==1);
    p=VertexLitParameters{};auto bad=p;bad.fog[0]=nan;CHECK(!Valid(bad));
    bad=p;bad.output_rules[1]=3;CHECK(!Valid(bad));bad=p;bad.output_rules[2]=512;CHECK(!Valid(bad));
    bad=p;bad.output_color[0]=-1;CHECK(!Valid(bad));bad=p;bad.material[2]=1;CHECK(!Valid(bad));
    std::cout<<"Native vertex-lit color, fog, alpha, coverage and gamma checks passed\n";return 0;
  }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
