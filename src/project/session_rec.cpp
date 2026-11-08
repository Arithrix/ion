// Session: resampling (HANDOFF §7.6, DECISIONS D-51 to D-53). Takes come back
// from the audio thread in tick(); here they're trimmed, written as WAV +
// sidecar, and sent to the library, a sampler track or drum pads.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <utility>

#include <nlohmann/json.hpp>

#include "audio/wav.h"
#include "core/version.h"
#include "engine/drum/drum_engine.h"
#include "engine/sampler/sampler_engine.h"
#include "project/session.h"

namespace ion {
namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr int kTicksPerBar = 96;
constexpr int64_t kMinTakeFrames = 480;          // 10 ms
constexpr double kFreeSeconds = 180;             // "until stopped" runs out after 3 minutes
constexpr double kSlowestBpm = 30;               // room for a synced take down to this tempo
constexpr int64_t kSliceFadeIn = 24, kSliceFadeOut = 96;  // 0.5 ms, 2 ms

std::string nowIso() {
  const std::time_t t = std::time(nullptr);
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
  return buf;
}

ParamRef mixer(int scope, int index) { return {static_cast<uint8_t>(scope), static_cast<uint16_t>(index)}; }

// "E.PIANO 1" → "e.piano-1": safe in a file name, readable in a list.
std::string slug(const std::string& s) {
  std::string out;
  for (char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || c == '.' || c == '_') out += static_cast<char>(std::tolower(u));
    else if (!out.empty() && out.back() != '-') out += '-';
  }
  while (!out.empty() && (out.back() == '-' || out.back() == '.')) out.pop_back();
  while (!out.empty() && (out.front() == '-' || out.front() == '.')) out.erase(out.begin());
  return out;
}

std::string tracksText(uint8_t bits, const char* joiner) {
  std::string out;
  for (int t = 0; t < kTrackCount; ++t)
    if (bits >> t & 1) out += (out.empty() ? "" : joiner) + std::to_string(t + 1);
  return out;
}

int trackCount(uint8_t bits) { return __builtin_popcount(bits); }

bool writeStereo(const fs::path& path, const float* l, const float* r, int64_t frames, int64_t fadeIn, int64_t fadeOut,
                 std::string& error) {
  wav::Audio a;
  a.sampleRate = kSampleRate;
  a.channels = 2;
  a.samples.resize(static_cast<std::size_t>(frames) * 2);
  for (int64_t i = 0; i < frames; ++i) {
    float g = 1;
    if (i < fadeIn) g = static_cast<float>(i) / static_cast<float>(fadeIn);
    if (frames - 1 - i < fadeOut) g = std::min(g, static_cast<float>(frames - 1 - i) / static_cast<float>(fadeOut));
    a.samples[static_cast<std::size_t>(2 * i)] = l[i] * g;
    a.samples[static_cast<std::size_t>(2 * i + 1)] = r[i] * g;
  }
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  return wav::write(path, a, error);
}

std::string barsText(int bars) { return std::to_string(bars) + (bars == 1 ? " bar" : " bars"); }

}  // namespace

fs::path Session::recordingsDir() const { return name_.empty() ? fs::path() : paths_.projectDir(name_) / "recordings"; }

Session::RecState Session::recState() const {
  if (take_) {
    switch (take_->state.load(std::memory_order_acquire)) {
      case Take::State::Armed: return RecState::Armed;
      case Take::State::Recording: return RecState::Recording;
      default: break;  // finished: tick() picks it up
    }
  }
  return review_ ? RecState::Review : RecState::Idle;
}

