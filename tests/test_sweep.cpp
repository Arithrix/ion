// The 0.10.1 sweep: randomised tests across the whole of ion, looking for
// what the feature tests don't think to try. Every run uses fixed seeds, so a
// failure repeats.

#include <doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <regex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "audio/wav.h"
#include "engine/acid/acid_engine.h"
#include "engine/chip/chip_engine.h"
#include "engine/drum/drum_engine.h"
#include "engine/fm/fm_engine.h"
#include "engine/fx/effects.h"
#include "engine/module.h"
#include "engine/plaits/plaits_engine.h"
#include "engine/sampler/sampler_engine.h"
#include "project/config.h"
#include "project/session.h"
#include "ui/app.h"

using namespace ion;
namespace fs = std::filesystem;

namespace {

// The seeds to run: the fixed ones, or with ION_SWEEP_DEEP=N set, N more
// besides (a long one-off hunt; the suite keeps to the fixed ones).
std::vector<unsigned> seeds(std::initializer_list<unsigned> fixed) {
  std::vector<unsigned> out(fixed);
  if (const char* deep = std::getenv("ION_SWEEP_DEEP"))
    for (int i = 0, n = std::atoi(deep); i < n; ++i) out.push_back(1000u + static_cast<unsigned>(i) * 7919u + *fixed.begin());
  return out;
}

struct Root {
  fs::path path = fs::temp_directory_path() / ("ion-sweep-" + std::to_string(std::random_device{}()));
  Root() { fs::create_directories(path); }
  ~Root() { fs::remove_all(path); }
};

// A session with a couple of samples on disk to put on pads.
struct Rig {
  Root root;
  std::unique_ptr<Module> module = std::make_unique<Module>();
  Config config;
  std::unique_ptr<Session> session;
  std::vector<float> buf = std::vector<float>(2 * 256);
  std::vector<std::string> samples;
  Rig() {
    config.autosaveMinutes = 0;
    session = std::make_unique<Session>(*module, Paths::under(root.path), config);
    session->newProject("sweep");
    for (int i = 0; i < 3; ++i) {
      wav::Audio a;
      a.channels = 1;
      a.samples.resize(4800);
      for (std::size_t k = 0; k < a.samples.size(); ++k) a.samples[k] = 0.5f * std::sin(0.05f * static_cast<float>(k) * static_cast<float>(i + 1));
      const fs::path f = root.path / ("s" + std::to_string(i) + ".wav");
      std::string error;
      REQUIRE(wav::write(f, a, error));
      samples.push_back(f.string());
    }
  }
  // Runs the audio a little and checks what came out.
  void run(int blocks = 2) {
    for (int i = 0; i < blocks; ++i) {
      module->process(buf.data(), 256);
      for (float s : buf) REQUIRE(std::isfinite(s));
    }
    session->tick();
  }
};

// The project as JSON without the moment it was written.
std::string doc(const Session& s) {
  return std::regex_replace(s.toJson(), std::regex("\"saved_at\": \"[^\"]*\""), "");
}

syx::Voice voice(std::mt19937& rng) {
  syx::Voice v{};
  for (auto& b : v) b = static_cast<uint8_t>(rng() % 100);
  return syx::normalize(v);
}

const EngineType kEngines[] = {EngineType::None, EngineType::Fm,     EngineType::Drum, EngineType::Sampler,
                               EngineType::Plaits, EngineType::Acid, EngineType::Chip};

// One random edit: what a person could do with keys or the mouse.
void randomEdit(Rig& r, std::mt19937& rng) {
  Session& s = *r.session;
  auto pick = [&](int n) { return static_cast<int>(rng() % static_cast<unsigned>(n)); };
  auto unit = [&] { return static_cast<float>(rng() % 10001) / 10000.0f; };
  const int t = pick(kTrackCount);
  switch (pick(14)) {
    case 0: s.setEngine(t, kEngines[pick(7)]); break;
    case 1:
    case 2:
    case 3: {  // a value somewhere on the track: mixer, insert, engine, modulation
      const int area = pick(4);
      ParamRef ref{static_cast<uint8_t>(t), 0};
      if (area == 0) ref.index = static_cast<uint16_t>(pick(kMixerParamCount));
      else if (area == 1) ref.index = static_cast<uint16_t>(kInsertBase + pick(kInsertSlots * kInsertStride));
      else if (area == 2) ref.index = static_cast<uint16_t>(kEngineParamBase + pick(kMaxEngineParams));
      else ref.index = static_cast<uint16_t>(kModBase + pick(kModParamCount));
      if (const ParamInfo* pi = s.paramInfo(ref)) {
        if (pick(2)) s.setParam(ref, fromNormalized(*pi, unit()));
        else s.nudgeParam(ref, pick(2) ? 1 : -1, pick(2) != 0);
      }
      break;
    }
    case 4: s.setInsert(pick(kScopeCount), pick(kInsertSlots), static_cast<EffectType>(pick(kEffectTypes))); break;
    case 5: s.moveInsert(pick(kScopeCount), pick(kInsertSlots), pick(2) ? 1 : -1); break;
    case 6: {
      const ParamRef target{static_cast<uint8_t>(t), static_cast<uint16_t>(pick(kModBase))};
      if (s.modulatable(target)) s.addModRoute(target);
      break;
    }
    case 7: s.setModTarget(t, pick(kModRoutes), std::nullopt); break;
    case 8: s.addMapping(static_cast<uint8_t>(pick(16)), static_cast<uint8_t>(pick(128)), {static_cast<uint8_t>(t), static_cast<uint16_t>(pick(kMixerParamCount))}); break;
    case 9:
      if (!s.ccMappings().empty()) s.removeMapping(s.ccMappings()[static_cast<std::size_t>(pick(static_cast<int>(s.ccMappings().size())))].slot);
      break;
    case 10: s.toggleProgram(voice(rng), "sweep"); break;
    case 11:
      if (s.track(t).engine == EngineType::Fm) s.setVoice(t, voice(rng), "sweep");
      else if (s.track(t).engine == EngineType::Drum || s.track(t).engine == EngineType::Sampler)
        s.setPadSample(t, pick(16), pick(4) ? r.samples[static_cast<std::size_t>(pick(3))] : std::string());
      else if (s.track(t).engine == EngineType::Plaits) {
        std::vector<syx::Voice> list;
        for (int i = pick(5); i > 0; --i) list.push_back(voice(rng));
        s.setPlaitsPatches(t, list);
      }
      break;
    case 12: s.setChannel(t, pick(16)); break;
    default: s.moveProgram(pick(8), pick(2) ? 1 : -1); break;
  }
}

// Notes and controllers: playing, not editing.
void randomPlay(Rig& r, std::mt19937& rng) {
  const auto ch = static_cast<uint8_t>(rng() % 8);
  const auto note = static_cast<uint8_t>(24 + rng() % 72);
  switch (rng() % 4) {
    case 0: r.session->sendMidi({static_cast<uint8_t>(0x90 | ch), note, static_cast<uint8_t>(1 + rng() % 127)}); break;
    case 1: r.session->sendMidi({static_cast<uint8_t>(0x80 | ch), note, 0}); break;
    case 2: r.session->sendMidi({static_cast<uint8_t>(0xE0 | ch), static_cast<uint8_t>(rng() % 128), static_cast<uint8_t>(rng() % 128)}); break;
    default: r.session->sendMidi({static_cast<uint8_t>(0xB0 | ch), 1, static_cast<uint8_t>(rng() % 128)}); break;
  }
}

}  // namespace

