#include "project/config.h"

#include <sstream>

#define TOML_EXCEPTIONS 0
#include <toml.hpp>

#include "project/paths.h"

namespace ion {

Config Config::load(const std::filesystem::path& path, std::vector<std::string>& warnings) {
  Config c;
  if (!std::filesystem::exists(path)) return c;
  toml::parse_result result = toml::parse_file(path.string());
  if (!result) {
    warnings.push_back(path.string() + ": " + std::string(result.error().description()) + " — using defaults");
    return c;
  }
  const toml::table& t = result.table();

  auto num = [&](const char* section, const char* key, auto& out, long lo, long hi) {
    if (auto v = t[section][key].value<int64_t>()) {
      if (*v >= lo && *v <= hi) out = static_cast<std::remove_reference_t<decltype(out)>>(*v);
      else warnings.push_back(std::string(section) + "." + key + " out of range, using " + std::to_string(out));
    }
  };
  auto str = [&](const char* section, const char* key, std::string& out) {
    if (auto v = t[section][key].value<std::string>()) out = *v;
  };
  auto boolean = [&](const char* section, const char* key, bool& out) {
    if (auto v = t[section][key].value<bool>()) out = *v;
  };

  num("audio", "latency", c.latencyFrames, 32, 2048);
  str("midi", "input", c.midiInput);
  boolean("midi", "accept_all", c.midiAcceptAll);
  if (auto v = t["midi"]["timing"].value<std::string>()) {
    if (*v == "tight") c.timing = MidiTiming::Tight;
    else if (*v == "immediate") c.timing = MidiTiming::Immediate;
    else warnings.push_back("midi.timing must be \"tight\" or \"immediate\"");
  }
  num("keyboard", "gate_ms", c.gateMs, 20, 5000);
  str("keyboard", "lower_row", c.lowerRow);
  str("keyboard", "upper_row", c.upperRow);
  boolean("fm", "program_change", c.programChange);
  boolean("fm", "preview_on_move", c.previewOnMove);
  str("fm", "import_folder", c.importFolder);
  if (const toml::array* roots = t["samples"]["roots"].as_array()) {
    std::vector<std::string> r;
    for (const auto& v : *roots)
      if (auto root = v.value<std::string>(); root && !root->empty()) r.push_back(*root);
    c.sampleRoots = r;
  }
  boolean("samples", "preview_on_move", c.samplePreview);
  num("samples", "preview_level_db", c.previewLevelDb, -40, 0);
  num("project", "autosave_minutes", c.autosaveMinutes, 0, 120);
  if (auto v = t["project"]["startup"].value<std::string>()) c.openLastProject = *v != "new";
  return c;
}

bool Config::save(const std::filesystem::path& path, std::string& error) const {
  toml::array roots;
  for (const std::string& r : sampleRoots) roots.push_back(r);
  toml::table t{
      {"audio", toml::table{{"latency", static_cast<int64_t>(latencyFrames)}}},
      {"midi", toml::table{{"input", midiInput},
                           {"accept_all", midiAcceptAll},
                           {"timing", timing == MidiTiming::Tight ? "tight" : "immediate"}}},
      {"keyboard", toml::table{{"gate_ms", gateMs}, {"lower_row", lowerRow}, {"upper_row", upperRow}}},
      {"fm", toml::table{{"program_change", programChange}, {"preview_on_move", previewOnMove},
                         {"import_folder", importFolder}}},
      {"samples", toml::table{{"roots", roots}, {"preview_on_move", samplePreview}, {"preview_level_db", previewLevelDb}}},
      {"project", toml::table{{"autosave_minutes", autosaveMinutes},
                              {"startup", openLastProject ? "last" : "new"}}},
  };
  std::ostringstream out;
  out << "# ion settings. Edit here or on the Settings screen (,).\n\n" << t << "\n";
  return writeFileAtomic(path, out.str(), error);
}

}  // namespace ion
