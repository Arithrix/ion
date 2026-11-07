#include <doctest.h>

#include <cstdlib>
#include <filesystem>
#include <memory>

#include "core/version.h"
#include "engine/module.h"
#include "project/config.h"
#include "project/paths.h"
#include "project/session.h"
#include "ui/app.h"
#include "ui/piano.h"

using namespace ion;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---- piano ------------------------------------------------------------------

TEST_CASE("piano: Renoise layout, octaves, velocity") {
  ui::Piano p("zsxdcvgbhnjm,l.;/", "q2w3er5t6y7ui9o0p", 300);
  const auto t0 = Clock::now();
  auto n = p.press('z', t0);
  REQUIRE(n);
  CHECK(n->note == 60);  // C3 at the default octave
  CHECK(n->velocity == 100);
  CHECK(p.press('s', t0)->note == 61);
  CHECK(p.press('/', t0)->note == 76);  // E above the next C: 17 keys
  CHECK(p.press('q', t0)->note == 72);  // upper row starts an octave higher
  CHECK(p.press('P', t0)->note == 88);  // case doesn't matter
  CHECK_FALSE(p.press('a', t0));        // not a piano key
  p.octaveDown();
  CHECK(p.press('x', t0 + std::chrono::seconds(1))->note == 50);
  for (int i = 0; i < 20; ++i) p.octaveUp();
  CHECK(p.octave() == 8);
  CHECK_FALSE(p.press('/', t0 + std::chrono::seconds(2)));  // above 127
  p.cycleVelocity();
  CHECK(p.velocity() == 127);
  p.cycleVelocity();
  CHECK(p.velocity() == 64);
}

TEST_CASE("piano: gate length and auto-repeat") {
  ui::Piano p("zsxdcvgbhnjm,l.;/", "q2w3er5t6y7ui9o0p", 300);
  const auto t0 = Clock::now();
  REQUIRE(p.press('c', t0));
  CHECK(p.expired(t0 + std::chrono::milliseconds(299)).empty());
  const auto off = p.expired(t0 + std::chrono::milliseconds(300));
  REQUIRE(off.size() == 1);
  CHECK(off[0] == 64);
  // Holding the key: X's first repeat (~500 ms) can't be told from a second tap,
  // so it plays once more; the 20 Hz repeats after it are ignored.
  CHECK(p.press('c', t0 + std::chrono::milliseconds(500)));
  for (int i = 1; i < 10; ++i) CHECK_FALSE(p.press('c', t0 + std::chrono::milliseconds(500 + 50 * i)));
  // Fast deliberate taps (100 ms apart) all play.
  const auto t1 = t0 + std::chrono::seconds(2);
  for (int i = 0; i < 4; ++i) CHECK(p.press('v', t1 + std::chrono::milliseconds(100 * i)));
  // A fresh press later plays again.
  CHECK(p.press('c', t0 + std::chrono::seconds(3)));
  CHECK(p.releaseAll().size() == 2);  // c and v are still sounding
}

// ---- snapshots ------------------------------------------------------------------

namespace {

std::string mask(std::string s) {
  // The version line changes with every commit; keep the layout, drop the text.
  const std::string v = versionLine();
  std::string placeholder = "ion VERSION";
  placeholder.resize(v.size(), ' ');
  for (auto at = s.find(v); at != std::string::npos; at = s.find(v, at)) s.replace(at, v.size(), placeholder);
  return s;
}

struct SnapshotRig {
  std::unique_ptr<Module> module = std::make_unique<Module>();
  Config config;
  Session session{*module, Paths::under("/tmp/ion-ui-snapshot"), config};

  SnapshotRig() {
    config.autosaveMinutes = 0;
    session.newProject("snapshot");
    session.setEngine(0, EngineType::Fm);
    syx::Voice v{};
    v[5 * 21 + 16] = 99;
    v[134] = 4;
    v[135] = 6;
    v[144] = 24;
    const char name[] = "SNAP TEST ";
    std::copy(name, name + 10, v.begin() + 145);
    session.setVoice(0, v, "unit");
    session.setEngine(2, EngineType::Fm);
    session.setParam({0, kPan}, -0.3f);
    session.setParam({0, kB1Gain}, 6.0f);
    session.setParam({0, kHpOn}, 1.0f);
    session.setParam({2, kFader}, -9.0f);
    session.setParam({2, kMute}, 1.0f);
    std::vector<float> buf(2 * 256);
    module->process(buf.data(), 256);
    session.tick();
  }
};

void checkSnapshot(const std::string& name, const std::string& frame) {
  const fs::path golden = fs::path(ION_TEST_DATA_DIR) / "ui" / (name + ".txt");
  const char* update = std::getenv("ION_UPDATE_GOLDEN");
  if ((update && *update == '1') || !fs::exists(golden)) {
    std::string error;
    REQUIRE(writeFileAtomic(golden, frame, error));
    WARN_MESSAGE(false, "wrote " << golden.string() << " — review and commit it");
    return;
  }
  std::string expected;
  REQUIRE(readFile(golden, expected));
  INFO("snapshot " << name << " changed (ION_UPDATE_GOLDEN=1 to accept):\n" << frame);
  CHECK(frame == expected);
}

}  // namespace

TEST_CASE("ui snapshots: every screen at the handoff's sizes") {
  SnapshotRig r;
  struct Case {
    const char* name;
    ui::ScreenId screen;
    int w, h;
    bool piano = false, help = false;
  };
  const Case cases[] = {
      {"tracks_130x36", ui::ScreenId::Tracks, 130, 36},
      {"tracks_80x24", ui::ScreenId::Tracks, 80, 24},
      {"tracks_60x35", ui::ScreenId::Tracks, 60, 35},
      {"tracks_200x50", ui::ScreenId::Tracks, 200, 50},
      {"mixer_130x36", ui::ScreenId::Mixer, 130, 36},
      {"mixer_80x24", ui::ScreenId::Mixer, 80, 24},
      {"mixer_60x35", ui::ScreenId::Mixer, 60, 35},
      {"instrument_130x36", ui::ScreenId::Instrument, 130, 36},
      {"settings_130x36", ui::ScreenId::Settings, 130, 36},
      {"settings_60x35", ui::ScreenId::Settings, 60, 35},
      {"library_130x36", ui::ScreenId::Library, 130, 36},
      {"help_130x36", ui::ScreenId::Tracks, 130, 36, false, true},
      {"piano_60x35", ui::ScreenId::Tracks, 60, 35, true, false},
  };
  for (const Case& c : cases) {
    const std::string frame = mask(ui::snapshot(r.session, *r.module, c.screen, c.w, c.h, c.piano, c.help));
    // Never wider than the terminal.
    std::size_t start = 0;
    for (std::size_t nl = frame.find('\n'); nl != std::string::npos; nl = frame.find('\n', start)) {
      int cells = 0;
      for (std::size_t i = start; i < nl; ++i) cells += (static_cast<unsigned char>(frame[i]) & 0xC0) != 0x80;
      CHECK_MESSAGE(cells <= c.w, c.name << " line too wide");
      start = nl + 1;
    }
    checkSnapshot(c.name, frame);
  }
}
