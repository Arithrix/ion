#pragma once

#include <string>

namespace ion {
class Session;
class Module;
class PipeWireOutput;
class AlsaSeqInput;
}  // namespace ion

namespace ion::ui {

enum class ScreenId { Tracks, Instrument, Mixer, Fx, Rec, Library, Settings };

struct RunOptions {
  PipeWireOutput* audio = nullptr;
  AlsaSeqInput* midi = nullptr;
  bool offerRecovery = false;  // an autosave newer than the project was found
  std::string notice;          // shown in the status line at start-up
};

// Runs the full-screen UI until the user quits.
int run(Session& session, Module& module, const RunOptions& options);

// One frame as plain text at a fixed size, for snapshot tests and bug reports.
std::string snapshot(Session& session, Module& module, ScreenId screen, int width, int height, bool piano = false,
                     bool help = false, int track = 0);

}  // namespace ion::ui
