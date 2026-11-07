#include "ui/piano.h"

#include <algorithm>
#include <cctype>

namespace ion::ui {
namespace {

// X auto-repeat sends the key every ~50 ms (20 Hz) after a ~500 ms delay. An
// event this soon after the previous one for the same key is a repeat; people
// can't tap one key faster than ~80 ms apart, so real taps still play.
// The first repeat (at ~500 ms) looks exactly like a second tap to a terminal,
// so holding a key retriggers once; that's the limit of gate mode (D-05).
constexpr auto kRepeatWindow = std::chrono::milliseconds(75);

}  // namespace

Piano::Piano(std::string lowerRow, std::string upperRow, int gateMs)
    : lower_(std::move(lowerRow)), upper_(std::move(upperRow)), gateMs_(gateMs) {}

bool Piano::isPianoKey(char key) const {
  const char k = static_cast<char>(std::tolower(static_cast<unsigned char>(key)));
  return lower_.find(k) != std::string::npos || upper_.find(k) != std::string::npos;
}

std::optional<Piano::Note> Piano::press(char key, Clock::time_point now) {
  const char k = static_cast<char>(std::tolower(static_cast<unsigned char>(key)));
  int offset = -1;
  if (auto i = lower_.find(k); i != std::string::npos) offset = static_cast<int>(i);
  else if (auto j = upper_.find(k); j != std::string::npos) offset = 12 + static_cast<int>(j);
  if (offset < 0) return std::nullopt;

  // Repeat detection is per key, from the previous event of that key.
  auto last = std::find_if(lastEvent_.begin(), lastEvent_.end(), [&](const auto& e) { return e.first == k; });
  const bool repeat = last != lastEvent_.end() && now - last->second < kRepeatWindow;
  if (last != lastEvent_.end()) last->second = now;
  else lastEvent_.emplace_back(k, now);
  if (repeat) return std::nullopt;

  const int note = 12 * (octave_ + 2) + offset;
  if (note > 127) return std::nullopt;
  const auto n = static_cast<uint8_t>(note);
  // Pressed again while still sounding: restart its gate rather than stack notes.
  std::erase_if(held_, [&](const Held& h) { return h.note == n; });
  held_.push_back({k, n, now + std::chrono::milliseconds(gateMs_), now});
  return Note{n, static_cast<uint8_t>(velocity_)};
}

std::vector<uint8_t> Piano::expired(Clock::time_point now) {
  std::vector<uint8_t> out;
  std::erase_if(held_, [&](const Held& h) {
    if (h.off > now) return false;
    out.push_back(h.note);
    return true;
  });
  return out;
}

std::vector<uint8_t> Piano::releaseAll() {
  std::vector<uint8_t> out;
  for (const Held& h : held_) out.push_back(h.note);
  held_.clear();
  return out;
}

void Piano::cycleVelocity() { velocity_ = velocity_ == 64 ? 100 : (velocity_ == 100 ? 127 : 64); }

}  // namespace ion::ui
