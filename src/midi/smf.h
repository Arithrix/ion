#pragma once

// Standard MIDI Files (format 0 and 1), for ion-render and tests.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "engine/midi_types.h"

namespace ion::smf {

struct Event {
  double seconds;
  MidiMessage msg;
};

struct File {
  std::vector<Event> events;  // channel messages, sorted by time
  double lengthSeconds = 0;   // time of the last event (incl. end-of-track)
};

// Parses an SMF. Honours tempo changes (applied across all tracks, as in
// format 1). SMPTE time division isn't supported. Never throws.
bool parse(std::span<const uint8_t> bytes, File& out, std::string& error);

// Writes a format-0 file at a fixed tempo, 480 ticks per quarter note.
std::vector<uint8_t> write(const std::vector<Event>& events, double bpm = 120.0);

}  // namespace ion::smf
