#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include "core/param_store.h"
#include "core/params.h"
#include "core/spsc_queue.h"
#include "engine/channel_eq.h"
#include "engine/clock.h"
#include "engine/modulation.h"
#include "engine/engine.h"
#include "engine/fx/effects.h"
#include "engine/midi_types.h"
#include "engine/sample.h"
#include "engine/take.h"
#include "library/syx.h"

namespace ion {

inline constexpr uint8_t kAnyChannel = 0xFF;
inline constexpr int kMaxCcMappings = 64;

// A CC mapped to a parameter. lo/hi are the normalized range the CC sweeps
// (hi < lo inverts).
struct PlaitsBank;

struct CcMapping {
  bool used = false;
  uint8_t channel = kAnyChannel;  // 0-15, or kAnyChannel
  uint8_t cc = 0;
  ParamRef target{};
  float lo = 0.0f;
  float hi = 1.0f;
  bool operator==(const CcMapping&) const = default;
};

// How live MIDI is timed (HANDOFF §5, DECISIONS D-19).
enum class MidiTiming : uint8_t {
  Tight,      // each event at its exact arrival offset, one buffer later: no jitter
  Immediate,  // at the start of the next block: lowest latency, up to one buffer of jitter
};

// The project's program-change list (HANDOFF §6.3): program N loads voice N
// onto the FM tracks listening on that channel. Built on the control thread,
// handed to the audio thread whole, and handed back to be freed.
inline constexpr int kMaxPrograms = 128;
struct ProgramTable {
  int count = 0;
  std::array<syx::Voice, kMaxPrograms> voices{};
};

// Control thread → audio thread. One producer (the UI thread, or main in headless).
struct Command {
  enum class Type : uint8_t {
    Midi,          // `midi`, dispatched like live input (piano mode, audition)
    Panic,
    LoadVoice,     // FM voice onto `track`
    SetParam,      // `param` = `value`
    SetEngine,     // `engine` onto `track` (may be null); the old one comes back as a Notification
    SetChannel,    // `track` listens on MIDI channel `value` (0-15)
    SetCcMapping,  // `mapping` into `slot`
    LearnCc,       // next incoming CC is reported for `param`
    CancelLearn,
    SetTiming,     // `value`: MidiTiming
    SetPrograms,   // `programs` (may be null: program change off); the old table comes back as a Notification
    SetSample,     // `sample` (may be null) onto pad `slot` of drum `track`; the old one comes back as SampleReleased
    Preview,       // play `sample` once at gain `value`, straight to the master (null stops); comes back as SampleReleased
    SetInsert,     // `effect` (may be null) into insert `slot` of `scope`; the old one comes back as EffectRetired
    Record,        // start `take` (armed until the next downbeat if it's synced); comes back as TakeDone
    StopRecord,    // armed: disarm; synced: stop on the next downbeat; otherwise stop now
    CancelRecord,  // drop the take (it comes back as TakeDone, Cancelled)
    SetPlaitsBank, // `plaitsBank` (may be null: Plaits' own) for the Plaits engine on `track`; the old one comes back as BankRetired
    SetModTarget,  // route `slot` of `track` now moves the value at index `value` in that track (−1: nothing; D-58)
  };
  Type type = Type::Panic;
  uint8_t track = 0;
  MidiMessage midi{};
  ParamRef param{};
  float value = 0;
  Engine* engine = nullptr;
  int slot = 0;
  CcMapping mapping{};
  syx::Voice voice{};
  ProgramTable* programs = nullptr;
  const Sample* sample = nullptr;
  uint8_t scope = 0;  // SetInsert: any scope, returns and master included
  Effect* effect = nullptr;
  Take* take = nullptr;
  PlaitsBank* plaitsBank = nullptr;
};

// Audio thread → control thread.
struct Notification {
  enum class Type : uint8_t {
    EngineRetired,    // delete `engine` (it's no longer used)
    CcLearned,        // `channel`/`cc` moved while learning `param`
    ProgramsRetired,  // delete `programs`
    ProgramChanged,   // program `program` was loaded onto `track`
    SampleReleased,   // the audio thread no longer uses `sample` (one per SetSample/Preview that sent it)
    EffectRetired,    // delete `effect`
    TakeDone,         // the audio thread has finished with `take` (Done or Cancelled)
    BankRetired,      // delete `plaitsBank`
  };
  Type type = Type::EngineRetired;
  Engine* engine = nullptr;
  uint8_t channel = 0;
  uint8_t cc = 0;
  ParamRef param{};
  ProgramTable* programs = nullptr;
  uint8_t track = 0;
  uint8_t program = 0;
  const Sample* sample = nullptr;
  Effect* effect = nullptr;
  Take* take = nullptr;
  PlaitsBank* plaitsBank = nullptr;
};

struct MeterStatus {
  std::atomic<float> peakLeft{0}, peakRight{0};  // decaying peak, linear
  std::atomic<float> rmsLeft{0}, rmsRight{0};    // ~300 ms RMS, linear
};

// Written by the audio thread, read by anyone.
struct ModuleStatus {
  std::atomic<float> dspLoad{0};      // smoothed fraction of the callback period
  std::atomic<float> dspLoadPeak{0};  // worst single callback since start
  std::atomic<uint32_t> callbackFrames{0};
  std::atomic<uint64_t> framesRendered{0};
  std::atomic<int> voices{0};
  std::atomic<uint32_t> midiReceived{0};
  std::atomic<uint32_t> lastMidi{0};  // status | data1 << 8 | data2 << 16 (excluding clock)
  std::atomic<uint32_t> droppedMidi{0};
  std::atomic<uint32_t> clippedBlocks{0};
  std::array<MeterStatus, kTrackCount> track{};
  MeterStatus master{};
  std::array<MeterStatus, 2> returns{};  // delay, reverb
  // Gain reduction in dB of each insert (compressors), and of the master limiter.
  std::array<std::array<std::atomic<float>, kInsertSlots>, kScopeCount> reduction{};
  std::atomic<float> limiter{0};
  std::array<std::atomic<uint32_t>, kTrackCount> trackNotes{};  // note-on counter, for activity lights
  std::array<std::atomic<int>, kTrackCount> trackVoices{};
  std::array<std::atomic<uint8_t>, kTrackCount> trackEngine{};  // EngineType
  std::array<std::array<std::atomic<uint32_t>, 16>, kTrackCount> padHits{};  // drum pad trigger counters
  // Modulation, for the UI: each route's target as modulation has it now
  // (0…1 of its range; −1 when the route isn't moving anything), and the
  // LFOs' and envelopes' outputs (LFOs −1…1, envelopes 0…1).
  std::array<std::array<std::atomic<float>, kModRoutes>, kTrackCount> modLive{};
  std::array<std::array<std::atomic<float>, kLfos + kModEnvs>, kTrackCount> modSources{};
  std::atomic<bool> previewing{false};
  std::atomic<float> bpm{0};
  std::atomic<bool> clockPresent{false};
  std::atomic<bool> running{false};
  std::atomic<int> bar{0};
  std::atomic<int> beat{0};
  std::atomic<bool> learning{false};
};

// The sound module: eight tracks, MIDI dispatch, mixer, master.
//
// Threads: exactly one MIDI producer (pushMidi), one control thread
// (pushCommand / popNotification) and the audio thread (process).
class Module {
 public:
  Module();
  ~Module();
  Module(const Module&) = delete;
  Module& operator=(const Module&) = delete;