TEST_CASE("sweep: hundreds of random edits while it plays; undo all of them, redo all of them") {
  for (unsigned seed : seeds({1u, 2u, 3u, 4u})) {
    CAPTURE(seed);
    Rig r;
    Session& s = *r.session;
    std::mt19937 rng(seed);
    r.run();
    const std::string start = doc(s);
    for (int i = 0; i < 250; ++i) {
      randomEdit(r, rng);
      s.commitEdit();  // as the UI does after each key
      if (i % 5 == 0) randomPlay(r, rng);
      if (i % 3 == 0) r.run(1);
    }
    s.sendMidi({0xB0, 123, 0});
    s.panic();
    r.run(4);
    s.waitForSamples();
    r.run(2);
    const std::string end = doc(s);

    int steps = 0;
    while (s.canUndo() && steps < 1000) {
      s.undo();
      ++steps;
      if (steps % 7 == 0) r.run(1);
    }
    r.run(2);
    CHECK(steps > 50);
    CHECK(doc(s) == start);
    int redone = 0;
    while (s.canRedo()) {
      s.redo();
      ++redone;
      if (redone % 7 == 0) r.run(1);
    }
    s.waitForSamples();
    r.run(2);
    CHECK(redone == steps);
    CHECK(doc(s) == end);
  }
}

