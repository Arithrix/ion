// Whole flows through the app, driven with keys as Michael would: the engine
// picker, piano mode into the engines, save as and loading from the Library,
// mute and solo, effects in and out, a take armed and cancelled.

#include <doctest.h>

#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "engine/fx/effects.h"
#include "engine/module.h"
#include "project/config.h"
#include "project/session.h"
#include "ui/app.h"

using namespace ion;
namespace fs = std::filesystem;

namespace {

struct AppRig {
  fs::path root = fs::temp_directory_path() / ("ion-app-" + std::to_string(std::random_device{}()));
  std::unique_ptr<Module> module = std::make_unique<Module>();
  Config config;
  std::unique_ptr<Session> session;
  std::vector<float> buf = std::vector<float>(2 * 256);
  AppRig() {
    fs::create_directories(root);
    config.autosaveMinutes = 0;
    session = std::make_unique<Session>(*module, Paths::under(root), config);
    session->newProject("flows");
  }
  ~AppRig() {
    session.reset();
    fs::remove_all(root);
  }
  std::string keys(ui::ScreenId screen, int track, const std::vector<std::string>& k) {
    return ui::snapshot(*session, *module, screen, 130, 36, false, false, track, k);
  }
  void run(int blocks = 8) {
    for (int i = 0; i < blocks; ++i) module->process(buf.data(), 256);
    session->tick();
  }
};

}  // namespace

TEST_CASE("app: ⏎ on an empty track picks an engine; each engine in the picker") {
  const EngineType order[] = {EngineType::Fm, EngineType::Drum, EngineType::Sampler, EngineType::Plaits, EngineType::Acid, EngineType::Chip};
  for (int i = 0; i < 6; ++i) {
    CAPTURE(i);
    AppRig r;
    std::vector<std::string> k = {"Return"};
    for (int d = 0; d < i; ++d) k.push_back("Down");
    k.push_back("Return");
    r.keys(ui::ScreenId::Tracks, 3, k);
    CHECK(r.session->track(3).engine == order[i]);
  }
}

TEST_CASE("app: piano mode plays the selected track's engine") {
  AppRig r;
  r.session->setEngine(0, EngineType::Chip);
  r.run();
  r.keys(ui::ScreenId::Tracks, 0, {"`", "z", "x", "c"});
  r.run(2);
  CHECK(r.module->status().trackVoices[0].load() >= 1);
}

TEST_CASE("app: save as, then load it back from the Library screen") {
  AppRig r;
  Session& s = *r.session;
  s.setEngine(2, EngineType::Acid);
  r.keys(ui::ScreenId::Tracks, 0, {"W", "Backspace", "Backspace", "Backspace", "Backspace", "Backspace", "m", "i", "n", "e", "Return"});
  CHECK(s.name() == "mine");
  CHECK(fs::exists(s.paths().projectFile("mine")));
  // Something else, unsaved; then load "mine" from the Library (⏎ twice: unsaved changes).
  s.newProject("other");
  s.setEngine(5, EngineType::Fm);
  const auto names = s.listProjects();
  int at = 0;
  for (int i = 0; i < static_cast<int>(names.size()); ++i)
    if (names[static_cast<std::size_t>(i)] == "mine") at = i;
  std::vector<std::string> k;
  for (int i = 0; i < at; ++i) k.push_back("Down");
  k.push_back("Return");
  k.push_back("Return");
  r.keys(ui::ScreenId::Library, 0, k);
  CHECK(s.name() == "mine");
  CHECK(s.track(2).engine == EngineType::Acid);
  CHECK(s.track(5).engine == EngineType::None);
}

TEST_CASE("app: m and s on Tracks mute and solo the selected track, and undo") {
  AppRig r;
  Session& s = *r.session;
  r.keys(ui::ScreenId::Tracks, 4, {"m", "s"});
  CHECK(s.param({4, static_cast<uint16_t>(kMute)}) == 1.0f);
  CHECK(s.param({4, static_cast<uint16_t>(kSolo)}) == 1.0f);
  r.keys(ui::ScreenId::Tracks, 4, {"^Z", "^Z"});
  CHECK(s.param({4, static_cast<uint16_t>(kMute)}) == 0.0f);
  CHECK(s.param({4, static_cast<uint16_t>(kSolo)}) == 0.0f);
}

TEST_CASE("app: an effect added from the FX screen's picker, then removed") {
  AppRig r;
  Session& s = *r.session;
  // The FX screen opens on slot 1 of the track: a, ↓ ↓ to DELAY (third), ⏎.
  r.keys(ui::ScreenId::Fx, 0, {"a", "Down", "Down", "Return"});
  CHECK(s.insert(0, 0) == EffectType::Delay);
  r.keys(ui::ScreenId::Fx, 0, {"x"});
  CHECK(s.insert(0, 0) == EffectType::None);
}

TEST_CASE("app: r arms a take on the Rec screen; Esc throws it away") {
  AppRig r;
  Session& s = *r.session;
  s.setEngine(0, EngineType::Chip);
  r.keys(ui::ScreenId::Rec, 0, {"r"});
  CHECK(s.recState() == Session::RecState::Armed);  // waiting for the OXI's next bar
  r.keys(ui::ScreenId::Rec, 0, {"Escape"});
  r.run(4);
  CHECK(s.recState() == Session::RecState::Idle);
}

TEST_CASE("app: Settings: the piano's gate length changes from the keys") {
  AppRig r;
  const int before = r.session->config().gateMs;
  // Settings' sections: Audio, MIDI, Piano… Tab to Piano, ↓ to its gate, →.
  const std::string frame = r.keys(ui::ScreenId::Settings, 0, {"Tab", "Tab"});
  bool changed = false;
  for (int down = 0; down < 6 && !changed; ++down) {
    std::vector<std::string> k = {"Tab", "Tab"};
    for (int d = 0; d < down; ++d) k.push_back("Down");
    k.push_back("Right");
    r.keys(ui::ScreenId::Settings, 0, k);
    changed = r.session->config().gateMs != before;
  }
  CHECK(changed);
  (void)frame;
}
