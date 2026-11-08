// The uniform envelope (DECISIONS D-50): one component, the same everywhere.

#include <doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "dsp/ahdsr.h"
#include "engine/drum/drum_engine.h"
#include "engine/envelope_params.h"
#include "engine/fm/fm_engine.h"
#include "engine/module.h"
#include "engine/sampler/sampler_engine.h"
#include "project/config.h"
#include "project/session.h"

using namespace ion;
namespace fs = std::filesystem;

namespace {

std::vector<float> runEnv(dsp::Ahdsr& e, const dsp::Ahdsr::Coeffs& c, int n) {
  std::vector<float> out(static_cast<std::size_t>(n));
  for (float& v : out) v = e.next(c);
  return out;
}

float rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
  double s = 0;
  for (std::size_t i = from; i < to; ++i) s += x[i] * x[i];
  return static_cast<float>(std::sqrt(s / static_cast<double>(to - from)));
}

}  // namespace

TEST_CASE("envelope: attack, hold, decay to sustain, release; full and off") {
  dsp::Ahdsr e;
  auto c = dsp::Ahdsr::make(10, 5, false, 100, 0.5f, 200, false);
  e.start();
  auto out = runEnv(e, c, 48000);
  CHECK(out[239] == doctest::Approx(0.5f).epsilon(0.01));    // half way up at 5 ms
  CHECK(out[480] == doctest::Approx(1.0f));                   // the top at 10 ms…
  CHECK(out[480 + 230] == doctest::Approx(1.0f));             // …held for 5 ms
  CHECK(out[480 + 240 + 4800] == doctest::Approx(0.5f + 0.5f * 0.001f).epsilon(0.001));  // −60 dB of the way to sustain
  CHECK(e.stage() == dsp::Ahdsr::Stage::Sustain);
  e.release();
  out = runEnv(e, c, 20000);
  CHECK(out[9600] == doctest::Approx(0.5f * 0.001f).epsilon(0.05));  // −60 dB after the release time
  CHECK(e.idle());

  // Hold "full" stays at the top until released; release "off" never fades.
  dsp::Ahdsr f;
  const auto full = dsp::Ahdsr::make(0, 0, true, 10, 0, 1, true);
  f.start();
  CHECK(runEnv(f, full, 48000).back() == 1.0f);
  f.release();
  CHECK(runEnv(f, full, 48000).back() == 1.0f);
  CHECK_FALSE(f.idle());

  // Sustain 0 ends the note after the decay, with no release needed (drums).
  dsp::Ahdsr g;
  g.start();
  runEnv(g, dsp::Ahdsr::make(0, 0, false, 20, 0, 100, false), 4800);
  CHECK(g.idle());
}

TEST_CASE("envelope: every engine has the same envelope parameters; only the defaults differ") {
  auto find = [](std::span<const ParamInfo> table, const std::string& key) -> const ParamInfo* {
    for (const ParamInfo& p : table)
      if (key == p.key) return &p;
    return nullptr;
  };
  const char* keys[] = {"amp.attack", "amp.hold", "amp.decay", "amp.sustain", "amp.release", "vel.amp"};
  for (const char* key : keys) {
    CAPTURE(key);
    const ParamInfo* fm = find(FmEngine::paramTable(), key);
    const ParamInfo* smp = find(SamplerEngine::paramTable(), key);
    const ParamInfo* drum = find(DrumEngine::padParamTable(), key);
    REQUIRE(fm);
    REQUIRE(smp);
    REQUIRE(drum);
    for (const ParamInfo* p : {smp, drum}) {
      CHECK(p->min == fm->min);
      CHECK(p->max == fm->max);
      CHECK(p->curve == fm->curve);
      CHECK(p->unit == fm->unit);
      CHECK(std::string(p->label) == fm->label);
    }
  }
  // FM is wide open by default, so a patch sounds as stored.
  CHECK(FmEngine::paramTable()[FmEngine::kAmpSustain].def == 1.0f);
  CHECK(FmEngine::paramTable()[FmEngine::kAmpRelease].def == kEnvOff);
  CHECK(FmEngine::paramTable()[FmEngine::kVelAmp].def == 0.0f);
}

namespace {

// A held organ-like FM voice (all carriers, slow patch release).
syx::Voice organ() {
  syx::Voice v = syx::initVoice();
  for (int op = 0; op < 6; ++op) {
    uint8_t* o = v.data() + op * 21;
    o[0] = 99; o[1] = 99; o[2] = 99; o[3] = 30;  // slow patch release (R4 = 30)
    o[4] = 99; o[5] = 99; o[6] = 99; o[7] = 0;
    o[16] = op == 5 ? 99 : 0;  // op 1 only (the last block)
  }
  v[134] = 31;  // algorithm 32: all carriers
  return v;
}

std::vector<float> play(FmEngine& fm, int blocks, int noteOffAt) {
  std::vector<float> out;
  float l[64], r[64];
  for (int b = 0; b < blocks; ++b) {
    if (b == 0) fm.noteOn(60, 100);
    if (b == noteOffAt) fm.noteOff(60);
    std::fill(l, l + 64, 0.0f);
    std::fill(r, r + 64, 0.0f);
    fm.render(l, r, 64);
    out.insert(out.end(), l, l + 64);
  }
  return out;
}

}  // namespace

