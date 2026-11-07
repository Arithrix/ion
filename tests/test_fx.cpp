// Effects (HANDOFF §7): each processor on its own.

#include <doctest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>
#include <vector>

#include "core/denormals.h"
#include "engine/engine.h"
#include "engine/fx/effects.h"

using namespace ion;

namespace {

struct Stereo {
  std::vector<float> l, r;
  explicit Stereo(std::size_t n = 0) : l(n, 0.0f), r(n, 0.0f) {}
  std::size_t size() const { return l.size(); }
};

// Runs `fx` over `in` in 64-frame blocks, in place.
template <class Fx>
void run(Fx& fx, Stereo& s) {
  disableDenormals();
  for (std::size_t i = 0; i < s.size(); i += kBlockSize) {
    const std::size_t n = std::min(kBlockSize, s.size() - i);
    fx.process(s.l.data() + i, s.r.data() + i, n);
  }
}

Stereo sine(double hz, std::size_t n, float amp) {
  Stereo s(n);
  for (std::size_t i = 0; i < n; ++i) s.l[i] = s.r[i] = amp * static_cast<float>(std::sin(2 * std::numbers::pi * hz * static_cast<double>(i) / kSampleRate));
  return s;
}

Stereo noise(std::size_t n, float amp, uint32_t seed = 1) {
  Stereo s(n);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> u(-amp, amp);
  for (std::size_t i = 0; i < n; ++i) {
    s.l[i] = u(rng);
    s.r[i] = u(rng);
  }
  return s;
}

float peak(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
  float p = 0;
  for (std::size_t i = from; i < std::min(to, x.size()); ++i) p = std::max(p, std::fabs(x[i]));
  return p;
}

bool finite(const Stereo& s) {
  for (std::size_t i = 0; i < s.size(); ++i)
    if (!std::isfinite(s.l[i]) || !std::isfinite(s.r[i])) return false;
  return true;
}

std::size_t argmax(const std::vector<float>& x, std::size_t from, std::size_t to) {
  std::size_t best = from;
  for (std::size_t i = from; i < std::min(to, x.size()); ++i)
    if (std::fabs(x[i]) > std::fabs(x[best])) best = i;
  return best;
}

// RT60 from an impulse response by Schroeder backward integration, fitted
// between −5 and −35 dB.
double rt60(const Stereo& ir) {
  std::vector<double> e(ir.size());
  double sum = 0;
  for (std::size_t i = ir.size(); i-- > 0;) {
    sum += static_cast<double>(ir.l[i]) * ir.l[i] + static_cast<double>(ir.r[i]) * ir.r[i];
    e[i] = sum;
  }
  auto at = [&](double db) {
    for (std::size_t i = 0; i < e.size(); ++i)
      if (10 * std::log10(e[i] / e[0]) <= db) return static_cast<double>(i);
    return static_cast<double>(e.size());
  };
  return (at(-35) - at(-5)) / kSampleRate * 2.0;
}

}  // namespace

// ---- compressor ----------------------------------------------------------------------

TEST_CASE("compressor: the static curve has the right threshold, ratio and knee") {
  Compressor c;
  c.setParam(Compressor::kThreshold, -20);
  c.setParam(Compressor::kRatio, 4);
  c.setParam(Compressor::kKnee, 0);
  CHECK(c.curve(-30) == doctest::Approx(-30));
  CHECK(c.curve(-20) == doctest::Approx(-20));
  CHECK(c.curve(-8) == doctest::Approx(-17));  // 12 dB over, 4:1 → 3 dB over
  c.setParam(Compressor::kKnee, 10);
  CHECK(c.curve(-26) == doctest::Approx(-26));  // below the knee
  CHECK(c.curve(-14) == doctest::Approx(-20 + 6.0f / 4));  // above it
  // Inside the knee: between the two lines, and continuous at its edges.
  CHECK(c.curve(-20) < -20.0f);
  CHECK(c.curve(-20) > -20.0f - 10.0f);
  CHECK(c.curve(-25.001f) == doctest::Approx(-25.001f).epsilon(1e-3));
  CHECK(c.curve(-14.999f) == doctest::Approx(-20 + 5.001f / 4).epsilon(1e-3));
}

