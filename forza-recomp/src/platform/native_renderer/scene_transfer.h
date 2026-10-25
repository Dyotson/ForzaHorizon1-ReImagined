#pragma once
#include <array>
#include <cstdint>
#include <optional>

namespace forza::native_renderer {
// This identity describes bytes, not a guest address alone. A caller must also
// verify a complete, single-owner R10 source and the supported R10 conversion.
struct SceneSnapshotKey {
  uint32_t address=0,bytes=0,width=0,height=0,pitch=0,endian=0,scale_x=0,scale_y=0;
  bool operator==(const SceneSnapshotKey& b) const {
    return address==b.address && bytes==b.bytes && width==b.width && height==b.height &&
        pitch==b.pitch && endian==b.endian && scale_x==b.scale_x && scale_y==b.scale_y;
  }
};
inline bool Valid(const SceneSnapshotKey& k) {
  return k.address && !(k.address&4095) && k.bytes && uint64_t(k.address)+k.bytes<=0x20000000 &&
      k.width==1280 && k.height==720 && k.pitch==1280 && k.endian<=3 &&
      k.scale_x>=1 && k.scale_x<=2 && k.scale_y==k.scale_x &&
      k.bytes>=uint64_t(k.pitch)*k.height*4;
}
// The owner serializes these operations with the memory-invalidation lock.
// GPU resources remain alive independently of validity until their last waited
// submission. Invalidating an address never frees an in-flight image.
class SceneSnapshotTracker {
 public:
  static constexpr uint32_t kSlots=8;
  std::optional<uint32_t> Reserve(uint64_t submission,uint64_t completed) {
    if(submission<=completed || submission<last_submission_ || completed<last_completed_)return {};
    for(uint32_t n=0;n<kSlots;++n){const uint32_t i=(next_+n)%kSlots;
      if(slots_[i].submission>completed)continue;
      slots_[i]={};slots_[i].submission=submission;
      next_=(i+1)%kSlots;last_submission_=submission;last_completed_=completed;return i;
    }
    return {};
  }
  bool Publish(uint32_t slot,const SceneSnapshotKey& key) {
    if(slot>=kSlots || !slots_[slot].submission || !Valid(key))return false;
    Invalidate(key.address,key.address+key.bytes-1);
    slots_[slot].key=key;slots_[slot].valid=true;return true;
  }
  std::optional<uint32_t> Find(const SceneSnapshotKey& key,uint64_t submission) {
    if(!Valid(key) || submission<last_submission_)return {};
    for(uint32_t i=0;i<kSlots;++i)if(slots_[i].valid && slots_[i].key==key){
      slots_[i].submission=submission;last_submission_=submission;return i;
    }
    return {};
  }
  void Invalidate(uint32_t first,uint32_t last) {
    if(first>last)return;
    for(auto& s:slots_)if(s.valid && first<uint64_t(s.key.address)+s.key.bytes && last>=s.key.address)s.valid=false;
  }
  void Clear(){for(auto& s:slots_)s.valid=false;}
 private:
  struct Slot {SceneSnapshotKey key{};uint64_t submission=0;bool valid=false;};
  std::array<Slot,kSlots> slots_{};
  uint32_t next_=0;
  uint64_t last_submission_=0,last_completed_=0;
};
} // namespace forza::native_renderer
