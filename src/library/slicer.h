#pragma once

// Where to cut a recording into slices for drum pads (HANDOFF §7.6, DECISIONS D-52).
// Every function returns slice start frames: ascending, the first always 0.

#include <cstdint>
#include <vector>

namespace ion {

enum class SliceMode : uint8_t { Equal, Chords, Transients };

// `count` equal slices.
std::vector<int64_t> equalSlices(int64_t frames, int count);

// Where the harmony changes: 12-bin chroma averaged over each side of a
// candidate point, compared by cosine distance. With a tempo (`framesPerBeat`
// > 0) the candidates are the beats, since a recording that starts on a
// downbeat has its chord changes there; without one they're every ~43 ms and
// the strongest peaks win. At most `maxSlices` slices.
std::vector<int64_t> chordSlices(const std::vector<float>& mono, double framesPerBeat, int maxSlices);

// Attacks: spectral flux peaks over an adaptive threshold, each moved back to
// where its rise begins, then to the zero crossing just before. At most
// `maxSlices`; the strongest are kept.
std::vector<int64_t> transientSlices(const std::vector<float>& mono, int maxSlices);

}  // namespace ion
