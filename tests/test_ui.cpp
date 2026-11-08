#include <doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <regex>

#include "audio/wav.h"
#include "core/version.h"
#include "engine/drum/drum_engine.h"
#include "engine/fx/effects.h"
#include "engine/fm/fm_engine.h"
#include "engine/module.h"
#include "library/fm_library.h"
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
  // DSP load depends on the build (sanitizers, debug): always show it as 0%.
  s = std::regex_replace(s, std::regex("cpu [0-9]%"), "cpu 0%");
  s = std::regex_replace(s, std::regex(" [0-9]% no audio"), " 0% no audio");
  return s;
}

const fs::path kSnapshotRoot = "/tmp/ion-ui-snapshot";  // fixed: it appears in the snapshots

// A small FM library of generated voices (no copyrighted data), imported the
// way a user would.
void makeLibrary(const fs::path& root) {
  const char* names[] = {"BASS 1", "SYN-BAS 4", "SLAP BASS", "SUB BASS 2", "E.PIANO 1", "RHODES MK1", "STRINGS 1", "BRASS 1",
                         "TUB BELLS", "KOTO", "FLUTE 1", "VOICE 1", "SYN-LEAD 1", "E.ORGAN 1", "WARM PAD", "TAKE OFF"};
  std::vector<syx::Voice> voices;
  for (int i = 0; i < 16; ++i) {
    syx::Voice v = syx::initVoice();
    for (std::size_t b = 0; b < 145; ++b) v[b] = static_cast<uint8_t>((b * 37 + static_cast<std::size_t>(i) * 11) % 100);
    v = syx::normalize(v);
    v[134] = static_cast<uint8_t>(i == 1 ? 4 : i * 2);  // SYN-BAS 4 on algorithm 5, as in the mockup
    char n[11];
    std::snprintf(n, sizeof n, "%-10s", names[i]);
    std::copy(n, n + 10, v.begin() + 145);
    voices.push_back(v);
  }
  const auto bank = syx::makeBanks(voices).at(0);
  fs::create_directories(root / "syx");
  std::string error;
  writeFileAtomic(root / "syx" / "cart_17.syx", std::string(bank.begin(), bank.end()), error);
  const Paths paths = Paths::under(root);
  FmLibrary::import(paths.libraryFile(), root / "syx", paths.importLog());
}

// Generated one-shots for the drum screen (no third-party audio): decaying
// sines and noise of set lengths, so the durations in the list are fixed.
void makeSamples(const fs::path& root) {
  struct File {
    const char* path;
    double seconds, hz;
    bool noise;
    unsigned rate;
  };
  const File files[] = {
      {"garage/kicks/kick_deep_01.wav", 0.61, 50, false, 44100},  {"garage/kicks/kick_garage_01.wav", 0.37, 60, false, 44100},
      {"garage/kicks/kick_sub_01.wav", 0.88, 40, false, 48000},    {"garage/kicks/kick_tight_01.wav", 0.31, 70, false, 44100},
      {"garage/kicks/kick_tight_02.wav", 0.29, 72, false, 44100},  {"garage/kicks/kick_tight_03.wav", 0.42, 65, false, 44100},
      {"garage/kicks/kick_tight_10.wav", 0.33, 68, false, 44100},  {"garage/hats/hat_closed.wav", 0.12, 0, true, 48000},
      {"garage/hats/hat_open.wav", 0.74, 0, true, 48000},          {"garage/perc/clap_room.wav", 0.35, 0, true, 44100},
      {"garage/perc/snare_rim.wav", 0.22, 180, false, 44100},      {"garage/perc/shaker.wav", 0.18, 0, true, 44100},
      {"garage/perc/rim.wav", 0.09, 400, false, 44100},            {"garage/perc/tom_low.wav", 0.55, 90, false, 44100},
      {"garage/perc/perc_wood.wav", 0.16, 700, false, 44100},
  };
  uint32_t seed = 1;
  for (const File& f : files) {
    wav::Audio a;
    a.channels = 1;
    a.sampleRate = f.rate;
    const auto n = static_cast<std::size_t>(f.seconds * f.rate);
    a.samples.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
      const double t = static_cast<double>(i) / f.rate;
      seed = seed * 1664525u + 1013904223u;
      const double src = f.noise ? (static_cast<double>(seed >> 8) / 8388608.0 - 1.0) : std::sin(2 * 3.14159265 * f.hz * t);
      a.samples[i] = static_cast<float>(0.8 * src * std::exp(-t * 6 / f.seconds));
    }
    fs::create_directories((root / f.path).parent_path());
    std::string error;
    wav::write(root / f.path, a, error);
  }
}