TEST_CASE("compressor: attack and release take about as long as they say") {
  Compressor c;
  c.setParam(Compressor::kThreshold, -18);
  c.setParam(Compressor::kRatio, 3);
  c.setParam(Compressor::kKnee, 0);
  c.setParam(Compressor::kAttack, 10);
  c.setParam(Compressor::kRelease, 100);
  // A steady −6 dB: 12 dB over at 3:1 is 8 dB of reduction.
  Stereo s(48000);
  std::fill(s.l.begin(), s.l.end(), 0.5f);
  std::fill(s.r.begin(), s.r.end(), 0.5f);
  Stereo first(480);  // the first 10 ms
  std::copy_n(s.l.begin(), 480, first.l.begin());
  std::copy_n(s.r.begin(), 480, first.r.begin());
  run(c, first);
  CHECK(c.reduction() == doctest::Approx(8 * (1 - std::exp(-1.0f))).epsilon(0.03));  // one time constant
  run(c, s);
  CHECK(c.reduction() == doctest::Approx(8).epsilon(0.01));
  CHECK(s.l.back() == doctest::Approx(0.5f * std::pow(10.0f, -8 / 20.0f)).epsilon(0.01));
  // Quiet again: after one release time the reduction is down to 37%.
  Stereo quiet(4800);
  std::fill(quiet.l.begin(), quiet.l.end(), 0.001f);
  std::fill(quiet.r.begin(), quiet.r.end(), 0.001f);
  run(c, quiet);
  CHECK(c.reduction() == doctest::Approx(8 * std::exp(-1.0f)).epsilon(0.03));

  // Makeup and mix.
  Compressor d;
  d.setParam(Compressor::kThreshold, 0);
  d.setParam(Compressor::kMakeup, 6);
  Stereo t = sine(100, 4800, 0.1f);
  run(d, t);
  CHECK(peak(t.l) == doctest::Approx(0.1f * 1.995f).epsilon(0.01));
  d.setParam(Compressor::kMix, 0);
  Stereo u = sine(100, 4800, 0.1f);
  run(d, u);
  CHECK(peak(u.l) == doctest::Approx(0.1f).epsilon(0.001));
}

// ---- chorus ------------------------------------------------------------------------------

TEST_CASE("chorus: dry at 0% mix, mono stays mono at 0% width, always finite") {
  Chorus c;
  c.setParam(Chorus::kMix, 0);
  Stereo s = sine(440, 4800, 0.5f), ref = s;
  run(c, s);
  CHECK(s.l == ref.l);

  Chorus w;
  w.setParam(Chorus::kWidth, 0);
  w.setParam(Chorus::kDepth, 1);
  w.setParam(Chorus::kRate, 5);
  Stereo m = sine(440, 48000, 0.5f);
  run(w, m);
  CHECK(m.l == m.r);
  CHECK(peak(m.l) < 0.75f);

  Chorus x;
  x.setParam(Chorus::kWidth, 1);
  x.setParam(Chorus::kMix, 1);
  Stereo n = noise(48000, 1);
  run(x, n);
  CHECK(finite(n));
  CHECK(m.l != n.l);
}

// ---- delay -------------------------------------------------------------------------------

TEST_CASE("delay: echoes land on time, in ms and synced, and repeat at the feedback level") {
  Delay d;
  d.setWetOnly(true);
  d.setParam(Delay::kSync, 0);
  d.setParam(Delay::kTime, 100);
  d.setParam(Delay::kFeedback, 0.5f);
  d.setParam(Delay::kHighCut, 20000);
  d.setParam(Delay::kLowCut, 20);
  Stereo s(20000);
  s.l[0] = s.r[0] = 1;
  run(d, s);
  const std::size_t first = argmax(s.l, 100, 7000), second = argmax(s.l, 7000, 12000);
  CHECK(std::abs(static_cast<int>(first) - 4800) <= 2);
  CHECK(std::abs(static_cast<int>(second) - 9600) <= 3);
  CHECK(std::fabs(s.l[second]) / std::fabs(s.l[first]) == doctest::Approx(0.5).epsilon(0.08));
  CHECK(peak(s.l, 0, 4700) < 0.01f);  // wet only: no dry signal

  // Synced: 1/8 dotted at 120 BPM is 0.375 s; at 90 BPM, 0.5 s.
  Delay e;
  e.setParam(Delay::kDivision, 6);
  CHECK(e.targetSamples() == doctest::Approx(18000));
  e.setTempo(90);
  CHECK(e.targetSamples() == doctest::Approx(24000));
  e.setParam(Delay::kDivision, 4);  // 1/8 triplet
  CHECK(e.targetSamples() == doctest::Approx(0.5 * 2 / 3 * 60 / 90 * 48000));
  for (int div = 0; div < 12; ++div) {
    e.setParam(Delay::kDivision, static_cast<float>(div));
    e.setTempo(40);  // slowest tempos still fit (or clamp)
    CHECK(e.targetSamples() <= Delay::kSize);
  }
}

