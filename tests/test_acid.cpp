// The 303-style bass (DECISIONS D-60).

#include <doctest.h>

#include <cmath>
#include <vector>

#include "engine/acid/acid_engine.h"

using namespace ion;

namespace {

std::vector<float> run(AcidEngine& e, int frames) {
  std::vector<float> l(static_cast<std::size_t>(frames)), r(static_cast<std::size_t>(frames));
  for (int i = 0; i < frames; i += 64) e.render(l.data() + i, r.data() + i, static_cast<std::size_t>(std::min(64, frames - i)));
  return l;
}

float peak(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
  float p = 0;
  for (std::size_t i = from; i < std::min(to, x.size()); ++i) p = std::max(p, std::fabs(x[i]));
  return p;
}

// The period by autocorrelation over a window, in samples: the shortest lag
// that correlates nearly as well as the best (so it doesn't land on a multiple).
double period(const std::vector<float>& x, std::size_t from, std::size_t len, int minLag, int maxLag) {
  std::vector<double> c;
  double best = -1e9;
  for (int lag = minLag; lag <= maxLag; ++lag) {
    double s = 0;
    for (std::size_t i = from; i < from + len; ++i) s += x[i] * x[i + static_cast<std::size_t>(lag)];
    c.push_back(s);
    best = std::max(best, s);
  }
  for (std::size_t i = 1; i + 1 < c.size(); ++i)
    if (c[i] >= 0.9 * best && c[i] >= c[i - 1] && c[i] >= c[i + 1]) return minLag + static_cast<int>(i);
  return 0;
}

}  // namespace

TEST_CASE("acid: plays in tune at a sensible level, then sleeps") {
  AcidEngine e;
  e.setParam(AcidEngine::kCutoff, 2000);
  e.setParam(AcidEngine::kResonance, 0.2f);
  e.noteOn(45, 90);  // A2: 110 Hz
  const auto x = run(e, 24000);
  for (float s : x) REQUIRE(std::isfinite(s));
  CHECK(peak(x) > 0.05f);
  CHECK(peak(x) < 1.0f);
  const double p = period(x, 2000, 4000, 300, 800);  // 110 Hz is 436.4 samples
  CHECK(p == doctest::Approx(48000.0 / 110.0).epsilon(0.01));
  e.noteOff(45);
  run(e, 48000);
  CHECK_FALSE(e.awake());  // silent and asleep: costs nothing
  CHECK(e.voiceCount() == 0);
}

TEST_CASE("acid: overlapping notes slide; accents are louder") {
  // Slide: hold A2, then press A3 before letting go: the pitch glides up.
  AcidEngine e;
  e.setParam(AcidEngine::kSlide, 100);
  e.setParam(AcidEngine::kCutoff, 2000);
  e.noteOn(45, 90);
  run(e, 9600);
  e.noteOn(57, 90);
  e.noteOff(45);
  const auto x = run(e, 24000);
  const double early = period(x, 200, 1200, 150, 500);
  const double late = period(x, 18000, 2000, 150, 500);
  CHECK(late == doctest::Approx(48000.0 / 220.0).epsilon(0.02));
  CHECK(early > late * 1.15);  // still on its way up

  // Accent: velocity at the threshold against one below it.
  auto loud = [](uint8_t vel) {
    AcidEngine a;
    a.setParam(AcidEngine::kAccent, 1);
    a.noteOn(45, vel);
    return peak(run(a, 9600));
  };
  CHECK(loud(100) > 1.2f * loud(99));
  AcidEngine b;
  b.setParam(AcidEngine::kAccentAt, 127);  // a higher threshold: 100 isn't an accent any more
  b.setParam(AcidEngine::kAccent, 1);
  b.noteOn(45, 100);
  CHECK(peak(run(b, 9600)) == doctest::Approx(loud(99)).epsilon(0.1));
}

TEST_CASE("acid: tuning holds when sliding back to a held note") {
  AcidEngine e;
  e.setParam(AcidEngine::kTune, 12);  // an octave up
  e.setParam(AcidEngine::kCutoff, 2000);
  e.noteOn(45, 90);
  run(e, 4800);
  e.noteOn(50, 90);
  run(e, 4800);
  e.noteOff(50);  // back to the held A2, an octave up: 220 Hz
  const auto x = run(e, 24000);
  CHECK(period(x, 18000, 2000, 150, 500) == doctest::Approx(48000.0 / 220.0).epsilon(0.02));
}

TEST_CASE("acid: every setting at its extremes stays finite and bounded") {
  const auto table = AcidEngine::paramTable();
  for (int p = 0; p < AcidEngine::kParamCount; ++p)
    for (bool high : {false, true}) {
      CAPTURE(table[static_cast<std::size_t>(p)].key);
      CAPTURE(high);
      AcidEngine e;
      e.setParam(p, high ? table[static_cast<std::size_t>(p)].max : table[static_cast<std::size_t>(p)].min);
      e.noteOn(36, 127);
      e.noteOn(48, 127);
      e.pitchBend(8191);
      const auto x = run(e, 9600);
      bool finite = true;
      for (float s : x) finite &= std::isfinite(s);
      CHECK(finite);
      CHECK(peak(x) < 4.0f);
      e.allNotesOff();
      CHECK_FALSE(e.awake());
    }
}
