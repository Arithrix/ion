// ion-bench: renders fixed scenes offline and reports how much of the real-time
// budget each one uses (HANDOFF §10).
//
//   ion-bench [--syx FILE --voice N] [--seconds S]

#include <algorithm>
#include <chrono>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/denormals.h"
#include "core/version.h"
#include "engine/drum/drum_engine.h"
#include "engine/fm/fm_engine.h"
#include "engine/fx/effects.h"
#include "engine/module.h"
#include "engine/acid/acid_engine.h"
#include "engine/chip/chip_engine.h"
#include "engine/plaits/plaits_engine.h"
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
  ion::FmEngine fm;
  fm.setParam(ion::FmEngine::kPoly, 16);
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
    fm.render(left, right, ion::kBlockSize);
    sink = sink + left[0];
    voiceSum += fm.voiceCount();
  }
  const std::chrono::duration<double> wall = std::chrono::steady_clock::now() - start;
  const double rendered = static_cast<double>(blocks * ion::kBlockSize) / ion::kSampleRate;
  const double periods = rendered * ion::kSampleRate / 128.0;
  return {rendered / wall.count(), wall.count() * 1e6 / periods, voiceSum / static_cast<double>(blocks)};
}

// Plaits: `voices` held notes on one model, restruck every 250 ms (the drum
// and string models decay; this keeps every voice busy).
Result benchPlaits(int model, int voices, double seconds) {
  ion::PlaitsEngine e;
  e.setParam(ion::PlaitsEngine::kModel, static_cast<float>(model));
  e.setParam(ion::PlaitsEngine::kPoly, static_cast<float>(voices));
  auto note = [](int i) { return static_cast<uint8_t>(48 + (i * 4) % 30); };
  constexpr std::size_t kRetriggerBlocks = ion::kSampleRate / 4 / ion::kBlockSize;
  const auto blocks = static_cast<std::size_t>(seconds * ion::kSampleRate / ion::kBlockSize);
  float left[ion::kBlockSize], right[ion::kBlockSize];
  volatile float sink = 0;
  double voiceSum = 0;
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t b = 0; b < blocks; ++b) {
    if (b % kRetriggerBlocks == 0)
      for (int i = 0; i < voices; ++i) e.noteOn(note(i), 100);
    std::fill(std::begin(left), std::end(left), 0.0f);
    std::fill(std::begin(right), std::end(right), 0.0f);
    e.render(left, right, ion::kBlockSize);
    sink = sink + left[0];
    voiceSum += e.voiceCount();
  }
  const std::chrono::duration<double> wall = std::chrono::steady_clock::now() - start;
  const double rendered = static_cast<double>(blocks * ion::kBlockSize) / ion::kSampleRate;
  const double periods = rendered * ion::kSampleRate / 128.0;
  return {rendered / wall.count(), wall.count() * 1e6 / periods, voiceSum / static_cast<double>(blocks)};
}

// Drum scene: `pads` pads with 1 s stereo noise samples, each hit every
// 62.5 ms (16ths at 240 BPM) so the per-pad voice cap is always full.
// `filter`/`drive` turn on the per-voice SVF and drive.
Result benchDrum(int pads, bool filter, bool drive, double seconds) {
  ion::Sample s;
  s.channels = 2;
  s.frames = ion::kSampleRate;
  s.data.assign(2 * static_cast<std::size_t>(s.frames + 2 * ion::Sample::kGuard), 0.0f);
  uint32_t seed = 1;
  for (float& v : s.data) v = static_cast<float>((seed = seed * 1664525u + 1013904223u) >> 8) / 16777216.0f - 0.5f;
  ion::DrumEngine e;
  for (int p = 0; p < pads; ++p) {
    e.setSample(p, &s);
    e.setParam(ion::DrumEngine::param(p, ion::DrumEngine::kTune), static_cast<float>(p % 7) - 3);  // interpolating, not copying
    if (filter) e.setParam(ion::DrumEngine::param(p, ion::DrumEngine::kFilter), ion::DrumEngine::kLowPass);
    if (drive) e.setParam(ion::DrumEngine::param(p, ion::DrumEngine::kDrive), 0.5f);
  }
  constexpr std::size_t kHitBlocks = ion::kSampleRate / 16 / ion::kBlockSize;
  const auto blocks = static_cast<std::size_t>(seconds * ion::kSampleRate / ion::kBlockSize);
  float left[ion::kBlockSize], right[ion::kBlockSize];
  volatile float sink = 0;
  double voiceSum = 0;
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t b = 0; b < blocks; ++b) {
    if (b % kHitBlocks == 0)
      for (int p = 0; p < pads; ++p) e.noteOn(static_cast<uint8_t>(ion::DrumEngine::kFirstNote + p), 100);
    std::fill(std::begin(left), std::end(left), 0.0f);
    std::fill(std::begin(right), std::end(right), 0.0f);
    e.render(left, right, ion::kBlockSize);
    sink = sink + left[0];
    voiceSum += e.voiceCount();
  }
  const std::chrono::duration<double> wall = std::chrono::steady_clock::now() - start;
  const double rendered = static_cast<double>(blocks * ion::kBlockSize) / ion::kSampleRate;
  const double periods = rendered * ion::kSampleRate / 128.0;
  return {rendered / wall.count(), wall.count() * 1e6 / periods, voiceSum / static_cast<double>(blocks)};
}

