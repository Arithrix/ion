#pragma once

#include <string>
#include <vector>

#include "library/syx.h"

namespace ion {

class Module;
class PipeWireOutput;
class AlsaSeqInput;

struct ShellContext {
  Module& module;
  PipeWireOutput* audio = nullptr;
  AlsaSeqInput* midi = nullptr;
  std::vector<syx::Voice> voices;  // the loaded bank, shown on track 1
  std::string bankName;
  int voiceIndex = 0;
};

// Runs the full-screen terminal UI until the user quits. Returns an exit code.
int runShell(ShellContext& context);

// Renders one frame of the UI at a fixed size as plain text (no colour), for
// snapshot tests and bug reports.
std::string snapshot(ShellContext& context, int width, int height);

}  // namespace ion
