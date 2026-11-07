#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ion::wav {

struct Audio {
  unsigned sampleRate = 48000;
  unsigned channels = 2;
  std::vector<float> samples;  // interleaved
  std::size_t frames() const { return channels ? samples.size() / channels : 0; }
};

// 32-bit float WAV.
bool write(const std::filesystem::path& path, const Audio& audio, std::string& error);
// Any WAV dr_wav reads, converted to float.
bool read(const std::filesystem::path& path, Audio& audio, std::string& error);

}  // namespace ion::wav
