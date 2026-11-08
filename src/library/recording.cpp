#include "library/recording.h"

#include <nlohmann/json.hpp>

#include "project/paths.h"

namespace ion {

std::filesystem::path sidecarPath(const std::filesystem::path& audio) {
  std::filesystem::path p = audio;
  p.replace_extension(".json");
  return p;
}

RecordingInfo readRecordingInfo(const std::filesystem::path& audio) {
  RecordingInfo info;
  std::string text;
  if (!readFile(sidecarPath(audio), text)) return info;
  const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("ion_recording")) return info;
  try {
    info.tempo = j.value("tempo", 0.0);
    info.bars = j.value("bars", 0);
    info.beatsPerBar = std::max(1, j.value("beats_per_bar", 4));
    info.sources = j.value("sources", std::string());
    info.recordedAt = j.value("recorded_at", std::string());
    info.sentTo = j.value("sent_to", std::string());
    info.sidecar = true;
    info.ok = info.tempo > 0;
  } catch (const nlohmann::json::exception&) {
    info = {};
  }
  return info;
}

}  // namespace ion