bool Session::arm(std::string& error) {
  if (take_) {
    error = "already recording";
    return false;
  }
  if (review_) {
    error = "the last take is waiting: ⏎ puts its slices on pads, Esc keeps just the recording";
    return false;
  }
  if (name_.empty()) {
    error = "the project has no name";
    return false;
  }
  if (rec_.source == RecSource::Tracks) {
    if (rec_.tracks == 0) {
      error = "choose the tracks to record (⏎ on the tracks row)";
      return false;
    }
    bool any = false;
    for (int t = 0; t < kTrackCount; ++t) any |= (rec_.tracks >> t & 1) && tracks_[static_cast<std::size_t>(t)].engine != EngineType::None;
    if (!any) {
      error = trackCount(rec_.tracks) == 1 ? "track " + tracksText(rec_.tracks, "") + " is empty"
                                           : "tracks " + tracksText(rec_.tracks, " and ") + " are empty";
      return false;
    }
  }
  auto t = std::make_unique<Take>();
  t->source = rec_.source;
  t->tracks = rec_.source == RecSource::Tracks ? rec_.tracks : 0;
  t->tap = rec_.tap;
  t->synced = rec_.synced;
  t->bars = std::clamp(rec_.bars, 0, 16);
  // Room for the take, allocated (and zeroed) here so the audio thread never faults pages in.
  const double bpm = module_.status().clockPresent.load() && module_.status().bpm.load() > 0 ? module_.status().bpm.load() : 120.0;
  double seconds = kFreeSeconds;
  if (t->bars > 0) seconds = t->bars * 4 * 60.0 / (t->synced ? kSlowestBpm : bpm);
  t->capacity = static_cast<int64_t>(seconds * kSampleRate) + 8192;
  t->left.assign(static_cast<std::size_t>(t->capacity), 0.0f);
  t->right.assign(static_cast<std::size_t>(t->capacity), 0.0f);
  if (t->synced) {
    const int bars = t->bars > 0 ? t->bars : static_cast<int>(kFreeSeconds / (4 * 60.0 / kSlowestBpm));
    t->ticks.assign(static_cast<std::size_t>(bars * kTicksPerBar + 2), 0);
  }
  Command c;
  c.type = Command::Type::Record;
  c.take = t.get();
  take_ = std::move(t);
  send(c);
  return true;
}

void Session::stopTake() {
  if (!take_) return;
  Command c;
  c.type = Command::Type::StopRecord;
  send(c);
}

void Session::cancelTake() {
  if (!take_) return;
  Command c;
  c.type = Command::Type::CancelRecord;
  send(c);
}

std::string Session::soundSlug(int t) const {
  const TrackState& tr = tracks_[static_cast<std::size_t>(t)];
  std::string s;
  if (tr.engine == EngineType::Fm && tr.hasVoice) s = syx::name(tr.voice);
  else if (tr.engine == EngineType::Drum) s = tr.kit;
  else if (tr.engine == EngineType::Sampler && !tr.pads[0].sample.empty()) s = fs::path(tr.pads[0].sample).stem().string();
  s = slug(s);
  return s.empty() ? "track" + std::to_string(t + 1) : s;
}

