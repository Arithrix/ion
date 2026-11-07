// make-demo: builds the demo project for a release from a DX7 bank on this
// machine (the factory patches can't live in the repo). Used by scripts/release.sh.
//
//   make-demo --syx ~/ion/downloads/dx7/x/sysex/rom1a.syx --projects ~/ion/projects --name "demo 0.1.0"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>

#include "audio/wav.h"
#include "engine/drum/drum_engine.h"
#include "engine/fm/fm_engine.h"
#include "engine/fx/effects.h"
#include "engine/module.h"
#include "library/syx.h"
#include "project/config.h"
#include "project/paths.h"
#include "project/session.h"

using namespace ion;

namespace {

// The demo kit, synthesised so it can ship with a release: one-shots built
// from swept sines and filtered noise.
struct Drum {
  const char* file;
  double seconds;
  double (*sound)(double t, double seconds, uint32_t& seed);
};

double noise(uint32_t& seed) { return static_cast<double>((seed = seed * 1664525u + 1013904223u) >> 8) / 8388608.0 - 1.0; }
double sweep(double t, double from, double to, double rate) {
  // Phase of a sine whose frequency falls exponentially from `from` to `to`.
  const double k = rate;
  return 2 * 3.14159265358979 * (to * t + (from - to) * (1 - std::exp(-k * t)) / k);
}

const Drum kKit[] = {
    {"kick.wav", 0.45, [](double t, double, uint32_t&) { return std::sin(sweep(t, 160, 48, 30)) * std::exp(-t * 7); }},
    {"snare.wav", 0.25, [](double t, double, uint32_t& s) {
       return 0.5 * std::sin(sweep(t, 260, 180, 40)) * std::exp(-t * 25) + 0.6 * noise(s) * std::exp(-t * 16);
     }},
    {"clap.wav", 0.35, [](double t, double, uint32_t& s) {
       const double bursts = (t < 0.01 || (t > 0.012 && t < 0.02) || (t > 0.024 && t < 0.032)) ? 1.0 : 0.0;
       return noise(s) * (0.8 * bursts + 0.5 * std::exp(-(t - 0.03) * 14) * (t > 0.03));
     }},
    {"rim.wav", 0.08, [](double t, double, uint32_t&) { return std::sin(2 * 3.14159265 * 820 * t) * std::exp(-t * 60); }},
    {"hat closed.wav", 0.07, [](double t, double, uint32_t& s) {
       static double prev = 0;
       const double n = noise(s), hp = n - prev;  // a crude high-pass: hats are bright
       prev = n;
       return 0.5 * hp * std::exp(-t * 60);
     }},
    {"hat open.wav", 0.6, [](double t, double, uint32_t& s) {
       static double prev = 0;
       const double n = noise(s), hp = n - prev;
       prev = n;
       return 0.4 * hp * std::exp(-t * 6);
     }},
    {"tom.wav", 0.4, [](double t, double, uint32_t&) { return std::sin(sweep(t, 140, 95, 12)) * std::exp(-t * 9); }},
    {"shaker.wav", 0.12, [](double t, double, uint32_t& s) {
       return 0.35 * noise(s) * std::sin(3.14159265 * t / 0.12) * std::sin(3.14159265 * t / 0.12);
     }},
};

bool writeKit(const std::filesystem::path& dir, std::string& error) {
  std::filesystem::create_directories(dir);
  uint32_t seed = 7;
  for (const Drum& d : kKit) {
    wav::Audio a;
    a.channels = 1;
    a.sampleRate = 48000;
    const auto n = static_cast<std::size_t>(d.seconds * 48000);
    a.samples.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / 48000.0;
      const double fadeOut = std::min(1.0, static_cast<double>(n - i) / 96.0);  // no click at the end
      a.samples[i] = static_cast<float>(0.8 * d.sound(t, d.seconds, seed) * fadeOut);
    }
    if (!wav::write(dir / d.file, a, error)) return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string syxPath, projects, name;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string a = argv[i];
    if (a == "--syx") syxPath = argv[i + 1];
    else if (a == "--projects") projects = argv[i + 1];
    else if (a == "--name") name = argv[i + 1];
  }
  if (syxPath.empty() || projects.empty() || name.empty()) {
    std::fputs("usage: make-demo --syx ROM1A.syx --projects DIR --name NAME\n", stderr);
    return 2;
  }
  std::ifstream f(syxPath, std::ios::binary);
  const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
  const auto parsed = syx::parse(bytes);
  if (parsed.dumps.empty()) {
    std::fprintf(stderr, "make-demo: no DX7 bank in %s\n", syxPath.c_str());
    return 1;
  }
  const auto bank = syx::voices(parsed.dumps.front());
  if (bank.size() < 32) {
    std::fprintf(stderr, "make-demo: %s isn't a 32-voice bank\n", syxPath.c_str());
    return 1;
  }

  auto module = std::make_unique<Module>();
  Config config;
  config.autosaveMinutes = 0;
  Paths paths = Paths::defaults();
  paths.projectsDir = projects;
  Session s(*module, paths, config);
  s.newProject(name);

  auto fm = [&](int track, int voice, int channel) {
    s.setEngine(track, EngineType::Fm);
    s.setVoice(track, bank[static_cast<std::size_t>(voice)], "rom1a");
    s.setChannel(track, channel);
  };
  auto set = [&](int track, MixerParam p, float v) { s.setParam({static_cast<uint8_t>(track), p}, v); };

