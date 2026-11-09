#pragma once

// One Plaits voice: every model, the internal decay envelope and the low-pass
// gate, as Émilie Gillet's plaits/dsp/voice.cc does it, adapted for ion
// (DECISIONS D-56). The upstream file is MIT-licensed:
//
//   Copyright 2016 Emilie Gillet. Permission is hereby granted, free of
//   charge, to any person obtaining a copy of this software and associated
//   documentation files (the "Software"), to deal in the Software without
//   restriction, including without limitation the rights to use, copy,
//   modify, merge, publish, distribute, sublicense, and/or sell copies of the
//   Software, and to permit persons to whom the Software is furnished to do
//   so, subject to the following conditions: The above copyright notice and
//   this permission notice shall be included in all copies or substantial
//   portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT
//   WARRANTY OF ANY KIND.
//
// What's different from upstream:
//  - float output (upstream converts to 16-bit for the codec);
//  - no 1 ms trigger delay: a MIDI note's pitch and gate arrive together;
//  - the trigger is a note-on, not a voltage, so a retrigger is never missed;
//  - the 6-op models' banks come from ion (the track's own list, or the
//    built-in banks), not from the module's flash;
//  - the low-pass gate has three modes (ion's gate / ping / off).

#include <cstddef>
#include <cstdint>

namespace ion::plaits_port {

class VoiceImpl;

class PlaitsVoice {
 public:
  static constexpr std::size_t kBlock = 12;       // Plaits renders 12 samples at a time
  static constexpr std::size_t kRam = 16 * 1024;  // shared scratch RAM for the models
  static constexpr int kModels = 24;

  enum class Lpg : uint8_t {
    Gate,  // follows the key: opens to the velocity while held, closes after (decay = how slowly)
    Ping,  // struck: every note pings it and it dies away by itself (Plaits with only TRIG patched)
    Off,   // bypassed: the model straight out
  };

  struct Settings {
    int model = 0;  // Plaits' own engine index, 0-23 (orange bank first)
    float harmonics = 0.5f, timbre = 0.5f, morph = 0.5f;
    float envPitch = 0, envTimbre = 0, envMorph = 0;  // −1…1: the internal decay envelope's amounts
    float decay = 0.5f, colour = 0.5f;                 // the LPG's (and the internal envelope's) decay; its colour
    Lpg lpg = Lpg::Gate;
  };

  // Allocates; construct off the audio thread.
  PlaitsVoice();
  ~PlaitsVoice();
  PlaitsVoice(const PlaitsVoice&) = delete;
  PlaitsVoice& operator=(const PlaitsVoice&) = delete;

  // The banks the three 6-op models play: 32 packed DX7 voices (4096 bytes)
  // each. Null keeps Plaits' built-in bank for that model. Takes effect when
  // that model is next selected, or at once if it's playing.
  void setBank(int sixOp, const uint8_t* bank);

  // A note starts: the internal envelope and the LPG (ping) fire on the next block.
  void trigger() { pendingTrigger_ = true; }
  void reset();

  // kBlock samples. `note` is a MIDI note number (fractional), `gate` whether
  // the key is held, `level` the velocity's level (0…1).
  void render(const Settings& s, float note, bool gate, float level, float* out, float* aux);

  int model() const { return model_; }

 private:
  VoiceImpl* impl_;
  char* ram_;
  bool pendingTrigger_ = false;
  int model_ = -1;
};

}  // namespace ion::plaits_port
