// TU4 phase A: ordered observation only. Never filters, changes a native pass,
// acquires guest resources, calls a car initializer/remover or renders a new view.
#include "../platform/pause_scene_provenance.h"
#include "../platform/pause_scene_protocol.h"
#include "../platform/pause_recorded_protocol.h"
#include "../platform/pause_track_policy.h"
#include "../platform/pause_record_manifest.h"
#include "../platform/pause_camera.h"
#include "../platform/guest_memory.h"
#include "../platform/guest_protected_range.h"
#include "../platform/pause_writer_diagnostics.h"
#include "../platform/guest_write_transaction.h"
#include "../platform/pause_fast_record_witness.h"
#include "../platform/pause_record_admission.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/hook.h>
#include <rex/memory/utils.h>
#include <rex/system/kernel_state.h>
#include <atomic>
#include <limits>
#include <string>
#include <mutex>
#include <chrono>
#include <cstring>

#if FORZA_TU4 && defined(FORZA_NATIVE_PAUSE)
REXCVAR_DEFINE_BOOL(forza_pause_studio, false, "Debug", "Reserved studio provider gate; owned studio rendering is not integrated");
REXCVAR_DEFINE_BOOL(forza_pause_persistent_partial, false, "Debug", "Private diagnostic fixed wheel-pose persistent partial filter; no CameraReady/pan/fade");
REXCVAR_DEFINE_STRING(forza_pause_partial_event, "", "Debug", "Diagnostic native cancellation event; never authorizes visibility");
REXCVAR_DEFINE_STRING(forza_pause_partial_seal, "", "Debug", "Diagnostic source/seal timing correlation; never authorizes visibility");
REXCVAR_DEFINE_STRING(forza_pause_partial_admission, "", "Debug", "Atomic sealed UID, revision and captured source identity; empty hard-revokes replay eligibility");
REXCVAR_DEFINE_STRING(forza_pause_partial_identity, "", "Debug", "Internal fresh native pause identity; empty disables persistent partial visibility");
REXCVAR_DEFINE_BOOL(forza_pause_provenance, false, "Debug", "Observe ordered native pause render provenance; never filters draws");
REXCVAR_DEFINE_UINT32(forza_pause_provenance_token, 0, "Debug", "Internal current observation generation; zero disables provenance");
REXCVAR_DEFINE_BOOL(forza_pause_provenance_diagnostics, false, "Debug", "Bounded private provenance boundary receipts; no render changes");
REXCVAR_DEFINE_BOOL(forza_pause_record_manifest, false, "Debug", "Read-only bounded native recording manifest; never emits recorded markers");
REXCVAR_DEFINE_BOOL(forza_pause_particles_binary, false, "Debug", "Private typed native particles on witnessed main target, one presented frame");
REXCVAR_DEFINE_BOOL(forza_pause_crowd_binary, false, "Debug", "Private source-owned crowd on witnessed main target, one presented frame");
REXCVAR_DEFINE_BOOL(forza_pause_scene_binary, false, "Debug", "Private same-replay witnessed main-target typed track-scene raster omission");
REXCVAR_DEFINE_BOOL(forza_pause_procedural_binary, false, "Debug", "Private one-presented-frame procedural leaf main-pass raster omission");
REXCVAR_DEFINE_BOOL(forza_pause_track_binary, false, "Debug", "Private partial static-track raster omission in a valid sealed native pause record");
REXCVAR_DEFINE_BOOL(forza_pause_recorded_provenance, false, "Debug", "Private recorded replay provenance only; never suppresses rendering");
REXCVAR_DEFINE_UINT32(forza_pause_recorded_sealed_uid, 0, "Debug", "Internal one latest sealed recording UID; zero revokes replay provenance");
extern "C" REX_FUNC(__imp__sub_82466538);
#define ORIGINAL(a) extern "C" REX_FUNC(__imp__sub_##a)
extern "C" REX_FUNC(forza_phase_a_scene_resolution_824744D0);
ORIGINAL(8320A3B8);ORIGINAL(82471898);
ORIGINAL(8249DBE8);ORIGINAL(8246C158);ORIGINAL(824A2F60);ORIGINAL(824436A0);ORIGINAL(8248A270);ORIGINAL(8247C158);ORIGINAL(82E7FDC0);ORIGINAL(82439AF0);ORIGINAL(824845C8);
ORIGINAL(8243D1A8);ORIGINAL(82498AD8);
#undef ORIGINAL
namespace forza::pause_scene_provenance {
void CancelLocked(const char* reason);
namespace {
namespace p=fh1::provenance;
std::atomic<uint32_t> next_token{0};
std::atomic<uint32_t> next_serial{0},revision{0};
std::atomic<bool> exhausted{false};
std::mutex eligibility_mutex;
p::Scene eligible_scene{};uint32_t eligible_token=0;
struct Frame {
  p::Scene scene{};uint32_t token=0,serial=1,binding=0,device=0;
  const PPCContext* context=nullptr;uint8_t* base=nullptr;bool failed=false;
};
thread_local std::string partial_record_identity;thread_local uint32_t partial_identity_revision=0;std::atomic<uint32_t> partial_event_ids{0};
thread_local fh1::writer_diag::Record writer_diagnostic;std::atomic<uint32_t> writer_summary_lines{0};
void WriterReject(uint32_t reason,uint32_t address=0,uint32_t width=0);
void WriterSummary(const char* outcome,uint32_t sealed=0);
thread_local Frame frame;thread_local uint32_t native_scene_depth=0;
struct HostScope{p::Phase phase;uint32_t serial;};thread_local std::array<HostScope,16> host_scopes{};thread_local size_t host_depth=0;thread_local p::Phase host_phase=p::Phase::Unknown;
std::atomic<uint32_t> diagnostic_lines{0};
struct HostDiagnostics {
  uint64_t generation=UINT64_MAX;uint32_t entries=0,eligibility=0,writes=0,rejects=0;
};
thread_local std::array<HostDiagnostics,8> diagnostic_generations;
bool DiagnosticEnabled(){return REXCVAR_GET(forza_pause_provenance)&&REXCVAR_GET(forza_pause_provenance_diagnostics);}
uint64_t DiagnosticTime(){return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
uint32_t DiagnosticPhysical(uint32_t address){
  auto* kernel=rex::system::kernel_state();
  return kernel&&kernel->memory()?kernel->memory()->GetPhysicalAddress(address):UINT32_MAX;
}
bool DiagnosticAdmit(uint64_t generation,unsigned category,unsigned bit=0) {
  if(!DiagnosticEnabled())return false;
  HostDiagnostics* selected=nullptr;
  for(auto& slot:diagnostic_generations)if(slot.generation==generation){selected=&slot;break;}
  if(!selected)for(auto& slot:diagnostic_generations)if(slot.generation==UINT64_MAX){slot.generation=generation;selected=&slot;break;}
  if(!selected)return false;
  auto& diagnostics=*selected;
  if(category==0){if(diagnostics.entries&(1u<<bit))return false;diagnostics.entries|=1u<<bit;}
  else {auto& count=category==1?diagnostics.eligibility:category==2?diagnostics.writes:diagnostics.rejects;
    if(count>=(category==2?12u:4u))return false;++count;}
  auto n=diagnostic_lines.load();
  while(n<256){if(diagnostic_lines.compare_exchange_weak(n,n+1))return true;}
  return false;
}
void Entry(uint32_t guest,unsigned bit,const PPCContext& c,uint64_t generation) {
  if(DiagnosticAdmit(generation,0,bit))REXLOG_INFO("FH1PH entry us={} generation={} guest={:08X} r3={:08X} r4={:08X} token={} frame={} device={:08X}",
    DiagnosticTime(),generation,guest,c.r3.u32,c.r4.u32,frame.token,frame.serial,frame.device);
}
bool Word(uint8_t* base,uint32_t address,uint32_t& value) {
  if(!protected_range::Allowed(base,address,4,1))return false;
  value=(uint32_t(*GuestPtr(base,address))<<24)|(uint32_t(*GuestPtr(base,address+1))<<16)|(uint32_t(*GuestPtr(base,address+2))<<8)|*GuestPtr(base,address+3);return true;
}
#include "pause_record_manifest.inl"
struct NativeWriter {
  Frame& f;
  uint32_t observed_cursor=0,observed_limit=0,flags=0,record=0;
  const char* reason="writer-unreadable";bool committed=false;
  bool Writer(uint32_t& cursor,uint32_t& limit,bool& recording) {
    reason="cursor-unreadable";if(!Word(f.base,f.device+48,observed_cursor))return false;
    reason="limit-unreadable";if(!Word(f.base,f.device+56,observed_limit))return false;
    reason="flags-unreadable";if(!Word(f.base,f.device+11068,flags))return false;
    reason="record-unreadable";if(!Word(f.base,f.device+13600,record))return false;
    cursor=observed_cursor;limit=observed_limit;
    recording=bool(flags&0x80000000u)||record!=0;
    reason=recording?"recording":!cursor||(cursor&3)||(limit&3)?"cursor-alignment":"capacity";
    return true;
  }
  bool Writable(uint32_t address,uint32_t bytes) {
    reason="protection";
    if(uint64_t(f.device)+52>0xF0000000ull){WriterReject(uint32_t(fh1::writer_diag::Reason::CommitBounds),f.device,4);return false;}
    if(!protected_range::Allowed(f.base,address,bytes,2)){WriterReject(uint32_t(fh1::writer_diag::Reason::PayloadProtection),address,bytes);return false;}
    if(!protected_range::Allowed(f.base,f.device+48,4,2)){WriterReject(uint32_t(fh1::writer_diag::Reason::CommitProtection),f.device+48,4);return false;}
    return true;
  }
  void Store(uint32_t address,uint32_t value){p::StoreBigEndian(GuestPtr(f.base,address),value);}
  void Commit(uint32_t cursor){Store(f.device+48,cursor);committed=true;reason="commit";}
};
fh1::recorded::AllocatorState ReadDeviceFields(uint8_t* base,uint32_t device,uint32_t* failed_mask=nullptr){
  fh1::recorded::AllocatorState out;if(failed_mask)*failed_mask=0;
  if(!base||uint64_t(device)+15336>0xF0000000ull){out.readable=false;if(failed_mask)*failed_mask=127;return out;}
  struct Window{uint32_t offset,bytes,first,count;};
  constexpr Window windows[]{{48,8,0,2},{11068,4,2,1},{13600,8,3,2},{15324,4,5,1},{15332,4,6,1}};
  for(const auto& w:windows){
    if(protected_range::Allowed(base,device+w.offset,w.bytes,1)){
      for(uint32_t n=0;n<w.count;++n){const auto at=device+fh1::recorded::kAllocatorOffsets[w.first+n];out.v[w.first+n]=(uint32_t(*GuestPtr(base,at))<<24)|(uint32_t(*GuestPtr(base,at+1))<<16)|(uint32_t(*GuestPtr(base,at+2))<<8)|*GuestPtr(base,at+3);}
    }else for(uint32_t n=0;n<w.count;++n){const bool ok=Word(base,device+fh1::recorded::kAllocatorOffsets[w.first+n],out.v[w.first+n]);out.readable&=ok;if(!ok&&failed_mask)*failed_mask|=1u<<(w.first+n);}
  }
  return out;
}
#include "pause_recorded_provenance.inl"
void Fail() {
  frame.failed=true;
  Cancel("writer-failure"); // A later retry must have a different token than partial old packets.
}
bool Live() {return frame.token&&!frame.failed&&REXCVAR_GET(forza_pause_provenance)&&
    REXCVAR_GET(forza_pause_provenance_token)==frame.token;}
bool Send(p::Op op,uint32_t serial,p::Phase phase=p::Phase::Unknown,std::array<uint32_t,5> data={}) {
  if(!Live()){
    if(DiagnosticAdmit(frame.scene.generation,3))REXLOG_INFO("FH1PH inactive us={} generation={} token={} current_token={} frame={} serial={} op={} failed={}",
      DiagnosticTime(),frame.scene.generation,frame.token,REXCVAR_GET(forza_pause_provenance_token),frame.serial,serial,uint32_t(op),frame.failed);
    return false;
  }
  if(record_attempt){if(RecordSend(op,serial,phase,data))return true;Fail();return false;}
  NativeWriter io{frame};
  const bool emitted=p::Emit(io,p::Record{op,frame.token,serial,phase,data});
  if(DiagnosticAdmit(frame.scene.generation,emitted?2:3))
    REXLOG_INFO("FH1PH writer us={} generation={} token={} frame={} serial={} op={} phase={} device={:08X} cursor={:08X} limit={:08X} flags={:08X} record={:08X} header_guest={:08X} header_phys={:08X} next={:08X} success={} reason={}",
      DiagnosticTime(),frame.scene.generation,frame.token,frame.serial,serial,uint32_t(op),uint32_t(phase),frame.device,
      io.observed_cursor,io.observed_limit,io.flags,io.record,io.observed_cursor+4,DiagnosticPhysical(io.observed_cursor+4),
      io.committed?io.observed_cursor+44:io.observed_cursor,emitted,io.reason);
  if(!emitted){Fail();return false;}
  return true;
}
uint32_t Next(std::atomic<uint32_t>& counter) {
  const auto id=p::NextId(counter);if(!id)exhausted=true;return id;
}
uint32_t Serial() {const auto n=Next(next_serial);if(!n)Fail();return n;}
void Targets() {
  if(!Live())return;
  uint32_t color=0,depth=0,cd=0,dd=0;
  if(!Word(frame.base,frame.device+12816,color)){WriterReject(4,frame.device+12816,4);Fail();return;}
  if(!Word(frame.base,frame.device+12832,depth)){WriterReject(5,frame.device+12832,4);Fail();return;}
  if(color&&!Word(frame.base,color+28,cd)){WriterReject(6,color+28,4);Fail();return;}
  if(depth&&!Word(frame.base,depth+28,dd)){WriterReject(7,depth+28,4);Fail();return;}
  if(frame.binding==UINT32_MAX){WriterReject(8);Fail();return;}
  Send(p::Op::Target,Serial(),p::Phase::Unknown,{color,depth,cd,dd,++frame.binding});
  // Second target descriptor words are separate ordered context metadata. The
  // binding serial is an observation generation, not an allocation generation.
  uint32_t c32=0,d32=0;
  if(color&&!Word(frame.base,color+32,c32)){WriterReject(9,color+32,4);Fail();return;}
  if(depth&&!Word(frame.base,depth+32,d32)){WriterReject(10,depth+32,4);Fail();return;}
  Send(p::Op::Context,Serial(),p::Phase::Unknown,{frame.device,c32,d32,color,depth});
  if(record_attempt){uint32_t c24=0,d24=0;const bool readable=color&&depth&&Word(frame.base,color+24,c24)&&Word(frame.base,depth+24,d24);if(!readable)c24=d24=0;Send(p::Op::Context,Serial(),p::Phase::Unknown,{rec::kSurface24,c24,d24,color,depth});}

}
struct ScopeIO {
  uint32_t Begin(p::Phase phase,std::array<uint32_t,5> data={}){if(!Live())return 0;if(host_depth==host_scopes.size()){WriterReject(2);Fail();return 0;}const auto s=Serial();if(!s||!Send(p::Op::Begin,s,phase,data))return 0;host_scopes[host_depth++]={host_phase,s};host_phase=phase;return s;}
  void End(uint32_t serial){if(!host_depth||host_scopes[host_depth-1].serial!=serial){if(Live()){WriterReject(3);Fail();}return;}Send(p::Op::End,serial);host_phase=host_scopes[--host_depth].phase;}
};

p::Phase Car(uint32_t car) {
  uint32_t parent=0,type=0,life=0;
  if(!Live()||!Word(frame.base,car,type)||type!=0x82003A6C||!Word(frame.base,car+8,parent)||
     !Word(frame.base,car+5248,life)||(life&0xFF))return p::Phase::Unknown;
  return p::CarPhase(frame.scene,car,parent);
}
}
ParticleMainScope::ParticleMainScope(uint8_t* base,unsigned object,unsigned graphics,unsigned kind,unsigned second,unsigned caller){
  if(!Live()||!record_attempt)return;
  bool car_enclosing=host_phase==p::Phase::PlayerCar||host_phase==p::Phase::OtherCar;
  for(size_t i=0;i<host_depth;++i)car_enclosing|=host_scopes[i].phase==p::Phase::PlayerCar||host_scopes[i].phase==p::Phase::OtherCar;
  const auto proof=fh1::track::ReadParticles(REXCVAR_GET(forza_pause_particles_binary),RecordEnabled(),host_phase==p::Phase::Unknown,car_enclosing,{object,graphics,kind,second,caller},frame.scene.graphics,frame.scene.core,frame.scene.camera,[&](uint32_t at,uint32_t& out){return Word(base,at,out);});
  if(proof.phase==p::Phase::Unknown)return;
  ScopeIO io;serial_=io.Begin(proof.phase,proof.data);
}
ParticleMainScope::~ParticleMainScope(){if(serial_){ScopeIO io;io.End(serial_);}}
CrowdMainScope::CrowdMainScope(uint8_t* base,unsigned entry,unsigned object,unsigned graphics,unsigned root,unsigned pass,unsigned caller){
  if(!Live()||!record_attempt)return;
  bool car_enclosing=host_phase==p::Phase::PlayerCar||host_phase==p::Phase::OtherCar;
  for(size_t i=0;i<host_depth;++i)car_enclosing|=host_scopes[i].phase==p::Phase::PlayerCar||host_scopes[i].phase==p::Phase::OtherCar;
  const auto proof=fh1::track::ReadCrowd(REXCVAR_GET(forza_pause_crowd_binary),RecordEnabled(),host_phase==p::Phase::Unknown,car_enclosing,{entry,object,graphics,root,pass,caller},frame.scene.graphics,frame.scene.core,frame.scene.camera,[&](uint32_t at,uint32_t& out){return Word(base,at,out);});
  if(proof.phase==p::Phase::Unknown)return;
  ScopeIO io;serial_=io.Begin(proof.phase,proof.data);
}
CrowdMainScope::~CrowdMainScope(){if(serial_){ScopeIO io;io.End(serial_);}}
ProceduralMainScope::ProceduralMainScope(uint8_t* base,unsigned object,unsigned graphics,unsigned root,unsigned stack,unsigned entry){
  if(!Live()||!record_attempt)return;
  bool car_enclosing=host_phase==p::Phase::PlayerCar||host_phase==p::Phase::OtherCar;
  for(size_t i=0;i<host_depth;++i)car_enclosing|=host_scopes[i].phase==p::Phase::PlayerCar||host_scopes[i].phase==p::Phase::OtherCar;
  const auto phase=fh1::track::ReadLeaf(REXCVAR_GET(forza_pause_procedural_binary),RecordEnabled(),native_scene_depth!=0&&host_phase==p::Phase::Scene,car_enclosing,{object,graphics,root,stack,entry},frame.scene.graphics,frame.scene.core,[&](uint32_t at,uint32_t& out){return Word(base,at,out);});
  if(phase==p::Phase::Unknown)return;
  ScopeIO io;serial_=io.Begin(phase,{entry,object,graphics,root,4});
}
ProceduralMainScope::~ProceduralMainScope(){if(serial_){ScopeIO io;io.End(serial_);}}
TrackMainScope::TrackMainScope(uint8_t* base,unsigned presentation,unsigned pass){
  uint32_t core=0;if(!REXCVAR_GET(forza_pause_track_binary)||!Live()||!record_attempt||!Word(base,presentation+4,core)||!fh1::track::SourceMainPass(RecordEnabled(),native_scene_depth!=0&&host_phase==p::Phase::Scene,pass,core,frame.scene.core))return;
  ScopeIO io;serial_=io.Begin(rec::kMainTrack,{0x82466418u,presentation,frame.scene.graphics,presentation,pass});
}
TrackMainScope::~TrackMainScope(){if(serial_){ScopeIO io;io.End(serial_);}}
bool Enabled(){return !exhausted.load()&&REXCVAR_GET(forza_pause_provenance);}
void Cancel(){Cancel("external");}
void Cancel(const char* reason){std::lock_guard lock(eligibility_mutex);CancelLocked(reason);}
void CancelLocked(const char* reason){rex::cvar::SetFlagByName("forza_pause_partial_admission","");const auto old_revision=revision.load();const auto old_token=eligible_token;const auto old_uid=REXCVAR_GET(forza_pause_recorded_sealed_uid);if(REXCVAR_GET(forza_pause_persistent_partial)&&REXCVAR_GET(forza_pause_provenance_diagnostics))rex::cvar::SetFlagByName("forza_pause_partial_event",std::to_string(++partial_event_ids)+","+std::to_string(DiagnosticTime())+","+reason);++revision;partial_record_identity.clear();partial_identity_revision=revision.load();eligible_scene={};eligible_token=0;rex::cvar::SetFlagByName("forza_pause_partial_identity","");RecordRevoke(reason);rex::cvar::SetFlagByName("forza_pause_provenance_token","0");if((old_token||old_uid)&&RecordEventLine())REXLOG_INFO("FH1RL cancel us={} reason={} old_uid={} published_uid={} old_token={} old_revision={} new_revision={}",DiagnosticTime(),reason,old_uid,REXCVAR_GET(forza_pause_recorded_sealed_uid),old_token,old_revision,revision.load());}

void Render(PPCContext& ctx,uint8_t* base) {
  // Nested unrelated original renders must not inherit a player tag. The
  // enclosing frame is invalidated, never guessed to be another main view.
  if(frame.token){WriterReject(1);Fail();forza_phase_a_scene_resolution_824744D0(ctx,base);return;}
  if(!Enabled()){{std::lock_guard lock(eligibility_mutex);if(eligible_token||!partial_record_identity.empty())CancelLocked("provenance-disabled");else rex::cvar::SetFlagByName("forza_pause_partial_admission","");rex::cvar::SetFlagByName("forza_pause_partial_identity","");}forza_phase_a_scene_resolution_824744D0(ctx,base);return;}
  const auto observed_revision=revision.load();
  const auto scene=pause_camera::ObserveSceneView(base,ctx.r3.u32);
  Entry(0x824744D0,0,ctx,scene.generation);
  uint32_t core=0,camera=0,life=0,app=0;
  const p::Scene identity{scene.core,scene.camera,scene.selection,scene.render_view,scene.generation};
  if(!scene.screen||!Word(base,ctx.r3.u32+4,core)||!Word(base,ctx.r3.u32+400,camera)||
     !p::ExactContext(identity,core,camera,ctx.r3.u32)||
     !Word(base,scene.selection+5248,life)||(life&0xFF)||
     !Word(base,0x833CAAFCu,app)) {
    if(DiagnosticAdmit(scene.generation,1))REXLOG_INFO("FH1PH eligibility us={} generation={} accepted=false graphics={:08X} core={:08X} camera={:08X} expected_graphics={:08X} expected_core={:08X} expected_camera={:08X} selection={:08X} screen={:08X} life={:08X} app={:08X}",
      DiagnosticTime(),scene.generation,ctx.r3.u32,core,camera,identity.graphics,identity.core,identity.camera,scene.selection,scene.screen,life,app);
    Cancel("invalid-scene-identity");
    forza_phase_a_scene_resolution_824744D0(ctx,base);return;}
  PPCContext device_context=ctx;device_context.r3.u64=app;
  __imp__sub_82466538(device_context,base);
  uint32_t token=0;
  {std::lock_guard lock(eligibility_mutex);
    if(revision.load()==observed_revision){
      if(eligible_scene.generation!=identity.generation||eligible_scene.core!=core||
         eligible_scene.car!=identity.car||eligible_scene.camera!=camera||!eligible_token){
        CancelLocked("scene-identity-change");eligible_token=Next(next_token);eligible_scene=identity;
      }
      token=eligible_token;
      rex::cvar::SetFlagByName("forza_pause_provenance_token",std::to_string(token));
      std::string partial;
      if(REXCVAR_GET(forza_pause_persistent_partial)&&!REXCVAR_GET(forza_pause_studio))partial=std::to_string(token)+","+std::to_string(scene.screen)+","+std::to_string(identity.graphics)+","+std::to_string(core)+","+std::to_string(camera)+","+std::to_string(identity.car)+","+std::to_string(identity.generation)+","+std::to_string(DiagnosticTime());
      rex::cvar::SetFlagByName("forza_pause_partial_identity",partial);
      partial_record_identity=partial;partial_identity_revision=revision.load();

    }
  }
  if(!token){forza_phase_a_scene_resolution_824744D0(ctx,base);return;}
  frame={identity,token,Serial(),0,device_context.r3.u32,&ctx,base,false};
  host_depth=0;host_phase=p::Phase::Unknown;
  RecordStart();
  ManifestWorld manifest_world;
  if(DiagnosticAdmit(scene.generation,1))REXLOG_INFO("FH1PH eligibility us={} generation={} accepted=true token={} frame={} graphics={:08X} core={:08X} camera={:08X} selection={:08X} app={:08X} device={:08X} revision={}",
    DiagnosticTime(),scene.generation,token,frame.serial,ctx.r3.u32,core,camera,scene.selection,app,frame.device,observed_revision);
  Send(p::Op::FrameBegin,frame.serial);
  Send(p::Op::Context,Serial(),p::Phase::OriginalFrame,{ctx.r3.u32,core,camera,scene.selection,uint32_t(scene.generation)});
  Send(p::Op::Context,Serial(),p::Phase::OriginalFrame,{0x47454E31u,uint32_t(scene.generation>>32),uint32_t(scene.frame),uint32_t(scene.frame>>32),scene.world_car});
  Targets();
  forza_phase_a_scene_resolution_824744D0(ctx,base);
  Send(p::Op::FrameEnd,frame.serial);
  manifest_world.Finish();
  frame={};record_attempt=false;
  // Keep this frame's token eligible until a subsequent frame or lifecycle
  // cancellation. GPU command consumption is asynchronous.
}
void Opaque(PPCContext& c,uint8_t* b){ManifestScope observation(0x8320A3B8,c.r3.u32>=2016?c.r3.u32-2016:0,2);ScopeIO io;p::ObserveAndForward(io,Car(c.r3.u32>=2016?c.r3.u32-2016:0),[&]{__imp__sub_8320A3B8(c,b);});}
void Secondary(PPCContext& c,uint8_t* b){ManifestScope observation(0x82471898,c.r4.u32,2);ScopeIO io;p::ObserveAndForward(io,Car(c.r4.u32),[&]{__imp__sub_82471898(c,b);});}
void Scene(PPCContext& c,uint8_t* b){const bool qualifies=Live()&&c.r3.u32==frame.scene.graphics;struct NativeSceneExit{bool active;~NativeSceneExit(){if(active)--native_scene_depth;}} depth_exit{qualifies};if(qualifies)++native_scene_depth;ManifestScope observation(0x8247C158,c.r3.u32,4);ScopeIO io;const auto source=fh1::track::ReadScene(qualifies,c.r3.u32,frame.scene.graphics,frame.scene.core,frame.scene.camera,[&](uint32_t at,uint32_t& out){return Word(b,at,out);});const auto serial=io.Begin(qualifies?p::Phase::Scene:p::Phase::Unknown,source);__imp__sub_8247C158(c,b);if(serial)io.End(serial);}
void Sky(PPCContext& c,uint8_t* b){ManifestScope observation(0x82E7FDC0,c.r3.u32,5);ScopeIO io;p::ObserveAndForward(io,Live()&&c.r3.u32==frame.scene.core?p::Phase::Sky:p::Phase::Unknown,[&]{__imp__sub_82E7FDC0(c,b);});}
void Color(PPCContext& c,uint8_t* b){const auto device=c.r3.u32;__imp__sub_82439AF0(c,b);if(device==frame.device)Targets();}
void Depth(PPCContext& c,uint8_t* b){const auto device=c.r3.u32;__imp__sub_824845C8(c,b);if(device==frame.device)Targets();}
void Reserve(PPCContext& c,uint8_t* b){ManifestNativeObserver observation{b,c.r3.u32,0x8243D1A8,c.r4.u32};rm::ForwardObserved(observation,[&]{__imp__sub_8243D1A8(c,b);observation.returned=c.r3.u32;});}
void FinalizeRecording(PPCContext& c,uint8_t* b){RecordFinalize(c,b);ManifestNativeObserver observation{b,c.r3.u32,0x82498AD8};rm::ForwardObserved(observation,[&]{__imp__sub_82498AD8(c,b);observation.returned=c.r3.u32;});}
}
#define WRAP(a,n,bit) extern "C" REX_FUNC(sub_##a){forza::pause_scene_provenance::Entry(0x##a,bit,ctx,forza::pause_scene_provenance::frame.scene.generation);forza::pause_scene_provenance::n(ctx,base);}
WRAP(824744D0,Render,0) WRAP(8320A3B8,Opaque,1) WRAP(82471898,Secondary,2)
WRAP(8247C158,Scene,3) WRAP(82E7FDC0,Sky,4) WRAP(82439AF0,Color,5) WRAP(824845C8,Depth,6)
#undef WRAP
extern "C" REX_FUNC(sub_8243D1A8){forza::pause_scene_provenance::Reserve(ctx,base);}
extern "C" REX_FUNC(sub_82498AD8){forza::pause_scene_provenance::FinalizeRecording(ctx,base);}
extern "C" REX_FUNC(sub_824436A0){forza::pause_scene_provenance::ProceduralMainScope scope(base,ctx.r3.u32,ctx.r5.u32,ctx.r6.u32,ctx.r1.u32,0x824436A0);__imp__sub_824436A0(ctx,base);}
extern "C" REX_FUNC(sub_8248A270){forza::pause_scene_provenance::ProceduralMainScope scope(base,ctx.r3.u32,ctx.r5.u32,ctx.r6.u32,ctx.r1.u32,0x8248A270);__imp__sub_8248A270(ctx,base);}
extern "C" REX_FUNC(sub_8246C158){forza::pause_scene_provenance::CrowdMainScope scope(base,0x8246C158,ctx.r3.u32,ctx.r5.u32,ctx.r6.u32,ctx.r7.u32,uint32_t(ctx.lr));__imp__sub_8246C158(ctx,base);}
extern "C" REX_FUNC(sub_824A2F60){forza::pause_scene_provenance::CrowdMainScope scope(base,0x824A2F60,0,ctx.r3.u32,0,0,uint32_t(ctx.lr));__imp__sub_824A2F60(ctx,base);}
extern "C" REX_FUNC(sub_8249DBE8){forza::pause_scene_provenance::ParticleMainScope scope(base,ctx.r3.u32,ctx.r5.u32,ctx.r6.u32,ctx.r4.u32,uint32_t(ctx.lr));__imp__sub_8249DBE8(ctx,base);}
#else
namespace forza::pause_scene_provenance {bool Enabled(){return false;}void Cancel(){}void Cancel(const char*){}}
#endif
