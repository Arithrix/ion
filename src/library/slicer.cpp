#include "library/slicer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

#include "dsp/fft.h"

namespace ion {
namespace {

constexpr double kRate = 48000;

// Keeps the `keep` strongest candidates (by score), in time order, after slice 0.
std::vector<int64_t> strongest(std::vector<std::pair<int64_t, float>> found, int maxSlices) {
  const std::size_t keep = static_cast<std::size_t>(std::max(0, maxSlices - 1));
  if (found.size() > keep) {
    std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    found.resize(keep);
  }
  std::vector<int64_t> out = {0};
  for (const auto& f : found) out.push_back(f.first);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace

std::vector<int64_t> equalSlices(int64_t frames, int count) {
  count = std::max(1, count);
  std::vector<int64_t> out;
  for (int i = 0; i < count; ++i) out.push_back(frames * i / count);
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// ---- chord changes ----------------------------------------------------------

std::vector<int64_t> chordSlices(const std::vector<float>& mono, double framesPerBeat, int maxSlices) {
  constexpr std::size_t kN = 4096, kHop = 1024;  // 85 ms windows, 21 ms apart
  const auto frames = static_cast<int64_t>(mono.size());
  if (frames < static_cast<int64_t>(kN) || maxSlices < 2) return {0};
  const auto spec = dsp::stft(mono, kN, kHop);

  // Pitch class of each bin between 100 Hz and 5 kHz (−1 outside).
  std::vector<int> pitchClass(kN / 2 + 1, -1);
  for (std::size_t k = 1; k <= kN / 2; ++k) {
    const double f = static_cast<double>(k) * kRate / static_cast<double>(kN);
    if (f < 100 || f > 5000) continue;
    const long p = std::lround(69 + 12 * std::log2(f / 440.0));
    pitchClass[k] = static_cast<int>(((p % 12) + 12) % 12);
  }

  // Per frame: chroma normalised to unit length, and its level. Frames far
  // below the loudest don't count (silence, or the last of a tail).
  struct Frame {
    std::array<float, 12> chroma{};
    float level = 0;
  };
  std::vector<Frame> fr(spec.size());
  float loudest = 0;
  for (std::size_t f = 0; f < spec.size(); ++f) {
    for (std::size_t k = 0; k < spec[f].size(); ++k)
      if (pitchClass[k] >= 0) fr[f].chroma[static_cast<std::size_t>(pitchClass[k])] += spec[f][k];
    float norm = 0;
    for (float c : fr[f].chroma) norm += c * c;
    norm = std::sqrt(norm);
    fr[f].level = norm;
    loudest = std::max(loudest, norm);
    if (norm > 0)
      for (float& c : fr[f].chroma) c /= norm;
  }
  if (loudest <= 0) return {0};
  const float gate = loudest * 0.003f;  // −50 dB

  // Mean chroma of the frames that lie wholly inside [a, b): how many there
  // are, and how many of them aren't silent.
  struct Side {
    int frames = 0, sounding = 0;
  };
  auto side = [&](int64_t a, int64_t b, std::array<float, 12>& mean) {
    mean.fill(0);
    Side n;
    for (std::size_t f = 0; f < fr.size(); ++f) {
      const auto start = static_cast<int64_t>(f * kHop);
      if (start < a || start + static_cast<int64_t>(kN) > b) continue;
      ++n.frames;
      if (fr[f].level < gate) continue;
      for (std::size_t c = 0; c < 12; ++c) mean[c] += fr[f].chroma[c];
      ++n.sounding;
    }
    return n;
  };
  // 1 − cosine similarity of the two sides of p; sound after silence is 1.
  auto novelty = [&](int64_t p, int64_t w) -> float {
    std::array<float, 12> before{}, after{};
    const int64_t reach = std::max(w, static_cast<int64_t>(kN + kHop));  // at least one whole window a side
    const Side nb = side(p - reach, p, before), na = side(p, p + reach, after);
    if (nb.frames == 0 || na.sounding == 0) return 0;  // the very start, or fading into silence
    if (nb.sounding == 0) return 1;
    float dot = 0, ab = 0, aa = 0;
    for (std::size_t c = 0; c < 12; ++c) {
      dot += before[c] * after[c];
      ab += before[c] * before[c];
      aa += after[c] * after[c];
    }
    return ab > 0 && aa > 0 ? 1.0f - dot / std::sqrt(ab * aa) : 0.0f;
  };

  std::vector<std::pair<int64_t, float>> candidates;
  if (framesPerBeat > 0) {
    const auto w = static_cast<int64_t>(framesPerBeat);
    for (int b = 1;; ++b) {
      const auto p = static_cast<int64_t>(std::llround(b * framesPerBeat));
      if (p >= frames - w / 4) break;
      candidates.emplace_back(p, novelty(p, w));
    }
  } else {
    const int64_t w = static_cast<int64_t>(kRate / 2);
    for (int64_t p = static_cast<int64_t>(kHop); p < frames - w / 4; p += static_cast<int64_t>(kHop))
      candidates.emplace_back(p, novelty(p, w));
  }
  // Most candidates sit inside a chord: their novelty is the noise floor
  // (chorus, vibrato, restrikes). A change has to stand well clear of it.
  if (candidates.empty()) return {0};
  std::vector<float> sorted;
  float top = 0;
  for (const auto& c : candidates) {
    sorted.push_back(c.second);
    top = std::max(top, c.second);
  }
  std::sort(sorted.begin(), sorted.end());
  const float floor = sorted[sorted.size() * 3 / 10];
  const float threshold = std::max({0.06f, 4.0f * floor, 0.1f * top});

  std::vector<std::pair<int64_t, float>> found;
  if (framesPerBeat > 0) {
    // Each beat's window ends where the next begins: a change shows on one beat only.
    for (const auto& c : candidates)
      if (c.second >= threshold) found.push_back(c);
  } else {
    // Peaks at least a quarter second apart.
    const int64_t spacing = static_cast<int64_t>(kRate / 4);
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      const auto [p, v] = candidates[i];
      if (v < threshold || p < spacing) continue;
      bool peak = true;
      for (std::size_t j = 0; j < candidates.size() && peak; ++j)
        if (j != i && std::llabs(candidates[j].first - p) < spacing &&
            (candidates[j].second > v || (candidates[j].second == v && j < i)))
          peak = false;
      if (peak) found.push_back(candidates[i]);
    }
  }
  return strongest(std::move(found), maxSlices);
}

// ---- transients -------------------------------------------------------------

std::vector<int64_t> transientSlices(const std::vector<float>& mono, int maxSlices) {
  constexpr std::size_t kN = 1024, kHop = 256;  // 21 ms windows, 5.3 ms apart
  const auto frames = static_cast<int64_t>(mono.size());
  if (frames < static_cast<int64_t>(kN) || maxSlices < 2) return {0};
  const auto spec = dsp::stft(mono, kN, kHop);
  const std::size_t n = spec.size();

  // Log-magnitude spectral flux: how much louder each bin got.
  std::vector<float> flux(n, 0.0f);
  std::vector<float> prev(kN / 2 + 1, 0.0f), cur(kN / 2 + 1);
  for (std::size_t t = 0; t < n; ++t) {
    float sum = 0;
    for (std::size_t k = 0; k <= kN / 2; ++k) {
      cur[k] = std::log1p(spec[t][k] * (2000.0f / static_cast<float>(kN)));
      if (t > 0) sum += std::max(0.0f, cur[k] - prev[k]);
    }
    flux[t] = sum;
    std::swap(prev, cur);
  }
  const float top = *std::max_element(flux.begin(), flux.end());
  if (top <= 0) return {0};

  // Above a local median (which follows sustained busy material) and a floor.
  constexpr int kMedian = 10, kPeak = 3;
  constexpr int64_t kSpacing = static_cast<int64_t>(kRate * 0.04);  // 40 ms
  std::vector<std::pair<int64_t, float>> found;
  std::vector<float> window;
  int64_t last = 0;
  for (std::size_t t = 1; t < n; ++t) {
    const std::size_t a = t >= kMedian ? t - kMedian : 0, b = std::min(n, t + kMedian + 1);
    window.assign(flux.begin() + static_cast<std::ptrdiff_t>(a), flux.begin() + static_cast<std::ptrdiff_t>(b));
    std::nth_element(window.begin(), window.begin() + static_cast<std::ptrdiff_t>(window.size() / 2), window.end());
    const float threshold = 1.5f * window[window.size() / 2] + 0.1f * top;
    if (flux[t] < threshold) continue;
    bool peak = true;
    for (std::size_t j = t >= kPeak ? t - kPeak : 0; j < std::min(n, t + kPeak + 1) && peak; ++j)
      if (j != t && (flux[j] > flux[t] || (flux[j] == flux[t] && j < t))) peak = false;
    if (!peak) continue;

    // Where the rise starts: the loudest 32-sample chunk in the window, then
    // back while it's still well above the floor before it.
    constexpr int64_t kChunk = 32;
    const int64_t from = std::max<int64_t>(0, static_cast<int64_t>(t * kHop) - static_cast<int64_t>(kHop));
    const int64_t to = std::min(frames, static_cast<int64_t>(t * kHop + kN));
    std::vector<float> env;
    for (int64_t c = from; c < to; c += kChunk) {
      float e = 0;
      for (int64_t i = c; i < std::min(to, c + kChunk); ++i) e = std::max(e, std::fabs(mono[static_cast<std::size_t>(i)]));
      env.push_back(e);
    }
    if (env.empty()) continue;
    const auto m = static_cast<std::size_t>(std::max_element(env.begin(), env.end()) - env.begin());
    const float floor = *std::min_element(env.begin(), env.begin() + static_cast<std::ptrdiff_t>(m) + 1);
    const float rise = floor + 0.2f * (env[m] - floor);
    std::size_t j = m;
    while (j > 0 && env[j - 1] > rise) --j;
    int64_t at = from + static_cast<int64_t>(j) * kChunk;
    // Back to the zero crossing just before (within 1 ms), so the slice starts clean.
    for (int64_t i = at; i > std::max<int64_t>(1, at - 48); --i) {
      const float x0 = mono[static_cast<std::size_t>(i - 1)], x1 = mono[static_cast<std::size_t>(i)];
      if ((x0 <= 0 && x1 > 0) || (x0 >= 0 && x1 < 0) || x1 == 0) {
        at = i;
        break;
      }
    }
    if (at < kSpacing / 2) continue;  // the start: slice 0 already covers it
    if (!found.empty() && at - last < kSpacing) {
      if (flux[t] > found.back().second) found.back() = {at, flux[t]};
      last = found.back().first;
      continue;
    }
    found.emplace_back(at, flux[t]);
    last = at;
  }
  return strongest(std::move(found), maxSlices);
}

}  // namespace ion
