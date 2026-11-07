#include <doctest.h>

#include <filesystem>
#include <memory>
#include <random>
#include <thread>

#include "engine/fm/fm_engine.h"
#include "engine/module.h"
#include "project/config.h"
#include "project/session.h"

using namespace ion;
namespace fs = std::filesystem;

namespace {

// A temporary directory standing in for $HOME.
struct TempRoot {
  fs::path path;
  TempRoot() {
    path = fs::temp_directory_path() / ("ion-test-" + std::to_string(std::random_device{}()));
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

  // Let the "audio thread" apply queued commands, then let Session see results.
  void run(int blocks = 4) {
    for (int i = 0; i < blocks; ++i) module->process(buf.data(), 256);
    session.tick();
  }
};

syx::Voice testVoice(const char* name) {
  syx::Voice v{};
  for (int op = 0; op < 6; ++op) {
    v[static_cast<std::size_t>(op * 21 + 4)] = 99;
    v[static_cast<std::size_t>(op * 21 + 18)] = 1;
  }
  v[5 * 21 + 16] = 99;
  v[134] = 31;
  v[144] = 24;
  std::string n = name;
  n.resize(10, ' ');
  std::copy(n.begin(), n.end(), v.begin() + 145);
  return v;
}

}  // namespace

TEST_CASE("session: edits reach the engine") {
  Rig r;
  r.session.newProject("a");
  r.session.setEngine(1, EngineType::Fm);
  r.session.setParam({1, kFader}, -12.0f);
  r.run();
  CHECK(r.module->status().trackEngine[1].load() == static_cast<uint8_t>(EngineType::Fm));
  CHECK(r.module->params().get({1, kFader}) == doctest::Approx(-12.0f));
  CHECK(r.session.param({1, kFader}) == doctest::Approx(-12.0f));
  CHECK(r.session.dirty());
}

TEST_CASE("session: nudges never read a stale value") {
  // Without the audio thread running at all, repeated nudges still accumulate.
  Rig r;
  r.session.newProject("a");
  for (int i = 0; i < 4; ++i) r.session.nudgeParam({0, kPan}, 1, false);
  CHECK(r.session.param({0, kPan}) == doctest::Approx(0.2f));
}

TEST_CASE("session: save, load and the file round-trip") {
  Rig r;
  r.session.newProject("song one");
  r.session.setEngine(2, EngineType::Fm);
  r.session.setVoice(2, testVoice("TEST EP"), "unit");
  r.session.setChannel(2, 9);
  r.session.setParam({2, kFader}, -3.5f);
  r.session.setParam({2, kB1Gain}, 4.0f);
  r.session.setParam({2, static_cast<uint16_t>(kEngineParamBase + FmEngine::kPoly)}, 12.0f);
  r.session.setParam({kMasterScope, kFader}, -1.0f);
  r.run();

  std::string error;
  REQUIRE(r.session.save(error));
  CHECK_FALSE(r.session.dirty());
  CHECK(fs::exists(r.session.paths().projectFile("song one")));
  const std::string first = r.session.toJson();

  // A fresh session loads the same project.
  Rig r2;
  r2.session.newProject("x");
  r2.run();
  std::string text;
  REQUIRE(readFile(r.session.paths().projectFile("song one"), text));
  REQUIRE(r2.session.fromJson(text, error));
  r2.run();
  CHECK(r2.session.track(2).engine == EngineType::Fm);
  CHECK(r2.session.track(2).channel == 9);
  CHECK(syx::name(r2.session.track(2).voice) == "TEST EP");
  CHECK(r2.session.param({2, kFader}) == doctest::Approx(-3.5f));
  CHECK(r2.module->params().get({2, kB1Gain}) == doctest::Approx(4.0f));
  CHECK(r2.module->params().get({2, static_cast<uint16_t>(kEngineParamBase + FmEngine::kPoly)}) == doctest::Approx(12.0f));
  CHECK(r2.session.param({kMasterScope, kFader}) == doctest::Approx(-1.0f));

  // Saving it again gives the same document (apart from the timestamp).
  auto withoutTime = [](std::string s) {
    const auto at = s.find("\"saved_at\"");
    return s.erase(at, s.find('\n', at) - at);
  };
  CHECK(withoutTime(r2.session.toJson()) == withoutTime(first));
}

TEST_CASE("session: project files are versioned and checked") {
  Rig r;
  std::string error;
  CHECK_FALSE(r.session.fromJson("{\"tracks\": []}", error));
  CHECK(error.find("format") != std::string::npos);
  CHECK_FALSE(r.session.fromJson("{\"format\": 99}", error));
  CHECK(error.find("newer") != std::string::npos);
  CHECK_FALSE(r.session.fromJson("not json", error));
  // Unknown keys and missing sections are fine.
  CHECK(r.session.fromJson("{\"format\": 1, \"future_thing\": 3}", error));
}

TEST_CASE("session: autosave and recovery") {
  Rig r;
  r.config.autosaveMinutes = 1;
  r.session.newProject("draft");
  std::string error;
  REQUIRE(r.session.save(error));
  CHECK_FALSE(r.session.hasRecovery("draft"));

  r.session.setParam({0, kPan}, 0.5f);
  r.run();
  r.session.autosaveIfNeeded();  // what a clean exit does
  CHECK(fs::exists(r.session.paths().autosaveFile("draft")));
  // Make sure the autosave is strictly newer than the project file.
  fs::last_write_time(r.session.paths().autosaveFile("draft"),
                      fs::last_write_time(r.session.paths().projectFile("draft")) + std::chrono::seconds(2));
  CHECK(r.session.hasRecovery("draft"));

  // A second session over the same files, as after a crash and restart.
  Rig r2;
  Session s3(*r2.module, r.session.paths(), r2.config);
  REQUIRE(s3.recover("draft", error));
  r2.run();
  CHECK(s3.param({0, kPan}) == doctest::Approx(0.5f));
  CHECK(s3.dirty());  // recovered changes are unsaved
  REQUIRE(s3.save(error));
  CHECK_FALSE(fs::exists(r.session.paths().autosaveFile("draft")));
}

TEST_CASE("session: an unsaved sketch comes back after quitting") {
  Rig r;
  r.session.newProject("sketch x");
  r.session.setEngine(0, EngineType::Fm);
  r.session.setParam({0, kFader}, -7.0f);
  r.run();
  r.session.autosaveIfNeeded();  // quit without saving
  // Next start: main() opens the last project; there's no project.json, only
  // the autosave, so recovery is offered.
  CHECK(r.session.lastProject() == "sketch x");
  CHECK(r.session.hasRecovery("sketch x"));
  CHECK(r.session.listProjects() == std::vector<std::string>{"sketch x"});
  Rig r2;
  Session s2(*r2.module, r.session.paths(), r2.config);
  std::string error;
  s2.newProject("sketch x");
  REQUIRE(s2.recover("sketch x", error));
  CHECK(s2.track(0).engine == EngineType::Fm);
  CHECK(s2.param({0, kFader}) == doctest::Approx(-7.0f));
}

TEST_CASE("session: CC learn end to end") {
  Rig r;
  r.session.newProject("cc");
  r.session.setEngine(3, EngineType::Fm);
  r.session.learn({3, kB1Gain});
  r.run();
  r.module->pushMidi({{0xB3, 74, 64}});
  r.run();
  CHECK_FALSE(r.session.learning());
  const auto maps = r.session.ccMappings();
  REQUIRE(maps.size() == 1);
  CHECK(maps[0].mapping.cc == 74);
  CHECK(maps[0].mapping.channel == 3);
  CHECK(r.session.takeMessage().find("cc 74") != std::string::npos);

  // Now the CC drives the parameter, and the Session sees it.
  r.module->pushMidi({{0xB3, 74, 127}});
  r.run();
  CHECK(r.session.param({3, kB1Gain}) == doctest::Approx(18.0f));
  CHECK(r.session.dirty());

  // The mapping is saved with the project and comes back.
  const std::string json = r.session.toJson();
  CHECK(json.find("track4.eq.b1.gain") != std::string::npos);
  Rig r2;
  std::string error;
  REQUIRE(r2.session.fromJson(json, error));
  r2.run();
  REQUIRE(r2.session.ccMappings().size() == 1);
  r2.module->pushMidi({{0xB3, 74, 0}});
  r2.run();
  CHECK(r2.session.param({3, kB1Gain}) == doctest::Approx(-18.0f));
}

TEST_CASE("session: parameter keys parse back") {
  Rig r;
  r.session.setEngine(0, EngineType::Fm);
  for (ParamRef ref : {ParamRef{0, kFader}, ParamRef{kMasterScope, kFader}, ParamRef{5, kHpSlope},
                       ParamRef{0, static_cast<uint16_t>(kEngineParamBase + FmEngine::kBendRange)}}) {
    const auto back = r.session.parseParamKey(r.session.paramKey(ref));
    REQUIRE(back);
    CHECK(*back == ref);
  }
  CHECK_FALSE(r.session.parseParamKey("track9.fader"));
  CHECK_FALSE(r.session.parseParamKey("track1.nonsense"));
}

TEST_CASE("session: names") {
  CHECK(validProjectName("garage sketch 03"));
  CHECK(validProjectName("ukg_chops-02"));
  CHECK_FALSE(validProjectName("../etc"));
  CHECK_FALSE(validProjectName(""));
  CHECK_FALSE(validProjectName(" padded"));
  Rig r;
  std::string error;
  r.session.newProject("one");
  REQUIRE(r.session.save(error));
  r.session.newProject("two");
  CHECK_FALSE(r.session.saveAs("one", error));  // won't overwrite another project
  CHECK(r.session.saveAs("three", error));
  CHECK(r.session.listProjects().size() == 2);
}

TEST_CASE("config: round-trip and bad values") {
  TempRoot root;
  const fs::path file = root.path / "config.toml";
  Config c;
  c.latencyFrames = 256;
  c.midiInput = "OXI One";
  c.timing = MidiTiming::Immediate;
  c.gateMs = 450;
  std::string error;
  REQUIRE(c.save(file, error));
  std::vector<std::string> warnings;
  const Config back = Config::load(file, warnings);
  CHECK(warnings.empty());
  CHECK(back.latencyFrames == 256);
  CHECK(back.midiInput == "OXI One");
  CHECK(back.timing == MidiTiming::Immediate);
  CHECK(back.gateMs == 450);

  REQUIRE(writeFileAtomic(file, "[audio]\nlatency = 7\n[midi\n", error));
  const Config broken = Config::load(file, warnings);
  CHECK_FALSE(warnings.empty());
  CHECK(broken.latencyFrames == 128);
}

// ---- FM library, macros, program list (Phase 2) ----

#include <fstream>

#include "library/fm_library.h"

namespace {

// Writes a generated bank into the rig's root and imports it, as a user would.
void importBank(Rig& r, const std::vector<syx::Voice>& voices) {
  const auto bank = syx::makeBanks(voices).at(0);
  const fs::path file = r.root.path / "syx" / "test_bank.syx";
  fs::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(bank.data()), static_cast<std::streamsize>(bank.size()));
  const Paths paths = Paths::under(r.root.path);
  REQUIRE(FmLibrary::import(paths.libraryFile(), file.parent_path(), paths.importLog()).error.empty());
  std::string error;
  REQUIRE(r.session.openLibrary(error));
}

ParamRef fmParam(int track, int p) { return {static_cast<uint8_t>(track), static_cast<uint16_t>(kEngineParamBase + p)}; }

}  // namespace