TEST_CASE("delay: ping-pong alternates sides; a tempo change glides; full feedback stays bounded") {
  Delay d;
  d.setWetOnly(true);
  d.setParam(Delay::kSync, 0);
  d.setParam(Delay::kTime, 50);
  d.setParam(Delay::kPingPong, 1);
  d.setParam(Delay::kFeedback, 0.8f);
  Stereo s(12000);
  s.l[0] = 1;
  run(d, s);
  CHECK(peak(s.l, 2300, 2500) > 0.1f);  // first echo left
  CHECK(peak(s.r, 2300, 2500) < 0.01f);
  CHECK(peak(s.r, 4700, 4900) > 0.1f);  // second right
  CHECK(peak(s.l, 4700, 4900) < 0.01f);

  // Changing tempo under a running delay never jumps the read point: no click.
  Delay g;
  g.setWetOnly(true);
  Stereo tone = sine(200, 96000, 0.3f);
  Stereo a(48000), b(48000);
  std::copy_n(tone.l.begin(), 48000, a.l.begin());
  std::copy_n(tone.r.begin(), 48000, a.r.begin());
  std::copy_n(tone.l.begin() + 48000, 48000, b.l.begin());
  std::copy_n(tone.r.begin() + 48000, 48000, b.r.begin());
  run(g, a);
  g.setTempo(150);
  run(g, b);
  float step = 0;
  for (std::size_t i = 1; i < b.size(); ++i) step = std::max(step, std::fabs(b.l[i] - b.l[i - 1]));
  CHECK(step < 0.06f);  // a 200 Hz sine at 0.3 steps ≤ 0.008; repeats add up, but nothing jumps

  Delay f;
  f.setParam(Delay::kFeedback, 1);
  f.setParam(Delay::kMix, 1);
  Stereo n = noise(48000 * 10, 1);
  run(f, n);
  CHECK(finite(n));
  CHECK(peak(n.l) < 4.0f);
}

// ---- reverb ------------------------------------------------------------------------------

TEST_CASE("reverb: each type decays in about the time set, and stays stable") {
  for (int kind : {Reverb::kPlate, Reverb::kHall, Reverb::kRoom})
    for (float decayMs : {800.0f, 3000.0f}) {
      Reverb r;
      r.setWetOnly(true);
      r.setParam(Reverb::kType, static_cast<float>(kind));
      r.setParam(Reverb::kDecay, decayMs);
      r.setParam(Reverb::kDamping, 0);
      r.setParam(Reverb::kLowCut, 20);
      r.setParam(Reverb::kHighCut, 20000);
      Stereo ir(static_cast<std::size_t>(decayMs / 1000.0f * 48000 * 2));
      ir.l[0] = ir.r[0] = 1;
      run(r, ir);
      CAPTURE(kind);
      CAPTURE(decayMs);
      const double t60 = rt60(ir);
      CHECK(t60 > decayMs / 1000.0 * 0.6);
      CHECK(t60 < decayMs / 1000.0 * 1.4);
      CHECK(peak(ir.l) < 1.0f);
      // Both sides carry it, and they differ (it's stereo).
      CHECK(peak(ir.r) > 0.01f);
      CHECK(ir.l != ir.r);
    }

  // Noise in, every setting swept, for 10 s.
  Reverb r;
  r.setParam(Reverb::kMix, 1);
  bool ok = true;
  float loudest = 0;
  for (int i = 0; i < 20; ++i) {
    r.setParam(Reverb::kSize, static_cast<float>(i % 2));
    r.setParam(Reverb::kDecay, i % 3 ? 20000.0f : 200.0f);
    r.setParam(Reverb::kType, static_cast<float>(i % 3));
    r.setParam(Reverb::kPreDelay, static_cast<float>((i * 37) % 200));
    r.setParam(Reverb::kMod, 1);
    Stereo n = noise(24000, 0.5f, static_cast<uint32_t>(i));
    run(r, n);
    ok &= finite(n);
    loudest = std::max(loudest, peak(n.l));
  }
  CHECK(ok);
  CHECK(loudest < 4.0f);
}

