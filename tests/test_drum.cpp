// Drum sampler (HANDOFF §6.1): sample loading, the engine, its hand-over
// through the module, and kits/projects in the session.

#include <doctest.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <random>
#include <thread>
#include <vector>

#include "audio/wav.h"
#include "engine/drum/drum_engine.h"
#include "engine/module.h"
#include "library/sample_loader.h"
#include "project/config.h"
#include "project/session.h"

using namespace ion;
namespace fs = std::filesystem;

namespace {

struct TempRoot {
  fs::path path;
  TempRoot() {
    path = fs::temp_directory_path() / ("ion-drum-" + std::to_string(std::random_device{}()));
    fs::create_directories(path);
  }
  ~TempRoot() { fs::remove_all(path); }
};

// A mono sample built in memory, as the loader would.
std::shared_ptr<Sample> makeSample(const std::vector<float>& x) {
  auto s = std::make_shared<Sample>();
  s->channels = 1;
  s->frames = static_cast<int64_t>(x.size());
  s->data.assign(x.size() + 2 * Sample::kGuard, 0.0f);
  std::copy(x.begin(), x.end(), s->data.begin() + Sample::kGuard);
  return s;
}

std::vector<float> sine(double hz, int frames, double rate = 48000, float amp = 0.5f) {
  std::vector<float> x(static_cast<std::size_t>(frames));
  for (int i = 0; i < frames; ++i) x[static_cast<std::size_t>(i)] = amp * static_cast<float>(std::sin(2 * std::numbers::pi * hz * i / rate));
  return x;
}

// Renders `frames` of the engine (left channel) in 64-frame blocks.
std::vector<float> render(DrumEngine& e, int frames) {
  std::vector<float> out;
  float l[64], r[64];
  for (int done = 0; done < frames; done += 64) {
    std::fill(l, l + 64, 0.0f);
    std::fill(r, r + 64, 0.0f);
    e.render(l, r, 64);
    out.insert(out.end(), l, l + 64);
  }
  out.resize(static_cast<std::size_t>(frames));
  return out;
}

float peakOf(const std::vector<float>& x, std::size_t from = 0, std::size_t to = SIZE_MAX) {
  float p = 0;
  for (std::size_t i = from; i < std::min(to, x.size()); ++i) p = std::max(p, std::fabs(x[i]));
  return p;
}

float rmsOf(const std::vector<float>& x, std::size_t from, std::size_t to) {
  double s = 0;
  for (std::size_t i = from; i < to; ++i) s += x[i] * x[i];
  return static_cast<float>(std::sqrt(s / static_cast<double>(to - from)));
}

std::size_t lastNonZero(const std::vector<float>& x) {
  for (std::size_t i = x.size(); i-- > 0;)
    if (std::fabs(x[i]) > 1e-6f) return i;
  return 0;
}

float maxStep(const std::vector<float>& x) {
  float m = 0;
  for (std::size_t i = 1; i < x.size(); ++i) m = std::max(m, std::fabs(x[i] - x[i - 1]));
  return m;
}

void set(DrumEngine& e, int pad, int p, float v) { e.setParam(DrumEngine::param(pad, p), v); }

// ---- writing test files ----

void put16(std::ofstream& f, uint16_t v, bool big = false) {
  const uint8_t b[2] = {static_cast<uint8_t>(big ? v >> 8 : v), static_cast<uint8_t>(big ? v : v >> 8)};
  f.write(reinterpret_cast<const char*>(b), 2);
}
void put32(std::ofstream& f, uint32_t v, bool big = false) {
  for (int i = 0; i < 4; ++i) f.put(static_cast<char>(big ? v >> (24 - 8 * i) : v >> (8 * i)));
}

// 16-bit PCM WAV, interleaved input.
void writeWav16(const fs::path& path, const std::vector<float>& x, int channels, uint32_t rate) {
  std::ofstream f(path, std::ios::binary);
  const auto bytes = static_cast<uint32_t>(x.size() * 2);
  f.write("RIFF", 4);
  put32(f, 36 + bytes);
  f.write("WAVEfmt ", 8);
  put32(f, 16);
  put16(f, 1);
  put16(f, static_cast<uint16_t>(channels));
  put32(f, rate);
  put32(f, rate * static_cast<uint32_t>(channels) * 2);
  put16(f, static_cast<uint16_t>(channels * 2));
  put16(f, 16);
  f.write("data", 4);
  put32(f, bytes);
  for (float v : x) put16(f, static_cast<uint16_t>(static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))));
}

