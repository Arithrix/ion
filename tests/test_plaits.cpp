// Plaits (HANDOFF §6.4, DECISIONS D-56, D-57).

#include <doctest.h>

#include <chrono>
#include <cmath>
#include <vector>

#include "engine/envelope_params.h"
#include "engine/plaits/plaits_engine.h"

using namespace ion;

namespace {

struct Out {
  std::vector<float> left, right;
  float peak() const {
    float p = 0;
    for (float s : left) p = std::max(p, std::fabs(s));
    return p;
  }
  bool finite() const {
    for (float s : left)
      if (!std::isfinite(s)) return false;
    return true;
  }
};

Out run(PlaitsEngine& e, int frames) {
  Out o;
  o.left.assign(static_cast<std::size_t>(frames), 0.0f);
  o.right.assign(static_cast<std::size_t>(frames), 0.0f);
  for (int i = 0; i < frames; i += 64) {
    const int n = std::min(64, frames - i);
    e.render(o.left.data() + i, o.right.data() + i, static_cast<std::size_t>(n));
  }
  return o;
}

// Zero crossings (rising) per second over a steady stretch.
double frequency(const std::vector<float>& x, std::size_t from) {
  int crossings = 0;
  std::size_t first = 0, last = 0;
  for (std::size_t i = from + 1; i < x.size(); ++i)
    if (x[i - 1] < 0 && x[i] >= 0) {
      if (!crossings) first = i;
      last = i;
      ++crossings;
    }
  return crossings > 1 ? (crossings - 1) * static_cast<double>(kSampleRate) / static_cast<double>(last - first) : 0;
}

}  // namespace

TEST_CASE("plaits: every model plays, stays finite, and its names and roles are there") {
  for (int m = 0; m < PlaitsEngine::kModels; ++m) {
    CAPTURE(PlaitsEngine::modelName(m));
    PlaitsEngine e;
    e.setParam(PlaitsEngine::kModel, static_cast<float>(m));
    e.noteOn(60, 110);
    const Out o = run(e, 24000);
    CHECK(o.finite());
    CHECK(o.peak() > 0.01f);
    CHECK(o.peak() < 2.0f);
    for (int k = 0; k < 3; ++k) CHECK(std::string(PlaitsEngine::macroRole(m, k)).size() > 0);
  }
  CHECK(std::string(PlaitsEngine::modelName(22)) == "String machine");
  CHECK(PlaitsEngine::plaitsIndex(0) == 8);    // green's first is Plaits' engine 8
  CHECK(PlaitsEngine::plaitsIndex(16) == 0);   // orange is registered first upstream
  CHECK(PlaitsEngine::isSixOp(19));
}

TEST_CASE("plaits: in tune at a true 48 kHz (V-01's patch)") {
  PlaitsEngine e;
  e.setParam(PlaitsEngine::kModel, 2);  // 2-op FM with no modulation: a sine
  e.setParam(PlaitsEngine::kTimbre, 0);
  e.setParam(PlaitsEngine::kMorph, 0.5f);
  e.setParam(PlaitsEngine::kLpgMode, 2);  // off: the oscillator itself
  e.noteOn(69, 100);
  const Out o = run(e, 48000);
  const double f = frequency(o.left, 4800);
  CAPTURE(f);
  MESSAGE("A4 plays at " << f << " Hz");
  CHECK(std::fabs(1200.0 * std::log2(f / 440.0)) < 1.0);  // within a cent (unpatched it's +4.6)
}

TEST_CASE("plaits: the low-pass gate's modes") {
  auto tail = [](int mode, bool hold) {
    PlaitsEngine e;
    e.setParam(PlaitsEngine::kModel, 0);
    e.setParam(PlaitsEngine::kLpgMode, static_cast<float>(mode));
    e.setParam(PlaitsEngine::kLpgDecay, 0.3f);
    e.noteOn(48, 120);
    Out held = run(e, 48000);  // a second held
    if (!hold) e.noteOff(48);
    float late = 0;
    for (std::size_t i = 40000; i < 48000; ++i) late = std::max(late, std::fabs(held.left[i]));
    return std::pair{late, e.voiceCount()};
  };
  // Gate: a held note sustains.
  CHECK(tail(0, true).first > 0.05f);
  // Ping: it dies away by itself even while held.
  CHECK(tail(1, true).first < 0.01f);
  // Off: the oscillator straight out, held.
  CHECK(tail(2, true).first > 0.05f);

  // After the key comes up the voice frees itself, in every mode.
  for (int mode : {0, 1, 2}) {
    CAPTURE(mode);
    PlaitsEngine e;
    e.setParam(PlaitsEngine::kLpgMode, static_cast<float>(mode));
    e.setParam(PlaitsEngine::kLpgDecay, 0.2f);
    e.noteOn(60, 100);
    run(e, 4800);
    e.noteOff(60);
    run(e, 48000 * 3);
    CHECK(e.voiceCount() == 0);
  }
}

