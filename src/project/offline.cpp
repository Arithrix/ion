#include "project/offline.h"

#include <cmath>

#include "engine/module.h"
#include "project/session.h"

namespace ion {

wav::Audio renderOffline(Module& module, Session& session, const std::vector<smf::Event>& events, double seconds) {
  wav::Audio out;
  const auto total = static_cast<std::size_t>(std::ceil(seconds * kSampleRate));
  out.samples.resize(total * 2);

  std::size_t next = 0;
  std::size_t done = 0;
  while (done < total) {
    // Feed events up to a second ahead; the MIDI queue holds 1024.
    const auto horizon = static_cast<int64_t>(done) + kSampleRate;
    int pushed = 0;
    while (next < events.size() && pushed < 512) {
      const auto frame = static_cast<int64_t>(std::llround(events[next].seconds * kSampleRate));
      if (frame > horizon) break;
      if (!module.pushMidi({events[next].msg, MidiEvent::Timing::Frame, frame})) break;
      ++next;
      ++pushed;
    }
    const std::size_t n = std::min<std::size_t>(kBlockSize, total - done);
    module.process(out.samples.data() + 2 * done, n, 0);
    done += n;
    session.tick();
  }
  return out;
}

}  // namespace ion
