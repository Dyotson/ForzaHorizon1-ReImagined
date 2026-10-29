#pragma once
#include <cstdint>
// Include the recorded protocol first. No resource identities or address registry.
namespace fh1::track {
template<class Read> std::array<uint32_t,5> ReadScene(bool live,uint32_t graphics,uint32_t expected_graphics,uint32_t core,uint32_t camera,Read&& read){
  uint32_t actual_core=0,root=0,type=0;
  if(!live||!graphics||graphics!=expected_graphics||!core||!camera||uint64_t(graphics)+4>0xEFFFFFFCull||uint64_t(core)+36>0xEFFFFFFCull||!read(graphics+4,actual_core)||actual_core!=core||!read(core+36,root)||!root||!read(root,type)||type!=recorded::kTrackType)return {};
  return {recorded::kSceneSource,graphics,root,type,camera};
}
struct CrowdInput {uint32_t entry=0,object=0,graphics=0,root=0,pass=0,caller=0;};
struct CrowdProof {recorded::Phase phase=provenance::Phase::Unknown;std::array<uint32_t,5> data{};};
template<class Read> CrowdProof ReadCrowd(bool requested,bool recording,bool original_frame,bool car_enclosing,const CrowdInput& in,uint32_t graphics,uint32_t core,uint32_t camera,Read&& read){
  if(!requested||!recording||!original_frame||car_enclosing||!graphics||in.graphics!=graphics||!core||!camera||uint64_t(graphics)+400>0xEFFFFFFCull||uint64_t(core)+60>0xEFFFFFFCull)return {};
  const auto source=ReadScene(true,graphics,graphics,core,camera,read);if(!source[2])return {};
  uint32_t actual_camera=0;if(!read(graphics+400,actual_camera)||actual_camera!=camera)return {};
  const auto root=source[2];uint32_t object=0,table=0,draw=0;
  if(in.entry==0x8246C158u){
    if(in.caller!=0x824752F4u||in.pass!=4||in.root!=root||uint64_t(root)+46664>0xEFFFFFFCull||!read(root+46664,object)||!object||object!=in.object||uint64_t(object)+4>0xEFFFFFFCull||!read(object+4,table))return {};
    return {recorded::kMainSkinnedCrowd,{in.entry,object,graphics,root,4}};
  }
  if(in.entry==0x824A2F60u){
    if(in.caller!=0x82475328u||!read(core+60,object)||!object||!read(object,table)||(table!=0x82256230u&&table!=0x822562D4u)||uint64_t(table)+40>0xEFFFFFFCull||!read(table+40,draw)||draw!=0x82EA82D8u)return {};
    return {recorded::kMainCrowdComponent,{in.entry,object,graphics,root,table}};
  }
  return {};
}
struct ParticleInput {uint32_t object=0,graphics=0,kind=0,second=0,caller=0;};
template<class Read> CrowdProof ReadParticles(bool requested,bool recording,bool original_frame,bool car_enclosing,const ParticleInput& in,uint32_t graphics,uint32_t core,uint32_t camera,Read&& read){
  if(!requested||!recording||!original_frame||car_enclosing||!graphics||in.graphics!=graphics||!core||!camera||in.caller!=0x8249DE48u||in.second!=2||uint64_t(graphics)+448>0xEFFFFFFCull||uint64_t(core)+96>0xEFFFFFFCull)return {};
  const auto source=ReadScene(true,graphics,graphics,core,camera,read);if(!source[2])return {};
  uint32_t actual_camera=0,kind=0,object=0,table=0,draw=0;
  if(!read(graphics+400,actual_camera)||actual_camera!=camera||!read(graphics+448,kind)||kind!=in.kind||(kind!=2&&kind!=3)||!read(core+96,object)||!object||object!=in.object||!read(object,table)||(table!=0x82001E8Cu&&table!=0x82256574u)||!read(table+92,draw)||draw!=0x8249DBE8u)return {};
  return {recorded::kMainParticles,{0x8249DBE8u,object,graphics,source[2],table}};
}
struct LeafInput {uint32_t object=0,graphics=0,root=0,stack=0,entry=0;};
inline recorded::Phase LeafPhase(uint32_t entry,uint32_t table){
  if(entry==0x82447B70u&&table==0x82002B64u)return recorded::kMainModels;
  if(entry==0x824436A0u&&table==0x82002AB4u)return recorded::kMainVegetation;
  if(entry==0x8248A270u&&table==0x820028A4u)return recorded::kMainCharacters;
  return provenance::Phase::Unknown;
}
template<class Read> recorded::Phase ReadLeaf(bool requested,bool recording,bool outer_scene,bool car_enclosing,const LeafInput& input,uint32_t graphics,uint32_t core,Read&& read){
  if(!requested||!recording||!outer_scene||car_enclosing||!input.object||!graphics||input.graphics!=graphics||!core||uint64_t(input.stack)+84>0xEFFFFFFCull||uint64_t(core)+36>0xEFFFFFFCull)return provenance::Phase::Unknown;
  uint32_t pass=0,root=0,table=0;
  if(!read(input.stack+84,pass)||pass!=4||!read(core+36,root)||!root||input.root!=root||!read(input.object,table))return provenance::Phase::Unknown;
  return LeafPhase(input.entry,table);
}
inline bool SourceMainPass(bool recording,bool outer_scene,uint32_t pass,uint32_t core,uint32_t root){return recording&&outer_scene&&pass==4&&core&&core==root;}
struct Raster {uint32_t mode=0,surface=0,color[4]{},depth=0,effective_mask=0,raw_mask=0,shader_exports=0;bool memexport=false,query=false;};
enum class Reason:uint32_t {Off,Unowned,Player,OtherOwner,MissingSurface,TargetMismatch,UnsupportedFormat,MRT,UnsupportedMode,Memexport,Query,Suppress};
struct Decision {bool suppress=false;Reason reason=Reason::Off;};
struct Probe {uint32_t uid=0,capture_uid=0;bool used=false;
  bool Begin(bool requested,const recorded::Context& c){if(!requested||used||!c.valid)return false;used=true;uid=c.uid;capture_uid=c.uid;return true;}
  void Eligibility(uint32_t allowed){if(allowed!=uid)uid=0;}
  void Presented(){uid=0;capture_uid=0;}
};
inline uint32_t CanonicalColor(uint32_t word){return ((word>>16)&15)==12?(word&~0xF0000u)|0x30000u:word;}
inline Decision QualifiedRaster(const recorded::Context& c,const Raster& r){
  if(!c.surface_ready||!c.binding||!c.color||!c.depth)return {false,Reason::MissingSurface};
  const uint32_t format=(c.cdesc>>16)&15;
  if(format!=3&&format!=12)return {false,Reason::UnsupportedFormat};
  // 82438A90 copies surface+24; 82439AF0 aliases format3/12 while
  // preserving all other color fields. HIZ pitch is separate from the primary
  // attachment key (Vulkan RT cache uses pitch/MSAA, color/depth base+format).
  constexpr uint32_t geometry=0x00033FFFu;
  if(((c.color_surface|c.depth_surface|r.surface)&0xC000u)||!(c.color_surface&0x3FFFu)||((c.color_surface^c.depth_surface)&geometry)||
     ((c.color_surface^r.surface)&geometry)||CanonicalColor(c.cdesc)!=CanonicalColor(r.color[0])||c.ddesc!=r.depth)return {false,Reason::TargetMismatch};
  // Production GetNormalizedColorMask combines static shader exports, raw mask,
  // and existing format components. Bound secondary descriptors may be stale.
  if(r.effective_mask&0xFFF0u)return {false,Reason::MRT};
  if(r.mode!=4&&r.mode!=5)return {false,Reason::UnsupportedMode};
  if(r.memexport)return {false,Reason::Memexport};
  if(r.query)return {false,Reason::Query};
  return {true,Reason::Suppress};
}
inline Decision Decide(bool requested,const recorded::Context& c,uint32_t allowed,const Raster& r,bool procedural_requested=false){
  if(!requested&&!procedural_requested)return {};
  if(!c.valid||!allowed||c.uid!=allowed)return {false,Reason::Unowned};
  if(c.player_enclosing||c.phase==provenance::Phase::PlayerCar)return {false,Reason::Player};
  if(c.other_car_enclosing||c.phase==provenance::Phase::OtherCar)return {false,Reason::OtherOwner};
  if(!((requested&&c.phase==recorded::kMainTrack)||(procedural_requested&&recorded::LeafEntry(c.phase))))return {false,Reason::OtherOwner};
  return QualifiedRaster(c,r);
}
enum class SceneReason:uint32_t {Off,Untyped,BeforeWitness,IdentityMismatch,TargetMismatch,Car,Guard,Eligible};
struct SceneDecision {Decision raster{};SceneReason reason=SceneReason::Off;};
struct MainTargetWitness {
  recorded::Context identity{};Raster attachment{};bool ready=false;
  void Reset(){identity={};attachment={};ready=false;}
  bool SameFrame(const recorded::Context& c,uint32_t allowed)const{return c.valid&&allowed&&c.uid==allowed&&identity.uid==c.uid&&identity.frame==c.frame&&identity.replay==c.replay&&identity.world_generation==c.world_generation&&identity.graphics==c.graphics&&identity.core==c.core&&identity.camera==c.camera&&identity.selected==c.selected;}
  void Synchronize(const recorded::Context& c,uint32_t allowed){if(ready&&!SameFrame(c,allowed))Reset();}
  bool SameTarget(const recorded::Context& c,const Raster& r)const{return c.binding==identity.binding&&c.color==identity.color&&c.depth==identity.depth&&CanonicalColor(c.cdesc)==CanonicalColor(identity.cdesc)&&c.ddesc==identity.ddesc&&c.color_surface==identity.color_surface&&c.depth_surface==identity.depth_surface&&c.surface_ready&&r.surface==attachment.surface&&CanonicalColor(r.color[0])==CanonicalColor(attachment.color[0])&&r.depth==attachment.depth;}
  void Observe(bool requested,const recorded::Context& c,uint32_t allowed,const Raster& r,const Decision& decision){
    Synchronize(c,allowed);
    if(!requested){Reset();return;}
    if(ready&&decision.suppress&&!(c.phase==recorded::kMainTrack||recorded::LeafEntry(c.phase)))Reset();
    if(ready&&(c.phase==recorded::kMainTrack||recorded::LeafEntry(c.phase))&&(c.scene_root!=identity.scene_root||c.scene_type!=identity.scene_type||c.main_root!=c.scene_root))Reset();
    if(!requested||!decision.suppress||!c.valid||c.uid!=allowed||!c.scene_root||c.scene_type!=recorded::kTrackType||c.main_root!=c.scene_root||c.player_enclosing||c.other_car_enclosing||!(c.phase==recorded::kMainTrack||recorded::LeafEntry(c.phase)))return;
    identity=c;attachment=r;ready=true;
  }
  SceneDecision Decide(bool requested,const recorded::Context& c,uint32_t allowed,const Raster& r){
    Synchronize(c,allowed);
    if(!requested){Reset();return {};}
    if(!c.valid||!allowed||c.uid!=allowed)return {{false,Reason::Unowned},SceneReason::IdentityMismatch};
    if(c.player_enclosing||c.phase==provenance::Phase::PlayerCar)return {{false,Reason::Player},SceneReason::Car};
    if(c.other_car_enclosing||c.phase==provenance::Phase::OtherCar)return {{false,Reason::OtherOwner},SceneReason::Car};
    if(c.phase!=provenance::Phase::Scene||!c.scene_root||c.scene_type!=recorded::kTrackType){if(c.phase==provenance::Phase::Scene)Reset();return {{false,Reason::OtherOwner},SceneReason::Untyped};}
    if(!ready)return {{false,Reason::OtherOwner},SceneReason::BeforeWitness};
    if(c.scene_root!=identity.scene_root||c.scene_type!=identity.scene_type){Reset();return {{false,Reason::OtherOwner},SceneReason::IdentityMismatch};}
    if(!SameTarget(c,r)){Reset();return {{false,Reason::TargetMismatch},SceneReason::TargetMismatch};}
    const auto result=QualifiedRaster(c,r);return {result,result.suppress?SceneReason::Eligible:SceneReason::Guard};
  }
  SceneDecision DecideCrowd(bool requested,const recorded::Context& c,uint32_t allowed,const Raster& r){
    Synchronize(c,allowed);if(!requested)return {};
    if(!c.valid||!allowed||c.uid!=allowed)return {{false,Reason::Unowned},SceneReason::IdentityMismatch};
    if(c.player_enclosing||c.phase==provenance::Phase::PlayerCar)return {{false,Reason::Player},SceneReason::Car};
    if(c.other_car_enclosing||c.phase==provenance::Phase::OtherCar)return {{false,Reason::OtherOwner},SceneReason::Car};
    if(!recorded::CrowdPhase(c.phase)||!c.crowd_root)return {{false,Reason::OtherOwner},SceneReason::Untyped};
    if(!ready)return {{false,Reason::OtherOwner},SceneReason::BeforeWitness};
    if(c.crowd_root!=identity.scene_root){Reset();return {{false,Reason::OtherOwner},SceneReason::IdentityMismatch};}
    if(!SameTarget(c,r)){Reset();return {{false,Reason::TargetMismatch},SceneReason::TargetMismatch};}
    const auto result=QualifiedRaster(c,r);return {result,result.suppress?SceneReason::Eligible:SceneReason::Guard};
  }
  SceneDecision DecideParticles(bool requested,const recorded::Context& c,uint32_t allowed,const Raster& r){
    Synchronize(c,allowed);if(!requested)return {};
    if(!c.valid||!allowed||c.uid!=allowed)return {{false,Reason::Unowned},SceneReason::IdentityMismatch};
    if(c.player_enclosing||c.phase==provenance::Phase::PlayerCar)return {{false,Reason::Player},SceneReason::Car};
    if(c.other_car_enclosing||c.phase==provenance::Phase::OtherCar)return {{false,Reason::OtherOwner},SceneReason::Car};
    if(c.phase!=recorded::kMainParticles||!c.particle_root)return {{false,Reason::OtherOwner},SceneReason::Untyped};
    if(!ready)return {{false,Reason::OtherOwner},SceneReason::BeforeWitness};
    if(c.particle_root!=identity.scene_root){Reset();return {{false,Reason::OtherOwner},SceneReason::IdentityMismatch};}
    if(!SameTarget(c,r)){Reset();return {{false,Reason::TargetMismatch},SceneReason::TargetMismatch};}
    const auto result=QualifiedRaster(c,r);return {result,result.suppress?SceneReason::Eligible:SceneReason::Guard};
  }

};

template<class Draw> void EmitGuestRaster(const Decision& d,Draw&& draw){if(!d.suppress)draw();}
}
