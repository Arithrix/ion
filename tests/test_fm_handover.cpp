// Patch changes and macros under held notes (HANDOFF §6.3, PLAN "hard parts" 2).

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/fm/dx7_voice.h"
#include "engine/fm/fm_engine.h"

using namespace ion;

namespace {

// Voices built here so the tests need no copyrighted bank. One sine carrier
// (op 1, algorithm 32) with the given envelope; the other operators are silent.
syx::Voice sineVoice(std::array<uint8_t, 8> eg, uint8_t level = 99) {
  syx::Voice v{};
  for (int op = 1; op <= 6; ++op) {
    uint8_t* o = v.data() + dx7::opOffset(op);
    std::copy(eg.begin(), eg.end(), o);
    o[18] = 1;  // coarse 1
    o[20] = 7;  // detune centre
    o[16] = op == 1 ? level : 0;
  }
  for (int i = 0; i < 4; ++i) { v[126 + i] = 99; v[130 + i] = 50; }  // flat pitch EG
  v[dx7::kAlgorithmByte] = 31;
  v[dx7::kTransposeByte] = 24;
  const char name[] = "TEST SINE ";
  std::copy(name, name + 10, v.begin() + syx::kNameOffset);
  return v;
}

// Organ-like: instant attack, full sustain, quick release.
syx::Voice organ() { return sineVoice({99, 99, 99, 70, 99, 99, 99, 0}); }
// Silent: a valid voice whose carrier has output level 0.
syx::Voice silent() { return sineVoice({99, 99, 99, 70, 99, 99, 99, 0}, 0); }

std::vector<float> run(FmEngine& fm, std::size_t blocks, auto at) {
  std::vector<float> out(blocks * kBlockSize), right(kBlockSize);
  for (std::size_t b = 0; b < blocks; ++b) {
    at(b);
    FmEngine::setClock(b * kBlockSize);
    fm.render(out.data() + b * kBlockSize, right.data(), kBlockSize);
    std::fill(right.begin(), right.end(), 0.0f);
  }
  return out;
}

float rms(const std::vector<float>& x, std::size_t fromBlock, std::size_t toBlock) {
  double s = 0;
  for (std::size_t i = fromBlock * kBlockSize; i < toBlock * kBlockSize; ++i) s += double(x[i]) * x[i];
  return static_cast<float>(std::sqrt(s / static_cast<double>((toBlock - fromBlock) * kBlockSize)));
}

// Largest sample-to-sample change: a click shows up as a step far bigger than
// the signal's own slope.
float worstStep(const std::vector<float>& x) {
  float w = 0;
  for (std::size_t i = 1; i < x.size(); ++i) w = std::max(w, std::fabs(x[i] - x[i - 1]));
  return w;
}

}  // namespace

TEST_CASE("FM handover: a held note keeps sounding through a patch change") {
  FmEngine fm;
  fm.loadVoice(organ());
  const auto out = run(fm, 200, [&](std::size_t b) {
    if (b == 10) fm.noteOn(60, 100);
    if (b == 50) fm.loadVoice(silent());  // the new patch makes no sound at all
  });
  const float before = rms(out, 30, 50), after = rms(out, 60, 200);
  CHECK(before > 0.02f);
  CHECK(after == doctest::Approx(before).epsilon(0.05));  // still the old patch, untouched
  // A sine at C4 moves at most 2π·261.6/48000·peak per sample.
  CHECK(worstStep(out) < 0.05f);
}

TEST_CASE("FM handover: new notes play the new patch, note-offs reach the old one") {
  FmEngine fm;
  fm.loadVoice(organ());
  const auto out = run(fm, 400, [&](std::size_t b) {
    if (b == 10) fm.noteOn(60, 100);
    if (b == 50) fm.loadVoice(silent());
    if (b == 60) fm.noteOn(64, 100);  // on the silent patch
    if (b == 100) fm.noteOff(60);     // must release the note on the old instance
  });
  CHECK(rms(out, 70, 100) > 0.02f);
  CHECK(rms(out, 300, 400) < 1e-4f);  // 60 released; 64 is silent
}

