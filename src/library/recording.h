#pragma once

// What a recording knows about itself (HANDOFF §7.6): a JSON sidecar next to
// the WAV ("chords.wav" → "chords.json") with tempo, bars and sources. The
// Rec screen writes them (Phase 6); the sampler shows them, and draws a bar
// ruler when it knows the tempo.

#include <filesystem>
#include <string>

namespace ion {

struct RecordingInfo {
  bool ok = false;      // a sidecar was found and read
  double tempo = 0;     // BPM
  int bars = 0;
  int beatsPerBar = 4;
  std::string sources;  // "tracks 3 + 6", "master"
  std::string recordedAt;
};

std::filesystem::path sidecarPath(const std::filesystem::path& audio);
RecordingInfo readRecordingInfo(const std::filesystem::path& audio);

}  // namespace ion