TEST_CASE("sweep: a random project saves and loads back exactly") {
  for (unsigned seed : seeds({11u, 12u, 13u})) {
    CAPTURE(seed);
    Rig r;
    std::mt19937 rng(seed);
    for (int i = 0; i < 200; ++i) randomEdit(r, rng);
    r.run(2);
    const std::string saved = doc(*r.session);

    Rig r2;
    std::string error;
    REQUIRE(r2.session->fromJson(r.session->toJson(), error));
    r2.session->waitForSamples();
    r2.run(2);
    CHECK(doc(*r2.session) == saved);
  }
}

TEST_CASE("sweep: every engine under random settings and notes stays finite, bounded, and lets go") {
  auto make = [](EngineType t) { return createEngine(t); };
  for (EngineType type : {EngineType::Fm, EngineType::Drum, EngineType::Sampler, EngineType::Plaits, EngineType::Acid, EngineType::Chip}) {
    for (unsigned seed : seeds({21u, 22u})) {
      const std::string engine = engineLabel(type);
      CAPTURE(engine);
      CAPTURE(seed);
      std::mt19937 rng(seed);
      auto e = make(type);
      const auto table = engineParams(type);
      Sample smp;  // for the sample engines: a short stereo tone
      smp.channels = 2;
      smp.frames = 9600;
      smp.data.assign(2 * static_cast<std::size_t>(smp.frames + 2 * Sample::kGuard), 0.0f);
      for (int64_t i = 0; i < smp.frames; ++i) smp.data[static_cast<std::size_t>(2 * (i + Sample::kGuard))] = 0.5f * std::sin(0.03f * static_cast<float>(i));
      if (type == EngineType::Drum)
        for (int p = 0; p < 16; ++p) e->setSample(p, &smp);
      if (type == EngineType::Sampler) e->setSample(0, &smp);
      std::vector<float> l(64), rr(64);
      float peak = 0;
      for (int block = 0; block < 3000; ++block) {
        if (block % 40 == 0)
          for (int k = 0; k < 4; ++k) {
            const int p = static_cast<int>(rng() % table.size());
            const ParamInfo& pi = table[static_cast<std::size_t>(p)];
            const float n = rng() % 5 == 0 ? (rng() % 2 ? 1.0f : 0.0f) : static_cast<float>(rng() % 1001) / 1000.0f;
            e->setParam(p, clampValue(pi, fromNormalized(pi, n)));
          }
        if (block % 7 == 0) e->noteOn(static_cast<uint8_t>(24 + rng() % 80), static_cast<uint8_t>(1 + rng() % 127));
        if (block % 5 == 0) e->noteOff(static_cast<uint8_t>(24 + rng() % 80));
        if (block % 97 == 0) e->pitchBend(static_cast<int>(rng() % 16384) - 8192);
        if (block == 1500) e->allNotesOff();
        std::fill(l.begin(), l.end(), 0.0f);
        std::fill(rr.begin(), rr.end(), 0.0f);
        e->render(l.data(), rr.data(), 64);
        for (std::size_t i = 0; i < 64; ++i) {
          REQUIRE(std::isfinite(l[i]));
          REQUIRE(std::isfinite(rr[i]));
          peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(rr[i])));
        }
      }
      // Bounded: a runaway filter or envelope goes to hundreds or more. Heavy
      // but legitimate settings (+24 dB of sampler level on 16 voices) reach ~20.
      CHECK(peak < 48.0f);
      // Every note off, then time to finish: nothing left sounding.
      for (int n = 0; n < 128; ++n) e->noteOff(static_cast<uint8_t>(n));
      e->setParam(static_cast<int>(table.size()) - 2, 200);  // a finite release where it's the amp release (most engines)
      for (int block = 0; block < 6000; ++block) e->render(l.data(), rr.data(), 64);
      e->allNotesOff();
      for (int block = 0; block < 100; ++block) e->render(l.data(), rr.data(), 64);
      CHECK(e->voiceCount() == 0);
    }
  }
}