TEST_CASE("session: loading a library patch links it; macros and the program list survive save/load") {
  Rig r;
  r.session.newProject("fm");
  // testVoice() only sets the name, so BETA gets a different feedback to be a different voice.
  syx::Voice beta = testVoice("BETA");
  beta[135] = 5;
  importBank(r, {testVoice("ALPHA"), beta});
  FmLibrary& lib = *r.session.library();
  REQUIRE(lib.patches().size() == 3);  // + the INIT VOICE padding the bank

  r.session.setEngine(0, EngineType::Fm);
  const FmPatch* b = nullptr;
  for (const FmPatch& p : lib.patches())
    if (p.name == "BETA") b = &p;
  REQUIRE(b);
  const int64_t betaId = b->id;
  r.session.loadPatch(0, *b);
  CHECK(r.session.track(0).libraryId == betaId);
  r.session.setParam(fmParam(0, FmEngine::kBright), 10.0f);
  r.session.setParam(fmParam(0, FmEngine::kFeedback), -2.0f);
  CHECK(r.session.macros(0) == dx7::Macros{.bright = 10, .feedback = -2});
  CHECK(r.session.toggleProgram(r.session.track(0).voice, "test_bank"));
  CHECK(r.session.programIndex(beta) == 0);
  std::string error;
  REQUIRE(r.session.save(error));

  // A second run of ion on the same "home".
  auto module2 = std::make_unique<Module>();
  Config config2;
  Session s2(*module2, Paths::under(r.root.path), config2);
  REQUIRE(s2.openLibrary(error));
  REQUIRE(s2.load("fm", error));
  CHECK(s2.track(0).libraryId == betaId);  // found again by its parameters
  CHECK(s2.macros(0) == dx7::Macros{.bright = 10, .feedback = -2});
  REQUIRE(s2.programs().size() == 1);
  CHECK(s2.programs()[0].voice == syx::normalize(beta));
}