TEST_CASE("reverb: dry at 0% mix; pre-delay delays the onset") {
  Reverb r;
  r.setParam(Reverb::kMix, 0);
  Stereo s = sine(300, 4800, 0.5f), ref = s;
  run(r, s);
  CHECK(s.l == ref.l);

  Reverb p;
  p.setWetOnly(true);
  p.setParam(Reverb::kPreDelay, 100);
  p.setParam(Reverb::kType, Reverb::kPlate);
  Stereo ir(24000);
  ir.l[0] = ir.r[0] = 1;
  run(p, ir);
  CHECK(peak(ir.l, 0, 4700) < 1e-4f);
  CHECK(peak(ir.l, 4800, 24000) > 1e-3f);
}

// ---- limiter -------------------------------------------------------------------------------

TEST_CASE("limiter: never above the ceiling, transparent below it, 1 ms late") {
  Limiter lim;
  Stereo loud = noise(48000 * 4, 4.0f);
  // Sudden single-sample spikes too, the hardest case for a limiter.
  for (std::size_t i = 1000; i < loud.size(); i += 7919) loud.l[i] = 20.0f;
  run(lim, loud);
  CHECK(peak(loud.l) <= Limiter::kCeiling);
  CHECK(peak(loud.r) <= Limiter::kCeiling);
  CHECK(lim.reduction() > 6.0f);

  Limiter quiet;
  Stereo s = sine(1000, 9600, 0.5f), out = s;
  run(quiet, out);
  for (std::size_t i = Limiter::kLookahead; i < s.size(); ++i) CHECK(out.l[i] == s.l[i - Limiter::kLookahead]);
  CHECK(quiet.reduction() == 0.0f);

  // A burst over the ceiling is turned down smoothly: the gain ramps ahead of it.
  Limiter ramp;
  Stereo b = sine(100, 48000, 0.3f);
  for (std::size_t i = 24000; i < 26400; ++i) {
    b.l[i] *= 6;
    b.r[i] *= 6;
  }
  Stereo ref = b;
  run(ramp, b);
  CHECK(peak(b.l) <= Limiter::kCeiling);
  float worst = 0;  // how far the gain changes per sample, where the signal is big enough to measure it
  for (std::size_t i = 23000 + Limiter::kLookahead; i < 27000; ++i) {
    const float in = ref.l[i - Limiter::kLookahead], in0 = ref.l[i - 1 - Limiter::kLookahead];
    if (std::fabs(in) > 0.2f && std::fabs(in0) > 0.2f) worst = std::max(worst, std::fabs(b.l[i] / in - b.l[i - 1] / in0));
  }
  CHECK(worst < 0.05f);
}

// ---- through the module and the session ------------------------------------------------------

#include <filesystem>
#include <memory>
#include <thread>

#include "engine/drum/drum_engine.h"
#include "engine/module.h"
#include "project/config.h"
#include "project/paths.h"
#include "project/session.h"

namespace {

namespace fs = std::filesystem;

struct TempDir {
  fs::path path = fs::temp_directory_path() / ("ion-fx-" + std::to_string(std::random_device{}()));
  TempDir() { fs::create_directories(path); }
  ~TempDir() { fs::remove_all(path); }
};

// A module with a drum track 1 whose pad 1 is a single click: impulses in,
// so where the sound goes (and when) is easy to see.
struct Rig {
  std::shared_ptr<Sample> click = [] {
    auto s = std::make_shared<Sample>();
    s->frames = 64;
    s->data.assign(64 + 2 * Sample::kGuard, 0.0f);
    s->data[Sample::kGuard] = 0.5f;
    return s;
  }();
  Module m;
  Rig() {
    Command c;
    c.type = Command::Type::SetEngine;
    c.engine = new DrumEngine();
    m.pushCommand(c);
    Command smp;
    smp.type = Command::Type::SetSample;
    smp.sample = click.get();
    m.pushCommand(smp);
    set(engineParamRef(0, DrumEngine::param(0, DrumEngine::kVelLevel)), 0);
  }
  void set(ParamRef ref, float v) {
    Command c;
    c.type = Command::Type::SetParam;
    c.param = ref;
    c.value = v;
    m.pushCommand(c);
  }
  void insert(int scope, int slot, EffectType type) {
    Command c;
    c.type = Command::Type::SetInsert;
    c.scope = static_cast<uint8_t>(scope);
    c.slot = slot;
    c.effect = createEffect(type).release();
    m.pushCommand(c);
  }
  // Hits pad 1 and renders `frames`; returns the left channel.
  std::vector<float> hit(int frames, bool note = true) {
    if (note) m.pushMidi({{0x90, 36, 127}});
    std::vector<float> out(2 * static_cast<std::size_t>(frames)), left(static_cast<std::size_t>(frames));
    for (int done = 0; done < frames; done += 256) m.process(out.data() + 2 * done, std::min(256, frames - done));
    for (int i = 0; i < frames; ++i) left[static_cast<std::size_t>(i)] = out[2 * static_cast<std::size_t>(i)];
    return left;
  }
};

constexpr std::size_t kL = Limiter::kLookahead;

}  // namespace

