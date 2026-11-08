#include "library/sample_loader.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

#include <dr_wav.h>  // the implementation is in audio/wav.cpp
#define DR_FLAC_IMPLEMENTATION
#include <dr_flac.h>
#include <samplerate.h>

namespace ion {
namespace fs = std::filesystem;

namespace {

std::string lowerExtension(const fs::path& p) {
  std::string e = p.extension().string();
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}

bool isFlac(const fs::path& p) { return lowerExtension(p) == ".flac"; }

// Interleaved float frames at the file's own rate.
struct Decoded {
  std::vector<float> samples;
  uint64_t frames = 0;
  unsigned rate = 0;
  int channels = 0;
  int bits = 0;
};

bool decode(const fs::path& path, Decoded& out, std::string& error) {
  if (isFlac(path)) {
    drflac* f = drflac_open_file(path.c_str(), nullptr);
    if (!f) {
      error = "not a FLAC file this can read";
      return false;
    }
    out.rate = f->sampleRate;
    out.channels = f->channels;
    out.bits = f->bitsPerSample;
    out.frames = f->totalPCMFrameCount;
    if (out.rate == 0 || out.channels == 0 || static_cast<double>(out.frames) / out.rate > SampleLoader::kMaxSeconds) {
      drflac_close(f);
      error = out.rate == 0 ? "no audio in the file" : "longer than 5 minutes";
      return false;
    }
    out.samples.resize(out.frames * static_cast<uint64_t>(out.channels));
    out.frames = drflac_read_pcm_frames_f32(f, out.frames, out.samples.data());
    out.samples.resize(out.frames * static_cast<uint64_t>(out.channels));
    drflac_close(f);
    return true;
  }
  drwav w;
  if (!drwav_init_file(&w, path.c_str(), nullptr)) {
    error = "not a WAV or AIFF file this can read";
    return false;
  }
  out.rate = w.sampleRate;
  out.channels = w.channels;
  out.bits = w.translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT ? 0 : w.bitsPerSample;
  out.frames = w.totalPCMFrameCount;
  if (out.rate == 0 || out.channels == 0 || static_cast<double>(out.frames) / out.rate > SampleLoader::kMaxSeconds) {
    drwav_uninit(&w);
    error = out.rate == 0 ? "no audio in the file" : "longer than 5 minutes";
    return false;
  }
  out.samples.resize(out.frames * static_cast<uint64_t>(out.channels));
  out.frames = drwav_read_pcm_frames_f32(&w, out.frames, out.samples.data());
  out.samples.resize(out.frames * static_cast<uint64_t>(out.channels));
  drwav_uninit(&w);
  if (out.frames == 0) {
    error = "no audio in the file (or a compression this can't decode)";
    return false;
  }
  return true;
}

}  // namespace

bool isAudioFile(const fs::path& path) {
  const std::string e = lowerExtension(path);
  return e == ".wav" || e == ".wave" || e == ".aif" || e == ".aiff" || e == ".aifc" || e == ".flac";
}

AudioFileInfo probeAudioFile(const fs::path& path) {
  AudioFileInfo info;
  if (isFlac(path)) {
    if (drflac* f = drflac_open_file(path.c_str(), nullptr)) {
      info = {f->sampleRate > 0, f->sampleRate ? static_cast<double>(f->totalPCMFrameCount) / f->sampleRate : 0, f->sampleRate,
              f->channels};
      drflac_close(f);
    }
    return info;
  }
  drwav w;
  if (drwav_init_file(&w, path.c_str(), nullptr)) {
    info = {w.sampleRate > 0, w.sampleRate ? static_cast<double>(w.totalPCMFrameCount) / w.sampleRate : 0, w.sampleRate,
            w.channels};
    drwav_uninit(&w);
  }
  return info;
}

std::shared_ptr<Sample> makeSample(const float* left, const float* right, int64_t frames, const fs::path& path) {
  auto s = std::make_shared<Sample>();
  const int keep = right ? 2 : 1;
  s->channels = s->sourceChannels = keep;
  s->frames = std::max<int64_t>(frames, 0);
  s->path = path;
  const std::size_t stride = static_cast<std::size_t>(s->frames + 2 * Sample::kGuard);
  s->data.assign(stride * static_cast<std::size_t>(keep), 0.0f);
  for (int c = 0; c < keep; ++c) {
    const float* src = c == 0 ? left : right;
    float* dst = s->data.data() + stride * static_cast<std::size_t>(c) + Sample::kGuard;
    for (int64_t i = 0; i < s->frames; ++i) {
      const float v = std::isfinite(src[i]) ? src[i] : 0.0f;
      dst[i] = v;
      s->peak = std::max(s->peak, std::fabs(v));
    }
  }
  for (int b = 0; b < Sample::kOverview; ++b) {
    const int64_t from = s->frames * b / Sample::kOverview, to = std::max(from + 1, s->frames * (b + 1) / Sample::kOverview);
    float p = 0;
    for (int c = 0; c < keep; ++c)
      for (int64_t i = from; i < std::min(to, s->frames); ++i) p = std::max(p, std::fabs(s->channel(c)[i]));
    s->overview[static_cast<std::size_t>(b)] = std::min(1.0f, p);
  }
  return s;
}

SampleLoader::Ptr SampleLoader::loadNow(const fs::path& path, std::string& error) {
  Decoded d;
  if (!decode(path, d, error)) return nullptr;
  const int keep = std::min(d.channels, 2);

  // To 48 kHz, still interleaved (libsamplerate works that way).
  std::vector<float> in;
  if (keep == d.channels) {
    in = std::move(d.samples);
  } else {  // a multichannel file: its first two channels
    in.resize(d.frames * 2);
    for (uint64_t i = 0; i < d.frames; ++i) {
      in[2 * i] = d.samples[i * static_cast<uint64_t>(d.channels)];
      in[2 * i + 1] = d.samples[i * static_cast<uint64_t>(d.channels) + 1];
    }
  }
  std::vector<float> at48;
  int64_t frames = static_cast<int64_t>(d.frames);
  if (d.rate != 48000) {
    const double ratio = 48000.0 / d.rate;
    const auto outFrames = static_cast<long>(std::ceil(static_cast<double>(d.frames) * ratio)) + 16;
    at48.resize(static_cast<std::size_t>(outFrames) * static_cast<std::size_t>(keep));
    SRC_DATA src{};
    src.data_in = in.data();
    src.input_frames = static_cast<long>(d.frames);
    src.data_out = at48.data();
    src.output_frames = outFrames;
    src.src_ratio = ratio;
    if (const int err = src_simple(&src, SRC_SINC_MEDIUM_QUALITY, keep)) {
      error = std::string("sample-rate conversion failed: ") + src_strerror(err);
      return nullptr;
    }
    frames = src.output_frames_gen;
  } else {
    at48 = std::move(in);
  }
  if (frames <= 0) {
    error = "no audio in the file";
    return nullptr;
  }

  auto s = std::make_shared<Sample>();
  s->channels = keep;
  s->frames = frames;
  s->path = path;
  s->sourceRate = d.rate;
  s->sourceChannels = d.channels;
  s->sourceBits = d.bits;
  const std::size_t stride = static_cast<std::size_t>(frames + 2 * Sample::kGuard);
  s->data.assign(stride * static_cast<std::size_t>(keep), 0.0f);
  for (int c = 0; c < keep; ++c) {
    float* dst = s->data.data() + stride * static_cast<std::size_t>(c) + Sample::kGuard;
    for (int64_t i = 0; i < frames; ++i) {
      float v = at48[static_cast<std::size_t>(i) * static_cast<std::size_t>(keep) + static_cast<std::size_t>(c)];
      if (!std::isfinite(v)) v = 0;  // a broken float file never reaches the engine
      dst[i] = v;
      s->peak = std::max(s->peak, std::fabs(v));
    }
  }
  for (int b = 0; b < Sample::kOverview; ++b) {
    const int64_t from = frames * b / Sample::kOverview, to = std::max(from + 1, frames * (b + 1) / Sample::kOverview);
    float p = 0;
    for (int c = 0; c < keep; ++c)
      for (int64_t i = from; i < std::min(to, frames); ++i) p = std::max(p, std::fabs(s->channel(c)[i]));
    s->overview[static_cast<std::size_t>(b)] = std::min(1.0f, p);
  }
  return s;
}

// ---- the disk thread ----------------------------------------------------------------

SampleLoader::SampleLoader() : thread_([this] { run(); }) {}

SampleLoader::~SampleLoader() {
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  thread_.join();
}

void SampleLoader::request(const fs::path& path, uint64_t tag, bool preview) {
  {
    std::lock_guard lock(mutex_);
    if (preview) std::erase_if(jobs_, [](const Job& j) { return j.preview; });
    jobs_.push_back({path, tag, preview});
  }
  wake_.notify_one();
}

std::vector<SampleLoader::Result> SampleLoader::poll() {
  std::lock_guard lock(mutex_);
  return std::exchange(done_, {});
}

int SampleLoader::pending() const {
  std::lock_guard lock(mutex_);
  return static_cast<int>(jobs_.size()) + running_;
}

SampleLoader::Ptr SampleLoader::cached(const fs::path& path) {
  std::error_code ec;
  const auto time = fs::last_write_time(path, ec);
  const auto size = fs::file_size(path, ec);
  if (ec) return nullptr;
  std::lock_guard lock(mutex_);
  auto it = cache_.find(path);
  if (it == cache_.end() || it->second.time != time || it->second.size != size) return nullptr;
  return it->second.sample.lock();
}

void SampleLoader::run() {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      wake_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
      if (stop_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
      ++running_;
    }
    Result r{job.tag, job.path, cached(job.path), {}, job.preview};
    if (!r.sample) {
      std::error_code ec;
      if (!fs::is_regular_file(job.path, ec)) r.error = "file not found";
      else r.sample = loadNow(job.path, r.error);
      if (r.sample) {
        const auto time = fs::last_write_time(job.path, ec);
        const auto size = fs::file_size(job.path, ec);
        std::lock_guard lock(mutex_);
        std::erase_if(cache_, [](const auto& e) { return e.second.sample.expired(); });
        cache_[job.path] = {r.sample, time, size};
      }
    }
    std::lock_guard lock(mutex_);
    done_.push_back(std::move(r));
    --running_;
  }
}

}  // namespace ion
