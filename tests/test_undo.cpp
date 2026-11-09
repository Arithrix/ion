// Undo and redo across the whole project (DECISIONS D-54).

#include <doctest.h>

#include <filesystem>
#include <memory>
#include <random>

#include "engine/fm/fm_engine.h"
#include "engine/fx/effects.h"
#include "engine/module.h"
#include "project/config.h"
#include "project/session.h"

using namespace ion;
namespace fs = std::filesystem;

namespace {

struct TempRoot {
  fs::path path;
  TempRoot() {
    path = fs::temp_directory_path() / ("ion-undo-" + std::to_string(std::random_device{}()));
    fs::create_directories(path);
  }
  ~TempRoot() { fs::remove_all(path); }
};

struct Rig {
  TempRoot root;
  std::unique_ptr<Module> module = std::make_unique<Module>();
  Config config;
  Session session{*module, Paths::under(root.path), config};
  std::vector<float> buf = std::vector<float>(2 * 256);
  Rig() { session.newProject("undo"); }
  void run(int blocks = 4) {
    for (int i = 0; i < blocks; ++i) module->process(buf.data(), 256);
    session.tick();
  }
  // What the audio thread has, which is what you hear.
  float live(ParamRef ref) const { return module->params().get(ref); }
};

syx::Voice voiceNamed(const char* name, uint8_t algorithm) {
  syx::Voice v{};
  for (int op = 0; op < 6; ++op) v[static_cast<std::size_t>(op * 21 + 18)] = 1;
  v[134] = algorithm;
  std::string n = name;
  n.resize(10, ' ');
  std::copy(n.begin(), n.end(), v.begin() + 145);
  return v;
}

constexpr ParamRef fader(int t) { return {static_cast<uint8_t>(t), static_cast<uint16_t>(kFader)}; }
constexpr ParamRef mute(int t) { return {static_cast<uint8_t>(t), static_cast<uint16_t>(kMute)}; }

}  // namespace

TEST_CASE("undo: a fresh project has nothing to undo") {
  Rig r;
  CHECK_FALSE(r.session.canUndo());
  CHECK_FALSE(r.session.canRedo());
  CHECK(r.session.undo().empty());
  CHECK(r.session.redo().empty());
}

TEST_CASE("undo: holding a value's key is one step; undo and redo reach the audio") {
  Rig r;
  Session& s = r.session;
  const float before = s.param(fader(2));
  for (int i = 0; i < 6; ++i) {
    s.nudgeParam(fader(2), 1, false);
    s.commitEdit();  // the UI commits after every key
  }
  const float after = s.param(fader(2));
  REQUIRE(after != before);
  CHECK(s.undoSteps() == 1);
  CHECK(s.undoLabel() == "Track 3 · Fader");

  CHECK(s.undo() == "Track 3 · Fader");
  r.run();
  CHECK(s.param(fader(2)) == before);
  CHECK(r.live(fader(2)) == before);
  CHECK(s.redoLabel() == "Track 3 · Fader");

  CHECK(s.redo() == "Track 3 · Fader");
  r.run();
  CHECK(s.param(fader(2)) == after);
  CHECK(r.live(fader(2)) == after);
}

TEST_CASE("undo: switches aren't merged; a new edit clears redo") {
  Rig r;
  Session& s = r.session;
  s.nudgeParam(mute(0), 1, false);
  s.commitEdit();
  s.nudgeParam(mute(0), 1, false);
  s.commitEdit();
  CHECK(s.undoSteps() == 2);
  s.undo();
  CHECK(s.param(mute(0)) == 1.0f);
  CHECK(s.canRedo());
  s.setParam(fader(0), -6);
  s.commitEdit();
  CHECK_FALSE(s.canRedo());
  CHECK(s.undoSteps() == 2);
}

