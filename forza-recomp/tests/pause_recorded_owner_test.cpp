#include "../src/platform/pause_scene_protocol.h"
#include "../src/platform/pause_recorded_protocol.h"
#include "../src/platform/pause_fast_record_witness.h"
#include "../src/platform/pause_record_admission.h"
#include <map>
#include <cassert>
namespace rec=fh1::recorded;
using forza::record_owner::Witness;
int main(){
  constexpr uint32_t device=0x40010000,list=0x40050000,reader=0x40070000;
  const rec::Retention expected{device,0xA0408000,722,721};
  std::map<uint32_t,uint32_t> words{{device+13760,0},{device+13772,list},{device+13776,list+0x1000},{device+11024,reader},{reader+4,722},{device+13920,0},{0x83409830,0}};
  auto read=[&](uint32_t a,uint32_t& v){auto i=words.find(a);if(i==words.end())return false;v=i->second;return true;};
  auto physical=[](uint32_t a){return a&0x1FFFFFFFu;};
  rec::AllocatorState old;old.v={0xA040F000,0xA040FFFC,0,expected.anchor,722,721,0xA040C000};
  Witness witness;witness.Start(device,expected,read);assert(witness.Allow(device,old,false,read));witness.Commit(device,old,0xA040F004,44,read,physical);
  words[list]=0x81001000;words[list+4]=0x0040C000;words[device+13772]=list+8;
  auto next=old;next.v[0]=0xA03FFFFC;next.v[1]=0xA0400FBC;next.v[5]=722;next.v[6]=0xA0400000;
  const auto original=witness;assert(witness.Allow(device,next,false,read));witness.Commit(device,next,0xA0400000,44,read,physical);
  words[list+8]=0x8100000B;words[list+12]=0x00400000;words[device+13772]=list+16;
  assert(witness.Allow(device,next,true,read));
  auto bad=next;bad.v[5]=721;assert(!witness.Allow(device,bad,false,read));bad=next;bad.v[5]=723;assert(!witness.Allow(device,bad,false,read));
  for(unsigned test=0;test<9;++test){auto w=original;auto state=next;const auto saved=words;
    switch(test){case 0:++state.v[3];break;case 1:++state.v[4];break;case 2:words[device+13920]=4;break;case 3:words[0x83409830]=1;break;case 4:words[reader+4]=720;break;case 5:words[device+13760]=4;break;case 6:words[device+13776]-=8;break;case 7:words[list]=0x82001000;break;case 8:state.v[1]=expected.anchor+4;break;}
    assert(!w.Allow(device,state,false,read));words=saved;
  }
  const std::string identity="1,2,3,4,5,6,7,8";using forza::record_admission::Certificate;
  assert(Certificate(7,1,1,identity,identity)=="7,1,"+identity);
  assert(Certificate(7,1,2,identity,identity).empty());assert(Certificate(0,1,1,identity,identity).empty());
  assert(Certificate(7,1,1,identity,identity+",9").empty());assert(Certificate(7,0,0,identity,identity).empty());
}