void Session::finishTake(Take* done) {
  if (!take_ || take_.get() != done) return;  // not ours (can't happen): leave it alone
  const std::unique_ptr<Take> take = std::move(take_);
  if (std::exchange(takeAbandoned_, false)) return;
  if (take->state.load(std::memory_order_acquire) == Take::State::Cancelled) {
    message_ = "take cancelled";
    return;
  }

  // The length. A synced take is trimmed to whole bars as the clock measured
  // them over the whole take (a least-squares line through every tick), so a
  // loop is exactly as long as the bars it holds, whatever the jitter on any one tick.
  int64_t frames = take->frames.load(std::memory_order_acquire);
  int bars = 0;
  double tempo = 0;
  if (take->synced) {
    const int ticks = take->tickCount.load(std::memory_order_acquire);
    int b = std::min(take->targetBars.load(), (ticks - 1) / kTicksPerBar);
    while (b > 0 && take->ticks[static_cast<std::size_t>(b * kTicksPerBar)] > frames) --b;
    if (b <= 0) {
      message_ = "the clock stopped before a whole bar was recorded: nothing kept";
      return;
    }
    const int n = b * kTicksPerBar + 1;
    double mx = (n - 1) / 2.0, my = 0;
    for (int i = 0; i < n; ++i) my += static_cast<double>(take->ticks[static_cast<std::size_t>(i)]);
    my /= n;
    double sxy = 0, sxx = 0;
    for (int i = 0; i < n; ++i) {
      sxy += (i - mx) * (static_cast<double>(take->ticks[static_cast<std::size_t>(i)]) - my);
      sxx += (i - mx) * (i - mx);
    }
    const double perTick = sxy / sxx;
    frames = std::min(frames, static_cast<int64_t>(std::llround(perTick * (n - 1))));
    tempo = 60.0 * kSampleRate / (perTick * 24);
    bars = b;
  } else if (take->bars > 0 && !take->cutShort) {
    bars = take->bars;
    tempo = take->bpm;
  }
  if (frames < kMinTakeFrames) {
    message_ = "the take was too short to keep";
    return;
  }
  tempo = std::round(tempo * 100) / 100;

  // A name from what was recorded, its tempo and length: "e.piano-1_124bpm_4bar".
  std::string what = take->source == RecSource::Master ? "mix"
                     : trackCount(take->tracks) == 1   ? soundSlug(__builtin_ctz(take->tracks))
                                                       : "tracks-" + tracksText(take->tracks, "-");
  char tail[48];
  if (bars > 0) std::snprintf(tail, sizeof tail, "_%gbpm_%dbar", std::round(tempo * 10) / 10, bars);
  else std::snprintf(tail, sizeof tail, "_%.1fs", static_cast<double>(frames) / kSampleRate);
  const fs::path dir = recordingsDir();
  std::string stem = what + tail;
  std::error_code ec;
  for (int k = 2; fs::exists(dir / (stem + ".wav"), ec) || fs::exists(dir / stem, ec); ++k) stem = what + tail + "-" + std::to_string(k);
  const fs::path file = dir / (stem + ".wav");

  std::string error;
  if (!writeStereo(file, take->left.data(), take->right.data(), frames, 0, 0, error)) {
    message_ = "recording not saved: " + error;
    return;
  }
  const std::string sources = take->source == RecSource::Master ? "master"
                              : trackCount(take->tracks) == 1   ? "track " + tracksText(take->tracks, "")
                                                                : "tracks " + tracksText(take->tracks, " + ");
  json side;
  side["ion_recording"] = 1;
  side["tempo"] = tempo;
  side["bars"] = bars;
  side["beats_per_bar"] = 4;
  side["sources"] = sources;
  side["recorded_at"] = nowIso();
  side["frames"] = frames;
  side["sample_rate"] = kSampleRate;
  side["synced"] = take->synced;
  if (take->synced) side["start_bar"] = take->startBar + 1;
  if (take->source == RecSource::Tracks) side["tap"] = take->tap == RecTap::Post ? "post-fx" : "pre-fx";
  side["saved_by"] = versionLine();
  // What made it, so a bounced track can be rebuilt: the source tracks (all of them for the master).
  json settings = json::array();
  for (int t = 0; t < kTrackCount; ++t) {
    const bool src = take->source == RecSource::Master ? tracks_[static_cast<std::size_t>(t)].engine != EngineType::None
                                                       : (take->tracks >> t & 1) != 0;
    if (src) settings.push_back(trackJson(t));
  }
  side["settings"] = settings;
  const fs::path sidecar = sidecarPath(file);

  undo_ = TakeUndo{};
  undo_->name = stem;
  undo_->files = {file, sidecar};
  undo_->sidecar = sidecar;

  const RecSetup::Dest dest = rec_.dest;
  const int dt = std::clamp(rec_.destTrack, 0, kTrackCount - 1);
  // Mute what was recorded, except a track that's about to play the recording.
  if (rec_.muteSources && take->source == RecSource::Tracks)
    for (int t = 0; t < kTrackCount; ++t) {
      if (!(take->tracks >> t & 1) || (dest != RecSetup::Dest::Library && t == dt)) continue;
      const ParamRef m = mixer(t, kMute);
      undo_->mutes.emplace_back(t, param(m));
      if (param(m) < 0.5f) setParam(m, 1);
    }

  const std::string stored = "recordings/" + stem + ".wav";
  std::string sentTo = "library";
  char secs[24];
  std::snprintf(secs, sizeof secs, "%.1f s", static_cast<double>(frames) / kSampleRate);
  const std::string len = bars > 0 ? barsText(bars) : std::string(secs);
  if (dest == RecSetup::Dest::Sampler) {
    undo_->tracks.emplace_back(dt, trackJson(dt).dump());
    sendToSampler(dt, stored);
    sentTo = "sampler · track " + std::to_string(dt + 1);
    message_ = "recorded " + stem + " (" + len + ") → sampler on track " + std::to_string(dt + 1) + ", looped, root C3";
  } else if (dest == RecSetup::Dest::Drum) {
    SliceReview r;
    r.file = file;
    r.sample = makeSample(take->left.data(), take->right.data(), frames, file);
    r.tempo = tempo;
    r.bars = bars;
    r.track = dt;
    r.firstPad = std::clamp(rec_.firstPad, 0, DrumEngine::kPads - 1);
    std::vector<float> mono(static_cast<std::size_t>(frames));
    for (int64_t i = 0; i < frames; ++i)
      mono[static_cast<std::size_t>(i)] = 0.5f * (take->left[static_cast<std::size_t>(i)] + take->right[static_cast<std::size_t>(i)]);
    switch (rec_.slice) {
      case SliceMode::Equal: r.slices = equalSlices(frames, std::min(rec_.sliceCount, r.maxSlices())); break;
      case SliceMode::Chords: r.slices = chordSlices(mono, tempo > 0 ? 60.0 * kSampleRate / tempo : 0, r.maxSlices()); break;
      case SliceMode::Transients: r.slices = transientSlices(mono, r.maxSlices()); break;
    }
    const int n = static_cast<int>(r.slices.size());
    review_ = std::move(r);
    sentTo = "library (slicing)";
    message_ = "recorded " + stem + " (" + len + "): " + std::to_string(n) + (n == 1 ? " slice" : " slices") +
               " · check them, then ⏎ puts them on pads";
  } else {
    message_ = "recorded " + stem + " (" + len + ") into the project's recordings";
  }
  side["sent_to"] = sentTo;
  if (!writeFileAtomic(sidecar, side.dump(2) + "\n", error)) message_ = "recording saved, but not its sidecar: " + error;
  markDirty();
}