TEST_CASE("sends: a track reaches the delay return before or after its fader; mute stops it") {
  Rig r;
  // The delay return: 100 ms, one repeat only.
  r.set(insertParamRef(kDelayScope, 0, Delay::kSync), 0);
  r.set(insertParamRef(kDelayScope, 0, Delay::kTime), 100);
  r.set(insertParamRef(kDelayScope, 0, Delay::kFeedback), 0);
  r.set({0, kSendA}, 0);
  r.hit(256, false);  // sends ramp up over a block: let it
  auto out = r.hit(9600);
  CHECK(peak(out, kL, kL + 64) > 0.4f);           // the dry hit
  CHECK(peak(out, kL + 4700, kL + 4900) > 0.1f);  // its echo from the return
  CHECK(peak(out, kL + 64, kL + 4700) < 1e-3f);

  // Fader down: a post-fader send goes too; pre-fader, only the echo is left.
  r.set({0, kFader}, kFaderMinDb);
  r.hit(9600, false);  // let ramps and the old echo settle
  out = r.hit(9600);
  CHECK(peak(out) < 1e-3f);
  r.set({0, kSendPre}, 1);
  r.hit(9600, false);
  out = r.hit(9600);
  CHECK(peak(out, kL, kL + 64) < 1e-3f);
  CHECK(peak(out, kL + 4700, kL + 4900) > 0.1f);

  r.set({0, kMute}, 1);
  r.hit(9600, false);
  out = r.hit(9600);
  CHECK(peak(out) < 1e-3f);
}

TEST_CASE("returns: the delay feeds the reverb; returns have faders and ignore solo") {
  Rig r;
  r.set(insertParamRef(kDelayScope, 0, Delay::kSync), 0);
  r.set(insertParamRef(kDelayScope, 0, Delay::kTime), 50);
  r.set(insertParamRef(kDelayScope, 0, Delay::kFeedback), 0);
  r.set({0, kSendA}, 0);
  r.set({kDelayScope, kSendB}, 0);  // the delay return also sends into the reverb (after its fader)
  r.hit(256, false);
  auto out = r.hit(24000);
  CHECK(peak(out, kL + 2500, 24000) > 1e-3f);  // reverb tail after the delay time
  CHECK(r.m.status().returns[1].peakLeft.load() > 0);

  // Solo on another track silences track 1, but not the returns.
  r.set({0, kSendA}, kFaderMinDb);
  r.set({kDelayScope, kSendB}, kFaderMinDb);
  r.set({0, kSendB}, 0);
  r.set({0, kSendPre}, 1);
  r.set({1, kSolo}, 1);
  r.hit(24000, false);
  out = r.hit(24000);
  CHECK(peak(out) < 1e-3f);  // muted by solo: no sends either
}

