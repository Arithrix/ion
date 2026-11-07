// ion-bench: renders fixed scenes offline and reports how much of the real-time
// budget each one uses (HANDOFF §10).
//
//   ion-bench [--syx FILE --voice N] [--seconds S]

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/denormals.h"
#include "core/version.h"
#include "engine/fm/fm_engine.h"
#include "library/syx.h"

namespace {

struct Result {
  double realtimeFactor;   // rendered seconds / wall seconds
  double usPerPeriod128;   // microseconds of DSP per 128-frame buffer
  double averageVoices;    // voices actually sounding, averaged over the run
};

// Plays `voices` notes on a fresh FmEngine for `seconds`, retriggering them every
// 250 ms so decaying patches keep every voice busy.
Result benchFm(const ion::syx::Voice* voice, int voices, double seconds) {
  ion::FmEngine fm(16);
  if (voice) fm.loadVoice(*voice);
  auto note = [](int i) { return static_cast<uint8_t>(48 + (i * 3) % 36); };
  constexpr std::size_t kRetriggerBlocks = ion::kSampleRate / 4 / ion::kBlockSize;
  double voiceSum = 0;

  const auto blocks = static_cast<std::size_t>(seconds * ion::kSampleRate / ion::kBlockSize);
  float left[ion::kBlockSize], right[ion::kBlockSize];
  volatile float sink = 0;
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t b = 0; b < blocks; ++b) {
    if (b % kRetriggerBlocks == 0)
      for (int i = 0; i < voices; ++i) {
        fm.noteOff(note(i));
        fm.noteOn(note(i), 100);
      }
    ion::FmEngine::setClock(b * ion::kBlockSize);
    std::fill(std::begin(left), std::end(left), 0.0f);
    std::fill(std::begin(right), std::end(right), 0.0f);
    fm.render(left, right);
    sink = sink + left[0];
    voiceSum += fm.voiceCount();
  }
  const std::chrono::duration<double> wall = std::chrono::steady_clock::now() - start;
  const double rendered = static_cast<double>(blocks * ion::kBlockSize) / ion::kSampleRate;
  const double periods = rendered * ion::kSampleRate / 128.0;
  return {rendered / wall.count(), wall.count() * 1e6 / periods, voiceSum / static_cast<double>(blocks)};
}

}  // namespace

int main(int argc, char** argv) {
  ion::disableDenormals();
  std::string syxPath;
  int voiceNumber = 1;
  double seconds = 10.0;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string a = argv[i];
    if (a == "--syx") syxPath = argv[i + 1];
    else if (a == "--voice") voiceNumber = std::atoi(argv[i + 1]);
    else if (a == "--seconds") seconds = std::atof(argv[i + 1]);
  }

  std::vector<ion::syx::Voice> bank;
  if (!syxPath.empty()) {
    std::ifstream f(syxPath, std::ios::binary);
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    for (const auto& d : ion::syx::parse(bytes).dumps) {
      const auto v = ion::syx::voices(d);
      bank.insert(bank.end(), v.begin(), v.end());
    }
  }
  const ion::syx::Voice* voice = nullptr;
  std::string voiceName = "INIT VOICE";
  if (!bank.empty()) {
    voice = &bank[static_cast<std::size_t>(std::clamp(voiceNumber - 1, 0, static_cast<int>(bank.size()) - 1))];
    voiceName = ion::syx::name(*voice);
  }

  std::printf("%s · ion-bench · %.0f s per scene\n", ion::versionLine().c_str(), seconds);
  std::printf("FM (Synth_Dexed, Mark I) · patch %s\n", voiceName.c_str());
  std::printf("  notes   sounding   realtime×   µs per 128-frame buffer   %% of 2.67 ms   µs per voice\n");
  for (int n : {1, 4, 8, 16}) {
    const Result r = benchFm(voice, n, seconds);
    std::printf("  %5d   %8.1f   %9.0f   %23.1f   %11.2f%%   %12.2f\n", n, r.averageVoices, r.realtimeFactor,
                r.usPerPeriod128, r.usPerPeriod128 / 2666.7 * 100.0,
                r.usPerPeriod128 / std::max(1.0, r.averageVoices));
  }
  return 0;
}