  // ROM1A order: 1 BRASS 1 … 4 STRINGS 1 … 11 E.PIANO 1 … 15 BASS 1.
  fm(0, 10, 0);  // E.PIANO 1 on ch 1
  set(0, kFader, -4.0f);
  set(0, kPan, -0.15f);
  set(0, kHpOn, 1.0f);
  set(0, kHpFreq, 80.0f);
  set(0, kB1Freq, 2500.0f);
  set(0, kB1Gain, 2.5f);
  set(0, kB1Q, 0.8f);

  fm(1, 14, 1);  // BASS 1 on ch 2
  set(1, kFader, -2.0f);
  set(1, kLsFreq, 90.0f);
  set(1, kLsGain, 3.0f);
  set(1, kLpOn, 1.0f);
  set(1, kLpFreq, 6000.0f);

  fm(2, 0, 2);  // BRASS 1 on ch 3
  set(2, kFader, -8.0f);
  set(2, kPan, 0.25f);

  fm(3, 3, 0);  // STRINGS 1 layered under the piano, also ch 1
  set(3, kFader, -14.0f);
  set(3, kHpOn, 1.0f);
  set(3, kHpFreq, 300.0f);

  // Macros: the bass a little darker with a shorter release, so "edited"
  // shows in the browser and save-as has something to bake.
  auto macro = [&](int track, int p, float v) {
    s.setParam({static_cast<uint8_t>(track), static_cast<uint16_t>(kEngineParamBase + p)}, v);
  };
  macro(1, FmEngine::kBright, -6.0f);
  macro(1, FmEngine::kRelease, -10.0f);

  // Mappings to try from the OXI: cc 74 on channel 1 is the piano's Bright
  // macro, cc 71 its Release; cc 7 on channel 2 rides the bass fader (kept audible).
  auto fmRef = [](int track, int p) { return ParamRef{static_cast<uint8_t>(track), static_cast<uint16_t>(kEngineParamBase + p)}; };
  s.addMapping(0, 74, fmRef(0, FmEngine::kBright));
  s.addMapping(0, 71, fmRef(0, FmEngine::kRelease));
  s.addMapping(1, 7, {1, kFader}, 0.4f, 1.0f);

  // A program list: program change 1-8 on a channel loads these onto the FM
  // tracks listening there.
  for (int v : {10, 14, 0, 3, 7, 20, 18, 25}) s.toggleProgram(bank[static_cast<std::size_t>(v)], "rom1a");

  // Track 5: the demo kit on ch 5, its samples inside the project (as
  // "collect" would leave them), hats in choke group A.
  std::string error;
  if (!writeKit(paths.projectDir(name) / "samples", error)) {
    std::fprintf(stderr, "make-demo: %s\n", error.c_str());
    return 1;
  }
  s.setEngine(4, EngineType::Drum);
  for (int pad = 0; pad < static_cast<int>(std::size(kKit)); ++pad)
    s.setPadSample(4, pad, std::string("samples/") + kKit[pad].file);
  auto drum = [&](int pad, int p, float v) { s.setParam(engineParamRef(4, DrumEngine::param(pad, p)), v); };
  drum(4, DrumEngine::kChoke, 1);
  drum(5, DrumEngine::kChoke, 1);
  drum(4, DrumEngine::kPan, 0.2f);
  drum(5, DrumEngine::kPan, 0.2f);
  drum(7, DrumEngine::kPan, -0.3f);
  drum(1, DrumEngine::kFilter, DrumEngine::kHighPass);  // a thinner snare
  drum(1, DrumEngine::kCutoff, 220);
  drum(6, DrumEngine::kTune, -3);
  set(4, kFader, -3.0f);
  // cc 74 on channel 5: the open hat's decay.
  s.addMapping(4, 74, engineParamRef(4, DrumEngine::param(5, DrumEngine::kDecay)));

  // Effects (Phase 4): chorus on the piano, a compressor on the bass, the
  // drums gently compressed; piano and drums in the reverb, the drums and
  // brass in the delay. The master's glue compressor stays out until wanted.
  s.setInsert(0, 0, EffectType::Chorus);
  s.setParam(insertParamRef(0, 0, Chorus::kMix), 0.35f);
  s.setInsert(1, 0, EffectType::Compressor);
  s.setParam(insertParamRef(1, 0, Compressor::kThreshold), -20);
  s.setParam(insertParamRef(1, 0, Compressor::kRatio), 4);
  s.setParam(insertParamRef(1, 0, Compressor::kMakeup), 3);
  s.setInsert(4, 0, EffectType::Compressor);
  s.setParam(insertParamRef(4, 0, Compressor::kThreshold), -14);
  s.setParam(insertParamRef(4, 0, Compressor::kAttack), 20);
  s.setParam(insertParamRef(4, 0, Compressor::kRelease), 120);
  s.setParam({0, kSendB}, -10.0f);
  s.setParam({2, kSendA}, -14.0f);
  s.setParam({2, kSendB}, -12.0f);
  s.setParam({4, kSendA}, -18.0f);
  s.setParam({4, kSendB}, -16.0f);
  s.setParam({kReverbScope, kFader}, -3.0f);
  s.setParam({kDelayScope, kFader}, -4.0f);
  s.setParam({kDelayScope, kSendB}, -12.0f);  // echoes into the reverb, a little
  // cc 75 on channel 1: the reverb's size; cc 76: the delay's feedback.
  s.addMapping(0, 75, insertParamRef(kReverbScope, 0, Reverb::kSize));
  s.addMapping(0, 76, insertParamRef(kDelayScope, 0, Delay::kFeedback));

  if (!s.save(error)) {
    std::fprintf(stderr, "make-demo: %s\n", error.c_str());
    return 1;
  }
  std::printf("make-demo: wrote %s (%zu cc mappings, %zu programs)\n", paths.projectFile(name).c_str(), s.ccMappings().size(),
              s.programs().size());
  return 0;
}
