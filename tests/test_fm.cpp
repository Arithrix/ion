#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "engine/fm/fm_engine.h"

using namespace ion;

namespace {

struct Render {
  std::vector<float> left, right;
};

// Renders `blocks` blocks, calling `at(block)` before each one.
template <typename Fn>
Render render(FmEngine& fm, std::size_t blocks, Fn at) {
  Render r;
  r.left.resize(blocks * kBlockSize);
  r.right.resize(blocks * kBlockSize);
  for (std::size_t b = 0; b < blocks; ++b) {
    at(b);
    FmEngine::setClock(b * kBlockSize);
    fm.render(r.left.data() + b * kBlockSize, r.right.data() + b * kBlockSize, kBlockSize);
  }
  return r;
}

float peak(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
  float p = 0;
  for (std::size_t i = from; i < std::min(to, x.size()); ++i) p = std::max(p, std::fabs(x[i]));
  return p;
}

// A piano-like voice: one carrier (op1, algorithm 32) whose envelope decays to
// level 0 (L3 = 0). Built here so the test needs no copyrighted bank.
syx::Voice decayingVoice() {
  syx::Voice v{};
  for (int op = 0; op < 6; ++op) {
    uint8_t* o = v.data() + op * 21;
    const uint8_t eg[8] = {95, 25, 25, 67, 99, 75, 0, 0};
    std::copy(eg, eg + 8, o);
    o[18] = 1;                     // coarse 1
    o[16] = op == 5 ? 99 : 0;      // output level: only op1 (stored last) sounds
  }
  for (int i = 0; i < 4; ++i) { v[126 + i] = 99; v[130 + i] = 50; }  // flat pitch EG
  v[134] = 31;  // algorithm 32
  v[144] = 24;  // no transpose
  const char name[] = "DECAY TEST";
  std::copy(name, name + 10, v.begin() + 145);
  return v;
}

}  // namespace

TEST_CASE("FM: a note started right after a voice load keeps its attack and decay") {
  // Regression: Synth_Dexed's post-load refresh used to jump such notes to
  // envelope stage 3, which for this voice is silence.
  FmEngine fm;
  fm.loadVoice(decayingVoice());
  fm.noteOn(60, 100);
  const auto r = render(fm, 100, [](std::size_t) {});
  CHECK(peak(r.left, 0, 20 * kBlockSize) > 0.05f);
}

TEST_CASE("FM: silence with no notes") {
  FmEngine fm;
  const auto r = render(fm, 200, [](std::size_t) {});
  CHECK(peak(r.left) == 0.0f);
  CHECK(fm.voiceCount() == 0);
}

TEST_CASE("FM: a note sounds, stays finite and in range, and stops after release") {
  FmEngine fm;
  const std::size_t on = 10, off = 400, total = 2000;
  const auto r = render(fm, total, [&](std::size_t b) {
    if (b == on) fm.noteOn(60, 100);
    if (b == off) fm.noteOff(60);
  });
  for (float s : r.left) REQUIRE(std::isfinite(s));
  CHECK(peak(r.left, 0, on * kBlockSize) == 0.0f);
  const float sounding = peak(r.left, (on + 20) * kBlockSize, off * kBlockSize);
  CHECK(sounding > 0.01f);
  CHECK(sounding < 1.0f);
  // INIT VOICE has a fast release; well after note-off it must be silent and the voice retired.
  CHECK(peak(r.left, (total - 100) * kBlockSize) < 1e-4f);
  CHECK(fm.voiceCount() == 0);
  CHECK(r.left == r.right);  // mono engine, centred
}

TEST_CASE("FM: polyphony and panic") {
  FmEngine fm;
  render(fm, 20, [&](std::size_t b) {
    if (b == 0)
      for (uint8_t n = 60; n < 64; ++n) fm.noteOn(n, 100);
  });
  CHECK(fm.voiceCount() == 4);
  fm.allNotesOff();
  const auto r = render(fm, 40, [](std::size_t) {});
  CHECK(fm.voiceCount() == 0);
  // Voices stop at once; what's left is Dexed's DC-blocking filter settling.
  CHECK(peak(r.left) < 0.001f);                   // below −60 dB immediately
  CHECK(peak(r.left, 32 * kBlockSize) < 1e-5f);   // below −100 dB soon after
}

TEST_CASE("FM: renders identically in any chunk size") {
  auto run = [](std::size_t chunk) {
    FmEngine fm;
    fm.loadVoice(decayingVoice());
    fm.noteOn(60, 100);
    std::vector<float> l(64 * 50, 0.0f), r(64 * 50, 0.0f);
    for (std::size_t pos = 0; pos < l.size();) {
      const std::size_t n = std::min(chunk, l.size() - pos);
      fm.render(l.data() + pos, r.data() + pos, n);
      pos += n;
    }
    return l;
  };
  CHECK(run(64) == run(17));
  CHECK(run(64) == run(1));
}

TEST_CASE("FM: polyphony parameter limits voices") {
  FmEngine fm;
  fm.setParam(FmEngine::kPoly, 2);
  render(fm, 5, [&](std::size_t b) {
    if (b == 0)
      for (uint8_t n = 60; n < 66; ++n) fm.noteOn(n, 100);
  });
  CHECK(fm.voiceCount() == 2);
}

TEST_CASE("FM: output is deterministic") {
  auto once = [] {
    FmEngine fm;
    return render(fm, 300, [&](std::size_t b) {
      if (b == 5) fm.noteOn(64, 90);
      if (b == 7) fm.noteOn(67, 90);
      if (b == 200) fm.noteOff(64);
    });
  };
  CHECK(once().left == once().left);
}