// 16-bit AIFF, mono, 48 kHz (sample rate as an 80-bit float).
void writeAiff16(const fs::path& path, const std::vector<float>& x) {
  std::ofstream f(path, std::ios::binary);
  const auto bytes = static_cast<uint32_t>(x.size() * 2);
  f.write("FORM", 4);
  put32(f, 4 + 8 + 18 + 8 + 8 + bytes, true);
  f.write("AIFFCOMM", 8);
  put32(f, 18, true);
  put16(f, 1, true);
  put32(f, static_cast<uint32_t>(x.size()), true);
  put16(f, 16, true);
  const uint8_t rate48k[10] = {0x40, 0x0E, 0xBB, 0x80, 0, 0, 0, 0, 0, 0};
  f.write(reinterpret_cast<const char*>(rate48k), 10);
  f.write("SSND", 4);
  put32(f, 8 + bytes, true);
  put32(f, 0, true);
  put32(f, 0, true);
  for (float v : x) put16(f, static_cast<uint16_t>(static_cast<int16_t>(std::lround(v * 32767.0f))), true);
}

int zeroCrossings(const float* x, int64_t n) {
  int c = 0;
  for (int64_t i = 1; i < n; ++i) c += (x[i - 1] < 0) != (x[i] < 0);
  return c;
}

}  // namespace

// ---- loading ------------------------------------------------------------------------------

TEST_CASE("samples: a 44.1 kHz 16-bit stereo WAV arrives at 48 kHz with its pitch and level") {
  TempRoot root;
  const int n = 22050;  // 0.5 s
  const auto l = sine(1000, n, 44100, 0.5f), r = sine(2000, n, 44100, 0.25f);
  std::vector<float> inter(2 * static_cast<std::size_t>(n));
  for (int i = 0; i < n; ++i) {
    inter[2 * static_cast<std::size_t>(i)] = l[static_cast<std::size_t>(i)];
    inter[2 * static_cast<std::size_t>(i) + 1] = r[static_cast<std::size_t>(i)];
  }
  writeWav16(root.path / "tone.wav", inter, 2, 44100);

  const AudioFileInfo info = probeAudioFile(root.path / "tone.wav");
  CHECK(info.ok);
  CHECK(info.rate == 44100);
  CHECK(info.channels == 2);
  CHECK(info.seconds == doctest::Approx(0.5).epsilon(0.001));

  std::string error;
  const auto s = SampleLoader::loadNow(root.path / "tone.wav", error);
  REQUIRE(s);
  CHECK(s->channels == 2);
  CHECK(std::abs(s->frames - 24000) <= 2);
  CHECK(s->sourceRate == 44100);
  CHECK(s->sourceBits == 16);
  // Pitch: 1 kHz has 2000 zero crossings a second, wherever it's measured.
  CHECK(std::abs(zeroCrossings(s->channel(0) + 2400, 19200) - 800) <= 2);
  CHECK(std::abs(zeroCrossings(s->channel(1) + 2400, 19200) - 1600) <= 2);
  // Level: the sinc converter keeps a 1 kHz tone's amplitude (away from the edges).
  float p = 0;
  for (int64_t i = 2400; i < 21600; ++i) p = std::max(p, std::fabs(s->channel(0)[i]));
  CHECK(p == doctest::Approx(0.5f).epsilon(0.01));
  CHECK(s->peak == doctest::Approx(0.5f).epsilon(0.02));
  // The guard frames are silent.
  for (int g = 1; g <= Sample::kGuard; ++g) {
    CHECK(s->channel(0)[-g] == 0.0f);
    CHECK(s->channel(1)[s->frames - 1 + g] == 0.0f);
  }
}

TEST_CASE("samples: AIFF and float WAV at 48 kHz load unchanged") {
  TempRoot root;
  const auto x = sine(440, 48000, 48000, 0.8f);
  writeAiff16(root.path / "a.aif", x);
  std::string error;
  const auto a = SampleLoader::loadNow(root.path / "a.aif", error);
  REQUIRE_MESSAGE(a, error);
  CHECK(a->frames == 48000);
  CHECK(a->channels == 1);
  for (int i = 0; i < 48000; i += 97) CHECK(a->channel(0)[i] == doctest::Approx(x[static_cast<std::size_t>(i)]).epsilon(0.001));

  wav::Audio w;
  w.channels = 1;
  w.sampleRate = 48000;
  w.samples = x;
  REQUIRE(wav::write(root.path / "f.wav", w, error));
  const auto f = SampleLoader::loadNow(root.path / "f.wav", error);
  REQUIRE(f);
  CHECK(f->sourceBits == 0);
  CHECK(std::memcmp(f->channel(0), x.data(), x.size() * sizeof(float)) == 0);
  // The overview follows the level: a sine is loud all the way through.
  for (float o : f->overview) CHECK(o > 0.5f);
}

TEST_CASE("samples: broken, missing and non-audio files fail with a reason") {
  TempRoot root;
  std::string error;
  std::ofstream(root.path / "junk.wav") << "this is not audio";
  CHECK_FALSE(SampleLoader::loadNow(root.path / "junk.wav", error));
  CHECK_FALSE(error.empty());
  CHECK_FALSE(probeAudioFile(root.path / "junk.wav").ok);
  error.clear();
  CHECK_FALSE(SampleLoader::loadNow(root.path / "nope.wav", error));
  CHECK_FALSE(error.empty());
  CHECK(isAudioFile("x/Kick.WAV"));
  CHECK(isAudioFile("x/pad.aiff"));
  CHECK(isAudioFile("x/loop.flac"));
  CHECK_FALSE(isAudioFile("x/notes.txt"));
}

