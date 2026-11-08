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

// ---- the Session: files, destinations, undo --------------------------------------

#include <filesystem>
#include <nlohmann/json.hpp>

#include "audio/wav.h"
#include "engine/drum/drum_engine.h"
#include "engine/sampler/sampler_engine.h"
#include "project/config.h"
#include "project/session.h"

namespace {

namespace fs = std::filesystem;

struct TempRoot {
  fs::path path;
  TempRoot() {
    path = fs::temp_directory_path() / ("ion-rec-" + std::to_string(std::random_device{}()));
    fs::create_directories(path);
  }
  ~TempRoot() { fs::remove_all(path); }
};

// A session with track 1 playing `audio` from a sampler (no loop), and MIDI
// clock at 120 BPM from frame 0.
struct SessionRig {
  TempRoot root;
  std::unique_ptr<Module> module = std::make_unique<Module>();
  Config config;
  Session session{*module, Paths::under(root.path), config};
  std::vector<float> buf = std::vector<float>(2 * 256);
  int64_t frame = 0, nextTick = 0;
  bool clock = true;

  explicit SessionRig(const std::vector<float>& audio) {
    session.newProject("rec");
    const fs::path wavPath = root.path / "source.wav";
    wav::Audio a;
    a.channels = 1;
    a.samples = audio;
    std::string error;
    REQUIRE(wav::write(wavPath, a, error));
    session.setEngine(0, EngineType::Sampler);
    session.setPadSample(0, 0, wavPath.string());
    REQUIRE(session.waitForSamples());
    run(1024);
  }
  void run(int64_t frames) {
    const int64_t end = frame + frames;
    while (frame < end) {
      const int64_t to = frame + 256;
      while (clock && nextTick < to) {
        module->pushMidi({{0xF8}, MidiEvent::Timing::Frame, nextTick});
        nextTick += kTick;
      }
      module->process(buf.data(), 256);
      frame = to;
      session.tick();
    }
  }
  // Start the transport on the next tick and play C3 on track 1 with it.
  void startWithNote() {
    module->pushMidi({{0xFA}, MidiEvent::Timing::Frame, nextTick});
    module->pushMidi({{0x90, 60, 127}, MidiEvent::Timing::Frame, nextTick});
  }
};

nlohmann::json readJson(const fs::path& p) {
  std::string text;
  REQUIRE(readFile(p, text));
  return nlohmann::json::parse(text);
}

}  // namespace

