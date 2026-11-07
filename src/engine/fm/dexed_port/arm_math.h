// Portable stand-in for the parts of CMSIS-DSP (and the Teensy/Circle platform
// helpers) that Synth_Dexed uses. Synth_Dexed includes <arm_math.h>
// unconditionally and gets millis(), constrain() and signed_saturate_rshift()
// from its platform, so on Linux this header supplies all of them.
//
// Part of ion. GPL-3.0-or-later.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef float float32_t;
typedef int16_t q15_t;
typedef bool boolean;  // Arduino type used by compressor.h

// ---- platform helpers -------------------------------------------------------

// Synth_Dexed timestamps note-ons with millis() to pick a voice to steal.
// ion drives this clock from the audio sample counter, so voice stealing is
// deterministic in offline renders and needs no system call on the audio thread.
extern uint32_t g_ion_dexed_millis;  // defined in fm_engine.cpp
static inline uint32_t millis(void) { return g_ion_dexed_millis; }

// Same definition Synth_Dexed uses on Circle (GNU statement expression).
#define constrain(amt, low, high) ({ \
  __typeof__(amt) _amt = (amt); \
  __typeof__(low) _low = (low); \
  __typeof__(high) _high = (high); \
  (_amt < _low) ? _low : ((_amt > _high) ? _high : _amt); \
})

static inline int32_t signed_saturate_rshift(int32_t val, int32_t bits, int32_t rshift)
{
  int32_t out = val >> rshift;
  const int32_t max = 1 << (bits - 1);
  if (out > max - 1) out = max - 1;
  if (out < -max) out = -max;
  return out;
}

// ---- CMSIS-DSP subset ---------------------------------------------------------

static inline void arm_fill_f32(float32_t value, float32_t* dst, uint32_t n)
{
  for (uint32_t i = 0; i < n; ++i) dst[i] = value;
}

static inline void arm_scale_f32(const float32_t* src, float32_t scale, float32_t* dst, uint32_t n)
{
  for (uint32_t i = 0; i < n; ++i) dst[i] = src[i] * scale;
}

static inline void arm_mult_f32(const float32_t* a, const float32_t* b, float32_t* dst, uint32_t n)
{
  for (uint32_t i = 0; i < n; ++i) dst[i] = a[i] * b[i];
}

static inline void arm_offset_f32(const float32_t* src, float32_t offset, float32_t* dst, uint32_t n)
{
  for (uint32_t i = 0; i < n; ++i) dst[i] = src[i] + offset;
}

static inline void arm_sub_f32(const float32_t* a, const float32_t* b, float32_t* dst, uint32_t n)
{
  for (uint32_t i = 0; i < n; ++i) dst[i] = a[i] - b[i];
}

static inline float32_t arm_sin_f32(float32_t x) { return sinf(x); }
static inline float32_t arm_cos_f32(float32_t x) { return cosf(x); }

typedef enum { ARM_MATH_SUCCESS = 0, ARM_MATH_ARGUMENT_ERROR = -1 } arm_status;

static inline arm_status arm_sqrt_f32(float32_t in, float32_t* out)
{
  if (in >= 0.0f) { *out = sqrtf(in); return ARM_MATH_SUCCESS; }
  *out = 0.0f;
  return ARM_MATH_ARGUMENT_ERROR;
}

static inline void arm_float_to_q15(const float32_t* src, q15_t* dst, uint32_t n)
{
  for (uint32_t i = 0; i < n; ++i) {
    float v = src[i] * 32768.0f;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    dst[i] = (q15_t)v;
  }
}

// Direct form I biquad cascade. Coefficients per stage: b0, b1, b2, a1, a2,
// with CMSIS's sign convention (y = b0x0 + b1x1 + b2x2 + a1y1 + a2y2).
typedef struct {
  uint32_t numStages;
  float32_t* pState;        // 4 per stage: x1, x2, y1, y2
  const float32_t* pCoeffs; // 5 per stage
} arm_biquad_casd_df1_inst_f32;

static inline void arm_biquad_cascade_df1_init_f32(arm_biquad_casd_df1_inst_f32* s, uint8_t numStages,
                                                   const float32_t* pCoeffs, float32_t* pState)
{
  s->numStages = numStages;
  s->pCoeffs = pCoeffs;
  s->pState = pState;
  memset(pState, 0, sizeof(float32_t) * 4u * numStages);
}

static inline void arm_biquad_cascade_df1_f32(const arm_biquad_casd_df1_inst_f32* s, const float32_t* src,
                                              float32_t* dst, uint32_t n)
{
  const float32_t* in = src;
  for (uint32_t stage = 0; stage < s->numStages; ++stage) {
    const float32_t* c = s->pCoeffs + 5 * stage;
    float32_t* st = s->pState + 4 * stage;
    float32_t x1 = st[0], x2 = st[1], y1 = st[2], y2 = st[3];
    for (uint32_t i = 0; i < n; ++i) {
      const float32_t x0 = in[i];
      const float32_t y0 = c[0] * x0 + c[1] * x1 + c[2] * x2 + c[3] * y1 + c[4] * y2;
      x2 = x1; x1 = x0; y2 = y1; y1 = y0;
      dst[i] = y0;
    }
    st[0] = x1; st[1] = x2; st[2] = y1; st[3] = y2;
    in = dst;
  }
}
