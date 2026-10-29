#pragma once
#include <array>
#include <cstdint>
namespace fh1::writer_diag {
enum class Reason:uint32_t {None,NestedRender,ScopeDepth,ScopeEnd,TargetColor,TargetDepth,TargetColorDesc,TargetDepthDesc,TargetBinding,TargetColorAux,TargetDepthAux,BeforeUnreadable,BeforeRetention,BeforeFlags,PayloadProtection,CommitProtection,CommitBounds,ProducerBase,EmitBase=40,Count=64};
struct Attempt {uint32_t native_proof=0,op=0,serial=0,phase=0,depth=0,returned=0,address=0,width=0,before_mask=0,after_mask=0;bool before_taken=false,after_taken=false,before_readable=false,after_readable=false;std::array<uint32_t,7> before{},after{};};
struct Record {uint32_t uid=0,events=0,first=0,last=0;bool emitted=false;std::array<uint32_t,64> counts{};Attempt current{},first_attempt{};
 void Start(uint32_t id){*this={};uid=id;}
 void Begin(uint32_t op,uint32_t serial,uint32_t phase,uint32_t depth){current={};current.op=op;current.serial=serial;current.phase=phase;current.depth=depth;}
 void Note(uint32_t reason,uint32_t address=0,uint32_t width=0){if(!uid||!reason||reason>=counts.size())return;current.address=address;current.width=width;if(!events){first=reason;first_attempt=current;}last=reason;if(events!=UINT32_MAX)++events;if(counts[reason]!=UINT32_MAX)++counts[reason];}
};
}
