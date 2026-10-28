#include <rex/logging.h>
#include "../src/platform/guest_write_transaction.h"
#include <cassert>
#include <cstdio>
namespace m=rex::memory;
static uint8_t* arena;
static constexpr uint32_t device=0x40010000,start=0xA0400000;
static void Put(uint32_t at,uint32_t v){auto* p=m::GuestPtr(arena,at);p[0]=uint8_t(v>>24);p[1]=uint8_t(v>>16);p[2]=uint8_t(v>>8);p[3]=uint8_t(v);}
static unsigned invalidations=0;static m::Memory* memory;static bool change_permission=false;
static std::pair<uint32_t,uint32_t> Invalidate(void*,uint32_t at,uint32_t bytes,bool exact){++invalidations;assert(exact&&bytes);if(change_permission){assert(memory->LookupHeap(start)->Protect(start,0x10000,m::kMemoryProtectRead));}return {at,bytes};}
static void SDKRegression(m::Memory& mem){
 memory=&mem;auto* heap=mem.LookupHeap(start);const uint32_t rw=m::kMemoryProtectRead|m::kMemoryProtectWrite;auto* callback=mem.RegisterPhysicalMemoryInvalidationCallback(Invalidate,nullptr);
 const auto watch=[&](uint32_t at,uint32_t bytes){mem.EnablePhysicalMemoryAccessCallbacks(mem.GetPhysicalAddress(at),bytes,true,false);};
 {forza::guest_write::Transaction t;assert(t.Begin(&mem,arena,start,44,device+48));Put(start,0x1234);Put(device+48,start+40);t.End();}
 watch(start,44);size_t length=0;m::PageAccess access;assert(m::QueryProtect(m::GuestPtr(arena,start),length,access)&&access==m::PageAccess::kReadOnly);const unsigned before=invalidations;
 {forza::guest_write::Transaction t;assert(t.Begin(&mem,arena,start,44,device+48));assert(invalidations==before+1);assert(m::QueryProtect(m::GuestPtr(arena,start),length,access)&&access==m::PageAccess::kReadWrite);Put(start,0xabcdef);Put(device+48,start+40);t.End();}
 assert(heap->Protect(start,0x10000,m::kMemoryProtectRead));{forza::guest_write::Transaction t;const auto old=invalidations;assert(!t.Begin(&mem,arena,start,44,device+48));assert(invalidations==old);}
 assert(heap->Protect(start,0x10000,rw));watch(start+4096,44);change_permission=true;{forza::guest_write::Transaction t;assert(!t.Begin(&mem,arena,start+4096,44,device+48));}change_permission=false;assert(heap->Protect(start,0x10000,rw));
 {forza::guest_write::Transaction t;assert(!t.Begin(&mem,arena,0xA0600000,44,device+48));assert(!t.Begin(&mem,arena+32,start,44,device+48));assert(!t.Begin(&mem,arena,start,44,0x7f000000));assert(!t.Begin(&mem,arena,0xfffffff0,44,device+48));}
 m::HeapAllocationInfo free_region{};assert(heap->QueryRegionInfo(0xA2000000,&free_region)&&free_region.state==0);assert(heap->AllocFixed(0xA2000000,0x10000,0x10000,m::kMemoryAllocationReserve,rw));m::HeapAllocationInfo reserved{};assert(heap->QueryRegionInfo(0xA2000000,&reserved)&&(reserved.state&m::kMemoryAllocationReserve)&&!(reserved.state&m::kMemoryAllocationCommit));{forza::guest_write::Transaction t;assert(!t.Begin(&mem,arena,0xA2000000,44,device+48));}
 auto* fine=mem.LookupHeap(0xE0800000);assert(fine->AllocFixed(0xE0800000,0x2000,0x1000,m::kMemoryAllocationReserve|m::kMemoryAllocationCommit,rw));assert(fine->Protect(0xE0801000,0x1000,m::kMemoryProtectRead));{forza::guest_write::Transaction t;assert(!t.Begin(&mem,arena,0xE0800ff0,44,device+48));}assert(fine->Protect(0xE0801000,0x1000,rw));watch(0xE0800ff0,44);{forza::guest_write::Transaction t;assert(t.Begin(&mem,arena,0xE0800ff0,44,device+48));t.End();}
 mem.UnregisterPhysicalMemoryInvalidationCallback(callback);std::puts("PASS actual SDK watched physical RO->callback->RW; real guest RO/reserved/unowned/cross-page/changed-permission reject; no direct host protection or writable alias");
}
int main(){rex::InitLogging();m::Memory mem;assert(mem.Initialize());arena=mem.virtual_membase();const auto rw=m::kMemoryProtectRead|m::kMemoryProtectWrite;assert(mem.LookupHeap(device)->AllocFixed(device,0x10000,0x10000,m::kMemoryAllocationReserve|m::kMemoryAllocationCommit,rw));assert(mem.LookupHeap(start)->AllocFixed(start,0x10000,0x10000,m::kMemoryAllocationReserve|m::kMemoryAllocationCommit,rw));SDKRegression(mem);}