struct SnapshotRig {
  std::unique_ptr<Module> module = std::make_unique<Module>();
  Config config;
  std::unique_ptr<Session> owned = [this] {
    fs::remove_all(kSnapshotRoot);
    makeLibrary(kSnapshotRoot);
    return std::make_unique<Session>(*module, Paths::under(kSnapshotRoot), config);
  }();
  Session& session = *owned;

  SnapshotRig() {
    config.autosaveMinutes = 0;
    std::string error;
    REQUIRE(session.openLibrary(error));
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
    // Track 3 browses the library: SYN-BAS 4, a favourite with tags and macro
    // edits, in the program list, with a CC on Bright (the FMBrowser mockup).
    FmLibrary& lib = *session.library();
    const FmPatch* synbas = nullptr;
    for (const FmPatch& p : lib.patches())
      if (p.name == "SYN-BAS 4") synbas = &p;
    REQUIRE(synbas);
    const int64_t id = synbas->id;
    lib.setFavourite(id, true);
    lib.setTags(id, {"bass", "garage"});
    for (const FmPatch& p : lib.patches())
      if (p.name == "BASS 1") lib.setFavourite(p.id, true);
    session.loadPatch(2, *lib.find(id));
    session.toggleProgram(lib.find(id)->voice, "cart_17");
    session.setParam({2, static_cast<uint16_t>(kEngineParamBase + FmEngine::kBright)}, 12.0f);
    session.setParam({2, static_cast<uint16_t>(kEngineParamBase + FmEngine::kRelease)}, -8.0f);
    session.addMapping(0, 74, {2, static_cast<uint16_t>(kEngineParamBase + FmEngine::kBright)});
    session.setParam({0, kPan}, -0.3f);
    session.setParam({0, kB1Gain}, 6.0f);
    session.setParam({0, kHpOn}, 1.0f);
    session.setParam({2, kFader}, -9.0f);
    session.setParam({2, kMute}, 1.0f);

    // Track 3's inserts and sends (the FX mockup): compressor and chorus, sends to both buses.
    session.setInsert(2, 0, EffectType::Compressor);
    session.setInsert(2, 1, EffectType::Chorus);
    session.setParam({2, kSendA}, -12.0f);
    session.setParam({2, kSendB}, -8.0f);
    session.setParam({kReverbScope, kFader}, -4.5f);
    session.setParam(insertOnRef(kMasterScope, 0), 1.0f);

    // Track 4: a drum kit (the DrumKit mockup), from generated samples.
    makeSamples(kSnapshotRoot / "samples");
    config.sampleRoots = {(kSnapshotRoot / "samples").string()};
    session.setEngine(3, EngineType::Drum);
    const char* pads[] = {"kicks/kick_tight_03", "kicks/kick_sub_01", "perc/clap_room", "perc/snare_rim",
                          "hats/hat_closed",     "hats/hat_open",     "perc/shaker",    "perc/rim",
                          "perc/tom_low",        "perc/perc_wood",    "perc/cowbell"};  // cowbell.wav doesn't exist
    for (int i = 0; i < 11; ++i)
      session.setPadSample(3, i, (kSnapshotRoot / "samples" / "garage" / (std::string(pads[i]) + ".wav")).string());
    auto pad = [&](int p, int q, float value) { session.setParam(engineParamRef(3, DrumEngine::param(p, q)), value); };
    pad(0, DrumEngine::kLevel, -3);
    pad(0, DrumEngine::kDecay, 420);
    pad(0, DrumEngine::kFilter, DrumEngine::kLowPass);
    pad(0, DrumEngine::kCutoff, 8200);
    pad(0, DrumEngine::kReso, 0.12f);
    pad(0, DrumEngine::kVelLevel, 0.8f);
    pad(0, DrumEngine::kDrive, 0.15f);
    pad(4, DrumEngine::kChoke, 1);
    pad(5, DrumEngine::kChoke, 1);
    pad(1, DrumEngine::kTune, -2);
    session.addMapping(3, 71, engineParamRef(3, DrumEngine::param(0, DrumEngine::kDecay)));
    REQUIRE(session.saveKit(3, "Garage 02", error));
    REQUIRE(session.waitForSamples());
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
    int track = 0;
    std::vector<std::string> keys = {};
  };
  // Into garage/kicks/ and down to kick_tight_03 (which previews).
  const std::vector<std::string> kIntoKicks = {"Return", "Down", "Return", "Down", "Return", "Down", "Down", "Down", "Down", "Down"};
  const Case cases[] = {
      {"tracks_130x36", ui::ScreenId::Tracks, 130, 36},
      {"tracks_80x24", ui::ScreenId::Tracks, 80, 24},
      {"tracks_60x35", ui::ScreenId::Tracks, 60, 35},
      {"tracks_200x50", ui::ScreenId::Tracks, 200, 50},
      {"mixer_130x36", ui::ScreenId::Mixer, 130, 36},
      {"mixer_80x24", ui::ScreenId::Mixer, 80, 24},
      {"mixer_60x35", ui::ScreenId::Mixer, 60, 35},
      {"instrument_130x36", ui::ScreenId::Instrument, 130, 36},
      {"fm_browser_130x36", ui::ScreenId::Instrument, 130, 36, false, false, 2},
      {"fm_browser_80x24", ui::ScreenId::Instrument, 80, 24, false, false, 2},
      {"fm_browser_60x35", ui::ScreenId::Instrument, 60, 35, false, false, 2},
      {"tracks_fm_130x36", ui::ScreenId::Tracks, 130, 36, false, false, 2},
      {"settings_130x36", ui::ScreenId::Settings, 130, 36},
      {"settings_60x35", ui::ScreenId::Settings, 60, 35},
      {"library_130x36", ui::ScreenId::Library, 130, 36},
      {"help_130x36", ui::ScreenId::Tracks, 130, 36, false, true},
      {"piano_60x35", ui::ScreenId::Tracks, 60, 35, true, false},
      {"tracks_drum_130x36", ui::ScreenId::Tracks, 130, 36, false, false, 3},
      {"drum_roots_130x36", ui::ScreenId::Instrument, 130, 36, false, false, 3},
      {"drum_130x36", ui::ScreenId::Instrument, 130, 36, false, false, 3, kIntoKicks},
      {"drum_80x24", ui::ScreenId::Instrument, 80, 24, false, false, 3, kIntoKicks},
      {"drum_60x35", ui::ScreenId::Instrument, 60, 35, false, false, 3, {"Tab"}},
      {"drum_params_130x36", ui::ScreenId::Instrument, 130, 36, false, false, 3, {"Tab", "Tab", "Down", "Down"}},
      {"drum_params_60x35", ui::ScreenId::Instrument, 60, 35, false, false, 3, {"Tab", "Tab", "Down", "Down"}},
      {"piano_drum_130x36", ui::ScreenId::Instrument, 130, 36, true, false, 3},
      {"fx_130x36", ui::ScreenId::Fx, 130, 36, false, false, 2, {"Down", "Down", "Down", "Down", "Down", "Down", "Down", "Tab", "Down"}},
      {"fx_80x24", ui::ScreenId::Fx, 80, 24, false, false, 2},
      {"fx_60x35", ui::ScreenId::Fx, 60, 35, false, false, 2},
      {"fx_picker_130x36", ui::ScreenId::Fx, 130, 36, false, false, 2, {"Down", "Down", "Return"}},
      {"library_kits_130x36", ui::ScreenId::Library, 130, 36, false, false, 3, {"Tab", "Down", "Down", "Right"}},
  };
  for (const Case& c : cases) {
    const std::string frame = mask(ui::snapshot(r.session, *r.module, c.screen, c.w, c.h, c.piano, c.help, c.track, c.keys));
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

// ---- the import prompt (0.2.1: a wrong folder used to fail quietly) ----

#include "ui/screens.h"

TEST_CASE("import prompt: a folder that doesn't exist asks again; a real one starts the import") {
  const fs::path root = fs::temp_directory_path() / "ion-import-prompt";
  fs::remove_all(root);
  fs::create_directories(root / "banks" / "dx7");
  auto module = std::make_unique<Module>();
  Config config;
  Session session(*module, Paths::under(root), config);
  ui::Context ctx(session, *module, nullptr, nullptr);

  ui::requestImport(ctx);
  REQUIRE(ctx.request == ui::Context::Request::Prompt);
  CHECK(ctx.promptPath);
  REQUIRE(ctx.promptDone);

  auto submit = [&](const std::string& text) {
    auto done = std::move(ctx.promptDone);
    ctx.request = ui::Context::Request::None;
    done(ctx, text);
  };
  submit((root / "nope").string());
  CHECK(ctx.request == ui::Context::Request::Prompt);  // asked again…
  CHECK(ctx.promptTitle.rfind("Not found", 0) == 0);  // …saying why
  CHECK(ctx.promptInitial == (root / "nope").string());  // with the text kept to fix
  REQUIRE(ctx.promptDone);

  submit((root / "banks").string() + "  ");  // trailing spaces are forgiven
  CHECK(ctx.request == ui::Context::Request::Import);
  CHECK(ctx.importPath == (root / "banks").string());
  fs::remove_all(root);
}

TEST_CASE("import: a failed or empty import is still written to the log") {
  const fs::path root = fs::temp_directory_path() / "ion-import-log";
  fs::remove_all(root);
  fs::create_directories(root / "empty");
  const Paths paths = Paths::under(root);
  const ImportReport missing = FmLibrary::import(paths.libraryFile(), root / "missing", paths.importLog());
  CHECK_FALSE(missing.error.empty());
  const ImportReport empty = FmLibrary::import(paths.libraryFile(), root / "empty", paths.importLog());
  CHECK(empty.error.empty());
  CHECK(empty.files == 0);
  std::string log;
  REQUIRE(readFile(paths.importLog(), log));
  CHECK(log.find("failed: no such file or folder") != std::string::npos);
  CHECK(log.find("no .syx or .snd files") != std::string::npos);
  fs::remove_all(root);
}

TEST_CASE("mixer: m and s act on the selected strip; returns mute but don't solo") {
  SnapshotRig r;
  // Strips: 8 tracks, then D, R, M. From track 1, eight steps right is D.
  const std::vector<std::string> toDelay = {"Right", "Right", "Right", "Right", "Right", "Right", "Right", "Right"};
  auto keys = toDelay;
  keys.push_back("m");
  keys.push_back("s");
  ui::snapshot(r.session, *r.module, ui::ScreenId::Mixer, 130, 36, false, false, 0, keys);
  CHECK(r.session.param({kDelayScope, kMute}) == 1.0f);
  CHECK(r.session.param({kDelayScope, kSolo}) == 0.0f);
  CHECK(r.session.param({0, kMute}) == 0.0f);
  CHECK(r.session.param({7, kSolo}) == 0.0f);
  // On a track, m mutes that track.
  ui::snapshot(r.session, *r.module, ui::ScreenId::Mixer, 130, 36, false, false, 0, {"Right", "m"});
  CHECK(r.session.param({1, kMute}) == 1.0f);
}
