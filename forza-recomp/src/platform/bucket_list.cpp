#include "bucket_list.h"
#include "bucket_list_store.h"
#include "guest_addr.h"
#include "guest_memory.h"
#include "challenge_hud.h"
#include "../kernel/live.h"
#include <rex/runtime.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace forza::bucket_list {
namespace {
std::mutex mutex;
Run run;
uint64_t profile=0,last_sample=0,last_hud=0;
uint8_t completed=0;
bool storage_error=false;
std::filesystem::path path;
bool Readable(uint32_t address,uint32_t size){
  auto* kernel=REX_KERNEL_STATE();
  auto* memory=kernel?kernel->memory():nullptr;
  if(!memory||!address||address+size<address)return false;
  // Globals live in the executable image, which isn't a guest heap allocation.
  if(address>=0x82000000&&address+size<=0x84000000)return true;
  auto* heap=memory->LookupHeap(address);uint32_t protect=0;
  return heap&&heap->QueryProtect(address,&protect)&&(protect&rex::memory::kMemoryProtectRead)&&
      heap->QueryProtect(address+size-1,&protect)&&(protect&rex::memory::kMemoryProtectRead);
}
bool Word(uint8_t* base,uint32_t address,uint32_t& value){
  if(!Readable(address,4))return false;
  const auto* p=GuestPtr(base,address);value=uint32_t(p[0])<<24|uint32_t(p[1])<<16|uint32_t(p[2])<<8|p[3];return true;
}
bool Car(uint8_t* base,Sample& sample){
  uint32_t root,holder,list,first,end;
  if(!Word(base,FORZA_ADDR(0x832DF024),root)||!Word(base,root+4,holder)||!Word(base,holder+4,list)||
     !Word(base,list+0x80,first)||!Word(base,list+0x84,end)||end<first||end-first>256||(end-first)%4)return false;
  for(auto slot=first;slot<end;slot+=4){
    uint32_t vehicle,kind,car;
    if(!Word(base,slot,vehicle)||!Word(base,vehicle+0x2DD0,kind)||kind!=0||
       !Word(base,vehicle+0x2D90,car)||car<0x100A20)continue;
    double* xyz[]{&sample.x,&sample.y,&sample.z};bool valid=true;
    for(unsigned i=0;i<3;++i){uint32_t bits=0;valid=Word(base,car-0xA20+4*i,bits)&&valid;*xyz[i]=std::bit_cast<float>(bits);}
    if(!valid||!std::isfinite(sample.x)||!std::isfinite(sample.y)||!std::isfinite(sample.z))return false;
    sample.car=car;return true;
  }
  return false;
}
void Save(){
  storage_error=!ProgressStore(path).Save(completed);
}
std::string Metric(){
  std::ostringstream text;
  const auto kind=kChallenges[run.index()].kind;
  if(kind==Kind::Speed)text<<int(run.progress()*2.236936)<<" / 100 MPH";
  else if(kind==Kind::HoldSpeed)text<<std::fixed<<std::setprecision(1)<<run.progress()<<" / 10 SEC AT 60 MPH";
  else text<<int(run.progress())<<" / 1609 M";
  return text.str();
}
}
void Profile(uint64_t xuid){
  if(!xuid)return;
  std::lock_guard lock(mutex);if(profile==xuid)return;
  run.Cancel();profile=xuid;completed=0;last_sample=last_hud=0;storage_error=false;
  const auto* runtime=rex::Runtime::instance();if(!runtime)return;
  std::ostringstream name;name<<std::hex<<std::setw(16)<<std::setfill('0')<<xuid<<".txt";
  path=runtime->user_data_root()/"bucket-list"/name.str();
  ProgressStore(path).Load(completed);
}
View Snapshot(){std::lock_guard lock(mutex);return {run.phase(),run.index(),completed,run.remaining(),run.progress(),run.fraction(),storage_error};}
bool Start(size_t index){
  std::lock_guard lock(mutex);
  if(!profile||LiveCurrentSession()||!run.Start(index))return false;
  last_sample=last_hud=0;
  REXLOG_INFO("Bucket List: starting {} with current car",kChallenges[index].id);return true;
}
void Cancel(){std::lock_guard lock(mutex);run.Cancel();}
void WorldFrame(uint8_t* base,const telemetry::WorldFrame& frame,uint64_t now){
  std::lock_guard lock(mutex);if(!run.Active())return;
  const auto previous=run.phase();
  if(!frame.loaded||frame.rewinding||LiveCurrentSession())run.Cancel();
  else if(frame.paused){last_sample=0;return;}
  else if(!frame.stepped)return;
  else{
    Sample sample;
    if(!Car(base,sample)){run.Cancel();}
    else{
      const double dt=last_sample&&now>=last_sample?double(now-last_sample)/1000.:0.;
      last_sample=now;run.Step(dt,sample);
    }
  }
  if(run.phase()==Phase::Complete&&previous!=Phase::Complete){completed|=uint8_t(1u<<run.index());Save();}
  if(!run.Active()){
    const char* result=run.phase()==Phase::Complete?"CHALLENGE COMPLETE":run.phase()==Phase::Failed?"TIME EXPIRED":"CHALLENGE CANCELLED";
    ChallengeHudShow("BUCKET LIST",std::string(result)+"\n"+kChallenges[run.index()].name,5);
    REXLOG_INFO("Bucket List: {} {}",kChallenges[run.index()].id,result);return;
  }
  if(now-last_hud>=250){
    last_hud=now;
    const auto line=run.phase()==Phase::Countdown?"START IN "+std::to_string(int(std::ceil(run.remaining()))):Metric();
    ChallengeHudShow("BUCKET LIST",line+"\n"+kChallenges[run.index()].name+" / "+
        std::to_string(int(std::ceil(run.remaining())))+" SEC",1.5,false);
  }
}
std::string Description(size_t index){
  std::lock_guard lock(mutex);if(index>=kChallenges.size())return {};
  if(run.Active())return std::string(kChallenges[index].goal)+"\n\n"+
      (run.index()==index?"IN PROGRESS / RESUME TO CONTINUE":"Finish or cancel the current challenge");
  return std::string(kChallenges[index].goal)+"\n\n"+((completed&(1u<<index))?"COMPLETE / SELECT TO RETRY":"SELECT TO START")+
      "\nCurrent car / single-player free roam";
}
std::string Status(){
  std::lock_guard lock(mutex);
  unsigned count=0;for(unsigned i=0;i<3;++i)if(completed&(1u<<i))++count;
  if(run.Active())return std::string(kChallenges[run.index()].name)+" / "+Metric()+" / "+std::to_string(int(std::ceil(run.remaining())))+" SEC";
  const char* result=run.phase()==Phase::Complete?"COMPLETE":run.phase()==Phase::Failed?"TIME EXPIRED":run.phase()==Phase::Cancelled?"CANCELLED":"READY";
  return std::to_string(count)+" / 3 COMPLETE / "+result+(storage_error?" / SAVE FAILED":"");
}
}
