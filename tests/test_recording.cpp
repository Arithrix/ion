// Recording (HANDOFF §7.6): the Module's takes, and the slicer.

#include <doctest.h>

#include <cmath>
#include <memory>
#include <numbers>
#include <random>
#include <vector>

#include "core/params.h"
#include "engine/module.h"
#include "library/slicer.h"

using namespace ion;

namespace {

// Plays a single-sample click of `level` at each note-on, exactly on the note's frame.
class ClickEngine final : public Engine {
 public:
  explicit ClickEngine(float level = 0.5f) : level_(level) {}
  EngineType type() const override { return EngineType::Plaits; }  // nothing the module special-cases
  void noteOn(uint8_t, uint8_t) override { pending_ = true; }
  void noteOff(uint8_t) override {}
  void controlChange(uint8_t, uint8_t) override {}
  void pitchBend(int) override {}
  void allNotesOff() override {}
  void setParam(int, float) override {}
  void render(float* left, float* right, std::size_t frames) override {
    if (pending_ && frames > 0) {
      left[0] += level_;
      right[0] += level_;
      pending_ = false;
    }
  }
  int voiceCount() const override { return 0; }

 private:
  float level_;
  bool pending_ = false;
};

constexpr int64_t kTick = 1000;  // 120 BPM: 48000 · 60 / 120 / 24

// Drives a Module in 256-frame chunks, feeding it MIDI clock and notes on exact frames.
struct Rig {
  std::unique_ptr<Module> module = std::make_unique<Module>();
  std::vector<float> buf = std::vector<float>(2 * 256);
  int64_t frame = 0;
  bool clock = true;       // send ticks
  int64_t nextTick = 0;
  std::vector<int64_t> notes;  // frames to play a note on channel 1 at
  std::vector<Notification> seen;

  explicit Rig(int track = 0, float level = 0.5f) {
    Command c;
    c.type = Command::Type::SetEngine;
    c.track = static_cast<uint8_t>(track);
    c.engine = new ClickEngine(level);
    module->pushCommand(c);
  }
  ~Rig() {
    for (const Notification& n : seen)
      if (n.type == Notification::Type::EngineRetired) delete n.engine;
  }
  void command(const Command& c) { REQUIRE(module->pushCommand(c)); }
  void midiAt(MidiMessage m, int64_t at) { module->pushMidi({m, MidiEvent::Timing::Frame, at}); }
  void start() { midiAt({0xFA}, frame); }
  void run(int64_t frames) {
    const int64_t end = frame + frames;
    while (frame < end) {
      const int64_t to = frame + 256;
      while (clock && nextTick < to) {
        midiAt({0xF8}, nextTick);
        nextTick += kTick;
      }
      for (int64_t n : notes)
        if (n >= frame && n < to) midiAt({0x90, 60, 100}, n);
      module->process(buf.data(), 256);
      frame = to;
      while (auto n = module->popNotification()) seen.push_back(*n);
    }
  }
  bool done(const Take* t) const {
    for (const Notification& n : seen)
      if (n.type == Notification::Type::TakeDone && n.take == t) return true;
    return false;
  }
};

std::unique_ptr<Take> makeTake(int bars, bool synced, RecSource source = RecSource::Tracks, uint8_t tracks = 1,
                               RecTap tap = RecTap::Pre) {
  auto t = std::make_unique<Take>();
  t->source = source;
  t->tracks = tracks;
  t->tap = tap;
  t->synced = synced;
  t->bars = bars;
  t->capacity = 48000 * 30;
  t->left.assign(static_cast<std::size_t>(t->capacity), 0.0f);
  t->right.assign(static_cast<std::size_t>(t->capacity), 0.0f);
  if (synced) t->ticks.assign(static_cast<std::size_t>((bars > 0 ? bars : 16) * 96 + 2), 0);
  return t;
}

Command record(Take* t) {
  Command c;
  c.type = Command::Type::Record;
  c.take = t;
  return c;
}

std::vector<int64_t> clicks(const Take& t, float threshold = 0.1f) {
  std::vector<int64_t> out;
  for (int64_t i = 0; i < t.frames.load(); ++i)
    if (std::fabs(t.left[static_cast<std::size_t>(i)]) > threshold) out.push_back(i);
  return out;
}

}  // namespace

TEST_CASE("recording: a synced take starts on the next downbeat, sample-exact, and runs its bars") {
  Rig r;
  r.start();
  r.run(150 * kTick);  // into bar 2 (ticks 96-191)
  auto take = makeTake(2, true);
  r.command(record(take.get()));
  r.run(10 * kTick);
  CHECK(take->state.load() == Take::State::Armed);
  // A note on the downbeat of bar 3 (tick 192) and one on beat 3 of the take's second bar.
  r.notes = {192 * kTick, 192 * kTick + 96 * kTick + 48 * kTick};
  r.run(300 * kTick);
  REQUIRE(r.done(take.get()));
  CHECK(take->state.load() == Take::State::Done);
  CHECK(take->startFrame == 192 * kTick);
  CHECK(take->startBar == 2);
  CHECK(take->bpm == doctest::Approx(120.0).epsilon(0.001));
  CHECK_FALSE(take->cutShort);
  CHECK(take->tickCount.load() == 2 * 96 + 1);  // every tick to the closing downbeat
  for (int k = 0; k <= 2 * 96; k += 17) CHECK(take->ticks[static_cast<std::size_t>(k)] == k * kTick);
  CHECK(take->frames.load() >= 2 * 96 * kTick);  // past the downbeat, to be trimmed
  CHECK(take->frames.load() < 2 * 96 * kTick + 4096);
  CHECK(clicks(*take) == std::vector<int64_t>{0, 144 * kTick});
}