TEST_CASE("FM handover: scrolling fast through patches with a note held never clicks") {
  FmEngine fm;
  fm.loadVoice(organ());
  const auto out = run(fm, 2000, [&](std::size_t b) {
    if (b == 10) fm.noteOn(60, 100);
    // Every 20 ms, alternating patches: faster than key repeat.
    if (b > 50 && b < 1500 && b % 15 == 0) fm.loadVoice(b % 30 ? organ() : silent());
    if (b % 15 == 7 && b > 50 && b < 1500) fm.noteOn(static_cast<uint8_t>(48 + b % 24), 100);
    if (b % 15 == 12 && b > 50 && b < 1500) fm.noteOff(static_cast<uint8_t>(48 + (b - 5) % 24));
    CHECK(fm.soundingSlots() <= FmEngine::kSlots);
  });
  for (float s : out) REQUIRE(std::isfinite(s));
  // Several sines summed, each moving ≤ 0.017 per sample at these pitches: a
  // hard cut of a voice at full level would be a step of ~0.5.
  CHECK(worstStep(out) < 0.12f);
}

TEST_CASE("FM handover: the same note retriggered across a patch change isn't left stuck") {
  FmEngine fm;
  fm.loadVoice(organ());
  const auto out = run(fm, 300, [&](std::size_t b) {
    if (b == 10) fm.noteOn(60, 100);
    if (b == 20) fm.loadVoice(organ());
    if (b == 30) fm.noteOn(60, 100);  // no note-off in between
    if (b == 40) fm.noteOff(60);
  });
  CHECK(rms(out, 250, 300) < 1e-4f);
  CHECK(fm.voiceCount() == 0);
}

TEST_CASE("FM handover: the sustain pedal carries over to the new patch") {
  FmEngine fm;
  fm.loadVoice(organ());
  const auto out = run(fm, 400, [&](std::size_t b) {
    if (b == 5) fm.controlChange(64, 127);
    if (b == 10) fm.noteOn(60, 100);
    if (b == 20) fm.noteOff(60);  // sustained
    if (b == 30) fm.loadVoice(organ());
    if (b == 40) fm.noteOn(67, 100);
    if (b == 50) fm.noteOff(67);  // sustained on the new instance
    if (b == 150) fm.controlChange(64, 0);
  });
  CHECK(rms(out, 100, 150) > 0.02f);
  CHECK(fm.voiceCount() == 0);
  CHECK(rms(out, 350, 400) < 1e-4f);
}

TEST_CASE("FM handover: idle instances aren't rendered") {
  FmEngine fm;
  fm.loadVoice(organ());
  run(fm, 10, [](std::size_t) {});
  CHECK(fm.soundingSlots() == 0);
  run(fm, 300, [&](std::size_t b) {
    if (b == 0) fm.noteOn(60, 100);
    if (b == 5) fm.noteOff(60);
  });
  CHECK(fm.soundingSlots() == 0);  // the release, then the quiet window, are over
}

// ---- macros ----

TEST_CASE("macros: zero changes nothing; results stay valid DX7 values") {
  const syx::Voice v = organ();
  CHECK(dx7::applyMacros(v, {}) == v);
  dx7::Macros extreme{50, 7, 7, 7};
  const syx::Voice e = dx7::applyMacros(v, extreme);
  CHECK(syx::normalize(e) == e);
  dx7::Macros other{-50, -7, -7, -7};
  const syx::Voice o = dx7::applyMacros(v, other);
  CHECK(syx::normalize(o) == o);
}