TEST_CASE("inserts: a compressor works and reports, bypass fades, slot 1 of a return is fixed") {
  Rig r;
  // A long 0.5 tone instead of the click.
  auto tone = std::make_shared<Sample>();
  tone->frames = 48000;
  tone->data.assign(48000 + 2 * Sample::kGuard, 0.5f);
  Command smp;
  smp.type = Command::Type::SetSample;
  smp.slot = 1;
  smp.sample = tone.get();
  r.m.pushCommand(smp);
  r.insert(0, 0, EffectType::Compressor);
  r.set(insertParamRef(0, 0, Compressor::kThreshold), -18);
  r.set(insertParamRef(0, 0, Compressor::kRatio), 4);
  r.set(insertParamRef(0, 0, Compressor::kKnee), 0);
  r.m.pushMidi({{0x90, 37, 127}});
  auto out = r.hit(24000, false);
  CHECK(r.m.status().reduction[0][0].load() == doctest::Approx(9).epsilon(0.05));  // 12 dB over at 4:1
  CHECK(out[20000] == doctest::Approx(0.5f * std::pow(10.0f, -9 / 20.0f)).epsilon(0.05));

  r.set(insertOnRef(0, 0), 0);  // bypass: back to 0.5, without a step
  out = r.hit(4800, false);
  CHECK(out[4000] == doctest::Approx(0.5f).epsilon(0.01));
  float step = 0;
  for (std::size_t i = 1; i < 2000; ++i) step = std::max(step, std::fabs(out[i] - out[i - 1]));
  CHECK(step < 0.01f);
  CHECK(r.m.status().reduction[0][0].load() == 0.0f);

  // The reverb return's own reverb can't be replaced; the refused effect comes back to be freed.
  r.insert(kReverbScope, 0, EffectType::Chorus);
  r.hit(64, false);
  bool retired = false;
  while (auto n = r.m.popNotification())
    if (n->type == Notification::Type::EffectRetired) {
      retired = true;
      CHECK(n->effect->type() == EffectType::Chorus);
      delete n->effect;
    }
  CHECK(retired);
}

TEST_CASE("master: the limiter holds the ceiling whatever goes in") {
  Rig r;
  r.set({0, kFader}, 6);
  r.set(engineParamRef(0, DrumEngine::param(0, DrumEngine::kLevel)), 6);
  r.set({kMasterScope, kFader}, 6);
  float worst = 0;
  for (int i = 0; i < 40; ++i) {
    r.m.pushMidi({{0x90, 36, 127}});  // stacked clicks: 4 voices at 0.5 × 4 × 4…
    r.m.pushMidi({{0x90, 36, 127}});
    for (float v : r.hit(256, false)) worst = std::max(worst, std::fabs(v));
  }
  CHECK(worst <= Limiter::kCeiling);
  CHECK(worst > 0.5f);
  CHECK(r.m.status().limiter.load() > 0.0f);
}

namespace {
struct SessionRig {
  TempDir root;
  std::unique_ptr<Module> module = std::make_unique<Module>();
  Config config;
  Session session{*module, Paths::under(root.path), config};
  void run() {
    std::vector<float> buf(2 * 256);
    for (int i = 0; i < 4; ++i) module->process(buf.data(), 256);
    session.tick();
  }
};
}  // namespace