void Session::sendToSampler(int t, const std::string& stored) {
  if (tracks_[static_cast<std::size_t>(t)].engine != EngineType::Sampler) setEngine(t, EngineType::Sampler);
  setPadSample(t, 0, stored);
  // Plays as recorded from C3, looping the whole take (D-53).
  auto set = [&](int p, float v) { setIfDifferent(engineParamRef(t, p), v); };
  set(SamplerEngine::kMode, 0);
  set(SamplerEngine::kRoot, 60);
  set(SamplerEngine::kStart, 0);
  set(SamplerEngine::kLoop, SamplerEngine::kLoopForward);
  set(SamplerEngine::kLoopStart, 0);
  set(SamplerEngine::kLoopEnd, 1);
  set(SamplerEngine::kCrossfade, 0);
}

// ---- slice review ------------------------------------------------------------------------

void Session::moveSlice(int i, int64_t delta) {
  if (!review_ || i <= 0 || i >= static_cast<int>(review_->slices.size())) return;
  auto& s = review_->slices;
  const auto k = static_cast<std::size_t>(i);
  const int64_t lo = s[k - 1] + kMinTakeFrames;
  const int64_t hi = (k + 1 < s.size() ? s[k + 1] : review_->sample->frames) - kMinTakeFrames;
  if (lo > hi) return;
  s[k] = std::clamp(s[k] + delta, lo, hi);
}