TEST_CASE("plaits: polyphony, stealing and the same note restruck") {
  PlaitsEngine e;
  e.setParam(PlaitsEngine::kPoly, 4);
  for (uint8_t n : std::initializer_list<uint8_t>{60, 64, 67, 71}) e.noteOn(n, 100);
  run(e, 256);
  CHECK(e.voiceCount() == 4);
  e.noteOn(74, 100);  // a fifth note takes the oldest
  run(e, 256);
  CHECK(e.voiceCount() == 4);
  e.noteOn(74, 100);  // the same note again doesn't take another voice
  run(e, 256);
  CHECK(e.voiceCount() == 4);
  e.setParam(PlaitsEngine::kPoly, 1);  // down to one: the others stop quickly
  run(e, 2048);
  CHECK(e.voiceCount() <= 1);
  e.allNotesOff();
  CHECK(e.voiceCount() == 0);
}

TEST_CASE("plaits: the amp envelope comes after the LPG; velocity sets the level") {
  auto peakAt = [](uint8_t vel, float velLpg) {
    PlaitsEngine e;
    e.setParam(PlaitsEngine::kVelLpg, velLpg);
    e.noteOn(60, vel);
    return run(e, 12000).peak();
  };
  CHECK(peakAt(30, 1.0f) < 0.7f * peakAt(127, 1.0f));
  CHECK(std::fabs(peakAt(30, 0.0f) - peakAt(127, 0.0f)) < 0.05f * peakAt(127, 0.0f));

  PlaitsEngine e;
  e.setParam(PlaitsEngine::kAmpAttack, 500);  // half a second
  e.noteOn(60, 100);
  const Out o = run(e, 4800);
  float early = 0;
  for (std::size_t i = 0; i < 480; ++i) early = std::max(early, std::fabs(o.left[i]));
  CHECK(early < 0.2f * o.peak());
}

TEST_CASE("plaits: a patch list for the first 6-op model") {
  PlaitsEngine e;
  e.setParam(PlaitsEngine::kModel, 18);
  e.noteOn(60, 100);
  const float builtIn = run(e, 9600).peak();
  CHECK(builtIn > 0.01f);
  // Silence: every packed voice all zeros means operators at level 0.
  auto* bank = new PlaitsBank();
  PlaitsBank* old = e.setBank(bank);
  CHECK(old == nullptr);
  e.noteOn(62, 100);
  run(e, 9600);
  delete e.setBank(nullptr);  // back to the built-in bank
}

TEST_CASE("plaits: six voices of every model fit the budget") {
  // Not a benchmark (ion-bench is), just a guard against something badly
  // wrong: six voices for a second of audio should take well under a second.
  for (int m : {0, 6, 11, 18, 22}) {
    CAPTURE(PlaitsEngine::modelName(m));
    PlaitsEngine e;
    e.setParam(PlaitsEngine::kModel, static_cast<float>(m));
    for (uint8_t n : std::initializer_list<uint8_t>{48, 52, 55, 60, 64, 67}) e.noteOn(n, 100);
    const auto t0 = std::chrono::steady_clock::now();
    run(e, 48000);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE(std::string(PlaitsEngine::modelName(m)) << ": six voices, one second, " << s * 1000 << " ms");
    CHECK(s < 0.5);
  }
}

// ---- the session: files, presets, the patch list, undo ----

#include <filesystem>
#include <memory>
#include <random>

#include "engine/module.h"
#include "project/config.h"
#include "project/session.h"