TEST_CASE("macros: each one touches what the handoff says, and only that") {
  syx::Voice v = organ();
  v[dx7::kAlgorithmByte] = 4;  // algorithm 5: carriers 1, 3, 5; modulators 2, 4, 6
  for (int op = 1; op <= 6; ++op) v[dx7::opOffset(op) + dx7::kOpLevel] = 60;
  v[dx7::opOffset(6) + dx7::kOpLevel] = 0;  // an unused modulator
  v[dx7::kFeedbackByte] = 5;

  const syx::Voice b = dx7::applyMacros(v, {.bright = 10});
  CHECK(b[dx7::opOffset(2) + dx7::kOpLevel] == 70);
  CHECK(b[dx7::opOffset(4) + dx7::kOpLevel] == 70);
  CHECK(b[dx7::opOffset(6) + dx7::kOpLevel] == 0);   // stays silent
  CHECK(b[dx7::opOffset(1) + dx7::kOpLevel] == 60);  // carriers untouched

  CHECK(dx7::applyMacros(v, {.feedback = 4})[dx7::kFeedbackByte] == 7);  // clamped
  CHECK(dx7::applyMacros(v, {.feedback = -2})[dx7::kFeedbackByte] == 3);

  const syx::Voice d = dx7::applyMacros(v, {.detune = 7});
  CHECK(d[dx7::opOffset(1) + dx7::kOpDetune] == 14);
  CHECK(d[dx7::opOffset(2) + dx7::kOpDetune] == 0);
  CHECK(d[dx7::opOffset(6) + dx7::kOpDetune] == 5);
  const syx::Voice centred = dx7::applyMacros(d, {.detune = -7});
  for (int op = 1; op <= 6; ++op) CHECK(centred[dx7::opOffset(op) + dx7::kOpDetune] == 7);

  syx::Voice vel = v;
  for (int op = 1; op <= 6; ++op) vel[dx7::opOffset(op) + dx7::kOpVelocity] = 2;
  const syx::Voice s = dx7::applyMacros(vel, {.velocity = 3});
  CHECK(s[dx7::opOffset(1) + dx7::kOpVelocity] == 5);
  CHECK(s[dx7::opOffset(2) + dx7::kOpVelocity] == 2);  // modulator
}

TEST_CASE("macros: moving one under a held note doesn't disturb its envelope") {
  // Regression for the ion change in Synth_Dexed's env.cpp: upstream sent held
  // notes to envelope stage 3 on every parameter change. This voice rises over
  // about a second, then decays to a quiet sustain; under upstream's code a CC
  // sweep starting mid-attack leaves it almost silent.
  const syx::Voice pad = sineVoice({55, 60, 60, 60, 99, 99, 40, 0});
  auto play = [&](bool sweep) {
    FmEngine fm;
    fm.loadVoice(pad);
    return run(fm, 300, [&](std::size_t b) {
      if (b == 0) fm.noteOn(60, 100);
      // A CC sweep on the detune macro, every 2 blocks: it refreshes the held
      // note (the path that used to restart envelopes) without changing its
      // envelopes or level, so the loudness shouldn't change.
      if (sweep && b >= 20 && b < 200 && b % 2 == 0) fm.setParam(FmEngine::kDetune, static_cast<float>((b / 2) % 7 - 3));
    });
  };
  const auto plain = play(false), swept = play(true);
  CHECK(rms(plain, 80, 100) > 3 * rms(plain, 20, 25));  // still rising when the sweep starts
  for (std::size_t w = 0; w < 300; w += 20)
    CHECK(rms(swept, w, w + 20) == doctest::Approx(rms(plain, w, w + 20)).epsilon(0.02).scale(1e-4));
}

TEST_CASE("macros: bright under a held, sustaining note changes the sound promptly and smoothly") {
  syx::Voice v = organ();
  v[dx7::kAlgorithmByte] = 0;  // algorithm 1: op 2 modulates op 1
  v[dx7::opOffset(2) + dx7::kOpLevel] = 40;
  // Op 2 holds at full level through stage 2 (L2 = L3) and then sustains, with
  // a very slow R3: retargeted at its own rate, the change would take seconds.
  v[dx7::opOffset(2) + 2] = 10;
  FmEngine fm;
  fm.loadVoice(v);
  const auto out = run(fm, 300, [&](std::size_t b) {
    if (b == 0) fm.noteOn(48, 100);
    if (b == 100) fm.setParam(FmEngine::kBright, 40);
  });
  CHECK(fm.effectiveVoice()[dx7::opOffset(2) + dx7::kOpLevel] == 80);
  // Brighter = more energy in the upper harmonics: measured as the mean
  // absolute first difference, which weights high frequencies.
  auto edge = [&](std::size_t from, std::size_t to) {
    double s = 0;
    for (std::size_t i = from * kBlockSize + 1; i < to * kBlockSize; ++i) s += std::fabs(out[i] - out[i - 1]);
    return s;
  };
  CHECK(edge(180, 200) > 1.5 * edge(80, 100));  // within ~0.1 s
  for (float s : out) REQUIRE(std::isfinite(s));
}