TEST_CASE("sweep: every effect under random settings stays finite and bounded") {
  for (int t = 1; t < kEffectTypes; ++t) {
    const auto type = static_cast<EffectType>(t);
    const std::string effect = effectLabel(type);
    CAPTURE(effect);
    std::mt19937 rng(31u + static_cast<unsigned>(t));
    auto fx = createEffect(type);
    const auto table = effectParams(type);
    std::vector<float> l(64), r(64);
    for (int block = 0; block < 4000; ++block) {
      if (block % 50 == 0) {
        const int p = static_cast<int>(rng() % table.size());
        fx->setParam(p, fromNormalized(table[static_cast<std::size_t>(p)], static_cast<float>(rng() % 1001) / 1000.0f));
        fx->setTempo(40.0 + static_cast<double>(rng() % 260));
      }
      for (std::size_t i = 0; i < 64; ++i) {
        const float x = block % 400 < 300 ? static_cast<float>(static_cast<int>(rng() % 2001) - 1000) / 1000.0f : 0.0f;
        l[i] = x;
        r[i] = -x;
      }
      fx->process(l.data(), r.data(), 64);
      for (std::size_t i = 0; i < 64; ++i) {
        REQUIRE(std::isfinite(l[i]));
        REQUIRE(std::isfinite(r[i]));
        REQUIRE(std::fabs(l[i]) < 64.0f);
      }
    }
    fx->reset();
  }
}

TEST_CASE("sweep: random MIDI bytes, clock and transport included, never break the module") {
  Rig r;
  Session& s = *r.session;
  for (int t = 0; t < kTrackCount; ++t) {
    s.setEngine(t, kEngines[1 + t % 6]);
    s.setChannel(t, t);
  }
  s.toggleProgram(syx::initVoice(), "init");
  s.setProgramChange(true);
  s.setModTarget(0, 0, ParamRef{0, static_cast<uint16_t>(kFader)});
  s.setParam(routeRef(0, 0, kRouteSource), kSrcEnv1);
  s.setParam(routeRef(0, 0, kRouteDepth), 0.5f);
  s.setParam(lfoRef(0, 0, kLfoSync), 1);
  std::mt19937 rng(41);
  std::string error;
  RecSetup& rec = s.recSetup();
  rec.source = RecSource::Master;
  rec.bars = 1;
  REQUIRE(s.arm(error));  // a take running through all of it
  for (int i = 0; i < 6000; ++i) {
    MidiMessage m{};
    const int kind = static_cast<int>(rng() % 10);
    if (kind == 0) m = {0xF8, 0, 0};
    else if (kind == 1) m = {static_cast<uint8_t>(0xFA + rng() % 3), 0, 0};
    else if (kind == 2) m = {0xF2, static_cast<uint8_t>(rng() % 128), static_cast<uint8_t>(rng() % 128)};
    else m = {static_cast<uint8_t>(0x80 + rng() % 0x70), static_cast<uint8_t>(rng() % 128), static_cast<uint8_t>(rng() % 128)};
    s.sendMidi(m);
    if (i % 20 == 0) r.run(1);
  }
  s.panic();
  r.run(8);
  s.cancelTake();
  r.run(4);
  CHECK(s.recState() != Session::RecState::Recording);
}

