// ion's Plaits voice; see plaits_voice.h for what's changed from
// plaits/dsp/voice.cc (MIT, Émilie Gillet) and why.

#include "engine/plaits/plaits_voice.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

#include "plaits/dsp/engine/additive_engine.h"
#include "plaits/dsp/engine/bass_drum_engine.h"
#include "plaits/dsp/engine/chord_engine.h"
#include "plaits/dsp/engine/engine.h"
#include "plaits/dsp/engine/fm_engine.h"
#include "plaits/dsp/engine/grain_engine.h"
#include "plaits/dsp/engine/hi_hat_engine.h"
#include "plaits/dsp/engine/modal_engine.h"
#include "plaits/dsp/engine/noise_engine.h"
#include "plaits/dsp/engine/particle_engine.h"
#include "plaits/dsp/engine/snare_drum_engine.h"
#include "plaits/dsp/engine/speech_engine.h"
#include "plaits/dsp/engine/string_engine.h"
#include "plaits/dsp/engine/swarm_engine.h"
#include "plaits/dsp/engine/virtual_analog_engine.h"
#include "plaits/dsp/engine/waveshaping_engine.h"
#include "plaits/dsp/engine/wavetable_engine.h"
#include "plaits/dsp/engine2/chiptune_engine.h"
#include "plaits/dsp/engine2/phase_distortion_engine.h"
#include "plaits/dsp/engine2/six_op_engine.h"
#include "plaits/dsp/engine2/string_machine_engine.h"
#include "plaits/dsp/engine2/virtual_analog_vcf_engine.h"
#include "plaits/dsp/engine2/wave_terrain_engine.h"
#include "plaits/dsp/envelope.h"
#include "plaits/dsp/fx/low_pass_gate.h"
#include "plaits/resources.h"
#include "stmlib/dsp/limiter.h"
#include "stmlib/utils/buffer_allocator.h"

namespace ion::plaits_port {

using namespace plaits;

static_assert(PlaitsVoice::kBlock == plaits::kBlockSize);

class VoiceImpl {
 public:
  void init(char* ram) {
    ram_ = ram;
    allocator_.Init(ram, PlaitsVoice::kRam);
    registry_.Init();
    // Plaits' order and settings (already enveloped, out gain, aux gain):
    // the orange bank (firmware 1.2), then green, then red.
    registry_.RegisterInstance(&vaVcf_, false, 1.0f, 1.0f);
    registry_.RegisterInstance(&phaseDistortion_, false, 0.7f, 0.7f);
    registry_.RegisterInstance(&sixOp_, true, 1.0f, 1.0f);
    registry_.RegisterInstance(&sixOp_, true, 1.0f, 1.0f);
    registry_.RegisterInstance(&sixOp_, true, 1.0f, 1.0f);
    registry_.RegisterInstance(&waveTerrain_, false, 0.7f, 0.7f);
    registry_.RegisterInstance(&stringMachine_, false, 0.8f, 0.8f);
    registry_.RegisterInstance(&chiptune_, false, 0.5f, 0.5f);
    registry_.RegisterInstance(&virtualAnalog_, false, 0.8f, 0.8f);
    registry_.RegisterInstance(&waveshaping_, false, 0.7f, 0.6f);
    registry_.RegisterInstance(&fm_, false, 0.6f, 0.6f);
    registry_.RegisterInstance(&grain_, false, 0.7f, 0.6f);
    registry_.RegisterInstance(&additive_, false, 0.8f, 0.8f);
    registry_.RegisterInstance(&wavetable_, false, 0.6f, 0.6f);
    registry_.RegisterInstance(&chord_, false, 0.8f, 0.8f);
    registry_.RegisterInstance(&speech_, false, -0.7f, 0.8f);
    registry_.RegisterInstance(&swarm_, false, -3.0f, 1.0f);
    registry_.RegisterInstance(&noise_, false, -1.0f, -1.0f);
    registry_.RegisterInstance(&particle_, false, -2.0f, 1.0f);
    registry_.RegisterInstance(&string_, true, -1.0f, 0.8f);
    registry_.RegisterInstance(&modal_, true, -1.0f, 0.8f);
    registry_.RegisterInstance(&bassDrum_, true, 0.8f, 0.8f);
    registry_.RegisterInstance(&snareDrum_, true, 0.8f, 0.8f);
    registry_.RegisterInstance(&hiHat_, true, 0.8f, 0.8f);
    for (int i = 0; i < registry_.size(); ++i) {
      allocator_.Free();  // the models share the RAM: only one runs at a time
      registry_.get(i)->Init(&allocator_);
    }
    reset();
  }

