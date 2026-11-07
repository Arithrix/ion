#include <doctest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "engine/fm/fm_engine.h"
#include "engine/module.h"

using namespace ion;

namespace {

std::unique_ptr<Module> makeModule() {
  auto m = std::make_unique<Module>();
  m->setFmEngine(0, std::make_unique<FmEngine>(8));
  return m;
}

float peak(const std::vector<float>& x) {
  float p = 0;
  for (float s : x) p = std::max(p, std::fabs(s));
  return p;
}

}  // namespace

TEST_CASE("module: MIDI on channel 1 plays track 1; other channels don't") {
  auto m = makeModule();
  std::vector<float> out(2 * 4096);

  m->pushMidi({0x91, 60, 100});  // channel 2: no engine there
  m->process(out.data(), 4096);
  CHECK(peak(out) == 0.0f);

  m->pushMidi({0x90, 60, 100});  // channel 1
  m->process(out.data(), 4096);
  CHECK(peak(out) > 0.01f);
  CHECK(m->status().voices.load() == 1);
  CHECK(m->status().trackNotes[0].load() == 1);
  CHECK(m->status().midiReceived.load() == 2);
}

TEST_CASE("module: note-on with velocity 0 is a note-off") {
  auto m = makeModule();
  std::vector<float> out(2 * 512);
  m->pushMidi({0x90, 60, 100});
  m->process(out.data(), 512);
  m->pushMidi({0x90, 60, 0});
  for (int i = 0; i < 400; ++i) m->process(out.data(), 512);
  CHECK(m->status().voices.load() == 0);
}

TEST_CASE("module: output doesn't depend on the callback size") {
  // The same MIDI, rendered with block-aligned and ragged callback sizes,
  // must produce identical samples (events land on 64-frame block starts).
  auto renderWith = [](const std::vector<std::size_t>& sizes) {
    auto m = makeModule();
    m->pushMidi({0x90, 64, 110});
    std::vector<float> all;
    std::vector<float> buf;
    for (std::size_t n : sizes) {
      buf.assign(2 * n, 0.0f);
      m->process(buf.data(), n);
      all.insert(all.end(), buf.begin(), buf.end());
    }
    return all;
  };
  std::vector<std::size_t> aligned(48, 128);           // 6144 frames
  std::vector<std::size_t> ragged;
  for (std::size_t total = 0; total < 6144;) {         // 100, 37, 211, … up to 6144
    std::size_t n = std::min<std::size_t>(6144 - total, 37 + (total * 7) % 200);
    ragged.push_back(n);
    total += n;
  }
  CHECK(renderWith(aligned) == renderWith(ragged));
}

TEST_CASE("module: panic command silences everything") {
  auto m = makeModule();
  std::vector<float> out(2 * 256);
  for (uint8_t n = 60; n < 66; ++n) m->pushMidi({0x90, n, 100});
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
  std::vector<float> out(2 * 1024);
  for (uint8_t n = 36; n < 44; ++n) m->pushMidi({0x90, n, 127});
  float worst = 0;
  for (int i = 0; i < 50; ++i) {
    m->process(out.data(), 1024);
    worst = std::max(worst, peak(out));
  }
  CHECK(worst <= 0.9660509f);
}
