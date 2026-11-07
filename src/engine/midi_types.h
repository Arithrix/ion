#pragma once

#include <cstdint>

namespace ion {

// A raw MIDI channel or system message.
struct MidiMessage {
  uint8_t status = 0;
  uint8_t data1 = 0;
  uint8_t data2 = 0;
  uint8_t channel() const { return status & 0x0F; }
  uint8_t type() const { return status & 0xF0; }
};

// A message plus when it should take effect.
struct MidiEvent {
  enum class Timing : uint8_t {
    Now,       // at the start of the next block (UI, piano mode)
    Monotonic, // `time` is CLOCK_MONOTONIC ns of arrival (live input)
    Frame,     // `time` is an absolute frame (offline rendering)
  };
  MidiMessage msg;
  Timing timing = Timing::Now;
  int64_t time = 0;
};

}  // namespace ion