int Session::addSlice(int64_t at) {
  if (!review_ || static_cast<int>(review_->slices.size()) >= review_->maxSlices()) return -1;
  auto& s = review_->slices;
  at = std::clamp<int64_t>(at, kMinTakeFrames, review_->sample->frames - kMinTakeFrames);
  for (int64_t x : s)
    if (std::llabs(x - at) < kMinTakeFrames) return -1;
  s.insert(std::upper_bound(s.begin(), s.end(), at), at);
  return static_cast<int>(std::find(s.begin(), s.end(), at) - s.begin());
}

void Session::removeSlice(int i) {
  if (!review_ || i <= 0 || i >= static_cast<int>(review_->slices.size())) return;
  review_->slices.erase(review_->slices.begin() + i);
}

void Session::playSlice(int i) {
  if (!review_ || i < 0 || i >= static_cast<int>(review_->slices.size())) return;
  const Sample& whole = *review_->sample;
  const auto k = static_cast<std::size_t>(i);
  const int64_t from = review_->slices[k];
  const int64_t to = k + 1 < review_->slices.size() ? review_->slices[k + 1] : whole.frames;
  SampleLoader::Ptr part = makeSample(whole.channel(0) + from, whole.channel(1) + from, to - from, whole.path);
  pin(part);
  Command c;
  c.type = Command::Type::Preview;
  c.sample = part.get();
  c.value = 1.0f;  // as it will play from the pad
  send(c);
}

void Session::keepRecording() {
  if (!review_) return;
  message_ = "kept " + review_->file.filename().string() + " in the project's recordings";
  review_.reset();
}

