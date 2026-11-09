#include "engine/plaits/plaits_engine.h"

#include <algorithm>
#include <cmath>

#include "engine/envelope_params.h"
#include "engine/voice_alloc.h"
#include "plaits/resources.h"

namespace ion {

namespace {

using plaits_port::PlaitsVoice;
constexpr std::size_t kBlock = PlaitsVoice::kBlock;
constexpr float kVoiceGain = 0.5f;   // −6 dB a voice: six chords' worth of headroom before the limiter
constexpr float kSilence = 1e-4f;    // −80 dB
constexpr int kQuietFrames = 2400;   // 50 ms of that after release frees the voice
constexpr int kCutFrames = 240;      // 5 ms: a note-off with nothing else to end it

// The grid's order (the mockup's): green, red, orange.
struct Model {
  const char* name;
  int plaits;  // Plaits' engine index
  const char* about;
  const char* harmonics;
  const char* timbre;
  const char* morph;
};
// Roles paraphrase the Plaits manual (firmware 1.2).
const Model kModelTable[PlaitsEngine::kModels] = {
    // green: the original pitched models
    {"Virtual analog", 8, "two classic waveforms, detuned or synced", "detune", "pulse width · sync", "saw ↔ triangle"},
    {"Waveshaper", 9, "a triangle through a waveshaper and folder", "waveshaper", "fold", "asymmetry"},
    {"2-op FM", 10, "two sine operators", "ratio", "modulation", "feedback"},
    {"Formant", 11, "granular formant oscillator", "formant ratio", "formant", "width · shape"},
    {"Harmonic", 12, "additive: 24 harmonics", "spread", "brightest harmonic", "peak shape"},
    {"Wavetable", 13, "4 banks of 8×8 waves", "bank", "row", "column"},
    {"Chords", 14, "four-note chords, string machine or organ", "chord", "inversion", "waveform"},
    {"Speech", 15, "vowels, then SAM, then words", "voice type", "species", "phoneme · word"},
    // red: noise and physical models
    {"Swarm", 16, "eight sawtooth voices in a cloud", "pitch spread", "density", "grain length"},
    {"Filtered noise", 17, "clocked noise through a filter", "LP ↔ BP ↔ HP", "clock rate", "resonance"},
    {"Particles", 18, "dust through resonant filters", "spread", "density", "filter type"},
    {"String", 19, "inharmonic string, plucked", "material", "brightness", "decay"},
    {"Modal", 20, "modal resonator, struck", "material", "brightness", "decay"},
    {"Kick", 21, "analog bass drum", "punch · drive", "brightness", "decay"},
    {"Snare", 22, "analog snare drum", "tone ↔ noise", "modes", "decay"},
    {"Hi-hat", 23, "analog hi-hat", "metal ↔ noise", "high-pass", "decay"},
    // orange: firmware 1.2
    {"VA + filter", 0, "classic waveshapes into a resonant filter", "resonance · slope", "cutoff", "waveform · sub"},
    {"Phase dist.", 1, "phase distortion", "ratio", "amount", "asymmetry"},
    {"6-op FM a", 2, "six-op FM: your patch list (else Plaits' bank 1)", "patch", "modulator level", "envelope stretch"},
    {"6-op FM b", 3, "six-op FM: Plaits' bank 2", "patch", "modulator level", "envelope stretch"},
    {"6-op FM c", 4, "six-op FM: Plaits' bank 3", "patch", "modulator level", "envelope stretch"},
    {"Wave terrain", 5, "a path across a 2-D wave terrain", "terrain", "path size", "path offset"},
    {"String machine", 6, "ensemble strings with a filter and chorus", "chord", "chorus · filter", "waveform"},
    {"Chiptune", 7, "square voices, chords and arpeggios", "chord", "arpeggio · inversion", "pulse width · sync"},
};

const char* const kModelNames[PlaitsEngine::kModels] = {
    kModelTable[0].name,  kModelTable[1].name,  kModelTable[2].name,  kModelTable[3].name,  kModelTable[4].name,
    kModelTable[5].name,  kModelTable[6].name,  kModelTable[7].name,  kModelTable[8].name,  kModelTable[9].name,
    kModelTable[10].name, kModelTable[11].name, kModelTable[12].name, kModelTable[13].name, kModelTable[14].name,
    kModelTable[15].name, kModelTable[16].name, kModelTable[17].name, kModelTable[18].name, kModelTable[19].name,
    kModelTable[20].name, kModelTable[21].name, kModelTable[22].name, kModelTable[23].name};
const char* const kLpgModes[] = {"gate", "ping", "off"};

constexpr float kFine = 1.0f / 60.0f, kCoarse = 1.0f / 12.0f;

// The amp envelope is wide open by default (D-50): the LPG shapes the note.
constexpr EnvDefaults kAmpDefaults{0.1f, kEnvFull, 1000, 1.0f, kEnvOff};

const ParamInfo kParams[PlaitsEngine::kParamCount] = {
    {"model", "Model", 0, PlaitsEngine::kModels - 1, 0, Curve::Choice, Unit::None, 1, 8, kModelNames},
    {"harmonics", "Harmonics", 0, 1, 0.5f, Curve::Linear, Unit::Percent, 0.01f, 0.1f},
    {"timbre", "Timbre", 0, 1, 0.5f, Curve::Linear, Unit::Percent, 0.01f, 0.1f},
    {"morph", "Morph", 0, 1, 0.5f, Curve::Linear, Unit::Percent, 0.01f, 0.1f},
    {"env.pitch", "Env→pitch", -1, 1, 0, Curve::Linear, Unit::Percent, 0.02f, 0.1f},
    {"env.timbre", "Env→timbre", -1, 1, 0, Curve::Linear, Unit::Percent, 0.02f, 0.1f},
    {"env.morph", "Env→morph", -1, 1, 0, Curve::Linear, Unit::Percent, 0.02f, 0.1f},
    {"lpg.mode", "LPG", 0, 2, 0, Curve::Choice, Unit::None, 1, 1, kLpgModes},
    {"lpg.decay", "LPG decay", 0, 1, 0.5f, Curve::Linear, Unit::Percent, 0.01f, 0.1f},
    {"lpg.colour", "LPG colour", 0, 1, 0.5f, Curve::Linear, Unit::Percent, 0.01f, 0.1f},
    {"out.aux", "Out ↔ aux", 0, 1, 0, Curve::Linear, Unit::Percent, 0.02f, 0.1f},
    {"vel.lpg", "Vel→LPG", 0, 1, 0.7f, Curve::Linear, Unit::Percent, 0.05f, 0.25f},
    {"poly", "Voices", 1, PlaitsEngine::kMaxVoices, 6, Curve::Choice, Unit::None, 1, 1},
    {"glide", "Glide", 0.1f, 2000, 0.1f, Curve::Log, Unit::Ms, kFine, kCoarse, nullptr, nullptr, "off"},
    {"bend", "Bend range", 0, 24, 2, Curve::Choice, Unit::Semitones, 1, 12},
    envelopeParam(false, kEnvAttack, kAmpDefaults),
    envelopeParam(false, kEnvHold, kAmpDefaults),
    envelopeParam(false, kEnvDecay, kAmpDefaults),
    envelopeParam(false, kEnvSustain, kAmpDefaults),
    envelopeParam(false, kEnvRelease, kAmpDefaults),
    velAmpParam(0),
};

}  // namespace

std::span<const ParamInfo> PlaitsEngine::paramTable() { return kParams; }

// As Plaits' quantizers pick them (without their hysteresis).
int PlaitsEngine::sixOpSlot(float harmonics) { return std::clamp(static_cast<int>(harmonics * 1.02f * 32.0f), 0, 31); }

std::string PlaitsEngine::chordName(float harmonics) {
  static const char* names[] = {"octave", "fifth", "sus4", "m", "m7", "m9", "m11", "6/9", "maj9", "maj7", "major"};
  return names[std::clamp(static_cast<int>(harmonics * 1.02f * 11.0f), 0, 10)];
}

std::string PlaitsEngine::builtInPatchName(int bank, int slot) {
  const uint8_t* p = plaits::fm_patches_table[std::clamp(bank, 0, 2)] + std::clamp(slot, 0, 31) * 128 + 118;
  std::string name(reinterpret_cast<const char*>(p), 10);
  for (char& c : name)
    if (c < 32 || c > 126) c = ' ';
  while (!name.empty() && name.back() == ' ') name.pop_back();
  return name;
}

int PlaitsEngine::plaitsIndex(int model) { return kModelTable[std::clamp(model, 0, kModels - 1)].plaits; }
const char* PlaitsEngine::modelName(int model) { return kModelTable[std::clamp(model, 0, kModels - 1)].name; }
const char* PlaitsEngine::modelAbout(int model) { return kModelTable[std::clamp(model, 0, kModels - 1)].about; }
const char* PlaitsEngine::macroRole(int model, int macro) {
  const Model& m = kModelTable[std::clamp(model, 0, kModels - 1)];
  return macro == 0 ? m.harmonics : macro == 1 ? m.timbre : m.morph;
}

PlaitsEngine::PlaitsEngine() {
  for (Voice& v : voices_) v.plaits = std::make_unique<PlaitsVoice>();
  for (int i = 0; i < kParamCount; ++i) values_[static_cast<std::size_t>(i)] = kParams[i].def;
  updateSettings();
}

PlaitsEngine::~PlaitsEngine() = default;

void PlaitsEngine::updateSettings() {
  auto v = [&](int p) { return values_[static_cast<std::size_t>(p)]; };
  settings_.model = plaitsIndex(static_cast<int>(std::lround(v(kModel))));
  settings_.harmonics = v(kHarmonics);
  settings_.timbre = v(kTimbre);
  settings_.morph = v(kMorph);
  settings_.envPitch = v(kEnvPitch);
  settings_.envTimbre = v(kEnvTimbre);
  settings_.envMorph = v(kEnvMorph);
  settings_.decay = v(kLpgDecay);
  settings_.colour = v(kLpgColour);
  settings_.lpg = static_cast<PlaitsVoice::Lpg>(std::clamp(static_cast<int>(std::lround(v(kLpgMode))), 0, 2));
  ampCoeffs_ = envelopeCoeffs(&values_[static_cast<std::size_t>(kAmpAttack)]);
}

void PlaitsEngine::setParam(int index, float value) {
  if (index < 0 || index >= kParamCount) return;
  values_[static_cast<std::size_t>(index)] = value;
  if (index == kPoly) {
    // Fewer voices: the ones past the new count stop (quickly, not with a click).
    const int n = std::clamp(static_cast<int>(std::lround(value)), 1, kMaxVoices);
    for (int i = n; i < kMaxVoices; ++i) {
      Voice& vc = voices_[static_cast<std::size_t>(i)];
      if (vc.on && vc.cut < 0) {
        vc.gate = false;
        vc.cut = kCutFrames;
      }
    }
  }
  updateSettings();
}

PlaitsBank* PlaitsEngine::setBank(PlaitsBank* bank) {
  PlaitsBank* old = bank_.release();
  bank_.reset(bank);
  for (Voice& v : voices_) v.plaits->setBank(0, bank ? bank->data.data() : nullptr);
  return old;
}

int PlaitsEngine::voiceCount() const {
  int n = 0;
  for (const Voice& v : voices_) n += v.on;
  return n;
}

void PlaitsEngine::start(Voice& v, uint8_t note, uint8_t velocity) {
  const float vel = static_cast<float>(velocity) / 127.0f;
  const float a = values_[kVelLpg];
  v.note = note;
  v.gate = true;
  v.level = 1.0f - a + a * vel;
  v.velGain = velocityGain(values_[kVelAmp], vel);
  // Glide from the last note played on the track (0.1 ms is "off").
  const float glideMs = values_[kGlide];
  if (glideMs > 0.1f && lastPitch_ >= 0) {
    v.glideFrom = lastPitch_;
    v.glideLeft = static_cast<int>(glideMs * kSampleRate / 1000.0f);
  } else {
    v.glideLeft = 0;
  }
  v.pitch = note;
  lastPitch_ = note;
  if (!v.on) {
    v.plaits->reset();
    v.pos = kBlock;
  }
  v.on = true;
  v.quiet = 0;
  v.cut = -1;
  v.age = ++age_;
  v.amp.start();
  v.plaits->trigger();
}

void PlaitsEngine::noteOn(uint8_t note, uint8_t velocity) {
  if (velocity == 0) {
    noteOff(note);
    return;
  }
  const int n = std::clamp(static_cast<int>(std::lround(values_[kPoly])), 1, kMaxVoices);
  // The same note again restrikes its voice.
  for (int i = 0; i < n; ++i) {
    Voice& v = voices_[static_cast<std::size_t>(i)];
    if (v.on && v.note == note) {
      start(v, note, velocity);
      return;
    }
  }
  std::span<const Voice> pool(voices_.data(), static_cast<std::size_t>(n));
  start(voices_[static_cast<std::size_t>(pickVoice(pool))], note, velocity);
}

void PlaitsEngine::noteOff(uint8_t note) {
  for (Voice& v : voices_)
    if (v.on && v.gate && v.note == note) {
      v.gate = false;
      v.amp.release();
    }
}

void PlaitsEngine::allNotesOff() {
  for (Voice& v : voices_) {
    v.on = v.gate = false;
    v.amp.reset();
    v.plaits->reset();
  }
}

void PlaitsEngine::render(float* left, float* right, std::size_t frames) {
  const float aux = values_[kAux];
  const float bendSemis = bend_ * values_[kBendRange];
  const bool lpgOff = settings_.lpg == PlaitsVoice::Lpg::Off;
  for (Voice& v : voices_) {
    if (!v.on) continue;
    for (std::size_t i = 0; i < frames; ++i) {
      if (v.pos == kBlock) {
        float pitch = v.pitch;
        if (v.glideLeft > 0) {
          // Linear in semitones over the glide time, a block at a time.
          const float total = values_[kGlide] * kSampleRate / 1000.0f;
          pitch = v.pitch + (v.glideFrom - v.pitch) * static_cast<float>(v.glideLeft) / std::max(1.0f, total);
          v.glideLeft = std::max(0, v.glideLeft - static_cast<int>(kBlock));
        }
        v.plaits->render(settings_, pitch + bendSemis, v.gate, v.level, v.out.data(), v.aux.data());
        v.pos = 0;
      }
      float s = (v.out[v.pos] + (v.aux[v.pos] - v.out[v.pos]) * aux) * kVoiceGain * v.velGain;
      ++v.pos;
      // After the LPG, the uniform envelope. With the LPG off and the
      // release off, nothing would end the note: a note-off fades it in 5 ms.
      float env = v.amp.next(ampCoeffs_);
      if (!v.gate && v.cut < 0 && lpgOff && ampCoeffs_.release >= 1.0f) v.cut = kCutFrames;
      if (v.cut >= 0) {
        env *= static_cast<float>(v.cut) / kCutFrames;
        if (v.cut-- == 0) {
          v.on = false;
          break;
        }
      }
      s *= env;
      left[i] += s;
      right[i] += s;
      // Free the voice once it has been quiet a while after its key came up.
      if (!v.gate) {
        if (v.amp.idle() || std::fabs(s) < kSilence) ++v.quiet;
        else v.quiet = 0;
        if (v.amp.idle() || v.quiet >= kQuietFrames) {
          v.on = false;
          break;
        }
      }
    }
  }
}

}  // namespace ion