  void reset() {
    previous_ = -1;
    reload_ = false;
    outLimiter_.Init();
    auxLimiter_.Init();
    outLpg_.Init();
    auxLpg_.Init();
    decayEnvelope_.Init();
    lpgEnvelope_.Init();
    gate_ = false;
    previousNote_ = 0;
    accent_ = 0.8f;
  }

  void setBank(int sixOp, const uint8_t* bank) {
    banks_[sixOp] = bank;
    if (previous_ == 2 + sixOp) reload_ = true;
  }

  void render(const PlaitsVoice::Settings& s, bool triggered, float note, bool gate, float level, float* outFloat, float* auxFloat) {
    const int index = std::clamp(s.model, 0, registry_.size() - 1);
    Engine* e = registry_.get(index);
    if (index != previous_ || reload_) {
      // The models share this RAM, and some keep state in it between blocks
      // with a Reset() that doesn't clear it (the 6-op models' accumulator):
      // a new model started on the last one's leftovers, sometimes NaN
      // (found by the 0.10.1 deep sweep). Clear it; Reset() and
      // LoadUserData() then set up what each model needs, as upstream intends.
      if (index != previous_) std::memset(ram_, 0, PlaitsVoice::kRam);
      const uint8_t* data = nullptr;
      if (index >= 2 && index <= 4) data = banks_[index - 2] ? banks_[index - 2] : fm_patches_table[index - 2];
      e->LoadUserData(data);
      e->Reset();
      outLimiter_.Init();
      previous_ = index;
      reload_ = false;
    }

    const bool rising = triggered;
    if (rising) {
      if (s.lpg == PlaitsVoice::Lpg::Ping) lpgEnvelope_.Trigger();
      decayEnvelope_.Trigger();
      // The velocity sets the accent for the whole note (the drums and
      // strings strike harder; upstream reads the LEVEL input).
      const float compressed = std::clamp(1.3f * level / (0.3f + std::fabs(level)), 0.0f, 1.0f);
      accent_ = compressed;
    }
    gate_ = gate;

    EngineParameters p;
    p.trigger = (rising ? TRIGGER_RISING_EDGE : TRIGGER_LOW) | (gate_ ? TRIGGER_HIGH : TRIGGER_LOW);
    const float smoothedNote = (note + previousNote_) * 0.5f;
    previousNote_ = note;

    const float shortDecay = (200.0f * kBlockSize) / kSampleRate * stmlib::SemitonesToRatio(-96.0f * s.decay);
    decayEnvelope_.Process(shortDecay * 2.0f);
    p.accent = accent_;

    p.harmonics = std::clamp(s.harmonics, 0.0f, 1.0f);
    float envAmp = 1.0f, envAmpTimbre = 1.0f;
    if (index == 15) {
      envAmp = std::clamp(2.0f - p.harmonics * 6.0f, 0.0f, 1.0f);
      speech_.set_prosody_amount(s.envPitch);
      speech_.set_speed(s.envMorph);
    } else if (index == 7) {
      // The chiptune model's own envelope stands in for the one on TIMBRE.
      envAmpTimbre = 0.0f;
      chiptune_.set_envelope_shape(s.envTimbre);
    }
    const float env = decayEnvelope_.value();
    p.note = modulate(smoothedNote, s.envPitch, envAmp * env * env * 48.0f, -119.0f, 120.0f);
    p.timbre = modulate(s.timbre, s.envTimbre, envAmpTimbre * env, 0.0f, 1.0f);
    p.morph = modulate(s.morph, s.envMorph, envAmp * env, 0.0f, 1.0f);

    const PostProcessingSettings& pp = e->post_processing_settings;
    bool alreadyEnveloped = pp.already_enveloped;
    e->Render(p, out_, aux_, kBlockSize, &alreadyEnveloped);

    const bool bypass = alreadyEnveloped || s.lpg == PlaitsVoice::Lpg::Off;
    if (!bypass) {
      const float hf = s.colour;
      const float decayTail = (20.0f * kBlockSize) / kSampleRate * stmlib::SemitonesToRatio(-72.0f * s.decay + 12.0f * hf) - shortDecay;
      if (s.lpg == PlaitsVoice::Lpg::Gate) {
        const float compressed = gate_ ? std::clamp(1.3f * level / (0.3f + std::fabs(level)), 0.0f, 1.0f) : 0.0f;
        lpgEnvelope_.ProcessLP(compressed, shortDecay, decayTail, hf);
      } else {
        const float attack = NoteToFrequency(p.note) * static_cast<float>(kBlockSize) * 2.0f;
        lpgEnvelope_.ProcessPing(attack, shortDecay, decayTail, hf);
      }
    } else {
      lpgEnvelope_.Init();
    }
    post(pp.out_gain, bypass, outLimiter_, outLpg_, out_, outFloat);
    post(pp.aux_gain, bypass, auxLimiter_, auxLpg_, aux_, auxFloat);
  }

