// Sampler (HANDOFF §6.2).

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <numbers>
#include <random>
#include <thread>
#include <vector>

#include "audio/wav.h"
#include "engine/module.h"
#include "engine/sampler/sampler_engine.h"
#include "project/config.h"
#include "project/session.h"

using namespace ion;
namespace fs = std::filesystem;

namespace {

std::shared_ptr<Sample> makeSample(const std::vector<float>& x) {
  auto s = std::make_shared<Sample>();
  s->channels = 1;
  s->frames = static_cast<int64_t>(x.size());
  s->data.assign(x.size() + 2 * Sample::kGuard, 0.0f);
  std::copy(x.begin(), x.end(), s->data.begin() + Sample::kGuard);
  return s;
}

std::vector<float> sine(double hz, int frames, float amp = 0.5f) {
  std::vector<float> x(static_cast<std::size_t>(frames));
  for (int i = 0; i < frames; ++i) x[static_cast<std::size_t>(i)] = amp * static_cast<float>(std::sin(2 * std::numbers::pi * hz * i / 48000.0));
  return x;
}

std::vector<float> render(SamplerEngine& e, int frames) {
  std::vector<float> out;
  float l[64], r[64];
  for (int done = 0; done < frames; done += 64) {
    std::fill(l, l + 64, 0.0f);
    std::fill(r, r + 64, 0.0f);
    e.render(l, r, 64);
    out.insert(out.end(), l, l + 64);
  }
  out.resize(static_cast<std::size_t>(frames));
  return out;
}

int crossings(const std::vector<float>& x, std::size_t from, std::size_t to) {
  int c = 0;
  for (std::size_t i = from + 1; i < to; ++i) c += (x[i - 1] < 0) != (x[i] < 0);
  return c;
}

float peak(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
  float p = 0;
  for (std::size_t i = from; i < std::min(to, x.size()); ++i) p = std::max(p, std::fabs(x[i]));
  return p;
}

float maxStep(const std::vector<float>& x, std::size_t from = 1, std::size_t to = SIZE_MAX) {
  float m = 0;
  for (std::size_t i = std::max<std::size_t>(from, 1); i < std::min(to, x.size()); ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
  return m;
}

// An engine with `x` loaded, quick envelopes and no velocity response.
struct Rig {
  std::shared_ptr<Sample> s;
  SamplerEngine e;
  explicit Rig(const std::vector<float>& x) : s(makeSample(x)) {
    e.setSample(0, s.get());
    e.setParam(SamplerEngine::kAmpAttack, 0.5f);
    e.setParam(SamplerEngine::kVelAmp, 0);
  }
};

}  // namespace

TEST_CASE("sampler: plays chromatically from the root, inside its key range") {
  Rig r(sine(1000, 24000));
  r.e.noteOn(60, 127);  // the root, C3: as recorded
  auto out = render(r.e, 12000);
  CHECK(std::abs(crossings(out, 2000, 10000) - 333) <= 2);  // 1 kHz
  r.e.allNotesOff();
  r.e.noteOn(72, 127);  // an octave up: 2 kHz, half as long
  out = render(r.e, 24000);
  CHECK(std::abs(crossings(out, 2000, 10000) - 667) <= 3);
  CHECK(peak(out, 12100) == 0.0f);
  r.e.allNotesOff();
  r.e.noteOn(67, 127);  // a fifth up
  out = render(r.e, 12000);
  CHECK(crossings(out, 2000, 10000) == doctest::Approx(333 * std::exp2(7.0 / 12)).epsilon(0.01));

  r.e.allNotesOff();
  r.e.setParam(SamplerEngine::kLowKey, 48);
  r.e.setParam(SamplerEngine::kHighKey, 72);
  r.e.noteOn(47, 127);
  r.e.noteOn(73, 127);
  CHECK(peak(render(r.e, 512)) == 0.0f);
  CHECK(r.e.voiceCount() == 0);
}

TEST_CASE("sampler: forward loop sustains while held; release ends it; ping-pong turns round") {
  Rig r(sine(500, 4800));
  r.e.setParam(SamplerEngine::kLoop, SamplerEngine::kLoopForward);
  r.e.setParam(SamplerEngine::kLoopStart, 0.25f);
  r.e.setParam(SamplerEngine::kLoopEnd, 0.75f);
  r.e.setParam(SamplerEngine::kAmpRelease, 50);
  r.e.noteOn(60, 127);
  auto out = render(r.e, 48000);  // ten times the sample's length
  CHECK(peak(out, 40000) > 0.4f);
  r.e.noteOff(60);
  out = render(r.e, 9600);
  CHECK(peak(out, 4800) < 1e-3f);
  CHECK(r.e.voiceCount() == 0);

  // Ping-pong on a ramp: going up, then down, then up again inside the loop.
  std::vector<float> ramp(4800);
  for (int i = 0; i < 4800; ++i) ramp[static_cast<std::size_t>(i)] = static_cast<float>(i) / 4800.0f;
  Rig p(ramp);
  p.e.setParam(SamplerEngine::kLoop, SamplerEngine::kLoopPingPong);
  p.e.setParam(SamplerEngine::kLoopStart, 0.5f);
  p.e.setParam(SamplerEngine::kLoopEnd, 0.75f);
  p.e.noteOn(60, 127);
  out = render(p.e, 9600);
  CHECK(out[3500] > out[3400]);                 // up to the loop end at 3600
  CHECK(out[4000] < out[3900]);                 // then back down
  CHECK(out[5000] > out[4900]);                 // up again after reaching 2400
  CHECK(*std::max_element(out.begin(), out.end()) <= 0.751f);
  CHECK(*std::min_element(out.begin() + 3000, out.end()) >= 0.499f);
}

TEST_CASE("sampler: a loop crossfade hides a loop point that isn't at a zero crossing") {
  // 437 Hz looped over a length that isn't a whole number of cycles: a jump at every wrap.
  auto run = [](float xfadeMs) {
    Rig r(sine(437, 48000));
    r.e.setParam(SamplerEngine::kLoop, SamplerEngine::kLoopForward);
    r.e.setParam(SamplerEngine::kLoopStart, 0.3f);
    r.e.setParam(SamplerEngine::kLoopEnd, 0.6123f);
    r.e.setParam(SamplerEngine::kCrossfade, xfadeMs);
    r.e.noteOn(60, 127);
    return maxStep(render(r.e, 96000), 2000);
  };
  const float steady = static_cast<float>(0.5 * 2 * std::numbers::pi * 437 / 48000);  // the sine's own steepest step
  CHECK(run(0) > steady * 3);                 // a click at each wrap
  CHECK(run(20) < steady * 1.3f);             // smooth with 20 ms
}

TEST_CASE("sampler: amp envelope times and levels") {
  Rig r(std::vector<float>(96000, 0.5f));
  r.e.setParam(SamplerEngine::kAmpAttack, 100);
  r.e.setParam(SamplerEngine::kAmpDecay, 200);
  r.e.setParam(SamplerEngine::kAmpSustain, 0.5f);
  r.e.setParam(SamplerEngine::kAmpRelease, 300);
  r.e.noteOn(60, 127);
  auto out = render(r.e, 48000);
  CHECK(out[2400] == doctest::Approx(0.25f).epsilon(0.03));   // half way up at 50 ms
  CHECK(out[4800] == doctest::Approx(0.5f).epsilon(0.02));    // the top at 100 ms
  CHECK(out[4800 + 9600] == doctest::Approx(0.25f).epsilon(0.02));  // decayed to sustain (0.5 × 0.5) 200 ms on
  CHECK(out[40000] == doctest::Approx(0.25f).epsilon(0.01));
  r.e.noteOff(60);
  out = render(r.e, 24000);
  CHECK(std::fabs(out[14400]) < 0.25f * 0.0012f);  // −60 dB after the release time
  CHECK(maxStep(out) < 0.01f);
}

TEST_CASE("sampler: filter, key tracking and velocity") {
  auto level = [](int note, float keytrack, float cutoff, uint8_t vel = 127, float velFilter = 0) {
    Rig r(sine(4000, 48000));
    r.e.setParam(SamplerEngine::kFilter, SamplerEngine::kLp24);
    r.e.setParam(SamplerEngine::kCutoff, cutoff);
    r.e.setParam(SamplerEngine::kKeyTrack, keytrack);
    r.e.setParam(SamplerEngine::kVelFilter, velFilter);
    r.e.noteOn(static_cast<uint8_t>(note), vel);
    return peak(render(r.e, 9600), 4800);
  };
  CHECK(level(60, 0, 20000) > 0.45f);
  CHECK(level(60, 0, 500) < 0.005f);            // 3 octaves above a 24 dB/oct low-pass
  // Key tracking 100%: an octave up opens the filter an octave, so the
  // (also an octave higher) tone sits at the same place relative to it.
  CHECK(level(72, 1, 4000) == doctest::Approx(level(60, 1, 2000)).epsilon(0.1));
  CHECK(level(60, 0, 8000, 40, 1) < level(60, 0, 8000, 127, 1) * 0.5f);  // soft notes darker
}

TEST_CASE("sampler: LFO onto pitch, synced to the tempo; glide") {
  Rig r(sine(1000, 96000));
  r.e.setParam(SamplerEngine::kLfoSync, 1);
  r.e.setParam(SamplerEngine::kLfoDivision, 4);  // a quarter note
  r.e.setParam(SamplerEngine::kLfoShape, SamplerEngine::kSquare);
  r.e.setParam(SamplerEngine::kLfoPitch, 1200);  // ± an octave
  r.e.setTempo(120);                              // a quarter = 0.5 s
  r.e.noteOn(60, 127);
  auto out = render(r.e, 48000);
  // Square: 0.25 s an octave up (2 kHz), 0.25 s down (500 Hz), and again.
  CHECK(crossings(out, 1000, 11000) == doctest::Approx(833).epsilon(0.03));
  CHECK(crossings(out, 13000, 23000) == doctest::Approx(208).epsilon(0.03));

  Rig g(sine(1000, 96000));
  g.e.setParam(SamplerEngine::kGlide, 200);
  g.e.noteOn(60, 127);
  render(g.e, 4800);
  g.e.noteOn(72, 127);  // glides from C3 up to C4
  g.e.setParam(SamplerEngine::kPoly, 1);
  out = render(g.e, 48000);
  const int early = crossings(out, 0, 2400), late = crossings(out, 24000, 26400);
  CHECK(late == doctest::Approx(200).epsilon(0.03));  // 2 kHz by then
  CHECK(early < late * 0.8);                          // still on the way at the start
}

TEST_CASE("sampler: polyphony limit fades the oldest; a replaced sample waits for its voices") {
  Rig r(std::vector<float>(96000, 0.1f));
  r.e.setParam(SamplerEngine::kPoly, 3);
  for (int n = 0; n < 6; ++n) {
    r.e.noteOn(static_cast<uint8_t>(60 + n), 127);
    render(r.e, 512);
  }
  CHECK(r.e.voiceCount() == 3);

  auto b = makeSample(std::vector<float>(4800, 0.2f));
  r.e.setSample(0, b.get());
  const Sample* got = nullptr;
  CHECK_FALSE(r.e.popReleased(got));  // the old one is still playing
  r.e.allNotesOff();
  REQUIRE(r.e.popReleased(got));
  CHECK(got == r.s.get());
  std::vector<const Sample*> held;
  r.e.heldSamples(held);
  CHECK(held == std::vector<const Sample*>{b.get()});
}

TEST_CASE("sampler: nothing blows up at the extremes") {
  std::mt19937 rng(3);
  std::uniform_real_distribution<float> u(-1, 1);
  std::vector<float> noise(48000);
  for (float& x : noise) x = u(rng);
  Rig r(noise);
  r.e.setParam(SamplerEngine::kFilter, SamplerEngine::kBp);
  r.e.setParam(SamplerEngine::kReso, 1);
  r.e.setParam(SamplerEngine::kEnvAmount, 1);
  r.e.setParam(SamplerEngine::kLfoCutoff, 1);
  r.e.setParam(SamplerEngine::kLfoRate, 20);
  r.e.setParam(SamplerEngine::kLfoShape, SamplerEngine::kSampleHold);
  r.e.setParam(SamplerEngine::kLoop, SamplerEngine::kLoopPingPong);
  r.e.setParam(SamplerEngine::kLoopStart, 0.4f);
  r.e.setParam(SamplerEngine::kLoopEnd, 0.4001f);  // shorter than 16 frames: not a loop
  r.e.setParam(SamplerEngine::kTune, 24);
  bool ok = true;
  for (int i = 0; i < 30; ++i) {
    r.e.noteOn(static_cast<uint8_t>(i * 4), 127);
    for (float v : render(r.e, 2048)) ok &= std::isfinite(v) && std::fabs(v) < 50;
  }
  CHECK(ok);
}

TEST_CASE("session: a sampler track keeps its sample and settings") {
  const fs::path root = fs::temp_directory_path() / ("ion-sampler-" + std::to_string(std::random_device{}()));
  fs::create_directories(root / "lib");
  wav::Audio a;
  a.channels = 1;
  a.samples = sine(220, 9600);
  std::string error;
  REQUIRE(wav::write(root / "lib" / "tone.wav", a, error));
  {
    auto module = std::make_unique<Module>();
    Config config;
    Session s(*module, Paths::under(root), config);
    s.newProject("smp");
    s.setEngine(2, EngineType::Sampler);
    s.setPadSample(2, 0, (root / "lib" / "tone.wav").string());
    s.setParam(engineParamRef(2, SamplerEngine::kRoot), 57);
    s.setParam(engineParamRef(2, SamplerEngine::kLoop), SamplerEngine::kLoopForward);
    s.setParam(engineParamRef(2, SamplerEngine::kLoopEnd), 0.8f);
    REQUIRE(s.waitForSamples());
    std::vector<float> buf(2 * 512);
    module->process(buf.data(), 512);
    module->pushMidi({{0x92, 57, 100}});
    module->process(buf.data(), 512);
    CHECK(module->status().trackVoices[2].load() == 1);
    REQUIRE(s.save(error));
    CHECK(s.toJson().find("tone.wav") != std::string::npos);
  }
  {
    auto module = std::make_unique<Module>();
    Config config;
    Session s(*module, Paths::under(root), config);
    REQUIRE(s.load("smp", error));
    REQUIRE(s.waitForSamples());
    std::vector<float> buf(2 * 512);
    module->process(buf.data(), 512);
    s.tick();
    CHECK(s.track(2).engine == EngineType::Sampler);
    CHECK(s.padStatus(2, 0) == Session::PadStatus::Ready);
    CHECK(s.param(engineParamRef(2, SamplerEngine::kRoot)) == 57.0f);
    CHECK(module->params().get(engineParamRef(2, SamplerEngine::kLoopEnd)) == doctest::Approx(0.8f));
    CHECK(s.projectSummary("smp").tracks[2].sound == "tone");
  }
  fs::remove_all(root);
}
