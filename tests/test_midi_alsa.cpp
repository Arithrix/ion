// MIDI input through a real ALSA sequencer, with a virtual port standing in for
// the OXI (HANDOFF §11). Skipped where there's no sequencer (e.g. a container).

#include <doctest.h>

#include <alsa/asoundlib.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "engine/fm/fm_engine.h"
#include "engine/module.h"
#include "midi/alsa_seq_in.h"

using namespace ion;
using namespace std::chrono_literals;

namespace {

// A sequencer client that looks like an OXI by name.
class FakeDevice {
 public:
  explicit FakeDevice(const char* name) {
    if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) {
      seq_ = nullptr;
      return;
    }
    snd_seq_set_client_name(seq_, name);
    port_ = snd_seq_create_simple_port(seq_, "MIDI 1", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                       SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_HARDWARE);
  }
  ~FakeDevice() {
    if (seq_) snd_seq_close(seq_);
  }
  bool ok() const { return seq_ && port_ >= 0; }

  void send(snd_seq_event_t ev) {
    snd_seq_ev_set_source(&ev, static_cast<unsigned char>(port_));
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_event_output_direct(seq_, &ev);
  }
  void noteOn(int ch, int note, int vel) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_noteon(&ev, static_cast<unsigned char>(ch), static_cast<unsigned char>(note), static_cast<unsigned char>(vel));
    send(ev);
  }
  void cc(int ch, int cc, int value) {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_controller(&ev, static_cast<unsigned char>(ch), static_cast<unsigned>(cc), value);
    send(ev);
  }
  void clock() {
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    ev.type = SND_SEQ_EVENT_CLOCK;
    send(ev);
  }

 private:
  snd_seq_t* seq_ = nullptr;
  int port_ = -1;
};

bool sequencerAvailable() {
  snd_seq_t* s = nullptr;
  if (snd_seq_open(&s, "default", SND_SEQ_OPEN_INPUT, 0) < 0) return false;
  snd_seq_close(s);
  return true;
}

// Stands in for the audio device: renders 128-frame cycles in step with the
// wall clock, as PipeWire would, so timestamped MIDI lands where it should.
class FakeAudio {
 public:
  explicit FakeAudio(Module& m) : m_(m), start_(std::chrono::steady_clock::now()) {}
  void pump() {
    const auto now = std::chrono::steady_clock::now();
    const auto due = static_cast<int64_t>(std::chrono::duration<double>(now - start_).count() * kSampleRate);
    while (rendered_ + 128 <= due) {
      const auto cycleStart = start_ + std::chrono::nanoseconds(rendered_ * 1'000'000'000 / kSampleRate);
      m_.process(buf_.data(), 128, std::chrono::duration_cast<std::chrono::nanoseconds>(cycleStart.time_since_epoch()).count());
      rendered_ += 128;
    }
  }

 private:
  Module& m_;
  std::chrono::steady_clock::time_point start_;
  int64_t rendered_ = 0;
  std::vector<float> buf_ = std::vector<float>(2 * 128);
};

// Waits up to `limit` for `cond`, running the fake audio device meanwhile.
template <typename Cond>
bool waitFor(FakeAudio& audio, Cond cond, std::chrono::milliseconds limit = 2000ms) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    audio.pump();
    if (cond()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

}  // namespace

TEST_CASE("alsa: routing, CC map, clock and hot-plug over a virtual port" * doctest::skip(!sequencerAvailable())) {
  // A unique name so a real OXI plugged into this machine is never touched.
  const char* deviceName = "OXI One MK2 (ion test)";
  auto module = std::make_unique<Module>();
  Command c;
  c.type = Command::Type::SetEngine;
  c.track = 1;
  c.engine = new FmEngine();
  module->pushCommand(c);

  AlsaSeqInput input(*module, {"ion test"});
  std::string error;
  REQUIRE(input.start(error));
  const ModuleStatus& st = module->status();
  FakeAudio audio(*module);

  {
    FakeDevice dev(deviceName);
    REQUIRE(dev.ok());
    // Plugged in after ion started: found through the announce port.
    REQUIRE(waitFor(audio, [&] { return !input.connected().empty(); }));
    CHECK(input.connected().front().find("ion test") != std::string::npos);

    dev.noteOn(1, 60, 100);  // channel 2 → track 2
    REQUIRE(waitFor(audio, [&] { return st.trackNotes[1].load() == 1; }));
    CHECK(st.trackNotes[0].load() == 0);

    // CC map: cc 7 on channel 2 drives track 2's fader.
    Command map;
    map.type = Command::Type::SetCcMapping;
    map.slot = 0;
    map.mapping = {true, 1, 7, {1, kFader}, 0.0f, 1.0f};
    module->pushCommand(map);
    dev.cc(1, 7, 64);
    REQUIRE(waitFor(audio, [&] { return module->params().get({1, kFader}) < -5.0f; }));

    // Clock at 120 BPM: a tick every 20.83 ms, paced against absolute times.
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 60; ++i) {
      while (std::chrono::steady_clock::now() < start + std::chrono::microseconds(20833 * i)) {
        audio.pump();
        std::this_thread::sleep_for(500us);
      }
      dev.clock();
    }
    waitFor(audio, [] { return false; }, 50ms);
    CHECK(st.clockPresent.load());
    CHECK(st.bpm.load() == doctest::Approx(120.0).epsilon(0.01));  // live timing, scheduler jitter included

    // The monitor saw it all.
    int seen = 0;
    while (input.monitor().pop()) ++seen;
    CHECK(seen >= 62);
  }  // unplugged

  REQUIRE(waitFor(audio, [&] { return input.connected().empty(); }));

  // Plugged back in: reconnects without restarting.
  FakeDevice again(deviceName);
  REQUIRE(again.ok());
  REQUIRE(waitFor(audio, [&] { return !input.connected().empty(); }));
  again.noteOn(1, 62, 100);
  REQUIRE(waitFor(audio, [&] { return st.trackNotes[1].load() == 2; }));
  input.stop();
}
