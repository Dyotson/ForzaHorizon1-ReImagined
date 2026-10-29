#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <cstddef>
#include <atomic>

// Phase A is observation only. This protocol never authorizes a raster change.
namespace fh1::provenance {
constexpr uint32_t kMagic=0x46483150, kVersion=1, kWords=10;
constexpr uint32_t kNop=0xC0001000u|((kWords-1)<<16);
inline void StoreBigEndian(uint8_t* destination,uint32_t value) {
  for(unsigned i=0;i<4;++i)destination[i]=uint8_t(value>>(24-i*8));
}
inline uint32_t NextId(std::atomic<uint32_t>& counter) {
  auto old=counter.load();
  while(old!=UINT32_MAX){if(counter.compare_exchange_weak(old,old+1))return old+1;}
  return 0; // Exhausted for process lifetime, never wraps to a reused token.
}
inline bool TraceAllowed(bool armed,uint32_t eligible,uint32_t& records) {
  return armed&&eligible&&records<4096?(++records,true):false;
}
enum class Op:uint16_t {Reset=1,FrameBegin,FrameEnd,Begin,End,Target,Context};
enum class Phase:uint32_t {Unknown,OriginalFrame,PlayerCar,OtherCar,Scene,Sky};
struct Record {
  Op op=Op::Reset;uint32_t token=0,serial=0;Phase phase=Phase::Unknown;
  // Target records: native color0/depth, their native descriptor28/32 words,
  // and binding serial. Context records carry graphics/core/camera/car/generation.
  std::array<uint32_t,5> data{};
};
inline std::array<uint32_t,kWords> Encode(const Record& r) {
  return {kMagic,(kVersion<<16)|uint16_t(r.op),r.token,r.serial,uint32_t(r.phase),
          r.data[0],r.data[1],r.data[2],r.data[3],r.data[4]};
}
enum class DecodeResult {Unrelated,Valid,Malformed};
inline DecodeResult Decode(std::span<const uint32_t> words,Record& r) {
  if(words.empty()||words[0]!=kMagic)return DecodeResult::Unrelated;
  if(words.size()!=kWords||words[1]>>16!=kVersion||
     uint16_t(words[1])<uint16_t(Op::Reset)||uint16_t(words[1])>uint16_t(Op::Context)||
     words[4]>uint32_t(Phase::Sky))return DecodeResult::Malformed;
  r={Op(uint16_t(words[1])),words[2],words[3],Phase(words[4]),
     {words[5],words[6],words[7],words[8],words[9]}};
  return DecodeResult::Valid;
}
struct Context {
  Phase phase=Phase::Unknown;
  uint32_t token=0,frame=0,binding=0,color=0,depth=0,color_desc=0,depth_desc=0;
  bool valid=false; // Valid provenance, never a main-target/auxiliary classification.
};
class Consumer {
 public:
  void Eligibility(uint32_t token) {
    if(token!=eligible_){Reset();eligible_=token;last_frame_=0;}
  }
  void Reset() {current_={};depth_=0;last_serial_=0;fault_=false;}
  void Fault() {current_={};depth_=0;fault_=true;}
  Context context()const{return current_;}
  bool fault()const{return fault_;}
  uint32_t eligible()const{return eligible_;}
  void Consume(std::span<const uint32_t> words) {
    if(!eligible_)return;
    Record r;const auto d=Decode(words,r);
    if(d==DecodeResult::Unrelated)return;
    if(d!=DecodeResult::Valid||r.token!=eligible_){Fault();return;}
    if(r.op==Op::Reset){Reset();return;}
    if(r.op==Op::FrameBegin) {
      if(current_.valid||!r.serial||r.serial<=last_frame_){Fault();return;}
      Reset();last_frame_=r.serial;last_serial_=r.serial;
      current_={Phase::Unknown,r.token,r.serial,0,0,0,0,0,true};return;
    }
    if(fault_||!current_.valid)return;
    if(r.op==Op::FrameEnd) {
      if(depth_||r.serial!=current_.frame){Fault();return;}
      Reset();return;
    }
    if(r.op==Op::Begin) {
      if(depth_==stack_.size()||r.serial<=last_serial_){Fault();return;}
      stack_[depth_++]={current_.phase,r.serial};last_serial_=r.serial;
      current_.phase=r.phase;return;
    }
    if(r.op==Op::End) {
      if(!depth_||stack_[depth_-1].serial!=r.serial){Fault();return;}
      current_.phase=stack_[--depth_].phase;return;
    }
    if(r.serial<=last_serial_){Fault();return;}
    last_serial_=r.serial;
    if(r.op==Op::Target) {
      if(!r.data[4]||r.data[4]<=current_.binding){Fault();return;}
      current_.color=r.data[0];current_.depth=r.data[1];
      current_.color_desc=r.data[2];current_.depth_desc=r.data[3];current_.binding=r.data[4];
    }
  }
  // Untagged IBs never inherit player provenance. Only a balanced child can
  // return to its saved parent, and only within the same live session/frame.
  void EnterBuffer() {
    if(buffers_==buffer_.size()){Fault();return;}
    buffer_[buffers_++]={current_,depth_,last_serial_,fault_,stack_};
    current_.phase=Phase::Unknown;depth_=0;
  }
  void LeaveBuffer() {
    if(!buffers_){Fault();return;}
    const auto saved=buffer_[--buffers_];
    if(fault_||depth_||saved.context.token!=eligible_||
       current_.frame!=saved.context.frame){Fault();return;}
    current_=saved.context;depth_=saved.depth;
    if(last_serial_<saved.serial)last_serial_=saved.serial;
    fault_=saved.fault;stack_=saved.stack;
  }
 private:
  struct Entry {Phase phase=Phase::Unknown;uint32_t serial=0;};
  struct Buffer {Context context;size_t depth=0;uint32_t serial=0;bool fault=false;std::array<Entry,16> stack;};
  Context current_{};uint32_t eligible_=0,last_frame_=0,last_serial_=0;
  size_t depth_=0,buffers_=0;bool fault_=false;
  std::array<Entry,16> stack_{};std::array<Buffer,8> buffer_{};
};

// This is the production NOP parser used by the CP and the byte-path test.
// Diagnostics may inspect disabled packets, but never enable consumption or
// introduce a fault when the ordinary disabled path would have skipped them.
enum class NopDisposition {Disabled,Unrelated,CountMismatch,Consumed,Truncated};
struct NopReceipt {
  NopDisposition disposition=NopDisposition::Disabled;
  uint32_t count=0,available=0;
  std::array<uint32_t,kWords> words{};
  bool ok=true,recognized=false;
};
template<class Reader> NopReceipt ParseNop(Reader& reader,uint32_t count,Consumer& consumer,
                                         bool inspect_disabled=false) {
  NopReceipt result;result.count=count;result.available=reader.read_count();
  const bool eligible=consumer.eligible()!=0;
  if(!eligible&&!inspect_disabled){reader.AdvanceRead(count*4);return result;}
  if(uint64_t(count)*4>reader.read_count()) {
    result.disposition=NopDisposition::Truncated;
    if(eligible){consumer.Fault();result.ok=false;}
    // The disabled production path only advances, even for malformed input.
    else reader.AdvanceRead(count*4);
    return result;
  }
  if(!count)return result;
  result.words[0]=reader.template ReadAndSwap<uint32_t>();
  if(result.words[0]!=kMagic) {
    reader.AdvanceRead((count-1)*4);result.disposition=NopDisposition::Unrelated;return result;
  }
  result.recognized=true;
  if(count!=kWords) {
    reader.AdvanceRead((count-1)*4);result.disposition=NopDisposition::CountMismatch;
    if(eligible)consumer.Fault();return result;
  }
  for(uint32_t i=1;i<count;++i)result.words[i]=reader.template ReadAndSwap<uint32_t>();
  if(eligible){consumer.Consume(result.words);result.disposition=NopDisposition::Consumed;}
  return result;
}

// Independent of frame-trace arming and consumer resets. Only diagnostics call
// this object; production-disabled calls leave every counter and cap untouched.
struct IngressDiagnostics {
  uint32_t packets=0,eligible=0,recognized=0,disabled_markers=0,malformed=0;
  uint32_t epoch=0,lines=0;
  void Observe(bool enabled,uint32_t token,const NopReceipt& r) {
    if(!enabled)return;
    auto increment=[](uint32_t& n){if(n!=UINT32_MAX)++n;};
    increment(packets);if(token)increment(eligible);
    if(r.recognized){increment(recognized);if(!token)increment(disabled_markers);}
    if(r.disposition==NopDisposition::CountMismatch||r.disposition==NopDisposition::Truncated)increment(malformed);
  }
  bool Admit(bool enabled) {return enabled&&lines<256?(++lines,true):false;}
  void Swap(bool enabled) {if(enabled&&epoch!=UINT32_MAX)++epoch;}
};
inline void ResetAtSwap(Consumer& consumer,IngressDiagnostics& diagnostic,bool enabled) {
  consumer.Reset();diagnostic.Swap(enabled);
}

struct Scene {uint32_t core=0,camera=0,car=0,graphics=0;uint64_t generation=0;};
inline bool ExactContext(const Scene& scene,uint32_t core,uint32_t camera,uint32_t graphics) {
  return scene.generation&&scene.core&&scene.camera&&scene.car&&scene.graphics&&
         core==scene.core&&camera==scene.camera&&graphics==scene.graphics;
}
inline Phase CarPhase(const Scene& scene,uint32_t car,uint32_t parent) {
  if(!scene.generation||!car||parent!=scene.core)return Phase::Unknown;
  return car==scene.car?Phase::PlayerCar:Phase::OtherCar;
}
// Native48 is the last written dword; limit56 bounds available immediate slab.
// Do not allocate/flush/extend a recording. If unavailable, emit nothing and
// invalidate this host session. Ordinary native code still forwards unchanged.
inline bool Capacity(uint32_t cursor,uint32_t limit,uint32_t bytes,bool recording) {
  return !recording&&cursor&&!(cursor&3)&&!(limit&3)&&bytes&&!(bytes&3)&&
         uint64_t(cursor)+bytes<=limit;
}
template<class IO> bool Emit(IO& io,const Record& r) {
  uint32_t cursor=0,limit=0;bool recording=true;
  if(!io.Writer(cursor,limit,recording)||!Capacity(cursor,limit,(kWords+1)*4,recording)||
     !io.Writable(cursor+4,(kWords+1)*4))return false;
  const auto words=Encode(r);
  io.Store(cursor+4,kNop);
  for(uint32_t i=0;i<kWords;++i)io.Store(cursor+8+i*4,words[i]);
  io.Commit(cursor+(kWords+1)*4);return true;
}
template<class IO,class Forward> void ObserveAndForward(IO& io,Phase phase,Forward&& forward) {
  struct Exit {IO& io;uint32_t serial;~Exit(){if(serial)io.End(serial);}};
  const Exit exit{io,io.Begin(phase)};
  forward(); // Even when recording is disabled or the marker cannot be emitted.
}
} // namespace fh1::provenance
