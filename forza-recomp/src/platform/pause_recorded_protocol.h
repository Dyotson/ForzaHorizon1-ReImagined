#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <cstddef>
// Include the existing provenance protocol first. Separate wire magic and
// consumer: recorded replay never weakens the immediate protocol's serial rules.
namespace fh1::recorded {
namespace p=fh1::provenance;
constexpr uint32_t kMagic=0x46485236,kVersion=6,kWords=10,kBytes=44;
using Record=p::Record;using Op=p::Op;using Phase=p::Phase;
constexpr Phase kMainTrack=Phase(6),kMainModels=Phase(7),kMainVegetation=Phase(8),kMainCharacters=Phase(9),kMainSkinnedCrowd=Phase(10),kMainCrowdComponent=Phase(11),kMainParticles=Phase(12);
constexpr bool CrowdPhase(Phase phase){return phase==kMainSkinnedCrowd||phase==kMainCrowdComponent;}
constexpr uint32_t LeafEntry(Phase phase){return phase==kMainModels?0x82447B70u:phase==kMainVegetation?0x824436A0u:phase==kMainCharacters?0x8248A270u:0u;}constexpr uint32_t kSurface24=0x53524631;
inline auto Encode(const Record& r){auto w=p::Encode(r);w[0]=kMagic;w[1]=(kVersion<<16)|uint16_t(r.op);return w;}
struct Retention {uint32_t device=0,anchor=0,generation=0,ring=0;bool operator==(const Retention&)const=default;};
struct AllocatorState {std::array<uint32_t,7> v{};bool readable=true;};
inline constexpr std::array<uint32_t,7> kAllocatorOffsets{48,52,11068,13600,13604,15324,15332};
template<class Read> AllocatorState ReadAllocatorState(Read&& read){AllocatorState out;for(size_t i=0;i<kAllocatorOffsets.size();++i)out.readable&=read(kAllocatorOffsets[i],out.v[i]);return out;}
inline Retention Retained(const AllocatorState& s,uint32_t device){return {device,s.v[3],s.v[4],s.v[5]};}
template<class Context,class Forward> uint32_t ReserveCopied(const Context& original,uint32_t device,uint32_t words,Forward&& forward){Context copy=original;copy.r3.u64=device;copy.r4.u64=words;forward(copy);return copy.r3.u32;}
template<class Setter> void Revoke(uint32_t current,Setter&& set){if(current)set(0u);}
// One pending record, one published UID. No address table or lifetime list.
enum class SealFailure:uint32_t {None,NoUID,Failed,Unclosed,Submitted,WorldActive,Flags,Retention};
enum class ProducerFailure:uint32_t {None,NoUID,Failed,Closed,Token,DuplicateFrame,ZeroFrame,MissingFrame,FrameEndDepth,FrameEndSerial,EndEmpty,EndSerial,SerialOrder,Depth};
class Producer {
 public:
  uint32_t uid=0;Retention retention{};bool failed=false,closed=false,submitted=false;ProducerFailure failure=ProducerFailure::None;SealFailure seal_failure=SealFailure::None;
  bool Start(uint32_t id,Retention r){*this={};if(!id||!r.device||!r.anchor)return false;uid=id;retention=r;return true;}
  void Fail(){failed=true;}
  bool Observe(const Record& r){
    const auto bad=[&](ProducerFailure why){failure=why;Fail();return false;};
    if(!uid)return bad(ProducerFailure::NoUID);if(failed)return bad(ProducerFailure::Failed);if(closed)return bad(ProducerFailure::Closed);if(r.token!=uid)return bad(ProducerFailure::Token);
    if(r.op==Op::FrameBegin){if(begun_)return bad(ProducerFailure::DuplicateFrame);if(!r.serial)return bad(ProducerFailure::ZeroFrame);begun_=true;frame_=last_=r.serial;return true;}
    if(!begun_)return bad(ProducerFailure::MissingFrame);
    if(r.op==Op::FrameEnd){if(depth_)return bad(ProducerFailure::FrameEndDepth);if(r.serial!=frame_)return bad(ProducerFailure::FrameEndSerial);closed=true;return true;}
    if(r.op==Op::End){if(!depth_)return bad(ProducerFailure::EndEmpty);if(stack_[depth_-1]!=r.serial)return bad(ProducerFailure::EndSerial);--depth_;return true;}
    if(r.serial<=last_)return bad(ProducerFailure::SerialOrder);last_=r.serial;
    if(r.op==Op::Begin){if(depth_==stack_.size())return bad(ProducerFailure::Depth);stack_[depth_++]=r.serial;}
    return true;
  }
  template<class Match> uint32_t SealChecked(Retention current,bool world_active,uint32_t flags,Match&& match){
    const auto bad=[&](SealFailure why){seal_failure=why;return 0u;};
    if(!uid)return bad(SealFailure::NoUID);if(failed)return bad(SealFailure::Failed);if(!closed)return bad(SealFailure::Unclosed);if(submitted)return bad(SealFailure::Submitted);if(world_active)return bad(SealFailure::WorldActive);if(flags&0x80200000u)return bad(SealFailure::Flags);if(!match(retention,current))return bad(SealFailure::Retention);
    seal_failure=SealFailure::None;submitted=true;return uid;
  }
  uint32_t Seal(Retention current,bool world_active,uint32_t flags=0){return SealChecked(current,world_active,flags,[](const Retention&a,const Retention&b){return a==b;});}
 private:
  uint32_t frame_=0,last_=0;size_t depth_=0;bool begun_=false;std::array<uint32_t,16> stack_{};
};
// Reserve is an exactly-once native allocator call. Allocation may flush/rotate;
// validation therefore reads state AFTER it returns. Failed writes never commit.
enum class EmitFailure:uint32_t {None,Disabled,Snapshot,NullCursor,Alignment,Cursor,Flags,Retention,Capacity,Writable};
template<class IO,class Report,class Match> bool EmitValidated(IO& io,bool enabled,const Record& r,Retention expected,Report&& report,Match&& match){
  const auto fail=[&](EmitFailure reason){report(reason);return false;};
  if(!enabled)return fail(EmitFailure::Disabled);
  const uint32_t cursor=io.Reserve(kWords+1);
  uint32_t actual=0,hard=0,flags=0;Retention current;
  if(!io.Snapshot(actual,hard,flags,current))return fail(EmitFailure::Snapshot);
  if(!cursor)return fail(EmitFailure::NullCursor);
  if(cursor&3)return fail(EmitFailure::Alignment);
  if(actual!=cursor)return fail(EmitFailure::Cursor);
  if(flags&0x80200000u)return fail(EmitFailure::Flags);
  if(!match(expected,current))return fail(EmitFailure::Retention);
  if(uint64_t(cursor)+kBytes>hard)return fail(EmitFailure::Capacity);
  if(!io.Writable(cursor+4,kBytes))return fail(EmitFailure::Writable);
  const auto words=fh1::recorded::Encode(r);io.Store(cursor+4,p::kNop);
  for(uint32_t i=0;i<kWords;++i)io.Store(cursor+8+4*i,words[i]);
  io.Commit(cursor+kBytes);return true;
}
template<class IO,class Report> bool EmitChecked(IO& io,bool enabled,const Record& r,Retention expected,Report&& report){return EmitValidated(io,enabled,r,expected,report,[](const Retention&a,const Retention&b){return a==b;});}
template<class IO> bool Emit(IO& io,bool enabled,const Record& r,Retention expected){return EmitChecked(io,enabled,r,expected,[](EmitFailure){});}

constexpr uint32_t kSceneSource=0x53434E31u,kTrackType=0x82003CECu;
struct Context {
  uint32_t uid=0,frame=0,selected=0,graphics=0,core=0,camera=0,scene_root=0,scene_type=0,main_root=0,crowd_root=0,particle_root=0;uint64_t world_generation=0,replay=0;
  Phase phase=Phase::Unknown;uint32_t color=0,depth=0,cdesc=0,ddesc=0,binding=0,color_surface=0,depth_surface=0;bool valid=false,player_enclosing=false,other_car_enclosing=false,surface_ready=false;
};
class Consumer {
 public:
  struct EligibilityReceipt {uint32_t requested=0,before=0,retired=0,after=0;bool changed=false;const char* reason="unchanged";};
  EligibilityReceipt Eligibility(uint32_t uid){EligibilityReceipt r{uid,allowed_,retired_,allowed_};if(uid!=allowed_){if(allowed_>retired_)retired_=allowed_;Reset();allowed_=uid>retired_?uid:0;r.changed=true;r.reason=!uid?"revoked":allowed_?"accepted-uid":"retired-uid";}r.retired=retired_;r.after=allowed_;return r;}
  void Reset(){context_={};depth_=0;last_=0;fault_=false;}
  void Fault(){Reset();fault_=true;}
  uint32_t eligible()const{return allowed_;}bool fault()const{return fault_;}
  Context context()const{return context_;}
  bool EnterBuffer(){if(buffers_==8){Fault();return false;}++buffers_;return true;}
  void LeaveBuffer(){if(!buffers_)Fault();else --buffers_;}
  bool TargetMatches(uint32_t c,uint32_t d,uint32_t cd,uint32_t dd)const{return context_.valid&&context_.binding&&context_.color==c&&context_.depth==d&&context_.cdesc==cd&&context_.ddesc==dd;}
  void Consume(std::span<const uint32_t> w){
    if(!allowed_)return;
    if(w.size()!=kWords||w[0]!=kMagic||w[1]>>16!=kVersion||w[2]!=allowed_||!w[3]||w[4]>uint32_t(kMainParticles)){Fault();return;}
    const auto op=Op(uint16_t(w[1]));const auto phase=Phase(w[4]);const uint32_t serial=w[3];
    if(op==Op::FrameBegin){
      if(context_.valid||replay_==UINT64_MAX||!w[5]||!w[6]||!w[7]||!w[8]||!w[9]){Fault();return;}
      Reset();context_.uid=w[2];context_.frame=serial;last_=serial;context_.graphics=w[5];context_.core=w[6];context_.camera=w[7];context_.selected=w[8];context_.world_generation=w[9];context_.replay=++replay_;context_.valid=true;return;
    }
    if(fault_||!context_.valid)return;
    if(op==Op::FrameEnd){if(depth_||serial!=context_.frame){Fault();return;}Reset();return;}
    if(op==Op::End){if(!depth_||stack_[depth_-1].serial!=serial){Fault();return;}context_.phase=stack_[--depth_].phase;context_.player_enclosing=stack_[depth_].player;context_.other_car_enclosing=stack_[depth_].other;context_.scene_root=stack_[depth_].root;context_.scene_type=stack_[depth_].type;context_.main_root=stack_[depth_].main;context_.crowd_root=stack_[depth_].crowd;context_.particle_root=stack_[depth_].particle;return;}
    if(serial<=last_){Fault();return;}last_=serial;
    if(op==Op::Begin){if(depth_==stack_.size()){Fault();return;}if(LeafEntry(phase)&&(!w[6]||w[5]!=LeafEntry(phase)||w[7]!=context_.graphics||!w[8]||w[9]!=4)){Fault();return;}if(CrowdPhase(phase)&&(!w[6]||w[7]!=context_.graphics||!w[8]||(phase==kMainSkinnedCrowd?(w[5]!=0x8246C158u||w[9]!=4):(w[5]!=0x824A2F60u||(w[9]!=0x82256230u&&w[9]!=0x822562D4u))))){Fault();return;}if(phase==kMainParticles&&(w[5]!=0x8249DBE8u||!w[6]||w[7]!=context_.graphics||!w[8]||(w[9]!=0x82001E8Cu&&w[9]!=0x82256574u))){Fault();return;}stack_[depth_++]={context_.phase,serial,context_.player_enclosing,context_.other_car_enclosing,context_.scene_root,context_.scene_type,context_.main_root,context_.crowd_root,context_.particle_root};context_.particle_root=phase==kMainParticles?w[8]:0;context_.crowd_root=CrowdPhase(phase)?w[8]:0;
      if(phase==Phase::Scene){context_.scene_root=0;context_.scene_type=0;if(w[5]==kSceneSource&&w[6]==context_.graphics&&w[7]&&w[8]==kTrackType&&w[9]==context_.camera){context_.scene_root=w[7];context_.scene_type=w[8];}}
      context_.main_root=LeafEntry(phase)?w[8]:phase==kMainTrack&&w[5]==0x82466418u&&w[6]&&w[6]==w[8]&&w[7]==context_.graphics&&w[9]==4?w[8]:0;context_.phase=phase;context_.player_enclosing|=phase==Phase::PlayerCar;context_.other_car_enclosing|=phase==Phase::OtherCar;return;}
    if(op==Op::Target){if(!w[9]||w[9]<=context_.binding){Fault();return;}context_.color=w[5];context_.depth=w[6];context_.cdesc=w[7];context_.ddesc=w[8];context_.binding=w[9];context_.surface_ready=false;return;}
    if(op==Op::Context){if(w[5]==kSurface24){context_.surface_ready=context_.binding&&w[8]==context_.color&&w[9]==context_.depth&&w[6]&&w[7];context_.color_surface=w[6];context_.depth_surface=w[7];}if(w[5]==0x47454E31u)context_.world_generation|=uint64_t(w[6])<<32;return;}
    Fault();
  }
 private:
  struct Entry{Phase phase=Phase::Unknown;uint32_t serial=0;bool player=false,other=false;uint32_t root=0,type=0,main=0,crowd=0,particle=0;};
  uint32_t allowed_=0,retired_=0,last_=0;uint64_t replay_=0;size_t depth_=0,buffers_=0;bool fault_=false;Context context_{};std::array<Entry,16>stack_{};
};
struct Receipt{bool recognized=false,ok=true;uint32_t count=0,prior_frame=0;uint64_t prior_replay=0;std::array<uint32_t,kWords>words{};};
// Probe a copy: unrelated/disabled NOPs retain the exact existing parser path.
template<class Reader> Receipt TryParse(Reader& reader,uint32_t count,Consumer& consumer,bool enabled){
  Receipt out;out.count=count;if(!enabled||!count||reader.read_count()<4)return out;
  auto probe=reader;if(probe.template ReadAndSwap<uint32_t>()!=kMagic)return out;
  out.recognized=true;
  if(count!=kWords||uint64_t(count)*4>reader.read_count()){consumer.Fault();out.ok=false;return out;}
  for(uint32_t i=0;i<kWords;++i)out.words[i]=reader.template ReadAndSwap<uint32_t>();
  const auto before=consumer.context();out.prior_frame=before.frame;out.prior_replay=before.replay;consumer.Consume(out.words);return out;
}
} // namespace fh1::recorded
