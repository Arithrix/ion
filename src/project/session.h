#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/fm/dx7_voice.h"
#include "engine/module.h"
#include "library/fm_library.h"
#include "library/syx.h"
#include "project/config.h"
#include "project/paths.h"

namespace ion {

// 1: Phase 1. 2: FM macros, the program list, library ids (v0.2.0).
inline constexpr int kProjectFormat = 2;

struct TrackState {
  EngineType engine = EngineType::None;
  int channel = 0;  // 0-15
  // FM. The voice is kept in the project too, so a project plays the same
  // even on a machine whose library doesn't have it.
  bool hasVoice = false;
  syx::Voice voice{};       // as stored, without macros
  std::string voiceSource;  // bank it came from, e.g. "rom1a", or "saved"
  int64_t libraryId = 0;    // in this machine's library, or 0
};

// One entry in the project's program-change list.
struct ProgramEntry {
  syx::Voice voice{};
  std::string source;
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

  // ---- JSON (exposed for tests and ion-render) ----
  std::string toJson() const;
  bool fromJson(const std::string& text, std::string& error);

  const Paths& paths() const { return paths_; }
  Config& config() { return config_; }

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

  Module& module_;
  Paths paths_;
  Config& config_;
  std::string name_;
  std::array<TrackState, kTrackCount> tracks_{};
  std::array<CcMapping, kMaxCcMappings> cc_{};
  std::optional<ParamRef> learning_;
  std::unique_ptr<FmLibrary> library_;
  std::vector<ProgramEntry> programs_;
  std::string message_;
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

  bool dirty_ = false;          // non-parameter edits since the last save
  bool autosavedDirty_ = false; // …since the last autosave
  std::chrono::steady_clock::time_point lastAutosave_ = std::chrono::steady_clock::now();
};

std::unique_ptr<Engine> createEngine(EngineType type);

}  // namespace ion