TEST_CASE("session: save as bakes the macros into a new library voice that sounds the same") {
  Rig r;
  r.session.newProject("bake");
  importBank(r, {testVoice("PAD")});
  r.session.setEngine(0, EngineType::Fm);
  const FmPatch* pad = nullptr;
  for (const FmPatch& p : r.session.library()->patches())
    if (p.name == "PAD") pad = &p;
  REQUIRE(pad);
  r.session.loadPatch(0, *pad);
  r.session.setParam(fmParam(0, FmEngine::kRelease), 20.0f);
  r.session.setParam(fmParam(0, FmEngine::kFeedback), 3.0f);
  const syx::Voice expected = dx7::applyMacros(r.session.track(0).voice, r.session.macros(0));

  std::string error;
  const int64_t id = r.session.savePatchAs(0, "MY PAD", error);
  REQUIRE(id > 0);
  CHECK_FALSE(r.session.macros(0).any());      // baked: the macros are back at 0…
  const syx::Voice now = r.session.track(0).voice;
  CHECK(std::equal(now.begin(), now.begin() + 145, expected.begin()));  // …and the voice carries them
  CHECK(syx::name(now) == "MY PAD");
  CHECK(r.session.track(0).libraryId == id);
  const FmPatch* p = r.session.library()->find(id);
  REQUIRE(p);
  CHECK(p->user);
  CHECK(r.session.library()->patches().size() == 3);  // PAD, INIT VOICE (bank padding), MY PAD

}