// One effect processing stereo noise, as an insert (or as a return when wetOnly).
double benchEffect(ion::Effect& fx, double seconds) {
  const auto blocks = static_cast<std::size_t>(seconds * ion::kSampleRate / ion::kBlockSize);
  float left[ion::kBlockSize], right[ion::kBlockSize];
  uint32_t seed = 1;
  volatile float sink = 0;
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t b = 0; b < blocks; ++b) {
    for (std::size_t i = 0; i < ion::kBlockSize; ++i) {
      left[i] = static_cast<float>((seed = seed * 1664525u + 1013904223u) >> 8) / 16777216.0f - 0.5f;
      right[i] = -left[i];
    }
    fx.process(left, right, ion::kBlockSize);
    sink = sink + left[0];
  }
  const std::chrono::duration<double> wall = std::chrono::steady_clock::now() - start;
  const double periods = static_cast<double>(blocks * ion::kBlockSize) / 128.0;
  return wall.count() * 1e6 / periods;
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

  std::printf("\nDrum (stereo samples, Hermite interpolation; 4 voices per pad)\n");
  std::printf("  pads  voice path      sounding   realtime×   µs per 128-frame buffer   %% of 2.67 ms   µs per voice\n");
  struct Scene {
    int pads;
    bool filter, drive;
    const char* label;
  };
  for (const Scene& sc : {Scene{4, false, false, "plain"}, Scene{16, false, false, "plain"}, Scene{16, true, false, "+ filter"},
                          Scene{16, true, true, "+ filter, drive"}}) {
    const Result r = benchDrum(sc.pads, sc.filter, sc.drive, seconds);
    std::printf("  %4d  %-15s %8.1f   %9.0f   %23.1f   %11.2f%%   %12.2f\n", sc.pads, sc.label, r.averageVoices, r.realtimeFactor,
                r.usPerPeriod128, r.usPerPeriod128 / 2666.7 * 100.0, r.usPerPeriod128 / std::max(1.0, r.averageVoices));
  }

  {
    // ACID: one voice playing 16ths with slides and accents; asleep it costs nothing.
    ion::AcidEngine e;
    const auto blocks = static_cast<std::size_t>(seconds * ion::kSampleRate / ion::kBlockSize);
    float left[ion::kBlockSize], right[ion::kBlockSize];
    volatile float sink = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t b = 0; b < blocks; ++b) {
      if (b % 94 == 0) {
        const auto n = static_cast<uint8_t>(36 + (b / 94) % 12);
        e.noteOn(n, (b / 94) % 4 == 0 ? 120 : 90);
        if ((b / 94) % 3) e.noteOff(static_cast<uint8_t>(n - 1));
      }
      std::fill(std::begin(left), std::end(left), 0.0f);
      std::fill(std::begin(right), std::end(right), 0.0f);
      e.render(left, right, ion::kBlockSize);
      sink = sink + left[0];
    }
    const std::chrono::duration<double> wall = std::chrono::steady_clock::now() - start;
    const double us = wall.count() * 1e6 / (static_cast<double>(blocks * ion::kBlockSize) / 128.0);
    std::printf("\nACID (Open303, one mono voice playing 16ths)\n  %.1f µs per 128-frame buffer (%.2f%%)\n", us, us / 2666.7 * 100.0);
  }

  {
    // CHIP: six voices of a 32-step pulse, filtered, at a crunched rate; and
    // the same with a table rebuilt every block (as when modulation moves its shape).
    auto chip = [&](bool rebuild) {
      ion::ChipEngine e;
      e.setParam(ion::ChipEngine::kFilter, ion::ChipEngine::kLowPass);
      e.setParam(ion::ChipEngine::kRate, 16000);
      for (uint8_t n : std::initializer_list<uint8_t>{48, 52, 55, 60, 64, 67}) e.noteOn(n, 100);
      const auto blocks = static_cast<std::size_t>(seconds * ion::kSampleRate / ion::kBlockSize);
      float left[ion::kBlockSize], right[ion::kBlockSize];
      volatile float sink = 0;
      const auto start = std::chrono::steady_clock::now();
      for (std::size_t b = 0; b < blocks; ++b) {
        if (rebuild) e.setParam(ion::ChipEngine::kWarp, static_cast<float>(b % 100) / 100.0f);
        std::fill(std::begin(left), std::end(left), 0.0f);
        std::fill(std::begin(right), std::end(right), 0.0f);
        e.render(left, right, ion::kBlockSize);
        sink = sink + left[0];
      }
      const std::chrono::duration<double> wall = std::chrono::steady_clock::now() - start;
      return wall.count() * 1e6 / (static_cast<double>(blocks * ion::kBlockSize) / 128.0);
    };
    const double plain = chip(false), moving = chip(true);
    std::printf("\nCHIP (6 voices, low-pass, 16 kHz rate)\n  %.1f µs per 128-frame buffer (%.2f%%); %.1f µs with the table rebuilt every block\n", plain,
                plain / 2666.7 * 100.0, moving);
  }

  std::printf("\nPlaits (6 voices, every model)\n");
  std::printf("  model             sounding   realtime×   µs per 128-frame buffer   %% of 2.67 ms   µs per voice\n");
  double worst = 0;
  for (int m = 0; m < ion::PlaitsEngine::kModels; ++m) {
    const Result r = benchPlaits(m, 6, seconds / 4);
    worst = std::max(worst, r.usPerPeriod128);
    std::printf("  %-16s  %8.1f   %9.0f   %23.1f   %11.2f%%   %12.2f\n", ion::PlaitsEngine::modelName(m), r.averageVoices,
                r.realtimeFactor, r.usPerPeriod128, r.usPerPeriod128 / 2666.7 * 100.0, r.usPerPeriod128 / std::max(1.0, r.averageVoices));
  }
  std::printf("  worst model: %.1f µs per buffer for 6 voices (%.1f%% of a 128-frame buffer)\n", worst, worst / 2666.7 * 100.0);

  // Modulation: the whole Module with no engines, without and then with all
  // 64 routes (8 per track) moving faders and pans, all four sources running.
  {
    auto moduleCost = [&](bool routes) {
      auto m = std::make_unique<ion::Module>();
      if (routes)
        for (int t = 0; t < ion::kTrackCount; ++t)
          for (int r = 0; r < ion::kModRoutes; ++r) {
            ion::Command c;
            c.type = ion::Command::Type::SetModTarget;
            c.track = static_cast<uint8_t>(t);
            c.slot = r;
            c.value = r % 2 ? ion::kPan : ion::kFader;
            m->pushCommand(c);
            c.type = ion::Command::Type::SetParam;
            c.param = ion::routeRef(t, r, ion::kRouteSource);
            c.value = static_cast<float>(1 + r % 4);
            m->pushCommand(c);
            c.param = ion::routeRef(t, r, ion::kRouteDepth);
            c.value = 0.2f;
            m->pushCommand(c);
          }
      std::vector<float> out(2 * 128);
      const int periods = static_cast<int>(seconds * ion::kSampleRate / 128);
      const auto start = std::chrono::steady_clock::now();
      for (int i = 0; i < periods; ++i) m->process(out.data(), 128);
      const std::chrono::duration<double> wall = std::chrono::steady_clock::now() - start;
      return wall.count() * 1e6 / periods;
    };
    const double base = moduleCost(false), withMod = moduleCost(true);
    std::printf("\nModulation (the Module, no engines; 8 tracks × 8 routes, 2 LFOs + 2 envelopes each)\n");
    std::printf("  without routes %.1f µs, with %.1f µs per 128-frame buffer: modulation costs %.1f µs (%.2f%%)\n", base, withMod,
                withMod - base, (withMod - base) / 2666.7 * 100.0);
  }

  std::printf("\nEffects (stereo noise in; the time includes making the noise)\n");
  std::printf("  effect                    µs per 128-frame buffer   %% of 2.67 ms\n");
  struct Fx {
    const char* label;
    ion::EffectType type;
    int param = -1;
    float value = 0;
  };
  for (const Fx& f : {Fx{"compressor", ion::EffectType::Compressor}, Fx{"chorus", ion::EffectType::Chorus},
                      Fx{"delay (1/8D, 40%)", ion::EffectType::Delay}, Fx{"reverb plate", ion::EffectType::Reverb, ion::Reverb::kType, 0},
                      Fx{"reverb hall", ion::EffectType::Reverb, ion::Reverb::kType, 1},
                      Fx{"reverb room", ion::EffectType::Reverb, ion::Reverb::kType, 2},
                      Fx{"drive (soft, 2× oversampled)", ion::EffectType::Drive}}) {
    auto fx = ion::createEffect(f.type);
    if (f.param >= 0) fx->setParam(f.param, f.value);
    const double us = benchEffect(*fx, seconds);
    std::printf("  %-24s  %23.1f   %11.2f%%\n", f.label, us, us / 2666.7 * 100.0);
  }
  {
    ion::Limiter lim;
    struct Wrap final : ion::Effect {
      ion::Limiter& l;
      explicit Wrap(ion::Limiter& x) : l(x) {}
      ion::EffectType type() const override { return ion::EffectType::None; }
      void setParam(int, float) override {}
      void process(float* a, float* b, std::size_t n) override { l.process(a, b, n); }
      void reset() override {}
    } w(lim);
    const double us = benchEffect(w, seconds);
    std::printf("  %-24s  %23.1f   %11.2f%%\n", "limiter (master)", us, us / 2666.7 * 100.0);
  }
  return 0;
}