TEST_CASE("session: a take sliced on its chord changes goes onto drum pads, and undo puts everything back") {
  const auto progression = chords({{60, 64, 67}, {57, 60, 64}, {53, 57, 60}, {55, 59, 62}});
  SessionRig r(progression);
  Session& s = r.session;
  s.setEngine(1, EngineType::Drum);  // track 2: the destination, with an old pad to keep
  RecSetup& rs = s.recSetup();
  rs.source = RecSource::Tracks;
  rs.tracks = 1;  // track 1
  rs.tap = RecTap::Pre;
  rs.bars = 4;
  rs.synced = true;
  rs.dest = RecSetup::Dest::Drum;
  rs.destTrack = 1;
  rs.firstPad = 4;
  rs.slice = SliceMode::Chords;
  rs.muteSources = true;

  std::string error;
  REQUIRE(s.arm(error));
  CHECK(s.recState() == Session::RecState::Armed);
  r.run(5000);
  r.startWithNote();
  r.run(10 * kTick);
  CHECK(s.recState() == Session::RecState::Recording);
  r.run(4 * 96 * kTick);
  REQUIRE(s.recState() == Session::RecState::Review);

  const Session::SliceReview* rv = s.review();
  REQUIRE(rv);
  CHECK(rv->sample->frames == 4 * 96 * kTick);  // trimmed to exactly 4 bars
  CHECK(rv->tempo == doctest::Approx(120.0));
  CHECK(rv->bars == 4);
  CHECK(rv->slices == std::vector<int64_t>{0, 96000, 192000, 288000});
  const fs::path wavFile = rv->file;
  CHECK(fs::exists(wavFile));
  CHECK(wavFile.filename().string().find("_120bpm_4bar") != std::string::npos);
  const auto side = readJson(sidecarPath(wavFile));
  CHECK(side["tempo"] == 120.0);
  CHECK(side["bars"] == 4);
  CHECK(side["frames"] == 4 * 96 * kTick);
  CHECK(side["sources"] == "track 1");
  CHECK(side["settings"].size() == 1);
  CHECK(side["settings"][0]["engine"] == "sampler");
  CHECK(s.param({0, static_cast<uint16_t>(kMute)}) == 1.0f);  // the source is muted

  // Nudge, add and remove, then commit.
  s.moveSlice(1, 48);
  CHECK(s.review()->slices[1] == 96048);
  s.moveSlice(1, -48);
  const int added = s.addSlice(330000);
  CHECK(added == 4);
  s.removeSlice(added);
  CHECK(s.review()->slices.size() == 4);
  s.moveSlice(0, 100);  // slice 0 stays at the start
  CHECK(s.review()->slices[0] == 0);
  REQUIRE(s.commitSlices(error));
  CHECK(s.recState() == Session::RecState::Idle);
  REQUIRE(s.waitForSamples());
  r.run(512);
  for (int i = 0; i < 4; ++i) {
    const int pad = 4 + i;
    CHECK(s.padStatus(1, pad) == Session::PadStatus::Ready);
    CHECK(s.padName(1, pad) == "slice 0" + std::to_string(i + 1));
    CHECK(s.param(engineParamRef(1, DrumEngine::param(pad, DrumEngine::kChoke))) == 1.0f);  // group A
    CHECK(s.param(engineParamRef(1, DrumEngine::param(pad, DrumEngine::kMode))) == static_cast<float>(DrumEngine::kOneShot));
    CHECK(s.padSample(1, pad)->frames == 96000);
  }
  CHECK(readJson(sidecarPath(wavFile))["sent_to"] == "drum · track 2 · pads 5–8");
  CHECK(s.recordings().size() == 1);

  // Undo: the pads are empty again, the source plays, the files are gone.
  REQUIRE(s.canUndoTake());
  REQUIRE(s.undoTake(error));
  REQUIRE(s.waitForSamples());
  r.run(512);
  CHECK(s.track(1).engine == EngineType::Drum);
  for (int pad = 4; pad < 8; ++pad) CHECK(s.padStatus(1, pad) == Session::PadStatus::Empty);
  CHECK(s.param({0, static_cast<uint16_t>(kMute)}) == 0.0f);
  CHECK_FALSE(fs::exists(wavFile));
  CHECK_FALSE(fs::exists(sidecarPath(wavFile)));
  CHECK_FALSE(fs::exists(wavFile.parent_path() / wavFile.stem()));
  CHECK(s.recordings().empty());
  CHECK_FALSE(s.canUndoTake());
}

