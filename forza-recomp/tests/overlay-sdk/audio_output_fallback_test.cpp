// The audio output's fallback order and the silent output's pacing (SDK patch 0103, #244), from
// the applied patch's private header, not a copy. No audio device needed.
#include "audio_output_fallback.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace rex::audio::sdl;
using namespace std::chrono_literals;

static void Check(bool value, const char* what) {
  if (!value) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

static const OutputRequest kGameMix{48000, kOutputFormatF32, 6};
static const OutputRequest kStereoF32{48000, kOutputFormatF32, 2};
static const OutputRequest kStereoS16{48000, kOutputFormatS16, 2};

static void TestOrder() {
  // Without a device format: the game's mix, then plain stereo, float and 16-bit.
  auto order = OutputOpenOrder(std::nullopt);
  Check(order.size() == 3, "three attempts without a device format");
  Check(order[0].request == kGameMix, "the game's mix first");
  Check(order[1].request == kStereoF32, "float stereo second");
  Check(order[2].request == kStereoS16, "16-bit stereo last");

  // A 192 kHz, 32-bit, 7.1 device: its own format right after the game's mix.
  const OutputRequest device{192000, 0x8020, 8};
  order = OutputOpenOrder(device);
  Check(order.size() == 4, "four attempts with a device format");
  Check(order[0].request == kGameMix, "the game's mix still first");
  Check(order[1].request == device, "the device's own format second");
  Check(order[2].request == kStereoF32 && order[3].request == kStereoS16, "stereo after it");

  // A device format equal to another attempt is asked for once.
  order = OutputOpenOrder(kStereoF32);
  Check(order.size() == 3, "no duplicate attempt");
  Check(order[1].request == kStereoF32 && order[2].request == kStereoS16, "order kept");

  // A format that makes no sense is left out.
  Check(OutputOpenOrder(OutputRequest{0, kOutputFormatF32, 2}).size() == 3, "no rate");
  Check(OutputOpenOrder(OutputRequest{48000, kOutputFormatF32, 0}).size() == 3, "no channels");
  Check(OutputOpenOrder(OutputRequest{48000, 0, 2}).size() == 3, "no sample format");
}

// OpenOutputInOrder with a device that refuses the first `refusals` attempts.
struct Run {
  std::vector<OutputRequest> asked;
  std::vector<int> rounds;
  std::vector<std::chrono::milliseconds> waits;
  int orders = 0;
  bool opened = false;
};

static Run OpenWithRefusals(int refusals) {
  Run run;
  const OutputRequest device{44100, kOutputFormatS16, 2};
  run.opened = OpenOutputInOrder(
      [&] {
        ++run.orders;
        return OutputOpenOrder(device);
      },
      [&](const OutputAttempt& attempt, int round) {
        run.asked.push_back(attempt.request);
        run.rounds.push_back(round);
        return int(run.asked.size()) > refusals;
      },
      [&](std::chrono::milliseconds delay) { run.waits.push_back(delay); });
  return run;
}

static void TestStartSequence() {
  Run run = OpenWithRefusals(0);
  Check(run.opened && run.asked.size() == 1 && run.asked[0] == kGameMix, "a working device");
  Check(run.waits.empty() && run.orders == 1, "no wait when the first attempt opens");

  run = OpenWithRefusals(1);
  Check(run.opened && run.asked.size() == 2, "second attempt opens");
  Check(run.asked[1] == OutputRequest{44100, kOutputFormatS16, 2}, "the device's own format");

  // Every format refused once: the whole order again after the delay, the order rebuilt.
  run = OpenWithRefusals(4);
  Check(run.opened && run.asked.size() == 5, "the retry round opens");
  Check(run.waits.size() == 1 && run.waits[0] == kOutputRetryDelay, "one wait between rounds");
  Check(run.orders == 2, "the order is asked again for the retry");
  Check(run.rounds[3] == 0 && run.rounds[4] == 1 && run.asked[4] == kGameMix, "round 2 restarts");

  // Nothing opens: two rounds of four, then the caller runs the silent output.
  run = OpenWithRefusals(1000);
  Check(!run.opened && run.asked.size() == 8, "two full rounds");
  Check(run.waits.size() == 1, "still one wait");
}

static void TestFailOpenCount() {
  Check(ParseFailOpenCount(nullptr) == 0, "unset");
  Check(ParseFailOpenCount("") == 0, "empty");
  Check(ParseFailOpenCount("0") == 0, "zero");
  Check(ParseFailOpenCount("3") == 3, "a count");
  Check(ParseFailOpenCount("all") == UINT64_MAX, "all");
  Check(ParseFailOpenCount("x") == 0 && ParseFailOpenCount("2x") == 0, "garbage");
  Check(ParseFailOpenCount("-1") == 0, "negative");
}

static void TestSilentClock() {
  using Clock = SilentClock::Clock;
  const Clock::time_point t0{};
  const auto at = [&](std::chrono::nanoseconds d) { return t0 + std::chrono::ceil<Clock::duration>(d); };

  SilentClock pace(48000, 256, 200ms);
  pace.Start(t0);
  Check(pace.Take(t0) == 0, "nothing due at the start");
  // One frame of the game (256 samples) lasts 5.333 ms.
  Check(pace.NextDue() == at(5333334ns), "first wake-up after one frame");
  Check(pace.Take(at(5333334ns)) == 256, "one frame due");
  Check(pace.NextDue() == at(10666667ns), "second wake-up after two frames");

  // Uneven wake-ups (a coarse timer) still add up to exactly 48000 a second, over ten minutes.
  pace.Start(t0);
  uint64_t total = 0;
  std::chrono::nanoseconds t{0};
  uint32_t step = 1;
  while (t < 600s) {
    step = step * 1103515245u + 12345u;
    t += std::chrono::microseconds(1000 + (step >> 8) % 30000);  // 1 to 31 ms late
    total += pace.Take(at(t));
  }
  const uint64_t expected =
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t).count()) * 48000 / 1000000000;
  Check(total == expected, "no drift over ten minutes");

  // A gap past 200 ms (suspended, a slow reopen): start over, do not ask for the backlog.
  pace.Start(t0);
  Check(pace.Take(at(100ms)) == 4800, "100 ms is played");
  Check(pace.Take(at(1100ms)) == 0, "a 1 s gap is skipped");
  Check(pace.Take(at(1110ms)) == 480, "and the clock goes on from there");
}

int main() {
  TestOrder();
  TestStartSequence();
  TestFailOpenCount();
  TestSilentClock();
  std::puts("audio output fallback: ok");
  return 0;
}
