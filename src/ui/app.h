#pragma once

#include <string>
#include <vector>

namespace ion {
class Session;
class Module;
class PipeWireOutput;
class AlsaSeqInput;
}  // namespace ion

namespace ion::ui {

enum class ScreenId { Tracks, Instrument, Mixer, Fx, Rec, Library, Settings, Mod };

struct RunOptions {
  PipeWireOutput* audio = nullptr;
  AlsaSeqInput* midi = nullptr;
  bool offerRecovery = false;  // an autosave newer than the project was found
  std::string notice;          // shown in the status line at start-up
};

// Runs the full-screen UI until the user quits.
int run(Session& session, Module& module, const RunOptions& options);

// One frame as plain text at a fixed size, for snapshot tests and bug reports.
// `keys` are pressed first: single characters, or Up Down Left Right Return
// Tab TabReverse Escape Backspace. Sample loads they start finish before the frame.
// Microseconds to build and lay out one frame of `screen`, averaged over
// `frames` (ion-bench). Includes FTXUI's layout and drawing into a cell buffer,
// not the terminal output.
double frameMicros(Session& session, Module& module, ScreenId screen, int width, int height, int track, int frames);

std::string snapshot(Session& session, Module& module, ScreenId screen, int width, int height, bool piano = false,
                     bool help = false, int track = 0, const std::vector<std::string>& keys = {});

}  // namespace ion::ui
