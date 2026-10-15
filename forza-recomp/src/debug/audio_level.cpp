// forza - audio level meter for unattended runs.
//
// FORZA_AUDIOLOG=1 logs the peak and RMS of the samples the game submits to the audio
// driver (XAudioSubmitRenderDriverFrame) every 5 seconds, so a scripted run can tell
// whether the game produces sound without anyone listening. A frame is 256 samples for
// each of 6 channels, big-endian floats, one channel after the other (front left, front
// right, center, LFE, back left, back right); each channel's RMS is logged too.
// FORZA_AUDIOLOG_MS sets the window in milliseconds.

#include "../platform/guest_memory.h"
#include "../platform/sdk_import.h"

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace {

using GuestFunc = void (*)(PPCContext&, uint8_t*);

constexpr int kChannels = 6;
constexpr int kSamplesPerChannel = 256;

bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("FORZA_AUDIOLOG");
    return v && *v && *v != '0';
  }();
  return on;
}

struct Window {
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point first = start;
  double sum_squares = 0;
  double channel_squares[kChannels] = {};
  float peak = 0;
  uint64_t samples = 0;
  uint32_t frames = 0;
};

void Measure(uint8_t* base, uint32_t samples_addr) {
  static Window w;
  const uint8_t* p = forza::GuestPtr(base, samples_addr);
  for (int i = 0; i < kChannels * kSamplesPerChannel; ++i) {
    uint32_t raw;
    std::memcpy(&raw, p + 4 * i, 4);
    raw = __builtin_bswap32(raw);
    float v;
    std::memcpy(&v, &raw, 4);
    if (!std::isfinite(v)) continue;
    w.peak = std::max(w.peak, std::fabs(v));
    w.sum_squares += double(v) * v;
    w.channel_squares[i / kSamplesPerChannel] += double(v) * v;
  }
  w.samples += kChannels * kSamplesPerChannel;
  ++w.frames;
  const auto now = std::chrono::steady_clock::now();
  static const auto window = std::chrono::milliseconds([] {
    const char* v = std::getenv("FORZA_AUDIOLOG_MS");
    return v && *v ? std::atoi(v) : 5000;
  }());
  if (now - w.start < window) return;
  const double t = std::chrono::duration<double>(now - w.first).count();
  double ch[kChannels];
  for (int c = 0; c < kChannels; ++c)
    ch[c] = std::sqrt(w.channel_squares[c] / double(w.frames ? w.frames * kSamplesPerChannel : 1));
  REXLOG_INFO(
      "FORZA_AUDIOLOG: t={:.1f}s frames={} peak={:.4f} rms={:.5f} channels {:.5f} {:.5f} {:.5f} {:.5f} {:.5f} {:.5f}",
      t, w.frames, w.peak, std::sqrt(w.sum_squares / double(w.samples ? w.samples : 1)), ch[0], ch[1], ch[2], ch[3],
      ch[4], ch[5]);
  const auto first = w.first;
  w = Window{};
  w.first = first;
}

}  // namespace

// DWORD XAudioSubmitRenderDriverFrame(void* driver, float* samples)
extern "C" REX_FUNC(__imp__XAudioSubmitRenderDriverFrame) {
  static GuestFunc sdk = forza::SdkImport("__imp__XAudioSubmitRenderDriverFrame");
  if (Enabled() && ctx.r4.u32) Measure(base, ctx.r4.u32);
  if (sdk) sdk(ctx, base);
}
