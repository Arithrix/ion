// Modulation (DECISIONS D-58).

#include <doctest.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <random>
#include <vector>

#include "engine/fx/effects.h"
#include "engine/modulation.h"
#include "engine/module.h"
#include "engine/plaits/plaits_engine.h"
#include "project/config.h"
#include "project/session.h"

using namespace ion;
namespace fs = std::filesystem;

namespace {

// A track's modulation values at their defaults, to change and hand to TrackModulation.
std::vector<float> defaults() {
  std::vector<float> v;
  for (const ParamInfo& p : modParams()) v.push_back(p.def);
  return v;
}
float& lfo(std::vector<float>& m, int l, int p) { return m[static_cast<std::size_t>(lfoRef(0, l, p).index - kModBase)]; }
float& env(std::vector<float>& m, int e, int p) { return m[static_cast<std::size_t>(modEnvRef(0, e, p).index - kModBase)]; }

void run(TrackModulation& tm, const std::vector<float>& m, double seconds, double bpm = 120) {
  const int blocks = static_cast<int>(seconds * kSampleRate / 64);
  for (int i = 0; i < blocks; ++i) tm.advance(m.data(), bpm, 64);
}

struct Rig {
  fs::path root = fs::temp_directory_path() / ("ion-mod-" + std::to_string(std::random_device{}()));
  std::unique_ptr<Module> module = std::make_unique<Module>();
  Config config;
  std::unique_ptr<Session> session;
  std::vector<float> buf = std::vector<float>(2 * 256);
  Rig() {
    fs::create_directories(root);
    session = std::make_unique<Session>(*module, Paths::under(root), config);
    session->newProject("mod");
  }
  ~Rig() {
    session.reset();
    fs::remove_all(root);
  }
  void run(int blocks = 4) {
    for (int i = 0; i < blocks; ++i) module->process(buf.data(), 256);
    session->tick();
  }
  float live(int t, int r) const { return module->status().modLive[static_cast<std::size_t>(t)][static_cast<std::size_t>(r)].load(); }
};

constexpr ParamRef fader(int t) { return {static_cast<uint8_t>(t), static_cast<uint16_t>(kFader)}; }

}  // namespace

TEST_CASE("modulation: the parameter table") {
  CHECK(modParams().size() == static_cast<std::size_t>(kModParamCount));
  CHECK(kModBase + kModRange == ParamStore::kPerScope);
  CHECK(std::string(modParams()[static_cast<std::size_t>(lfoRef(0, 1, kLfoRate).index - kModBase)].key) == "lfo2.rate");
  CHECK(std::string(modParams()[static_cast<std::size_t>(modEnvRef(0, 0, kMEnvDecay).index - kModBase)].key) == "env1.decay");
  CHECK(std::string(modParams()[static_cast<std::size_t>(routeRef(0, 7, kRouteDepth).index - kModBase)].key) == "mod8.depth");
  CHECK(routeRef(0, 7, kRouteDepth).index < kModBase + kModParamCount);
  CHECK(lfoRef(3, 0, kLfoShape).isMod());
  CHECK_FALSE(lfoRef(3, 0, kLfoShape).isEngine());
  CHECK(engineParamRef(3, 0).isEngine());
}

