#pragma once

#include <array>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "engine/drum/drum_engine.h"
#include "engine/fm/dx7_voice.h"
#include "engine/fx/effects.h"
#include "engine/module.h"
#include "engine/take.h"
#include "library/fm_library.h"
#include "library/recording.h"
#include "library/slicer.h"
#include "library/sample_loader.h"
#include "library/syx.h"
#include "project/config.h"
#include "project/paths.h"

namespace ion {

// 1: Phase 1. 2: FM macros, the program list, library ids (v0.2.0). 3: drum kits (v0.3.0).
// 4: inserts, sends, the returns and the master chain (v0.4.0). 5: sampler tracks (v0.5.0).
// 6: the Rec screen's setup (v0.6.0).
inline constexpr int kProjectFormat = 6;

struct PadState {
  // As saved: an absolute path, or one relative to the project folder
  // ("samples/bd.wav", after collect). Empty: no sample.
  std::string sample;
  bool operator==(const PadState&) const = default;
};

struct TrackState {
  EngineType engine = EngineType::None;
  int channel = 0;  // 0-15
  // FM. The voice is kept in the project too, so a project plays the same
  // even on a machine whose library doesn't have it.
  bool hasVoice = false;
  syx::Voice voice{};       // as stored, without macros
  std::string voiceSource;  // bank it came from, e.g. "rom1a", or "saved"
  int64_t libraryId = 0;    // in this machine's library, or 0
  // Drum: 16 pads; Sampler: its one sample is pads[0]. Their settings are
  // engine parameters, like FM's macros.
  std::array<PadState, DrumEngine::kPads> pads{};
  std::string kit;  // the kit's name, if it was loaded from or saved as one
  bool operator==(const TrackState&) const = default;
};

// How the next take is recorded and where it goes (the Rec screen's Setup).
struct RecSetup {
  RecSource source = RecSource::Master;
  uint8_t tracks = 0;      // RecSource::Tracks: bit t = track t
  RecTap tap = RecTap::Post;
  int bars = 4;            // 1, 2, 4, 8 or 16; 0 = until stopped
  bool synced = true;      // start on the next bar of MIDI clock; else at once
  enum class Dest : uint8_t { Library, Sampler, Drum };
  Dest dest = Dest::Library;
  int destTrack = 0;
  int firstPad = 0;        // Drum: slices go onto this pad and up
  SliceMode slice = SliceMode::Chords;
  int sliceCount = 8;      // SliceMode::Equal
  bool muteSources = true; // after the take, mute the tracks it recorded
};

// One entry in the project's program-change list.
struct ProgramEntry {
  syx::Voice voice{};
  std::string source;
  bool operator==(const ProgramEntry&) const = default;
};

// The project document and everything that changes it.
//
// Session is the only thing that sends commands to the Module, from a single
// control thread: the UI thread, or main() in headless mode. It keeps what the
// audio thread can't be asked for (engine types, voices, channel map, CC map);
// parameter values live in the Module's ParamStore.
class Session {
 public:
  Session(Module& module, Paths paths, Config& config);
  ~Session();

  // ---- project files ----
  void newProject(const std::string& name);  // empty tracks, defaults
  bool load(const std::string& name, std::string& error);
  bool save(std::string& error);
  bool saveAs(const std::string& name, std::string& error);
  // An autosave newer than the saved project (or with no saved project).
  bool hasRecovery(const std::string& name) const;
  bool recover(const std::string& name, std::string& error);
  void discardRecovery();
  const std::string& name() const { return name_; }
  bool dirty() const;
  // Writes the autosave now if anything changed since the last one (clean exit).
  void autosaveIfNeeded();
  std::vector<std::string> listProjects() const;
  // What's in a saved project, for the Library screen, without loading it.
  struct ProjectSummary {
    struct Track {
      EngineType engine = EngineType::None;
      std::string sound;
    };
    std::array<Track, kTrackCount> tracks{};
    int programs = 0;
    std::string savedAt;  // ISO time from the file
    bool ok = false;
  };
  ProjectSummary projectSummary(const std::string& name) const;
  std::string lastProject() const;  // from state.toml
  // A fresh name like "sketch 2026-11-06" that isn't taken.
  std::string freshName() const;

  // Called ~10 times a second: handles audio-thread notifications, autosaves.
  void tick();
  // The last thing worth telling the user ("learned cc 74 …"); cleared on read.
  std::string takeMessage();

  // ---- tracks ----
  const TrackState& track(int t) const { return tracks_[static_cast<std::size_t>(t)]; }
  void setEngine(int t, EngineType type);
  void setVoice(int t, const syx::Voice& voice, const std::string& source);
  void setChannel(int t, int channel);