TEST_CASE("sweep: random keys and clicks on every screen, at every size") {
  Rig r;
  Session& s = *r.session;
  // Something on every track, so every screen has something to show.
  for (int t = 0; t < kTrackCount; ++t) s.setEngine(t, kEngines[1 + t % 6]);
  s.setPadSample(1, 0, r.samples[0]);
  s.setPadSample(2, 0, r.samples[1]);
  s.waitForSamples();
  r.run(2);
  const ui::ScreenId screens[] = {ui::ScreenId::Tracks, ui::ScreenId::Instrument, ui::ScreenId::Mixer, ui::ScreenId::Fx,
                                  ui::ScreenId::Rec,    ui::ScreenId::Library,    ui::ScreenId::Settings, ui::ScreenId::Mod};
  // Keys that don't leave the test's own folders: no import (i) of real paths, no quit confirm loops.
  const std::vector<std::string> keys = {"Up", "Down", "Left", "Right", "Return", "Tab", "TabReverse", "Escape", "Backspace",
                                         "1", "2", "3", "4", "5", "6", "7", "8", "T", "I", "O", "M", "F", "R", "L",
                                         "a", "b", "c", "e", "f", "g", "k", "m", "n", "o", "r", "s", "t", "u", "v", "x", "z",
                                         "[", "]", "/", "*", "+", " ", "?", "~", "`", "^Z", "^Y", "l", "p"};
  const int sizes[][2] = {{60, 35}, {80, 24}, {130, 36}};
  std::mt19937 rng(51);
  for (int run = 0; run < 60; ++run) {
    const auto& sz = sizes[run % 3];
    std::vector<std::string> seq;
    for (int i = 0; i < 25; ++i) {
      const int what = static_cast<int>(rng() % 10);
      const int x = static_cast<int>(rng() % static_cast<unsigned>(sz[0])), y = static_cast<int>(rng() % static_cast<unsigned>(sz[1]));
      if (what == 0) seq.push_back("click:" + std::to_string(x) + "," + std::to_string(y));
      else if (what == 1) seq.push_back((rng() % 2 ? "wheelup:" : "wheeldown:") + std::to_string(x) + "," + std::to_string(y));
      else if (what == 2) seq.push_back("dclick:" + std::to_string(x) + "," + std::to_string(y));
      else if (what == 3) seq.push_back("drag:" + std::to_string(x) + "," + std::to_string(y) + "," + std::to_string(rng() % static_cast<unsigned>(sz[0])));
      else seq.push_back(keys[rng() % keys.size()]);
    }
    const std::string frame = ui::snapshot(s, *r.module, screens[run % 8], sz[0], sz[1], false, false, static_cast<int>(rng() % 8), seq);
    CHECK(frame.size() > static_cast<std::size_t>(sz[0]));
    s.commitEdit();
    r.run(1);
  }
  // And everything it did can be undone.
  int steps = 0;
  while (s.canUndo() && steps < 2000) {
    s.undo();
    ++steps;
  }
  r.run(2);
  CHECK_FALSE(s.canUndo());
}

// ---- regressions: one test per bug the sweep found ----

TEST_CASE("regression: removing the master's glue compressor survives save and load") {
  Rig r;
  Session& s = *r.session;
  REQUIRE(s.insert(kMasterScope, 0) == EffectType::Compressor);
  s.setInsert(kMasterScope, 0, EffectType::None);
  Rig r2;
  std::string error;
  REQUIRE(r2.session->fromJson(s.toJson(), error));
  CHECK(r2.session->insert(kMasterScope, 0) == EffectType::None);
  // A file from before inserts existed (format 3) still gets the default.
  REQUIRE(r2.session->fromJson(R"({"format": 3, "tracks": []})", error));
  CHECK(r2.session->insert(kMasterScope, 0) == EffectType::Compressor);
}

TEST_CASE("regression: FM stealing a voice when every one is busy (Synth_Dexed heap overflow)") {
  FmEngine e;
  e.setParam(FmEngine::kPoly, 2);
  std::vector<float> l(64), r(64);
  for (int n = 0; n < 40; ++n) {
    e.noteOn(static_cast<uint8_t>(40 + n), 100);  // never released: every note past two steals
    e.render(l.data(), r.data(), 64);
  }
  for (float s : l) CHECK(std::isfinite(s));
}

