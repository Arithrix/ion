#include <doctest.h>

#include <cmath>
#include <memory>
#include <random>
#include <vector>

#include "engine/fm/fm_engine.h"
#include "engine/module.h"

using namespace ion;

namespace {

std::unique_ptr<Module> makeModule(int fmTrack = 0) {
  auto m = std::make_unique<Module>();
  Command c;
  c.type = Command::Type::SetEngine;
  c.track = static_cast<uint8_t>(fmTrack);
  c.engine = new FmEngine();
  m->pushCommand(c);
  return m;
}

float peak(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
  float p = 0;
  for (std::size_t i = from; i < std::min(to, x.size()); ++i) p = std::max(p, std::fabs(x[i]));
  return p;
}

void setParam(Module& m, ParamRef ref, float v) {
  Command c;
  c.type = Command::Type::SetParam;
  c.param = ref;
  c.value = v;
  m.pushCommand(c);
}

// First output frame (interleaved index / 2) above a threshold.
std::size_t onset(const std::vector<float>& x, float threshold = 1e-4f) {
  for (std::size_t i = 0; i < x.size(); ++i)
    if (std::fabs(x[i]) > threshold) return i / 2;
  return SIZE_MAX;
}

}  // namespace

TEST_CASE("module: MIDI on channel 1 plays track 1; other channels don't") {
  auto m = makeModule();
  std::vector<float> out(2 * 4096);

  m->pushMidi({{0x91, 60, 100}});  // channel 2: no engine there
  m->process(out.data(), 4096);
  CHECK(peak(out) == 0.0f);

  m->pushMidi({{0x90, 60, 100}});  // channel 1
  m->process(out.data(), 4096);
  CHECK(peak(out) > 0.01f);
  CHECK(m->status().voices.load() == 1);
  CHECK(m->status().trackNotes[0].load() == 1);
  CHECK(m->status().midiReceived.load() == 2);
}

TEST_CASE("module: channel map routes and layers") {
  auto m = makeModule(2);  // FM on track 3, listening on channel 3 by default
  std::vector<float> out(2 * 2048);
  m->pushMidi({{0x92, 60, 100}});
  m->process(out.data(), 2048);
  CHECK(m->status().trackNotes[2].load() == 1);

  Command c;
  c.type = Command::Type::SetChannel;
  c.track = 2;
  c.value = 9;  // channel 10
  m->pushCommand(c);
  m->pushMidi({{0x92, 62, 100}});
  m->pushMidi({{0x99, 64, 100}});
  m->process(out.data(), 2048);
  CHECK(m->status().trackNotes[2].load() == 2);  // only the channel-10 note
}

TEST_CASE("module: note-on with velocity 0 is a note-off") {
  auto m = makeModule();
  std::vector<float> out(2 * 512);
  m->pushMidi({{0x90, 60, 100}});
  m->process(out.data(), 512);
  m->pushMidi({{0x90, 60, 0}});
  for (int i = 0; i < 400; ++i) m->process(out.data(), 512);
  CHECK(m->status().voices.load() == 0);
}

TEST_CASE("module: output doesn't depend on the callback size") {
  auto renderWith = [](const std::vector<std::size_t>& sizes) {
    auto m = makeModule();
    m->pushMidi({{0x90, 64, 110}});
    std::vector<float> all;
    std::vector<float> buf;
    for (std::size_t n : sizes) {
      buf.assign(2 * n, 0.0f);
      m->process(buf.data(), n);
      all.insert(all.end(), buf.begin(), buf.end());
    }
    return all;
  };
  std::vector<std::size_t> aligned(48, 128);
  std::vector<std::size_t> ragged;
  for (std::size_t total = 0; total < 6144;) {
    std::size_t n = std::min<std::size_t>(6144 - total, 37 + (total * 7) % 200);
    ragged.push_back(n);
    total += n;
  }
  CHECK(renderWith(aligned) == renderWith(ragged));
}

TEST_CASE("module: frame-timed MIDI lands on FM's 64-frame grid, inside the callback") {
  // FM note-ons start at the first 64-frame boundary at or after the event.
  // So events at frames 1000 and 1024 sound identical, and one at 1025 sounds
  // exactly one block (64 frames) later.
  auto render = [](int64_t frame) {
    auto m = makeModule();
    m->pushMidi({{0x90, 60, 127}, MidiEvent::Timing::Frame, frame});
    std::vector<float> out(2 * 4096);
    m->process(out.data(), 4096);  // one callback: timing is within it
    return out;
  };
  const auto at1000 = render(1000), at1024 = render(1024), at1025 = render(1025);
  CHECK(at1000 == at1024);
  CHECK(onset(at1000) >= 1024 + Limiter::kLookahead);
  CHECK(onset(at1025) == onset(at1024) + 64);
}

