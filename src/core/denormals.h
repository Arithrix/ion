#pragma once

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

namespace ion {

// Flush denormals to zero on the calling thread. Call once at the top of every
// real-time thread (the audio callback calls it each time; it's a register write).
inline void disableDenormals() noexcept {
#if defined(__SSE__) || defined(__x86_64__)
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(__aarch64__)
  unsigned long fpcr;
  __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
  fpcr |= (1ul << 24);  // FZ
  __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#endif
}

}  // namespace ion
