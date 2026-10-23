#include "../src/platform/bucket_list_model.h"
#include "../src/platform/social_presence.h"
#include "../src/platform/bucket_list_store.h"
#include "../src/platform/world_session.h"
#include <cassert>
#include <chrono>
#include <limits>

using namespace forza::bucket_list;
void Begin(Run& run,size_t index){
  assert(run.Start(index));run.Step(0,{1,0,0,0});
  for(unsigned i=0;i<3;++i)run.Step(1,{1,0,0,0});
  assert(run.phase()==Phase::Running);
}
int main(){
  assert(forza::world_session::IsWorld(3,7,7));
  assert(forza::world_session::IsWorld(0,7,7)); // Invite, no local search.
  assert(!forza::world_session::IsWorld(2,7,7));
  assert(!forza::world_session::IsWorld(0,7,1));
  assert(!forza::world_session::IsWorld(0,10,7));
  {Run r;assert(!r.Start(3));Begin(r,0);assert(!r.Start(1));
    r.Step(1,{1,44,0,0});assert(r.phase()==Phase::Running);
    r.Step(1,{1,88.704,0,0});assert(r.phase()==Phase::Complete);
    assert(r.fraction()==1);assert(r.Start(2));assert(r.progress()==0);}
  {Run r;Begin(r,1);double x=0;
    for(int i=0;i<4;++i){x+=30;r.Step(1,{1,x,0,0});}
    assert(r.progress()==4);x+=20;r.Step(1,{1,x,0,0});assert(r.progress()==0);
    for(int i=0;i<10;++i){x+=30;r.Step(1,{1,x,0,0});}
    assert(r.phase()==Phase::Complete);}
  {Run r;Begin(r,2);for(int i=1;i<=54;++i)r.Step(1,{1,i*30.,0,0});assert(r.phase()==Phase::Complete);}
  {Run r;Begin(r,0);for(int i=0;i<60;++i)r.Step(1,{1,0,0,0});assert(r.phase()==Phase::Failed);}
  {Run r;Begin(r,0);r.Step(61,{1,61*50.,0,0});assert(r.phase()==Phase::Failed);}
  {Run r;Begin(r,2);r.Step(1,{1,1000,0,0});assert(r.phase()==Phase::Cancelled);}
  {Run r;Begin(r,0);r.Step(.01,{1,10,0,0});assert(r.phase()==Phase::Cancelled);}
  {Run r;Begin(r,2);r.Step(1,{2,1,0,0});assert(r.phase()==Phase::Cancelled);}
  {Run r;Begin(r,2);r.Step(std::numeric_limits<double>::quiet_NaN(),{1,1,0,0});assert(r.phase()==Phase::Cancelled);}
  {Run r;Begin(r,2);r.Cancel();r.Step(1,{1,30,0,0});assert(r.phase()==Phase::Cancelled);assert(r.Start(2));}
  {Run r;Begin(r,1);const auto before=r.remaining();assert(r.remaining()==before);r.Step(0,{1,0,0,0});assert(r.remaining()==before);}
  using namespace forza::social_presence;
  assert(!InteractiveSession(0x702,0)); // Offline placeholder.
  assert(InteractiveSession(0x823,42)); // Native presence party.
  assert(InteractiveSession(0x2D,42)); // Horizon World gameplay replaces the party.
  assert(InteractiveSession(0x2C,42)); // Joined gameplay session.
  assert(!InteractiveSession(0x4,42)); // Stats-only session is not a playable party.
  assert(JoinState(0x23)==0x13);
  assert(JoinState(0x823)==0x113);
  assert(JoinState(0x2D)==0x113);
  assert(!JoinState(0x223)&&!JoinState(0x22D));
  const Car car{123,437,4,"Lancer Evolution X GSR"};
  std::vector<uint8_t> extra{8,0,2,0,0,3,1,2,3};Append(extra,car);
  assert(Read(extra)&&*Read(extra)==car);
  for(size_t size=0;size<extra.size();++size){auto broken=extra;broken.resize(size);assert(!Read(broken));}
  {auto broken=extra;broken[4]=255;broken[5]=255;assert(!Read(broken));}
  {auto broken=extra;broken[13]=2;assert(!Read(broken));}
  {auto dirty=car;dirty.name="Driver\nCar";std::vector<uint8_t> bytes(6);Append(bytes,dirty);assert(Read(bytes)->name=="Driver Car");}
  assert(!Read(std::vector<uint8_t>(6)));assert(std::string(ClassName(4))=="B");
  {std::vector<uint8_t> bytes(1006);const auto before=bytes;Append(bytes,car);assert(bytes==before);}
  assert(Joinable(0x13,42,false,false,false));
  assert(!Joinable(0x13,0,true,false,false));
  assert(!Joinable(0x12,42,true,false,false));
  assert(!Joinable(0x113,42,false,false,false));
  assert(Joinable(0x113,42,true,false,false));
  assert(!Joinable(0x13,42,true,true,false));
  assert(!Joinable(0x13,42,true,false,true));
  const auto folder=std::filesystem::temp_directory_path()/
      ("fh1-bucket-store-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  assert(std::filesystem::create_directory(folder));
  const auto first=folder/"profile-a.txt",second=folder/"profile-b.txt";
  ProgressStore a(first),b(second);uint8_t mask=0;
  assert(a.Save(1)&&b.Save(2));assert(a.Load(mask)&&mask==1);assert(b.Load(mask)&&mask==2);
  assert(a.Save(3));assert(ProgressStore(first).Load(mask)&&mask==3);
  assert(!a.Save(8));assert(a.Load(mask)&&mask==3);
  {std::ofstream out(first);out<<"broken data";}
  assert(a.Load(mask)&&mask==1); // Last complete write survives a corrupt primary.
  std::filesystem::create_directory(second.string()+".tmp");
  assert(!b.Save(7));assert(b.Load(mask)&&mask==2);
  std::filesystem::remove(second.string()+".tmp");
  for(const auto& file:{first,second})for(const auto& suffix:{"",".bak",".tmp"})
    std::filesystem::remove(file.string()+suffix);
  assert(std::filesystem::remove(folder));
}
