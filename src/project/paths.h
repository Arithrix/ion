#pragma once

#include <filesystem>
#include <string>

namespace ion {

// Where ion keeps things (HANDOFF §9).
struct Paths {
  std::filesystem::path configFile;   // ~/.config/ion/config.toml
  std::filesystem::path dataDir;      // ~/.local/share/ion
  std::filesystem::path projectsDir;  // ~/ion/projects

  // From $HOME, honouring $XDG_CONFIG_HOME and $XDG_DATA_HOME.
  static Paths defaults();
  // Everything under one root, for tests.
  static Paths under(const std::filesystem::path& root);

  std::filesystem::path projectDir(const std::string& name) const { return projectsDir / name; }
  std::filesystem::path projectFile(const std::string& name) const { return projectDir(name) / "project.json"; }
  std::filesystem::path autosaveFile(const std::string& name) const {
    return projectDir(name) / "project.autosave.json";
  }
  std::filesystem::path stateFile() const { return dataDir / "state.toml"; }
};

// Writes `content` to `path` via a temporary file and rename, so a crash never
// leaves a half-written file. Creates parent directories.
bool writeFileAtomic(const std::filesystem::path& path, const std::string& content, std::string& error);
bool readFile(const std::filesystem::path& path, std::string& content);

// Project names become directory names: letters, digits, - _ . and spaces only.
bool validProjectName(const std::string& name);

}  // namespace ion
