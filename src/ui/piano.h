#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ion::ui {

// Computer keyboard as piano, Renoise layout (HANDOFF §8). Gate mode only
// (DECISIONS D-05): each note plays for a set time; key auto-repeat is ignored.
class Piano {
 public:
  using Clock = std::chrono::steady_clock;

  struct Note {
    uint8_t note;
    uint8_t velocity;
  };

  Piano(std::string lowerRow, std::string upperRow, int gateMs);

  // A key press. Returns the note to start, or nothing (not a piano key, the
  // note would be out of range, or it's auto-repeat of a key already held).
  std::optional<Note> press(char key, Clock::time_point now);
  // Notes whose gate has run out; send their note-offs.
  std::vector<uint8_t> expired(Clock::time_point now);
  // All sounding notes (leaving piano mode).
  std::vector<uint8_t> releaseAll();

  bool isPianoKey(char key) const;
  void octaveDown() { octave_ = std::max(0, octave_ - 1); }
  void octaveUp() { octave_ = std::min(8, octave_ + 1); }
  void cycleVelocity();
  void setOctave(int o) { octave_ = std::clamp(o, 0, 8); }
  void setGate(int ms) { gateMs_ = ms; }
  int octave() const { return octave_; }
  int velocity() const { return velocity_; }
  int gateMs() const { return gateMs_; }

 private:
  struct Held {
    char key;
    uint8_t note;
    Clock::time_point off;
    Clock::time_point lastEvent;
  };
  std::string lower_, upper_;
  int gateMs_;
  int octave_ = 3;  // Z row starts at C3 (= MIDI 60)
  int velocity_ = 100;
  std::vector<Held> held_;
  std::vector<std::pair<char, Clock::time_point>> lastEvent_;
};

}  // namespace ion::ui
