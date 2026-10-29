#pragma once
#include <cstdint>
#if defined(FORZA_TITLE_UPDATE) && FORZA_TITLE_UPDATE == 4 && defined(FORZA_NATIVE_PAUSE)
namespace forza::pause_scene_provenance {
class ParticleMainScope {public:ParticleMainScope(uint8_t* base,unsigned object,unsigned graphics,unsigned kind,unsigned second,unsigned caller);~ParticleMainScope();private:unsigned serial_=0;};
class CrowdMainScope {public:CrowdMainScope(uint8_t* base,unsigned entry,unsigned object,unsigned graphics,unsigned root,unsigned pass,unsigned caller);~CrowdMainScope();private:unsigned serial_=0;};
class ProceduralMainScope {public:ProceduralMainScope(uint8_t* base,unsigned object,unsigned graphics,unsigned root,unsigned stack,unsigned entry);~ProceduralMainScope();private:unsigned serial_=0;};
class TrackMainScope {public:TrackMainScope(uint8_t* base,unsigned presentation,unsigned pass);~TrackMainScope();private:unsigned serial_=0;};
}
#else
namespace forza::pause_scene_provenance {
class ParticleMainScope {public:ParticleMainScope(uint8_t*,unsigned,unsigned,unsigned,unsigned,unsigned){};};
class CrowdMainScope {public:CrowdMainScope(uint8_t*,unsigned,unsigned,unsigned,unsigned,unsigned,unsigned){};};
class ProceduralMainScope {public:ProceduralMainScope(uint8_t*,unsigned,unsigned,unsigned,unsigned,unsigned){};};
class TrackMainScope {public:TrackMainScope(uint8_t*,unsigned,unsigned){};};
}
#endif
namespace forza::pause_scene_provenance {
bool Enabled();
void Cancel();
void Cancel(const char* reason);
}