  // ---- producers ----
  bool pushMidi(const MidiEvent& e);    // MIDI thread (or offline driver) only
  bool pushCommand(const Command& c);   // control thread only; false when full (retry)
  std::optional<Notification> popNotification();  // control thread only

  // ---- audio thread ----
  // Fills `frames` interleaved stereo frames. `cycleNs` is the CLOCK_MONOTONIC
  // time of this cycle's start (0 if unknown) and is used to place live MIDI.
  void process(float* interleaved, std::size_t frames, int64_t cycleNs = 0) noexcept;

  const ModuleStatus& status() const { return status_; }
  const ParamStore& params() const { return store_; }

 private:
  struct Pending {
    MidiMessage msg;
    int64_t frame;  // -1 = as soon as possible
  };

  void renderBlock() noexcept;
  void renderEngines(std::size_t from, std::size_t to) noexcept;
  void mix() noexcept;
  void dispatch(const MidiMessage& m, int64_t frame) noexcept;
  void handleCommand(const Command& c) noexcept;
  void applyParam(ParamRef ref, float value) noexcept;
  // Modulation (D-58): puts track t's routes onto their targets, over the values set.
  void modulate(int t) noexcept;
  // Sets the value the DSP uses without touching the value set (the ParamStore's).
  void applyEffective(ParamRef ref, float value) noexcept;
  const ParamInfo* info(ParamRef ref) const noexcept;
  void notify(const Notification& n) noexcept;
  void releaseSample(const Sample* s) noexcept;
  void renderPreview() noexcept;
  // Runs scope's insert chain over l/r (kBlockSize), with bypass crossfades.
  void runInserts(int scope, float* l, float* r) noexcept;
  // EQ, then fader and pan into the master sum, for a return or a track.
  void strip(int scope, float* l, float* r, float gainScale, float* outL, float* outR) noexcept;
  // Recording (HANDOFF §7.6, DECISIONS D-51).
  void startTake(Take* t) noexcept;
  void takeTick(int64_t frame, bool downbeat) noexcept;
  void takeClockStopped() noexcept;
  void captureTake(const float* l, const float* r, int64_t delay) noexcept;
  void endTake(Take::State state) noexcept;
  bool capturing() const noexcept { return take_ && take_->state.load(std::memory_order_relaxed) == Take::State::Recording; }

