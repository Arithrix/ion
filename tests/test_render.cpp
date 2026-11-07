#include <doctest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>

#include "audio/wav.h"
#include "engine/fm/fm_engine.h"
#include "engine/module.h"
#include "midi/smf.h"
#include "project/config.h"
#include "project/offline.h"
#include "project/session.h"

using namespace ion;
namespace fs = std::filesystem;

TEST_CASE("smf: write → parse round-trip, with timing") {
  const std::vector<smf::Event> events = {
      {0.0, {0x90, 60, 100}}, {0.5, {0x80, 60, 0}}, {0.5, {0x91, 64, 90}}, {1.25, {0xB1, 74, 33}}, {2.0, {0x81, 64, 0}}};
  const auto bytes = smf::write(events, 124.0);
  smf::File f;
  std::string error;
  REQUIRE(smf::parse(bytes, f, error));
  REQUIRE(f.events.size() == events.size());
  for (std::size_t i = 0; i < events.size(); ++i) {
    CHECK(f.events[i].msg.status == events[i].msg.status);
    CHECK(f.events[i].msg.data1 == events[i].msg.data1);
    CHECK(f.events[i].seconds == doctest::Approx(events[i].seconds).epsilon(0.002));
  }
  CHECK(f.lengthSeconds == doctest::Approx(2.0).epsilon(0.002));
}

TEST_CASE("smf: rejects junk, never crashes") {
  smf::File f;
  std::string error;
  CHECK_FALSE(smf::parse(std::vector<uint8_t>{1, 2, 3}, f, error));
  auto good = smf::write({{0.0, {0x90, 60, 100}}, {1.0, {0x80, 60, 0}}});
  for (std::size_t cut = 14; cut < good.size(); cut += 3) {
    std::vector<uint8_t> truncated(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(cut));
    CHECK_NOTHROW(smf::parse(truncated, f, error));
  }
  std::mt19937 rng(5);
  for (int i = 0; i < 200; ++i) {
    auto noisy = good;
    noisy[14 + rng() % (noisy.size() - 14)] = static_cast<uint8_t>(rng());
    CHECK_NOTHROW(smf::parse(noisy, f, error));
  }
}

namespace {

// The golden scene: one FM track with a generated (non-copyright) voice, EQ on,
// a short phrase with a CC move. Any change to how ion sounds shows up here.
wav::Audio renderGoldenScene() {
  auto module = std::make_unique<Module>();
  Config config;
  config.autosaveMinutes = 0;
  const fs::path root = fs::temp_directory_path() / "ion-golden";
  Session session(*module, Paths::under(root), config);
  session.newProject("golden");
  session.setEngine(0, EngineType::Fm);
  syx::Voice v{};
  for (int op = 0; op < 6; ++op) {
    uint8_t* o = v.data() + op * 21;
    const uint8_t eg[8] = {90, 40, 30, 60, 99, 85, 60, 0};
    std::copy(eg, eg + 8, o);
    o[18] = static_cast<uint8_t>(op == 4 ? 2 : 1);  // op2 an octave up
    o[16] = op == 5 ? 99 : (op == 4 ? 70 : 0);      // op1 carrier, op2 modulating it
    o[15] = 3;
  }
  for (int i = 0; i < 4; ++i) {
    v[static_cast<std::size_t>(126 + i)] = 99;
    v[static_cast<std::size_t>(130 + i)] = 50;
  }
  v[134] = 0;  // algorithm 1: op2 → op1
  v[135] = 3;
  v[144] = 24;
  const char name[] = "GOLDEN FM ";
  std::copy(name, name + 10, v.begin() + 145);
  session.setVoice(0, v, "test");
  session.setParam({0, kB1Gain}, 6.0f);
  session.setParam({0, kB1Freq}, 1500.0f);
  session.setParam({0, kPan}, -0.3f);

  const std::vector<smf::Event> phrase = {
      {0.00, {0x90, 48, 110}}, {0.25, {0x90, 55, 90}}, {0.50, {0x80, 48, 0}}, {0.50, {0x90, 60, 100}},
      {0.75, {0xE0, 0x00, 0x50}}, {0.90, {0x80, 55, 0}}, {1.00, {0x80, 60, 0}}};
  wav::Audio out = renderOffline(*module, session, phrase, 1.5);
  fs::remove_all(root);
  return out;
}

}  // namespace

TEST_CASE("golden render: FM phrase") {
  const fs::path golden = fs::path(ION_TEST_DATA_DIR) / "fm_phrase.wav";
  const wav::Audio audio = renderGoldenScene();
  REQUIRE(audio.frames() == 72000);

  float peak = 0;
  for (float s : audio.samples) {
    REQUIRE(std::isfinite(s));
    peak = std::max(peak, std::fabs(s));
  }
  CHECK(peak > 0.05f);  // it actually plays

  const char* update = std::getenv("ION_UPDATE_GOLDEN");
  if ((update && *update == '1') || !fs::exists(golden)) {
    std::string error;
    fs::create_directories(golden.parent_path());
    REQUIRE(wav::write(golden, audio, error));
    WARN_MESSAGE(false, "wrote golden file " << golden.string() << " — commit it if the change is intended");
    return;
  }

  wav::Audio expected;
  std::string error;
  REQUIRE(wav::read(golden, expected, error));
  REQUIRE(expected.samples.size() == audio.samples.size());
  double sumSq = 0, maxDiff = 0;
  for (std::size_t i = 0; i < audio.samples.size(); ++i) {
    const double d = std::fabs(double(audio.samples[i]) - expected.samples[i]);
    sumSq += d * d;
    maxDiff = std::max(maxDiff, d);
  }
  const double rms = std::sqrt(sumSq / static_cast<double>(audio.samples.size()));
  INFO("rms diff " << rms << ", max diff " << maxDiff << " (ION_UPDATE_GOLDEN=1 to accept a change)");
  CHECK(rms < 1e-5);
  CHECK(maxDiff < 1e-4);
}
