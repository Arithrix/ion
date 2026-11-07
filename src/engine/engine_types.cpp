#include <cstring>

#include "engine/engine.h"
#include "engine/fm/fm_engine.h"

namespace ion {

const char* engineKey(EngineType type) {
  switch (type) {
    case EngineType::Fm: return "fm";
    case EngineType::Drum: return "drum";
    case EngineType::Sampler: return "sampler";
    case EngineType::Plaits: return "plaits";
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
    case EngineType::None: break;
  }
  return "—";
}

EngineType engineFromKey(const char* key) {
  for (EngineType t : {EngineType::Fm, EngineType::Drum, EngineType::Sampler, EngineType::Plaits})
    if (std::strcmp(key, engineKey(t)) == 0) return t;
  return EngineType::None;
}

std::span<const ParamInfo> engineParams(EngineType type) {
  switch (type) {
    case EngineType::Fm: return FmEngine::paramTable();
    default: return {};
  }
}

}  // namespace ion