  // ---- audio-thread state ----
  std::array<Engine*, kTrackCount> engines_{};
  std::array<int, kTrackCount> channels_{};
  std::array<std::array<float, ParamStore::kPerScope>, ParamStore::kScopes> values_{};
  std::array<CcMapping, kMaxCcMappings> ccMap_{};
  std::optional<ParamRef> learn_;
  MidiTiming timing_ = MidiTiming::Tight;
  ProgramTable* programs_ = nullptr;
  std::array<TrackModulation, kTrackCount> mod_{};
  std::array<std::array<int16_t, kModRoutes>, kTrackCount> modTarget_{};  // −1: none
  std::array<std::array<int16_t, kModRoutes>, kTrackCount> modApplied_{};  // targets moved last block (to put back)

  // The sample browser's preview: plays once into the master. A new preview
  // fades the previous one out over 5 ms instead of cutting it.
  struct PreviewVoice {
    const Sample* sample = nullptr;
    int64_t pos = 0;
    float gain = 0;
    float fade = 1;      // 1 = full; falls to 0 when fading
    bool fading = false;
  };
  std::array<PreviewVoice, 2> preview_{};

  std::array<ChannelEq, kScopeCount> eq_;
  struct StripState {
    float gainLeft = 0, gainRight = 0;  // current, ramped per block
    float peakLeft = 0, peakRight = 0, msLeft = 0, msRight = 0;
    float sendA = 0, sendB = 0;         // current send gains, ramped
  };
  std::array<StripState, kScopeCount> strips_{};  // master_ is strips_[kMasterScope]
  StripState& master_ = strips_[kMasterScope];

  // Inserts, per scope. The returns' slot 0 is their own effect (the delay,
  // the reverb), made with the module and never swapped.
  std::array<std::array<Effect*, kInsertSlots>, kScopeCount> fx_{};
  std::array<std::array<float, kInsertSlots>, kScopeCount> fxMix_{};  // bypass crossfade, 1 = in
  Limiter limiter_;
  // Blocks each scope's EQ and inserts have had silence in and (near) silence
  // out: past a few, they're skipped until sound arrives (an idle reverb costs nothing).
  std::array<int, kScopeCount> quiet_{};
  bool asleep(int scope, const float* l, const float* r) noexcept;
  void settle(int scope, const float* l, const float* r) noexcept;
  float busLeft_[2][kBlockSize]{}, busRight_[2][kBlockSize]{};
  float scratchLeft_[kBlockSize]{}, scratchRight_[kBlockSize]{};
  ClockFollower clock_;

  Take* take_ = nullptr;
  int64_t takeStart_ = 0;  // module frame of the take's first sample
  int64_t takeEnd_ = -1;   // capture stops here (exclusive) once known
  int takeTicks_ = 0;      // clock ticks since the start tick
  float recLeft_[kBlockSize]{}, recRight_[kBlockSize]{};  // the tracks being recorded, summed
  std::array<float, kTrackCount> recGainLeft_{}, recGainRight_{};  // post-fx tap: fader and pan, ramped

  float trackLeft_[kTrackCount][kBlockSize]{};
  float trackRight_[kTrackCount][kBlockSize]{};
  float left_[kBlockSize]{};
  float right_[kBlockSize]{};

  std::array<Pending, 2048> pending_{};
  std::size_t pendingHead_ = 0, pendingCount_ = 0;

  std::size_t blockPos_ = kBlockSize;  // kBlockSize = "need a new block"
  int64_t frames_ = 0;                 // first frame of the next block to render
  int64_t outFrame_ = 0;               // frames handed to the device so far
  float load_ = 0;

  // ---- shared ----
  SpscQueue<MidiEvent, 1024> midiQueue_;
  SpscQueue<Command, 2048> commandQueue_;  // a project load sends a few hundred
  SpscQueue<Notification, 256> notifications_;
  ModuleStatus status_;
  ParamStore store_;
};

}  // namespace ion
