#pragma once

#include <vector>

#include "audio/wav.h"
#include "midi/smf.h"

namespace ion {

class Module;
class Session;

// Renders `events` through `module` (already set up by `session`) with no audio
// device, sample-accurately, for `seconds`. Used by ion-render and golden tests.
wav::Audio renderOffline(Module& module, Session& session, const std::vector<smf::Event>& events, double seconds);

}  // namespace ion
