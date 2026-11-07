#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "engine/module.h"
#include "library/syx.h"
#include "project/config.h"
#include "project/paths.h"

namespace ion {

inline constexpr int kProjectFormat = 1;

struct TrackState {
  EngineType engine = EngineType::None;
  int channel = 0;  // 0-15
  // FM
  bool hasVoice = false;
  syx::Voice voice{};
  std::string voiceSource;  // bank it came from, e.g. "rom1a"
  int bankIndex = -1;       // position in Session::bank(), when browsing
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
  void setVoice(int t, const syx::Voice& voice, const std::string& source, int bankIndex = -1);
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

  // ---- the bank loaded with --syx (Phase 2 replaces this with the library) ----
  void setBank(std::vector<syx::Voice> voices, std::string name);
  const std::vector<syx::Voice>& bank() const { return bank_; }
  const std::string& bankName() const { return bankName_; }
  void stepBank(int t, int step);  // load the next/previous bank voice onto FM track t

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

  Module& module_;
  Paths paths_;
  Config& config_;
  std::string name_;
  std::array<TrackState, kTrackCount> tracks_{};
  std::array<CcMapping, kMaxCcMappings> cc_{};
  std::optional<ParamRef> learning_;
  std::vector<syx::Voice> bank_;
  std::string bankName_;
  std::string message_;

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
