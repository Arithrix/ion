#include <cstring>

#include "engine/engine.h"
#include "engine/drum/drum_engine.h"
#include "engine/fm/fm_engine.h"
#include "engine/acid/acid_engine.h"
#include "engine/plaits/plaits_engine.h"
#include "engine/sampler/sampler_engine.h"

namespace ion {

const char* engineKey(EngineType type) {
  switch (type) {
    case EngineType::Fm: return "fm";
    case EngineType::Drum: return "drum";
    case EngineType::Sampler: return "sampler";
    case EngineType::Plaits: return "plaits";
    case EngineType::Acid: return "acid";
    case EngineType::None: break;
  }
  return "none";
}

const char* engineLabel(EngineType type) {
  switch (type) {
    case EngineType::Fm: return "FM";
    case EngineType::Drum: return "DRUM";
    case EngineType::Sampler: return "SMPL";
    case EngineType::Plaits: return "PLTS";
    case EngineType::Acid: return "ACID";
    case EngineType::None: break;
  }
  return "—";
}

EngineType engineFromKey(const char* key) {
  for (EngineType t : {EngineType::Fm, EngineType::Drum, EngineType::Sampler, EngineType::Plaits, EngineType::Acid})
    if (std::strcmp(key, engineKey(t)) == 0) return t;
  return EngineType::None;
}

static_assert(DrumEngine::kParamCount <= kMaxEngineParams, "drum parameters don't fit the engine range");
static_assert(SamplerEngine::kParamCount <= kMaxEngineParams, "sampler parameters don't fit the engine range");
static_assert(FmEngine::kParamCount <= kMaxEngineParams, "FM parameters don't fit the engine range");
static_assert(PlaitsEngine::kParamCount <= kMaxEngineParams, "Plaits parameters don't fit the engine range");
static_assert(AcidEngine::kParamCount <= kMaxEngineParams, "Acid parameters don't fit the engine range");

std::span<const ParamInfo> engineParams(EngineType type) {
  switch (type) {
    case EngineType::Fm: return FmEngine::paramTable();
    case EngineType::Drum: return DrumEngine::paramTable();
    case EngineType::Sampler: return SamplerEngine::paramTable();
    case EngineType::Plaits: return PlaitsEngine::paramTable();
    case EngineType::Acid: return AcidEngine::paramTable();
    default: return {};
  }
}

}  // namespace ion
