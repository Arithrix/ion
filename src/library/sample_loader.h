#pragma once

// Loads samples on a disk thread (HANDOFF §4: "sample loading happens on the
// disk thread and hands over finished buffers"). WAV, AIFF and FLAC are
// decoded, the first two channels kept, and converted to 48 kHz with
// libsamplerate's medium sinc converter (DECISIONS D-35).

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine/sample.h"

namespace ion {

// Formats the browser lists and the loader reads.
bool isAudioFile(const std::filesystem::path& path);

// What a file is, read from its header only (fast enough for a list).
struct AudioFileInfo {
  bool ok = false;
  double seconds = 0;
  unsigned rate = 0;
  int channels = 0;
};
AudioFileInfo probeAudioFile(const std::filesystem::path& path);

// A sample from planar 48 kHz audio already in memory (a recording); `right`
// may be null for mono. Peak and overview are filled in.
std::shared_ptr<Sample> makeSample(const float* left, const float* right, int64_t frames, const std::filesystem::path& path);

class SampleLoader {
 public:
  using Ptr = std::shared_ptr<const Sample>;
  static constexpr double kMaxSeconds = 300;  // longer files are refused, not truncated

  SampleLoader();
  ~SampleLoader();
  SampleLoader(const SampleLoader&) = delete;
  SampleLoader& operator=(const SampleLoader&) = delete;

  // Queues a load; `tag` comes back with the result. A preview replaces any
  // preview still waiting, so scrolling fast never builds a backlog.
  void request(const std::filesystem::path& path, uint64_t tag, bool preview = false);

  struct Result {
    uint64_t tag = 0;
    std::filesystem::path path;
    Ptr sample;  // null on error
    std::string error;
    bool preview = false;
  };
  // Finished loads, oldest first. Control thread.
  std::vector<Result> poll();
  // Loads still queued or running.
  int pending() const;

  // Decodes and converts now, on the calling thread (tests, ion-render).
  static Ptr loadNow(const std::filesystem::path& path, std::string& error);

 private:
  struct Job {
    std::filesystem::path path;
    uint64_t tag;
    bool preview;
  };
  void run();
  Ptr cached(const std::filesystem::path& path);

  mutable std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<Job> jobs_;
  std::vector<Result> done_;
  int running_ = 0;
  bool stop_ = false;
  // Already-loaded files, so a sample on several pads (or previewed, then
  // loaded) is decoded once. Keyed by path; checked against the file's time and size.
  struct CacheEntry {
    std::weak_ptr<const Sample> sample;
    std::filesystem::file_time_type time;
    uintmax_t size = 0;
  };
  std::map<std::filesystem::path, CacheEntry> cache_;
  std::thread thread_;
};

}  // namespace ion
