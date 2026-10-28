#pragma once
#include "guest_memory.h"
#include <rex/memory/utils.h>
#include <algorithm>
#include <cstdint>
#include <limits>
namespace forza::protected_range {
// Query each touched translated host page. SDK RegionSize lacks BaseAddress,
// so it must not be consumed as a length starting at an arbitrary pointer.
template<class Translate,class Query> bool CheckPages(uint32_t address,uint32_t bytes,uint32_t required,size_t page,Translate&& translate,Query&& query){
  const uint64_t end=uint64_t(address)+bytes;
  if(address<0x100000u||end>0xF0000000ull||!page||(required!=1&&required!=2))return false;
  uint64_t current=address;
  while(current<end){auto* host=translate(uint32_t(current));if(!host||!query(host,required))return false;
    uint64_t step=std::min(end-current,uint64_t(page-(reinterpret_cast<uintptr_t>(host)%page)));
    if(current<0xE0000000ull)step=std::min(step,0xE0000000ull-current);
    if(!step)return false;current+=step;
  }
  return true;
}
inline bool Allowed(uint8_t* base,uint32_t address,uint32_t bytes,uint32_t required){
  const uint64_t end=uint64_t(address)+bytes;
  if(!base||end>0xF0000000ull)return false;
  const uint64_t last=bytes?end-1:address;
  const uint64_t offset=last+rex::memory::detail::PhysicalHostOffset(uint32_t(last));
  if(offset>std::numeric_limits<uintptr_t>::max()-reinterpret_cast<uintptr_t>(base))return false;
  return CheckPages(address,bytes,required,rex::memory::page_size(),[&](uint32_t at){return GuestPtr(base,at);},[](uint8_t* p,uint32_t need){size_t region=0;rex::memory::PageAccess access;return rex::memory::QueryProtect(p,region,access)&&(uint32_t(access)&need)==need;});
}
}