TEST_CASE("session: export writes valid 32-voice banks and never overwrites") {
  Rig r;
  r.session.newProject("x");
  std::vector<syx::Voice> voices;
  for (int i = 0; i < 40; ++i) {
    syx::Voice v = testVoice(("V" + std::to_string(i)).c_str());
    v[135] = static_cast<uint8_t>(i % 8);
    v[137] = static_cast<uint8_t>(i);
    voices.push_back(v);
  }
  std::vector<fs::path> written;
  std::string error;
  REQUIRE(r.session.exportVoices(voices, "my set", written, error));
  REQUIRE(written.size() == 2);
  CHECK(written[0].filename() == "my set 1.syx");
  for (const fs::path& f : written) CHECK(fs::file_size(f) == syx::kBulkDumpSize);
  // Importing the export gives back the same voices.
  const Paths paths = Paths::under(r.root.path);
  const ImportReport rep = FmLibrary::import(paths.libraryFile(), paths.exportsDir, paths.importLog());
  CHECK(rep.corrupt == 0);
  CHECK(rep.voices == 64);
  CHECK(rep.added == 41);  // 40 + one INIT VOICE padding the second bank
  written.clear();
  CHECK_FALSE(r.session.exportVoices(voices, "my set", written, error));
  CHECK(error.find("already exists") != std::string::npos);
}

TEST_CASE("session: program change on a track's channel loads from the program list") {
  Rig r;
  r.session.newProject("pc");
  r.session.setEngine(0, EngineType::Fm);
  r.session.setEngine(1, EngineType::Fm);  // channel 2: not addressed
  syx::Voice a = testVoice("PROG ONE"), b = testVoice("PROG TWO");
  b[135] = 6;
  r.session.toggleProgram(a, "x");
  r.session.toggleProgram(b, "x");
  r.run();
  r.session.sendMidi({0xC0, 1, 0});  // program 2 on channel 1
  r.run();
  CHECK(syx::name(r.session.track(0).voice) == "PROG TWO");
  CHECK_FALSE(r.session.track(1).hasVoice);
  CHECK(r.session.takeMessage().find("program 2") != std::string::npos);

  r.session.sendMidi({0xC0, 7, 0});  // beyond the list: ignored
  r.run();
  CHECK(syx::name(r.session.track(0).voice) == "PROG TWO");

  r.session.setProgramChange(false);
  r.run();
  r.session.sendMidi({0xC0, 0, 0});
  r.run();
  CHECK(syx::name(r.session.track(0).voice) == "PROG TWO");  // off: ignored
}

TEST_CASE("session: a format 1 project (v0.1.0) still opens") {
  Rig r;
  const std::string v1 = R"({
  "format": 1,
  "saved_by": "ion 0.1.0",
  "tracks": [ { "track": 1, "engine": "fm", "channel": 1, "mixer": { "fader": -3.0 },
                "fm": { "params": { "poly": 6, "bend": 2, "mono": 0 }, "name": "OLD" } } ],
  "master": { "fader": 0.0 },
  "cc": []
})";
  std::string error;
  REQUIRE(writeFileAtomic(Paths::under(r.root.path).projectFile("old"), v1, error));
  REQUIRE(r.session.load("old", error));
  CHECK(r.session.track(0).engine == EngineType::Fm);
  CHECK(r.session.param(fmParam(0, FmEngine::kPoly)) == doctest::Approx(6.0f));
  CHECK_FALSE(r.session.macros(0).any());
  CHECK(r.session.programs().empty());
  // Saved again, it's the current format.
  REQUIRE(r.session.save(error));
  std::string text;
  REQUIRE(readFile(Paths::under(r.root.path).projectFile("old"), text));
  CHECK(text.find("\"format\": " + std::to_string(kProjectFormat)) != std::string::npos);
}