TEST_CASE("regression: Plaits' wavetable model at the top of every macro (read past its wave map)") {
  PlaitsEngine e;
  e.setParam(PlaitsEngine::kModel, 5);  // Wavetable
  e.setParam(PlaitsEngine::kHarmonics, 1);
  e.setParam(PlaitsEngine::kTimbre, 1);
  e.setParam(PlaitsEngine::kMorph, 1);
  e.noteOn(60, 100);
  std::vector<float> l(64), r(64);
  for (int b = 0; b < 400; ++b) {
    e.render(l.data(), r.data(), 64);
    for (float s : l) REQUIRE(std::isfinite(s));
  }
}

TEST_CASE("regression: shortening the glide time mid-glide (CHIP's pitch ran to infinity)") {
  ChipEngine e;
  e.setParam(ChipEngine::kGlide, 2000);
  e.noteOn(40, 100);
  std::vector<float> l(64), r(64);
  e.render(l.data(), r.data(), 64);
  e.noteOn(80, 100);  // gliding up over two seconds…
  e.render(l.data(), r.data(), 64);
  e.setParam(ChipEngine::kGlide, 0.1f);  // …and glide switched off part way
  for (int b = 0; b < 100; ++b) {
    std::fill(l.begin(), l.end(), 0.0f);
    e.render(l.data(), r.data(), 64);
    for (float s : l) REQUIRE(std::isfinite(s));
  }
}

TEST_CASE("sweep-debug: find the redo step that diverges" * doctest::skip(std::getenv("ION_SWEEP_SEED") == nullptr)) {
  const unsigned seed = static_cast<unsigned>(std::atoi(std::getenv("ION_SWEEP_SEED")));
  Rig r;
  Session& s = *r.session;
  std::mt19937 rng(seed);
  r.run();
  std::vector<std::string> states = {doc(s)};
  std::vector<std::string> labels = {""};
  for (int i = 0; i < 250; ++i) {
    randomEdit(r, rng);
    const int before = s.undoSteps();
    s.commitEdit();
    if (s.undoSteps() != before) {
      states.push_back(doc(s));
      labels.push_back(s.undoLabel());
    } else if (!states.empty()) {
      states.back() = doc(s);  // merged into the last step
    }
    if (i % 5 == 0) randomPlay(r, rng);
    if (i % 3 == 0) r.run(1);
  }
  s.sendMidi({0xB0, 123, 0});
  s.panic();
  r.run(4);
  s.waitForSamples();
  r.run(2);
  states.back() = doc(s);
  int steps = 0;
  while (s.canUndo()) {
    s.undo();
    if (++steps % 7 == 0) r.run(1);
  }
  r.run(2);
  if (doc(s) != states[0]) MESSAGE("undo-all doesn't reach the start");
  for (std::size_t k = 1; k < states.size(); ++k) {
    s.redo();
    r.run(1);
    if (doc(s) != states[k]) {
      const auto a = doc(s), b = states[k];
      std::size_t at = 0;
      while (at < a.size() && at < b.size() && a[at] == b[at]) ++at;
      MESSAGE("got:  …" << a.substr(at > 80 ? at - 80 : 0, 200));
      MESSAGE("want: …" << b.substr(at > 80 ? at - 80 : 0, 200));
      MESSAGE("diverges at redo " << k << ": " << labels[k]);
      MESSAGE("previous step: " << labels[k - 1]);
      FAIL("");
    }
  }
}

TEST_CASE("regression: redo of moving a switched-off effect keeps it off") {
  Rig r;
  Session& s = *r.session;
  // The master's glue compressor is in slot 1, switched off. Move it to slot 2.
  REQUIRE(s.param(insertOnRef(kMasterScope, 0)) == 0.0f);
  s.moveInsert(kMasterScope, 0, 1);
  s.commitEdit();
  CHECK(s.param(insertOnRef(kMasterScope, 1)) == 0.0f);
  s.undo();
  r.run(2);  // the audio thread echoes the emptied slot's reset back
  s.redo();
  r.run(2);
  CHECK(s.insert(kMasterScope, 1) == EffectType::Compressor);
  CHECK(s.param(insertOnRef(kMasterScope, 1)) == 0.0f);
}

