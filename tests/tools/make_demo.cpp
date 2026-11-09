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
#include "engine/acid/acid_engine.h"
#include "engine/chip/chip_engine.h"
#include "engine/drum/drum_engine.h"
#include "engine/fm/fm_engine.h"
#include "engine/fx/effects.h"
#include "engine/plaits/plaits_engine.h"
#include "engine/sampler/sampler_engine.h"
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

  // Track 2 (ch 2): ACID (0.9.0), squelchy, through a Drive insert. cc 74
  // on ch 2 is the cutoff, cc 71 the resonance; LFO 1 (synced, 1 bar) moves
  // the cutoff a little too.
  s.setEngine(1, EngineType::Acid);
  s.setChannel(1, 1);
  {
    auto ac = [&](int p, float v) { s.setParam(engineParamRef(1, p), v); };
    ac(AcidEngine::kCutoff, 650);
    ac(AcidEngine::kResonance, 0.82f);
    ac(AcidEngine::kEnvMod, 0.65f);
    ac(AcidEngine::kDecay, 380);
    ac(AcidEngine::kAccent, 0.7f);
    s.setInsert(1, 0, EffectType::Drive);
    s.setParam(insertParamRef(1, 0, Drive::kDrive), 14);
    s.setParam(insertParamRef(1, 0, Drive::kTone), 6000);
    s.addMapping(1, 74, engineParamRef(1, AcidEngine::kCutoff));
    s.addMapping(1, 71, engineParamRef(1, AcidEngine::kResonance));
    s.setModTarget(1, 0, engineParamRef(1, AcidEngine::kCutoff));
    s.setParam(routeRef(1, 0, kRouteSource), kSrcLfo1);
    s.setParam(routeRef(1, 0, kRouteDepth), 0.12f);
    s.setParam(lfoRef(1, 0, kLfoSync), 1);
    s.setParam(lfoRef(1, 0, kLfoDivision), 2);  // 1 bar
  }
  set(1, kFader, -4.0f);

  // Track 3 (ch 3): CHIP (0.10.0), a 25% pulse with a little warp, 4-bit, at
  // 22 kHz, with envelope 1 on tune for a blip of pitch at each note.
  s.setEngine(2, EngineType::Chip);
  s.setChannel(2, 2);
  {
    auto cp = [&](int p, float v) { s.setParam(engineParamRef(2, p), v); };
    cp(ChipEngine::kShape, ChipEngine::kPulse25);
    cp(ChipEngine::kSize, 32);
    cp(ChipEngine::kWarp, 0.25f);
    cp(ChipEngine::kBits, 4);
    cp(ChipEngine::kRate, 22050);
    cp(ChipEngine::kAmpDecay, 300);
    cp(ChipEngine::kAmpSustain, 0.6f);
    s.setModTarget(2, 0, engineParamRef(2, ChipEngine::kTune));
    s.setParam(routeRef(2, 0, kRouteSource), kSrcEnv1);
    s.setParam(routeRef(2, 0, kRouteDepth), 0.25f);  // a quarter of ±24 semitones: up an octave, falling back
    s.setParam(modEnvRef(2, 0, kMEnvDecay), 60);
  }
  set(2, kFader, -10.0f);
  set(2, kPan, 0.25f);

  fm(3, 3, 0);  // STRINGS 1 layered under the piano, also ch 1
  set(3, kFader, -14.0f);
  set(3, kHpOn, 1.0f);
  set(3, kHpFreq, 300.0f);

  // The bass a little darker (a macro, so "edited" shows in the browser and
  // save-as has something to bake) and tighter (the amp envelope's release).
  auto macro = [&](int track, int p, float v) {
    s.setParam({static_cast<uint8_t>(track), static_cast<uint16_t>(kEngineParamBase + p)}, v);
  };
  macro(1, FmEngine::kBright, -6.0f);
  macro(1, FmEngine::kAmpRelease, 150.0f);  // the uniform amp envelope: a tighter bass

  // Mappings to try from the OXI: cc 74 on channel 1 is the piano's Bright
  // macro, cc 71 its amp release; cc 7 on channel 2 rides the bass fader (kept audible).
  auto fmRef = [](int track, int p) { return ParamRef{static_cast<uint8_t>(track), static_cast<uint16_t>(kEngineParamBase + p)}; };
  s.addMapping(0, 74, fmRef(0, FmEngine::kBright));
  s.addMapping(0, 71, fmRef(0, FmEngine::kAmpRelease));
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
  // Track 6 (ch 6): a sampler with a 4-bar chord loop at 120 BPM, synthesised
  // and written with its sidecar, so it shows its bars. Looped, root C3.
  {
    wav::Audio loop;
    loop.channels = 2;
    const int frames = 4 * 4 * 24000;  // 4 bars of 4 beats at 120 BPM
    loop.samples.assign(2 * static_cast<std::size_t>(frames), 0.0f);
    const double chords[4][3] = {{220.0, 261.63, 329.63}, {174.61, 220.0, 261.63}, {196.0, 246.94, 293.66}, {164.81, 207.65, 246.94}};
    for (int i = 0; i < frames; ++i) {
      const double t = i / 48000.0;
      const int bar = std::min(3, i / (4 * 24000));
      const double inBar = std::fmod(t, 2.0);
      const double env = std::min(1.0, inBar / 0.02) * (0.55 + 0.45 * std::exp(-inBar * 1.5));
      double v = 0;
      for (int k = 0; k < 3; ++k) v += std::sin(2 * 3.14159265358979 * chords[bar][k] * t) + 0.3 * std::sin(4 * 3.14159265358979 * chords[bar][k] * t);
      const float x = static_cast<float>(0.16 * env * v);
      loop.samples[2 * static_cast<std::size_t>(i)] = x;
      loop.samples[2 * static_cast<std::size_t>(i) + 1] = x;
    }
    const auto dir = paths.projectDir(name) / "samples";
    if (!wav::write(dir / "chords 120bpm 4 bars.wav", loop, error) ||
        !writeFileAtomic(dir / "chords 120bpm 4 bars.json",
                         R"({"ion_recording": 1, "tempo": 120.0, "bars": 4, "beats_per_bar": 4})" "\n", error)) {
      std::fprintf(stderr, "make-demo: %s\n", error.c_str());
      return 1;
    }
    s.setEngine(5, EngineType::Sampler);
    s.setPadSample(5, 0, "samples/chords 120bpm 4 bars.wav");
    s.setParam(engineParamRef(5, SamplerEngine::kLoop), SamplerEngine::kLoopForward);
    s.setParam(engineParamRef(5, SamplerEngine::kAmpAttack), 5);
    s.setParam(engineParamRef(5, SamplerEngine::kAmpRelease), 400);
    s.setParam(engineParamRef(5, SamplerEngine::kFilter), SamplerEngine::kLp24);
    s.setParam(engineParamRef(5, SamplerEngine::kCutoff), 5000);
    s.setParam(engineParamRef(5, SamplerEngine::kLfoSync), 1);
    s.setParam(engineParamRef(5, SamplerEngine::kLfoDivision), 2);  // a bar
    s.setParam(engineParamRef(5, SamplerEngine::kLfoCutoff), 0.25f);
    s.setParam({5, kSendB}, -12.0f);
    s.addMapping(5, 74, engineParamRef(5, SamplerEngine::kCutoff));
  }

  // Track 7 (ch 7): the chord loop resampled through the real recorder (Phase 6):
  // four bars of track 6 from a downbeat of a pretend 120 BPM clock, sliced on
  // its chord changes onto pads 1-4 (C1-D#1), choke group A.
  {
    s.setEngine(6, EngineType::Drum);
    if (!s.waitForSamples()) {
      std::fputs("make-demo: the chord loop didn't load\n", stderr);
      return 1;
    }
    RecSetup& rs = s.recSetup();
    rs.source = RecSource::Tracks;
    rs.tracks = 1 << 5;
    rs.bars = 4;
    rs.synced = true;
    rs.dest = RecSetup::Dest::Drum;
    rs.destTrack = 6;
    rs.firstPad = 0;
    rs.slice = SliceMode::Chords;
    rs.muteSources = false;
    if (!s.arm(error)) {
      std::fprintf(stderr, "make-demo: %s\n", error.c_str());
      return 1;
    }
    std::vector<float> buf(2 * 256);
    int64_t frame = 0, tick = 0;
    auto run = [&](int64_t frames) {
      for (const int64_t end = frame + frames; frame < end; frame += 256) {
        for (; tick < frame + 256; tick += 1000) module->pushMidi({{0xF8}, MidiEvent::Timing::Frame, tick});
        module->process(buf.data(), 256);
        s.tick();
      }
    };
    run(10000);
    module->pushMidi({{0xFA}, MidiEvent::Timing::Frame, tick});
    module->pushMidi({{0x95, 60, 110}, MidiEvent::Timing::Frame, tick});
    run(4 * 96000 + 8192);
    module->pushMidi({{0x85, 60, 0}, MidiEvent::Timing::Frame, frame});
    run(4096);
    if (s.recState() != Session::RecState::Review || !s.commitSlices(error) || !s.waitForSamples()) {
      std::fprintf(stderr, "make-demo: the demo take didn't make slices: %s\n", error.empty() ? s.takeMessage().c_str() : error.c_str());
      return 1;
    }
    s.tick();
    std::printf("make-demo: %s\n", s.takeMessage().c_str());
    set(6, kFader, -4.0f);
    // Ready for a take of your own: the piano and strings (both on ch 1),
    // four bars from the next downbeat, sliced onto pads 5-8 of track 7.
    rs.tracks = 1 << 0 | 1 << 3;
    rs.firstPad = 4;
    rs.muteSources = true;
  }

  // Track 8 (ch 8): Plaits on String machine, the mockup's patch: harmonics
  // on a minor 7th, cc 20-22 on the three macros, into the reverb. Its 6-op
  // list is ROM1A, so "6-op FM a" plays the DX7's own first bank.
  {
    s.setEngine(7, EngineType::Plaits);
    s.setChannel(7, 7);
    auto pl = [&](int p, float v) { s.setParam(engineParamRef(7, p), v); };
    pl(PlaitsEngine::kModel, 22);
    pl(PlaitsEngine::kHarmonics, 0.38f);
    pl(PlaitsEngine::kTimbre, 0.68f);
    pl(PlaitsEngine::kMorph, 0.40f);
    pl(PlaitsEngine::kLpgDecay, 0.62f);
    pl(PlaitsEngine::kAux, 0.3f);
    set(7, kFader, -8.0f);
    set(7, kSendB, -10.0f);
    s.setPlaitsPatches(7, std::vector<syx::Voice>(bank.begin(), bank.begin() + 32));
    s.addMapping(7, 20, engineParamRef(7, PlaitsEngine::kHarmonics));
    s.addMapping(7, 21, engineParamRef(7, PlaitsEngine::kTimbre));
    s.addMapping(7, 22, engineParamRef(7, PlaitsEngine::kMorph));
    // Modulation (0.8.0): LFO 1, a triangle synced to two bars, sweeps timbre;
    // envelope 1 opens morph on every note and settles back.
    s.setModTarget(7, 0, engineParamRef(7, PlaitsEngine::kTimbre));
    s.setParam(routeRef(7, 0, kRouteSource), kSrcLfo1);
    s.setParam(routeRef(7, 0, kRouteDepth), 0.25f);
    s.setParam(lfoRef(7, 0, kLfoShape), kLfoTriangle);
    s.setParam(lfoRef(7, 0, kLfoSync), 1);
    s.setParam(lfoRef(7, 0, kLfoDivision), 1);  // 2 bars
    s.setModTarget(7, 1, engineParamRef(7, PlaitsEngine::kMorph));
    s.setParam(routeRef(7, 1, kRouteSource), kSrcEnv1);
    s.setParam(routeRef(7, 1, kRouteDepth), 0.35f);
    s.setParam(modEnvRef(7, 0, kMEnvAttack), 30);
    s.setParam(modEnvRef(7, 0, kMEnvDecay), 1500);
  }

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