 private:
  // Upstream's ApplyModulations with the internal envelope as the source.
  static float modulate(float base, float amount, float envelope, float lo, float hi) {
    amount *= std::max(std::fabs(amount) - 0.05f, 0.05f);
    amount *= 1.05f;
    return std::clamp(base + amount * envelope, lo, hi);
  }

  void post(float gain, bool bypass, stmlib::Limiter& limiter, LowPassGate& lpg, float* in, float* out) {
    if (gain < 0.0f) limiter.Process(-gain, in, kBlockSize);
    const float g = gain < 0.0f ? 1.0f : gain;
    if (!bypass) {
      lpg.Process(g * lpgEnvelope_.gain(), lpgEnvelope_.frequency(), lpgEnvelope_.hf_bleed(), in, kBlockSize);
      for (std::size_t i = 0; i < kBlockSize; ++i) out[i] = std::clamp(in[i], -1.0f, 1.0f);
    } else {
      for (std::size_t i = 0; i < kBlockSize; ++i) out[i] = std::clamp(in[i] * g, -1.0f, 1.0f);
    }
    // The clamp is the module's own: its 16-bit output (Clip16) clips at full
    // scale. A few models go far past it at extreme settings (Chords with the
    // envelope dragging pitch four octaves down).
  }

  VirtualAnalogEngine virtualAnalog_;
  WaveshapingEngine waveshaping_;
  FMEngine fm_;
  GrainEngine grain_;
  AdditiveEngine additive_;
  WavetableEngine wavetable_;
  ChordEngine chord_;
  SpeechEngine speech_;
  SwarmEngine swarm_;
  NoiseEngine noise_;
  ParticleEngine particle_;
  StringEngine string_;
  ModalEngine modal_;
  BassDrumEngine bassDrum_;
  SnareDrumEngine snareDrum_;
  HiHatEngine hiHat_;
  VirtualAnalogVCFEngine vaVcf_;
  PhaseDistortionEngine phaseDistortion_;
  SixOpEngine sixOp_;
  WaveTerrainEngine waveTerrain_;
  StringMachineEngine stringMachine_;
  ChiptuneEngine chiptune_;

  EngineRegistry<PlaitsVoice::kModels> registry_;
  stmlib::BufferAllocator allocator_;
  char* ram_ = nullptr;
  const uint8_t* banks_[3] = {nullptr, nullptr, nullptr};
  int previous_ = -1;
  bool reload_ = false;
  bool gate_ = false;
  float previousNote_ = 0;
  float accent_ = 0.8f;
  DecayEnvelope decayEnvelope_;
  LPGEnvelope lpgEnvelope_;
  stmlib::Limiter outLimiter_, auxLimiter_;
  LowPassGate outLpg_, auxLpg_;
  float out_[kMaxBlockSize];
  float aux_[kMaxBlockSize];
};

PlaitsVoice::PlaitsVoice() : impl_(new VoiceImpl), ram_(new char[kRam]()) { impl_->init(ram_); }

PlaitsVoice::~PlaitsVoice() {
  delete impl_;
  delete[] ram_;
}

void PlaitsVoice::setBank(int sixOp, const uint8_t* bank) {
  if (sixOp >= 0 && sixOp < 3) impl_->setBank(sixOp, bank);
}

void PlaitsVoice::reset() {
  impl_->reset();
  pendingTrigger_ = false;
}

void PlaitsVoice::render(const Settings& s, float note, bool gate, float level, float* out, float* aux) {
  impl_->render(s, std::exchange(pendingTrigger_, false), note, gate, level, out, aux);
  model_ = s.model;
}

}  // namespace ion::plaits_port
