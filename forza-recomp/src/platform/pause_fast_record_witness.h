#pragma once
#include <array>
#include <cstdint>
#include <algorithm>
namespace forza::record_owner {
namespace rec=fh1::recorded;
enum class Failure:uint32_t {None,Owner,Transition,Range,Variant,Global,Metadata,Reader,Membership,Limit};
struct Metadata {uint32_t head=0,cursor=0,limit=0,reader=0,reader_word=0;};
class Witness {
 struct Span {uint32_t ring=0,slab=0,begin=0,end=0;};
 std::array<Span,64> spans_{};uint32_t count_=0;rec::Retention expected_{};
 Metadata initial_{},previous_{};bool started_=false,tracked_=true,rotated_=false;
 bool Bad(Failure why){failure=why;return false;}
 template<class Read> bool Meta(uint32_t device,Read&& read,Metadata& out){
  return uint64_t(device)+15336<=0xF0000000ull&&read(device+13760,out.head)&&read(device+13772,out.cursor)&&read(device+13776,out.limit)&&read(device+11024,out.reader)&&
    out.reader&&uint64_t(out.reader)+8<=0xF0000000ull&&read(out.reader+4,out.reader_word);
 }
 static bool Reader(uint32_t word,uint32_t ring,uint32_t end){const auto delta=(ring-word)&3;return delta==0||(delta==1&&end<=(word&~3u));}
 template<class Read> bool Membership(const Metadata& now,bool all,Read&& read){
  if(now.head!=initial_.head||now.limit!=initial_.limit||now.reader!=initial_.reader||now.cursor<initial_.cursor||now.cursor>now.limit||
     (now.cursor-initial_.cursor)%8||now.cursor-initial_.cursor>4096)return Bad(Failure::Metadata);
  for(uint32_t s=0;s<count_;++s){
   if(!all&&spans_[s].ring==expected_.generation)continue;
   bool found=false;
   for(uint64_t at=initial_.cursor;at<now.cursor;at+=8){uint32_t header=0,physical=0;
    if(!read(uint32_t(at),header)||!read(uint32_t(at+4),physical)||(header&0xFF000000u)!=0x81000000u||!(header&0xFFFFFFu)||uint64_t(physical)+(header&0xFFFFFFu)*4ull>0x20000000ull)return Bad(Failure::Metadata);
    if(spans_[s].begin>=physical&&spans_[s].end<=uint64_t(physical)+(header&0xFFFFFFu)*4ull)found=true;
   }
   if(!found)return Bad(Failure::Membership);
  }
  return true;
 }
 public:
 Failure failure=Failure::None;
 template<class Read> void Start(uint32_t device,rec::Retention expected,Read&& read){
  *this={};expected_=expected;started_=Meta(device,read,initial_);previous_=initial_;
 }
 template<class Read> bool Allow(uint32_t device,const rec::AllocatorState& state,bool final,Read&& read){
  failure=Failure::None;const auto actual=rec::Retained(state,device);
  if(!state.readable||!expected_.device||!expected_.anchor||actual.device!=expected_.device||actual.anchor!=expected_.anchor||actual.generation!=expected_.generation)return Bad(Failure::Owner);
  if(actual.ring==expected_.ring)return rotated_?Bad(Failure::Transition):true;
  if(expected_.ring==UINT32_MAX||expected_.generation!=expected_.ring+1||actual.ring!=expected_.generation)return Bad(Failure::Transition);
  const auto cursor=state.v[0],hard=state.v[1],slab=state.v[6];
  if(!cursor||(cursor&3)||!slab||slab>uint64_t(cursor)+4||hard>expected_.anchor||cursor>=expected_.anchor||uint64_t(hard)+68>0xF0000000ull||(!final&&uint64_t(cursor)+44>hard))return Bad(Failure::Range);
  uint32_t variant=0,global=0;if(!read(device+13920,variant)||variant)return Bad(Failure::Variant);
  if(!read(0x83409830,global)||global)return Bad(Failure::Global);
  Metadata now{};if(!started_||!tracked_||!Meta(device,read,now)||previous_.cursor>previous_.limit||uint64_t(previous_.cursor)+8>previous_.limit||
    now.head!=initial_.head||now.limit!=initial_.limit||now.reader!=initial_.reader||now.cursor<previous_.cursor)return Bad(Failure::Metadata);
  const uint32_t allocation_end=hard+68;
  if(!Reader(previous_.reader_word,actual.ring,allocation_end)||!Reader(now.reader_word,actual.ring,allocation_end))return Bad(Failure::Reader);
  if(!rotated_&&now.cursor<=previous_.cursor)return Bad(Failure::Membership);
  if(!Membership(now,final,read))return false;
  rotated_=true;return true;
 }
 template<class Read,class Physical> void Commit(uint32_t device,const rec::AllocatorState& state,uint32_t address,uint32_t bytes,Read&& read,Physical&& physical){
  const uint32_t begin=physical(address);const uint64_t end=uint64_t(begin)+bytes;
  if(begin==UINT32_MAX||end>0x20000000ull||!bytes){tracked_=false;return;}
  uint32_t slot=count_;
  for(uint32_t i=0;i<count_;++i)if(spans_[i].ring==state.v[5]&&spans_[i].slab==state.v[6]){slot=i;break;}
  if(slot==count_){if(count_==spans_.size()){tracked_=false;failure=Failure::Limit;return;}spans_[count_++]={state.v[5],state.v[6],begin,uint32_t(end)};}
  else {spans_[slot].begin=std::min(spans_[slot].begin,begin);spans_[slot].end=std::max(spans_[slot].end,uint32_t(end));}
  Metadata now{};if(!Meta(device,read,now)){tracked_=false;return;}previous_=now;
 }
};
}