  // ---- parameters ----
  // The value the user set (or a CC moved it to); see values_.
  float param(ParamRef ref) const { return values_[static_cast<std::size_t>(ParamStore::index(ref))]; }
  const ParamInfo* paramInfo(ParamRef ref) const;
  void setParam(ParamRef ref, float value);
  void nudgeParam(ParamRef ref, int direction, bool coarse);
  std::string paramLabel(ParamRef ref) const;  // "Track 3 · Fader"
  std::string paramKey(ParamRef ref) const;    // "track3.fader"
  std::optional<ParamRef> parseParamKey(const std::string& key) const;

  // ---- CC map ----
  struct CcEntry {
    int slot;
    CcMapping mapping;
  };
  std::vector<CcEntry> ccMappings() const;
  void learn(ParamRef ref);
  void cancelLearn();
  std::optional<ParamRef> learning() const { return learning_; }
  void removeMapping(int slot);
  // Maps a CC (channel 0-15 or kAnyChannel) to a parameter, replacing anything
  // that CC or that parameter was mapped to. Returns the slot, or -1 if full.
  int addMapping(uint8_t channel, uint8_t cc, ParamRef target, float lo = 0.0f, float hi = 1.0f);
  void setMappingRange(int slot, float lo, float hi);

  // ---- playing ----
  void sendMidi(const MidiMessage& m);  // dispatched exactly like OXI input
  void panic();
  void setTiming(MidiTiming timing);    // also stored in the config

  // ---- the FM library ----
  // Opens ~/.local/share/ion/library.db. Without it, browsing is empty but
  // everything else works (projects carry their own voices).
  bool openLibrary(std::string& error);
  FmLibrary* library() { return library_.get(); }
  const FmLibrary* library() const { return library_.get(); }
  // Loads a library patch onto FM track t. Macros stay where they are: they
  // apply to whatever patch is loaded.
  void loadPatch(int t, const FmPatch& patch);
  dx7::Macros macros(int t) const;
  void resetMacros(int t);  // "undo edits"
  // The track's voice with its macros baked in, as a new library voice named
  // `name`, which then replaces it on the track with the macros at zero.
  // Returns the library id (an existing one if the result is a duplicate).
  int64_t savePatchAs(int t, const std::string& name, std::string& error);
  // Writes `voices` as 32-voice banks: ~/ion/exports/<name>.syx, or
  // <name> 1.syx, <name> 2.syx … when there are more than 32.
  bool exportVoices(const std::vector<syx::Voice>& voices, const std::string& name,
                    std::vector<std::filesystem::path>& written, std::string& error);

  // ---- program change (HANDOFF §6.3): a per-project list of up to 128 voices ----
  const std::vector<ProgramEntry>& programs() const { return programs_; }
  int programIndex(const syx::Voice& voice) const;  // -1 if not in the list
  // Adds the voice at the end, or removes it if it's already there. False if the list is full.
  bool toggleProgram(const syx::Voice& voice, const std::string& source);
  void moveProgram(int index, int delta);
  void setProgramChange(bool on);  // also stored in the config

  // ---- drum kits (HANDOFF §6.1) ----
  // Puts the sample at `path` on a pad of drum track t. It loads on the disk
  // thread and starts playing once tick() hands it over; the pad's old sample
  // plays until then. An empty path clears the pad.
  void setPadSample(int t, int pad, const std::string& path);
  enum class PadStatus { Empty, Loading, Ready, Failed };
  PadStatus padStatus(int t, int pad) const;
  const std::string& padError(int t, int pad) const;  // why it Failed
  SampleLoader::Ptr padSample(int t, int pad) const;   // null until loaded
  std::string padName(int t, int pad) const;           // the file name without extension, or ""
  // A saved pad path made absolute (relative ones are inside the project folder).
  std::filesystem::path resolveSample(const std::string& stored) const;
  // Copies every sample the drum tracks use from outside the project folder
  // into its samples/ folder and points the pads at the copies, so the
  // project can be moved or backed up whole. Returns the number of files copied.
  int collectSamples(std::string& error);
  // Kits: ~/ion/kits/<name>.json, with each pad's sample (absolute path) and settings.
  bool saveKit(int t, const std::string& name, std::string& error);
  bool loadKit(int t, const std::filesystem::path& file, std::string& error);
  std::vector<std::string> listKits() const;
  struct KitSummary {
    std::array<std::string, DrumEngine::kPads> samples{};  // as saved; empty = no sample
    std::string savedAt;
    bool ok = false;
  };
  KitSummary kitSummary(const std::string& name) const;

