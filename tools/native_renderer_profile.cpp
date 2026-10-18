// GPU replay timings only. Captures and generated results stay outside the repo.
#include "renderdoc_replay.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>

REPLAY_PROGRAM_MARKER();

void Collect(const rdcarray<ActionDescription>& actions, const SDFile& file,
             std::map<uint32_t, std::string>& names) {
  for (const auto& action : actions) {
    names[action.eventId] = action.GetName(file).c_str();
    Collect(action.children, file, names);
  }
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "Usage: native_renderer_profile capture.rdc timings.tsv\n";
    return 2;
  }
  GlobalEnvironment environment;
  environment.enumerateGPUs = false;
  RENDERDOC_InitialiseReplay(environment, {});
  auto* capture = RENDERDOC_OpenCaptureFile();
  if (!capture->OpenFile(argv[1], "", nullptr).OK()) {
    capture->Shutdown(); RENDERDOC_ShutdownReplay(); return 3;
  }
  auto opened = capture->OpenCapture(ReplayOptions(), nullptr);
  if (!opened.first.OK()) {
    capture->Shutdown(); RENDERDOC_ShutdownReplay(); return 4;
  }
  auto* controller = opened.second;
  int result = 0;
  try {
    const auto counters = controller->EnumerateCounters();
    if (std::find(counters.begin(), counters.end(), GPUCounter::EventGPUDuration) == counters.end())
      throw std::runtime_error("GPU duration counter unavailable");
    std::map<uint32_t, std::string> names;
    Collect(controller->GetRootActions(), controller->GetStructuredFile(), names);
    const auto timings = controller->FetchCounters({GPUCounter::EventGPUDuration});
    if (timings.empty()) throw std::runtime_error("Replay returned no GPU timings");
    std::ofstream output(argv[2]);
    output << "event\tgpu_ms\taction\n" << std::setprecision(12);
    for (const auto& timing : timings) {
      if (timing.counter != GPUCounter::EventGPUDuration || !std::isfinite(timing.value.d) || timing.value.d < 0)
        throw std::runtime_error("Invalid GPU duration");
      auto name = names[timing.eventId];
      std::replace(name.begin(), name.end(), '\t', ' ');
      std::replace(name.begin(), name.end(), '\n', ' ');
      std::replace(name.begin(), name.end(), '\r', ' ');
      output << timing.eventId << '\t' << timing.value.d * 1000 << '\t' << name << '\n';
    }
    if (!output) throw std::runtime_error("Could not write timings");
    std::cout << timings.size() << " replay events timed. Replay timings are not live frame times.\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; result = 5;
  }
  controller->Shutdown(); capture->Shutdown(); RENDERDOC_ShutdownReplay();
  return result;
}
