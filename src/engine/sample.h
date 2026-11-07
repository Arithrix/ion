#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace ion {

// A sample ready to play: 48 kHz float, mono or stereo, never changed after
// it's made. Built on the disk thread (SampleLoader), shared by every pad that
// uses it, and handed to the audio thread as a plain pointer; the control
// thread keeps it alive until the audio thread hands it back (DECISIONS D-36).
struct Sample {
  // Zero frames before and after each channel, so 4-point interpolation never
  // reads out of bounds, even at the ends.
  static constexpr int kGuard = 4;
  static constexpr int kOverview = 256;

  int channels = 1;     // 1 or 2
  int64_t frames = 0;   // at 48 kHz
  std::vector<float> data;  // planar: kGuard zeros, frames, kGuard zeros, per channel

  const float* channel(int c) const {
    return data.data() + static_cast<std::size_t>(c) * static_cast<std::size_t>(frames + 2 * kGuard) + kGuard;
  }
  double seconds() const { return static_cast<double>(frames) / 48000.0; }

  // For the UI: what the file was, and its shape.
  std::filesystem::path path;
  unsigned sourceRate = 48000;
  int sourceChannels = 1;
  int sourceBits = 0;       // 0 when the format doesn't say (float, FLAC sub-formats)
  float peak = 0;           // largest absolute sample
  std::array<float, kOverview> overview{};  // peak of each 1/256 of the length, 0…1
};

}  // namespace ion