  // The sample browser's preview. Loads the file and, if `play`, plays it once
  // into the master. previewed() returns it once loaded (for the waveform).
  void preview(const std::filesystem::path& path, bool play);
  void stopPreview();
  SampleLoader::Ptr previewed(const std::filesystem::path& path) const;
  std::string previewError(const std::filesystem::path& path) const;
  // Loads queued or waiting for tick().
  int samplesLoading() const;
  // Waits until every pad sample (and preview) is loaded and handed to the engine (ion-render, tests).
  bool waitForSamples(int timeoutMs = 10000);
  int pinnedSamples() const { return static_cast<int>(pinned_.size()); }  // for tests

  // ---- effects (HANDOFF §7) ----
  // Three insert slots in every scope (tracks, master, the two returns). A
  // return's slot 0 is its own delay or reverb and can't be changed.
  EffectType insert(int scope, int slot) const;
  bool insertFixed(int scope, int slot) const;
  // A new effect of `type` in the slot (None empties it), at its defaults and switched in.
  void setInsert(int scope, int slot, EffectType type);
  // Swaps a slot with its neighbour (delta ±1), settings and all.
  void moveInsert(int scope, int slot, int delta);

  // ---- resampling (HANDOFF §7.6, DECISIONS D-51 to D-53) ----
  // Takes are written to the project's recordings/ folder as 48 kHz float WAV
  // with a JSON sidecar (D-49), then sent where the setup says.
  RecSetup& recSetup() { return rec_; }
  const RecSetup& recSetup() const { return rec_; }
  enum class RecState { Idle, Armed, Recording, Review };
  RecState recState() const;
  // The take in flight (armed or recording), for progress and the live waveform.
  const Take* take() const { return take_.get(); }
  // Starts a take with the current setup. False (and why) if it can't.
  bool arm(std::string& error);
  void stopTake();    // armed: disarm; synced: stop on the next downbeat; else stop now
  void cancelTake();  // throw the take away

  // A take for drum pads waits here with its slice points until it's committed.
  struct SliceReview {
    std::filesystem::path file;  // the recording
    SampleLoader::Ptr sample;    // its audio
    std::vector<int64_t> slices; // start frames, ascending, the first 0
    double tempo = 0;
    int bars = 0;
    int track = 0, firstPad = 0;
    int maxSlices() const { return DrumEngine::kPads - firstPad; }
  };
  const SliceReview* review() const { return review_ ? &*review_ : nullptr; }
  void moveSlice(int i, int64_t delta);  // slice 0 stays at the start
  int addSlice(int64_t at);              // returns its index, or -1 if there's no room
  void removeSlice(int i);
  void playSlice(int i);
  // Writes the slices next to the recording and puts them on the pads, in a choke group.
  bool commitSlices(std::string& error);
  void keepRecording();  // end the review: the recording stays, no pads change

  // A take is undone like any other edit (D-54): the tracks it changed come
  // back; its recording stays in recordings/.

  // This project's recordings, newest first.
  struct RecordingEntry {
    std::filesystem::path path;
    RecordingInfo info;
    double seconds = 0;
  };
  std::vector<RecordingEntry> recordings(int max = 50) const;
  std::filesystem::path recordingsDir() const;

  // ---- undo and redo (DECISIONS D-54) ----
  // Every edit to the project is a step: values, engines, sounds, pads,
  // inserts, the CC map, the program list, what a take changed. CC moves and
  // program changes from the OXI are playing, not editing, and aren't steps.
  // Rec setup, selection and screens aren't part of it either.
  //
  // commitEdit() turns everything changed since it was last called into one
  // step. The UI calls it after every key or click; tick() calls it too, so
  // headless edits and takes are steps. Repeated presses on one value within
  // a second and a half are one step. Loading or starting a project clears the history.
  void commitEdit();
  bool canUndo() const { return !undo_.empty() || !docUnchanged() || values_ != undoValues_; }
  bool canRedo() const { return !redo_.empty(); }
  std::string undoLabel() const;  // what undo would undo
  std::string redoLabel() const;
  // Undoes the last step (committing any edit in progress first). Returns its
  // label, or "" if there was nothing to undo.
  std::string undo();
  std::string redo();
  int undoSteps() const { return static_cast<int>(undo_.size()); }

  // ---- JSON (exposed for tests and ion-render) ----
  std::string toJson() const;
  bool fromJson(const std::string& text, std::string& error);

  const Paths& paths() const { return paths_; }
  Config& config() { return config_; }
  const Config& config() const { return config_; }

