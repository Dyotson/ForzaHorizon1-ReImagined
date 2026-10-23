#pragma once
#include "bucket_list_model.h"
#include "driving_gate.h"
#include <string>

namespace forza::bucket_list {
struct View {
  Phase phase=Phase::Idle;
  size_t index=0;
  uint8_t completed=0;
  double remaining=0,progress=0,fraction=0;
  bool storage_error=false;
};
// The native pause supplies the actual offline XUID, even during Live sign-in.
void Profile(uint64_t xuid);
View Snapshot();
bool Start(size_t index);
void Cancel();
void WorldFrame(uint8_t* base,const telemetry::WorldFrame& frame,uint64_t now);
std::string Description(size_t index);
std::string Status();
}
