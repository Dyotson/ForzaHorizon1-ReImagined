// Included inside the private observer TU after guarded Word(). No guest writes.
namespace rm=fh1::record_manifest;
struct RecordState {
  std::array<uint32_t,10> v{};uint32_t color=0,depth=0,cdesc=0,ddesc=0;bool readable=true;
};
bool ManifestEnabled(){return REXCVAR_GET(forza_pause_record_manifest)&&REXCVAR_GET(forza_pause_provenance_diagnostics);}
rm::Budget manifest_budget;std::atomic<uint32_t> manifest_ids{0};
bool ManifestLine(){
  return manifest_budget.Take(ManifestEnabled());
}
RecordState RecordSnapshot(uint8_t* base,uint32_t device){
  RecordState out;constexpr uint32_t offsets[]{48,52,56,11068,13600,13604,15332,15324,13760,13772};
  for(unsigned i=0;i<10;++i)out.readable&=Word(base,device+offsets[i],out.v[i]);
  out.readable&=Word(base,device+12816,out.color);out.readable&=Word(base,device+12832,out.depth);
  if(out.color)out.readable&=Word(base,out.color+28,out.cdesc);
  if(out.depth)out.readable&=Word(base,out.depth+28,out.ddesc);
  return out;
}
struct ManifestContext {
  p::Scene scene{};uint8_t* base=nullptr;uint32_t device=0,anchor=0,anchor_generation=0,world=0,producer_frame=0;
  uint32_t scopes=0,reserves=0;bool finalized=false;
};
std::mutex manifest_mutex;
std::array<ManifestContext,2> manifest_contexts{};
uint32_t manifest_worlds=0;
thread_local int manifest_current=-1;
thread_local uint32_t manifest_parent=0;
ManifestContext ManifestCopy(int slot){std::lock_guard lock(manifest_mutex);return slot>=0?manifest_contexts[slot]:ManifestContext{};}
int ManifestFind(uint32_t device,const RecordState& state,bool reserve=false,bool finalize=false){
  if(!ManifestEnabled()||!state.readable||!state.v[4])return -1;
  std::lock_guard lock(manifest_mutex);
  for(uint32_t i=0;i<manifest_worlds;++i){auto& c=manifest_contexts[i];
    if(c.device!=device||c.anchor!=state.v[4]||c.anchor_generation!=state.v[5])continue;
    if(reserve){if(c.reserves>=32)return -1;++c.reserves;}
    if(finalize){if(c.finalized)continue;c.finalized=true;}
    return int(i);
  }return -1;
}
void ManifestReceipt(const char* event,uint32_t guest,uint32_t call,uint32_t parent,const ManifestContext& c,
                     const RecordState& s,uint32_t car,uint32_t phase,uint32_t argument=0,uint32_t returned=0){
  if(!ManifestLine())return;
  REXLOG_INFO("FH1RM scope us={} event={} guest={:08X} call={} parent={} world={} generation={} producer_frame={} current_token={} graphics={:08X} core={:08X} camera={:08X} selected={:08X} car={:08X} phase={} device={:08X} readable={} cursor={:08X} next_phys={:08X} hard={:08X} soft={:08X} flags={:08X} anchor={:08X} anchor_gen={} slab={:08X} ring_gen={} list_head={:08X} list_cursor={:08X} color={:08X} depth={:08X} cdesc={:08X} ddesc={:08X} argument={} returned={:08X}",
    DiagnosticTime(),event,guest,call,parent,c.world,c.scene.generation,c.producer_frame,REXCVAR_GET(forza_pause_provenance_token),
    c.scene.graphics,c.scene.core,c.scene.camera,c.scene.car,car,phase,c.device,s.readable,s.v[0],DiagnosticPhysical(s.v[0]+4),
    s.v[1],s.v[2],s.v[3],s.v[4],s.v[5],s.v[6],s.v[7],s.v[8],s.v[9],s.color,s.depth,s.cdesc,s.ddesc,argument,returned);
}
struct ManifestWorld {
  int slot=-1,previous=-1;uint32_t parent=0;bool active=false;
  ManifestWorld(){
    if(!ManifestEnabled())return;
    const auto state=RecordSnapshot(frame.base,frame.device);
    if(!state.readable||!state.v[4])return;
    {std::lock_guard lock(manifest_mutex);if(manifest_worlds==manifest_contexts.size())return;
      slot=int(manifest_worlds++);manifest_contexts[slot]={frame.scene,frame.base,frame.device,state.v[4],state.v[5],p::NextId(manifest_ids),frame.serial};}
    const auto c=ManifestCopy(slot);previous=manifest_current;parent=manifest_parent;
    manifest_current=slot;manifest_parent=c.world;active=true;
    ManifestReceipt("world-enter",0x824744D0,c.world,parent,c,state,c.scene.car,1);
  }
  void Finish(){if(!active)return;const auto c=ManifestCopy(slot);
    ManifestReceipt("world-exit",0x824744D0,c.world,parent,c,RecordSnapshot(c.base,c.device),c.scene.car,1);
    manifest_current=previous;manifest_parent=parent;active=false;}
  ~ManifestWorld(){Finish();}
};
struct ManifestScope {
  ManifestContext context{};uint32_t guest=0,call=0,parent=0,car=0,phase=0;bool active=false;
  ManifestScope(uint32_t g,uint32_t input,uint32_t kind):guest(g),car(input){
    if(!ManifestEnabled()||manifest_current<0)return;
    {std::lock_guard lock(manifest_mutex);auto& c=manifest_contexts[manifest_current];if(c.scopes>=64)return;++c.scopes;context=c;}
    const auto state=RecordSnapshot(context.base,context.device);if(!state.readable||!state.v[4])return;
    if(kind==2){uint32_t type=0,owner=0,life=0;
      if(Word(context.base,input,type)&&type==0x82003A6C&&Word(context.base,input+8,owner)&&
         Word(context.base,input+5248,life)&&!(life&255))phase=uint32_t(p::CarPhase(context.scene,input,owner));}
    else if(kind==4&&input==context.scene.graphics)phase=4;
    else if(kind==5&&input==context.scene.core)phase=5;
    call=p::NextId(manifest_ids);parent=manifest_parent;manifest_parent=call;active=true;
    ManifestReceipt("enter",guest,call,parent,context,state,car,phase);
  }
  ~ManifestScope(){if(!active)return;ManifestReceipt("exit",guest,call,parent,context,RecordSnapshot(context.base,context.device),car,phase);manifest_parent=parent;}
};
struct ManifestPhysicalIO {
  uint32_t Physical(uint32_t guest){return DiagnosticPhysical(guest);}
  bool ReadPhysical(uint32_t physical,uint32_t& value){
    if(!rm::Physical(physical)||uint64_t(physical)+4>0x20000000ull)return false;
    auto* kernel=rex::system::kernel_state();if(!kernel||!kernel->memory())return false;
    auto* ptr=kernel->memory()->TranslatePhysical(physical);
    for(unsigned i=0;i<4;++i){size_t region=0;rex::memory::PageAccess access;
      if(!rex::memory::QueryProtect(ptr+i,region,access)||!(int(access)&1))return false;}
    value=(uint32_t(ptr[0])<<24)|(uint32_t(ptr[1])<<16)|(uint32_t(ptr[2])<<8)|ptr[3];return true;
  }
};
struct ManifestSink {
  uint32_t world,list;
  bool Block(uint32_t block,uint32_t start,uint32_t end,uint32_t encoded,bool last){
    if(!ManifestLine())return false;
    REXLOG_INFO("FH1RM block us={} world={} list={} block={} start_phys={:08X} end_phys={:08X} encoded_end={:08X} last={}",DiagnosticTime(),world,list,block,start,end,encoded,last);return true;
  }
  bool Words(uint32_t physical,const std::array<uint32_t,8>& w,uint32_t count){
    if(!ManifestLine())return false;
    REXLOG_INFO("FH1RM words us={} world={} list={} physical={:08X} count={} data={:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X},{:08X}",
      DiagnosticTime(),world,list,physical,count,w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7]);return true;
  }
};
struct ManifestNativeObserver {
  uint8_t* base;uint32_t device,guest,argument=0,returned=0,call=0;int slot=-1;ManifestContext context{};
  void Before(){
    if(!ManifestEnabled())return;const auto state=RecordSnapshot(base,device);
    slot=ManifestFind(device,state,guest==0x8243D1A8,guest==0x82498AD8);if(slot<0)return;
    context=ManifestCopy(slot);call=p::NextId(manifest_ids);
    ManifestReceipt("native-enter",guest,call,0,context,state,context.scene.car,0,argument);
    if(guest!=0x82498AD8)return;
    constexpr uint32_t offsets[]{13760,13616,13736,13640,13664,13688,13712};
    for(const auto offset:offsets){rm::NativeList list;uint32_t* fields[]{&list.head,&list.tail,&list.capacity,&list.cursor,&list.limit,&list.owner};bool readable=true;
      for(unsigned i=0;i<6;++i)readable&=Word(base,device+offset+i*4,*fields[i]);
      if(!ManifestLine())return;
      REXLOG_INFO("FH1RM list us={} world={} list={} device={:08X} readable={} head={:08X} tail={:08X} capacity={} cursor={:08X} limit={:08X} owner={:08X}",
        DiagnosticTime(),context.world,offset,device,readable,list.head,list.tail,list.capacity,list.cursor,list.limit,list.owner);
      rm::Result result;
      if(readable&&list.owner==device){ManifestPhysicalIO io;ManifestSink sink{context.world,offset};result=rm::Snapshot(io,sink,list,offset==13760?2048:256);}
      if(ManifestLine())REXLOG_INFO("FH1RM list-result us={} world={} list={} status={} blocks={} words={}",DiagnosticTime(),context.world,offset,uint32_t(result.status),result.blocks,result.words);
    }
  }
  void After(){if(slot<0)return;ManifestReceipt("native-exit",guest,call,0,context,RecordSnapshot(base,device),context.scene.car,0,argument,returned);}
};