TEST_CASE("samples: the disk thread loads, caches, and drops stale previews") {
  TempRoot root;
  std::string error;
  wav::Audio w;
  w.channels = 1;
  w.samples = sine(100, 4800);
  for (int i = 0; i < 6; ++i) REQUIRE(wav::write(root.path / ("s" + std::to_string(i) + ".wav"), w, error));

  SampleLoader loader;
  loader.request(root.path / "s0.wav", 1);
  loader.request(root.path / "s0.wav", 2);
  loader.request(root.path / "missing.wav", 3);
  std::vector<SampleLoader::Result> got;
  for (int i = 0; i < 2000 && got.size() < 3; ++i) {
    for (auto& r : loader.poll()) got.push_back(std::move(r));
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(got.size() == 3);
  CHECK(got[0].tag == 1);
  CHECK(got[1].tag == 2);
  REQUIRE(got[0].sample);
  CHECK(got[0].sample.get() == got[1].sample.get());  // decoded once
  CHECK_FALSE(got[2].sample);
  CHECK(got[2].error == "file not found");

  // Five previews queued at once: the newest is never dropped, and older
  // ones still waiting are.
  std::vector<SampleLoader::Result> previews;
  for (int i = 1; i < 6; ++i) loader.request(root.path / ("s" + std::to_string(i) + ".wav"), 10 + static_cast<uint64_t>(i), true);
  for (int i = 0; i < 2000; ++i) {
    for (auto& r : loader.poll()) previews.push_back(std::move(r));
    if (!previews.empty() && previews.back().tag == 15) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE_FALSE(previews.empty());
  CHECK(previews.back().tag == 15);
  CHECK(previews.size() < 5);
  CHECK(loader.pending() == 0);
}

// ---- the engine -----------------------------------------------------------------------------

TEST_CASE("drum: pads play on their notes; the note map is editable") {
  DrumEngine e;
  auto s = makeSample(std::vector<float>(4800, 0.5f));
  e.setSample(0, s.get());
  e.setSample(5, s.get());
  e.noteOn(36, 127);  // C1 = pad 1
  CHECK(peakOf(render(e, 256)) > 0.4f);
  CHECK(e.voiceCount() == 1);
  CHECK(e.padsForNote(41) == (1 << 5));  // F1 = pad 6

  e.allNotesOff();
  set(e, 0, DrumEngine::kNote, 40);
  CHECK(e.padsForNote(36) == 0);
  e.noteOn(36, 127);
  CHECK(peakOf(render(e, 256)) == 0.0f);
  e.noteOn(40, 127);
  CHECK(peakOf(render(e, 256)) > 0.4f);
  e.noteOn(37, 127);  // pad 2: no sample, silent
  CHECK(e.voiceCount() == 1);
}

TEST_CASE("drum: tune changes pitch and length; reverse and start offset") {
  auto s = makeSample(sine(1000, 4800));
  {
    DrumEngine e;
    e.setSample(0, s.get());
    e.noteOn(36, 127);
    const auto out = render(e, 9600);
    CHECK(std::abs(static_cast<int>(lastNonZero(out)) - 4800) < 8);
  }
  {
    DrumEngine e;
    e.setSample(0, s.get());
    set(e, 0, DrumEngine::kTune, 12);
    e.noteOn(36, 127);
    const auto out = render(e, 9600);
    CHECK(std::abs(static_cast<int>(lastNonZero(out)) - 2400) < 8);
    CHECK(std::abs(zeroCrossings(out.data() + 200, 1920) - 160) <= 2);  // 2 kHz now
  }
  {
    DrumEngine e;  // −1 semitone and −50 cents: a quarter-tone more than a semitone down
    e.setSample(0, s.get());
    set(e, 0, DrumEngine::kTune, -1);
    set(e, 0, DrumEngine::kFine, -50);
    e.noteOn(36, 127);
    const auto out = render(e, 9600);
    CHECK(static_cast<double>(lastNonZero(out)) == doctest::Approx(4800 * std::exp2(1.5 / 12)).epsilon(0.003));
  }
  {
    // A rising ramp, reversed, falls.
    std::vector<float> ramp(4800);
    for (int i = 0; i < 4800; ++i) ramp[static_cast<std::size_t>(i)] = static_cast<float>(i) / 4800.0f;
    auto rs = makeSample(ramp);
    DrumEngine e;
    e.setSample(0, rs.get());
    set(e, 0, DrumEngine::kReverse, 1);
    e.noteOn(36, 127);
    const auto out = render(e, 4800);
    CHECK(out[10] > 0.9f);
    CHECK(out[4000] < 0.2f);
    CHECK(out[2000] > out[3000]);
  }
  {
    // Start 50 ms in: the sample's first 2400 frames are skipped, with a 1 ms fade-in.
    std::vector<float> step(4800, 0.0f);
    std::fill(step.begin() + 2400, step.end(), 0.5f);
    auto st = makeSample(step);
    DrumEngine e;
    e.setSample(0, st.get());
    set(e, 0, DrumEngine::kStart, 50);
    e.noteOn(36, 127);
    const auto out = render(e, 4800);
    CHECK(out[0] < 0.02f);
    CHECK(out[100] > 0.45f);
    CHECK(std::abs(static_cast<int>(lastNonZero(out)) - 2400) < 8);
    CHECK(maxStep({out.begin(), out.begin() + 2300}) < 0.02f);  // the fade-in: no click
  }
}

TEST_CASE("drum: choke groups stop the other hat without a click") {
  DrumEngine e;
  auto open = makeSample(sine(300, 48000, 48000, 0.8f));
  auto closed = makeSample(sine(5000, 2400, 48000, 0.1f));
  e.setSample(4, closed.get());  // pad 5, E1
  e.setSample(5, open.get());    // pad 6, F1
  set(e, 4, DrumEngine::kChoke, 1);
  set(e, 5, DrumEngine::kChoke, 1);
  set(e, 4, DrumEngine::kPan, -1);  // keep the hats apart: open on the right, closed on the left
  set(e, 5, DrumEngine::kPan, 1);
  e.noteOn(41, 127);
  render(e, 4800);
  e.noteOn(40, 127);  // closed hat chokes the open one
  std::vector<float> right;
  float l[64], r[64];
  for (int b = 0; b < 20; ++b) {
    std::fill(l, l + 64, 0.0f);
    std::fill(r, r + 64, 0.0f);
    e.render(l, r, 64);
    right.insert(right.end(), r, r + 64);
  }
  CHECK(peakOf(right, DrumEngine::kFadeFrames + 64) == 0.0f);  // gone after 5 ms
  // A 300 Hz sine at 0.8 × √2 steps at most ~0.045 per frame; a cut would jump far more.
  CHECK(maxStep(right) < 0.06f);
  CHECK(e.voiceCount() == 1);

  // Not in a group: both ring.
  DrumEngine f;
  f.setSample(4, closed.get());
  f.setSample(5, open.get());
  f.noteOn(41, 127);
  render(f, 480);
  f.noteOn(40, 127);
  render(f, 480);
  CHECK(f.voiceCount() == 2);
}

TEST_CASE("drum: a pad plays at most 4 voices; a pad alone in a choke group is mono") {
  DrumEngine e;
  auto s = makeSample(sine(200, 48000));
  e.setSample(0, s.get());
  for (int i = 0; i < 6; ++i) {
    e.noteOn(36, 100);
    render(e, 512);
  }
  CHECK(e.voiceCount() == DrumEngine::kPadVoices);
  set(e, 0, DrumEngine::kChoke, 2);
  e.noteOn(36, 100);
  render(e, 512);
  CHECK(e.voiceCount() == 1);
}

TEST_CASE("drum: one-shot ignores note-off; gate decays from it; hold then decay") {
  auto s = makeSample(std::vector<float>(96000, 0.5f));  // 2 s of DC
  {
    DrumEngine e;
    e.setSample(0, s.get());
    e.noteOn(36, 127);
    render(e, 4800);
    e.noteOff(36);
    const auto out = render(e, 9600);
    CHECK(out.back() > 0.45f);
  }
  {
    DrumEngine e;
    e.setSample(0, s.get());
    set(e, 0, DrumEngine::kMode, DrumEngine::kGate);
    set(e, 0, DrumEngine::kRelease, 100);
    e.noteOn(36, 127);
    CHECK(render(e, 4800).back() > 0.45f);  // held
    e.noteOff(36);
    const auto out = render(e, 9600);
    // −60 dB after the decay time (100 ms), and the voice ends below −80 dB.
    CHECK(std::fabs(out[4800]) < 0.5f * 0.0011f);
    CHECK(e.voiceCount() == 0);
  }
  {
    DrumEngine e;
    e.setSample(0, s.get());
    set(e, 0, DrumEngine::kHold, 10);
    set(e, 0, DrumEngine::kDecay, 50);
    e.noteOn(36, 127);
    const auto out = render(e, 9600);
    CHECK(out[400] == doctest::Approx(0.5f).epsilon(0.01));  // still holding at 8 ms
    CHECK(std::fabs(out[480 + 2400]) < 0.5f * 0.0011f);       // −60 dB 50 ms after the hold
    CHECK(maxStep(out) < 0.01f);
  }
  {
    DrumEngine e;  // "full" hold: the sample plays to its end
    e.setSample(0, s.get());
    e.noteOn(36, 127);
    CHECK(render(e, 48000).back() > 0.45f);
  }
}

TEST_CASE("drum: velocity, level and pan") {
  auto s = makeSample(std::vector<float>(4800, 0.5f));
  auto level = [&](float vel, float velAmount, float db) {
    DrumEngine e;
    e.setSample(0, s.get());
    set(e, 0, DrumEngine::kVelAmp, velAmount);
    set(e, 0, DrumEngine::kLevel, db);
    e.noteOn(36, static_cast<uint8_t>(vel));
    return render(e, 256)[128];
  };
  CHECK(level(127, 1, 0) == doctest::Approx(0.5f));
  CHECK(level(64, 0, 0) == doctest::Approx(0.5f));                     // no velocity response
  CHECK(level(64, 1, 0) == doctest::Approx(0.5f * (64.0f / 127) * (64.0f / 127)).epsilon(0.01));
  CHECK(level(127, 1, -6) == doctest::Approx(0.5f * 0.501f).epsilon(0.01));

  DrumEngine e;
  e.setSample(0, s.get());
  set(e, 0, DrumEngine::kPan, -1);
  e.noteOn(36, 127);
  float l[64]{}, r[64]{};
  e.render(l, r, 64);
  CHECK(l[32] == doctest::Approx(0.5f * std::numbers::sqrt2_v<float>));
  CHECK(r[32] == doctest::Approx(0.0f).epsilon(1e-6));
}

TEST_CASE("drum: the filter cuts what it should, and nothing blows up") {
  auto tone = [&](double hz, int filter, float cutoff) {
    DrumEngine e;
    auto s = makeSample(sine(hz, 24000));
    e.setSample(0, s.get());
    set(e, 0, DrumEngine::kFilter, static_cast<float>(filter));
    set(e, 0, DrumEngine::kCutoff, cutoff);
    e.noteOn(36, 127);
    const auto out = render(e, 24000);
    return rmsOf(out, 4800, 20000);
  };
  const float dry = tone(5000, DrumEngine::kFilterOff, 1000);
  CHECK(tone(5000, DrumEngine::kLowPass, 200) < dry * 0.002f);   // −54 dB: 12 dB/oct, 4.6 octaves
  CHECK(tone(100, DrumEngine::kHighPass, 5000) < dry * 0.001f);
  CHECK(tone(1000, DrumEngine::kBandPass, 1000) == doctest::Approx(dry).epsilon(0.02));  // unity at the centre
  CHECK(tone(1000, DrumEngine::kLowPass, 20000) == doctest::Approx(dry).epsilon(0.01));

  // Everything at its extreme, with noise, retriggered.
  DrumEngine e;
  std::mt19937 rng(1);
  std::uniform_real_distribution<float> u(-1, 1);
  std::vector<float> noise(48000);
  for (float& v : noise) v = u(rng);
  auto s = makeSample(noise);
  e.setSample(0, s.get());
  set(e, 0, DrumEngine::kFilter, DrumEngine::kBandPass);
  set(e, 0, DrumEngine::kReso, 1);
  set(e, 0, DrumEngine::kDrive, 1);
  set(e, 0, DrumEngine::kTune, 24);
  set(e, 0, DrumEngine::kLevel, 6);
  bool finite = true;
  for (int i = 0; i < 40; ++i) {
    set(e, 0, DrumEngine::kCutoff, i % 2 ? 20000.0f : 20.0f);
    e.noteOn(36, 127);
    for (float v : render(e, 1024)) finite &= std::isfinite(v) && std::fabs(v) < 100;
  }
  CHECK(finite);
}

TEST_CASE("drum: a replaced sample is released only once nothing plays it") {
  DrumEngine e;
  auto a = makeSample(std::vector<float>(9600, 0.5f));
  auto b = makeSample(std::vector<float>(9600, 0.25f));
  e.setSample(0, a.get());
  e.noteOn(36, 127);
  render(e, 256);
  e.setSample(0, b.get());
  const Sample* got = nullptr;
  CHECK_FALSE(e.popReleased(got));  // the voice still plays a
  // The old voice keeps playing a (0.5); a new hit plays b on top.
  e.noteOn(36, 127);
  CHECK(render(e, 64)[32] == doctest::Approx(0.75f));
  render(e, 9600);
  REQUIRE(e.popReleased(got));
  CHECK(got == a.get());
  CHECK_FALSE(e.popReleased(got));

  // Not playing: released at once. Every reference is listed.
  e.setSample(1, a.get());
  e.setSample(1, nullptr);
  REQUIRE(e.popReleased(got));
  CHECK(got == a.get());
  int refs = 0;
  e.forEachSample([&](const Sample*) { ++refs; });
  CHECK(refs == 1);  // b on pad 1
}

// ---- through the module -------------------------------------------------------------------------

TEST_CASE("module: drum samples go in and come back; pads light up; previews play once") {
  Module m;
  auto s = makeSample(std::vector<float>(2400, 0.5f));
  Command c;
  c.type = Command::Type::SetEngine;
  c.track = 2;
  c.engine = new DrumEngine();
  m.pushCommand(c);
  Command set;
  set.type = Command::Type::SetSample;
  set.track = 2;
  set.slot = 3;
  set.sample = s.get();
  m.pushCommand(set);
  std::vector<float> out(2 * 512);
  m.process(out.data(), 512);
  m.pushMidi({{0x92, 39, 127}});  // D#1 on channel 3: pad 4
  m.process(out.data(), 512);
  CHECK(m.status().padHits[2][3].load() == 1);
  CHECK(m.status().trackVoices[2].load() == 1);

  // A sample sent to a track that isn't a drum comes straight back.
  set.track = 5;
  m.pushCommand(set);
  m.process(out.data(), 512);
  auto n = m.popNotification();
  REQUIRE(n);
  CHECK(n->type == Notification::Type::SampleReleased);
  CHECK(n->sample == s.get());

  // Preview: plays into the master, then comes back when it ends.
  Command p;
  p.type = Command::Type::Preview;
  p.sample = s.get();
  p.value = 1.0f;
  m.pushCommand(p);
  m.process(out.data(), 256);
  CHECK(m.status().previewing.load());
  m.process(out.data(), 512);
  m.process(out.data(), 512);
  m.process(out.data(), 512);
  m.process(out.data(), 512);
  m.process(out.data(), 512);
  CHECK_FALSE(m.status().previewing.load());
  n = m.popNotification();
  REQUIRE(n);
  CHECK(n->type == Notification::Type::SampleReleased);
}

// ---- session: pads, projects, kits, collect --------------------------------------------------------

namespace {

struct Rig {
  TempRoot root;
  std::unique_ptr<Module> module = std::make_unique<Module>();
  Config config;
  Session session{*module, Paths::under(root.path), config};
  std::vector<float> buf = std::vector<float>(2 * 256);
  void run(int blocks = 4) {
    for (int i = 0; i < blocks; ++i) module->process(buf.data(), 256);
    session.tick();
  }
  // Loads, hand-over, release: until nothing changes.
  void settle() {
    for (int i = 0; i < 400; ++i) {
      run();
      if (session.samplesLoading() == 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    run();
  }
  fs::path write(const std::string& name, float level, int frames = 4800) {
    wav::Audio w;
    w.channels = 1;
    w.samples.assign(static_cast<std::size_t>(frames), level);
    const fs::path p = root.path / "lib" / name;
    fs::create_directories(p.parent_path());
    std::string error;
    REQUIRE(wav::write(p, w, error));
    return p;
  }
  float hit(int track, uint8_t note) {
    std::vector<float> out(2 * 512);
    module->pushMidi({{static_cast<uint8_t>(0x90 | track), note, 127}});
    module->process(out.data(), 512);
    float p = 0;
    for (float v : out) p = std::max(p, std::fabs(v));
    session.panic();
    run();
    return p;
  }
};

}  // namespace

TEST_CASE("session: pad samples load on the disk thread and play") {
  Rig r;
  r.session.newProject("kit test");
  r.session.setEngine(0, EngineType::Drum);
  const fs::path kick = r.write("kick.wav", 0.5f);
  r.session.setPadSample(0, 0, kick.string());
  CHECK(r.session.padStatus(0, 0) == Session::PadStatus::Loading);
  r.settle();
  CHECK(r.session.padStatus(0, 0) == Session::PadStatus::Ready);
  CHECK(r.session.padName(0, 0) == "kick");
  CHECK(r.hit(0, 36) > 0.4f);
  CHECK(r.session.pinnedSamples() == 1);

  // A missing file: the pad says so and is silent; the path is kept.
  r.session.setPadSample(0, 1, (r.root.path / "gone.wav").string());
  r.settle();
  CHECK(r.session.padStatus(0, 1) == Session::PadStatus::Failed);
  CHECK(r.session.padError(0, 1) == "file not found");
  CHECK(r.session.track(0).pads[1].sample == (r.root.path / "gone.wav").string());
  CHECK(r.hit(0, 37) == 0.0f);

  // Replacing and clearing hand the old samples back.
  r.session.setPadSample(0, 0, r.write("kick2.wav", 0.25f).string());
  r.settle();
  CHECK(r.hit(0, 36) == doctest::Approx(0.25f).epsilon(0.01));
  r.settle();
  CHECK(r.session.pinnedSamples() == 1);
  r.session.setPadSample(0, 0, "");
  r.settle();
  CHECK(r.session.pinnedSamples() == 0);
  CHECK(r.hit(0, 36) == 0.0f);

  // Changing the engine releases everything the old one held.
  r.session.setPadSample(0, 2, kick.string());
  r.settle();
  CHECK(r.session.pinnedSamples() == 1);
  r.session.setEngine(0, EngineType::Fm);
  r.settle();
  CHECK(r.session.pinnedSamples() == 0);
  CHECK(r.session.track(0).pads[2].sample.empty());
}

TEST_CASE("session: drum tracks save and load, pads and pad settings included") {
  Rig r;
  r.session.newProject("drums");
  r.session.setEngine(1, EngineType::Drum);
  r.session.setPadSample(1, 0, r.write("bd.wav", 0.5f).string());
  r.session.setPadSample(1, 15, r.write("perc.wav", 0.3f).string());
  r.session.setParam(engineParamRef(1, DrumEngine::param(15, DrumEngine::kTune)), 7);
  r.session.setParam(engineParamRef(1, DrumEngine::param(4, DrumEngine::kChoke)), 1);
  r.session.addMapping(0, 74, engineParamRef(1, DrumEngine::param(0, DrumEngine::kCutoff)));
  r.settle();
  std::string error;
  REQUIRE(r.session.save(error));

  const std::string json = r.session.toJson();
  CHECK(json.find("\"format\": " + std::to_string(kProjectFormat)) != std::string::npos);
  CHECK(json.find("\"pad16.tune\": 7.0") != std::string::npos);
  CHECK(json.find("track2.drum.pad1.cutoff") != std::string::npos);

  Rig r2;
  fs::create_directories(r2.root.path / "projects");
  fs::copy(r.root.path / "projects" / "drums", r2.root.path / "projects" / "drums", fs::copy_options::recursive);
  REQUIRE(r2.session.load("drums", error));
  r2.settle();
  CHECK(r2.session.track(1).engine == EngineType::Drum);
  CHECK(r2.session.padStatus(1, 0) == Session::PadStatus::Ready);
  CHECK(r2.session.padStatus(1, 15) == Session::PadStatus::Ready);
  CHECK(r2.session.param(engineParamRef(1, DrumEngine::param(15, DrumEngine::kTune))) == 7.0f);
  CHECK(r2.module->params().get(engineParamRef(1, DrumEngine::param(4, DrumEngine::kChoke))) == 1.0f);
  CHECK(r2.session.ccMappings().size() == 1);
  CHECK(r2.hit(1, 36) > 0.4f);
  CHECK(r2.session.projectSummary("drums").tracks[1].sound == "2 pads");
}

TEST_CASE("session: kits save every pad and load onto another track") {
  Rig r;
  r.session.newProject("kits");
  r.session.setEngine(0, EngineType::Drum);
  r.session.setPadSample(0, 0, r.write("bd.wav", 0.5f).string());
  r.session.setPadSample(0, 4, r.write("ch.wav", 0.2f).string());
  r.session.setParam(engineParamRef(0, DrumEngine::param(4, DrumEngine::kDecay)), 80);
  r.settle();
  std::string error;
  REQUIRE(r.session.saveKit(0, "Garage 01", error));
  CHECK(r.session.track(0).kit == "Garage 01");
  CHECK(fs::exists(r.root.path / "kits" / "Garage 01.json"));
  CHECK(r.session.listKits() == std::vector<std::string>{"Garage 01"});
  CHECK_FALSE(r.session.saveKit(0, "bad/name", error));

  r.session.setEngine(3, EngineType::Drum);
  r.session.setPadSample(3, 9, r.write("other.wav", 0.1f).string());  // cleared by the kit
  r.session.setParam(engineParamRef(3, DrumEngine::param(0, DrumEngine::kTune)), 5);  // reset by the kit
  REQUIRE(r.session.loadKit(3, r.root.path / "kits" / "Garage 01.json", error));
  r.settle();
  CHECK(r.session.track(3).kit == "Garage 01");
  CHECK(r.session.padName(3, 0) == "bd");
  CHECK(r.session.padName(3, 4) == "ch");
  CHECK(r.session.padStatus(3, 9) == Session::PadStatus::Empty);
  CHECK(r.session.param(engineParamRef(3, DrumEngine::param(4, DrumEngine::kDecay))) == 80.0f);
  CHECK(r.session.param(engineParamRef(3, DrumEngine::param(0, DrumEngine::kTune))) == 0.0f);
  CHECK(r.hit(3, 36) > 0.4f);

  std::ofstream(r.root.path / "kits" / "junk.json") << "{}";
  CHECK_FALSE(r.session.loadKit(3, r.root.path / "kits" / "junk.json", error));
  CHECK(error == "not an ion kit");
}

TEST_CASE("session: collect copies samples into the project and save-as takes them along") {
  Rig r;
  r.session.newProject("collect");
  r.session.setEngine(0, EngineType::Drum);
  const fs::path bd = r.write("a/kick.wav", 0.5f);
  const fs::path other = r.write("b/kick.wav", 0.3f);  // same name, different sound
  r.session.setPadSample(0, 0, bd.string());
  r.session.setPadSample(0, 1, bd.string());     // the same file twice: copied once
  r.session.setPadSample(0, 2, other.string());
  r.session.setPadSample(0, 3, (r.root.path / "gone.wav").string());
  r.settle();
  std::string error;
  CHECK(r.session.collectSamples(error) == 2);
  CHECK(error.find("1 sample is missing") != std::string::npos);
  CHECK(r.session.track(0).pads[0].sample == "samples/kick.wav");
  CHECK(r.session.track(0).pads[1].sample == "samples/kick.wav");
  CHECK(r.session.track(0).pads[2].sample == "samples/kick 2.wav");
  CHECK(r.session.track(0).pads[3].sample == (r.root.path / "gone.wav").string());
  const fs::path dir = r.root.path / "projects" / "collect" / "samples";
  CHECK(fs::exists(dir / "kick.wav"));
  CHECK(fs::exists(dir / "kick 2.wav"));
  REQUIRE(r.session.save(error));

  // Collecting again copies nothing.
  error.clear();
  CHECK(r.session.collectSamples(error) == 0);

  // The originals can go: the project plays from its own folder, and so does
  // a copy made with save as.
  fs::remove_all(r.root.path / "lib");
  REQUIRE(r.session.saveAs("collect copy", error));
  CHECK(fs::exists(r.root.path / "projects" / "collect copy" / "samples" / "kick 2.wav"));
  Rig r2;
  fs::create_directories(r2.root.path / "projects");
  fs::copy(r.root.path / "projects" / "collect copy", r2.root.path / "projects" / "collect copy", fs::copy_options::recursive);
  REQUIRE(r2.session.load("collect copy", error));
  r2.settle();
  CHECK(r2.session.padStatus(0, 0) == Session::PadStatus::Ready);
  CHECK(r2.session.padStatus(0, 2) == Session::PadStatus::Ready);
  CHECK(r2.hit(0, 38) == doctest::Approx(0.3f).epsilon(0.01));
}

TEST_CASE("session: previews play once and are handed back") {
  Rig r;
  r.session.newProject("preview");
  const fs::path a = r.write("a.wav", 0.5f, 2400);
  r.session.preview(a, true);
  for (int i = 0; i < 400 && !r.session.previewed(a); ++i) {
    r.run();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(r.session.previewed(a));
  r.run();
  CHECK(r.session.pinnedSamples() == 1);
  r.run(40);  // it ends
  r.run();
  CHECK(r.session.pinnedSamples() == 0);

  // Looking without playing (preview off) still gives the waveform.
  const fs::path b = r.write("b.wav", 0.5f);
  r.session.preview(b, false);
  for (int i = 0; i < 400 && !r.session.previewed(b); ++i) {
    r.run();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  REQUIRE(r.session.previewed(b));
  CHECK(r.session.pinnedSamples() == 0);
  r.session.preview(r.root.path / "nope.wav", true);
  for (int i = 0; i < 400 && r.session.previewError(r.root.path / "nope.wav").empty(); ++i) {
    r.run();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(r.session.previewError(r.root.path / "nope.wav") == "file not found");
}

TEST_CASE("session: eight drum kits load with no audio thread and nothing is lost") {
  // ion-render and make-demo load projects with nobody draining the command
  // queue (2048 commands). Eight full kits must still arrive whole.
  Rig r;
  r.session.newProject("eight kits");
  const fs::path smp = r.write("x.wav", 0.5f);
  for (int t = 0; t < kTrackCount; ++t) {
    r.session.setEngine(t, EngineType::Drum);
    for (int pad = 0; pad < DrumEngine::kPads; ++pad) {
      r.session.setPadSample(t, pad, smp.string());
      r.session.setParam(engineParamRef(t, DrumEngine::param(pad, DrumEngine::kTune)), static_cast<float>(pad % 5));
      r.session.setParam(engineParamRef(t, DrumEngine::param(pad, DrumEngine::kDecay)), 100.0f + static_cast<float>(t));
    }
    r.run();
  }
  r.settle();
  std::string error;
  REQUIRE(r.session.save(error));

  Rig r2;
  fs::create_directories(r2.root.path / "projects");
  fs::copy(r.root.path / "projects" / "eight kits", r2.root.path / "projects" / "eight kits", fs::copy_options::recursive);
  REQUIRE(r2.session.load("eight kits", error));  // the module isn't processing yet
  CHECK(r2.session.takeMessage().empty());        // no "an edit was lost"
  r2.settle();
  for (int t = 0; t < kTrackCount; ++t) {
    CHECK(r2.module->params().get(engineParamRef(t, DrumEngine::param(15, DrumEngine::kTune))) == 0.0f);  // 15 % 5
    CHECK(r2.module->params().get(engineParamRef(t, DrumEngine::param(14, DrumEngine::kTune))) == 4.0f);
    CHECK(r2.module->params().get(engineParamRef(t, DrumEngine::param(3, DrumEngine::kDecay))) == 100.0f + static_cast<float>(t));
    CHECK(r2.session.padStatus(t, 15) == Session::PadStatus::Ready);
  }
}