TEST_CASE("recording: the master is captured in line with the bar, through the limiter's lookahead") {
  Rig r;
  r.start();
  auto take = makeTake(1, true, RecSource::Master);
  r.command(record(take.get()));
  r.notes = {0, 24 * kTick};  // the first downbeat, then beat 2
  r.run(120 * kTick);
  REQUIRE(r.done(take.get()));
  CHECK(take->startFrame == 0);
  CHECK(clicks(*take) == std::vector<int64_t>{0, 24 * kTick});
  CHECK(take->left[0] == doctest::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("recording: the post-fx tap has the fader and pan but ignores mute; pre-fx is the dry engine") {
  for (RecTap tap : {RecTap::Post, RecTap::Pre}) {
    Rig r;
    Command c;
    c.type = Command::Type::SetParam;
    c.param = {0, static_cast<uint16_t>(kFader)};
    c.value = -6.0f;
    r.command(c);
    c.param = {0, static_cast<uint16_t>(kMute)};
    c.value = 1;
    r.command(c);
    r.clock = false;
    auto take = makeTake(1, false, RecSource::Tracks, 1, tap);
    r.command(record(take.get()));
    r.notes = {1000};
    r.run(100000);
    REQUIRE(r.done(take.get()));
    const auto at = clicks(*take, 0.05f);
    REQUIRE(at.size() == 1);
    CHECK(at[0] == 1000);
    CHECK(take->left[1000] == doctest::Approx(tap == RecTap::Post ? 0.5f * faderGain(-6.0f) : 0.5f).epsilon(0.01));
  }
}

TEST_CASE("recording: only the chosen tracks are recorded") {
  Rig r(0);
  Command c;
  c.type = Command::Type::SetEngine;
  c.track = 2;
  c.engine = new ClickEngine(0.25f);
  r.command(c);
  r.clock = false;
  auto take = makeTake(1, false, RecSource::Tracks, 1 << 2);
  r.command(record(take.get()));
  r.module->pushMidi({{0x92, 60, 100}, MidiEvent::Timing::Frame, 5000});  // track 3 (channel 3)
  r.notes = {6000};                                                       // track 1: not recorded
  r.run(100000);
  REQUIRE(r.done(take.get()));
  CHECK(clicks(*take, 0.05f) == std::vector<int64_t>{5000});
}

TEST_CASE("recording: without clock, 'now' with bars counts them at 120 BPM; free runs until stopped") {
  Rig r;
  r.clock = false;
  auto take = makeTake(1, false);
  r.command(record(take.get()));
  r.run(100000);
  REQUIRE(r.done(take.get()));
  CHECK(take->frames.load() == 96000);
  CHECK(take->bpm == 120.0);

  auto free = makeTake(0, false);
  r.command(record(free.get()));
  r.run(20000);
  CHECK(free->state.load() == Take::State::Recording);
  Command stop;
  stop.type = Command::Type::StopRecord;
  r.command(stop);
  r.run(1024);
  REQUIRE(r.done(free.get()));
  CHECK(free->frames.load() >= 20000);
  CHECK(free->frames.load() < 20000 + 1024);
  CHECK_FALSE(free->cutShort);
}

TEST_CASE("recording: stop during a synced take ends it on the next downbeat") {
  Rig r;
  r.start();
  auto take = makeTake(4, true);
  r.command(record(take.get()));
  r.run(130 * kTick);  // in the take's second bar
  Command stop;
  stop.type = Command::Type::StopRecord;
  r.command(stop);
  r.run(200 * kTick);
  REQUIRE(r.done(take.get()));
  CHECK(take->targetBars.load() == 2);
  CHECK(take->tickCount.load() == 2 * 96 + 1);
  CHECK(take->frames.load() >= 2 * 96 * kTick);
}

TEST_CASE("recording: the clock stopping mid-take keeps the whole bars; disarm and cancel drop it") {
  Rig r;
  r.start();
  auto take = makeTake(4, true);
  r.command(record(take.get()));
  r.run(250 * kTick);  // 2 bars and a half
  r.midiAt({0xFC}, r.frame);
  r.clock = false;
  r.run(4096);
  REQUIRE(r.done(take.get()));
  CHECK(take->state.load() == Take::State::Done);
  CHECK(take->cutShort);
  CHECK(take->targetBars.load() == 2);

  // Armed with the transport stopped: it waits; stop disarms it.
  auto armed = makeTake(1, true);
  r.command(record(armed.get()));
  r.run(10000);
  CHECK(armed->state.load() == Take::State::Armed);
  Command stop;
  stop.type = Command::Type::StopRecord;
  r.command(stop);
  r.run(256);
  REQUIRE(r.done(armed.get()));
  CHECK(armed->state.load() == Take::State::Cancelled);

  auto cancelled = makeTake(0, false);
  r.command(record(cancelled.get()));
  r.run(4096);
  Command cancel;
  cancel.type = Command::Type::CancelRecord;
  r.command(cancel);
  r.run(256);
  REQUIRE(r.done(cancelled.get()));
  CHECK(cancelled->state.load() == Take::State::Cancelled);
}

// ---- slicing ------------------------------------------------------------------

namespace {

constexpr double kRate = 48000;

double midiHz(int note) { return 440.0 * std::pow(2.0, (note - 69) / 12.0); }

// A chord per bar (4 beats of 24000 frames at 120 BPM), restruck on every beat
// with a soft attack and a slow decay, four harmonics a note.
std::vector<float> chords(const std::vector<std::vector<int>>& bars, bool restrike = true) {
  const int64_t beat = 24000;
  std::vector<float> out(static_cast<std::size_t>(beat * 4 * static_cast<int64_t>(bars.size())), 0.0f);
  for (std::size_t b = 0; b < bars.size(); ++b)
    for (int n : bars[b])
      for (int h = 1; h <= 4; ++h) {
        const double f = midiHz(n) * h, amp = 0.06 / h;
        for (int64_t i = 0; i < beat * 4; ++i) {
          const double inBeat = restrike ? static_cast<double>(i % beat) / kRate : static_cast<double>(i) / kRate;
          const double env = std::min(1.0, inBeat / 0.01) * std::exp(-inBeat * 1.5);
          const int64_t at = static_cast<int64_t>(b) * beat * 4 + i;
          out[static_cast<std::size_t>(at)] += static_cast<float>(amp * env * std::sin(2 * std::numbers::pi * f * static_cast<double>(at) / kRate));
        }
      }
  return out;
}

}  // namespace

TEST_CASE("slicer: equal slices") {
  CHECK(equalSlices(96000, 8) == std::vector<int64_t>{0, 12000, 24000, 36000, 48000, 60000, 72000, 84000});
  CHECK(equalSlices(10, 1) == std::vector<int64_t>{0});
}

TEST_CASE("slicer: chord changes land on the bars, not on every restruck beat") {
  // C, A minor, F, G: one per bar.
  const auto x = chords({{60, 64, 67}, {57, 60, 64}, {53, 57, 60}, {55, 59, 62}});
  CHECK(chordSlices(x, 24000, 16) == std::vector<int64_t>{0, 96000, 192000, 288000});
  // The same chord all the way: one slice.
  CHECK(chordSlices(chords({{60, 64, 67}, {60, 64, 67}}), 24000, 16) == std::vector<int64_t>{0});
  // A change halfway through a bar is found on its beat.
  const auto y = chords({{60, 64, 67}, {57, 60, 64}});
  std::vector<float> z(y.begin(), y.begin() + 48000);
  z.insert(z.end(), y.begin() + 96000 + 48000, y.end());  // C for two beats, then A minor
  z.insert(z.end(), y.begin() + 96000, y.begin() + 96000 + 48000);
  CHECK(chordSlices(z, 24000, 16) == std::vector<int64_t>{0, 48000});
  // At most maxSlices.
  CHECK(chordSlices(x, 24000, 2).size() == 2);
}

TEST_CASE("slicer: chord changes without a tempo come out near the changes") {
  const auto x = chords({{60, 64, 67}, {57, 60, 64}, {53, 57, 60}}, false);
  const auto s = chordSlices(x, 0, 16);
  REQUIRE(s.size() == 3);
  CHECK(s[0] == 0);
  CHECK(std::llabs(s[1] - 96000) < 2400);  // within 50 ms
  CHECK(std::llabs(s[2] - 192000) < 2400);
}

TEST_CASE("slicer: transients find each hit within a couple of milliseconds") {
  std::mt19937 rng(3);
  std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
  const std::vector<int64_t> hits = {0, 9600, 26400, 38400, 62400, 70000};
  std::vector<float> x(96000, 0.0f);
  for (std::size_t h = 0; h < hits.size(); ++h) {
    const float level = h % 2 ? 0.4f : 0.8f;
    for (int64_t i = 0; hits[h] + i < 96000 && i < 24000; ++i)
      x[static_cast<std::size_t>(hits[h] + i)] += level * noise(rng) * std::exp(-static_cast<float>(i) / 1500.0f);
  }
  const auto s = transientSlices(x, 16);
  REQUIRE(s.size() == hits.size());
  for (std::size_t i = 0; i < hits.size(); ++i) CHECK(std::llabs(s[i] - hits[i]) <= 96);
  CHECK(transientSlices(x, 3).size() == 3);
  CHECK(transientSlices(std::vector<float>(48000, 0.0f), 16) == std::vector<int64_t>{0});
}