TEST_CASE("session: inserts, sends and returns save and load; keys parse; slots reorder") {
  SessionRig r;
  Session& s = r.session;
  s.newProject("fx");
  // A new project: the glue compressor is on the master, switched out.
  CHECK(s.insert(kMasterScope, 0) == EffectType::Compressor);
  CHECK(s.param(insertOnRef(kMasterScope, 0)) == 0.0f);
  CHECK(s.insert(kDelayScope, 0) == EffectType::Delay);
  CHECK(s.insertFixed(kReverbScope, 0));

  s.setInsert(2, 0, EffectType::Compressor);
  s.setInsert(2, 1, EffectType::Chorus);
  s.setParam(insertParamRef(2, 0, Compressor::kThreshold), -24);
  s.setParam(insertParamRef(2, 1, Chorus::kRate), 1.5f);
  s.setParam(insertOnRef(2, 1), 0);
  s.setParam({2, kSendA}, -12);
  s.setParam({2, kSendB}, -6);
  s.setParam({kReverbScope, kFader}, -3);
  s.setParam(insertParamRef(kReverbScope, 0, Reverb::kDecay), 4000);
  s.setParam(insertParamRef(kDelayScope, 0, Delay::kDivision), 4);
  s.setParam({kMasterScope, kB1Gain}, 2);
  s.setParam(insertOnRef(kMasterScope, 0), 1);
  s.addMapping(0, 20, insertParamRef(kReverbScope, 0, Reverb::kSize));
  s.addMapping(0, 21, insertParamRef(2, 0, Compressor::kRatio));
  r.run();
  CHECK(r.module->params().get(insertParamRef(2, 0, Compressor::kThreshold)) == -24.0f);

  CHECK(s.paramKey(insertParamRef(2, 0, Compressor::kRatio)) == "track3.fx1.ratio");
  CHECK(s.paramKey(insertOnRef(2, 1)) == "track3.fx2.on");
  CHECK(s.paramKey(insertParamRef(kReverbScope, 0, Reverb::kDecay)) == "reverb.fx1.decay");
  CHECK(s.paramKey({kDelayScope, kSendB}) == "delay.send.b");
  CHECK(s.paramLabel(insertParamRef(2, 0, Compressor::kRatio)) == "Track 3 · COMP 1 Ratio");
  for (ParamRef ref : {insertParamRef(2, 0, Compressor::kRatio), insertOnRef(2, 1), insertParamRef(kReverbScope, 0, Reverb::kDecay),
                       ParamRef{kDelayScope, kSendB}, ParamRef{kMasterScope, kB1Gain}}) {
    const auto back = s.parseParamKey(s.paramKey(ref));
    REQUIRE(back);
    CHECK(*back == ref);
  }

  std::string error;
  REQUIRE(s.save(error));
  SessionRig r2;
  fs::create_directories(r2.root.path / "projects");
  fs::copy(r.root.path / "projects" / "fx", r2.root.path / "projects" / "fx", fs::copy_options::recursive);
  REQUIRE(r2.session.load("fx", error));
  r2.run();
  Session& t = r2.session;
  CHECK(t.insert(2, 0) == EffectType::Compressor);
  CHECK(t.insert(2, 1) == EffectType::Chorus);
  CHECK(t.insert(2, 2) == EffectType::None);
  CHECK(t.param(insertParamRef(2, 1, Chorus::kRate)) == doctest::Approx(1.5f));
  CHECK(t.param(insertOnRef(2, 1)) == 0.0f);
  CHECK(t.param({2, kSendA}) == doctest::Approx(-12));
  CHECK(r2.module->params().get(insertParamRef(kReverbScope, 0, Reverb::kDecay)) == doctest::Approx(4000));
  CHECK(r2.module->params().get(insertParamRef(kDelayScope, 0, Delay::kDivision)) == 4.0f);
  CHECK(r2.module->params().get({kReverbScope, kFader}) == doctest::Approx(-3));
  CHECK(r2.module->params().get({kMasterScope, kB1Gain}) == doctest::Approx(2));
  CHECK(t.param(insertOnRef(kMasterScope, 0)) == 1.0f);
  CHECK(t.ccMappings().size() == 2);

  // Reorder: the chorus moves to slot 1 with its settings; the compressor to slot 2.
  t.moveInsert(2, 1, -1);
  r2.run();
  CHECK(t.insert(2, 0) == EffectType::Chorus);
  CHECK(t.insert(2, 1) == EffectType::Compressor);
  CHECK(t.param(insertParamRef(2, 0, Chorus::kRate)) == doctest::Approx(1.5f));
  CHECK(t.param(insertOnRef(2, 0)) == 0.0f);
  CHECK(r2.module->params().get(insertParamRef(2, 1, Compressor::kThreshold)) == -24.0f);
  t.moveInsert(kReverbScope, 1, -1);  // a return's own slot doesn't move
  CHECK(t.insert(kReverbScope, 0) == EffectType::Reverb);

  // A new project clears the inserts (and puts the glue compressor back, out).
  t.newProject("fresh");
  r2.run();
  CHECK(t.insert(2, 0) == EffectType::None);
  CHECK(t.param({2, kSendA}) == kFaderMinDb);
  CHECK(r2.module->params().get(insertParamRef(kReverbScope, 0, Reverb::kDecay)) == doctest::Approx(2400));
  CHECK(t.param(insertOnRef(kMasterScope, 0)) == 0.0f);
}

TEST_CASE("session: a format 3 project opens with no inserts and its master fader") {
  SessionRig r;
  const std::string old = R"({"format": 3, "tracks": [{"track": 1, "engine": "none", "channel": 1,
      "mixer": {"fader": -4.0}}], "master": {"fader": -2.5}, "cc": [], "programs": []})";
  std::string error;
  REQUIRE(r.session.fromJson(old, error));
  r.run();
  CHECK(r.session.param({kMasterScope, kFader}) == doctest::Approx(-2.5f));
  CHECK(r.session.param({0, kFader}) == doctest::Approx(-4.0f));
  CHECK(r.session.param({0, kSendA}) == kFaderMinDb);
  CHECK(r.session.insert(kMasterScope, 0) == EffectType::Compressor);
  CHECK(r.session.param(insertOnRef(kMasterScope, 0)) == 0.0f);
}