TEST_CASE("regression: the sampler's 24 dB low-pass at full resonance stays near the 12 dB one's peak") {
  auto peakWith = [](int filter) {
    SamplerEngine e;
    Sample smp;
    smp.channels = 1;
    smp.frames = 48000;
    smp.data.assign(static_cast<std::size_t>(smp.frames + 2 * Sample::kGuard), 0.0f);
    std::mt19937 rng(7);
    for (int64_t i = 0; i < smp.frames; ++i) smp.data[static_cast<std::size_t>(i + Sample::kGuard)] = static_cast<float>(static_cast<int>(rng() % 2001) - 1000) / 2000.0f;
    e.setSample(0, &smp);
    e.setParam(SamplerEngine::kFilter, static_cast<float>(filter));
    e.setParam(SamplerEngine::kReso, 1);
    e.setParam(SamplerEngine::kCutoff, 800);
    e.noteOn(60, 127);
    std::vector<float> l(64), r(64);
    float pk = 0;
    for (int b = 0; b < 600; ++b) {
      std::fill(l.begin(), l.end(), 0.0f);
      e.render(l.data(), r.data(), 64);
      for (float v : l) pk = std::max(pk, std::fabs(v));
    }
    return pk;
  };
  CHECK(peakWith(SamplerEngine::kLp24) < 2.0f * peakWith(SamplerEngine::kLp12));
}

TEST_CASE("regression: Plaits' speech model, struck at every address (read past the phoneme table)") {
  PlaitsEngine e;
  e.setParam(PlaitsEngine::kModel, 7);  // Speech
  e.setParam(PlaitsEngine::kHarmonics, 0.1f);  // the LPC vowels and consonants
  std::vector<float> l(64), r(64);
  for (int i = 0; i <= 40; ++i) {
    e.setParam(PlaitsEngine::kMorph, static_cast<float>(i) / 40.0f);
    e.setParam(PlaitsEngine::kTimbre, static_cast<float>(i % 7) / 6.0f);
    e.noteOn(static_cast<uint8_t>(30 + i), 100);
    for (int b = 0; b < 20; ++b) {
      e.render(l.data(), r.data(), 64);
      for (float v : l) REQUIRE(std::isfinite(v));
    }
    e.noteOff(static_cast<uint8_t>(30 + i));
  }
}

TEST_CASE("regression: Plaits' speech model, moving from words back to vowels mid-word") {
  PlaitsEngine e;
  e.setParam(PlaitsEngine::kModel, 7);  // Speech
  std::vector<float> l(64), r(64);
  for (int i = 0; i < 60; ++i) {
    e.setParam(PlaitsEngine::kHarmonics, 0.95f);  // a word bank
    e.setParam(PlaitsEngine::kMorph, static_cast<float>(i % 10) / 9.0f);
    e.noteOn(static_cast<uint8_t>(40 + i % 20), 100);
    for (int b = 0; b < 1 + i % 9; ++b) e.render(l.data(), r.data(), 64);
    e.setParam(PlaitsEngine::kHarmonics, 0.3f);  // back to the vowels, mid-word
    for (int b = 0; b < 4; ++b) {
      e.render(l.data(), r.data(), 64);
      for (float v : l) REQUIRE(std::isfinite(v));
    }
    e.noteOff(static_cast<uint8_t>(40 + i % 20));
  }
}

TEST_CASE("regression: Plaits' 6-op FM at full velocity (read past its velocity table)") {
  PlaitsEngine e;
  e.setParam(PlaitsEngine::kModel, 18);  // 6-op FM a
  e.setParam(PlaitsEngine::kVelLpg, 1);
  std::vector<float> l(64), r(64);
  for (int i = 0; i < 8; ++i) {
    e.noteOn(static_cast<uint8_t>(48 + i), 127);
    for (int b = 0; b < 30; ++b) e.render(l.data(), r.data(), 64);
  }
  for (float v : l) CHECK(std::isfinite(v));
}
