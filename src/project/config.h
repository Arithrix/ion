#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "engine/module.h"

namespace ion {

// ~/.config/ion/config.toml: machine settings that aren't part of a project.
struct Config {
  // [audio]
  unsigned latencyFrames = 128;
  // [midi]
  std::string midiInput = "OXI";  // connect inputs whose name contains this
  bool midiAcceptAll = false;     // …or every hardware input
  MidiTiming timing = MidiTiming::Tight;
  // [keyboard] — computer keyboard as piano (HANDOFF §8)
  int gateMs = 300;
  std::string lowerRow = "zsxdcvgbhnjm,l.;/";  // C … E, base octave
  std::string upperRow = "q2w3er5t6y7ui9o0p";  // C … E, one octave up
  // [fm]
  bool programChange = true;   // program change on a track's channel picks from the project's program list
  bool previewOnMove = false;  // play a short C3 each time the browser loads a patch
  std::string importFolder;    // the last folder imported, offered next time
  // [project]
  int autosaveMinutes = 2;
  bool openLastProject = true;

  // Missing file → defaults. Unknown or bad values are reported in `warnings`
  // and fall back to defaults; never throws.
  static Config load(const std::filesystem::path& path, std::vector<std::string>& warnings);
  bool save(const std::filesystem::path& path, std::string& error) const;
};

}  // namespace ion