TEST_CASE("module: live MIDI timing — tight mode is jitter-free") {
  // Two events that arrived 1 ms apart, at different points of the previous
  // cycle, keep their 48-frame spacing (to FM's 64-frame grid: same or next block).
  auto m = makeModule();
  const int64_t cycle = 10'000'000'000;  // the cycle starts at t = 10 s
  std::vector<float> out(2 * 256);
  m->process(out.data(), 256, cycle - 5'333'333);  // one earlier 256-frame cycle
  m->pushMidi({{0x90, 60, 100}, MidiEvent::Timing::Monotonic, cycle - 4'000'000});  // 4 ms before
  m->process(out.data(), 256, cycle);
  // 4 ms before the cycle → 256 − 192 = 64 frames into it (tight adds one buffer).
  // (+ the master limiter's lookahead.)
  CHECK(onset(out) >= 64 + Limiter::kLookahead);
  CHECK(onset(out) < 64 + 64 + 8 + Limiter::kLookahead);
}

TEST_CASE("module: fader, pan, mute and solo") {
  auto run = [](auto setup) {
    auto m = makeModule();
    setup(*m);
    m->pushMidi({{0x90, 60, 100}});
    std::vector<float> out(2 * 4800);
    for (int i = 0; i < 3; ++i) m->process(out.data(), 4800);
    float l = 0, r = 0;
    for (std::size_t i = 0; i < out.size(); i += 2) {
      l = std::max(l, std::fabs(out[i]));
      r = std::max(r, std::fabs(out[i + 1]));
    }
    return std::pair{l, r};
  };
  const auto base = run([](Module&) {});
  CHECK(base.first == doctest::Approx(base.second));

  const auto minus6 = run([](Module& m) { setParam(m, {0, kFader}, -6.0f); });
  CHECK(minus6.first == doctest::Approx(base.first * 0.501f).epsilon(0.02));

  const auto hardLeft = run([](Module& m) { setParam(m, {0, kPan}, -1.0f); });
  CHECK(hardLeft.second < 1e-6f);
  CHECK(hardLeft.first == doctest::Approx(base.first * 1.4142f).epsilon(0.02));

  CHECK(run([](Module& m) { setParam(m, {0, kMute}, 1.0f); }).first < 1e-6f);
  // Soloing another track silences this one (solo in place).
  CHECK(run([](Module& m) { setParam(m, {3, kSolo}, 1.0f); }).first < 1e-6f);
  CHECK(run([](Module& m) { setParam(m, {0, kSolo}, 1.0f); }).first == doctest::Approx(base.first));
}

TEST_CASE("module: fader moves don't click") {
  auto m = makeModule();
  m->pushMidi({{0x90, 48, 100}});
  std::vector<float> out(2 * 64 * 200), buf(2 * 64);
  for (int b = 0; b < 200; ++b) {
    if (b == 100) setParam(*m, {0, kFader}, -30.0f);  // a big jump in one step
    m->process(buf.data(), 64);
    std::copy(buf.begin(), buf.end(), out.begin() + b * 128);
  }
  // Largest sample-to-sample step around the jump stays within what the
  // signal itself does before it.
  auto maxStep = [&](std::size_t from, std::size_t to) {
    float s = 0;
    for (std::size_t i = from + 2; i < to; i += 2) s = std::max(s, std::fabs(out[i] - out[i - 2]));
    return s;
  };
  CHECK(maxStep(99 * 128, 102 * 128) <= maxStep(90 * 128, 99 * 128) * 1.05f);
}

TEST_CASE("module: CC map and learn") {
  auto m = makeModule();
  std::vector<float> out(2 * 256);

  Command learn;
  learn.type = Command::Type::LearnCc;
  learn.param = {0, kFader};
  m->pushCommand(learn);
  m->process(out.data(), 256);
  CHECK(m->status().learning.load());
  m->pushMidi({{0xB3, 74, 10}});  // cc 74 on channel 4
  m->process(out.data(), 256);
  auto n = m->popNotification();
  REQUIRE(n);
  CHECK(n->type == Notification::Type::CcLearned);
  CHECK(n->cc == 74);
  CHECK(n->channel == 3);
  CHECK(n->param == ParamRef{0, kFader});
  CHECK_FALSE(m->status().learning.load());

  Command map;
  map.type = Command::Type::SetCcMapping;
  map.slot = 0;
  map.mapping = {true, 3, 74, {0, kFader}, 0.0f, 1.0f};
  m->pushCommand(map);
  m->pushMidi({{0xB3, 74, 127}});
  m->process(out.data(), 256);
  CHECK(m->params().get({0, kFader}) == doctest::Approx(6.0f));
  m->pushMidi({{0xB3, 74, 0}});
  m->process(out.data(), 256);
  CHECK(m->params().get({0, kFader}) <= kFaderMinDb);
  m->pushMidi({{0xB2, 74, 127}});  // same CC, other channel: not mapped
  m->process(out.data(), 256);
  CHECK(m->params().get({0, kFader}) <= kFaderMinDb);
}