TEST_CASE("undo: an engine change comes back with its sound and settings") {
  Rig r;
  Session& s = r.session;
  s.setEngine(0, EngineType::Fm);
  s.setVoice(0, voiceNamed("BRASS 1", 21), "test");
  s.commitEdit();
  const ParamRef bright = engineParamRef(0, FmEngine::kFirstMacro);
  s.setParam(bright, 10);
  s.commitEdit();
  s.setEngine(0, EngineType::Drum);
  s.commitEdit();
  r.run();
  CHECK(s.undoLabel() == "Track 1 · engine DRUM");

  CHECK(s.undo() == "Track 1 · engine DRUM");
  r.run();
  CHECK(s.track(0).engine == EngineType::Fm);
  REQUIRE(s.track(0).hasVoice);
  CHECK(syx::name(s.track(0).voice) == "BRASS 1");
  CHECK(s.param(bright) == 10.0f);
  CHECK(r.live(bright) == 10.0f);

  s.undo();  // the macro
  r.run();
  CHECK(s.param(bright) == 0.0f);
  CHECK(s.undo() == "Track 1 · engine FM");  // the patch was part of the same step
  r.run();
  CHECK(s.track(0).engine == EngineType::None);
  CHECK_FALSE(s.track(0).hasVoice);

  s.redo();
  s.redo();
  s.redo();
  r.run();
  CHECK(s.track(0).engine == EngineType::Drum);
  CHECK_FALSE(s.canRedo());
}

TEST_CASE("undo: inserts and their settings") {
  Rig r;
  Session& s = r.session;
  s.setInsert(3, 1, EffectType::Compressor);
  s.commitEdit();
  const ParamRef ratio = insertParamRef(3, 1, Compressor::kRatio);
  s.setParam(ratio, 8);
  s.commitEdit();
  s.moveInsert(3, 1, -1);
  s.commitEdit();
  CHECK(s.insert(3, 0) == EffectType::Compressor);
  CHECK(s.param(insertParamRef(3, 0, Compressor::kRatio)) == 8.0f);

  s.undo();  // the move
  CHECK(s.insert(3, 0) == EffectType::None);
  CHECK(s.insert(3, 1) == EffectType::Compressor);
  CHECK(s.param(ratio) == 8.0f);
  s.undo();  // the ratio
  CHECK(s.param(ratio) != 8.0f);
  s.undo();  // the insert
  r.run();
  CHECK(s.insert(3, 1) == EffectType::None);
  s.redo();
  s.redo();
  r.run();
  CHECK(s.insert(3, 1) == EffectType::Compressor);
  CHECK(s.param(ratio) == 8.0f);
  CHECK(r.live(ratio) == 8.0f);
}

TEST_CASE("undo: CC moves from the OXI aren't steps, and undo leaves them alone") {
  Rig r;
  Session& s = r.session;
  s.addMapping(0, 74, fader(1));
  s.commitEdit();
  s.setParam(fader(0), -12);
  s.commitEdit();
  const int steps = s.undoSteps();

  s.sendMidi({0xB0, 74, 20});  // the OXI moves track 2's fader
  r.run();
  const float moved = s.param(fader(1));
  CHECK(moved != mixerParam(kFader).def);
  CHECK(s.undoSteps() == steps);

  CHECK(s.undo() == "Track 1 · Fader");
  r.run();
  CHECK(s.param(fader(0)) == mixerParam(kFader).def);
  CHECK(s.param(fader(1)) == moved);  // the CC's value stays

  CHECK(s.undo().rfind("CC map: cc 74", 0) == 0);
  CHECK(s.ccMappings().empty());
}

TEST_CASE("undo: the program list") {
  Rig r;
  Session& s = r.session;
  s.toggleProgram(voiceNamed("ONE", 1), "a");
  s.toggleProgram(voiceNamed("TWO", 2), "a");
  s.commitEdit();  // one key press can make one step of several changes
  CHECK(s.undoSteps() == 1);
  s.moveProgram(0, 1);
  s.commitEdit();
  CHECK(syx::name(s.programs()[0].voice) == "TWO");
  s.undo();
  CHECK(syx::name(s.programs()[0].voice) == "ONE");
  s.undo();
  CHECK(s.programs().empty());
}

TEST_CASE("undo: edits without the UI are steps at the next tick; loading clears the history") {
  Rig r;
  Session& s = r.session;
  s.setChannel(4, 9);
  r.run();  // tick commits
  CHECK(s.undoLabel() == "Track 5 · channel 10");
  std::string error;
  REQUIRE(s.saveAs("undo saved", error));
  CHECK(s.canUndo());  // saving keeps it
  REQUIRE(s.load("undo saved", error));
  CHECK_FALSE(s.canUndo());
  CHECK(s.track(4).channel == 9);
}