 private:
  void send(const Command& c);
  void setValue(ParamRef ref, float value);  // values_ + SetParam command
  void defaultValues();
  void reset();
  void markDirty() { dirty_ = autosavedDirty_ = true; }
  void rememberLastProject() const;
  bool writeAutosave(std::string& error);
  void sendPrograms();
  void linkLibrary(TrackState& tr);  // finds the track's voice in the library
  void requestPad(int t, int pad);
  void sendSample(int t, int pad, const SampleLoader::Ptr& sample);
  void pin(const SampleLoader::Ptr& sample);
  void unpin(const Sample* sample);
  void handleLoads();
  nlohmann::json insertsJson(int scope) const;
  nlohmann::json trackJson(int t) const;  // as saved in project.json
  void trackFromJson(int t, const nlohmann::json& jt);
  void insertsFromJson(int scope, const nlohmann::json& arr);
  void mixerFromJson(int scope, const nlohmann::json& mixer);
  void setIfDifferent(ParamRef ref, float value);
  void finishTake(Take* take);
  nlohmann::json recJson() const;
  void recFromJson(const nlohmann::json& j);
  void sendToSampler(int t, const std::string& stored);
  std::string soundSlug(int t) const;

  Module& module_;
  Paths paths_;
  Config& config_;
  std::string name_;
  std::array<TrackState, kTrackCount> tracks_{};
  std::array<CcMapping, kMaxCcMappings> cc_{};
  std::array<std::array<EffectType, kInsertSlots>, kScopeCount> inserts_{};
  std::optional<ParamRef> learning_;
  std::unique_ptr<FmLibrary> library_;
  std::vector<ProgramEntry> programs_;
  std::string message_;

  // Samples. Every pointer sent to the audio thread is pinned here (one entry
  // per send) until the audio thread hands it back, so it's never freed in use.
  std::unique_ptr<SampleLoader> loader_;
  struct PadLoad {
    uint64_t tag = 0;         // the load in flight, or the last one
    bool loading = false;
    SampleLoader::Ptr sample; // what the pad plays
    std::string error;
  };
  std::array<std::array<PadLoad, DrumEngine::kPads>, kTrackCount> padLoads_{};
  std::vector<SampleLoader::Ptr> pinned_;
  uint64_t nextTag_ = 1;
  uint64_t previewTag_ = 0;
  bool previewPlay_ = false;
  std::filesystem::path previewPath_;
  std::string previewError_;
  std::deque<SampleLoader::Ptr> recentPreviews_;  // so going back up the list is instant

  // Library "last used": a patch counts once it has stayed on a track a while,
  // so scrolling past it doesn't.
  std::array<std::chrono::steady_clock::time_point, kTrackCount> loadedAt_{};
  std::array<int64_t, kTrackCount> usedMarked_{};

  // Parameter values as the user intends them. Edits update this at once (so
  // repeated key presses never read a value the audio thread hasn't applied
  // yet); CC moves made on the audio thread are pulled in by tick().
  using Values = std::array<float, ParamStore::kScopes * ParamStore::kPerScope>;
  Values values_{};
  Values lastStore_{};  // ParamStore as seen at the last tick
  Values saved_{};      // values at the last save/load
  Values autosaved_{};  // values at the last autosave

  // Resampling.
  RecSetup rec_;
  std::unique_ptr<Take> take_;  // in flight: the audio thread may be writing it
  bool takeAbandoned_ = false;  // …but it belongs to a project that's been closed
  std::optional<SliceReview> review_;

  // Undo and redo (D-54). The document outside parameter values, as plain
  // data: cheap to copy and compare on every key press.
  struct Doc {
    std::array<TrackState, kTrackCount> tracks{};
    std::array<std::array<EffectType, kInsertSlots>, kScopeCount> inserts{};
    std::array<CcMapping, kMaxCcMappings> cc{};
    std::vector<ProgramEntry> programs;
    bool operator==(const Doc&) const = default;
  };
  struct ValueChange {
    uint32_t index;  // into Values
    float before, after;
  };
  struct Step {
    std::string label;
    std::optional<Doc> before, after;  // only when more than values changed
    std::vector<ValueChange> values;
    std::chrono::steady_clock::time_point at;
  };
  static constexpr std::size_t kMaxSteps = 256;
  Doc doc() const;
  bool docUnchanged() const;  // the document is still undoDoc_
  void restoreDoc(const Doc& d);
  void applyStep(const Step& step, bool forward);
  void clearHistory();
  // Names the step being made ("Track 3 · patch BRASS 1"); the first name wins until commitEdit().
  void editLabel(std::string label);
  std::deque<Step> undo_, redo_;
  Doc undoDoc_;           // the document as of the last step
  Values undoValues_{};   // …and the values
  std::string editLabel_;

  bool dirty_ = false;          // non-parameter edits since the last save
  bool autosavedDirty_ = false; // …since the last autosave
  std::chrono::steady_clock::time_point lastAutosave_ = std::chrono::steady_clock::now();
};

std::unique_ptr<Engine> createEngine(EngineType type);

}  // namespace ion