TEST_CASE("envelope on FM: open by default; a short release ends the note early; attack ramps") {
  FmEngine open;
  open.loadVoice(organ());
  const auto natural = play(open, 1500, 300);
  CHECK(rms(natural, 300 * 64 + 9600, 300 * 64 + 14400) > 0.01f);  // the patch's own slow release is still going

  FmEngine shortRelease;
  shortRelease.loadVoice(organ());
  shortRelease.setParam(FmEngine::kAmpRelease, 50);
  const auto cut = play(shortRelease, 1500, 300);
  CHECK(rms(cut, 0, 300 * 64) == doctest::Approx(rms(natural, 0, 300 * 64)).epsilon(0.01));  // the same while held
  CHECK(rms(cut, 300 * 64 + 4800, 300 * 64 + 9600) < 1e-4f);  // gone 100 ms after note-off
  CHECK(shortRelease.voiceCount() == 0);                         // and the voice is free

  FmEngine slow;
  slow.loadVoice(organ());
  slow.setParam(FmEngine::kAmpAttack, 500);
  const auto swell = play(slow, 600, 590);
  CHECK(rms(swell, 0, 2400) < rms(natural, 0, 2400) * 0.2f);   // quiet at the start
  CHECK(rms(swell, 24000, 28800) == doctest::Approx(rms(natural, 24000, 28800)).epsilon(0.05));  // full after 0.5 s
}

TEST_CASE("envelope: drum pads and kits from 0.3/0.4 convert exactly") {
  const fs::path root = fs::temp_directory_path() / ("ion-env-" + std::to_string(std::random_device{}()));
  fs::create_directories(root);
  const std::string old = R"({"format": 4, "tracks": [{"track": 1, "engine": "drum", "channel": 1,
      "drum": {"kit": "", "pads": [], "params": {
        "pad1.mode": 1.0, "pad1.decay": 80.0, "pad1.hold": 300.0, "pad1.vel.level": 0.3,
        "pad2.mode": 0.0, "pad2.decay": 250.0, "pad2.hold": 5000.0, "pad2.vel.level": 0.9,
        "pad3.mode": 0.0, "pad3.decay": 120.0, "pad3.hold": 40.0}}}],
      "cc": [{"cc": 20, "channel": 1, "target": "track1.drum.pad2.vel.level", "lo": 0, "hi": 1},
             {"cc": 21, "channel": 1, "target": "track1.fm.attack", "lo": 0, "hi": 1}],
      "programs": []})";
  auto module = std::make_unique<Module>();
  Config config;
  Session s(*module, Paths::under(root), config);
  std::string error;
  REQUIRE(s.fromJson(old, error));
  auto pad = [&](int p, int q) { return s.param(engineParamRef(0, DrumEngine::param(p, q))); };
  // Gate: held while the key's down, then the old decay as its release.
  CHECK(pad(0, DrumEngine::kMode) == 1.0f);
  CHECK(pad(0, DrumEngine::kHold) == kEnvFull);
  CHECK(pad(0, DrumEngine::kRelease) == 80.0f);
  CHECK(pad(0, DrumEngine::kVelAmp) == doctest::Approx(0.3f));
  // One-shot: hold then decay, as before; the old "full" hold is still full.
  CHECK(pad(1, DrumEngine::kHold) == kEnvFull);
  CHECK(pad(1, DrumEngine::kDecay) == 250.0f);
  CHECK(pad(1, DrumEngine::kSustain) == 0.0f);
  CHECK(pad(2, DrumEngine::kHold) == 40.0f);
  CHECK(pad(2, DrumEngine::kVelAmp) == 0.5f);  // not set: the default
  // The CC on vel.level follows it; the one on FM's old Attack macro goes.
  const auto maps = s.ccMappings();
  REQUIRE(maps.size() == 1);
  CHECK(s.paramKey(maps[0].mapping.target) == "track1.drum.pad2.vel.amp");
  CHECK(s.paramKey(engineParamRef(0, DrumEngine::param(1, DrumEngine::kDecay))) == "track1.drum.pad2.amp.decay");

  // A version-1 kit.
  fs::create_directories(root / "kits");
  std::ofstream(root / "kits" / "old.json") << R"({"ion_kit": 1, "name": "old", "pads": [
      {"pad": 1, "sample": "", "params": {"mode": 1.0, "decay": 90.0, "hold": 5000.0, "vel.level": 0.2}}]})";
  REQUIRE(s.loadKit(0, root / "kits" / "old.json", error));
  CHECK(pad(0, DrumEngine::kRelease) == 90.0f);
  CHECK(pad(0, DrumEngine::kHold) == kEnvFull);
  CHECK(pad(0, DrumEngine::kVelAmp) == doctest::Approx(0.2f));
  fs::remove_all(root);
}