TEST_CASE("module: engine swap hands the old engine back") {
  auto m = makeModule();
  std::vector<float> out(2 * 256);
  m->process(out.data(), 256);
  CHECK(m->status().trackEngine[0].load() == static_cast<uint8_t>(EngineType::Fm));
  Command c;
  c.type = Command::Type::SetEngine;
  c.track = 0;
  c.engine = nullptr;
  m->pushCommand(c);
  m->process(out.data(), 256);
  auto n = m->popNotification();
  REQUIRE(n);
  CHECK(n->type == Notification::Type::EngineRetired);
  CHECK(n->engine != nullptr);
  delete n->engine;
  CHECK(m->status().trackEngine[0].load() == static_cast<uint8_t>(EngineType::None));
}

TEST_CASE("module: panic command silences everything") {
  auto m = makeModule();
  std::vector<float> out(2 * 256);
  for (uint8_t n = 60; n < 66; ++n) m->pushMidi({{0x90, n, 100}});
  m->process(out.data(), 256);
  CHECK(m->status().voices.load() == 6);
  Command c;
  c.type = Command::Type::Panic;
  m->pushCommand(c);
  m->process(out.data(), 256);
  CHECK(m->status().voices.load() == 0);
  CHECK(peak(out) < 0.001f);  // only the DC blocker settling (see test_fm)
}

TEST_CASE("module: output never exceeds the safety ceiling") {
  auto m = makeModule();
  setParam(*m, {0, kFader}, 6.0f);
  setParam(*m, {kMasterScope, kFader}, 6.0f);
  std::vector<float> out(2 * 1024);
  for (uint8_t n = 36; n < 44; ++n) m->pushMidi({{0x90, n, 127}});
  float worst = 0;
  for (int i = 0; i < 50; ++i) {
    m->process(out.data(), 1024);
    worst = std::max(worst, peak(out));
  }
  CHECK(worst <= 0.9660509f);
}

TEST_CASE("module: MIDI clock sets tempo and transport") {
  auto m = makeModule();
  // 124 BPM: 24 ticks per beat → a tick every 60/124/24 s = 967.7 frames.
  const double interval = 60.0 / 124.0 / 24.0 * kSampleRate;
  m->pushMidi({{0xFA}, MidiEvent::Timing::Frame, 0});
  for (int i = 0; i < 200; ++i)
    m->pushMidi({{0xF8}, MidiEvent::Timing::Frame, static_cast<int64_t>(i * interval)});
  std::vector<float> out(2 * 4096);
  for (int i = 0; i < 48; ++i) m->process(out.data(), 4096);
  CHECK(m->status().clockPresent.load());
  CHECK(m->status().running.load());
  CHECK(m->status().bpm.load() == doctest::Approx(124.0).epsilon(0.002));
  CHECK(m->status().bar.load() == 2);  // 200 ticks = 2 bars and change
  m->pushMidi({{0xFC}, MidiEvent::Timing::Frame, 48 * 4096});
  for (int i = 0; i < 20; ++i) m->process(out.data(), 4096);
  CHECK_FALSE(m->status().running.load());
  CHECK_FALSE(m->status().clockPresent.load());  // no ticks for > 0.5 s
}

TEST_CASE("clock follower: right within a beat, steady under jitter, follows changes") {
  ClockFollower c(kSampleRate);
  std::mt19937 rng(11);
  std::uniform_real_distribution<double> jitter(-48.0, 48.0);  // ±1 ms, like USB MIDI
  const double interval124 = 60.0 / 124.0 / 24.0 * kSampleRate;
  double t = 0;
  for (int i = 0; i < 24; ++i, t += interval124) c.tick(static_cast<int64_t>(t + jitter(rng)));
  CHECK(c.bpm() == doctest::Approx(124.0).epsilon(0.005));  // after one beat: within 0.5%
  double lo = 1e9, hi = 0;
  for (int i = 0; i < 24 * 16; ++i, t += interval124) {
    c.tick(static_cast<int64_t>(t + jitter(rng)));
    if (i > 48) {
      lo = std::min(lo, c.bpm());
      hi = std::max(hi, c.bpm());
    }
  }
  CHECK(hi - lo < 0.3);  // steady: well under a third of a BPM of wobble
  // The OXI changes tempo to 130: followed within two beats.
  const double interval130 = 60.0 / 130.0 / 24.0 * kSampleRate;
  for (int i = 0; i < 48; ++i, t += interval130) c.tick(static_cast<int64_t>(t + jitter(rng)));
  CHECK(c.bpm() == doctest::Approx(130.0).epsilon(0.005));
}
