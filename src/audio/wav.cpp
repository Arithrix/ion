#include "audio/wav.h"

#define DR_WAV_IMPLEMENTATION
#include <dr_wav.h>

namespace ion::wav {

bool write(const std::filesystem::path& path, const Audio& audio, std::string& error) {
  drwav_data_format format{};
  format.container = drwav_container_riff;
  format.format = DR_WAVE_FORMAT_IEEE_FLOAT;
  format.channels = audio.channels;
  format.sampleRate = audio.sampleRate;
  format.bitsPerSample = 32;
  drwav w;
  if (!drwav_init_file_write(&w, path.c_str(), &format, nullptr)) {
    error = "can't write " + path.string();
    return false;
  }
  const drwav_uint64 written = drwav_write_pcm_frames(&w, audio.frames(), audio.samples.data());
  drwav_uninit(&w);
  if (written != audio.frames()) {
    error = "short write to " + path.string();
    return false;
  }
  return true;
}

bool read(const std::filesystem::path& path, Audio& audio, std::string& error) {
  unsigned channels = 0, rate = 0;
  drwav_uint64 frames = 0;
  float* data = drwav_open_file_and_read_pcm_frames_f32(path.c_str(), &channels, &rate, &frames, nullptr);
  if (!data) {
    error = "can't read " + path.string();
    return false;
  }
  audio.channels = channels;
  audio.sampleRate = rate;
  audio.samples.assign(data, data + frames * channels);
  drwav_free(data, nullptr);
  return true;
}

}  // namespace ion::wav
