#pragma once

// A small in-place radix-2 FFT for offline analysis (slicing). Not for the
// audio thread: it's simple rather than fast.

#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <utility>
#include <vector>

namespace ion::dsp {

// `x.size()` must be a power of two.
inline void fft(std::vector<std::complex<float>>& x) {
  const std::size_t n = x.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(x[i], x[j]);
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const double a = -2.0 * std::numbers::pi / static_cast<double>(len);
    const std::complex<float> w(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    for (std::size_t i = 0; i < n; i += len) {
      std::complex<float> wk(1.0f, 0.0f);
      for (std::size_t k = 0; k < len / 2; ++k) {
        const std::complex<float> u = x[i + k], v = x[i + k + len / 2] * wk;
        x[i + k] = u + v;
        x[i + k + len / 2] = u - v;
        wk *= w;
      }
    }
  }
}

// Magnitude spectra (bins 0…n/2) of frames of `n` samples every `hop`,
// Hann-windowed. Frame f covers [f·hop, f·hop + n); the signal is zero past its end.
inline std::vector<std::vector<float>> stft(const std::vector<float>& signal, std::size_t n, std::size_t hop) {
  std::vector<float> window(n);
  for (std::size_t i = 0; i < n; ++i)
    window[i] = 0.5f - 0.5f * static_cast<float>(std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n)));
  std::vector<std::vector<float>> out;
  std::vector<std::complex<float>> buf(n);
  for (std::size_t start = 0; start < signal.size(); start += hop) {
    for (std::size_t i = 0; i < n; ++i) buf[i] = start + i < signal.size() ? signal[start + i] * window[i] : 0.0f;
    fft(buf);
    std::vector<float> mag(n / 2 + 1);
    for (std::size_t k = 0; k <= n / 2; ++k) mag[k] = std::abs(buf[k]);
    out.push_back(std::move(mag));
  }
  return out;
}

}  // namespace ion::dsp
