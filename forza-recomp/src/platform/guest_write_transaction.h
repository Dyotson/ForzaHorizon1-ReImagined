#pragma once
#include "guest_protected_range.h"
#include <rex/thread/mutex.h>
#include <mutex>
namespace forza::guest_write {
// Owner/cursor/revision validation belongs to the caller after Begin returns.
// Keep this guard through all stores, then release immediately after commit.
class Transaction {
 std::unique_lock<std::recursive_mutex> lock_;
 rex::memory::Memory* memory_=nullptr;uint8_t* base_=nullptr;
 bool Guest(uint32_t at,uint32_t bytes){
  const uint64_t end=uint64_t(at)+bytes;if(!bytes||at<0x100000||end>0xF0000000ull)return false;
  for(uint64_t p=at;p<end;){auto* heap=memory_->LookupHeap(uint32_t(p));if(!heap)return false;
   rex::memory::HeapAllocationInfo info{};uint32_t protection=0;
   if(!heap->QueryRegionInfo(uint32_t(p),&info)||!(info.state&rex::memory::kMemoryAllocationCommit)||!heap->QueryProtect(uint32_t(p),&protection)||
      (protection&(rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite))!=(rex::memory::kMemoryProtectRead|rex::memory::kMemoryProtectWrite))return false;
   const uint64_t next=uint64_t(info.base_address)+info.region_size;if(next<=p)return false;p=std::min(end,next);
  }return true;
 }
 bool Prepare(uint32_t at,uint32_t bytes){
  const size_t page=rex::memory::page_size();const uint64_t end=uint64_t(at)+bytes;
  for(uint64_t p=at;p<end;){auto* host=GuestPtr(base_,uint32_t(p));size_t length=0;rex::memory::PageAccess access;
   if(!rex::memory::QueryProtect(host,length,access))return false;
   const auto n=uint32_t(std::min(end-p,uint64_t(page-reinterpret_cast<uintptr_t>(host)%page)));
   if(access!=rex::memory::PageAccess::kReadWrite){
    auto* heap=memory_->LookupHeap(uint32_t(p));
    if(access!=rex::memory::PageAccess::kReadOnly||!heap||heap->heap_type()!=rex::memory::HeapType::kGuestPhysical)return false;
    // This consumes the depth-one global guard. SDK invalidates caches and
    // only unwatches pages whose CURRENT guest permission permits writing.
    const bool watched=memory_->TriggerPhysicalMemoryCallbacks(std::move(lock_),uint32_t(p),n,true,true,true);
    lock_=rex::thread::global_critical_region::AcquireDirect();
    if(!watched)return false;++callbacks;
   }
   p+=n;
  }return true;
 }
 public:
 uint32_t callbacks=0;
 bool Begin(rex::memory::Memory* memory,uint8_t* base,uint32_t payload,uint32_t bytes,uint32_t commit){
  End();callbacks=0;memory_=memory;base_=base;
  if(!memory||base!=memory->virtual_membase())return false;
  lock_=rex::thread::global_critical_region::AcquireDirect();
  if(!Guest(payload,bytes)||!Guest(commit,4)||!Prepare(payload,bytes)||!Prepare(commit,4)||
     !Guest(payload,bytes)||!Guest(commit,4)||!protected_range::Allowed(base,payload,bytes,2)||!protected_range::Allowed(base,commit,4,2)){End();return false;}
  return true;
 }
 void End(){if(lock_.owns_lock())lock_.unlock();}
};
}
