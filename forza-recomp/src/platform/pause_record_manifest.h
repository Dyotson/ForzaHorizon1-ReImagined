#pragma once
#include <array>
#include <atomic>
#include <cstdint>
namespace fh1::record_manifest {
struct Budget {
  std::atomic<uint32_t> lines{0};
  bool Take(bool enabled){if(!enabled)return false;auto n=lines.load();while(n<2048){if(lines.compare_exchange_weak(n,n+1))return true;}return false;}
};
struct NativeList {uint32_t head=0,tail=0,capacity=0,cursor=0,limit=0,owner=0;};
enum class Status:uint32_t {Empty,Complete,Invalid,Unreadable,Cycle,WordCap,BlockCap,OutputCap};
struct Result {Status status=Status::Invalid;uint32_t blocks=0,words=0;};
inline bool Physical(uint32_t p){return p&&p<0x20000000u&&!(p&3);}
// Native82425850: block[0] encodes this block's data END as physical-40000000;
// the word at that end encodes NEXT block head the same way. Last block end
// comes from list.cursor before8249C358 finalizes it. No descriptor is executed
// or guessed: preserve raw list words for source-checked offline decoding.
template<class IO,class Sink> Result Snapshot(IO& io,Sink& sink,const NativeList& list,uint32_t max_words) {
  Result result;
  if(!list.head&&!list.tail&&!list.cursor){result.status=Status::Empty;return result;}
  if(list.capacity<8||list.capacity>65536||(list.capacity&3)||!max_words)return result;
  uint32_t head=io.Physical(list.head),tail=io.Physical(list.tail),cursor=io.Physical(list.cursor),limit=io.Physical(list.limit);
  if(!Physical(head)||!Physical(tail)||!Physical(cursor)||!Physical(limit))return result;
  std::array<uint32_t,4> seen{};
  while(result.blocks<seen.size()){
    for(uint32_t i=0;i<result.blocks;++i)if(seen[i]==head){result.status=Status::Cycle;return result;}
    seen[result.blocks]=head;uint32_t encoded_end=0;
    if(!io.ReadPhysical(head,encoded_end)){result.status=Status::Unreadable;return result;}
    const bool last=head==tail;const uint32_t end=last?cursor:encoded_end+0x40000000u;
    if(!Physical(end)||uint64_t(head)+4>end||uint64_t(end)+4>uint64_t(head)+list.capacity||
       (last&&(cursor>limit||uint64_t(limit)+4>uint64_t(tail)+list.capacity)))return result;
    if(!sink.Block(result.blocks,head,end,encoded_end,last)){result.status=Status::OutputCap;return result;}
    ++result.blocks;
    for(uint32_t position=head+4;position<end;){
      std::array<uint32_t,8> words{};uint32_t count=0;
      while(count<words.size()&&position+count*4<end){
        if(result.words+count==max_words){result.status=Status::WordCap;return result;}
        if(!io.ReadPhysical(position+count*4,words[count])){result.status=Status::Unreadable;return result;}
        ++count;
      }
      if(!sink.Words(position,words,count)){result.status=Status::OutputCap;return result;}
      result.words+=count;position+=count*4;
    }
    if(last){result.status=Status::Complete;return result;}
    uint32_t next=0;if(!io.ReadPhysical(end,next)){result.status=Status::Unreadable;return result;}
    head=next+0x40000000u;if(!Physical(head))return result;
  }
  result.status=Status::BlockCap;return result;
}
template<class Observer,class Original> void ForwardObserved(Observer& observer,Original&& original){
  observer.Before();struct Exit{Observer& observer;~Exit(){observer.After();}}exit{observer};
  original(); // Read failure/disabled/cap cannot change this single forward.
}
} // namespace fh1::record_manifest