bool Session::commitSlices(std::string& error) {
  if (!review_) {
    error = "no slices waiting";
    return false;
  }
  const SliceReview& r = *review_;
  const int t = r.track;
  const int n = std::min(static_cast<int>(r.slices.size()), r.maxSlices());
  const Sample& whole = *r.sample;

  // Each slice as its own file, beside the recording: recordings/<take>/slice 01.wav …
  const fs::path folder = r.file.parent_path() / r.file.stem();
  std::vector<std::string> stored;
  for (int i = 0; i < n; ++i) {
    const int64_t from = r.slices[static_cast<std::size_t>(i)];
    const int64_t to = i + 1 < static_cast<int>(r.slices.size()) ? r.slices[static_cast<std::size_t>(i + 1)] : whole.frames;
    char name[32];
    std::snprintf(name, sizeof name, "slice %02d.wav", i + 1);
    if (!writeStereo(folder / name, whole.channel(0) + from, whole.channel(1) + from, to - from, from > 0 ? kSliceFadeIn : 0,
                     kSliceFadeOut, error))
      return false;
    stored.push_back("recordings/" + r.file.stem().string() + "/" + name);
  }
  if (undo_) {
    undo_->files.push_back(folder);
    bool have = false;
    for (const auto& [ut, js] : undo_->tracks) have |= ut == t;
    if (!have) undo_->tracks.emplace_back(t, trackJson(t).dump());
  }

  if (tracks_[static_cast<std::size_t>(t)].engine != EngineType::Drum) setEngine(t, EngineType::Drum);
  // A choke group no other pad on the track uses, so the slices cut each other off and nothing else.
  std::array<bool, 5> used{};
  for (int pad = 0; pad < DrumEngine::kPads; ++pad) {
    if (pad >= r.firstPad && pad < r.firstPad + n) continue;
    if (tracks_[static_cast<std::size_t>(t)].pads[static_cast<std::size_t>(pad)].sample.empty()) continue;
    used[static_cast<std::size_t>(std::clamp(static_cast<int>(param(engineParamRef(t, DrumEngine::param(pad, DrumEngine::kChoke)))), 0, 4))] = true;
  }
  int group = 1;
  while (group < 4 && used[static_cast<std::size_t>(group)]) ++group;

  const auto table = DrumEngine::padParamTable();
  for (int i = 0; i < n; ++i) {
    const int pad = r.firstPad + i;
    setPadSample(t, pad, stored[static_cast<std::size_t>(i)]);
    // The pad starts fresh (its note stays), one-shot, in the group.
    for (int p = 0; p < DrumEngine::kPadParamCount; ++p)
      if (p != DrumEngine::kNote) setIfDifferent(engineParamRef(t, DrumEngine::param(pad, p)), table[static_cast<std::size_t>(p)].def);
    setIfDifferent(engineParamRef(t, DrumEngine::param(pad, DrumEngine::kMode)), DrumEngine::kOneShot);
    setIfDifferent(engineParamRef(t, DrumEngine::param(pad, DrumEngine::kChoke)), static_cast<float>(group));
  }

  // The sidecar says where it went, and where the cuts were.
  char where[96];
  std::snprintf(where, sizeof where, "drum · track %d · pads %d–%d", t + 1, r.firstPad + 1, r.firstPad + n);
  std::string text;
  const fs::path sidecar = sidecarPath(r.file);
  if (readFile(sidecar, text)) {
    json side = json::parse(text, nullptr, false);
    if (side.is_object()) {
      side["sent_to"] = where;
      side["slices"] = std::vector<int64_t>(r.slices.begin(), r.slices.begin() + n);
      std::string ignored;
      writeFileAtomic(sidecar, side.dump(2) + "\n", ignored);
    }
  }
  char msg[160];
  std::snprintf(msg, sizeof msg, "%d %s → pads %d–%d of track %d, choke group %c", n, n == 1 ? "slice" : "slices", r.firstPad + 1,
                r.firstPad + n, t + 1, static_cast<char>('A' + group - 1));
  message_ = msg;
  review_.reset();
  markDirty();
  return true;
}

// ---- undo -----------------------------------------------------------------------------------

bool Session::undoTake(std::string& error) {
  if (take_) {
    error = "a take is running: Esc cancels it";
    return false;
  }
  if (!undo_) {
    error = "no take to undo";
    return false;
  }
  const TakeUndo u = std::move(*undo_);
  undo_.reset();
  review_.reset();
  for (const auto& [t, text] : u.tracks) {
    const json jt = json::parse(text, nullptr, false);
    if (jt.is_object()) trackFromJson(t, jt);
  }
  for (const auto& [t, mute] : u.mutes) setParam(mixer(t, kMute), mute);
  std::error_code ec;
  for (const fs::path& f : u.files) fs::remove_all(f, ec);
  message_ = "undid " + u.name + ": its files are gone and the tracks are back as they were";
  markDirty();
  return true;
}

// ---- the list -------------------------------------------------------------------------------

std::vector<Session::RecordingEntry> Session::recordings(int max) const {
  std::vector<std::pair<fs::file_time_type, fs::path>> found;
  std::error_code ec;
  const fs::path dir = recordingsDir();
  if (dir.empty()) return {};
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_regular_file(ec) && isAudioFile(e.path())) found.emplace_back(e.last_write_time(ec), e.path());
  std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first != b.first ? a.first > b.first : a.second > b.second; });
  std::vector<RecordingEntry> out;
  for (const auto& [time, path] : found) {
    if (static_cast<int>(out.size()) >= max) break;
    RecordingEntry r;
    r.path = path;
    r.info = readRecordingInfo(path);
    r.seconds = probeAudioFile(path).seconds;
    out.push_back(std::move(r));
  }
  return out;
}

}  // namespace ion