TEST_CASE("session: a take for the sampler loops from C3 on its track, which isn't muted with the others") {
  SessionRig r(chords({{60, 64, 67}}));
  Session& s = r.session;
  s.setEngine(2, EngineType::Fm);
  RecSetup& rs = s.recSetup();
  rs.source = RecSource::Tracks;
  rs.tracks = 1 | 4;  // tracks 1 and 3
  rs.bars = 1;
  rs.dest = RecSetup::Dest::Sampler;
  rs.destTrack = 2;  // track 3 records itself into a sampler
  std::string error;
  REQUIRE(s.arm(error));
  r.startWithNote();
  r.run(2 * 96 * kTick);
  REQUIRE(s.recState() == Session::RecState::Idle);
  REQUIRE(s.waitForSamples());
  r.run(512);
  CHECK(s.track(2).engine == EngineType::Sampler);
  CHECK(s.track(2).pads[0].sample.rfind("recordings/tracks-1-3_120bpm_1bar", 0) == 0);
  CHECK(s.padSample(2, 0)->frames == 96000);
  CHECK(s.param(engineParamRef(2, SamplerEngine::kLoop)) == static_cast<float>(SamplerEngine::kLoopForward));
  CHECK(s.param(engineParamRef(2, SamplerEngine::kRoot)) == 60.0f);
  CHECK(s.param({0, static_cast<uint16_t>(kMute)}) == 1.0f);
  CHECK(s.param({2, static_cast<uint16_t>(kMute)}) == 0.0f);  // it plays the recording now
  const auto recs = s.recordings();
  REQUIRE(recs.size() == 1);
  CHECK(recs[0].info.sentTo == "sampler · track 3");
  CHECK(recs[0].info.sources == "tracks 1 + 3");

  // A project saved under a new name takes its recordings along.
  REQUIRE(s.saveAs("rec copy", error));
  CHECK(fs::exists(s.recordingsDir() / recs[0].path.filename()));

  // Undo brings the FM track back.
  REQUIRE(s.undoTake(error));
  CHECK(s.track(2).engine == EngineType::Fm);
}

TEST_CASE("session: arming checks the setup; a cancelled take leaves nothing") {
  SessionRig r(chords({{60, 64, 67}}));
  Session& s = r.session;
  RecSetup& rs = s.recSetup();
  std::string error;
  rs.source = RecSource::Tracks;
  rs.tracks = 0;
  CHECK_FALSE(s.arm(error));
  CHECK(error.find("choose the tracks") != std::string::npos);
  rs.tracks = 1 << 5;
  CHECK_FALSE(s.arm(error));
  CHECK(error == "track 6 is empty");

  rs.source = RecSource::Master;
  rs.synced = false;
  rs.bars = 0;
  REQUIRE(s.arm(error));
  CHECK_FALSE(s.arm(error));  // one at a time
  r.run(4096);
  s.cancelTake();
  r.run(512);
  CHECK(s.recState() == Session::RecState::Idle);
  CHECK(s.recordings().empty());
  CHECK_FALSE(s.canUndoTake());

  // Free, at once: stops when told, and keeps what it has.
  REQUIRE(s.arm(error));
  r.run(48000);
  s.stopTake();
  r.run(512);
  const auto recs = s.recordings();
  REQUIRE(recs.size() == 1);
  CHECK(recs[0].info.tempo == 0);
  CHECK(recs[0].path.filename().string().rfind("mix_1.0s", 0) == 0);
}

TEST_CASE("session: the Rec setup is saved with the project") {
  SessionRig r(chords({{60, 64, 67}}));
  Session& s = r.session;
  RecSetup& rs = s.recSetup();
  rs.source = RecSource::Tracks;
  rs.tracks = 1 | 8;
  rs.tap = RecTap::Pre;
  rs.bars = 16;
  rs.synced = false;
  rs.dest = RecSetup::Dest::Drum;
  rs.destTrack = 6;
  rs.firstPad = 4;
  rs.slice = SliceMode::Equal;
  rs.sliceCount = 16;
  rs.muteSources = false;
  std::string error;
  REQUIRE(s.save(error));
  s.newProject("other");
  CHECK(s.recSetup().source == RecSource::Master);
  REQUIRE(s.load("rec", error));
  const RecSetup& back = s.recSetup();
  CHECK(back.source == RecSource::Tracks);
  CHECK(back.tracks == (1 | 8));
  CHECK(back.tap == RecTap::Pre);
  CHECK(back.bars == 16);
  CHECK_FALSE(back.synced);
  CHECK(back.dest == RecSetup::Dest::Drum);
  CHECK(back.destTrack == 6);
  CHECK(back.firstPad == 4);
  CHECK(back.slice == SliceMode::Equal);
  CHECK(back.sliceCount == 16);
  CHECK_FALSE(back.muteSources);
}