TEST_CASE("modulation: LFO shapes, rate, phase and sync") {
  auto m = defaults();
  TrackModulation tm;
  // Sine at 1 Hz: a quarter second in, it's at the top.
  run(tm, m, 0.25);
  CHECK(tm.source(kSrcLfo1) == doctest::Approx(1.0f).epsilon(0.02));
  run(tm, m, 0.5);
  CHECK(tm.source(kSrcLfo1) == doctest::Approx(-1.0f).epsilon(0.02));
  // A 90° start phase moves it on a quarter.
  lfo(m, 0, kLfoPhase) = 90;
  TrackModulation t2;
  run(t2, m, 0.25);
  CHECK(std::fabs(t2.source(kSrcLfo1)) < 0.05f);
  // Synced to the clock: a quarter note at 120 BPM is 2 Hz.
  lfo(m, 0, kLfoPhase) = 0;
  lfo(m, 0, kLfoSync) = 1;
  lfo(m, 0, kLfoDivision) = 4;  // 1/4
  TrackModulation t3;
  run(t3, m, 0.125);
  CHECK(t3.source(kSrcLfo1) == doctest::Approx(1.0f).epsilon(0.02));
  // Square, saws and triangle stay in −1…1; sample & hold steps once a cycle.
  for (int shape : {kLfoTriangle, kLfoSawUp, kLfoSawDown, kLfoSquare, kLfoSampleHold, kLfoSmooth}) {
    CAPTURE(shape);
    auto ms = defaults();
    lfo(ms, 0, kLfoShape) = static_cast<float>(shape);
    lfo(ms, 0, kLfoRate) = 7;
    TrackModulation t;
    std::vector<float> seen;
    for (int i = 0; i < 1500; ++i) {
      t.advance(ms.data(), 120, 64);
      seen.push_back(t.source(kSrcLfo1));
    }
    float lo = 1, hi = -1;
    for (float v : seen) {
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    CHECK(lo >= -1.0f);
    CHECK(hi <= 1.0f);
    CHECK(hi - lo > 0.5f);
  }
}

TEST_CASE("modulation: LFOs restart on notes or bars") {
  auto m = defaults();
  lfo(m, 0, kLfoRestart) = kRestartNote;
  TrackModulation tm;
  run(tm, m, 0.3);
  tm.noteOn(m.data(), 60, 100);
  CHECK(std::fabs(tm.source(kSrcLfo1)) < 0.01f);  // sine from 0
  lfo(m, 1, kLfoRestart) = kRestartBar;
  run(tm, m, 0.3);
  tm.downbeat(m.data(), 3);
  CHECK(tm.lfoPhase(1) == 0.0f);
}

TEST_CASE("modulation: envelopes fire on notes, every note or the first of a phrase") {
  auto m = defaults();
  env(m, 0, kMEnvDecay) = 100;
  env(m, 0, kMEnvSustain) = 0.5f;
  env(m, 1, kMEnvTrigger) = 1;  // first note only
  env(m, 1, kMEnvSustain) = 0;
  env(m, 1, kMEnvDecay) = 100;
  TrackModulation tm;
  CHECK(tm.source(kSrcEnv1) == 0.0f);
  tm.noteOn(m.data(), 60, 127);
  CHECK(tm.source(kSrcEnv1) == doctest::Approx(1.0f));  // an instant attack is up already
  CHECK(tm.source(kSrcVelocity) == doctest::Approx(1.0f));
  CHECK(tm.source(kSrcKey) == doctest::Approx(60.0f / 127));
  run(tm, m, 0.5);
  CHECK(tm.source(kSrcEnv1) == doctest::Approx(0.5f).epsilon(0.02));  // at sustain
  CHECK(tm.source(kSrcEnv2) < 0.01f);
  tm.noteOn(m.data(), 64, 64);  // legato: env 2 doesn't fire again, env 1 does
  CHECK(tm.source(kSrcEnv1) == doctest::Approx(1.0f));
  CHECK(tm.source(kSrcEnv2) < 0.01f);
  tm.noteOff(60);
  run(tm, m, 0.05);
  CHECK(tm.source(kSrcEnv1) > 0.3f);  // a note is still held
  tm.noteOff(64);
  run(tm, m, 1.0);
  CHECK(tm.source(kSrcEnv1) == 0.0f);  // released
  tm.noteOn(m.data(), 67, 100);  // a new phrase: env 2 fires
  CHECK(tm.source(kSrcEnv2) == doctest::Approx(1.0f));
}

TEST_CASE("modulation: a route moves its target over the value set, and lets go") {
  Rig r;
  Session& s = *r.session;
  s.setParam(fader(0), -12);
  REQUIRE(s.addModRoute(fader(0)) == 0);
  CHECK(s.param(routeRef(0, 0, kRouteSource)) == static_cast<float>(kSrcLfo1));
  CHECK(s.param(routeRef(0, 0, kRouteDepth)) == doctest::Approx(0.25f));
  s.setParam(lfoRef(0, 0, kLfoRate), 5);
  r.run(2);
  float lo = 2, hi = -1;
  for (int i = 0; i < 100; ++i) {
    r.run(1);
    lo = std::min(lo, r.live(0, 0));
    hi = std::max(hi, r.live(0, 0));
  }
  const float base = toNormalized(mixerParam(kFader), -12);
  CHECK(lo == doctest::Approx(base - 0.25f).epsilon(0.05));
  CHECK(hi == doctest::Approx(base + 0.25f).epsilon(0.05));
  CHECK(r.module->params().get(fader(0)) == -12.0f);  // the value set doesn't move
  CHECK(s.param(fader(0)) == -12.0f);

  s.setParam(routeRef(0, 0, kRouteDepth), 0);
  r.run(2);
  CHECK(r.live(0, 0) == -1.0f);
  // Switches and choices can't be targets; nor can another track's values.
  CHECK_FALSE(s.modulatable({0, static_cast<uint16_t>(kMute)}));
  CHECK_FALSE(s.modulatable(lfoRef(0, 0, kLfoRate)));
  CHECK_FALSE(s.modulatable(fader(kMasterScope)));
  s.setModTarget(0, 1, fader(1));
  CHECK_FALSE(s.modTarget(0, 1));
}

TEST_CASE("modulation: an envelope on a Plaits macro, fired by the track's notes") {
  Rig r;
  Session& s = *r.session;
  s.setEngine(2, EngineType::Plaits);
  const ParamRef timbre = engineParamRef(2, PlaitsEngine::kTimbre);
  s.setParam(timbre, 0.2f);
  const int route = s.addModRoute(timbre);
  REQUIRE(route == 0);
  s.setParam(routeRef(2, route, kRouteSource), kSrcEnv1);
  s.setParam(routeRef(2, route, kRouteDepth), 0.5f);
  s.setParam(modEnvRef(2, 0, kMEnvDecay), 500);
  r.run(2);
  CHECK(r.live(2, 0) == doctest::Approx(0.2f));  // nothing played: the envelope's at 0
  s.sendMidi({0x92, 60, 100});
  r.run(1);
  CHECK(r.live(2, 0) > 0.5f);
  r.run(60);  // the decay is over
  CHECK(r.live(2, 0) == doctest::Approx(0.2f).epsilon(0.02));
}

TEST_CASE("modulation: saved, loaded, undone; engine and insert changes look after the routes") {
  Rig r;
  Session& s = *r.session;
  s.setEngine(0, EngineType::Plaits);
  s.commitEdit();
  REQUIRE(s.addModRoute(engineParamRef(0, PlaitsEngine::kMorph)) == 0);
  REQUIRE(s.addModRoute(fader(0)) == 1);
  s.setParam(lfoRef(0, 1, kLfoShape), kLfoSquare);
  s.commitEdit();
  std::string error;
  REQUIRE(s.save(error));

  Rig r2;
  r2.session = std::make_unique<Session>(*r2.module, Paths::under(r.root), r2.config);
  REQUIRE(r2.session->load("mod", error));
  CHECK(r2.session->modTarget(0, 0) == engineParamRef(0, PlaitsEngine::kMorph));
  CHECK(r2.session->modTarget(0, 1) == fader(0));
  CHECK(r2.session->param(lfoRef(0, 1, kLfoShape)) == static_cast<float>(kLfoSquare));
  CHECK(r2.session->param(routeRef(0, 1, kRouteDepth)) == doctest::Approx(0.25f));
  r2.session.reset();

  // A new engine: routes onto the old engine's values let go; the fader's stays. Undo brings them back.
  s.setEngine(0, EngineType::Fm);
  s.commitEdit();
  CHECK_FALSE(s.modTarget(0, 0));
  CHECK(s.modTarget(0, 1) == fader(0));
  s.undo();
  CHECK(s.modTarget(0, 0) == engineParamRef(0, PlaitsEngine::kMorph));

  // A route onto an insert follows it when it moves, and lets go when it's replaced.
  s.setInsert(0, 0, EffectType::Compressor);
  const int route = s.addModRoute(insertParamRef(0, 0, Compressor::kThreshold));
  REQUIRE(route == 2);
  s.moveInsert(0, 0, 1);
  s.commitEdit();
  CHECK(s.modTarget(0, route) == insertParamRef(0, 1, Compressor::kThreshold));
  s.setInsert(0, 1, EffectType::Chorus);
  CHECK_FALSE(s.modTarget(0, route));
  s.commitEdit();
  s.undo();
  CHECK(s.modTarget(0, route) == insertParamRef(0, 1, Compressor::kThreshold));

  // Removing a route is a step too.
  s.setModTarget(0, 1, std::nullopt);
  s.commitEdit();
  CHECK(s.undoLabel() == "Track 1 · mod 2 cleared");
  s.undo();
  CHECK(s.modTarget(0, 1) == fader(0));
  r.run();
}

TEST_CASE("modulation: keys for CC learn and labels") {
  Rig r;
  Session& s = *r.session;
  CHECK(s.paramKey(lfoRef(2, 0, kLfoRate)) == "track3.lfo1.rate");
  CHECK(s.parseParamKey("track3.mod4.depth") == routeRef(2, 3, kRouteDepth));
  CHECK(s.paramLabel(modEnvRef(1, 1, kMEnvAttack)) == "Track 2 · Env 2 attack");
  CHECK(s.paramLabel(routeRef(0, 2, kRouteDepth)) == "Track 1 · Mod 3 depth");
  CHECK(s.addMapping(0, 30, routeRef(0, 0, kRouteDepth)) >= 0);
  s.sendMidi({0xB0, 30, 127});
  r.run();
  CHECK(s.param(routeRef(0, 0, kRouteDepth)) == 1.0f);
}