namespace {

namespace fs = std::filesystem;

struct PlaitsRig {
  fs::path root = fs::temp_directory_path() / ("ion-plaits-" + std::to_string(std::random_device{}()));
  std::unique_ptr<Module> module = std::make_unique<Module>();
  Config config;
  std::unique_ptr<Session> session;
  std::vector<float> buf = std::vector<float>(2 * 256);
  PlaitsRig() {
    fs::create_directories(root);
    session = std::make_unique<Session>(*module, Paths::under(root), config);
    session->newProject("plaits");
  }
  ~PlaitsRig() {
    session.reset();
    fs::remove_all(root);
  }
  void run(int blocks = 4) {
    for (int i = 0; i < blocks; ++i) module->process(buf.data(), 256);
    session->tick();
  }
};

syx::Voice named(const char* name) {
  syx::Voice v{};
  for (int op = 0; op < 6; ++op) v[static_cast<std::size_t>(op * 21 + 18)] = 1;
  std::string n = name;
  n.resize(10, ' ');
  std::copy(n.begin(), n.end(), v.begin() + 145);
  return v;
}

}  // namespace

TEST_CASE("plaits session: a track saves and loads with its settings and patch list") {
  PlaitsRig r;
  Session& s = *r.session;
  s.setEngine(2, EngineType::Plaits);
  s.setParam(engineParamRef(2, PlaitsEngine::kModel), 18);
  s.setParam(engineParamRef(2, PlaitsEngine::kTimbre), 0.8f);
  s.setPlaitsPatches(2, {named("BRASS 1"), named("E.PIANO 1")});
  r.run();
  std::string error;
  REQUIRE(s.save(error));
  // Plays: the engine got its bank (a note sounds).
  s.sendMidi({0x92, 60, 100});
  r.run(40);
  CHECK(r.module->status().trackVoices[2].load() == 1);

  PlaitsRig r2;
  r2.session = std::make_unique<Session>(*r2.module, Paths::under(r.root), r2.config);
  REQUIRE(r2.session->load("plaits", error));
  const Session& l = *r2.session;
  CHECK(l.track(2).engine == EngineType::Plaits);
  CHECK(l.param(engineParamRef(2, PlaitsEngine::kModel)) == 18.0f);
  CHECK(l.param(engineParamRef(2, PlaitsEngine::kTimbre)) == doctest::Approx(0.8f));
  REQUIRE(l.track(2).plaitsPatches.size() == 2);
  CHECK(syx::name(l.track(2).plaitsPatches[1]) == "E.PIANO 1");
  r2.session.reset();
}

TEST_CASE("plaits session: presets keep every setting and the list; undo covers them") {
  PlaitsRig r;
  Session& s = *r.session;
  s.setEngine(0, EngineType::Plaits);
  s.setParam(engineParamRef(0, PlaitsEngine::kModel), 22);
  s.setParam(engineParamRef(0, PlaitsEngine::kHarmonics), 0.4f);
  s.setPlaitsPatches(0, {named("STRINGS 1")});
  std::string error;
  REQUIRE(s.savePlaitsPreset(0, "glass strings", error));
  CHECK(s.listPlaitsPresets() == std::vector<std::string>{"glass strings"});
  CHECK_FALSE(s.savePlaitsPreset(0, "bad/name", error));

  s.setEngine(1, EngineType::Plaits);
  s.commitEdit();
  REQUIRE(s.loadPlaitsPreset(1, "glass strings", error));
  s.commitEdit();
  CHECK(s.param(engineParamRef(1, PlaitsEngine::kModel)) == 22.0f);
  CHECK(s.param(engineParamRef(1, PlaitsEngine::kHarmonics)) == doctest::Approx(0.4f));
  CHECK(s.track(1).plaitsPatches.size() == 1);
  CHECK(s.undoLabel() == "Track 2 · preset glass strings");
  s.undo();
  CHECK(s.param(engineParamRef(1, PlaitsEngine::kModel)) == 0.0f);
  CHECK(s.track(1).plaitsPatches.empty());
  s.redo();
  CHECK(s.track(1).plaitsPatches.size() == 1);
  CHECK_FALSE(s.loadPlaitsPreset(1, "missing", error));

  // Clearing the list is a step too.
  s.setPlaitsPatches(1, {});
  s.commitEdit();
  CHECK(s.track(1).plaitsPatches.empty());
  s.undo();
  CHECK(s.track(1).plaitsPatches.size() == 1);
  r.run();
}

TEST_CASE("plaits session: a format 6 project (v0.6.0) still opens") {
  PlaitsRig r;
  std::string error;
  const std::string old = R"({"format": 6, "tracks": [{"track": 1, "engine": "fm", "channel": 1}], "rec": {"bars": 2}})";
  REQUIRE(r.session->fromJson(old, error));
  CHECK(r.session->track(0).engine == EngineType::Fm);
  CHECK(r.session->recSetup().bars == 2);
  CHECK(r.session->toJson().find("\"format\": 7") != std::string::npos);
}
