// CHIP, ion's lo-fi wave synth (DECISIONS D-61).

#include <doctest.h>

#include <cmath>
#include <set>
#include <vector>

#include "engine/chip/chip_engine.h"
#include "engine/envelope_params.h"

using namespace ion;

namespace {

std::vector<float> run(ChipEngine& e, int frames) {
  std::vector<float> l(static_cast<std::size_t>(frames)), r(static_cast<std::size_t>(frames));
  for (int i = 0; i < frames; i += 64) e.render(l.data() + i, r.data() + i, static_cast<std::size_t>(std::min(64, frames - i)));
  return l;
}

int risingCrossings(const std::vector<float>& x, std::size_t from, std::size_t to) {
  int n = 0;
  for (std::size_t i = from + 1; i < to; ++i) n += x[i - 1] < 0 && x[i] >= 0;
  return n;
}

}  // namespace

TEST_CASE("chip: a square plays in tune") {
  ChipEngine e;  // pulse 50% by default
  e.noteOn(69, 127);
  const auto x = run(e, 48000);
  CHECK(risingCrossings(x, 0, 48000) == doctest::Approx(440).epsilon(0.01));
  float pk = 0;
  for (float s : x) pk = std::max(pk, std::fabs(s));
  CHECK(pk > 0.2f);
  CHECK(pk < 1.0f);
}

TEST_CASE("chip: the table: size, bits, mult, warp, mirror, the limiter") {
  ChipEngine e;
  e.setParam(ChipEngine::kShape, ChipEngine::kSaw);
  e.setParam(ChipEngine::kSize, 16);
  e.rebuildIfNeeded();
  CHECK(e.table().size() == 16);
  e.setParam(ChipEngine::kBits, 2);  // four levels at most
  e.rebuildIfNeeded();
  std::set<float> levels(e.table().begin(), e.table().end());
  CHECK(levels.size() <= 4);
  // Mult: three repeats of the saw is three falling edges.
  e.setParam(ChipEngine::kBits, 8);
  e.setParam(ChipEngine::kSize, 96);
  e.setParam(ChipEngine::kMult, 3);
  e.rebuildIfNeeded();
  int drops = 0;
  for (std::size_t i = 1; i < e.table().size(); ++i) drops += e.table()[i] < e.table()[i - 1] - 0.5f;
  CHECK(drops == 2);  // and the wrap from the end back to the start is the third
  // Warp and mirror change it; every limiter keeps it within ±1.
  e.setParam(ChipEngine::kMult, 1);
  e.rebuildIfNeeded();
  const std::vector<float> plain(e.table().begin(), e.table().end());
  for (int p : {ChipEngine::kWarp, ChipEngine::kMirror}) {
    ChipEngine w;
    w.setParam(ChipEngine::kShape, ChipEngine::kSaw);
    w.setParam(ChipEngine::kSize, 96);
    w.setParam(p, 0.6f);
    w.rebuildIfNeeded();
    CHECK(std::vector<float>(w.table().begin(), w.table().end()) != plain);
  }
  for (int limit = 0; limit < 4; ++limit) {
    ChipEngine l;
    l.setParam(ChipEngine::kShape, ChipEngine::kSine);
    l.setParam(ChipEngine::kAmp, 24);
    l.setParam(ChipEngine::kLimit, static_cast<float>(limit));
    l.rebuildIfNeeded();
    for (float v : l.table()) CHECK(std::fabs(v) <= 1.0f);
  }
}

TEST_CASE("chip: tonal noise repeats at the note; plain noise doesn't") {
  auto periodic = [](int shape) {
    ChipEngine e;
    e.setParam(ChipEngine::kShape, static_cast<float>(shape));
    e.noteOn(57, 127);  // 220 Hz: 218.18 samples a cycle
    const auto x = run(e, 48000);
    // Correlation with itself one cycle later.
    double same = 0, total = 0;
    const int lag = 218;
    for (std::size_t i = 10000; i < 30000; ++i) {
      same += x[i] * x[i + lag];
      total += x[i] * x[i];
    }
    return same / total;
  };
  CHECK(periodic(ChipEngine::kNoiseTone) > 0.6);
  CHECK(periodic(ChipEngine::kNoise) < 0.3);
}

TEST_CASE("chip: a lower sample rate holds samples") {
  ChipEngine e;
  e.setParam(ChipEngine::kShape, ChipEngine::kSine);
  e.setParam(ChipEngine::kSize, 256);
  e.setParam(ChipEngine::kRate, 4000);  // every 12th sample is new
  e.noteOn(60, 127);
  const auto x = run(e, 4800);
  int changes = 0;
  for (std::size_t i = 1; i < x.size(); ++i) changes += x[i] != x[i - 1];
  CHECK(changes <= 4800 / 12 + 2);
  CHECK(changes > 100);
}

TEST_CASE("chip: voices, release, stealing, and every setting at its extremes") {
  ChipEngine e;
  e.setParam(ChipEngine::kPoly, 3);
  for (uint8_t n : std::initializer_list<uint8_t>{60, 64, 67, 71}) e.noteOn(n, 100);
  run(e, 256);
  CHECK(e.voiceCount() == 3);
  for (uint8_t n : std::initializer_list<uint8_t>{60, 64, 67, 71}) e.noteOff(n);
  run(e, 48000);
  CHECK(e.voiceCount() == 0);  // the 40 ms release ended them
  // Release "off": the oscillator would never stop, so a note-off fades it in 5 ms.
  e.setParam(ChipEngine::kAmpRelease, kEnvOff);
  e.noteOn(60, 100);
  run(e, 512);
  e.noteOff(60);
  run(e, 512);
  CHECK(e.voiceCount() == 0);

  const auto table = ChipEngine::paramTable();
  for (int p = 0; p < ChipEngine::kParamCount; ++p)
    for (bool high : {false, true}) {
      CAPTURE(table[static_cast<std::size_t>(p)].key);
      ChipEngine c;
      c.setParam(p, high ? table[static_cast<std::size_t>(p)].max : table[static_cast<std::size_t>(p)].min);
      c.noteOn(24, 127);
      c.noteOn(108, 127);
      c.pitchBend(-8192);
      const auto x = run(c, 4800);
      bool ok = true;
      for (float s : x) ok &= std::isfinite(s) && std::fabs(s) < 4.0f;
      CHECK(ok);
    }
}
