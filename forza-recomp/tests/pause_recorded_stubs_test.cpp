#include "../src/platform/pause_scene_provenance.h"
#include <array>
#include <cassert>
int main(){
  std::array<uint8_t,64> bytes{};const auto before=bytes;
  {forza::pause_scene_provenance::TrackMainScope track(bytes.data(),1,4);
   forza::pause_scene_provenance::ProceduralMainScope leaf(bytes.data(),1,2,3,4,5);
   forza::pause_scene_provenance::CrowdMainScope crowd(bytes.data(),1,2,3,4,5,6);
   forza::pause_scene_provenance::ParticleMainScope particles(bytes.data(),1,2,3,4,5);}
  assert(bytes==before);
}
