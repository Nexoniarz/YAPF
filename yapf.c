/*
 * yapf.c  —  Yet Another Picture Format
 * Encoder and decoder implementation.
 *
 * Copyright 2026 Nexoniarz
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE        /* sysconf(_SC_NPROCESSORS_ONLN) on macOS */
#endif

#include "yapf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ───────────────────────────────────────────────────────── */

#define YAPF__MAGIC       "YAPF"
#define YAPF__VERSION     1
#define YAPF__HDR_LEN     20
#define YAPF__MIP_ENTRY   8
#define YAPF__TILE_PX     (YAPF_TILE_SIZE * YAPF_TILE_SIZE)

#define YAPF__GROUP       8      /* residuals per group: w bits each = w bytes */
#define YAPF__MAX_GROUPS  (YAPF__TILE_PX / YAPF__GROUP)
#define YAPF__BAND        8      /* rows sharing one filter byte               */
#define YAPF__STORED      0x80   /* tile header: raw pixels follow             */

/* Control nibble → number of groups it covers.
 * 0..8: one group of that width;  9..15: that many groups repeating the
 * width of the last 0..8 nibble (0 at the start of a plane). */
static const uint8_t yapf__nib_groups[16] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 3, 4, 8, 16, 32, 64
};
#define YAPF__NIB_REPEAT  9

/* Row filters (one byte per band of 8 rows, per plane).  L / A / D / AR
 * are the left, above, above-left and above-right samples.  Every filter
 * depends on the previous sample through at most one add; MED and
 * average-with-left predictors were measured and dropped (~1 % smaller,
 * half the decode speed). */
enum {
    YAPF__F_LEFT    = 0,    /* L                                      */
    YAPF__F_GRAD    = 1,    /* L + A - D                              */
    YAPF__F_UPRIGHT = 2,    /* AR                                     */
    YAPF__F_UPAVG   = 3,    /* (A + AR) >> 1                          */
    YAPF__F_UP      = 16    /* 16 + k - 1: the sample k rows above,   */
                            /* k = 1..8 (repeating textures, dither)  */
};
#define YAPF__UP_MAX      8
#define YAPF__RING        16     /* decoder row ring, >= UP_MAX + 1, power of 2 */

static int yapf__filter_valid(uint8_t f) {
    return f <= YAPF__F_UPAVG || (f >= YAPF__F_UP && f < YAPF__F_UP + YAPF__UP_MAX);
}

/* Filters the encoder tries, cheapest to decode first: the up-row filters
 * vectorise, Left and Gradient carry a serial dependency.  On equal cost
 * the earlier one wins. */
static const uint8_t yapf__filters[] = {
    YAPF__F_UP + 0, YAPF__F_UP + 1, YAPF__F_UP + 2, YAPF__F_UP + 3,
    YAPF__F_UP + 4, YAPF__F_UP + 5, YAPF__F_UP + 6, YAPF__F_UP + 7,
    YAPF__F_UPRIGHT, YAPF__F_UPAVG, YAPF__F_LEFT, YAPF__F_GRAD
};

static uint32_t yapf__r32le(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] <<  8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void yapf__w32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >>  8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Little-endian 64-bit load; compilers turn this into a single load. */
static uint64_t yapf__r64le(const uint8_t *p) {
    return (uint64_t)p[0]
         | ((uint64_t)p[1] <<  8)
         | ((uint64_t)p[2] << 16)
         | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32)
         | ((uint64_t)p[5] << 40)
         | ((uint64_t)p[6] << 48)
         | ((uint64_t)p[7] << 56);
}

/* ── Growable output buffer (encoder) ────────────────────────────────── */

typedef struct {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
} yapf__out;

static int yapf__reserve(yapf__out *o, size_t n) {
    if (o->len + n <= o->cap) return 0;
    size_t nc = o->cap ? o->cap : 4096;
    while (nc < o->len + n) nc *= 2;
    uint8_t *nb = (uint8_t *)realloc(o->buf, nc);
    if (!nb) return -1;
    o->buf = nb;
    o->cap = nc;
    return 0;
}

/* ── YCoCg-R, 8-bit modular form ─────────────────────────────────────── */
/*
 * The usual YCoCg-R lifting steps, computed mod 256, with Co and Cg biased
 * by 128 so neutral colours sit mid-range.  Every lifting step adds a
 * function of the other channel, so the transform stays exactly
 * reversible while every plane remains one byte per sample.
 */
static void yapf__forward(uint8_t *y, uint8_t *co, uint8_t *cg,
                          uint8_t r, uint8_t g, uint8_t b) {
    uint8_t o = (uint8_t)(r - b);
    uint8_t t = (uint8_t)(b + ((int8_t)o >> 1));
    uint8_t q = (uint8_t)(g - t);
    *y  = (uint8_t)(t + ((int8_t)q >> 1));
    *co = (uint8_t)(o + 128);
    *cg = (uint8_t)(q + 128);
}

/* ── Prediction ──────────────────────────────────────────────────────── */
/* Samples outside the tile (left of column 0, above row 0) read as 0. */


/* Residuals are folded so small magnitudes become small codes:
 * 0, -1, 1, -2, 2, ... → 0, 1, 2, 3, 4, ... */
static uint8_t yapf__zz(uint8_t r) {
    return (uint8_t)((r << 1) ^ (uint8_t)((int8_t)r >> 7));
}

static uint8_t yapf__unzz(uint8_t z) {
    return (uint8_t)((z >> 1) ^ (uint8_t)-(z & 1));
}

/* Prediction for one row: R = the row, U = the row above, K = the row k
 * rows above for YAPF__F_UP + k - 1 (rows outside the tile are all 0).
 * Writes pred[0..tw).  The encoder knows every sample, so this has no
 * serial dependency. */
static void yapf__predict_row(uint8_t f, const uint8_t *R, const uint8_t *U,
                              const uint8_t *K, uint8_t *pred, uint32_t tw)
{
    uint32_t x;
    switch (f) {
    case YAPF__F_LEFT:
        pred[0] = 0;
        for (x = 1; x < tw; x++) pred[x] = R[x - 1];
        break;
    case YAPF__F_GRAD:
        pred[0] = U[0];
        for (x = 1; x < tw; x++) pred[x] = (uint8_t)(R[x - 1] + U[x] - U[x - 1]);
        break;
    case YAPF__F_UPRIGHT:
        for (x = 0; x + 1 < tw; x++) pred[x] = U[x + 1];
        pred[tw - 1] = U[tw - 1];
        break;
    case YAPF__F_UPAVG:
        for (x = 0; x + 1 < tw; x++) pred[x] = (uint8_t)((U[x] + U[x + 1]) >> 1);
        pred[tw - 1] = U[tw - 1];
        break;
    default:
        memcpy(pred, K, tw);
        break;
    }
}

static int yapf__width(uint8_t z) {
    int w = 0;
    while (z) { z >>= 1; w++; }
    return w;
}

static const uint8_t yapf__zero_row[YAPF_TILE_SIZE + 1] = { 0 };

/* ── SIMD ────────────────────────────────────────────────────────────── */
/*
 * On by default; define YAPF_NO_SIMD for plain C everywhere.
 *   x86 / x64 (GCC, Clang, MSVC, MinGW): SSSE3 kernels, chosen at run time
 *     with CPUID, so the same binary still runs on CPUs without SSSE3.
 *   ARM64 (GCC, Clang, MSVC): NEON kernels; NEON is always present.
 * Every kernel produces exactly the same bytes as the scalar code.  Row
 * kernels work in whole 16-sample chunks: they may read and write up to
 * the end of the 64 (+1) sample row buffers, never past them, and pixels
 * of narrow edge tiles go through a scratch row before being copied out.
 *
 * Group unpack, both ISAs: lane i of 16 bits gathers the two bytes holding
 * value i (table yapf__simd_shuf), shifts right by that value's bit offset
 * and masks to w bits.  SSSE3 has no per-lane shift, so it multiplies:
 * (2u * 2^(15 - s)) >> 16 = u >> s; overflow of 2u only touches bits >= 8.
 */
#if !defined(YAPF_NO_SIMD) && \
    (defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86))
#  define YAPF__SIMD_X86 1
#elif !defined(YAPF_NO_SIMD) && (defined(__aarch64__) || defined(_M_ARM64))
#  define YAPF__SIMD_NEON 1
#endif

#if defined(YAPF__SIMD_X86) || defined(YAPF__SIMD_NEON)
#  define YAPF__SIMD 1
static const uint8_t yapf__simd_shuf[9][16] = {
    { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 },
    { 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01 },
    { 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x01, 0x02, 0x01, 0x02, 0x01, 0x02, 0x01, 0x02 },
    { 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x01, 0x02, 0x01, 0x02, 0x01, 0x02, 0x02, 0x03, 0x02, 0x03 },
    { 0x00, 0x01, 0x00, 0x01, 0x01, 0x02, 0x01, 0x02, 0x02, 0x03, 0x02, 0x03, 0x03, 0x04, 0x03, 0x04 },
    { 0x00, 0x01, 0x00, 0x01, 0x01, 0x02, 0x01, 0x02, 0x02, 0x03, 0x03, 0x04, 0x03, 0x04, 0x04, 0x05 },
    { 0x00, 0x01, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0x04, 0x03, 0x04, 0x04, 0x05, 0x05, 0x06 },
    { 0x00, 0x01, 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0x04, 0x04, 0x05, 0x05, 0x06, 0x06, 0x07 },
    { 0x00, 0x01, 0x01, 0x02, 0x02, 0x03, 0x03, 0x04, 0x04, 0x05, 0x05, 0x06, 0x06, 0x07, 0x07, 0x80 }
};
#else
#  define YAPF__SIMD 0
#endif

#if defined(YAPF__SIMD_X86)
#  if defined(_MSC_VER)
#    include <intrin.h>
#  else
#    include <cpuid.h>
#  endif
#  include <tmmintrin.h>
#  if defined(__GNUC__) || defined(__clang__)
#    define YAPF__SIMD_FN static __attribute__((target("ssse3")))
#  else
#    define YAPF__SIMD_FN static       /* MSVC allows intrinsics anywhere */
#  endif

static const uint16_t yapf__simd_mul[9][8] = {
    { 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000 },
    { 0x8000, 0x4000, 0x2000, 0x1000, 0x0800, 0x0400, 0x0200, 0x0100 },
    { 0x8000, 0x2000, 0x0800, 0x0200, 0x8000, 0x2000, 0x0800, 0x0200 },
    { 0x8000, 0x1000, 0x0200, 0x4000, 0x0800, 0x0100, 0x2000, 0x0400 },
    { 0x8000, 0x0800, 0x8000, 0x0800, 0x8000, 0x0800, 0x8000, 0x0800 },
    { 0x8000, 0x0400, 0x2000, 0x0100, 0x0800, 0x4000, 0x0200, 0x1000 },
    { 0x8000, 0x0200, 0x0800, 0x2000, 0x8000, 0x0200, 0x0800, 0x2000 },
    { 0x8000, 0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000 },
    { 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000 }
};

/* Shuffle masks: R, G, B rows of 16 → 48 interleaved bytes. */
static const uint8_t yapf__simd_rgb[3][3][16] = {
    {
        { 0x00, 0x80, 0x80, 0x01, 0x80, 0x80, 0x02, 0x80, 0x80, 0x03, 0x80, 0x80, 0x04, 0x80, 0x80, 0x05 },
        { 0x80, 0x00, 0x80, 0x80, 0x01, 0x80, 0x80, 0x02, 0x80, 0x80, 0x03, 0x80, 0x80, 0x04, 0x80, 0x80 },
        { 0x80, 0x80, 0x00, 0x80, 0x80, 0x01, 0x80, 0x80, 0x02, 0x80, 0x80, 0x03, 0x80, 0x80, 0x04, 0x80 }
    },
    {
        { 0x80, 0x80, 0x06, 0x80, 0x80, 0x07, 0x80, 0x80, 0x08, 0x80, 0x80, 0x09, 0x80, 0x80, 0x0A, 0x80 },
        { 0x05, 0x80, 0x80, 0x06, 0x80, 0x80, 0x07, 0x80, 0x80, 0x08, 0x80, 0x80, 0x09, 0x80, 0x80, 0x0A },
        { 0x80, 0x05, 0x80, 0x80, 0x06, 0x80, 0x80, 0x07, 0x80, 0x80, 0x08, 0x80, 0x80, 0x09, 0x80, 0x80 }
    },
    {
        { 0x80, 0x0B, 0x80, 0x80, 0x0C, 0x80, 0x80, 0x0D, 0x80, 0x80, 0x0E, 0x80, 0x80, 0x0F, 0x80, 0x80 },
        { 0x80, 0x80, 0x0B, 0x80, 0x80, 0x0C, 0x80, 0x80, 0x0D, 0x80, 0x80, 0x0E, 0x80, 0x80, 0x0F, 0x80 },
        { 0x0A, 0x80, 0x80, 0x0B, 0x80, 0x80, 0x0C, 0x80, 0x80, 0x0D, 0x80, 0x80, 0x0E, 0x80, 0x80, 0x0F }
    }
};

/* CPUID leaf 1, ECX bit 9 = SSSE3.  Called once per load. */
static int yapf__simd_available(void) {
#  if defined(_MSC_VER)
    int r[4];
    __cpuid(r, 1);
    return (r[2] >> 9) & 1;
#  else
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d)) return 0;
    return (int)((c >> 9) & 1u);
#  endif
}

YAPF__SIMD_FN const uint8_t *yapf__simd_unpack(const uint8_t *p,
                                               const uint8_t *end,
                                               const uint8_t *gw, uint32_t ng,
                                               uint8_t *z)
{
    for (uint32_t g = 0; g < ng; g++) {
        int     w = gw[g];
        __m128i x;
        if ((size_t)(end - p) >= 8) {
            x = _mm_loadl_epi64((const __m128i *)(const void *)p);
        } else {
            uint8_t tmp[8] = {0};
            memcpy(tmp, p, (size_t)w);
            x = _mm_loadl_epi64((const __m128i *)(const void *)tmp);
        }
        __m128i u = _mm_shuffle_epi8(x, _mm_loadu_si128(
                        (const __m128i *)(const void *)yapf__simd_shuf[w]));
        u = _mm_add_epi16(u, u);
        __m128i v = _mm_mulhi_epu16(u, _mm_loadu_si128(
                        (const __m128i *)(const void *)yapf__simd_mul[w]));
        v = _mm_and_si128(v, _mm_set1_epi16((short)((1 << w) - 1)));
        _mm_storel_epi64((__m128i *)(void *)(z + g * YAPF__GROUP),
                         _mm_packus_epi16(v, v));
        p += w;
    }
    return p;
}

/* Left / Gradient for a full 64-sample row: the per-sample increments are
 * independent (r, or r + A - D), so compute them 16 at a time and turn the
 * serial chain into a prefix sum. */
YAPF__SIMD_FN void yapf__simd_serial_row(uint8_t f, uint8_t *R,
                                         const uint8_t *U, const uint8_t *Z,
                                         uint32_t tw)
{
    const __m128i zero  = _mm_setzero_si128();
    __m128i       carry = zero, uprev = zero;
    for (uint32_t k = 0; k < tw; k += 16) {
        __m128i zz = _mm_loadu_si128((const __m128i *)(const void *)(Z + k));
        __m128i t  = _mm_xor_si128(
            _mm_and_si128(_mm_srli_epi16(zz, 1), _mm_set1_epi8(0x7F)),
            _mm_sub_epi8(zero, _mm_and_si128(zz, _mm_set1_epi8(1))));
        if (f == YAPF__F_GRAD) {
            __m128i u = _mm_loadu_si128((const __m128i *)(const void *)(U + k));
            t     = _mm_add_epi8(t, _mm_sub_epi8(u, _mm_alignr_epi8(u, uprev, 15)));
            uprev = u;
        }
        t = _mm_add_epi8(t, _mm_slli_si128(t, 1));
        t = _mm_add_epi8(t, _mm_slli_si128(t, 2));
        t = _mm_add_epi8(t, _mm_slli_si128(t, 4));
        t = _mm_add_epi8(t, _mm_slli_si128(t, 8));
        t = _mm_add_epi8(t, carry);
        _mm_storeu_si128((__m128i *)(void *)(R + k), t);
        carry = _mm_shuffle_epi8(t, _mm_set1_epi8(15));
    }
}

/* Bytes as int8, arithmetic shift right by one: ((v ^ 0x80) >> 1) - 64. */
YAPF__SIMD_FN __m128i yapf__sse_half_s8(__m128i v) {
    __m128i x = _mm_xor_si128(v, _mm_set1_epi8((char)0x80));
    x = _mm_and_si128(_mm_srli_epi16(x, 1), _mm_set1_epi8(0x7F));
    return _mm_sub_epi8(x, _mm_set1_epi8(64));
}

YAPF__SIMD_FN __m128i yapf__sse_unzz(__m128i z) {
    return _mm_xor_si128(_mm_and_si128(_mm_srli_epi16(z, 1), _mm_set1_epi8(0x7F)),
                         _mm_sub_epi8(_mm_setzero_si128(), _mm_and_si128(z, _mm_set1_epi8(1))));
}

/* Vertical filters for a full 64-sample row: R = ((a + b) >> 1) + r. */
YAPF__SIMD_FN void yapf__simd_vertical_row(uint8_t *R, const uint8_t *A,
                                           const uint8_t *B, const uint8_t *Z,
                                           uint32_t tw)
{
    for (uint32_t k = 0; k < tw; k += 16) {
        __m128i a = _mm_loadu_si128((const __m128i *)(const void *)(A + k));
        __m128i b = _mm_loadu_si128((const __m128i *)(const void *)(B + k));
        __m128i avg = _mm_sub_epi8(_mm_avg_epu8(a, b),       /* avg rounds up */
                                   _mm_and_si128(_mm_xor_si128(a, b), _mm_set1_epi8(1)));
        __m128i z = _mm_loadu_si128((const __m128i *)(const void *)(Z + k));
        _mm_storeu_si128((__m128i *)(void *)(R + k), _mm_add_epi8(avg, yapf__sse_unzz(z)));
    }
}

/* Planes of one 64-pixel row → interleaved output pixels. */
YAPF__SIMD_FN void yapf__simd_inverse_row(uint8_t *d, const uint8_t *P0,
                                          const uint8_t *P1, const uint8_t *P2,
                                          const uint8_t *P3, int ch, int xf)
{
    const __m128i bias = _mm_set1_epi8((char)0x80);
    for (int k = 0; k < 64; k += 16) {
        __m128i p0 = _mm_loadu_si128((const __m128i *)(const void *)(P0 + k));
        if (ch == 1) {
            _mm_storeu_si128((__m128i *)(void *)(d + k), p0);
            continue;
        }
        __m128i p1 = _mm_loadu_si128((const __m128i *)(const void *)(P1 + k));
        if (ch == 2) {
            _mm_storeu_si128((__m128i *)(void *)(d + 2 * k), _mm_unpacklo_epi8(p0, p1));
            _mm_storeu_si128((__m128i *)(void *)(d + 2 * k + 16), _mm_unpackhi_epi8(p0, p1));
            continue;
        }
        __m128i p2 = _mm_loadu_si128((const __m128i *)(const void *)(P2 + k));
        __m128i r, g, b;
        if (xf == 2) {                                    /* none (v2)      */
            r = p0; g = p1; b = p2;
        } else if (xf) {                                  /* subtract-green */
            g = p0;
            r = _mm_add_epi8(_mm_sub_epi8(p1, bias), g);
            b = _mm_add_epi8(_mm_sub_epi8(p2, bias), g);
        } else {                                          /* YCoCg-R */
            __m128i co = _mm_sub_epi8(p1, bias), cg = _mm_sub_epi8(p2, bias);
            __m128i t  = _mm_sub_epi8(p0, yapf__sse_half_s8(cg));
            b = _mm_sub_epi8(t, yapf__sse_half_s8(co));
            r = _mm_add_epi8(b, co);
            g = _mm_add_epi8(cg, t);
        }
        if (ch == 3) {
            uint8_t *o = d + 3 * k;
            for (int j = 0; j < 3; j++) {
                const __m128i *m = (const __m128i *)(const void *)yapf__simd_rgb[j];
                __m128i v = _mm_or_si128(
                    _mm_or_si128(_mm_shuffle_epi8(r, _mm_loadu_si128(m)),
                                 _mm_shuffle_epi8(g, _mm_loadu_si128(m + 1))),
                    _mm_shuffle_epi8(b, _mm_loadu_si128(m + 2)));
                _mm_storeu_si128((__m128i *)(void *)(o + 16 * j), v);
            }
        } else {
            __m128i a  = _mm_loadu_si128((const __m128i *)(const void *)(P3 + k));
            __m128i rg0 = _mm_unpacklo_epi8(r, g), rg1 = _mm_unpackhi_epi8(r, g);
            __m128i ba0 = _mm_unpacklo_epi8(b, a), ba1 = _mm_unpackhi_epi8(b, a);
            uint8_t *o = d + 4 * k;
            _mm_storeu_si128((__m128i *)(void *)(o),      _mm_unpacklo_epi16(rg0, ba0));
            _mm_storeu_si128((__m128i *)(void *)(o + 16), _mm_unpackhi_epi16(rg0, ba0));
            _mm_storeu_si128((__m128i *)(void *)(o + 32), _mm_unpacklo_epi16(rg1, ba1));
            _mm_storeu_si128((__m128i *)(void *)(o + 48), _mm_unpackhi_epi16(rg1, ba1));
        }
    }
}

#elif defined(YAPF__SIMD_NEON)
#  include <arm_neon.h>
#  define YAPF__SIMD_FN static

static const int16_t yapf__simd_nshift[9][8] = {
    {  0,  0,  0,  0,  0,  0,  0,  0 },
    {  0, -1, -2, -3, -4, -5, -6, -7 },
    {  0, -2, -4, -6,  0, -2, -4, -6 },
    {  0, -3, -6, -1, -4, -7, -2, -5 },
    {  0, -4,  0, -4,  0, -4,  0, -4 },
    {  0, -5, -2, -7, -4, -1, -6, -3 },
    {  0, -6, -4, -2,  0, -6, -4, -2 },
    {  0, -7, -6, -5, -4, -3, -2, -1 },
    {  0,  0,  0,  0,  0,  0,  0,  0 }
};

static int yapf__simd_available(void) { return 1; }

YAPF__SIMD_FN const uint8_t *yapf__simd_unpack(const uint8_t *p,
                                               const uint8_t *end,
                                               const uint8_t *gw, uint32_t ng,
                                               uint8_t *z)
{
    for (uint32_t g = 0; g < ng; g++) {
        int     w = gw[g];
        uint8_t tmp[8] = {0};
        const uint8_t *src = p;
        if ((size_t)(end - p) < 8) { memcpy(tmp, p, (size_t)w); src = tmp; }
        uint8x16_t x = vcombine_u8(vld1_u8(src), vdup_n_u8(0));
        uint16x8_t u = vreinterpretq_u16_u8(vqtbl1q_u8(x, vld1q_u8(yapf__simd_shuf[w])));
        uint16x8_t v = vandq_u16(vshlq_u16(u, vld1q_s16(yapf__simd_nshift[w])),
                                 vdupq_n_u16((uint16_t)((1u << w) - 1)));
        vst1_u8(z + g * YAPF__GROUP, vmovn_u16(v));
        p += w;
    }
    return p;
}

YAPF__SIMD_FN void yapf__simd_serial_row(uint8_t f, uint8_t *R,
                                         const uint8_t *U, const uint8_t *Z,
                                         uint32_t tw)
{
    const uint8x16_t zero  = vdupq_n_u8(0);
    uint8x16_t       carry = zero, uprev = zero;
    for (uint32_t k = 0; k < tw; k += 16) {
        uint8x16_t zz = vld1q_u8(Z + k);
        uint8x16_t t  = veorq_u8(vshrq_n_u8(zz, 1),
                                 vsubq_u8(zero, vandq_u8(zz, vdupq_n_u8(1))));
        if (f == YAPF__F_GRAD) {
            uint8x16_t u = vld1q_u8(U + k);
            t     = vaddq_u8(t, vsubq_u8(u, vextq_u8(uprev, u, 15)));
            uprev = u;
        }
        t = vaddq_u8(t, vextq_u8(zero, t, 15));
        t = vaddq_u8(t, vextq_u8(zero, t, 14));
        t = vaddq_u8(t, vextq_u8(zero, t, 12));
        t = vaddq_u8(t, vextq_u8(zero, t, 8));
        t = vaddq_u8(t, carry);
        vst1q_u8(R + k, t);
        carry = vdupq_laneq_u8(t, 15);
    }
}

YAPF__SIMD_FN void yapf__simd_vertical_row(uint8_t *R, const uint8_t *A,
                                           const uint8_t *B, const uint8_t *Z,
                                           uint32_t tw)
{
    for (uint32_t k = 0; k < tw; k += 16) {
        uint8x16_t z = vld1q_u8(Z + k);
        uint8x16_t r = veorq_u8(vshrq_n_u8(z, 1),
                                vsubq_u8(vdupq_n_u8(0), vandq_u8(z, vdupq_n_u8(1))));
        vst1q_u8(R + k, vaddq_u8(vhaddq_u8(vld1q_u8(A + k), vld1q_u8(B + k)), r));
    }
}

YAPF__SIMD_FN void yapf__simd_inverse_row(uint8_t *d, const uint8_t *P0,
                                          const uint8_t *P1, const uint8_t *P2,
                                          const uint8_t *P3, int ch, int xf)
{
    const uint8x16_t bias = vdupq_n_u8(0x80);
    for (int k = 0; k < 64; k += 16) {
        uint8x16_t p0 = vld1q_u8(P0 + k);
        if (ch == 1) { vst1q_u8(d + k, p0); continue; }
        uint8x16_t p1 = vld1q_u8(P1 + k);
        if (ch == 2) {
            uint8x16x2_t v; v.val[0] = p0; v.val[1] = p1;
            vst2q_u8(d + 2 * k, v);
            continue;
        }
        uint8x16_t p2 = vld1q_u8(P2 + k), r, g, b;
        if (xf == 2) {
            r = p0; g = p1; b = p2;
        } else if (xf) {
            g = p0;
            r = vaddq_u8(vsubq_u8(p1, bias), g);
            b = vaddq_u8(vsubq_u8(p2, bias), g);
        } else {
            uint8x16_t co = vsubq_u8(p1, bias), cg = vsubq_u8(p2, bias);
            uint8x16_t t  = vsubq_u8(p0, vreinterpretq_u8_s8(vshrq_n_s8(vreinterpretq_s8_u8(cg), 1)));
            b = vsubq_u8(t, vreinterpretq_u8_s8(vshrq_n_s8(vreinterpretq_s8_u8(co), 1)));
            r = vaddq_u8(b, co);
            g = vaddq_u8(cg, t);
        }
        if (ch == 3) {
            uint8x16x3_t v; v.val[0] = r; v.val[1] = g; v.val[2] = b;
            vst3q_u8(d + 3 * k, v);
        } else {
            uint8x16x4_t v; v.val[0] = r; v.val[1] = g; v.val[2] = b; v.val[3] = vld1q_u8(P3 + k);
            vst4q_u8(d + 4 * k, v);
        }
    }
}
#endif


/* ── Plane coding ────────────────────────────────────────────────────── */
/*
 * Plane payload (all byte aligned):
 *   filter[ceil(th / 8)]   one YAPF__F_* byte per band of 8 rows
 *   control nibbles        low nibble first; one per group of 8 folded
 *                          residuals, in raster order:
 *                            0..8    width w of this group
 *                            9..15   2, 3, 4, 8, 16, 32 or 64 groups repeating
 *                                    the last 0..8 width (0 at plane start)
 *                          an odd count is padded with a zero nibble
 *   group data             w bytes per group: the 8 values packed
 *                          little-endian, value i at bit i*w
 * The last group of a plane is padded with zeros.
 */

/* Chooses a filter per band and writes the folded residuals to z (zero
 * padded to a whole group) and the filters to filt.  Returns the estimated
 * coded size in 1/16 byte units. */
static uint32_t yapf__choose_filters(const uint8_t *P, uint32_t tw, uint32_t th,
                                     uint8_t *z, uint8_t *filt)
{
    uint32_t total = tw * th;
    uint32_t nb    = (th + YAPF__BAND - 1) / YAPF__BAND;
    uint32_t sum   = 0;

    /* Per band, pick the filter whose residuals code smallest: group
     * widths plus control nibbles, in 1/16 byte units (zero groups mostly
     * end up in runs, so they are counted as cheap).  Left and Gradient
     * decode serially, so they must win by more than 4 %.  The previous
     * band's winner is tried first and the others stop as soon as they
     * cannot win; ties go to the earlier filter in yapf__filters. */
    size_t prev_fi = 0;
    for (uint32_t b = 0; b < nb; b++) {
        uint32_t y0 = b * YAPF__BAND;
        uint32_t y1 = y0 + YAPF__BAND < th ? y0 + YAPF__BAND : th;
        uint32_t n  = (y1 - y0) * tw;
        uint8_t *Z  = z + y0 * tw;
        uint8_t  tmp[YAPF__BAND * YAPF_TILE_SIZE + YAPF__GROUP];
        uint32_t best_cost = UINT32_MAX;
        size_t   best_fi = 0;

        for (size_t t = 0; t < sizeof(yapf__filters); t++) {
            size_t   fi = t == 0 ? prev_fi : (t <= prev_fi ? t - 1 : t);
            uint8_t  f  = yapf__filters[fi];
            uint32_t k  = f >= YAPF__F_UP ? (uint32_t)(f - YAPF__F_UP + 1) : 1;
            int      serial = f == YAPF__F_LEFT || f == YAPF__F_GRAD;
            uint32_t cost = 0, done = 0;
            int      lost = 0;

            for (uint32_t y = y0; y < y1 && !lost; y++) {
                const uint8_t *R = P + y * tw;
                const uint8_t *U = y ? R - tw : yapf__zero_row;
                const uint8_t *K = y >= k ? R - k * tw : yapf__zero_row;
                uint8_t       *T = tmp + (y - y0) * tw;
                uint8_t        pred[YAPF_TILE_SIZE];
                yapf__predict_row(f, R, U, K, pred, tw);
                for (uint32_t x = 0; x < tw; x++)
                    T[x] = yapf__zz((uint8_t)(R[x] - pred[x]));

                /* cost of the groups completed so far */
                uint32_t end = y + 1 == y1 ? n : (y + 1 - y0) * tw;
                if (y + 1 == y1) memset(tmp + n, 0, YAPF__GROUP);
                for (; done + YAPF__GROUP <= end || (y + 1 == y1 && done < end);
                     done += YAPF__GROUP) {
                    uint8_t m = 0;
                    for (int i = 0; i < YAPF__GROUP; i++) m |= tmp[done + i];
                    int w = yapf__width(m);
                    cost += w ? (uint32_t)(16 * w + 8) : 2;
                }
                uint32_t c = serial ? cost + cost / 25 : cost;
                if (c > best_cost || (c == best_cost && fi > best_fi)) lost = 1;
            }
            if (lost) continue;
            if (serial) cost += cost / 25;
            if (cost < best_cost || (cost == best_cost && fi < best_fi)) {
                best_cost = cost;
                best_fi = fi;
                memcpy(Z, tmp, n);
            }
        }
        prev_fi = best_fi;
        filt[b] = yapf__filters[best_fi];
        sum += best_cost;
    }
    memset(z + total, 0, YAPF__GROUP);
    return sum;
}

/* Writes a plane payload from residuals and filters made by
 * yapf__choose_filters. */
static int yapf__emit_plane(yapf__out *o, const uint8_t *z, const uint8_t *filt,
                            uint32_t tw, uint32_t th)
{
    uint32_t total = tw * th;
    uint32_t ng    = (total + YAPF__GROUP - 1) / YAPF__GROUP;
    uint32_t nb    = (th + YAPF__BAND - 1) / YAPF__BAND;
    uint8_t  gw[YAPF__MAX_GROUPS];
    uint8_t  nib[YAPF__MAX_GROUPS];
    uint32_t nn = 0;

    /* worst case: filters + a nibble and 8 bytes per group */
    if (yapf__reserve(o, nb + ng + (size_t)ng * 8) != 0) return -1;
    memcpy(o->buf + o->len, filt, nb);
    o->len += nb;

    /* Group widths → control nibbles.  A group may be stored wider than
     * it needs if that lets a repeat code cover it; a small dynamic
     * program over (group, current width) finds the cheapest sequence,
     * counted in nibbles (1 per code, 2 per data byte). */
    uint8_t need[YAPF__MAX_GROUPS];
    for (uint32_t g = 0; g < ng; g++) {
        uint8_t m = 0;
        for (int i = 0; i < YAPF__GROUP; i++) m |= z[g * YAPF__GROUP + i];
        need[g] = (uint8_t)yapf__width(m);
    }
    {
        static const uint8_t lens[7] = { 2, 3, 4, 8, 16, 32, 64 };
        uint32_t cost[YAPF__MAX_GROUPS + 1][9];
        uint8_t  pick[YAPF__MAX_GROUPS][9];      /* nibble code */

        /* span[j][g] = widest need in groups [g, g + 2^j) */
        static const int levels = 7;
        uint8_t span[7][YAPF__MAX_GROUPS];
        memcpy(span[0], need, ng);
        for (int j = 1; j < levels; j++)
            for (uint32_t g = 0; g + (1u << j) <= ng; g++) {
                uint8_t a = span[j - 1][g], c = span[j - 1][g + (1u << (j - 1))];
                span[j][g] = a > c ? a : c;
            }

        for (int pw = 0; pw <= 8; pw++) cost[ng][pw] = 0;
        for (uint32_t g = ng; g-- > 0; ) {
            /* Best literal: independent of the current width. */
            uint32_t lc = UINT32_MAX;
            uint8_t  lk = 0;
            for (int w = need[g]; w <= 8; w++) {
                uint32_t c = 1 + 2u * (uint32_t)w + cost[g + 1][w];
                if (c < lc) { lc = c; lk = (uint8_t)w; }
            }
            /* Widest need over each repeat length that fits, from the
             * power-of-two max table (length 3 = 2 + one more). */
            uint8_t  mx[7];
            int      nl = 0;
            for (; nl < 7 && g + lens[nl] <= ng; nl++) {
                uint32_t L = lens[nl];
                if (L == 3) {
                    uint8_t a = span[1][g], c = need[g + 2];
                    mx[nl] = a > c ? a : c;
                } else {
                    int j = 0;
                    while ((1u << j) < L) j++;
                    mx[nl] = span[j][g];
                }
            }
            for (int pw = 0; pw <= 8; pw++) {
                uint32_t bc = lc;
                uint8_t  bk = lk;
                for (int li = 0; li < nl && mx[li] <= pw; li++) {
                    uint32_t c = 1 + 2u * (uint32_t)pw * lens[li] + cost[g + lens[li]][pw];
                    if (c < bc) { bc = c; bk = (uint8_t)(YAPF__NIB_REPEAT + li); }
                }
                cost[g][pw] = bc;
                pick[g][pw] = bk;
            }
        }
        uint8_t pw = 0;
        for (uint32_t g = 0; g < ng; ) {
            uint8_t k = pick[g][pw];
            nib[nn++] = k;
            if (k < YAPF__NIB_REPEAT) {
                pw = k;
                gw[g++] = k;
            } else {
                for (uint32_t n = 0; n < yapf__nib_groups[k]; n++) gw[g++] = pw;
            }
        }
    }
    for (uint32_t k = 0; k < nn; k += 2)
        o->buf[o->len++] = (uint8_t)(nib[k] | (k + 1 < nn ? nib[k + 1] << 4 : 0));

    /* Packed group data: 8 values of w bits = w bytes */
    for (uint32_t g = 0; g < ng; g++) {
        int      w = gw[g];
        uint64_t acc = 0;
        for (int i = 0; i < YAPF__GROUP; i++)
            acc |= (uint64_t)z[g * YAPF__GROUP + i] << (i * w);
        for (int k = 0; k < w; k++) o->buf[o->len++] = (uint8_t)(acc >> (8 * k));
    }
    return 0;
}

/* Parses one plane payload from [p, end) and unpacks its folded residuals
 * into z; *filt receives the band filters.  Returns the end of the payload
 * or NULL on corrupt input.  All sizes are validated before the unpack. */
static const uint8_t *yapf__unpack_plane(const uint8_t *p, const uint8_t *end,
                                         uint8_t *z, const uint8_t **filt_out,
                                         uint32_t tw, uint32_t th, int simd)
{
    uint32_t total = tw * th;
    uint32_t ng    = (total + YAPF__GROUP - 1) / YAPF__GROUP;
    uint32_t nb    = (th + YAPF__BAND - 1) / YAPF__BAND;
    uint8_t  gw[YAPF__MAX_GROUPS + 64];   /* a final run may overshoot */

    if ((size_t)(end - p) < nb) return NULL;
    const uint8_t *filt = p;
    *filt_out = filt;
    p += nb;
    for (uint32_t b = 0; b < nb; b++)
        if (!yapf__filter_valid(filt[b])) return NULL;

    /* Control nibbles → group widths, and the total data size.  Every
     * nibble stores its width into the next 8 entries with one 64-bit
     * write (the array has room); only runs longer than 8 groups, which
     * are rare, take a branch to fill the rest. */
    size_t   data = 0;
    uint32_t g = 0;
    int      hi = 0;
    uint8_t  w = 0;
    while (g < ng) {
        if (p >= end) return NULL;
        uint8_t  c = (uint8_t)((*p >> (hi * 4)) & 15);
        uint32_t n = yapf__nib_groups[c];
        p  += hi;
        hi ^= 1;
        w   = c < YAPF__NIB_REPEAT ? c : w;
        {
            uint64_t w8 = (uint64_t)w * 0x0101010101010101ull;
            memcpy(gw + g, &w8, 8);
            if (n > 8) memset(gw + g + 8, w, 56);
        }
        data += (size_t)w * n;
        g    += n;
    }
    if (g != ng) return NULL;                  /* run past the last group */
    if (hi) {                                  /* padding nibble must be 0 */
        if (*p >> 4) return NULL;
        p++;
    }
    if ((size_t)(end - p) < data) return NULL;

#if YAPF__SIMD
    if (simd) return yapf__simd_unpack(p, end, gw, ng, z);
#else
    (void)simd;
#endif

    /* Unpack.  Branch-free per group: one 64-bit load, 8 shifts. */
    for (g = 0; g < ng; g++) {
        int      w = gw[g];
        uint64_t x;
        if ((size_t)(end - p) >= 8) {
            x = yapf__r64le(p);
        } else {
            uint8_t tmp[8] = {0};
            memcpy(tmp, p, (size_t)w);
            x = yapf__r64le(tmp);
        }
        uint64_t m = (1u << w) - 1;
        uint8_t *o = z + g * YAPF__GROUP;
        for (int i = 0; i < YAPF__GROUP; i++)
            o[i] = (uint8_t)((x >> (i * w)) & m);
        p += w;
    }

    return p;
}

/* Undo filter f for one row.  R = reconstructed row, U = row above,
 * K = row k above (for YAPF__F_UP + k - 1), Z = folded residuals.  Rows
 * carry one extra sample, R[tw] = R[tw - 1], so "above-right" needs no
 * edge case.  All vertical filters share one loop, pred = (a + b) >> 1:
 *   Up k:      a = b = K          Up-right:  a = b = U + 1
 *   Up-avg:    a = U, b = U + 1
 * which keeps the per-row dispatch down to three targets. */
static void yapf__unfilter_row(uint8_t f, uint8_t *R, const uint8_t *U,
                               const uint8_t *K, const uint8_t *Z, uint32_t tw)
{
    uint32_t x;
    if (f == YAPF__F_LEFT) {
        uint8_t L = 0;
        for (x = 0; x < tw; x++) R[x] = L = (uint8_t)(L + yapf__unzz(Z[x]));
    } else if (f == YAPF__F_GRAD) {
        /* L + (A - D + r): only one add depends on the previous sample. */
        uint8_t L = (uint8_t)(U[0] + yapf__unzz(Z[0]));
        R[0] = L;
        for (x = 1; x < tw; x++) {
            uint8_t t = (uint8_t)(U[x] - U[x - 1] + yapf__unzz(Z[x]));
            R[x] = L = (uint8_t)(L + t);
        }
    } else {
        const uint8_t *a = f >= YAPF__F_UP ? K : f == YAPF__F_UPAVG ? U : U + 1;
        const uint8_t *b = f >= YAPF__F_UP ? K : U + 1;
        for (x = 0; x < tw; x++)
            R[x] = (uint8_t)(((a[x] + b[x]) >> 1) + yapf__unzz(Z[x]));
    }
    R[tw] = R[tw - 1];
}

/* ── Tile encode / decode ────────────────────────────────────────────── */
/*
 * Tile layout (byte aligned):
 *   header byte:  0x80          stored: tw*th*ch raw interleaved bytes follow
 *                 bits 0..3     coded: bit c set = plane c is constant
 *                 bit 4         coded RGB(A): planes are G, R-G, B-G (+ A)
 *                               instead of Y, Co, Cg (+ A)
 *   coded:  one value byte per constant plane, in plane order,
 *           then a plane payload per non-constant plane, in plane order
 * The encoder tries both colour transforms and keeps the smaller tile, and
 * stores the tile raw whenever coding would not make it smaller.
 */
#define YAPF__SUBGREEN    0x10
#define YAPF__ENTROPY     0x20   /* v2: entropy plane mask follows          */
#define YAPF__NOXFORM     0x40   /* v2: planes are R, G, B (+ A) as stored  */

/* ── yapf-sum ────────────────────────────────────────────────────────── */
/*
 * The version 2 checksum.  Four independent 64-bit lanes take the 8-byte
 * little-endian words of the input in turn (word j goes to lane j mod 4;
 * the last word is zero padded):
 *     a = (a ^ w) * K;  a ^= a >> 32;          K = 0x9E3779B97F4A7C15
 * Lane i starts at (i + 1) * K ^ n, n = byte length.  The result is
 *     h = a0 ^ rotl(a1, 16) ^ rotl(a2, 32) ^ rotl(a3, 48);
 *     h *= K;  h ^= h >> 32;  low 32 bits of h.
 * The lanes keep four multiplies in flight, so it runs well above the
 * decoder's own speed.
 */
#define YAPF__K 0x9E3779B97F4A7C15ull

static uint64_t yapf__rotl(uint64_t v, int r) { return (v << r) | (v >> (64 - r)); }

uint32_t yapf_checksum(const void *data, size_t size) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t n = (uint64_t)size;
    uint64_t a[4];
    for (int i = 0; i < 4; i++) a[i] = ((uint64_t)(i + 1) * YAPF__K) ^ n;
    size_t j = 0, words = size / 8;
    for (; j + 4 <= words; j += 4) {
        for (int i = 0; i < 4; i++) {
            uint64_t v = (a[i] ^ yapf__r64le(p + (j + i) * 8)) * YAPF__K;
            a[i] = v ^ (v >> 32);
        }
    }
    for (; j < words; j++) {
        uint64_t v = (a[j & 3] ^ yapf__r64le(p + j * 8)) * YAPF__K;
        a[j & 3] = v ^ (v >> 32);
    }
    if (size & 7) {
        uint8_t tail[8] = {0};
        memcpy(tail, p + words * 8, size & 7);
        uint64_t v = (a[j & 3] ^ yapf__r64le(tail)) * YAPF__K;
        a[j & 3] = v ^ (v >> 32);
    }
    uint64_t h = a[0] ^ yapf__rotl(a[1], 16) ^ yapf__rotl(a[2], 32) ^ yapf__rotl(a[3], 48);
    h *= YAPF__K;
    h ^= h >> 32;
    return (uint32_t)h;
}

static uint16_t yapf__r16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void yapf__w16le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void yapf__w64le(uint8_t *p, uint64_t v) {
    yapf__w32le(p, (uint32_t)v);
    yapf__w32le(p + 4, (uint32_t)(v >> 32));
}

/* ── Entropy stage (version 2, optional) ─────────────────────────────── */
/*
 * A surface that uses it starts with a code book: Huffman tables keyed by
 * (slot, context).  Pixel planes: slot = transform · 16 + plane (transform
 * 0 YCoCg-R, 1 subtract-green, 2 none), context = how busy the neighbours
 * are (folded residuals left + above, in 7 classes).  Block tiles: slot =
 * byte lane of the block, context 0.  Sharing the tables per surface
 * keeps tiles independent and their headers tiny.
 *
 * Code table:  1 byte n − 1 (n = symbols 0..n−1 described), then n 4-bit
 *              code lengths (0 = unused, 1..11), low nibble first; an odd
 *              count is padded with a zero nibble.  Kraft sum <= 1.
 * Codes are canonical (shorter first, then by symbol, as in deflate) and
 * written least significant bit first, so a decoder reads them with one
 * 2048-entry table lookup.
 */
#define YAPF__HUFF_BITS 11
#define YAPF__HUFF_SIZE (1u << YAPF__HUFF_BITS)

static uint32_t yapf__bitrev(uint32_t c, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) { r = (r << 1) | (c & 1); c >>= 1; }
    return r;
}

/* Parses a code table into lut (entry = length << 8 | symbol, 0 = no
 * code).  Returns the byte after the table, or NULL. */
static const uint8_t *yapf__huff_table(const uint8_t *p, const uint8_t *end,
                                       uint16_t *lut)
{
    if (p >= end) return NULL;
    uint32_t n = (uint32_t)*p++ + 1, nbytes = (n + 1) / 2;
    if ((size_t)(end - p) < nbytes) return NULL;
    uint8_t  len[256];
    uint32_t count[YAPF__HUFF_BITS + 1] = {0}, kraft = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t l = (uint8_t)((p[i >> 1] >> ((i & 1) * 4)) & 15);
        if (l > YAPF__HUFF_BITS) return NULL;
        len[i] = l;
        count[l]++;
        if (l) kraft += YAPF__HUFF_SIZE >> l;
    }
    if ((n & 1) && (p[n >> 1] >> 4)) return NULL;
    if (kraft == 0 || kraft > YAPF__HUFF_SIZE) return NULL;
    p += nbytes;

    uint32_t next[YAPF__HUFF_BITS + 2], code = 0;
    count[0] = 0;
    for (int l = 1; l <= YAPF__HUFF_BITS; l++) {
        code = (code + count[l - 1]) << 1;
        next[l] = code;
    }
    memset(lut, 0, YAPF__HUFF_SIZE * sizeof(uint16_t));
    for (uint32_t s = 0; s < n; s++) {
        int l = len[s];
        if (!l) continue;
        uint32_t r = yapf__bitrev(next[l]++, l);
        for (uint32_t k = r; k < YAPF__HUFF_SIZE; k += 1u << l)
            lut[k] = (uint16_t)((l << 8) | s);
    }
    return p;
}

/* Bit reader for code streams; reading past the end yields zeros and is
 * caught by yapf__br_done. */
typedef struct {
    const uint8_t *p;
    size_t         L, pos;
    uint64_t       buf, used;
    int            bits;
} yapf__br;

static void yapf__br_init(yapf__br *b, const uint8_t *p, size_t L) {
    b->p = p; b->L = L; b->pos = 0; b->buf = 0; b->used = 0; b->bits = 0;
}

static int yapf__br_sym(yapf__br *b, const uint16_t *lut) {
    if (b->bits < (int)YAPF__HUFF_BITS) {
        if (b->pos + 8 <= b->L) {
            b->buf |= yapf__r64le(b->p + b->pos) << b->bits;
            b->pos += (size_t)((63 - b->bits) >> 3);
            b->bits |= 56;
        } else {
            while (b->bits <= 56) {
                b->buf |= (uint64_t)(b->pos < b->L ? b->p[b->pos] : 0) << b->bits;
                b->pos++;
                b->bits += 8;
            }
        }
    }
    uint16_t e = lut[b->buf & (YAPF__HUFF_SIZE - 1)];
    if (!e) return -1;
    int l = e >> 8;
    b->buf >>= l;
    b->bits -= l;
    b->used += (uint64_t)l;
    return e & 0xFF;
}

/* The stream must end in its last byte. */
static int yapf__br_done(const yapf__br *b) {
    return (b->used + 7) / 8 == b->L;
}

#define YAPF__SLOTS  48
#define YAPF__CTX    7

/* Context of a residual from its left and upper neighbours' folded
 * residuals s = l + u: 0, 1–2, 3–6, 7–14, 15–30, 31–62, 63 and up
 * (= min(6, floor(log2(s + 1)))).  Branch free: the decoder computes it
 * on the critical path of every sample. */
static int yapf__ctx(uint32_t s) {
    return (s > 0) + (s > 2) + (s > 6) + (s > 14) + (s > 30) + (s > 62);
}

/* The same as table offsets for the decoder: ctx(min(s, 63)) << 11. */
#define YAPF__C(c) (uint16_t)((c) << 11)
static const uint16_t yapf__ctx_off[64] = {   /* index min(s, 63) */
    YAPF__C(0), YAPF__C(1), YAPF__C(1), YAPF__C(2), YAPF__C(2), YAPF__C(2), YAPF__C(2),
    YAPF__C(3), YAPF__C(3), YAPF__C(3), YAPF__C(3), YAPF__C(3), YAPF__C(3), YAPF__C(3), YAPF__C(3),
    YAPF__C(4), YAPF__C(4), YAPF__C(4), YAPF__C(4), YAPF__C(4), YAPF__C(4), YAPF__C(4), YAPF__C(4),
    YAPF__C(4), YAPF__C(4), YAPF__C(4), YAPF__C(4), YAPF__C(4), YAPF__C(4), YAPF__C(4), YAPF__C(4),
    YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5),
    YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5),
    YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5),
    YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5), YAPF__C(5),
    YAPF__C(6)
};
#undef YAPF__C

/* Decoder side of a code book: per slot one array of 7 × 2048 lookup
 * entries (contexts without a table stay 0 = invalid), so context and
 * bits form a single index. */
typedef struct {
    uint16_t *slot[YAPF__SLOTS];
    uint16_t *mem;
} yapf__dbook;

/* Book: u16 table count, then per table: slot, context, code table. */
static int yapf__book_read(const uint8_t *p, size_t len, yapf__dbook *bk) {
    const uint8_t *end = p + len, *q;
    uint8_t        seen[YAPF__SLOTS][YAPF__CTX];
    uint32_t       slots = 0;
    memset(bk, 0, sizeof(*bk));
    memset(seen, 0, sizeof(seen));
    if (len < 2) return -1;
    uint32_t n = yapf__r16le(p);
    p += 2;
    if (n == 0 || n > YAPF__SLOTS * YAPF__CTX) return -1;
    /* first pass: validate the layout and count the slots in use */
    for (uint32_t i = 0, k; i < n; i++) {
        if (end - p < 3) return -1;
        uint8_t slot = p[0], ctx = p[1];
        if (slot >= YAPF__SLOTS || ctx >= YAPF__CTX || seen[slot][ctx]) return -1;
        seen[slot][ctx] = 1;
        for (k = 0; k < YAPF__CTX && (k == ctx || !seen[slot][k]); k++) {}
        if (k == YAPF__CTX) slots++;               /* first table of this slot */
        uint32_t ns = (uint32_t)p[2] + 1;
        p += 3 + (ns + 1) / 2;
        if (p > end) return -1;
    }
    if (p != end) return -1;
    bk->mem = (uint16_t *)calloc((size_t)slots * YAPF__CTX * YAPF__HUFF_SIZE, sizeof(uint16_t));
    if (!bk->mem) return -1;
    uint32_t used = 0;
    for (q = end - len + 2; q < end; ) {
        uint8_t slot = q[0], ctx = q[1];
        if (!bk->slot[slot]) bk->slot[slot] = bk->mem + (size_t)used++ * YAPF__CTX * YAPF__HUFF_SIZE;
        q = yapf__huff_table(q + 2, end, bk->slot[slot] + (size_t)ctx * YAPF__HUFF_SIZE);
        if (!q) return -1;
    }
    return 0;
}

/* Entropy plane payload: filters, u16 stream length, the folded
 * residuals in raster order, each with the table of its context.
 *
 * Every residual's context needs the one decoded just before it, so one
 * stream is a chain of dependent steps.  The planes of a tile are
 * independent streams, though: the decoder runs up to four of them in
 * the same loop and the CPU overlaps their chains. */
typedef struct {
    const uint8_t  *p;
    size_t          L;
    const uint16_t *lut;
    uint8_t        *z;
} yapf__hstream;

/* Parses the header of an entropy plane into *s; returns the end of the
 * payload or NULL. */
static const uint8_t *yapf__huff_head(const uint8_t *p, const uint8_t *end,
                                      const uint8_t **filt_out, uint32_t th,
                                      const yapf__dbook *bk, int slot,
                                      uint8_t *z, yapf__hstream *s)
{
    uint32_t nb = (th + YAPF__BAND - 1) / YAPF__BAND;
    if (!bk || !bk->slot[slot] || (size_t)(end - p) < nb + 2) return NULL;
    *filt_out = p;
    for (uint32_t b = 0; b < nb; b++)
        if (!yapf__filter_valid(p[b])) return NULL;
    p += nb;
    s->L = yapf__r16le(p);
    p += 2;
    if ((size_t)(end - p) < s->L) return NULL;
    s->p = p;
    s->lut = bk->slot[slot];
    s->z = z;
    return p + s->L;
}

#define YAPF__HSTATE(k)                                                     \
    uint64_t buf##k = 0;                                                    \
    int bits##k = 0;                                                        \
    size_t pos##k = 0;                                                      \
    uint32_t l##k = 0;                                                      \
    const uint8_t *P##k = s[k].p, *U##k = yapf__zero_row;                   \
    const size_t L##k = s[k].L;                                             \
    const uint16_t *T##k = s[k].lut;                                        \
    uint8_t *Z##k = s[k].z;

#define YAPF__HROW(k)                                                       \
    U##k = y ? Z##k + i - tw : yapf__zero_row;                              \
    l##k = 0;

#define YAPF__HSTEP(k)                                                      \
    {                                                                       \
        if (pos##k + 8 <= L##k) {           /* branch free while not near the end */ \
            buf##k |= yapf__r64le(P##k + pos##k) << bits##k;                \
            pos##k += (size_t)((63 - bits##k) >> 3);                        \
            bits##k |= 56;                                                  \
        } else if (bits##k < (int)YAPF__HUFF_BITS) {                        \
            while (bits##k <= 56) {                                         \
                buf##k |= (uint64_t)(pos##k < L##k ? P##k[pos##k] : 0) << bits##k; \
                pos##k++;                                                   \
                bits##k += 8;                                               \
            }                                                               \
        }                                                                   \
        uint32_t s_ = l##k + U##k[x];                                       \
        uint32_t e_ = T##k[yapf__ctx_off[s_ < 63 ? s_ : 63] |               \
                           (uint32_t)(buf##k & (YAPF__HUFF_SIZE - 1))];     \
        int n_ = (int)(e_ >> 8);                                            \
        if (!e_) return -1;                                                 \
        buf##k >>= n_;                                                      \
        bits##k -= n_;                                                      \
        l##k = e_ & 0xFF;                                                   \
        Z##k[i] = (uint8_t)l##k;                                            \
    }

/* bits consumed = 8·pos − bits still buffered; the stream must end in
 * its last byte */
#define YAPF__HEND(k) if ((8 * (uint64_t)pos##k - (uint64_t)bits##k + 7) / 8 != L##k) return -1;

static int yapf__hdec1(const yapf__hstream *s, uint32_t tw, uint32_t th) {
    YAPF__HSTATE(0)
    for (uint32_t y = 0, i = 0; y < th; y++) {
        YAPF__HROW(0)
        for (uint32_t x = 0; x < tw; x++, i++) YAPF__HSTEP(0)
    }
    YAPF__HEND(0)
    return 0;
}

static int yapf__hdec2(const yapf__hstream *s, uint32_t tw, uint32_t th) {
    YAPF__HSTATE(0) YAPF__HSTATE(1)
    for (uint32_t y = 0, i = 0; y < th; y++) {
        YAPF__HROW(0) YAPF__HROW(1)
        for (uint32_t x = 0; x < tw; x++, i++) { YAPF__HSTEP(0) YAPF__HSTEP(1) }
    }
    YAPF__HEND(0) YAPF__HEND(1)
    return 0;
}

static int yapf__hdec3(const yapf__hstream *s, uint32_t tw, uint32_t th) {
    YAPF__HSTATE(0) YAPF__HSTATE(1) YAPF__HSTATE(2)
    for (uint32_t y = 0, i = 0; y < th; y++) {
        YAPF__HROW(0) YAPF__HROW(1) YAPF__HROW(2)
        for (uint32_t x = 0; x < tw; x++, i++) { YAPF__HSTEP(0) YAPF__HSTEP(1) YAPF__HSTEP(2) }
    }
    YAPF__HEND(0) YAPF__HEND(1) YAPF__HEND(2)
    return 0;
}

static int yapf__hdec4(const yapf__hstream *s, uint32_t tw, uint32_t th) {
    YAPF__HSTATE(0) YAPF__HSTATE(1) YAPF__HSTATE(2) YAPF__HSTATE(3)
    for (uint32_t y = 0, i = 0; y < th; y++) {
        YAPF__HROW(0) YAPF__HROW(1) YAPF__HROW(2) YAPF__HROW(3)
        for (uint32_t x = 0; x < tw; x++, i++) {
            YAPF__HSTEP(0) YAPF__HSTEP(1) YAPF__HSTEP(2) YAPF__HSTEP(3)
        }
    }
    YAPF__HEND(0) YAPF__HEND(1) YAPF__HEND(2) YAPF__HEND(3)
    return 0;
}

/* Decodes n (1..) streams, four at a time; zeroes each tail group. */
static int yapf__huff_run(const yapf__hstream *s, int n, uint32_t tw, uint32_t th) {
    for (int k = 0; k < n; k += 4) {
        int m = n - k < 4 ? n - k : 4, rc;
        rc = m == 4 ? yapf__hdec4(s + k, tw, th) : m == 3 ? yapf__hdec3(s + k, tw, th)
           : m == 2 ? yapf__hdec2(s + k, tw, th) : yapf__hdec1(s + k, tw, th);
        if (rc != 0) return -1;
    }
    for (int k = 0; k < n; k++) memset(s[k].z + (size_t)tw * th, 0, YAPF__GROUP);
    return 0;
}

/* One entropy plane on its own. */
static const uint8_t *yapf__huff_plane(const uint8_t *p, const uint8_t *end,
                                       uint8_t *z, const uint8_t **filt_out,
                                       uint32_t tw, uint32_t th,
                                       const yapf__dbook *bk, int slot)
{
    yapf__hstream s;
    p = yapf__huff_head(p, end, filt_out, th, bk, slot, z, &s);
    if (!p || yapf__huff_run(&s, 1, tw, th) != 0) return NULL;
    return p;
}

/* Code lengths (<= 11 bits) for freq[0..n).  Plain Huffman by the
 * two-queue method; if a code comes out too long the counts are halved
 * and it is built again. */
static void yapf__huff_lengths(const uint32_t *freq_in, uint32_t n, uint8_t *len) {
    uint32_t freq[256], sym[256], m = 0;
    memcpy(freq, freq_in, n * sizeof(uint32_t));
    for (;;) {
        m = 0;
        for (uint32_t s = 0; s < n; s++) { len[s] = 0; if (freq[s]) sym[m++] = s; }
        if (m == 0) return;
        if (m == 1) { len[sym[0]] = 1; return; }
        /* insertion sort by frequency (m <= 256) */
        for (uint32_t i = 1; i < m; i++) {
            uint32_t v = sym[i], j = i;
            while (j > 0 && freq[sym[j - 1]] > freq[v]) { sym[j] = sym[j - 1]; j--; }
            sym[j] = v;
        }
        /* nodes 0..m-1 leaves (sorted), m..2m-2 internal in creation order */
        uint64_t w[512];
        uint32_t parent[512], li = 0, ii = m, nn = m;
        for (uint32_t i = 0; i < m; i++) w[i] = freq[sym[i]];
        for (uint32_t k = 0; k + 1 < m; k++) {
            uint32_t pick[2];
            for (int t = 0; t < 2; t++) {
                if (li < m && (ii >= nn || w[li] <= w[ii])) pick[t] = li++;
                else pick[t] = ii++;
            }
            w[nn] = w[pick[0]] + w[pick[1]];
            parent[pick[0]] = parent[pick[1]] = nn;
            nn++;
        }
        uint8_t depth[512];
        depth[nn - 1] = 0;
        int maxl = 0;
        for (uint32_t k = nn - 1; k-- > 0; ) {
            depth[k] = (uint8_t)(depth[parent[k]] + 1);
            if (k < m && depth[k] > maxl) maxl = depth[k];
        }
        if (maxl <= (int)YAPF__HUFF_BITS) {
            for (uint32_t i = 0; i < m; i++) len[sym[i]] = depth[i];
            return;
        }
        for (uint32_t s = 0; s < n; s++) if (freq[s]) freq[s] = (freq[s] >> 1) | 1;
    }
}

typedef struct {
    uint64_t acc;
    int      bits;
    yapf__out *o;
} yapf__bitw;

static void yapf__bitw_put(yapf__bitw *b, uint32_t code, int len) {
    b->acc |= (uint64_t)code << b->bits;
    b->bits += len;
    while (b->bits >= 8) {
        b->o->buf[b->o->len++] = (uint8_t)b->acc;
        b->acc >>= 8;
        b->bits -= 8;
    }
}

static void yapf__bitw_flush(yapf__bitw *b) {
    if (b->bits > 0) b->o->buf[b->o->len++] = (uint8_t)b->acc;
    b->acc = 0;
    b->bits = 0;
}

/* Canonical codes for lengths len[0..ns). */
static void yapf__huff_codes(const uint8_t *len, uint32_t ns, uint16_t *code) {
    uint32_t count[YAPF__HUFF_BITS + 1] = {0}, next[YAPF__HUFF_BITS + 2], c = 0;
    for (uint32_t s = 0; s < ns; s++) if (len[s]) count[len[s]]++;
    for (int l = 1; l <= (int)YAPF__HUFF_BITS; l++) {
        c = (c + count[l - 1]) << 1;
        next[l] = c;
    }
    for (uint32_t s = 0; s < ns; s++)
        code[s] = len[s] ? (uint16_t)yapf__bitrev(next[len[s]]++, len[s]) : 0;
}

typedef struct {                    /* encoder side of a code book */
    uint32_t freq[YAPF__SLOTS][YAPF__CTX][256];
    uint8_t  len[YAPF__SLOTS][YAPF__CTX][256];
    uint16_t code[YAPF__SLOTS][YAPF__CTX][256];
} yapf__ebook;

/* Encoder modes for the tile writers. */
#define YAPF__ENC_PLAIN  0          /* groups only                          */
#define YAPF__ENC_STATS  1          /* groups, and count symbols for a book */
#define YAPF__ENC_BOOK   2          /* entropy planes / packed blocks       */

/* Builds every table that has symbols and writes the book. */
static int yapf__book_write(yapf__ebook *eb, yapf__out *o) {
    uint32_t n = 0;
    if (yapf__reserve(o, 2) != 0) return -1;
    size_t at = o->len;
    o->len += 2;
    memset(eb->len, 0, sizeof(eb->len));
    for (int s = 0; s < YAPF__SLOTS; s++)
        for (int c = 0; c < YAPF__CTX; c++) {
            const uint32_t *f = eb->freq[s][c];
            uint32_t ns = 256;
            while (ns > 0 && !f[ns - 1]) ns--;
            if (ns == 0) continue;
            yapf__huff_lengths(f, ns, eb->len[s][c]);
            yapf__huff_codes(eb->len[s][c], ns, eb->code[s][c]);
            if (yapf__reserve(o, 3 + 128) != 0) return -1;
            o->buf[o->len++] = (uint8_t)s;
            o->buf[o->len++] = (uint8_t)c;
            o->buf[o->len++] = (uint8_t)(ns - 1);
            for (uint32_t k = 0; k < ns; k += 2)
                o->buf[o->len++] = (uint8_t)(eb->len[s][c][k] | (k + 1 < ns ? eb->len[s][c][k + 1] << 4 : 0));
            n++;
        }
    yapf__w16le(o->buf + at, n);
    return n ? 0 : 1;               /* 1: nothing to code */
}

static void yapf__plane_stats(yapf__ebook *eb, int slot, const uint8_t *z, uint32_t tw, uint32_t th) {
    for (uint32_t y = 0, i = 0; y < th; y++)
        for (uint32_t x = 0; x < tw; x++, i++) {
            uint32_t s = (x ? z[i - 1] : 0u) + (y ? z[i - tw] : 0u);
            eb->freq[slot][yapf__ctx(s)][z[i]]++;
        }
}

/* Entropy plane payload (see yapf__huff_plane).  Returns 1 when some
 * symbol has no code in the book (the caller keeps the group form). */
static int yapf__huff_emit_plane(yapf__out *o, const uint8_t *z, const uint8_t *filt,
                                 uint32_t tw, uint32_t th, const yapf__ebook *eb, int slot)
{
    uint32_t nb = (th + YAPF__BAND - 1) / YAPF__BAND;
    uint64_t bits = 0;
    for (uint32_t y = 0, i = 0; y < th; y++)
        for (uint32_t x = 0; x < tw; x++, i++) {
            uint32_t s = (x ? z[i - 1] : 0u) + (y ? z[i - tw] : 0u);
            uint8_t  l = eb->len[slot][yapf__ctx(s)][z[i]];
            if (!l) return 1;
            bits += l;
        }
    size_t L = (size_t)((bits + 7) / 8);
    if (L > 0xFFFF) return 1;
    if (yapf__reserve(o, nb + 2 + L + 8) != 0) return -1;
    memcpy(o->buf + o->len, filt, nb);
    o->len += nb;
    yapf__w16le(o->buf + o->len, (uint32_t)L);
    o->len += 2;
    yapf__bitw bw = { 0, 0, o };
    for (uint32_t y = 0, i = 0; y < th; y++)
        for (uint32_t x = 0; x < tw; x++, i++) {
            uint32_t s = (x ? z[i - 1] : 0u) + (y ? z[i - tw] : 0u);
            int      c = yapf__ctx(s);
            yapf__bitw_put(&bw, eb->code[slot][c][z[i]], eb->len[slot][c][z[i]]);
        }
    yapf__bitw_flush(&bw);
    return 0;
}

/* ── Plane set emission (version 2) ──────────────────────────────────── */
/*
 * Writes a coded v2 tile: header byte, (wide) u16 constant mask, entropy
 * mask if any plane is entropy coded, constant values, payloads.  With a
 * book (mode BOOK) each plane is coded both ways and the entropy form is
 * kept only if it is at least 1/32 smaller; mode STATS feeds the book.
 */
typedef struct {
    int            np;          /* planes */
    int            wide;        /* masks are u16 (wide samples)            */
    uint32_t       cmask;       /* constant planes                         */
    const uint8_t *cval;        /* constant value per plane                */
    const uint8_t *z[16];       /* folded residuals per plane              */
    const uint8_t *filt[16];
} yapf__planeset;

static int yapf__emit_planeset(yapf__out *o, yapf__out *tmp, uint8_t hdr,
                               const yapf__planeset *ps, uint32_t tw, uint32_t th,
                               int mode, yapf__ebook *eb)
{
    size_t   off[17];
    uint32_t emask = 0;
    int      tsel = (hdr & YAPF__SUBGREEN) ? 1 : (hdr & YAPF__NOXFORM) ? 2 : 0;
    tmp->len = 0;
    for (int c = 0; c < ps->np; c++) {
        off[c] = tmp->len;
        if (ps->cmask & (1u << c)) continue;
        if (yapf__emit_plane(tmp, ps->z[c], ps->filt[c], tw, th) != 0) return -1;
        if (mode == YAPF__ENC_STATS) yapf__plane_stats(eb, tsel * 16 + c, ps->z[c], tw, th);
        if (mode != YAPF__ENC_BOOK) continue;
        size_t gsz = tmp->len - off[c];
        int    r = yapf__huff_emit_plane(tmp, ps->z[c], ps->filt[c], tw, th, eb, tsel * 16 + c);
        if (r < 0) return -1;
        size_t hsz = r ? (size_t)-1 : tmp->len - off[c] - gsz;
        if (!r && hsz + gsz / 32 < gsz) {
            memmove(tmp->buf + off[c], tmp->buf + off[c] + gsz, hsz);
            tmp->len = off[c] + hsz;
            emask |= 1u << c;
        } else {
            tmp->len = off[c] + gsz;
        }
    }
    if (yapf__reserve(o, 5 + (size_t)ps->np + tmp->len) != 0) return -1;
    o->buf[o->len++] = (uint8_t)(hdr | (emask ? YAPF__ENTROPY : 0));
    if (ps->wide) { yapf__w16le(o->buf + o->len, ps->cmask); o->len += 2; }
    if (emask) {
        if (ps->wide) { yapf__w16le(o->buf + o->len, emask); o->len += 2; }
        else o->buf[o->len++] = (uint8_t)emask;
    }
    for (int c = 0; c < ps->np; c++)
        if (ps->cmask & (1u << c)) o->buf[o->len++] = ps->cval[c];
    if (tmp->len) memcpy(o->buf + o->len, tmp->buf, tmp->len);
    o->len += tmp->len;
    return 0;
}

/* ── Wide samples (version 2) ────────────────────────────────────────── */
/*
 * 16- and 32-bit samples are coded as byte planes with the 8-bit
 * machinery.  Floats first become order-preserving integers (sign set:
 * all bits flipped; else the sign bit set), so nearby values stay nearby.
 * The colour transforms are the 8-bit ones at the sample width (mod 2^16
 * or 2^32); a third choice keeps R, G, B as they are.  Channel c gives
 * planes c·B .. c·B + B − 1, most significant byte first, so the smooth
 * high bytes and the noisy low bytes each get their own filters.
 */
#define YAPF__MAX_PLANES 16
#define YAPF__ROW        (YAPF_TILE_SIZE + 1)

static size_t yapf__sbytes(int sample) {
    return sample == YAPF_SAMPLE_U8 ? 1 : sample == YAPF_SAMPLE_F32 ? 4 : 2;
}

static uint32_t yapf__vmask(int bits) {
    return bits == 32 ? 0xFFFFFFFFu : (1u << bits) - 1;
}

static uint32_t yapf__fmap(uint32_t v, int bits) {
    uint32_t s = 1u << (bits - 1);
    return (v & s) ? (~v & yapf__vmask(bits)) : (v | s);
}

static uint32_t yapf__funmap(uint32_t v, int bits) {
    uint32_t s = 1u << (bits - 1);
    return (v & s) ? (v ^ s) : (~v & yapf__vmask(bits));
}

/* Arithmetic shift right by one of a bits-wide two's complement value. */
static uint32_t yapf__sar1(uint32_t v, int bits) {
    return ((v >> 1) | (v & (1u << (bits - 1)))) & yapf__vmask(bits);
}

static uint32_t yapf__get_sample(const uint8_t *p, int sample) {
    if (sample == YAPF_SAMPLE_U8) return *p;
    if (sample == YAPF_SAMPLE_F32) { uint32_t v; memcpy(&v, p, 4); return v; }
    uint16_t v; memcpy(&v, p, 2); return v;
}

static void yapf__put_sample(uint8_t *p, int sample, uint32_t v) {
    if (sample == YAPF_SAMPLE_U8) { *p = (uint8_t)v; return; }
    if (sample == YAPF_SAMPLE_F32) { memcpy(p, &v, 4); return; }
    uint16_t h = (uint16_t)v;
    memcpy(p, &h, 2);
}

typedef struct {
    uint8_t plane[2][YAPF__MAX_PLANES][YAPF__TILE_PX];
    uint8_t z[2][YAPF__MAX_PLANES][YAPF__TILE_PX + YAPF__GROUP];
    uint8_t filt[2][YAPF__MAX_PLANES][YAPF_TILE_SIZE / YAPF__BAND];
    uint32_t val[4][YAPF__TILE_PX];
} yapf__wscratch;

static int yapf__encode_tile_wide(
    yapf__out *o, yapf__out *tmp, yapf__wscratch *sc,
    const uint8_t *pixels, uint32_t img_w,
    uint32_t tx, uint32_t ty, uint32_t tw, uint32_t th,
    int ch, int sample, int mode, yapf__ebook *eb)
{
    int      sb    = (int)yapf__sbytes(sample), bits = 8 * sb;
    int      isf   = sample == YAPF_SAMPLE_F16 || sample == YAPF_SAMPLE_F32;
    int      np    = ch * sb;
    uint32_t total = tw * th, half = 1u << (bits - 1), m = yapf__vmask(bits);
    size_t   start = o->len, raw = (size_t)total * ch * sb;
    int      tries = ch >= 3 ? 3 : 1, cur = 0;
    uint32_t best_est = UINT32_MAX, cmask[2] = { 0, 0 };
    uint8_t  cval[2][YAPF__MAX_PLANES];
    uint8_t  xfs[2] = { 0, 0 };

    /* t = 0: YCoCg-R, 1: subtract-green, 2: none (only with 3+ channels;
     * with 1–2 channels the planes are the channels). */
    for (int t = 0; t < tries; t++) {
        for (uint32_t y = 0; y < th; y++) {
            const uint8_t *row = pixels + ((size_t)(ty + y) * img_w + tx) * (size_t)ch * sb;
            for (uint32_t x = 0; x < tw; x++) {
                uint32_t v[4], i = y * tw + x;
                for (int c = 0; c < ch; c++) {
                    v[c] = yapf__get_sample(row + ((size_t)x * ch + c) * sb, sample);
                    if (isf) v[c] = yapf__fmap(v[c], bits);
                }
                if (ch >= 3 && t == 0) {
                    uint32_t r = v[0], g = v[1], b = v[2];
                    uint32_t o_ = (r - b) & m, t_ = (b + yapf__sar1(o_, bits)) & m;
                    uint32_t q  = (g - t_) & m;
                    v[0] = (t_ + yapf__sar1(q, bits)) & m;
                    v[1] = (o_ + half) & m;
                    v[2] = (q + half) & m;
                } else if (ch >= 3 && t == 1) {
                    uint32_t r = v[0], g = v[1], b = v[2];
                    v[0] = g;
                    v[1] = (r - g + half) & m;
                    v[2] = (b - g + half) & m;
                }
                for (int c = 0; c < ch; c++) sc->val[c][i] = v[c];
            }
        }
        /* planes, constant mask, filters, size estimate */
        uint8_t (*plane)[YAPF__TILE_PX] = sc->plane[cur];
        uint32_t est = 0, cm = 0;
        for (int c = 0; c < ch; c++)
            for (int k = 0; k < sb; k++) {
                int      pl = c * sb + k, sh = 8 * (sb - 1 - k);
                uint8_t *P  = plane[pl];
                for (uint32_t i = 0; i < total; i++) P[i] = (uint8_t)(sc->val[c][i] >> sh);
                uint32_t i = 1;
                while (i < total && P[i] == P[0]) i++;
                if (i == total) { cm |= 1u << pl; cval[cur][pl] = P[0]; est += 16; continue; }
                est += yapf__choose_filters(P, tw, th, sc->z[cur][pl], sc->filt[cur][pl]);
            }
        if (est < best_est) {
            best_est = est;
            cmask[cur] = cm;
            xfs[cur] = (uint8_t)t;
            cur ^= 1;      /* keep this set, work in the other one */
        }
    }
    int keep = cur ^ 1;
    uint8_t hdr = ch < 3 ? 0 : xfs[keep] == 1 ? YAPF__SUBGREEN : xfs[keep] == 2 ? YAPF__NOXFORM : 0;

    yapf__planeset ps;
    memset(&ps, 0, sizeof(ps));
    ps.np = np;
    ps.wide = 1;
    ps.cmask = cmask[keep];
    ps.cval = cval[keep];
    for (int c = 0; c < np; c++) { ps.z[c] = sc->z[keep][c]; ps.filt[c] = sc->filt[keep][c]; }
    if (yapf__emit_planeset(o, tmp, hdr, &ps, tw, th, mode, eb) != 0) return -1;

    if (o->len - start > raw + 1) {           /* stored: raw LE samples */
        o->len = start;
        if (yapf__reserve(o, raw + 1) != 0) return -1;
        o->buf[o->len++] = YAPF__STORED;
        for (uint32_t y = 0; y < th; y++) {
            const uint8_t *row = pixels + ((size_t)(ty + y) * img_w + tx) * (size_t)ch * sb;
            for (uint32_t i = 0; i < tw * (uint32_t)ch; i++) {
                uint32_t v = yapf__get_sample(row + (size_t)i * sb, sample);
                for (int k = 0; k < sb; k++) o->buf[o->len++] = (uint8_t)(v >> (8 * k));
            }
        }
    }
    return 0;
}

typedef struct {
    uint8_t plane[YAPF__MAX_PLANES][YAPF_TILE_SIZE * YAPF__ROW];
    uint8_t z[YAPF__TILE_PX + YAPF_TILE_SIZE];
} yapf__wdec;

static int yapf__decode_tile_wide(
    const uint8_t *data, size_t len,
    uint8_t *pixels, uint32_t img_w,
    uint32_t tx, uint32_t ty, uint32_t tw, uint32_t th,
    int ch, int sample, int simd, const yapf__dbook *bk, yapf__wdec *sc)
{
    int            sb = (int)yapf__sbytes(sample), bits = 8 * sb, np = ch * sb;
    int            isf = sample == YAPF_SAMPLE_F16 || sample == YAPF_SAMPLE_F32;
    uint32_t       half = 1u << (bits - 1), m = yapf__vmask(bits);
    const uint8_t *p = data, *end = data + len;
    size_t         rowb = (size_t)tw * ch * sb;

    if (len == 0) return -1;
    uint8_t hdr = *p++;
    if (hdr == YAPF__STORED) {
        if (len != 1 + rowb * th) return -1;
        for (uint32_t y = 0; y < th; y++) {
            uint8_t *d = pixels + ((size_t)(ty + y) * img_w + tx) * (size_t)ch * sb;
            for (uint32_t i = 0; i < tw * (uint32_t)ch; i++, p += sb) {
                uint32_t v = 0;
                for (int k = sb; k-- > 0; ) v = (v << 8) | p[k];
                yapf__put_sample(d + (size_t)i * sb, sample, v);
            }
        }
        return 0;
    }
    if (hdr & 0x8F) return -1;
    if ((hdr & YAPF__NOXFORM) && (hdr & YAPF__SUBGREEN)) return -1;
    if (ch < 3 && (hdr & (YAPF__NOXFORM | YAPF__SUBGREEN))) return -1;
    int xf = (hdr & YAPF__NOXFORM) ? 2 : (hdr & YAPF__SUBGREEN) ? 1 : 0;
    if (ch < 3) xf = 2;

    if (end - p < 2) return -1;
    uint32_t cmask = yapf__r16le(p), emask = 0;
    p += 2;
    if (cmask >> np) return -1;
    if (hdr & YAPF__ENTROPY) {
        if (end - p < 2 || !bk) return -1;
        emask = yapf__r16le(p);
        p += 2;
        if ((emask >> np) || (emask & cmask)) return -1;
    }
    for (int pl = 0; pl < np; pl++) {
        if (!(cmask & (1u << pl))) continue;
        if (p >= end) return -1;
        memset(sc->plane[pl], *p++, sizeof(sc->plane[pl]));
    }
    for (int pl = 0; pl < np; pl++) {
        if (cmask & (1u << pl)) continue;
        const uint8_t *filt = NULL;
        memset(sc->z + (size_t)tw * th, 0, sizeof(sc->z) - (size_t)tw * th);
        p = (emask & (1u << pl))
          ? yapf__huff_plane(p, end, sc->z, &filt, tw, th, bk, (ch < 3 ? 0 : xf) * 16 + pl)
          : yapf__unpack_plane(p, end, sc->z, &filt, tw, th, simd);
        if (!p) return -1;
        if (tw != YAPF_TILE_SIZE) {
            size_t used = ((size_t)tw * th + YAPF__GROUP - 1) / YAPF__GROUP * YAPF__GROUP;
            memset(sc->z + used, 0, sizeof(sc->z) - used);
        }
        uint8_t *P = sc->plane[pl];
        for (uint32_t y = 0; y < th; y++) {
            uint8_t  f = filt[y / YAPF__BAND];
            uint32_t k = f >= YAPF__F_UP ? (uint32_t)(f - YAPF__F_UP + 1) : 1;
            uint8_t       *R = P + (size_t)y * YAPF__ROW;
            const uint8_t *U = y ? R - YAPF__ROW : yapf__zero_row;
            const uint8_t *K = y >= k ? R - (size_t)k * YAPF__ROW : yapf__zero_row;
            const uint8_t *Z = sc->z + (size_t)y * tw;
#if YAPF__SIMD
            if (simd) {
                if (f == YAPF__F_LEFT || f == YAPF__F_GRAD)
                    yapf__simd_serial_row(f, R, U, Z, tw);
                else if (f >= YAPF__F_UP)
                    yapf__simd_vertical_row(R, K, K, Z, tw);
                else if (f == YAPF__F_UPAVG)
                    yapf__simd_vertical_row(R, U, U + 1, Z, tw);
                else
                    yapf__simd_vertical_row(R, U + 1, U + 1, Z, tw);
                R[tw] = R[tw - 1];
                continue;
            }
#endif
            yapf__unfilter_row(f, R, U, K, Z, tw);
        }
    }
    if (p != end) return -1;

    for (uint32_t y = 0; y < th; y++) {
        uint8_t *d = pixels + ((size_t)(ty + y) * img_w + tx) * (size_t)ch * sb;
        size_t   r = (size_t)y * YAPF__ROW;
        for (uint32_t x = 0; x < tw; x++) {
            uint32_t v[4];
            for (int c = 0; c < ch; c++) {
                uint32_t a = 0;
                for (int k = 0; k < sb; k++) a = (a << 8) | sc->plane[c * sb + k][r + x];
                v[c] = a;
            }
            if (xf == 0) {
                uint32_t o_ = (v[1] - half) & m, q = (v[2] - half) & m;
                uint32_t t_ = (v[0] - yapf__sar1(q, bits)) & m;
                uint32_t g  = (q + t_) & m;
                uint32_t b  = (t_ - yapf__sar1(o_, bits)) & m;
                v[0] = (b + o_) & m;
                v[1] = g;
                v[2] = b;
            } else if (xf == 1) {
                uint32_t g = v[0];
                v[0] = (v[1] + g - half) & m;
                v[2] = (v[2] + g - half) & m;
                v[1] = g;
            }
            for (int c = 0; c < ch; c++)
                yapf__put_sample(d + ((size_t)x * ch + c) * sb, sample,
                                 isf ? yapf__funmap(v[c], bits) : v[c]);
        }
    }
    return 0;
}

/* ── GPU block tiles (version 2) ─────────────────────────────────────── */
/*
 * A block tile holds up to 16 × 16 blocks, rows of blocks top to bottom,
 * exactly as the GPU takes them.  Packed tiles (bit 31 of the tile size)
 * code the bytes lane by lane — byte 0 of every block, then byte 1, … —
 * each lane with its own table of the surface's code book (slot = lane):
 * endpoint bytes and index bytes have very different statistics.  The
 * bit stream fills the tile exactly.
 */
static int yapf__decode_block_tile(const uint8_t *src, size_t len, int packed,
                                   uint8_t *dst, size_t dst_row,
                                   uint32_t tbw, uint32_t tbh, uint32_t S,
                                   const yapf__dbook *bk)
{
    size_t n = (size_t)tbw * tbh, raw = n * S;
    if (!packed) {
        if (len != raw) return -1;
        for (uint32_t r = 0; r < tbh; r++)
            memcpy(dst + r * dst_row, src + (size_t)r * tbw * S, (size_t)tbw * S);
        return 0;
    }
    if (!bk) return -1;
    yapf__br br;
    yapf__br_init(&br, src, len);
    for (uint32_t k = 0; k < S; k++) {
        const uint16_t *lut = bk->slot[k];
        if (!lut) return -1;
        for (size_t i = 0; i < n; i++) {
            int v = yapf__br_sym(&br, lut);
            if (v < 0) return -1;
            dst[(i / tbw) * dst_row + (i % tbw) * S + k] = (uint8_t)v;
        }
    }
    return yapf__br_done(&br) ? 0 : -1;
}

/* Writes one block tile; *packed is set when the packed form is used. */
static int yapf__encode_block_tile(yapf__out *o, const uint8_t *src, size_t src_row,
                                   uint32_t tbw, uint32_t tbh, uint32_t S,
                                   int mode, yapf__ebook *eb, int *packed)
{
    size_t n = (size_t)tbw * tbh, raw = n * S;
    *packed = 0;
    if (mode == YAPF__ENC_STATS)
        for (uint32_t r = 0; r < tbh; r++)
            for (size_t i = 0; i < (size_t)tbw * S; i++)
                eb->freq[i % S][0][src[r * src_row + i]]++;
    if (mode == YAPF__ENC_BOOK) {
        uint64_t bits = 0;
        for (uint32_t r = 0; r < tbh && bits != UINT64_MAX; r++)
            for (size_t i = 0; i < (size_t)tbw * S; i++) {
                uint8_t l = eb->len[i % S][0][src[r * src_row + i]];
                if (!l) { bits = UINT64_MAX; break; }
                bits += l;
            }
        if (bits != UINT64_MAX && (bits + 7) / 8 < raw) {
            if (yapf__reserve(o, (size_t)(bits + 7) / 8 + 8) != 0) return -1;
            yapf__bitw bw = { 0, 0, o };
            for (uint32_t k = 0; k < S; k++)          /* lane by lane */
                for (size_t i = 0; i < n; i++) {
                    uint8_t v = src[(i / tbw) * src_row + (i % tbw) * S + k];
                    yapf__bitw_put(&bw, eb->code[k][0][v], eb->len[k][0][v]);
                }
            yapf__bitw_flush(&bw);
            *packed = 1;
            return 0;
        }
    }
    if (yapf__reserve(o, raw) != 0) return -1;
    for (uint32_t r = 0; r < tbh; r++) {
        memcpy(o->buf + o->len, src + r * src_row, (size_t)tbw * S);
        o->len += (size_t)tbw * S;
    }
    return 0;
}

/* Encoder scratch, heap allocated once per level (too big for small
 * thread stacks). */
typedef struct {
    uint8_t plane[2][4][YAPF__TILE_PX];
    uint8_t z[2][4][YAPF__TILE_PX + YAPF__GROUP];
    uint8_t filt[2][4][YAPF_TILE_SIZE / YAPF__BAND];
} yapf__scratch;

static int yapf__encode_tile(
    yapf__out *o, yapf__scratch *sc,
    const uint8_t *pixels, uint32_t img_w,
    uint32_t tx, uint32_t ty, uint32_t tw, uint32_t th,
    int ch, int mode, yapf__ebook *eb, yapf__out *tmp)
{
    uint32_t total = tw * th;
    size_t   start = o->len;
    size_t   raw   = (size_t)total * ch;
    int      tries = ch >= 3 ? 2 : 1;
    uint8_t  mask[2] = { 0, 0 };
    uint32_t est[2]  = { 0, 0 };

    /* For each colour transform: planes, constant mask, filters, and an
     * estimate of the coded size.  Only the cheaper one is written. */
    for (int t = 0; t < tries; t++) {
        uint8_t (*plane)[YAPF__TILE_PX] = sc->plane[t];
        for (uint32_t y = 0; y < th; y++) {
            const uint8_t *row = pixels + ((size_t)(ty + y) * img_w + tx) * (size_t)ch;
            for (uint32_t x = 0; x < tw; x++) {
                const uint8_t *s = row + (size_t)x * ch;
                uint32_t i = y * tw + x;
                if (ch < 3) {
                    for (int c = 0; c < ch; c++) plane[c][i] = s[c];
                    continue;
                }
                if (t == 0) {
                    yapf__forward(&plane[0][i], &plane[1][i], &plane[2][i],
                                  s[0], s[1], s[2]);
                } else {
                    plane[0][i] = s[1];
                    plane[1][i] = (uint8_t)(s[0] - s[1] + 128);
                    plane[2][i] = (uint8_t)(s[2] - s[1] + 128);
                }
                if (ch == 4) plane[3][i] = s[3];
            }
        }
        for (int c = 0; c < ch; c++) {
            uint32_t i = 1;
            while (i < total && plane[c][i] == plane[c][0]) i++;
            if (i == total) { mask[t] |= (uint8_t)(1u << c); est[t] += 16; continue; }
            est[t] += yapf__choose_filters(plane[c], tw, th, sc->z[t][c], sc->filt[t][c]);
        }
    }
    int t = (tries == 2 && est[1] < est[0]) ? 1 : 0;

    if (mode != YAPF__ENC_PLAIN) {      /* v2: statistics / entropy planes */
        yapf__planeset ps;
        uint8_t        cval[4];
        memset(&ps, 0, sizeof(ps));
        ps.np = ch;
        ps.cmask = mask[t];
        ps.cval = cval;
        for (int c = 0; c < ch; c++) {
            cval[c] = sc->plane[t][c][0];
            ps.z[c] = sc->z[t][c];
            ps.filt[c] = sc->filt[t][c];
        }
        if (yapf__emit_planeset(o, tmp, (uint8_t)(mask[t] | (t ? YAPF__SUBGREEN : 0)),
                                &ps, tw, th, mode, eb) != 0) return -1;
    } else {
    if (yapf__reserve(o, 1 + 4) != 0) return -1;
    o->buf[o->len++] = (uint8_t)(mask[t] | (t ? YAPF__SUBGREEN : 0));
    for (int c = 0; c < ch; c++)
        if (mask[t] & (1u << c)) o->buf[o->len++] = sc->plane[t][c][0];
    for (int c = 0; c < ch; c++)
        if (!(mask[t] & (1u << c)) &&
            yapf__emit_plane(o, sc->z[t][c], sc->filt[t][c], tw, th) != 0) return -1;
    }

    /* Fall back to a stored tile if coding did not pay off. */
    if (o->len - start > raw + 1) {
        o->len = start;
        if (yapf__reserve(o, raw + 1) != 0) return -1;
        o->buf[o->len++] = YAPF__STORED;
        for (uint32_t y = 0; y < th; y++) {
            const uint8_t *row = pixels + ((size_t)(ty + y) * img_w + tx) * (size_t)ch;
            memcpy(o->buf + o->len, row, (size_t)tw * ch);
            o->len += (size_t)tw * ch;
        }
    }
    return 0;
}

/* v2 (version 2 files) additionally allows bit 5 (a mask byte follows:
 * bit c = plane c is entropy coded with book bk) and bit 6 (no colour
 * transform). */
static int yapf__decode_tile(
    const uint8_t *data, size_t len,
    uint8_t *pixels, uint32_t img_w,
    uint32_t tx, uint32_t ty, uint32_t tw, uint32_t th,
    int ch, int simd, int v2, const yapf__dbook *bk)
{
    uint8_t        rows[4][YAPF__RING][YAPF_TILE_SIZE + 1];  /* last rows, ring */
    uint8_t        z[4][YAPF__TILE_PX + YAPF__GROUP];
    const uint8_t *filt[4] = { NULL, NULL, NULL, NULL };
    size_t         rowb  = (size_t)tw * ch;
    const uint8_t *p     = data, *end = data + len;

    if (len == 0) return -1;
    uint8_t hdr = *p++;

    if (hdr == YAPF__STORED) {
        if (len != 1 + rowb * th) return -1;
        for (uint32_t y = 0; y < th; y++)
            memcpy(pixels + ((size_t)(ty + y) * img_w + tx) * (size_t)ch,
                   p + rowb * y, rowb);
        return 0;
    }
    if (hdr & (v2 ? 0x80 : 0xE0)) return -1;        /* reserved bits set */
    if ((hdr & YAPF__NOXFORM) && (hdr & YAPF__SUBGREEN)) return -1;
    uint8_t xf = (hdr & YAPF__NOXFORM) ? 2 : (hdr & YAPF__SUBGREEN) ? 1 : 0;
    uint8_t emask = 0;
    int     ent = hdr & YAPF__ENTROPY;
    hdr &= 0x0F;
    if ((hdr >> ch) || (xf && ch < 3)) return -1;   /* unknown bits set */
    if (ent) {
        if (p >= end || !bk) return -1;      /* entropy needs the surface book */
        emask = *p++;
        if ((emask >> ch) || (emask & hdr)) return -1;
    }

    for (int c = 0; c < ch; c++) {
        if (!(hdr & (1u << c))) continue;
        if (p >= end) return -1;
        memset(rows[c], *p++, sizeof(rows[c]));
    }
    yapf__hstream hs[4];
    int           nh = 0;
    for (int c = 0; c < ch; c++) {
        if (hdr & (1u << c)) continue;
        p = (emask & (1u << c))
          ? yapf__huff_head(p, end, &filt[c], th, bk, xf * 16 + c, z[c], &hs[nh++])
          : yapf__unpack_plane(p, end, z[c], &filt[c], tw, th, simd);
        if (!p) return -1;
        /* SIMD row kernels read whole 16-sample chunks; keep the unused
         * tail of narrow tiles defined. */
        if (tw != YAPF_TILE_SIZE) {
            size_t used = ((size_t)tw * th + YAPF__GROUP - 1) / YAPF__GROUP * YAPF__GROUP;
            memset(z[c] + used, 0, sizeof(z[c]) - used);
        }
    }
    if (p != end) return -1;
    if (nh && yapf__huff_run(hs, nh, tw, th) != 0) return -1;

    /* Row by row: undo the filters of every plane (independent dependency
     * chains the CPU can overlap), then the colour transform while the
     * row is still in L1. */
    for (uint32_t y = 0; y < th; y++) {
        for (int c = 0; c < ch; c++) {
            if (!filt[c]) continue;
            uint8_t  f = filt[c][y / YAPF__BAND];
            uint32_t k = f >= YAPF__F_UP ? (uint32_t)(f - YAPF__F_UP + 1) : 1;
            uint8_t       *R = rows[c][y % YAPF__RING];
            const uint8_t *U = y ? rows[c][(y - 1) % YAPF__RING] : yapf__zero_row;
            const uint8_t *K = y >= k ? rows[c][(y - k) % YAPF__RING] : yapf__zero_row;
            const uint8_t *Z = z[c] + y * tw;
#if YAPF__SIMD
            if (simd) {
                if (f == YAPF__F_LEFT || f == YAPF__F_GRAD)
                    yapf__simd_serial_row(f, R, U, Z, tw);
                else if (f >= YAPF__F_UP)
                    yapf__simd_vertical_row(R, K, K, Z, tw);
                else if (f == YAPF__F_UPAVG)
                    yapf__simd_vertical_row(R, U, U + 1, Z, tw);
                else
                    yapf__simd_vertical_row(R, U + 1, U + 1, Z, tw);
                R[tw] = R[tw - 1];
                continue;
            }
#endif
            yapf__unfilter_row(f, R, U, K, Z, tw);
        }

        uint8_t       *d  = pixels + ((size_t)(ty + y) * img_w + tx) * (size_t)ch;
        const uint8_t *P0 = rows[0][y % YAPF__RING], *P1 = rows[1][y % YAPF__RING];
        const uint8_t *P2 = rows[2][y % YAPF__RING], *P3 = rows[3][y % YAPF__RING];
#if YAPF__SIMD
        if (simd) {
            if (tw == YAPF_TILE_SIZE) {
                yapf__simd_inverse_row(d, P0, P1, P2, P3, ch, xf);
            } else {
                uint8_t tmp[YAPF_TILE_SIZE * 4];
                yapf__simd_inverse_row(tmp, P0, P1, P2, P3, ch, xf);
                memcpy(d, tmp, (size_t)tw * ch);
            }
            continue;
        }
#endif

#define YAPF__INVERSE(N, ALPHA)                                             \
        for (uint32_t x = 0; x < tw; x++, d += N) {                         \
            uint8_t co = (uint8_t)(P1[x] - 128), cg = (uint8_t)(P2[x] - 128); \
            uint8_t t  = (uint8_t)(P0[x] - ((int8_t)cg >> 1));              \
            uint8_t b  = (uint8_t)(t - ((int8_t)co >> 1));                  \
            d[0] = (uint8_t)(b + co);                                       \
            d[1] = (uint8_t)(cg + t);                                       \
            d[2] = b;                                                       \
            ALPHA;                                                          \
        }
#define YAPF__INVERSE_SG(N, ALPHA)                                          \
        for (uint32_t x = 0; x < tw; x++, d += N) {                         \
            uint8_t g = P0[x];                                              \
            d[0] = (uint8_t)(P1[x] + g - 128);                              \
            d[1] = g;                                                       \
            d[2] = (uint8_t)(P2[x] + g - 128);                              \
            ALPHA;                                                          \
        }
        if (ch == 3) {
            /* Planes → separate R, G, B rows (vectorises), then interleave. */
            uint8_t rgb[3][YAPF_TILE_SIZE];
            if (xf == 2) {
                memcpy(rgb[0], P0, tw); memcpy(rgb[1], P1, tw); memcpy(rgb[2], P2, tw);
            } else if (xf) {
                for (uint32_t x = 0; x < tw; x++) {
                    uint8_t g = P0[x];
                    rgb[0][x] = (uint8_t)(P1[x] + g - 128);
                    rgb[1][x] = g;
                    rgb[2][x] = (uint8_t)(P2[x] + g - 128);
                }
            } else {
                for (uint32_t x = 0; x < tw; x++) {
                    uint8_t co = (uint8_t)(P1[x] - 128), cg = (uint8_t)(P2[x] - 128);
                    uint8_t t  = (uint8_t)(P0[x] - ((int8_t)cg >> 1));
                    uint8_t b  = (uint8_t)(t - ((int8_t)co >> 1));
                    rgb[0][x] = (uint8_t)(b + co);
                    rgb[1][x] = (uint8_t)(cg + t);
                    rgb[2][x] = b;
                }
            }
            d = pixels + ((size_t)(ty + y) * img_w + tx) * 3;
            for (uint32_t x = 0; x < tw; x++, d += 3) {
                d[0] = rgb[0][x]; d[1] = rgb[1][x]; d[2] = rgb[2][x];
            }
        } else if (xf == 2) {
            for (uint32_t x = 0; x < tw; x++, d += 4) {
                d[0] = P0[x]; d[1] = P1[x]; d[2] = P2[x]; d[3] = P3[x];
            }
        } else if (xf) {
            YAPF__INVERSE_SG(4, d[3] = P3[x])
        } else if (ch == 4) {
            YAPF__INVERSE(4, d[3] = P3[x])
        } else if (ch == 2) {
            for (uint32_t x = 0; x < tw; x++, d += 2) { d[0] = P0[x]; d[1] = P1[x]; }
        } else {
            memcpy(d, P0, tw);
        }
#undef YAPF__INVERSE
#undef YAPF__INVERSE_SG
    }
    return 0;
}

/* ── Level encode / decode ───────────────────────────────────────────── */
/*
 * Level layout:
 *   uint32 tile_size[tile_count]   byte size of each tile, row-major order
 *   tile data, back to back
 * Tiles are independent, so they can be decoded in any order or in
 * parallel, and a sub-rectangle can be decoded without the rest.
 */

static uint32_t yapf__tiles(uint32_t n) {
    return (n + YAPF_TILE_SIZE - 1) / YAPF_TILE_SIZE;
}

static int yapf__encode_level(
    yapf__out *o, const uint8_t *pixels, uint32_t mw, uint32_t mh, int ch)
{
    uint32_t tcx = yapf__tiles(mw), tcy = yapf__tiles(mh);
    size_t   tcount = (size_t)tcx * tcy;

    /* Each level has its own buffer, so the table starts at offset 0.
     * Reserve the size table, fill it after each tile is written. */
    if (yapf__reserve(o, tcount * 4) != 0) return -1;
    memset(o->buf, 0, tcount * 4);
    o->len = tcount * 4;

    yapf__scratch *sc = (yapf__scratch *)malloc(sizeof(yapf__scratch));
    if (!sc) return -1;

    size_t t = 0;
    int    rc = 0;
    for (uint32_t ty = 0; ty < tcy && rc == 0; ty++) {
        for (uint32_t tx = 0; tx < tcx && rc == 0; tx++, t++) {
            uint32_t x0 = tx * YAPF_TILE_SIZE, y0 = ty * YAPF_TILE_SIZE;
            uint32_t tw = mw - x0 < YAPF_TILE_SIZE ? mw - x0 : YAPF_TILE_SIZE;
            uint32_t th = mh - y0 < YAPF_TILE_SIZE ? mh - y0 : YAPF_TILE_SIZE;
            size_t   start = o->len;
            rc = yapf__encode_tile(o, sc, pixels, mw, x0, y0, tw, th, ch, YAPF__ENC_PLAIN, NULL, NULL);
            if (rc == 0) yapf__w32le(o->buf + t * 4, (uint32_t)(o->len - start));
        }
    }
    free(sc);
    return rc;
}

/* A level (surface) whose tile table has been validated; tiles can then
 * be decoded in any order, from any thread.  Version 1 levels are one
 * 8-bit slice with 4-byte table entries; version 2 adds slices, wide
 * samples, block tiles and 8-byte entries (size, yapf-sum). */
typedef struct {
    const uint8_t *data;
    size_t        *offs;     /* tcount + 1 byte offsets into data */
    uint8_t       *pixels;
    uint32_t       mw, mh, md, tcx, tcy;   /* tcy: tile rows per slice */
    int            ch;
    int            sample;   /* YAPF_SAMPLE_* (pixel payload) */
    int            simd;     /* SIMD kernels usable on this CPU */
    int            v2;
    int            verify;   /* check tile yapf-sums */
    uint32_t       blk;      /* bytes per GPU block, 0 = pixel payload */
    uint32_t       nbx, nby; /* blocks across / down */
    yapf__dbook   *book;     /* the surface's code book, or NULL */
} yapf__level;

static void yapf__level_free(yapf__level *lv) {
    free(lv->offs);
    lv->offs = NULL;
    if (lv->book) { free(lv->book->mem); free(lv->book); lv->book = NULL; }
}

/* Parses the tile table; lv must have data, the grid and v2 set. */
static int yapf__level_table(yapf__level *lv, size_t len) {
    uint64_t tc64 = (uint64_t)lv->tcx * lv->tcy * lv->md;
    size_t   ent  = lv->v2 ? 8 : 4;
    if ((uint64_t)(len / ent) < tc64) return -1;
    size_t tcount = (size_t)tc64;
    lv->offs = (size_t *)malloc((tcount + 1) * sizeof(size_t));
    if (!lv->offs) return -1;

    size_t pos = tcount * ent;
    for (size_t t = 0; t < tcount; t++) {
        uint32_t sz = yapf__r32le(lv->data + t * ent);
        if (lv->v2) {
            if ((sz >> 31) && (!lv->blk || !lv->book)) return -1;   /* packed: blocks + book */
            sz &= 0x7FFFFFFFu;
        }
        if (sz > len - pos) return -1;
        lv->offs[t] = pos;
        pos += sz;
    }
    lv->offs[tcount] = pos;
    if (lv->v2 && pos != len) return -1;
    return 0;
}

static int yapf__level_init(yapf__level *lv, const uint8_t *data, size_t len,
                            uint8_t *pixels, uint32_t mw, uint32_t mh, int ch)
{
    lv->data   = data;
    lv->pixels = pixels;
    lv->mw     = mw;
    lv->mh     = mh;
    lv->md     = 1;
    lv->tcx    = yapf__tiles(mw);
    lv->tcy    = yapf__tiles(mh);
    lv->ch     = ch;
    lv->sample = YAPF_SAMPLE_U8;
    return yapf__level_table(lv, len);
}

/* Decodes tile (tx, row) of a level, row counting across all slices.
 * dst == NULL writes into the level's pixels; otherwise into dst with
 * dst_w texels (or blocks) per row, the tile at its top-left corner.
 * Returns 0, -1 (corrupt) or YAPF_ERR_CHECKSUM. */
static int yapf__level_tile(const yapf__level *lv, uint32_t tx, uint32_t row,
                            yapf__wdec *wsc, uint8_t *dst, size_t dst_w)
{
    size_t         t   = (size_t)row * lv->tcx + tx;
    uint32_t       z   = row / lv->tcy, ty = row % lv->tcy;
    const uint8_t *src = lv->data + lv->offs[t];
    size_t         len = lv->offs[t + 1] - lv->offs[t];

    if (lv->v2 && lv->verify && yapf_checksum(src, len) != yapf__r32le(lv->data + t * 8 + 4))
        return YAPF_ERR_CHECKSUM;

    if (lv->blk) {
        uint32_t bx0 = tx * YAPF_BLOCK_TILE, by0 = ty * YAPF_BLOCK_TILE;
        uint32_t tbw = lv->nbx - bx0 < YAPF_BLOCK_TILE ? lv->nbx - bx0 : YAPF_BLOCK_TILE;
        uint32_t tbh = lv->nby - by0 < YAPF_BLOCK_TILE ? lv->nby - by0 : YAPF_BLOCK_TILE;
        int      packed = (int)(yapf__r32le(lv->data + t * 8) >> 31);
        size_t   rowb;
        if (!dst) {
            rowb = (size_t)lv->nbx * lv->blk;
            dst  = lv->pixels + (size_t)z * lv->nby * rowb + (size_t)by0 * rowb + (size_t)bx0 * lv->blk;
        } else {
            rowb = dst_w * lv->blk;
        }
        return yapf__decode_block_tile(src, len, packed, dst, rowb, tbw, tbh, lv->blk, lv->book);
    }

    uint32_t x0 = tx * YAPF_TILE_SIZE, y0 = ty * YAPF_TILE_SIZE;
    uint32_t tw = lv->mw - x0 < YAPF_TILE_SIZE ? lv->mw - x0 : YAPF_TILE_SIZE;
    uint32_t th = lv->mh - y0 < YAPF_TILE_SIZE ? lv->mh - y0 : YAPF_TILE_SIZE;
    size_t   px = yapf__sbytes(lv->sample) * (size_t)lv->ch;
    uint8_t *base;
    uint32_t img_w;
    if (!dst) {
        base  = lv->pixels + (size_t)z * lv->mw * lv->mh * px;
        img_w = lv->mw;
    } else {
        base  = dst;
        img_w = (uint32_t)dst_w;
        x0 = y0 = 0;
    }
    if (lv->sample == YAPF_SAMPLE_U8)
        return yapf__decode_tile(src, len, base, img_w, x0, y0, tw, th, lv->ch, lv->simd, lv->v2, lv->book);
    return yapf__decode_tile_wide(src, len, base, img_w, x0, y0, tw, th, lv->ch,
                                  lv->sample, lv->simd, lv->book, wsc);
}

/* Decode tile rows first_row, first_row + step, ... (over all slices) */
static int yapf__level_decode_rows(const yapf__level *lv,
                                   uint32_t first_row, uint32_t step)
{
    uint32_t    rows = lv->tcy * lv->md;
    yapf__wdec *wsc  = NULL;
    int         rc   = 0;
    if (!lv->blk && lv->sample != YAPF_SAMPLE_U8 && first_row < rows) {
        wsc = (yapf__wdec *)malloc(sizeof(yapf__wdec));
        if (!wsc) return YAPF_ERR_OOM;
    }
    for (uint32_t ty = first_row; ty < rows && rc == 0; ty += step)
        for (uint32_t tx = 0; tx < lv->tcx && rc == 0; tx++)
            rc = yapf__level_tile(lv, tx, ty, wsc, NULL, 0);
    free(wsc);
    return rc;
}

/* ── Threads ─────────────────────────────────────────────────────────── */
/*
 * Worker t of n decodes tile rows t, t + n, t + 2n, ... of every level.
 * Define YAPF_NO_THREADS to build without any threading support.
 */

typedef struct {
    const yapf__level *levels;
    int                nlevels;
    uint32_t           index, count;
    int                rc;
} yapf__job;

static void yapf__job_run(yapf__job *j) {
    j->rc = 0;
    for (int m = 0; m < j->nlevels && j->rc == 0; m++)
        j->rc = yapf__level_decode_rows(&j->levels[m], j->index, j->count);
}

#if !defined(YAPF_NO_THREADS) && defined(_WIN32)
#  include <windows.h>
#  define YAPF__THREADS 1
typedef HANDLE yapf__thread;
static DWORD WINAPI yapf__thread_main(LPVOID arg) {
    yapf__job_run((yapf__job *)arg);
    return 0;
}
static int yapf__thread_start(yapf__thread *t, yapf__job *j) {
    *t = CreateThread(NULL, 0, yapf__thread_main, j, 0, NULL);
    return *t ? 0 : -1;
}
static void yapf__thread_join(yapf__thread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
static int yapf__cpu_count(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}
#elif !defined(YAPF_NO_THREADS) && (defined(__unix__) || defined(__APPLE__))
#  include <pthread.h>
#  include <unistd.h>
#  define YAPF__THREADS 1
typedef pthread_t yapf__thread;
static void *yapf__thread_main(void *arg) {
    yapf__job_run((yapf__job *)arg);
    return NULL;
}
static int yapf__thread_start(yapf__thread *t, yapf__job *j) {
    return pthread_create(t, NULL, yapf__thread_main, j) == 0 ? 0 : -1;
}
static void yapf__thread_join(yapf__thread t) {
    pthread_join(t, NULL);
}
static int yapf__cpu_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}
#else
#  define YAPF__THREADS 0
#endif

#define YAPF__MAX_THREADS 64
#define YAPF__TILES_PER_THREAD 32

/* Decode all levels with up to `threads` threads (0 = one per core). */
static int yapf__decode_levels(const yapf__level *levels, int nlevels,
                               int threads)
{
    yapf__job jobs[YAPF__MAX_THREADS];
    int       n = 1;

#if YAPF__THREADS
    yapf__thread th[YAPF__MAX_THREADS];
    n = threads > 0 ? threads : yapf__cpu_count();
    if (n > YAPF__MAX_THREADS) n = YAPF__MAX_THREADS;
    {
        uint32_t rows = 0;
        for (int m = 0; m < nlevels; m++)
            if (levels[m].tcy * levels[m].md > rows) rows = levels[m].tcy * levels[m].md;
        if ((uint32_t)n > rows) n = (int)rows;
    }
    /* Starting a thread costs about as much as decoding a few tiles, so
     * give each one at least YAPF__TILES_PER_THREAD tiles of work. */
    {
        size_t tiles = 0;
        for (int m = 0; m < nlevels; m++) tiles += (size_t)levels[m].tcx * levels[m].tcy;
        if ((size_t)n > tiles / YAPF__TILES_PER_THREAD)
            n = (int)(tiles / YAPF__TILES_PER_THREAD);
    }
    if (n < 1) n = 1;
#else
    (void)threads;
#endif

    for (int i = 0; i < n; i++) {
        jobs[i].levels  = levels;
        jobs[i].nlevels = nlevels;
        jobs[i].index   = (uint32_t)i;
        jobs[i].count   = (uint32_t)n;
    }

#if YAPF__THREADS
    int started = 1;
    for (; started < n; started++)
        if (yapf__thread_start(&th[started], &jobs[started]) != 0) break;
    /* If a thread failed to start, the calling thread takes its rows. */
    yapf__job_run(&jobs[0]);
    for (int i = started; i < n; i++) yapf__job_run(&jobs[i]);
    for (int i = 1; i < started; i++) yapf__thread_join(th[i]);
#else
    yapf__job_run(&jobs[0]);
#endif

    for (int i = 0; i < n; i++)
        if (jobs[i].rc != 0) return jobs[i].rc;
    return 0;
}

/* ========================================================================
 *  yapf_load_memory / yapf_load
 * ======================================================================== */

static yapf_image_t *yapf__load_v2_image(const uint8_t *buf, size_t size, int threads);

static yapf_image_t *yapf__load(const uint8_t *buf, size_t size, int threads) {
    if (!buf || size < YAPF__HDR_LEN) return NULL;
    if (memcmp(buf, YAPF__MAGIC, 4) != 0) return NULL;
    if (buf[4] == 2) return yapf__load_v2_image(buf, size, threads);

    uint8_t  version    = buf[4];
    uint8_t  channels   = buf[5];
    uint8_t  mip_levels = buf[7];
    uint32_t base_w     = yapf__r32le(buf + 12);
    uint32_t base_h     = yapf__r32le(buf + 16);

    if (version != YAPF__VERSION) return NULL;
    if (channels < 1 || channels > 4) return NULL;
    if (mip_levels < 1 || mip_levels > YAPF_MAX_MIPS) return NULL;
    if (base_w == 0 || base_w > YAPF_MAX_DIM) return NULL;
    if (base_h == 0 || base_h > YAPF_MAX_DIM) return NULL;

    size_t idx_end = YAPF__HDR_LEN + (size_t)mip_levels * YAPF__MIP_ENTRY;
    if (size < idx_end) return NULL;

    yapf__level levels[YAPF_MAX_MIPS];
    memset(levels, 0, sizeof(levels));
    int ok = 0;

    yapf_image_t *img = (yapf_image_t *)calloc(1, sizeof(yapf_image_t));
    if (!img) return NULL;
    img->width      = base_w;
    img->height     = base_h;
    img->channels   = channels;
    img->gpu_format = buf[6];
    img->flags      = buf[8];
    img->mip_levels = mip_levels;
    img->mips = (uint8_t **)calloc(mip_levels, sizeof(uint8_t *));
    if (!img->mips) goto done;

    for (uint8_t m = 0; m < mip_levels; m++) {
        const uint8_t *e = buf + YAPF__HDR_LEN + (size_t)m * YAPF__MIP_ENTRY;
        uint32_t off = yapf__r32le(e);
        uint32_t len = yapf__r32le(e + 4);
        uint32_t mw  = base_w >> m; if (mw < 1) mw = 1;
        uint32_t mh  = base_h >> m; if (mh < 1) mh = 1;

        if (off < idx_end || off > size || len > size - off) goto done;

        img->mips[m] = (uint8_t *)malloc((size_t)mw * mh * channels);
        if (!img->mips[m]) goto done;

        if (yapf__level_init(&levels[m], buf + off, len,
                             img->mips[m], mw, mh, channels) != 0)
            goto done;
    }

#if YAPF__SIMD
    {
        int simd = yapf__simd_available();
        for (uint8_t m = 0; m < mip_levels; m++) levels[m].simd = simd;
    }
#endif
    ok = yapf__decode_levels(levels, mip_levels, threads) == 0;

done:
    for (unsigned m = 0; m < YAPF_MAX_MIPS; m++) free(levels[m].offs);
    if (!ok) { yapf_free(img); return NULL; }
    img->pixels = img->mips[0];
    return img;
}

yapf_image_t *yapf_load_memory(const void *buffer, size_t size) {
    return yapf__load((const uint8_t *)buffer, size, 1);
}

yapf_image_t *yapf_load_memory_mt(const void *buffer, size_t size, int threads) {
    return yapf__load((const uint8_t *)buffer, size, threads);
}

static uint8_t *yapf__read_file(const char *filename, size_t *out_len) {
    if (!filename) return NULL;
    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;

    /* Read the whole file in chunks; avoids ftell() limits on 32-bit long. */
    size_t cap = 1 << 16, len = 0;
    uint8_t *buf = (uint8_t *)malloc(cap);
    while (buf) {
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (len < cap) break;
        uint8_t *nb = (uint8_t *)realloc(buf, cap * 2);
        if (!nb) { free(buf); buf = NULL; break; }
        buf = nb;
        cap *= 2;
    }
    int err = ferror(f);
    fclose(f);
    if (!buf) return NULL;
    if (err) { free(buf); return NULL; }
    *out_len = len;
    return buf;
}

static yapf_image_t *yapf__load_file(const char *filename, int threads) {
    size_t   len = 0;
    uint8_t *buf = yapf__read_file(filename, &len);
    if (!buf) return NULL;
    yapf_image_t *img = yapf__load(buf, len, threads);
    free(buf);
    return img;
}

yapf_image_t *yapf_load(const char *filename) {
    return yapf__load_file(filename, 1);
}

yapf_image_t *yapf_load_mt(const char *filename, int threads) {
    return yapf__load_file(filename, threads);
}

/* ========================================================================
 *  yapf_encode / yapf_save
 * ======================================================================== */

int yapf_encode(const yapf_image_t *img, void **out_data, size_t *out_size) {
    if (!out_data || !out_size) return YAPF_ERR_INVALID;
    *out_data = NULL;
    *out_size = 0;
    if (!img || !img->pixels)                     return YAPF_ERR_INVALID;
    if (img->width == 0 || img->width > YAPF_MAX_DIM)   return YAPF_ERR_INVALID;
    if (img->height == 0 || img->height > YAPF_MAX_DIM) return YAPF_ERR_INVALID;
    if (img->channels < 1 || img->channels > 4)   return YAPF_ERR_INVALID;
    if (img->mip_levels < 1 || img->mip_levels > YAPF_MAX_MIPS) return YAPF_ERR_INVALID;

    int     ch         = (int)img->channels;
    uint8_t mip_levels = img->mip_levels;
    int     err        = YAPF_OK;

    yapf__out bws[YAPF_MAX_MIPS];
    memset(bws, 0, sizeof(bws));

    for (uint8_t m = 0; m < mip_levels && err == YAPF_OK; m++) {
        uint32_t mw = img->width  >> m; if (mw < 1) mw = 1;
        uint32_t mh = img->height >> m; if (mh < 1) mh = 1;

        const uint8_t *src = (m == 0) ? img->pixels
                           : (img->mips ? img->mips[m] : NULL);
        if (!src) { err = YAPF_ERR_INVALID; break; }

        if (yapf__reserve(&bws[m], (size_t)mw * mh * (size_t)ch / 2 + 1024) != 0 ||
            yapf__encode_level(&bws[m], src, mw, mh, ch) != 0)
            err = YAPF_ERR_OOM;
    }

    /* Offsets are 32-bit: refuse files that would not fit. */
    uint64_t total = YAPF__HDR_LEN + (uint64_t)mip_levels * YAPF__MIP_ENTRY;
    for (uint8_t m = 0; m < mip_levels && err == YAPF_OK; m++) {
        total += bws[m].len;
        if (total > 0xFFFFFFFFull) err = YAPF_ERR_INVALID;
    }

    uint8_t *file = NULL;
    if (err == YAPF_OK && !(file = (uint8_t *)malloc((size_t)total))) err = YAPF_ERR_OOM;

    if (err == YAPF_OK) {
        memset(file, 0, YAPF__HDR_LEN);
        memcpy(file, YAPF__MAGIC, 4);
        file[4] = YAPF__VERSION;
        file[5] = img->channels;
        file[6] = img->gpu_format;
        file[7] = mip_levels;
        file[8] = img->flags;
        yapf__w32le(file + 12, img->width);
        yapf__w32le(file + 16, img->height);

        size_t pos = YAPF__HDR_LEN + (size_t)mip_levels * YAPF__MIP_ENTRY;
        for (uint8_t m = 0; m < mip_levels; m++) {
            uint8_t *entry = file + YAPF__HDR_LEN + (size_t)m * YAPF__MIP_ENTRY;
            yapf__w32le(entry + 0, (uint32_t)pos);
            yapf__w32le(entry + 4, (uint32_t)bws[m].len);
            memcpy(file + pos, bws[m].buf, bws[m].len);
            pos += bws[m].len;
        }
        *out_data = file;
        *out_size = (size_t)total;
    }

    for (uint8_t m = 0; m < mip_levels; m++) free(bws[m].buf);
    return err;
}

void yapf_free_buffer(void *data) {
    free(data);
}

int yapf_save(const char *filename, const yapf_image_t *img) {
    if (!filename) return YAPF_ERR_INVALID;
    void  *data = NULL;
    size_t size = 0;
    int    err  = yapf_encode(img, &data, &size);
    if (err != YAPF_OK) return err;

    FILE *f = fopen(filename, "wb");
    if (!f) err = YAPF_ERR_IO;
    else {
        if (fwrite(data, 1, size, f) != size) err = YAPF_ERR_IO;
        if (fclose(f) != 0 && err == YAPF_OK) err = YAPF_ERR_IO;
    }
    yapf_free_buffer(data);
    return err;
}

/* ========================================================================
 *  yapf_free
 * ======================================================================== */

void yapf_free(yapf_image_t *img) {
    if (!img) return;
    if (img->mips) {
        for (uint8_t m = 0; m < img->mip_levels; m++)
            free(img->mips[m]);
        free(img->mips);
    }
    free(img);
}

/* ========================================================================
 *  Version 2: textures
 * ======================================================================== */

#define YAPF__V2_HDR      64
#define YAPF__V2_SURFACE  16
#define YAPF__MAX_SURFACES (1u << 24)

static const struct { uint8_t fmt, bw, bh, bytes; } yapf__block_formats[] = {
    { YAPF_GPU_BC1, 4, 4, 8 },        { YAPF_GPU_BC2, 4, 4, 16 },
    { YAPF_GPU_BC3, 4, 4, 16 },       { YAPF_GPU_BC4, 4, 4, 8 },
    { YAPF_GPU_BC4_SNORM, 4, 4, 8 },  { YAPF_GPU_BC5, 4, 4, 16 },
    { YAPF_GPU_BC5_SNORM, 4, 4, 16 }, { YAPF_GPU_BC6H, 4, 4, 16 },
    { YAPF_GPU_BC6H_SF, 4, 4, 16 },   { YAPF_GPU_BC7, 4, 4, 16 },
    { YAPF_GPU_ETC2_RGB8, 4, 4, 8 },  { YAPF_GPU_ETC2_RGB8A1, 4, 4, 8 },
    { YAPF_GPU_ETC2_RGBA8, 4, 4, 16 }, { YAPF_GPU_EAC_R11, 4, 4, 8 },
    { YAPF_GPU_EAC_R11_SNORM, 4, 4, 8 }, { YAPF_GPU_EAC_RG11, 4, 4, 16 },
    { YAPF_GPU_EAC_RG11_SNORM, 4, 4, 16 },
    { YAPF_GPU_ASTC_4x4, 4, 4, 16 },   { YAPF_GPU_ASTC_5x4, 5, 4, 16 },
    { YAPF_GPU_ASTC_5x5, 5, 5, 16 },   { YAPF_GPU_ASTC_6x5, 6, 5, 16 },
    { YAPF_GPU_ASTC_6x6, 6, 6, 16 },   { YAPF_GPU_ASTC_8x5, 8, 5, 16 },
    { YAPF_GPU_ASTC_8x6, 8, 6, 16 },   { YAPF_GPU_ASTC_8x8, 8, 8, 16 },
    { YAPF_GPU_ASTC_10x5, 10, 5, 16 }, { YAPF_GPU_ASTC_10x6, 10, 6, 16 },
    { YAPF_GPU_ASTC_10x8, 10, 8, 16 }, { YAPF_GPU_ASTC_10x10, 10, 10, 16 },
    { YAPF_GPU_ASTC_12x10, 12, 10, 16 }, { YAPF_GPU_ASTC_12x12, 12, 12, 16 }
};

int yapf_block_info(uint8_t format, uint32_t *bw, uint32_t *bh, uint32_t *bytes) {
    for (size_t i = 0; i < sizeof(yapf__block_formats) / sizeof(yapf__block_formats[0]); i++)
        if (yapf__block_formats[i].fmt == format) {
            if (bw) *bw = yapf__block_formats[i].bw;
            if (bh) *bh = yapf__block_formats[i].bh;
            if (bytes) *bytes = yapf__block_formats[i].bytes;
            return 1;
        }
    return 0;
}

size_t yapf_sample_bytes(uint8_t sample) {
    return sample <= YAPF_SAMPLE_F32 ? yapf__sbytes(sample) : 0;
}

/* Library-side state of a texture: the file tables of an opened file,
 * and the marker that the library owns data, notes and frame times. */
typedef struct {
    int       version;     /* 1 or 2 (0: made by yapf_tex_create)   */
    int       flags;       /* YAPF_LOAD_*                           */
    uint32_t  needs;
    uint64_t  file_size, front;
    uint64_t *off;
    uint32_t *size, *tcheck;
} yapf__texi;

static uint32_t yapf__faces(const yapf_texture_t *t) {
    return t->shape == YAPF_SHAPE_CUBE ? 6u : 1u;
}

uint32_t yapf_tex_faces(const yapf_texture_t *tex) {
    return tex ? yapf__faces(tex) : 0;
}

/* Checks a description; *count receives the number of surfaces. */
static int yapf__tex_check(const yapf_texture_t *t, uint32_t *count) {
    if (!t) return -1;
    if (t->channels < 1 || t->channels > 4) return -1;
    if (t->sample > YAPF_SAMPLE_F32 || t->payload > YAPF_PAYLOAD_BLOCKS) return -1;
    if (t->shape > YAPF_SHAPE_VOLUME) return -1;
    if (t->width == 0 || t->width > YAPF_MAX_DIM) return -1;
    if (t->height == 0 || t->height > YAPF_MAX_DIM) return -1;
    if (t->layers == 0 || t->frames == 0) return -1;
    if (t->mip_levels < 1 || t->mip_levels > YAPF_MAX_MIPS) return -1;
    if (t->shape == YAPF_SHAPE_VOLUME) {
        if (t->depth == 0 || t->depth > YAPF_MAX_DIM || t->layers != 1) return -1;
    } else if (t->depth > 1) {
        return -1;
    }
    if (t->shape == YAPF_SHAPE_CUBE && t->width != t->height) return -1;
    if (t->payload == YAPF_PAYLOAD_BLOCKS && !yapf_block_info(t->format, NULL, NULL, NULL))
        return -1;
    uint64_t n = (uint64_t)t->frames * t->layers * yapf__faces(t) * t->mip_levels;
    if (n > YAPF__MAX_SURFACES) return -1;
    /* the largest surface must be addressable (32-bit builds) */
    uint64_t texels = (uint64_t)t->width * t->height * (t->shape == YAPF_SHAPE_VOLUME ? t->depth : 1);
    if (texels * 4 * 4 > (uint64_t)(SIZE_MAX / 2)) return -1;
    if (count) *count = (uint32_t)n;
    return 0;
}

uint32_t yapf_tex_surface_count(const yapf_texture_t *tex) {
    uint32_t n = 0;
    return yapf__tex_check(tex, &n) == 0 ? n : 0;
}

uint32_t yapf_tex_surface(const yapf_texture_t *tex, uint32_t frame,
                          uint32_t layer, uint32_t face, uint32_t mip)
{
    return ((frame * tex->layers + layer) * yapf__faces(tex) + face) * tex->mip_levels + mip;
}

void yapf_tex_mip_size(const yapf_texture_t *tex, uint32_t mip,
                       uint32_t *w, uint32_t *h, uint32_t *d)
{
    uint32_t a = tex->width >> mip, b = tex->height >> mip;
    uint32_t c = tex->shape == YAPF_SHAPE_VOLUME ? tex->depth >> mip : 1;
    if (w) *w = a ? a : 1;
    if (h) *h = b ? b : 1;
    if (d) *d = c ? c : 1;
}

/* Blocks across and down for a mip of a block texture. */
static void yapf__mip_blocks(const yapf_texture_t *t, uint32_t mip,
                             uint32_t *nbx, uint32_t *nby, uint32_t *S)
{
    uint32_t w, h, bw = 4, bh = 4, bytes = 16;
    yapf_tex_mip_size(t, mip, &w, &h, NULL);
    yapf_block_info(t->format, &bw, &bh, &bytes);
    *nbx = (w + bw - 1) / bw;
    *nby = (h + bh - 1) / bh;
    *S = bytes;
}

size_t yapf_tex_surface_bytes(const yapf_texture_t *tex, uint32_t mip) {
    uint32_t w, h, d;
    yapf_tex_mip_size(tex, mip, &w, &h, &d);
    if (tex->payload == YAPF_PAYLOAD_BLOCKS) {
        uint32_t nbx, nby, S;
        yapf__mip_blocks(tex, mip, &nbx, &nby, &S);
        return (size_t)nbx * nby * d * S;
    }
    return (size_t)w * h * d * tex->channels * yapf__sbytes(tex->sample);
}

void yapf_tex_tile_grid(const yapf_texture_t *tex, uint32_t mip,
                        uint32_t *tiles_x, uint32_t *tiles_y)
{
    uint32_t tx, ty;
    if (tex->payload == YAPF_PAYLOAD_BLOCKS) {
        uint32_t nbx, nby, S;
        yapf__mip_blocks(tex, mip, &nbx, &nby, &S);
        tx = (nbx + YAPF_BLOCK_TILE - 1) / YAPF_BLOCK_TILE;
        ty = (nby + YAPF_BLOCK_TILE - 1) / YAPF_BLOCK_TILE;
    } else {
        uint32_t w, h;
        yapf_tex_mip_size(tex, mip, &w, &h, NULL);
        tx = yapf__tiles(w);
        ty = yapf__tiles(h);
    }
    if (tiles_x) *tiles_x = tx;
    if (tiles_y) *tiles_y = ty;
}

static yapf_texture_t *yapf__tex_new(void) {
    yapf_texture_t *t = (yapf_texture_t *)calloc(1, sizeof(yapf_texture_t));
    if (!t) return NULL;
    t->internal_ = calloc(1, sizeof(yapf__texi));
    if (!t->internal_) { free(t); return NULL; }
    return t;
}

void yapf_tex_free(yapf_texture_t *tex) {
    if (!tex) return;
    yapf__texi *ti = (yapf__texi *)tex->internal_;
    uint32_t    n  = 0;
    if (tex->data) {
        n = (uint32_t)((uint64_t)tex->frames * tex->layers * yapf__faces(tex) * tex->mip_levels);
        for (uint32_t s = 0; s < n; s++) free(tex->data[s]);
        free(tex->data);
    }
    for (uint32_t i = 0; i < tex->note_count; i++) free(tex->notes[i].data);
    free(tex->notes);
    free(tex->frame_us);
    if (ti) { free(ti->off); free(ti->size); free(ti->tcheck); free(ti); }
    free(tex);
}

/* Copies the description fields (everything up to loop_count). */
static void yapf__tex_copy_desc(yapf_texture_t *d, const yapf_texture_t *s) {
    d->width = s->width; d->height = s->height;
    d->depth = s->shape == YAPF_SHAPE_VOLUME ? s->depth : 1;
    d->layers = s->layers; d->frames = s->frames; d->shape = s->shape;
    d->mip_levels = s->mip_levels; d->payload = s->payload; d->format = s->format;
    d->sample = s->sample; d->channels = s->channels; d->flags = s->flags;
    memcpy(d->channel_map, s->channel_map, 4);
    d->linear_mask = s->linear_mask; d->normal = s->normal;
    d->primaries = s->primaries; d->transfer = s->transfer;
    d->mip_filter = s->mip_filter; d->alpha_ref = s->alpha_ref;
    d->loop_count = s->loop_count;
}

yapf_texture_t *yapf_tex_create(const yapf_texture_t *desc) {
    yapf_texture_t probe;
    uint32_t       n;
    if (!desc) return NULL;
    probe = *desc;
    if (probe.shape != YAPF_SHAPE_VOLUME && probe.depth == 0) probe.depth = 1;
    if (yapf__tex_check(&probe, &n) != 0) return NULL;
    yapf_texture_t *t = yapf__tex_new();
    if (!t) return NULL;
    yapf__tex_copy_desc(t, &probe);
    t->data = (void **)calloc(n, sizeof(void *));
    if (!t->data) { yapf_tex_free(t); return NULL; }
    if (t->frames > 1) {
        t->frame_us = (uint32_t *)calloc(t->frames, sizeof(uint32_t));
        if (!t->frame_us) { yapf_tex_free(t); return NULL; }
    }
    for (uint32_t s = 0; s < n; s++) {
        t->data[s] = calloc(1, yapf_tex_surface_bytes(t, s % t->mip_levels));
        if (!t->data[s]) { yapf_tex_free(t); return NULL; }
    }
    return t;
}

int yapf_tex_add_note(yapf_texture_t *tex, const char tag[4],
                      const void *data, uint32_t size)
{
    if (!tex || !tex->internal_ || !tag || (size && !data)) return YAPF_ERR_INVALID;
    yapf_note_t *n = (yapf_note_t *)realloc(tex->notes, (tex->note_count + 1) * sizeof(yapf_note_t));
    if (!n) return YAPF_ERR_OOM;
    tex->notes = n;
    n += tex->note_count;
    memcpy(n->tag, tag, 4);
    n->size = size;
    n->data = (uint8_t *)malloc(size ? size : 1);
    if (!n->data) return YAPF_ERR_OOM;
    if (size) memcpy(n->data, data, size);
    tex->note_count++;
    return YAPF_OK;
}

const yapf_note_t *yapf_tex_find_note(const yapf_texture_t *tex, const char tag[4]) {
    if (!tex || !tag) return NULL;
    for (uint32_t i = 0; i < tex->note_count; i++)
        if (memcmp(tex->notes[i].tag, tag, 4) == 0) return &tex->notes[i];
    return NULL;
}

/* ── Writing ─────────────────────────────────────────────────────────── */

typedef struct {
    yapf__scratch  *sc;
    yapf__wscratch *wsc;
    yapf__ebook    *eb;
    yapf__out       tmp;
} yapf__enc;

/* u32 book size, the book, the tile table and the tiles of one surface.
 * *checked receives the length the tile-table check covers. */
static int yapf__encode_tiles(yapf__out *o, yapf__enc *e, const yapf_texture_t *t,
                              uint32_t mip, const uint8_t *src, int mode,
                              const yapf__out *book, size_t *checked)
{
    uint32_t w, h, d, tcx, tcy, nbx = 0, nby = 0, S = 0;
    int      blocks = t->payload == YAPF_PAYLOAD_BLOCKS;
    int      ch = t->channels, sb = (int)yapf__sbytes(t->sample);
    yapf_tex_mip_size(t, mip, &w, &h, &d);
    yapf_tex_tile_grid(t, mip, &tcx, &tcy);
    if (blocks) yapf__mip_blocks(t, mip, &nbx, &nby, &S);
    size_t n = (size_t)tcx * tcy * d, bl = book ? book->len : 0;
    size_t slice = blocks ? (size_t)nbx * nby * S : (size_t)w * h * ch * sb;

    o->len = 0;
    if (yapf__reserve(o, 4 + bl + n * 8) != 0) return -1;
    yapf__w32le(o->buf, (uint32_t)bl);
    if (bl) memcpy(o->buf + 4, book->buf, bl);
    size_t table = 4 + bl;
    memset(o->buf + table, 0, n * 8);
    o->len = table + n * 8;
    *checked = o->len;

    size_t t_i = 0;
    for (uint32_t z = 0; z < d; z++)
        for (uint32_t ty = 0; ty < tcy; ty++)
            for (uint32_t tx = 0; tx < tcx; tx++, t_i++) {
                size_t start = o->len;
                int    packed = 0, rc;
                const uint8_t *sl = src + (size_t)z * slice;
                if (blocks) {
                    uint32_t bx0 = tx * YAPF_BLOCK_TILE, by0 = ty * YAPF_BLOCK_TILE;
                    uint32_t tbw = nbx - bx0 < YAPF_BLOCK_TILE ? nbx - bx0 : YAPF_BLOCK_TILE;
                    uint32_t tbh = nby - by0 < YAPF_BLOCK_TILE ? nby - by0 : YAPF_BLOCK_TILE;
                    rc = yapf__encode_block_tile(o, sl + ((size_t)by0 * nbx + bx0) * S,
                                                 (size_t)nbx * S, tbw, tbh, S, mode, e->eb, &packed);
                } else {
                    uint32_t x0 = tx * YAPF_TILE_SIZE, y0 = ty * YAPF_TILE_SIZE;
                    uint32_t tw = w - x0 < YAPF_TILE_SIZE ? w - x0 : YAPF_TILE_SIZE;
                    uint32_t th = h - y0 < YAPF_TILE_SIZE ? h - y0 : YAPF_TILE_SIZE;
                    rc = sb == 1
                       ? yapf__encode_tile(o, e->sc, sl, w, x0, y0, tw, th, ch, mode, e->eb, &e->tmp)
                       : yapf__encode_tile_wide(o, &e->tmp, e->wsc, sl, w, x0, y0, tw, th, ch,
                                                t->sample, mode, e->eb);
                }
                if (rc != 0) return -1;
                size_t sz = o->len - start;
                yapf__w32le(o->buf + table + t_i * 8, (uint32_t)sz | ((uint32_t)packed << 31));
                yapf__w32le(o->buf + table + t_i * 8 + 4, yapf_checksum(o->buf + start, sz));
            }
    return 0;
}

/* One surface; with entropy: a counting pass, the book, a coding pass,
 * and the book version is kept only if it comes out smaller. */
static int yapf__encode_surface(yapf__out *o, yapf__enc *e, const yapf_texture_t *t,
                                uint32_t mip, const uint8_t *src, int entropy,
                                size_t *checked, int *used_entropy)
{
    if (!entropy || !e->eb)
        return yapf__encode_tiles(o, e, t, mip, src, YAPF__ENC_PLAIN, NULL, checked);
    memset(e->eb->freq, 0, sizeof(e->eb->freq));
    if (yapf__encode_tiles(o, e, t, mip, src, YAPF__ENC_STATS, NULL, checked) != 0) return -1;
    yapf__out book = { NULL, 0, 0 }, alt = { NULL, 0, 0 };
    size_t    alt_checked = 0;
    int       rc = yapf__book_write(e->eb, &book);
    if (rc == 0) rc = yapf__encode_tiles(&alt, e, t, mip, src, YAPF__ENC_BOOK, &book, &alt_checked);
    if (rc == 0 && alt.len < o->len) {
        yapf__out tmp = *o;
        *o = alt;
        alt = tmp;
        *checked = alt_checked;
        *used_entropy = 1;
    }
    free(book.buf);
    free(alt.buf);
    return rc < 0 ? -1 : 0;
}

int yapf_tex_encode(const yapf_texture_t *tex, int flags, void **out_data, size_t *out_size) {
    if (!out_data || !out_size) return YAPF_ERR_INVALID;
    *out_data = NULL;
    *out_size = 0;
    uint32_t S;
    yapf_texture_t t;
    if (!tex) return YAPF_ERR_INVALID;
    t = *tex;
    if (t.shape != YAPF_SHAPE_VOLUME && t.depth == 0) t.depth = 1;
    if (yapf__tex_check(&t, &S) != 0 || !t.data) return YAPF_ERR_INVALID;
    for (uint32_t s = 0; s < S; s++) if (!t.data[s]) return YAPF_ERR_INVALID;
    if (t.frames > 1 && !t.frame_us) return YAPF_ERR_INVALID;
    uint64_t notes = 0;
    for (uint32_t i = 0; i < t.note_count; i++) {
        if (t.notes[i].size && !t.notes[i].data) return YAPF_ERR_INVALID;
        notes += 8 + (uint64_t)t.notes[i].size;
    }
    if (notes > 0xFFFFFFFFull) return YAPF_ERR_INVALID;

    int        err = YAPF_OK, used_ent = 0;
    int        entropy = (flags & YAPF_ENC_ENTROPY) != 0;
    yapf__out *bufs = (yapf__out *)calloc(S, sizeof(yapf__out));
    uint32_t  *tchk = (uint32_t *)calloc(S, sizeof(uint32_t));
    yapf__enc  e;
    memset(&e, 0, sizeof(e));
    if (!bufs || !tchk) err = YAPF_ERR_OOM;
    if (err == YAPF_OK && t.payload == YAPF_PAYLOAD_PIXELS) {
        if (t.sample == YAPF_SAMPLE_U8) e.sc = (yapf__scratch *)malloc(sizeof(yapf__scratch));
        else e.wsc = (yapf__wscratch *)malloc(sizeof(yapf__wscratch));
        if (!e.sc && !e.wsc) err = YAPF_ERR_OOM;
    }
    if (err == YAPF_OK && entropy && !(e.eb = (yapf__ebook *)malloc(sizeof(yapf__ebook))))
        err = YAPF_ERR_OOM;
    for (uint32_t s = 0; s < S && err == YAPF_OK; s++) {
        size_t n = 0;
        if (yapf__encode_surface(&bufs[s], &e, &t, s % t.mip_levels,
                                 (const uint8_t *)t.data[s], entropy, &n, &used_ent) != 0)
            err = YAPF_ERR_OOM;
        else
            tchk[s] = yapf_checksum(bufs[s].buf, n);
    }

    uint64_t front = YAPF__V2_HDR + (uint64_t)S * YAPF__V2_SURFACE
                   + (t.frames > 1 ? 4ull * t.frames : 0) + notes;
    uint64_t total = front;
    for (uint32_t s = 0; s < S && err == YAPF_OK; s++) total += bufs[s].len;
    if (err == YAPF_OK && total > (uint64_t)SIZE_MAX) err = YAPF_ERR_INVALID;

    uint8_t *file = NULL;
    if (err == YAPF_OK && !(file = (uint8_t *)malloc((size_t)total))) err = YAPF_ERR_OOM;
    if (err == YAPF_OK) {
        uint32_t needs = 0;
        if (t.payload == YAPF_PAYLOAD_PIXELS && t.sample != YAPF_SAMPLE_U8) needs |= YAPF_NEED_WIDE;
        if (t.payload == YAPF_PAYLOAD_BLOCKS) needs |= YAPF_NEED_BLOCKS;
        if (used_ent) needs |= YAPF_NEED_ENTROPY;
        if (t.shape != YAPF_SHAPE_2D || t.layers > 1) needs |= YAPF_NEED_LAYOUT;
        if (t.frames > 1) needs |= YAPF_NEED_FRAMES;

        uint8_t *h = file;
        memset(h, 0, YAPF__V2_HDR);
        memcpy(h, YAPF__MAGIC, 4);
        h[4] = 2;
        h[5] = t.channels;   h[6] = t.format;  h[7] = t.mip_levels;
        h[8] = t.flags;      h[9] = t.payload; h[10] = t.sample; h[11] = t.shape;
        yapf__w32le(h + 0x0C, t.width);
        yapf__w32le(h + 0x10, t.height);
        yapf__w32le(h + 0x14, t.depth);
        yapf__w16le(h + 0x18, t.layers);
        yapf__w16le(h + 0x1A, t.frames);
        yapf__w32le(h + 0x1C, needs);
        memcpy(h + 0x20, t.channel_map, 4);
        h[0x24] = t.linear_mask; h[0x25] = t.normal;
        h[0x26] = t.primaries;   h[0x27] = t.transfer;
        h[0x28] = t.mip_filter;  h[0x29] = t.alpha_ref;
        yapf__w16le(h + 0x2A, t.loop_count);
        yapf__w32le(h + 0x2C, (uint32_t)notes);
        yapf__w64le(h + 0x30, total);

        /* surface data, smallest mips first */
        uint64_t pos = front;
        for (uint32_t m = t.mip_levels; m-- > 0; )
            for (uint32_t s = m; s < S; s += t.mip_levels) {
                uint8_t *ent = file + YAPF__V2_HDR + (size_t)s * YAPF__V2_SURFACE;
                yapf__w64le(ent, pos);
                yapf__w32le(ent + 8, (uint32_t)bufs[s].len);
                yapf__w32le(ent + 12, tchk[s]);
                memcpy(file + pos, bufs[s].buf, bufs[s].len);
                pos += bufs[s].len;
            }
        uint8_t *q = file + YAPF__V2_HDR + (size_t)S * YAPF__V2_SURFACE;
        if (t.frames > 1)
            for (uint32_t f = 0; f < t.frames; f++, q += 4) yapf__w32le(q, t.frame_us[f]);
        for (uint32_t i = 0; i < t.note_count; i++) {
            memcpy(q, t.notes[i].tag, 4);
            yapf__w32le(q + 4, t.notes[i].size);
            if (t.notes[i].size) memcpy(q + 8, t.notes[i].data, t.notes[i].size);
            q += 8 + t.notes[i].size;
        }
        yapf__w32le(h + 0x38, yapf_checksum(file + YAPF__V2_HDR, (size_t)(front - YAPF__V2_HDR)));
        yapf__w32le(h + 0x3C, yapf_checksum(file, 0x3C));
        *out_data = file;
        *out_size = (size_t)total;
    }

    if (bufs) for (uint32_t s = 0; s < S; s++) free(bufs[s].buf);
    free(bufs);
    free(tchk);
    free(e.sc);
    free(e.wsc);
    free(e.eb);
    free(e.tmp.buf);
    return err;
}

int yapf_tex_save(const char *filename, const yapf_texture_t *tex, int flags) {
    if (!filename) return YAPF_ERR_INVALID;
    void  *data = NULL;
    size_t size = 0;
    int    err  = yapf_tex_encode(tex, flags, &data, &size);
    if (err != YAPF_OK) return err;
    FILE *f = fopen(filename, "wb");
    if (!f) err = YAPF_ERR_IO;
    else {
        if (fwrite(data, 1, size, f) != size) err = YAPF_ERR_IO;
        if (fclose(f) != 0 && err == YAPF_OK) err = YAPF_ERR_IO;
    }
    yapf_free_buffer(data);
    return err;
}

/* ── Reading ─────────────────────────────────────────────────────────── */

static yapf_texture_t *yapf__open_fail(yapf_texture_t *t, int *err, int code) {
    if (err) *err = code;
    yapf_tex_free(t);
    return NULL;
}

/* Tiles in surface s (all slices). */
static uint64_t yapf__surface_tiles(const yapf_texture_t *t, uint32_t mip) {
    uint32_t tcx, tcy, d;
    yapf_tex_tile_grid(t, mip, &tcx, &tcy);
    yapf_tex_mip_size(t, mip, NULL, NULL, &d);
    return (uint64_t)tcx * tcy * d;
}

yapf_texture_t *yapf_tex_open(const void *buffer, size_t avail, int flags, int *err) {
    const uint8_t *b = (const uint8_t *)buffer;
    if (err) *err = YAPF_OK;
    if (!b) return yapf__open_fail(NULL, err, YAPF_ERR_INVALID);
    if (avail < 5) return yapf__open_fail(NULL, err, YAPF_ERR_TRUNCATED);
    if (memcmp(b, YAPF__MAGIC, 4) != 0) return yapf__open_fail(NULL, err, YAPF_ERR_CORRUPT);
    if (b[4] != 1 && b[4] != 2) return yapf__open_fail(NULL, err, YAPF_ERR_UNSUPPORTED);

    yapf_texture_t *t = yapf__tex_new();
    if (!t) return yapf__open_fail(NULL, err, YAPF_ERR_OOM);
    yapf__texi *ti = (yapf__texi *)t->internal_;
    ti->flags = flags;
    uint32_t S;

    if (b[4] == 1) {
        if (avail < YAPF__HDR_LEN) return yapf__open_fail(t, err, YAPF_ERR_TRUNCATED);
        t->channels = b[5]; t->format = b[6]; t->mip_levels = b[7]; t->flags = b[8];
        t->width = yapf__r32le(b + 12); t->height = yapf__r32le(b + 16);
        t->depth = 1; t->layers = 1; t->frames = 1;
        ti->version = 1;
        if (yapf__tex_check(t, &S) != 0) return yapf__open_fail(t, err, YAPF_ERR_CORRUPT);
        ti->front = YAPF__HDR_LEN + (uint64_t)S * YAPF__MIP_ENTRY;
        ti->file_size = avail;
        if (avail < ti->front) return yapf__open_fail(t, err, YAPF_ERR_TRUNCATED);
    } else {
        if (avail < YAPF__V2_HDR) return yapf__open_fail(t, err, YAPF_ERR_TRUNCATED);
        if (!(flags & YAPF_LOAD_TRUST) && yapf_checksum(b, 0x3C) != yapf__r32le(b + 0x3C))
            return yapf__open_fail(t, err, YAPF_ERR_CHECKSUM);
        t->channels = b[5]; t->format = b[6]; t->mip_levels = b[7]; t->flags = b[8];
        t->payload = b[9]; t->sample = b[10]; t->shape = b[11];
        t->width  = yapf__r32le(b + 0x0C);
        t->height = yapf__r32le(b + 0x10);
        t->depth  = yapf__r32le(b + 0x14);
        t->layers = yapf__r16le(b + 0x18);
        t->frames = yapf__r16le(b + 0x1A);
        ti->needs = yapf__r32le(b + 0x1C);
        memcpy(t->channel_map, b + 0x20, 4);
        t->linear_mask = b[0x24]; t->normal = b[0x25];
        t->primaries = b[0x26];   t->transfer = b[0x27];
        t->mip_filter = b[0x28];  t->alpha_ref = b[0x29];
        t->loop_count = yapf__r16le(b + 0x2A);
        uint32_t notes = yapf__r32le(b + 0x2C);
        ti->file_size = yapf__r64le(b + 0x30);
        ti->version = 2;

        if (ti->needs & ~(uint32_t)YAPF_NEED_ALL) return yapf__open_fail(t, err, YAPF_ERR_UNSUPPORTED);
        if (yapf__tex_check(t, &S) != 0) return yapf__open_fail(t, err, YAPF_ERR_CORRUPT);
        /* every feature in use must be announced */
        if ((t->payload == YAPF_PAYLOAD_PIXELS && t->sample != YAPF_SAMPLE_U8 && !(ti->needs & YAPF_NEED_WIDE)) ||
            (t->payload == YAPF_PAYLOAD_BLOCKS && !(ti->needs & YAPF_NEED_BLOCKS)) ||
            ((t->shape != YAPF_SHAPE_2D || t->layers > 1) && !(ti->needs & YAPF_NEED_LAYOUT)) ||
            (t->frames > 1 && !(ti->needs & YAPF_NEED_FRAMES)))
            return yapf__open_fail(t, err, YAPF_ERR_CORRUPT);

        ti->front = YAPF__V2_HDR + (uint64_t)S * YAPF__V2_SURFACE
                  + (t->frames > 1 ? 4ull * t->frames : 0) + notes;
        if (ti->front > ti->file_size) return yapf__open_fail(t, err, YAPF_ERR_CORRUPT);
        if (avail < ti->front) return yapf__open_fail(t, err, YAPF_ERR_TRUNCATED);
        if (!(flags & YAPF_LOAD_TRUST) &&
            yapf_checksum(b + YAPF__V2_HDR, (size_t)(ti->front - YAPF__V2_HDR)) != yapf__r32le(b + 0x38))
            return yapf__open_fail(t, err, YAPF_ERR_CHECKSUM);

        const uint8_t *q = b + YAPF__V2_HDR + (size_t)S * YAPF__V2_SURFACE;
        if (t->frames > 1) {
            t->frame_us = (uint32_t *)malloc(t->frames * sizeof(uint32_t));
            if (!t->frame_us) return yapf__open_fail(t, err, YAPF_ERR_OOM);
            for (uint32_t f = 0; f < t->frames; f++, q += 4) t->frame_us[f] = yapf__r32le(q);
        }
        const uint8_t *qe = q + notes;
        while (q < qe) {
            if (qe - q < 8) return yapf__open_fail(t, err, YAPF_ERR_CORRUPT);
            uint32_t sz = yapf__r32le(q + 4);
            if ((size_t)(qe - q - 8) < sz) return yapf__open_fail(t, err, YAPF_ERR_CORRUPT);
            if (yapf_tex_add_note(t, (const char *)q, q + 8, sz) != YAPF_OK)
                return yapf__open_fail(t, err, YAPF_ERR_OOM);
            q += 8 + sz;
        }
    }

    t->data = (void **)calloc(S, sizeof(void *));
    ti->off = (uint64_t *)calloc(S, sizeof(uint64_t));
    ti->size = (uint32_t *)calloc(S, sizeof(uint32_t));
    ti->tcheck = (uint32_t *)calloc(S, sizeof(uint32_t));
    if (!t->data || !ti->off || !ti->size || !ti->tcheck)
        return yapf__open_fail(t, err, YAPF_ERR_OOM);
    for (uint32_t s = 0; s < S; s++) {
        uint32_t mip = s % t->mip_levels;
        if (ti->version == 1) {
            const uint8_t *e = b + YAPF__HDR_LEN + (size_t)s * YAPF__MIP_ENTRY;
            ti->off[s]  = yapf__r32le(e);
            ti->size[s] = yapf__r32le(e + 4);
        } else {
            const uint8_t *e = b + YAPF__V2_HDR + (size_t)s * YAPF__V2_SURFACE;
            ti->off[s]    = yapf__r64le(e);
            ti->size[s]   = yapf__r32le(e + 8);
            ti->tcheck[s] = yapf__r32le(e + 12);
            if (ti->off[s] > ti->file_size || ti->size[s] > ti->file_size - ti->off[s])
                return yapf__open_fail(t, err, YAPF_ERR_CORRUPT);
            if (ti->size[s] < 4 || (ti->size[s] - 4) / 8 < yapf__surface_tiles(t, mip))
                return yapf__open_fail(t, err, YAPF_ERR_CORRUPT);
        }
        if (ti->off[s] < ti->front) return yapf__open_fail(t, err, YAPF_ERR_CORRUPT);
    }
    return t;
}

uint64_t yapf_tex_stream_bytes(const yapf_texture_t *tex, uint32_t first_mip) {
    if (!tex || !tex->internal_) return 0;
    const yapf__texi *ti = (const yapf__texi *)tex->internal_;
    if (!ti->off) return 0;
    uint32_t S = yapf_tex_surface_count(tex);
    uint64_t end = ti->front;
    for (uint32_t s = 0; s < S; s++)
        if (s % tex->mip_levels >= first_mip && ti->off[s] + ti->size[s] > end)
            end = ti->off[s] + ti->size[s];
    return end;
}

/* Prepares the level for surface s; alloc: allocate tex->data[s]. */
static int yapf__surface_level(yapf_texture_t *t, const uint8_t *buf, size_t avail,
                               uint32_t s, yapf__level *lv, int alloc)
{
    const yapf__texi *ti = (const yapf__texi *)t->internal_;
    uint32_t mip = s % t->mip_levels, w, h, d;
    memset(lv, 0, sizeof(*lv));
    if (ti->off[s] + ti->size[s] > avail) return YAPF_ERR_TRUNCATED;
    uint64_t       tiles = yapf__surface_tiles(t, mip);
    const uint8_t *sd = buf + ti->off[s];
    size_t         ssz = ti->size[s], pre = 0;
    if (ti->version == 2) {
        /* u32 book size, book, tile table: covered by the tile-table check */
        uint32_t bl = yapf__r32le(sd);
        if (ssz < 4 || bl > ssz - 4 || (ssz - 4 - bl) / 8 < tiles) return YAPF_ERR_CORRUPT;
        pre = 4 + (size_t)bl;
        if (!(ti->flags & YAPF_LOAD_TRUST) && yapf_checksum(sd, pre + (size_t)tiles * 8) != ti->tcheck[s])
            return YAPF_ERR_CHECKSUM;
        if (bl) {
            if (!(ti->needs & YAPF_NEED_ENTROPY)) return YAPF_ERR_CORRUPT;
            lv->book = (yapf__dbook *)malloc(sizeof(yapf__dbook));
            if (!lv->book) return YAPF_ERR_OOM;
            if (yapf__book_read(sd + 4, bl, lv->book) != 0) return YAPF_ERR_CORRUPT;
        }
    }
    if (alloc && !t->data[s]) {
        t->data[s] = malloc(yapf_tex_surface_bytes(t, mip));
        if (!t->data[s]) return YAPF_ERR_OOM;
    }
    yapf_tex_mip_size(t, mip, &w, &h, &d);
    lv->data   = sd + pre;
    lv->pixels = (uint8_t *)t->data[s];
    lv->mw = w; lv->mh = h; lv->md = d;
    lv->ch = t->channels;
    lv->sample = t->sample;
    lv->v2 = ti->version == 2;
    lv->verify = !(ti->flags & YAPF_LOAD_TRUST);
#if YAPF__SIMD
    lv->simd = yapf__simd_available();
#endif
    yapf_tex_tile_grid(t, mip, &lv->tcx, &lv->tcy);
    if (t->payload == YAPF_PAYLOAD_BLOCKS) {
        uint32_t S;
        yapf__mip_blocks(t, mip, &lv->nbx, &lv->nby, &S);
        lv->blk = S;
    }
    if (yapf__level_table(lv, ssz - pre) != 0) return YAPF_ERR_CORRUPT;
    return YAPF_OK;
}

static int yapf__map_rc(int rc) {
    return rc == 0 ? YAPF_OK : rc == YAPF_ERR_CHECKSUM || rc == YAPF_ERR_OOM ? rc : YAPF_ERR_CORRUPT;
}

int yapf_tex_decode_surface(yapf_texture_t *tex, const void *buffer, size_t avail,
                            uint32_t surface, int threads)
{
    if (!tex || !tex->internal_ || !buffer) return YAPF_ERR_INVALID;
    if (!((yapf__texi *)tex->internal_)->off) return YAPF_ERR_INVALID;
    if (surface >= yapf_tex_surface_count(tex)) return YAPF_ERR_INVALID;
    yapf__level lv;
    int rc = yapf__surface_level(tex, (const uint8_t *)buffer, avail, surface, &lv, 1);
    if (rc == YAPF_OK) rc = yapf__map_rc(yapf__decode_levels(&lv, 1, threads));
    yapf__level_free(&lv);
    return rc;
}

int yapf_tex_decode_tile(const yapf_texture_t *tex, const void *buffer, size_t avail,
                         uint32_t surface, uint32_t tx, uint32_t ty, uint32_t slice,
                         void *out, size_t stride,
                         uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{
    if (!tex || !tex->internal_ || !buffer || !out) return YAPF_ERR_INVALID;
    if (!((const yapf__texi *)tex->internal_)->off) return YAPF_ERR_INVALID;
    if (surface >= yapf_tex_surface_count(tex)) return YAPF_ERR_INVALID;
    uint32_t mip = surface % tex->mip_levels, mw, mh, md, tcx, tcy;
    yapf_tex_mip_size(tex, mip, &mw, &mh, &md);
    yapf_tex_tile_grid(tex, mip, &tcx, &tcy);
    if (tx >= tcx || ty >= tcy || slice >= md) return YAPF_ERR_INVALID;

    /* unit: bytes per texel (or block); the row must hold the tile */
    size_t   unit;
    uint32_t span, bw = 1, bh = 1, across;
    if (tex->payload == YAPF_PAYLOAD_BLOCKS) {
        uint32_t bytes = 16, nbx, nby, S;
        yapf_block_info(tex->format, &bw, &bh, &bytes);
        yapf__mip_blocks(tex, mip, &nbx, &nby, &S);
        unit = bytes;
        span = YAPF_BLOCK_TILE;
        across = nbx;
    } else {
        unit = (size_t)tex->channels * yapf__sbytes(tex->sample);
        span = YAPF_TILE_SIZE;
        across = mw;
    }
    {
        uint32_t need = across - tx * span < span ? across - tx * span : span;
        if (stride % unit != 0 || stride / unit < need) return YAPF_ERR_INVALID;
    }
    yapf__level lv;
    int rc = yapf__surface_level((yapf_texture_t *)tex, (const uint8_t *)buffer, avail,
                                 surface, &lv, 0);
    if (rc == YAPF_OK) {
        yapf__wdec *wsc = NULL;
        if (!lv.blk && lv.sample != YAPF_SAMPLE_U8) {
            wsc = (yapf__wdec *)malloc(sizeof(yapf__wdec));
            if (!wsc) rc = YAPF_ERR_OOM;
        }
        if (rc == YAPF_OK)
            rc = yapf__map_rc(yapf__level_tile(&lv, tx, slice * lv.tcy + ty, wsc,
                                               (uint8_t *)out, stride / unit));
        free(wsc);
    }
    yapf__level_free(&lv);
    if (rc == YAPF_OK) {
        uint32_t px = tx * span * bw, py = ty * span * bh;
        uint32_t pw = span * bw, ph = span * bh;
        if (x) *x = px;
        if (y) *y = py;
        if (w) *w = mw - px < pw ? mw - px : pw;
        if (h) *h = mh - py < ph ? mh - py : ph;
    }
    return rc;
}

yapf_texture_t *yapf_tex_load_memory(const void *buffer, size_t size,
                                     int flags, int threads, int *err)
{
    yapf_texture_t *t = yapf_tex_open(buffer, size, flags, err);
    if (!t) return NULL;
    yapf__texi *ti = (yapf__texi *)t->internal_;
    if (ti->version == 2 && size < ti->file_size)
        return yapf__open_fail(t, err, YAPF_ERR_TRUNCATED);
    uint32_t     S  = yapf_tex_surface_count(t);
    yapf__level *lv = (yapf__level *)calloc(S, sizeof(yapf__level));
    int          rc = lv ? YAPF_OK : YAPF_ERR_OOM;
    for (uint32_t s = 0; s < S && rc == YAPF_OK; s++)
        rc = yapf__surface_level(t, (const uint8_t *)buffer, size, s, &lv[s], 1);
    if (rc == YAPF_ERR_TRUNCATED) rc = YAPF_ERR_CORRUPT;
    if (rc == YAPF_OK) rc = yapf__map_rc(yapf__decode_levels(lv, (int)S, threads));
    if (lv) for (uint32_t s = 0; s < S; s++) yapf__level_free(&lv[s]);
    free(lv);
    if (rc != YAPF_OK) return yapf__open_fail(t, err, rc);
    return t;
}

yapf_texture_t *yapf_tex_load(const char *filename, int flags, int threads, int *err) {
    size_t   len = 0;
    uint8_t *buf = yapf__read_file(filename, &len);
    if (!buf) { if (err) *err = YAPF_ERR_IO; return NULL; }
    yapf_texture_t *t = yapf_tex_load_memory(buf, len, flags, threads, err);
    free(buf);
    return t;
}

/* v1 API on a version 2 file: frame 0, layer 0, face 0 of an 8-bit
 * pixel texture (not volumes), with its mips. */
static yapf_image_t *yapf__load_v2_image(const uint8_t *buf, size_t size, int threads) {
    yapf_texture_t *t = yapf_tex_open(buf, size, 0, NULL);
    if (!t) return NULL;
    yapf__texi *ti = (yapf__texi *)t->internal_;
    if (t->payload != YAPF_PAYLOAD_PIXELS || t->sample != YAPF_SAMPLE_U8 ||
        t->shape == YAPF_SHAPE_VOLUME || size < ti->file_size) {
        yapf_tex_free(t);
        return NULL;
    }
    yapf__level lv[YAPF_MAX_MIPS];
    int rc = YAPF_OK;
    memset(lv, 0, sizeof(lv));
    for (uint32_t m = 0; m < t->mip_levels && rc == YAPF_OK; m++)
        rc = yapf__surface_level(t, buf, size, m, &lv[m], 1);
    if (rc == YAPF_OK) rc = yapf__decode_levels(lv, t->mip_levels, threads);
    for (uint32_t m = 0; m < YAPF_MAX_MIPS; m++) yapf__level_free(&lv[m]);

    yapf_image_t *img = NULL;
    if (rc == 0) img = (yapf_image_t *)calloc(1, sizeof(yapf_image_t));
    if (img) img->mips = (uint8_t **)calloc(t->mip_levels, sizeof(uint8_t *));
    if (img && img->mips) {
        img->width = t->width; img->height = t->height; img->channels = t->channels;
        img->gpu_format = t->format; img->flags = t->flags; img->mip_levels = t->mip_levels;
        for (uint32_t m = 0; m < t->mip_levels; m++) {
            img->mips[m] = (uint8_t *)t->data[m];   /* surface m: frame 0, layer 0, face 0 */
            t->data[m] = NULL;
        }
        img->pixels = img->mips[0];
    } else if (img) {
        free(img);
        img = NULL;
    }
    yapf_tex_free(t);
    return img;
}

/* ========================================================================
 *  Half floats
 * ======================================================================== */

uint16_t yapf_float_to_half(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u, e = (x >> 23) & 0xFFu, m = x & 0x7FFFFFu;
    if (e == 255) return (uint16_t)(sign | 0x7C00u | (m ? 0x200u | (m >> 13) : 0));
    int32_t ee = (int32_t)e - 127 + 15;
    if (ee >= 31) return (uint16_t)(sign | 0x7C00u);
    if (ee <= 0) {                                   /* subnormal or zero */
        if (ee < -10) return (uint16_t)sign;
        m |= 0x800000u;
        int      shift = 14 - ee;
        uint32_t hm = m >> shift, rem = m & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (hm & 1))) hm++;
        return (uint16_t)(sign | hm);
    }
    uint32_t h = sign | ((uint32_t)ee << 10) | (m >> 13), rem = m & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1))) h++;   /* carries into the exponent */
    return (uint16_t)h;
}

float yapf_half_to_float(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000u) << 16, e = (h >> 10) & 31u, m = h & 0x3FFu, x;
    if (e == 0) {
        if (!m) x = s;
        else {
            e = 113;
            while (!(m & 0x400u)) { m <<= 1; e--; }
            x = s | (e << 23) | ((m & 0x3FFu) << 13);
        }
    } else if (e == 31) {
        x = s | 0x7F800000u | (m << 13);
    } else {
        x = s | ((e + 112) << 23) | (m << 13);
    }
    float f;
    memcpy(&f, &x, 4);
    return f;
}

/* ========================================================================
 *  GPU block codecs (BC1–BC7)
 * ======================================================================== */

/* BC6H / BC7 partitions.  2 subsets: bit i set = pixel i in subset 1.
 * 3 subsets: 2 bits per pixel.  Anchors: the pixel of each extra subset
 * whose index drops its top bit. */
static const uint16_t yapf__part2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80,
    0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
    0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE,
    0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
    0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A,
    0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
    0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C,
    0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22
};

static const uint32_t yapf__part3[64] = {
    0xAA685050, 0x6A5A5040, 0x5A5A4200, 0x5450A0A8, 0xA5A50000, 0xA0A05050, 0x5555A0A0, 0x5A5A5050,
    0xAA550000, 0xAA555500, 0xAAAA5500, 0x90909090, 0x94949494, 0xA4A4A4A4, 0xA9A59450, 0x2A0A4250,
    0xA5945040, 0x0A425054, 0xA5A5A500, 0x55A0A0A0, 0xA8A85454, 0x6A6A4040, 0xA4A45000, 0x1A1A0500,
    0x0050A4A4, 0xAAA59090, 0x14696914, 0x69691400, 0xA08585A0, 0xAA821414, 0x50A4A450, 0x6A5A0200,
    0xA9A58000, 0x5090A0A8, 0xA8A09050, 0x24242424, 0x00AA5500, 0x24924924, 0x24499224, 0x50A50A50,
    0x500AA550, 0xAAAA4444, 0x66660000, 0xA5A0A5A0, 0x50A050A0, 0x69286928, 0x44AAAA44, 0x66666600,
    0xAA444444, 0x54A854A8, 0x95809580, 0x96969600, 0xA85454A8, 0x80959580, 0xAA141414, 0x96960000,
    0xAAAA1414, 0xA05050A0, 0xA0A5A5A0, 0x96000000, 0x40804080, 0xA9A8A9A8, 0xAAAAAA44, 0x2A4A5254
};

static const uint8_t yapf__anchor2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15,  2,  8,  2,  2,  8,  8, 15,  2,  8,  2,  2,  8,  8,  2,  2,
    15, 15,  6,  8,  2,  8, 15, 15,  2,  8,  2,  2,  2, 15, 15,  6,
     6,  2,  6,  8, 15, 15,  2,  2, 15, 15, 15, 15, 15,  2,  2, 15
};
static const uint8_t yapf__anchor3a[64] = {
     3,  3, 15, 15,  8,  3, 15, 15,  8,  8,  6,  6,  6,  5,  3,  3,
     3,  3,  8, 15,  3,  3,  6, 10,  5,  8,  8,  6,  8,  5, 15, 15,
     8, 15,  3,  5,  6, 10,  8, 15, 15,  3, 15,  5, 15, 15, 15, 15,
     3, 15,  5,  5,  5,  8,  5, 10,  5, 10,  8, 13, 15, 12,  3,  3
};
static const uint8_t yapf__anchor3b[64] = {
    15,  8,  8,  3, 15, 15,  3,  8, 15, 15, 15, 15, 15, 15, 15,  8,
    15,  8, 15,  3, 15,  8, 15,  8,  3, 15,  6, 10, 15, 15, 10,  8,
    15,  3, 15, 10, 10,  8,  9, 10,  6, 15,  8, 15,  3,  6,  6,  8,
    15,  3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,  3, 15, 15,  8
};

static const uint8_t yapf__w2[4]  = { 0, 21, 43, 64 };
static const uint8_t yapf__w3[8]  = { 0, 9, 18, 27, 37, 46, 55, 64 };
static const uint8_t yapf__w4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

static const uint8_t *yapf__weights(int bits) {
    return bits == 2 ? yapf__w2 : bits == 3 ? yapf__w3 : yapf__w4;
}

static int yapf__bc_interp(int a, int b, int w) {
    return (a * (64 - w) + b * w + 32) >> 6;
}

/* 128-bit little-endian bit reader / writer */
static uint32_t yapf__getbits(const uint8_t *b, unsigned *pos, unsigned n) {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++, (*pos)++)
        v |= (uint32_t)((b[*pos >> 3] >> (*pos & 7)) & 1u) << i;
    return v;
}

static void yapf__putbits(uint8_t *b, unsigned *pos, uint32_t v, unsigned n) {
    for (unsigned i = 0; i < n; i++, (*pos)++)
        if ((v >> i) & 1u) b[*pos >> 3] |= (uint8_t)(1u << (*pos & 7));
}

/* ── BC1–BC5 ─────────────────────────────────────────────────────────── */

static void yapf__565(uint16_t c, int *rgb) {
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    rgb[0] = (r << 3) | (r >> 2);
    rgb[1] = (g << 2) | (g >> 4);
    rgb[2] = (b << 3) | (b >> 2);
}

/* BC1 colour block; four_only: BC2 / BC3 always use four colours. */
static void yapf__bc1_block(const uint8_t *s, uint8_t out[16][4], int four_only) {
    uint16_t c0 = yapf__r16le(s), c1 = yapf__r16le(s + 2);
    uint32_t idx = yapf__r32le(s + 4);
    int      pal[4][4];
    yapf__565(c0, pal[0]);
    yapf__565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
    for (int k = 0; k < 3; k++) {
        if (c0 > c1 || four_only) {
            pal[2][k] = (2 * pal[0][k] + pal[1][k] + 1) / 3;
            pal[3][k] = (pal[0][k] + 2 * pal[1][k] + 1) / 3;
        } else {
            pal[2][k] = (pal[0][k] + pal[1][k] + 1) / 2;
            pal[3][k] = 0;
        }
    }
    if (!(c0 > c1 || four_only)) pal[3][3] = 0;
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 4; k++) out[i][k] = (uint8_t)pal[(idx >> (2 * i)) & 3][k];
}

/* BC4 channel block into out[i][ch]; snorm values are shifted by +128. */
static void yapf__bc4_block(const uint8_t *s, uint8_t out[16][4], int ch, int snorm) {
    int      pal[8];
    uint64_t idx = 0;
    for (int k = 0; k < 6; k++) idx |= (uint64_t)s[2 + k] << (8 * k);
    if (!snorm) {
        int r0 = s[0], r1 = s[1];
        pal[0] = r0; pal[1] = r1;
        if (r0 > r1) for (int i = 2; i < 8; i++) pal[i] = ((8 - i) * r0 + (i - 1) * r1 + 3) / 7;
        else {
            for (int i = 2; i < 6; i++) pal[i] = ((6 - i) * r0 + (i - 1) * r1 + 2) / 5;
            pal[6] = 0; pal[7] = 255;
        }
    } else {
        int r0 = (int8_t)s[0], r1 = (int8_t)s[1];
        if (r0 < -127) r0 = -127;
        if (r1 < -127) r1 = -127;
        pal[0] = r0; pal[1] = r1;
        if (r0 > r1) {
            for (int i = 2; i < 8; i++) {
                int v = (8 - i) * r0 + (i - 1) * r1;
                pal[i] = v >= 0 ? (v + 3) / 7 : -((-v + 3) / 7);
            }
        } else {
            for (int i = 2; i < 6; i++) {
                int v = (6 - i) * r0 + (i - 1) * r1;
                pal[i] = v >= 0 ? (v + 2) / 5 : -((-v + 2) / 5);
            }
            pal[6] = -127; pal[7] = 127;
        }
        for (int i = 0; i < 8; i++) pal[i] += 128;
    }
    for (int i = 0; i < 16; i++) out[i][ch] = (uint8_t)pal[(idx >> (3 * i)) & 7];
}

/* ── BC7 ─────────────────────────────────────────────────────────────── */

static const struct {
    uint8_t ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2;
} yapf__bc7_modes[8] = {
    { 3, 4, 0, 0, 4, 0, 1, 0, 3, 0 }, { 2, 6, 0, 0, 6, 0, 0, 1, 3, 0 },
    { 3, 6, 0, 0, 5, 0, 0, 0, 2, 0 }, { 2, 6, 0, 0, 7, 0, 1, 0, 2, 0 },
    { 1, 0, 2, 1, 5, 6, 0, 0, 2, 3 }, { 1, 0, 2, 0, 7, 8, 0, 0, 2, 2 },
    { 1, 0, 0, 0, 7, 7, 1, 0, 4, 0 }, { 2, 6, 0, 0, 5, 5, 1, 0, 2, 0 }
};

static int yapf__subset(int ns, int part, int i) {
    if (ns == 2) return (yapf__part2[part] >> i) & 1;
    if (ns == 3) return (int)((yapf__part3[part] >> (2 * i)) & 3);
    return 0;
}

static int yapf__is_anchor(int ns, int part, int i) {
    if (i == 0) return 1;
    if (ns == 2) return i == yapf__anchor2[part];
    if (ns == 3) return i == yapf__anchor3a[part] || i == yapf__anchor3b[part];
    return 0;
}

static void yapf__bc7_block(const uint8_t *s, uint8_t out[16][4]) {
    unsigned pos = 0;
    int      mode = 0;
    while (mode < 8 && !yapf__getbits(s, &pos, 1)) mode++;
    if (mode == 8) { memset(out, 0, 64); return; }
    int ns = yapf__bc7_modes[mode].ns, cb = yapf__bc7_modes[mode].cb, ab = yapf__bc7_modes[mode].ab;
    int part = (int)yapf__getbits(s, &pos, yapf__bc7_modes[mode].pb);
    int rot  = (int)yapf__getbits(s, &pos, yapf__bc7_modes[mode].rb);
    int isb  = (int)yapf__getbits(s, &pos, yapf__bc7_modes[mode].isb);
    int ne = ns * 2, ep[6][4];
    for (int c = 0; c < 3; c++)
        for (int e = 0; e < ne; e++) ep[e][c] = (int)yapf__getbits(s, &pos, cb);
    for (int e = 0; e < ne; e++) ep[e][3] = ab ? (int)yapf__getbits(s, &pos, ab) : 255;
    int pbit = yapf__bc7_modes[mode].epb | yapf__bc7_modes[mode].spb;
    if (yapf__bc7_modes[mode].epb) {
        for (int e = 0; e < ne; e++) {
            int p = (int)yapf__getbits(s, &pos, 1);
            for (int c = 0; c < 4; c++) if (c < 3 || ab) ep[e][c] = (ep[e][c] << 1) | p;
        }
    } else if (yapf__bc7_modes[mode].spb) {
        for (int k = 0; k < ns; k++) {
            int p = (int)yapf__getbits(s, &pos, 1);
            for (int c = 0; c < 3; c++) {
                ep[2 * k][c] = (ep[2 * k][c] << 1) | p;
                ep[2 * k + 1][c] = (ep[2 * k + 1][c] << 1) | p;
            }
        }
    }
    for (int e = 0; e < ne; e++)
        for (int c = 0; c < 4; c++) {
            int bits = (c < 3 ? cb : ab) + pbit;
            if (c == 3 && !ab) continue;
            int v = ep[e][c] << (8 - bits);
            ep[e][c] = v | (v >> bits);
        }
    int ib = yapf__bc7_modes[mode].ib, ib2 = yapf__bc7_modes[mode].ib2;
    int idx[16], idx2[16];
    for (int i = 0; i < 16; i++)
        idx[i] = (int)yapf__getbits(s, &pos, ib - yapf__is_anchor(ns, part, i));
    for (int i = 0; i < 16; i++)
        idx2[i] = ib2 ? (int)yapf__getbits(s, &pos, ib2 - (i == 0)) : 0;
    const uint8_t *w1 = yapf__weights(ib), *w2 = yapf__weights(ib2 ? ib2 : 2);
    for (int i = 0; i < 16; i++) {
        int k = yapf__subset(ns, part, i), px[4];
        const int *a = ep[2 * k], *b = ep[2 * k + 1];
        int cw, aw;
        if (!ib2)      { cw = w1[idx[i]];  aw = w1[idx[i]]; }
        else if (!isb) { cw = w1[idx[i]];  aw = w2[idx2[i]]; }
        else           { cw = w2[idx2[i]]; aw = w1[idx[i]]; }
        for (int c = 0; c < 3; c++) px[c] = yapf__bc_interp(a[c], b[c], cw);
        px[3] = yapf__bc_interp(a[3], b[3], aw);
        if (rot) { int t = px[3]; px[3] = px[rot - 1]; px[rot - 1] = t; }
        for (int c = 0; c < 4; c++) out[i][c] = (uint8_t)px[c];
    }
}

/* ── BC6H ────────────────────────────────────────────────────────────── */
/*
 * Header layouts as sequences of (field, first bit, bit count), in the
 * order they appear after the mode bits.  Fields: 0–3 red w, x, y, z;
 * 4–7 green; 8–11 blue; 12 partition.  The same table drives the reader
 * and the encoder's writer.
 */
#define YAPF__F(f, lo, n) { f, lo, n }
typedef struct { uint8_t f, lo, n; } yapf__seg;
enum { RW, RX, RY, RZ, GW, GX, GY, GZ, BW, BX, BY, BZ, PD };

static const yapf__seg yapf__bc6_m0[] = {
    {GY,4,1},{BY,4,1},{BZ,4,1},{RW,0,10},{GW,0,10},{BW,0,10},{RX,0,5},{GZ,4,1},{GY,0,4},
    {GX,0,5},{BZ,0,1},{GZ,0,4},{BX,0,5},{BZ,1,1},{BY,0,4},{RY,0,5},{BZ,2,1},{RZ,0,5},{BZ,3,1},{PD,0,5},{0,0,0} };
static const yapf__seg yapf__bc6_m1[] = {
    {GY,5,1},{GZ,4,1},{GZ,5,1},{RW,0,7},{BZ,0,1},{BZ,1,1},{BY,4,1},{GW,0,7},{BY,5,1},{BZ,2,1},
    {GY,4,1},{BW,0,7},{BZ,3,1},{BZ,5,1},{BZ,4,1},{RX,0,6},{GY,0,4},{GX,0,6},{GZ,0,4},{BX,0,6},
    {BY,0,4},{RY,0,6},{RZ,0,6},{PD,0,5},{0,0,0} };
static const yapf__seg yapf__bc6_m2[] = {
    {RW,0,10},{GW,0,10},{BW,0,10},{RX,0,5},{RW,10,1},{GY,0,4},{GX,0,4},{GW,10,1},{BZ,0,1},{GZ,0,4},
    {BX,0,4},{BW,10,1},{BZ,1,1},{BY,0,4},{RY,0,5},{BZ,2,1},{RZ,0,5},{BZ,3,1},{PD,0,5},{0,0,0} };
static const yapf__seg yapf__bc6_m3[] = {
    {RW,0,10},{GW,0,10},{BW,0,10},{RX,0,4},{RW,10,1},{GZ,4,1},{GY,0,4},{GX,0,5},{GW,10,1},{GZ,0,4},
    {BX,0,4},{BW,10,1},{BZ,1,1},{BY,0,4},{RY,0,4},{BZ,0,1},{BZ,2,1},{RZ,0,4},{GY,4,1},{BZ,3,1},{PD,0,5},{0,0,0} };
static const yapf__seg yapf__bc6_m4[] = {
    {RW,0,10},{GW,0,10},{BW,0,10},{RX,0,4},{RW,10,1},{BY,4,1},{GY,0,4},{GX,0,4},{GW,10,1},{BZ,0,1},
    {GZ,0,4},{BX,0,5},{BW,10,1},{BY,0,4},{RY,0,4},{BZ,1,1},{BZ,2,1},{RZ,0,4},{BZ,4,1},{BZ,3,1},{PD,0,5},{0,0,0} };
static const yapf__seg yapf__bc6_m5[] = {
    {RW,0,9},{BY,4,1},{GW,0,9},{GY,4,1},{BW,0,9},{BZ,4,1},{RX,0,5},{GZ,4,1},{GY,0,4},{GX,0,5},
    {BZ,0,1},{GZ,0,4},{BX,0,5},{BZ,1,1},{BY,0,4},{RY,0,5},{BZ,2,1},{RZ,0,5},{BZ,3,1},{PD,0,5},{0,0,0} };
static const yapf__seg yapf__bc6_m6[] = {
    {RW,0,8},{GZ,4,1},{BY,4,1},{GW,0,8},{BZ,2,1},{GY,4,1},{BW,0,8},{BZ,3,1},{BZ,4,1},{RX,0,6},
    {GY,0,4},{GX,0,5},{BZ,0,1},{GZ,0,4},{BX,0,5},{BZ,1,1},{BY,0,4},{RY,0,6},{RZ,0,6},{PD,0,5},{0,0,0} };
static const yapf__seg yapf__bc6_m7[] = {
    {RW,0,8},{BZ,0,1},{BY,4,1},{GW,0,8},{GY,5,1},{GY,4,1},{BW,0,8},{GZ,5,1},{BZ,4,1},{RX,0,5},
    {GZ,4,1},{GY,0,4},{GX,0,6},{GZ,0,4},{BX,0,5},{BZ,1,1},{BY,0,4},{RY,0,5},{BZ,2,1},{RZ,0,5},{BZ,3,1},{PD,0,5},{0,0,0} };
static const yapf__seg yapf__bc6_m8[] = {
    {RW,0,8},{BZ,1,1},{BY,4,1},{GW,0,8},{BY,5,1},{GY,4,1},{BW,0,8},{BZ,5,1},{BZ,4,1},{RX,0,5},
    {GZ,4,1},{GY,0,4},{GX,0,5},{BZ,0,1},{GZ,0,4},{BX,0,6},{BY,0,4},{RY,0,5},{BZ,2,1},{RZ,0,5},{BZ,3,1},{PD,0,5},{0,0,0} };
static const yapf__seg yapf__bc6_m9[] = {
    {RW,0,6},{GZ,4,1},{BZ,0,1},{BZ,1,1},{BY,4,1},{GW,0,6},{GY,5,1},{BY,5,1},{BZ,2,1},{GY,4,1},
    {BW,0,6},{GZ,5,1},{BZ,3,1},{BZ,5,1},{BZ,4,1},{RX,0,6},{GY,0,4},{GX,0,6},{GZ,0,4},{BX,0,6},
    {BY,0,4},{RY,0,6},{RZ,0,6},{PD,0,5},{0,0,0} };
static const yapf__seg yapf__bc6_m10[] = {
    {RW,0,10},{GW,0,10},{BW,0,10},{RX,0,10},{GX,0,10},{BX,0,10},{0,0,0} };
static const yapf__seg yapf__bc6_m11[] = {
    {RW,0,10},{GW,0,10},{BW,0,10},{RX,0,9},{RW,10,1},{GX,0,9},{GW,10,1},{BX,0,9},{BW,10,1},{0,0,0} };
static const yapf__seg yapf__bc6_m12[] = {
    {RW,0,10},{GW,0,10},{BW,0,10},{RX,0,8},{RW,11,1},{RW,10,1},{GX,0,8},{GW,11,1},{GW,10,1},
    {BX,0,8},{BW,11,1},{BW,10,1},{0,0,0} };
static const yapf__seg yapf__bc6_m13[] = {
    {RW,0,10},{GW,0,10},{BW,0,10},{RX,0,4},{RW,15,1},{RW,14,1},{RW,13,1},{RW,12,1},{RW,11,1},{RW,10,1},
    {GX,0,4},{GW,15,1},{GW,14,1},{GW,13,1},{GW,12,1},{GW,11,1},{GW,10,1},
    {BX,0,4},{BW,15,1},{BW,14,1},{BW,13,1},{BW,12,1},{BW,11,1},{BW,10,1},{0,0,0} };

static const struct {
    uint8_t          code, codebits, w, dr, dg, db;
    const yapf__seg *seg;
} yapf__bc6_modes[14] = {
    { 0x00, 2, 10, 5, 5, 5, yapf__bc6_m0 },  { 0x01, 2, 7, 6, 6, 6, yapf__bc6_m1 },
    { 0x02, 5, 11, 5, 4, 4, yapf__bc6_m2 },  { 0x06, 5, 11, 4, 5, 4, yapf__bc6_m3 },
    { 0x0A, 5, 11, 4, 4, 5, yapf__bc6_m4 },  { 0x0E, 5, 9, 5, 5, 5, yapf__bc6_m5 },
    { 0x12, 5, 8, 6, 5, 5, yapf__bc6_m6 },   { 0x16, 5, 8, 5, 6, 5, yapf__bc6_m7 },
    { 0x1A, 5, 8, 5, 5, 6, yapf__bc6_m8 },   { 0x1E, 5, 6, 6, 6, 6, yapf__bc6_m9 },
    { 0x03, 5, 10, 10, 10, 10, yapf__bc6_m10 }, { 0x07, 5, 11, 9, 9, 9, yapf__bc6_m11 },
    { 0x0B, 5, 12, 8, 8, 8, yapf__bc6_m12 }, { 0x0F, 5, 16, 4, 4, 4, yapf__bc6_m13 }
};

static int yapf__sext(int v, int bits) {
    int m = 1 << (bits - 1);
    v &= (1 << bits) - 1;
    return (v ^ m) - m;
}

static int yapf__bc6_unq(int v, int bits, int sgn) {
    if (!sgn) {
        if (bits >= 15) return v;
        if (v == 0) return 0;
        if (v == (1 << bits) - 1) return 0xFFFF;
        return ((v << 16) + 0x8000) >> bits;
    }
    if (bits >= 16) return v;
    int s = v < 0;
    if (s) v = -v;
    int u;
    if (v == 0) u = 0;
    else if (v >= (1 << (bits - 1)) - 1) u = 0x7FFF;
    else u = ((v << 15) + 0x4000) >> (bits - 1);
    return s ? -u : u;
}

static uint16_t yapf__bc6_finish(int v, int sgn) {
    if (!sgn) return (uint16_t)((v * 31) >> 6);
    v = v < 0 ? -(((-v) * 31) >> 5) : (v * 31) >> 5;
    return (uint16_t)(v < 0 ? 0x8000 | -v : v);
}

static void yapf__bc6_block(const uint8_t *s, uint16_t out[16][3], int sgn) {
    unsigned pos = 0;
    int      code = (int)yapf__getbits(s, &pos, 2), m = -1;
    if (code > 1) code |= (int)yapf__getbits(s, &pos, 3) << 2;
    for (int i = 0; i < 14; i++)
        if (yapf__bc6_modes[i].code == code) { m = i; break; }
    if (m < 0) { memset(out, 0, 96); return; }         /* reserved modes */

    int f[13] = {0};
    for (const yapf__seg *g = yapf__bc6_modes[m].seg; g->n; g++)
        f[g->f] |= (int)yapf__getbits(s, &pos, g->n) << g->lo;
    int W = yapf__bc6_modes[m].w, regions = m < 10 ? 2 : 1, ne = regions * 2;
    int db[3] = { yapf__bc6_modes[m].dr, yapf__bc6_modes[m].dg, yapf__bc6_modes[m].db };
    int ep[4][3];
    for (int c = 0; c < 3; c++) {
        for (int e = 0; e < ne; e++) ep[e][c] = f[c * 4 + e];
        if (sgn) ep[0][c] = yapf__sext(ep[0][c], W);
        int delta = m != 9 && m != 10;
        for (int e = 1; e < ne; e++) {
            if (delta || sgn) ep[e][c] = yapf__sext(ep[e][c], db[c]);
            if (delta) {
                ep[e][c] = (ep[e][c] + ep[0][c]) & ((1 << W) - 1);
                if (sgn) ep[e][c] = yapf__sext(ep[e][c], W);
            }
        }
        for (int e = 0; e < ne; e++) ep[e][c] = yapf__bc6_unq(ep[e][c], W, sgn);
    }
    int part = f[PD], ib = regions == 2 ? 3 : 4;
    const uint8_t *wt = yapf__weights(ib);
    for (int i = 0; i < 16; i++) {
        int k = regions == 2 ? (yapf__part2[part] >> i) & 1 : 0;
        int anchor = i == 0 || (regions == 2 && i == yapf__anchor2[part]);
        int idx = (int)yapf__getbits(s, &pos, ib - anchor);
        for (int c = 0; c < 3; c++)
            out[i][c] = yapf__bc6_finish(yapf__bc_interp(ep[2 * k][c], ep[2 * k + 1][c], wt[idx]), sgn);
    }
}

int yapf_blocks_decode(uint8_t format, const void *blocks, uint32_t w, uint32_t h, void *out) {
    uint32_t bw, bh, S;
    if (!blocks || !out || !w || !h) return YAPF_ERR_INVALID;
    if (!yapf_block_info(format, &bw, &bh, &S)) return YAPF_ERR_INVALID;
    if (format >= YAPF_GPU_ETC2_RGB8) return YAPF_ERR_UNSUPPORTED;
    const uint8_t *b = (const uint8_t *)blocks;
    uint32_t nbx = (w + 3) / 4, nby = (h + 3) / 4;
    int hdr = format == YAPF_GPU_BC6H || format == YAPF_GPU_BC6H_SF;
    for (uint32_t by = 0; by < nby; by++)
        for (uint32_t bx = 0; bx < nbx; bx++, b += S) {
            uint8_t  px[16][4];
            uint16_t hx[16][3];
            switch (format) {
            case YAPF_GPU_BC1: yapf__bc1_block(b, px, 0); break;
            case YAPF_GPU_BC2:
                yapf__bc1_block(b + 8, px, 1);
                for (int i = 0; i < 16; i++) px[i][3] = (uint8_t)(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
                break;
            case YAPF_GPU_BC3:
                yapf__bc1_block(b + 8, px, 1);
                yapf__bc4_block(b, px, 3, 0);
                break;
            case YAPF_GPU_BC4: case YAPF_GPU_BC4_SNORM:
                memset(px, 0, sizeof(px));
                yapf__bc4_block(b, px, 0, format == YAPF_GPU_BC4_SNORM);
                for (int i = 0; i < 16; i++) px[i][3] = 255;
                break;
            case YAPF_GPU_BC5: case YAPF_GPU_BC5_SNORM:
                memset(px, 0, sizeof(px));
                yapf__bc4_block(b, px, 0, format == YAPF_GPU_BC5_SNORM);
                yapf__bc4_block(b + 8, px, 1, format == YAPF_GPU_BC5_SNORM);
                for (int i = 0; i < 16; i++) px[i][3] = 255;
                break;
            case YAPF_GPU_BC7: yapf__bc7_block(b, px); break;
            default: yapf__bc6_block(b, hx, format == YAPF_GPU_BC6H_SF); break;
            }
            for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++)
                for (uint32_t x = 0; x < 4 && bx * 4 + x < w; x++) {
                    size_t o = (size_t)(by * 4 + y) * w + bx * 4 + x;
                    if (hdr) memcpy((uint16_t *)out + o * 3, hx[y * 4 + x], 6);
                    else     memcpy((uint8_t *)out + o * 4, px[y * 4 + x], 4);
                }
        }
    return YAPF_OK;
}

/* ── Encoders ────────────────────────────────────────────────────────── */
/*
 * Straightforward, decent-quality encoders so YAPF can make GPU textures
 * on its own: principal axis endpoints with least-squares refinement.
 * For the best BC7 / ASTC quality use a dedicated encoder and store its
 * blocks (payload = BLOCKS).
 */

/* Principal axis of n points of dim k (power iteration). */
static void yapf__pca(const float (*p)[4], int n, int k, float *mean, float *axis) {
    for (int c = 0; c < k; c++) {
        mean[c] = 0;
        for (int i = 0; i < n; i++) mean[c] += p[i][c];
        mean[c] /= (float)n;
    }
    float cov[4][4] = {{0}};
    for (int i = 0; i < n; i++)
        for (int a = 0; a < k; a++)
            for (int b = 0; b < k; b++)
                cov[a][b] += (p[i][a] - mean[a]) * (p[i][b] - mean[b]);
    float v[4] = { 1.0f, 0.7f, 0.4f, 0.2f };
    for (int it = 0; it < 12; it++) {
        float nv[4] = {0}, len = 0;
        for (int a = 0; a < k; a++) {
            for (int b = 0; b < k; b++) nv[a] += cov[a][b] * v[b];
            len += nv[a] * nv[a];
        }
        if (len < 1e-12f) break;
        float mx = 0;                    /* scale by the largest component */
        for (int a = 0; a < k; a++) mx = nv[a] > mx ? nv[a] : -nv[a] > mx ? -nv[a] : mx;
        for (int a = 0; a < k; a++) v[a] = nv[a] / mx;
    }
    for (int a = 0; a < k; a++) axis[a] = v[a];
}

/* Endpoints at the extremes of the projections onto the principal axis. */
static void yapf__extents(const float (*p)[4], int n, int k, float *e0, float *e1) {
    float mean[4], axis[4], lo = 1e30f, hi = -1e30f, len2 = 0;
    yapf__pca(p, n, k, mean, axis);
    for (int c = 0; c < k; c++) len2 += axis[c] * axis[c];
    if (len2 < 1e-20f) len2 = 1.0f;
    for (int i = 0; i < n; i++) {
        float t = 0;
        for (int c = 0; c < k; c++) t += (p[i][c] - mean[c]) * axis[c];
        if (t < lo) lo = t;
        if (t > hi) hi = t;
    }
    lo /= len2;                         /* the axis need not be unit length */
    hi /= len2;
    for (int c = 0; c < k; c++) { e0[c] = mean[c] + axis[c] * lo; e1[c] = mean[c] + axis[c] * hi; }
}

/* Least squares endpoints for given interpolation weights (0..1). */
static int yapf__lsq(const float (*p)[4], const float *wt, int n, int k, float *e0, float *e1) {
    float aa = 0, ab = 0, bb = 0, ax[4] = {0}, bx[4] = {0};
    for (int i = 0; i < n; i++) {
        float b = wt[i], a = 1.0f - b;
        aa += a * a; ab += a * b; bb += b * b;
        for (int c = 0; c < k; c++) { ax[c] += a * p[i][c]; bx[c] += b * p[i][c]; }
    }
    float det = aa * bb - ab * ab;
    if (det < 1e-8f && det > -1e-8f) return -1;
    for (int c = 0; c < k; c++) {
        e0[c] = (ax[c] * bb - bx[c] * ab) / det;
        e1[c] = (bx[c] * aa - ax[c] * ab) / det;
    }
    return 0;
}

static int yapf__clampi(float v, int lo, int hi) {
    int i = (int)(v + 0.5f);
    if (v < 0) i = (int)(v - 0.5f);
    return i < lo ? lo : i > hi ? hi : i;
}

static uint16_t yapf__to565(const float *c) {
    return (uint16_t)((yapf__clampi(c[0] * 31.0f / 255.0f, 0, 31) << 11) |
                      (yapf__clampi(c[1] * 63.0f / 255.0f, 0, 63) << 5) |
                       yapf__clampi(c[2] * 31.0f / 255.0f, 0, 31));
}

/* BC1 colour block from 16 RGBA pixels.  four: force 4-colour mode
 * (BC2 / BC3); otherwise pixels with alpha < 128 become transparent. */
static void yapf__bc1_encode(const uint8_t px[16][4], uint8_t *out, int four) {
    float    p[16][4];
    int      transparent = 0, n = 0, map[16];
    for (int i = 0; i < 16; i++) {
        if (!four && px[i][3] < 128) { transparent = 1; map[i] = -1; continue; }
        map[i] = n;
        for (int c = 0; c < 3; c++) p[n][c] = px[i][c];
        n++;
    }
    if (n == 0) {                                   /* all transparent */
        yapf__w16le(out, 0); yapf__w16le(out + 2, 0); yapf__w32le(out + 4, 0xFFFFFFFFu);
        return;
    }
    float    e0[4], e1[4];
    uint32_t best_idx = 0, best_err = UINT32_MAX;
    uint16_t best_c0 = 0, best_c1 = 0;
    yapf__extents((const float (*)[4])p, n, 3, e0, e1);
    for (int it = 0; it < 3; it++) {
        uint16_t c0 = yapf__to565(e1), c1 = yapf__to565(e0);
        int three = transparent;
        if (three ? c0 > c1 : c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
        if (!three && c0 == c1) {                    /* single colour */
            if (c1 > 0) c1--; else c0++;
            if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
        }
        uint8_t blk[8], full[16][4];
        /* decode a block with indices 0, 1, 2, 3 to get the palette */
        yapf__w16le(blk, c0); yapf__w16le(blk + 2, c1); yapf__w32le(blk + 4, 0xE4u);
        yapf__bc1_block(blk, full, four);
        int nc = three ? 3 : 4;
        uint32_t idx = 0, err = 0;
        float wt[16];
        static const float wv4[4] = { 0.0f, 1.0f, 1.0f / 3, 2.0f / 3 };
        static const float wv3[3] = { 0.0f, 1.0f, 0.5f };
        for (int i = 0; i < 16; i++) {
            if (map[i] < 0) { idx |= 3u << (2 * i); continue; }
            uint32_t be = UINT32_MAX;
            int      bi = 0;
            for (int j = 0; j < nc; j++) {
                uint32_t e = 0;
                for (int c = 0; c < 3; c++) { int d = (int)full[j][c] - px[i][c]; e += (uint32_t)(d * d); }
                if (e < be) { be = e; bi = j; }
            }
            idx |= (uint32_t)bi << (2 * i);
            err += be;
            wt[map[i]] = three ? wv3[bi] : wv4[bi];
        }
        if (err < best_err) { best_err = err; best_idx = idx; best_c0 = c0; best_c1 = c1; }
        if (err == 0) break;
        /* refine: endpoints in palette order (index 0 = c0) */
        float n0[4], n1[4];
        if (yapf__lsq((const float (*)[4])p, wt, n, 3, n0, n1) != 0) break;
        for (int c = 0; c < 3; c++) { e1[c] = n0[c]; e0[c] = n1[c]; }
    }
    yapf__w16le(out, best_c0);
    yapf__w16le(out + 2, best_c1);
    yapf__w32le(out + 4, best_idx);
}

/* BC4 block from 16 values. */
static void yapf__bc4_encode(const uint8_t *v, uint8_t *out) {
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
    uint64_t best_bits = 0;
    uint32_t best_err = UINT32_MAX;
    int      best0 = hi, best1 = lo;
    for (int d0 = 0; d0 < 3; d0++)
        for (int d1 = 0; d1 < 3; d1++) {
            int r0 = hi - d0, r1 = lo + d1;
            if (r0 < r1) continue;
            if (r0 == r1) { if (d0 || d1) continue; }
            uint8_t blk[8] = { (uint8_t)r0, (uint8_t)r1, 0, 0, 0, 0, 0, 0 };
            uint8_t pal[16][4];
            int     palv[8];
            for (int j = 0; j < 8; j++) {
                uint64_t ix = 0;
                for (int i = 0; i < 16; i++) ix |= (uint64_t)j << (3 * i);
                for (int k = 0; k < 6; k++) blk[2 + k] = (uint8_t)(ix >> (8 * k));
                yapf__bc4_block(blk, pal, 0, 0);
                palv[j] = pal[0][0];
            }
            uint64_t bits = 0;
            uint32_t err = 0;
            for (int i = 0; i < 16; i++) {
                int bj = 0, be = 1 << 30;
                for (int j = 0; j < 8; j++) {
                    int d = palv[j] - v[i];
                    if (d * d < be) { be = d * d; bj = j; }
                }
                bits |= (uint64_t)bj << (3 * i);
                err += (uint32_t)be;
            }
            if (err < best_err) { best_err = err; best_bits = bits; best0 = r0; best1 = r1; }
        }
    out[0] = (uint8_t)best0;
    out[1] = (uint8_t)best1;
    for (int k = 0; k < 6; k++) out[2 + k] = (uint8_t)(best_bits >> (8 * k));
}

/* BC7 mode 6 (one subset, RGBA 7.7.7.7 + p-bit per endpoint, 4-bit
 * indices).  Opaque blocks keep the p-bits at 1 so alpha stays 255.
 * Returns the squared error. */
static uint32_t yapf__bc7_mode6(const uint8_t px[16][4], uint8_t *out) {
    float p[16][4], e0[4], e1[4];
    int   opaque = 1;
    for (int i = 0; i < 16; i++) {
        for (int c = 0; c < 4; c++) p[i][c] = px[i][c];
        if (px[i][3] != 255) opaque = 0;
    }
    yapf__extents((const float (*)[4])p, 16, opaque ? 3 : 4, e0, e1);
    if (opaque) e0[3] = e1[3] = 255;

    uint8_t  best[16];
    uint32_t best_err = UINT32_MAX;
    for (int it = 0; it < 3; it++) {
        int q[2][4], pb[2];
        const float *e[2] = { e0, e1 };
        for (int k = 0; k < 2; k++) {
            uint32_t bestp = UINT32_MAX;
            for (int pbit = opaque ? 1 : 0; pbit < 2; pbit++) {
                uint32_t er = 0;
                int      qq[4];
                for (int c = 0; c < 4; c++) {
                    qq[c] = yapf__clampi((e[k][c] - (float)pbit) / 2.0f, 0, 127);
                    int d = (2 * qq[c] + pbit) - (int)(e[k][c] + 0.5f);
                    er += (uint32_t)(d * d);
                }
                if (er < bestp) { bestp = er; pb[k] = pbit; memcpy(q[k], qq, sizeof(qq)); }
            }
        }
        int v[2][4];
        for (int k = 0; k < 2; k++) for (int c = 0; c < 4; c++) v[k][c] = 2 * q[k][c] + pb[k];
        int      idx[16];
        uint32_t err = 0;
        float    wt[16];
        for (int i = 0; i < 16; i++) {
            uint32_t be = UINT32_MAX;
            int      bi = 0;
            for (int j = 0; j < 16; j++) {
                uint32_t er = 0;
                for (int c = 0; c < 4; c++) {
                    int d = yapf__bc_interp(v[0][c], v[1][c], yapf__w4[j]) - px[i][c];
                    er += (uint32_t)(d * d);
                }
                if (er < be) { be = er; bi = j; }
            }
            idx[i] = bi;
            err += be;
            wt[i] = yapf__w4[bi] / 64.0f;
        }
        if (err < best_err) {
            best_err = err;
            int swap = idx[0] >= 8;
            int a = swap ? 1 : 0, b = swap ? 0 : 1;
            memset(best, 0, 16);
            unsigned pos = 0;
            yapf__putbits(best, &pos, 1u << 6, 7);
            for (int c = 0; c < 4; c++) {
                yapf__putbits(best, &pos, (uint32_t)q[a][c], 7);
                yapf__putbits(best, &pos, (uint32_t)q[b][c], 7);
            }
            yapf__putbits(best, &pos, (uint32_t)pb[a], 1);
            yapf__putbits(best, &pos, (uint32_t)pb[b], 1);
            for (int i = 0; i < 16; i++) {
                int ix = swap ? 15 - idx[i] : idx[i];
                yapf__putbits(best, &pos, (uint32_t)ix, i == 0 ? 3 : 4);
            }
        }
        if (err == 0) break;
        float n0[4], n1[4];
        if (yapf__lsq((const float (*)[4])p, wt, 16, 4, n0, n1) != 0) break;
        memcpy(e0, n0, sizeof(n0));
        memcpy(e1, n1, sizeof(n1));
        if (opaque) e0[3] = e1[3] = 255;
    }
    memcpy(out, best, 16);
    return best_err;
}

/* Fits indices of `bits` bits for the points of one subset against
 * endpoints v0, v1 (k channels); returns the error, fills idx / wt. */
static uint32_t yapf__bc7_fit(const uint8_t px[16][4], const int *sel, int n,
                              const int *v0, const int *v1, int c0, int k, int bits,
                              int *idx, float *wt)
{
    const uint8_t *w = yapf__weights(bits);
    uint32_t err = 0;
    for (int i = 0; i < n; i++) {
        uint32_t be = UINT32_MAX;
        int      bi = 0;
        for (int j = 0; j < (1 << bits); j++) {
            uint32_t e = 0;
            for (int c = c0; c < c0 + k; c++) {
                int d = yapf__bc_interp(v0[c], v1[c], w[j]) - px[sel[i]][c];
                e += (uint32_t)(d * d);
            }
            if (e < be) { be = e; bi = j; }
        }
        idx[i] = bi;
        wt[i] = w[bi] / 64.0f;
        err += be;
    }
    return err;
}

/* BC7 mode 5: RGB 7 bits and alpha 8 bits on separate 2-bit indices, so
 * colour and alpha need not move together (cut-outs, decals). */
static uint32_t yapf__bc7_mode5(const uint8_t px[16][4], uint8_t *out) {
    int   sel[16];
    float p[16][4], e0[4], e1[4];
    for (int i = 0; i < 16; i++) { sel[i] = i; for (int c = 0; c < 4; c++) p[i][c] = px[i][c]; }
    yapf__extents((const float (*)[4])p, 16, 3, e0, e1);

    int      bq[2][3] = {{0}}, bidx[16] = {0};
    uint32_t cerr = UINT32_MAX;
    for (int it = 0; it < 3; it++) {
        int q[2][3], v[2][4] = {{0}}, idx[16];
        float wt[16];
        for (int c = 0; c < 3; c++) {
            q[0][c] = yapf__clampi(e0[c] * 127.0f / 255.0f, 0, 127);
            q[1][c] = yapf__clampi(e1[c] * 127.0f / 255.0f, 0, 127);
            v[0][c] = (q[0][c] << 1) | (q[0][c] >> 6);
            v[1][c] = (q[1][c] << 1) | (q[1][c] >> 6);
        }
        uint32_t e = yapf__bc7_fit(px, sel, 16, v[0], v[1], 0, 3, 2, idx, wt);
        if (e < cerr) { cerr = e; memcpy(bq, q, sizeof(q)); memcpy(bidx, idx, sizeof(idx)); }
        float n0[4], n1[4];
        if (e == 0 || yapf__lsq((const float (*)[4])p, wt, 16, 3, n0, n1) != 0) break;
        memcpy(e0, n0, sizeof(n0));
        memcpy(e1, n1, sizeof(n1));
    }
    int lo = 255, hi = 0;
    for (int i = 0; i < 16; i++) { if (px[i][3] < lo) lo = px[i][3]; if (px[i][3] > hi) hi = px[i][3]; }
    int      ba[2] = { lo, hi }, aidx[16] = {0};
    uint32_t aerr = UINT32_MAX;
    for (int d0 = 0; d0 < 4; d0++)
        for (int d1 = 0; d1 < 4; d1++) {
            int a0 = lo + d0, a1 = hi - d1, v0[4], v1[4], idx[16];
            float wt[16];
            if (a0 > a1) continue;
            v0[3] = a0; v1[3] = a1;
            uint32_t e = yapf__bc7_fit(px, sel, 16, v0, v1, 3, 1, 2, idx, wt);
            if (e < aerr) { aerr = e; ba[0] = a0; ba[1] = a1; memcpy(aidx, idx, sizeof(idx)); }
        }
    /* anchors: the first index of each set keeps its top bit clear */
    if (bidx[0] >= 2) {
        for (int c = 0; c < 3; c++) { int t = bq[0][c]; bq[0][c] = bq[1][c]; bq[1][c] = t; }
        for (int i = 0; i < 16; i++) bidx[i] = 3 - bidx[i];
    }
    if (aidx[0] >= 2) {
        int t = ba[0]; ba[0] = ba[1]; ba[1] = t;
        for (int i = 0; i < 16; i++) aidx[i] = 3 - aidx[i];
    }
    unsigned pos = 0;
    memset(out, 0, 16);
    yapf__putbits(out, &pos, 1u << 5, 6);
    yapf__putbits(out, &pos, 0, 2);                       /* no rotation */
    for (int c = 0; c < 3; c++) {
        yapf__putbits(out, &pos, (uint32_t)bq[0][c], 7);
        yapf__putbits(out, &pos, (uint32_t)bq[1][c], 7);
    }
    yapf__putbits(out, &pos, (uint32_t)ba[0], 8);
    yapf__putbits(out, &pos, (uint32_t)ba[1], 8);
    for (int i = 0; i < 16; i++) yapf__putbits(out, &pos, (uint32_t)bidx[i], i ? 2 : 1);
    for (int i = 0; i < 16; i++) yapf__putbits(out, &pos, (uint32_t)aidx[i], i ? 2 : 1);
    return cerr + aerr;
}

/* BC7 mode 1: two subsets, RGB 6 bits + a shared p-bit per subset, 3-bit
 * indices; for opaque blocks with two colour regions. */
static uint32_t yapf__bc7_mode1(const uint8_t px[16][4], int part, uint8_t *out) {
    int      q[2][2][3], pb[2], idx16[16];
    uint32_t total = 0;
    for (int k = 0; k < 2; k++) {
        int   sel[16], n = 0;
        float p[16][4], e0[4], e1[4];
        for (int i = 0; i < 16; i++)
            if (yapf__subset(2, part, i) == k) {
                sel[n] = i;
                for (int c = 0; c < 3; c++) p[n][c] = px[i][c];
                n++;
            }
        yapf__extents((const float (*)[4])p, n, 3, e0, e1);
        uint32_t best = UINT32_MAX;
        for (int it = 0; it < 2; it++) {
            for (int pbit = 0; pbit < 2; pbit++) {
                int qq[2][3], v[2][4] = {{0}}, idx[16];
                float wt[16];
                for (int c = 0; c < 3; c++) {
                    const float *e[2] = { e0, e1 };
                    for (int j = 0; j < 2; j++) {
                        qq[j][c] = yapf__clampi((e[j][c] * 127.0f / 255.0f - (float)pbit) / 2.0f, 0, 63);
                        int x = (qq[j][c] << 1) | pbit;
                        v[j][c] = (x << 1) | (x >> 6);
                    }
                }
                uint32_t er = yapf__bc7_fit(px, sel, n, v[0], v[1], 0, 3, 3, idx, wt);
                if (er < best) {
                    best = er;
                    pb[k] = pbit;
                    memcpy(q[k], qq, sizeof(qq));
                    for (int i = 0; i < n; i++) idx16[sel[i]] = idx[i];
                }
                if (it == 0 && pbit == 1) {
                    float n0[4], n1[4];
                    if (yapf__lsq((const float (*)[4])p, wt, n, 3, n0, n1) == 0) {
                        memcpy(e0, n0, sizeof(n0));
                        memcpy(e1, n1, sizeof(n1));
                    }
                }
            }
        }
        total += best;
        int anchor = k == 0 ? 0 : yapf__anchor2[part];
        if (idx16[anchor] >= 4) {
            for (int c = 0; c < 3; c++) { int t = q[k][0][c]; q[k][0][c] = q[k][1][c]; q[k][1][c] = t; }
            for (int i = 0; i < 16; i++) if (yapf__subset(2, part, i) == k) idx16[i] = 7 - idx16[i];
        }
    }
    unsigned pos = 0;
    memset(out, 0, 16);
    yapf__putbits(out, &pos, 2, 2);
    yapf__putbits(out, &pos, (uint32_t)part, 6);
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < 2; k++) {
            yapf__putbits(out, &pos, (uint32_t)q[k][0][c], 6);
            yapf__putbits(out, &pos, (uint32_t)q[k][1][c], 6);
        }
    yapf__putbits(out, &pos, (uint32_t)pb[0], 1);
    yapf__putbits(out, &pos, (uint32_t)pb[1], 1);
    for (int i = 0; i < 16; i++)
        yapf__putbits(out, &pos, (uint32_t)idx16[i], 3 - yapf__is_anchor(2, part, i));
    return total;
}

/* BC7: mode 6, plus mode 5 when there is alpha, plus mode 1 on the two
 * most promising partitions (least summed per-subset variance) for
 * opaque blocks.  The block with the smallest error wins. */
static void yapf__bc7_encode(const uint8_t px[16][4], uint8_t *out) {
    uint8_t  cand[16];
    uint32_t best = yapf__bc7_mode6(px, out), e;
    int      opaque = 1;
    for (int i = 0; i < 16; i++) if (px[i][3] != 255) opaque = 0;
    if (best == 0) return;
    if (!opaque) {
        if ((e = yapf__bc7_mode5(px, cand)) < best) { best = e; memcpy(out, cand, 16); }
        return;
    }
    float score[64];
    for (int part = 0; part < 64; part++) {
        float s = 0;
        for (int k = 0; k < 2; k++) {
            float sum[3] = {0}, sq[3] = {0};
            int   n = 0;
            for (int i = 0; i < 16; i++)
                if (yapf__subset(2, part, i) == k) {
                    n++;
                    for (int c = 0; c < 3; c++) { sum[c] += px[i][c]; sq[c] += (float)px[i][c] * px[i][c]; }
                }
            for (int c = 0; c < 3; c++) s += sq[c] - sum[c] * sum[c] / (float)n;
        }
        score[part] = s;
    }
    for (int t = 0; t < 2; t++) {
        int bp = 0;
        for (int part = 1; part < 64; part++) if (score[part] < score[bp]) bp = part;
        score[bp] = 1e30f;
        if ((e = yapf__bc7_mode1(px, bp, cand)) < best) { best = e; memcpy(out, cand, 16); }
    }
}

/* BC6H unsigned, one region: tries modes 11–14 (10.10, 11.9, 12.8, 16.4)
 * and keeps the one with the smallest error in half-float bits. */
static void yapf__bc6_encode(const uint16_t px[16][3], uint8_t *out) {
    float p[16][4], e0[4], e1[4];
    int   hv[16][3];
    for (int i = 0; i < 16; i++)
        for (int c = 0; c < 3; c++) {
            int h = px[i][c];
            if (h & 0x8000) h = 0;          /* negative: unsigned format */
            if (h > 0x7BFF) h = 0x7BFF;     /* inf / nan: largest finite */
            hv[i][c] = h;
            p[i][c] = (float)h * 64.0f / 31.0f;   /* interpolation domain */
        }
    yapf__extents((const float (*)[4])p, 16, 3, e0, e1);

    uint8_t  best[16];
    uint64_t best_err = UINT64_MAX;
    memset(best, 0, 16);
    for (int m = 10; m < 14; m++) {
        int   W = yapf__bc6_modes[m].w, d = yapf__bc6_modes[m].dr;
        float f0[4], f1[4];
        memcpy(f0, e0, sizeof(f0));
        memcpy(f1, e1, sizeof(f1));
        for (int it = 0; it < 2; it++) {
            int q[2][3], ok = 1;
            for (int c = 0; c < 3; c++) {
                const float *e[2] = { f0, f1 };
                for (int k = 0; k < 2; k++) {
                    float u = e[k][c];
                    int   v = W >= 15 ? yapf__clampi(u, 0, (1 << W) - 1)
                                      : yapf__clampi((u - (float)(1 << (15 - W))) / (float)(1 << (16 - W)),
                                                     0, (1 << W) - 1);
                    q[k][c] = v;
                }
                if (m != 10) {
                    int delta = q[1][c] - q[0][c];
                    if (delta < -(1 << (d - 1)) || delta > (1 << (d - 1)) - 1) ok = 0;
                }
            }
            if (!ok) break;
            int u[2][3];
            for (int k = 0; k < 2; k++) for (int c = 0; c < 3; c++) u[k][c] = yapf__bc6_unq(q[k][c], W, 0);
            int      idx[16];
            uint64_t err = 0;
            float    wt[16];
            for (int i = 0; i < 16; i++) {
                uint64_t be = UINT64_MAX;
                int      bi = 0;
                for (int j = 0; j < 16; j++) {
                    uint64_t er = 0;
                    for (int c = 0; c < 3; c++) {
                        int64_t dd = (int64_t)yapf__bc6_finish(yapf__bc_interp(u[0][c], u[1][c], yapf__w4[j]), 0) - hv[i][c];
                        er += (uint64_t)(dd * dd);
                    }
                    if (er < be) { be = er; bi = j; }
                }
                idx[i] = bi;
                err += be;
                wt[i] = yapf__w4[bi] / 64.0f;
            }
            if (err < best_err) {
                best_err = err;
                int swap = idx[0] >= 8, a = swap ? 1 : 0, b = swap ? 0 : 1;
                int f[13] = {0};
                for (int c = 0; c < 3; c++) {
                    f[c * 4] = q[a][c];
                    f[c * 4 + 1] = m == 10 ? q[b][c] : (q[b][c] - q[a][c]) & ((1 << d) - 1);
                }
                /* with swapped endpoints the delta must still fit */
                int fits = 1;
                if (m != 10)
                    for (int c = 0; c < 3; c++) {
                        int delta = q[b][c] - q[a][c];
                        if (delta < -(1 << (d - 1)) || delta > (1 << (d - 1)) - 1) fits = 0;
                    }
                if (fits) {
                    memset(best, 0, 16);
                    unsigned pos = 0;
                    yapf__putbits(best, &pos, yapf__bc6_modes[m].code, 5);
                    for (const yapf__seg *g = yapf__bc6_modes[m].seg; g->n; g++)
                        yapf__putbits(best, &pos, (uint32_t)(f[g->f] >> g->lo), g->n);
                    for (int i = 0; i < 16; i++) {
                        int ix = swap ? 15 - idx[i] : idx[i];
                        yapf__putbits(best, &pos, (uint32_t)ix, i == 0 ? 3 : 4);
                    }
                } else {
                    best_err = UINT64_MAX;
                }
            }
            float n0[4], n1[4];
            if (yapf__lsq((const float (*)[4])p, wt, 16, 3, n0, n1) != 0) break;
            memcpy(f0, n0, sizeof(n0));
            memcpy(f1, n1, sizeof(n1));
        }
    }
    memcpy(out, best, 16);
}

/* Reads texel (x, y) of a pixel surface slice as RGBA8 / RGB half. */
static void yapf__texel_rgba8(const yapf_texture_t *t, const uint8_t *src, uint32_t w,
                              uint32_t h, uint32_t x, uint32_t y, uint8_t *o)
{
    if (x >= w) x = w - 1;           /* edge blocks repeat the last texel */
    if (y >= h) y = h - 1;
    const uint8_t *s = src + ((size_t)y * w + x) * t->channels;
    switch (t->channels) {
    case 1:  o[0] = o[1] = o[2] = s[0]; o[3] = 255; break;
    case 2:  o[0] = o[1] = o[2] = s[0]; o[3] = s[1]; break;
    case 3:  o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; o[3] = 255; break;
    default: memcpy(o, s, 4); break;
    }
}

static void yapf__texel_half(const yapf_texture_t *t, const uint8_t *src, uint32_t w,
                             uint32_t h, uint32_t x, uint32_t y, uint16_t *o)
{
    if (x >= w) x = w - 1;
    if (y >= h) y = h - 1;
    size_t sb = yapf__sbytes(t->sample);
    const uint8_t *s = src + ((size_t)y * w + x) * t->channels * sb;
    for (int c = 0; c < 3; c++) {
        int      k = t->channels >= 3 ? c : 0;
        uint32_t v = yapf__get_sample(s + (size_t)k * sb, t->sample);
        if (t->sample == YAPF_SAMPLE_F16) { o[c] = (uint16_t)v; continue; }
        float f;
        if (t->sample == YAPF_SAMPLE_F32) memcpy(&f, &v, 4);
        else f = t->sample == YAPF_SAMPLE_U16 ? (float)v / 65535.0f : (float)v / 255.0f;
        o[c] = yapf_float_to_half(f);
    }
}

int yapf_tex_compress(const yapf_texture_t *src, uint8_t format, yapf_texture_t **out) {
    uint32_t S;
    if (!out) return YAPF_ERR_INVALID;
    *out = NULL;
    if (yapf__tex_check(src, &S) != 0 || src->payload != YAPF_PAYLOAD_PIXELS || !src->data)
        return YAPF_ERR_INVALID;
    int hdr = format == YAPF_GPU_BC6H;
    if (format != YAPF_GPU_BC1 && format != YAPF_GPU_BC3 && format != YAPF_GPU_BC4 &&
        format != YAPF_GPU_BC5 && format != YAPF_GPU_BC7 && !hdr)
        return YAPF_ERR_UNSUPPORTED;
    if (hdr == (src->sample == YAPF_SAMPLE_U8)) return YAPF_ERR_INVALID;

    yapf_texture_t d = *src;
    d.payload = YAPF_PAYLOAD_BLOCKS;
    d.format  = format;
    d.sample  = hdr ? YAPF_SAMPLE_F16 : YAPF_SAMPLE_U8;
    d.channels = format == YAPF_GPU_BC4 ? 1 : format == YAPF_GPU_BC5 ? 2 : hdr ? 3
               : format == YAPF_GPU_BC3 ? 4 : (src->channels == 4 || src->channels == 2) ? 4 : 3;
    for (int c = d.channels; c < 4; c++) d.channel_map[c] = YAPF_CH_DEFAULT;
    d.linear_mask &= (uint8_t)((1u << d.channels) - 1);
    if (d.normal && d.channels == 2) d.normal = (uint8_t)((d.normal & 3) | YAPF_NORMAL_XY);
    yapf_texture_t *t = yapf_tex_create(&d);
    if (!t) return YAPF_ERR_OOM;
    for (uint32_t i = 0; i < src->note_count; i++)
        if (yapf_tex_add_note(t, src->notes[i].tag, src->notes[i].data, src->notes[i].size) != YAPF_OK) {
            yapf_tex_free(t);
            return YAPF_ERR_OOM;
        }
    if (t->frames > 1 && src->frame_us) memcpy(t->frame_us, src->frame_us, t->frames * sizeof(uint32_t));

    for (uint32_t s = 0; s < S; s++) {
        uint32_t m = s % src->mip_levels, w, h, dd, nbx, nby, B;
        yapf_tex_mip_size(src, m, &w, &h, &dd);
        yapf__mip_blocks(t, m, &nbx, &nby, &B);
        size_t   slice = (size_t)w * h * src->channels * yapf__sbytes(src->sample);
        uint8_t *o = (uint8_t *)t->data[s];
        for (uint32_t z = 0; z < dd; z++) {
            const uint8_t *sl = (const uint8_t *)src->data[s] + z * slice;
            for (uint32_t by = 0; by < nby; by++)
                for (uint32_t bx = 0; bx < nbx; bx++, o += B) {
                    uint8_t  px[16][4];
                    uint16_t hx[16][3];
                    for (int i = 0; i < 16; i++) {
                        uint32_t x = bx * 4 + (uint32_t)(i & 3), y = by * 4 + (uint32_t)(i >> 2);
                        if (hdr) yapf__texel_half(src, sl, w, h, x, y, hx[i]);
                        else     yapf__texel_rgba8(src, sl, w, h, x, y, px[i]);
                    }
                    if (format == YAPF_GPU_BC4 || format == YAPF_GPU_BC5) {
                        /* raw channels, not the gray expansion */
                        for (int k = 0; k < (format == YAPF_GPU_BC5 ? 2 : 1); k++) {
                            uint8_t v[16];
                            for (int i = 0; i < 16; i++) {
                                uint32_t x = bx * 4 + (uint32_t)(i & 3), y = by * 4 + (uint32_t)(i >> 2);
                                if (x >= w) x = w - 1;
                                if (y >= h) y = h - 1;
                                int c = k < src->channels ? k : 0;
                                v[i] = sl[((size_t)y * w + x) * src->channels + c];
                            }
                            yapf__bc4_encode(v, o + 8 * k);
                        }
                    } else if (format == YAPF_GPU_BC1) {
                        yapf__bc1_encode((const uint8_t (*)[4])px, o, 0);
                    } else if (format == YAPF_GPU_BC3) {
                        uint8_t a[16];
                        for (int i = 0; i < 16; i++) a[i] = px[i][3];
                        yapf__bc4_encode(a, o);
                        yapf__bc1_encode((const uint8_t (*)[4])px, o + 8, 1);
                    } else if (format == YAPF_GPU_BC7) {
                        yapf__bc7_encode((const uint8_t (*)[4])px, o);
                    } else {
                        yapf__bc6_encode((const uint16_t (*)[3])hx, o);
                    }
                }
        }
    }
    *out = t;
    return YAPF_OK;
}

/* ========================================================================
 *  Mip generation
 * ======================================================================== */

/* x^2.4 for x in [0, 1] without libm: x² · x^0.4, x^0.4 by Newton on
 * y^5 = x². */
static double yapf__pow24(double x) {
    if (x <= 0) return 0;
    double a = x * x, y = 1.0;
    for (int i = 0; i < 60; i++) {
        double y4 = y * y * y * y, ny = y - (y4 * y - a) / (5.0 * y4);
        if (ny == y) break;
        y = ny;
    }
    return x * x * y;
}

static double yapf__srgb_to_lin(double v) {
    return v <= 0.04045 ? v / 12.92 : yapf__pow24((v + 0.055) / 1.055);
}

/* 8-bit sRGB: code → linear, and the linear thresholds halfway between
 * codes, so linear → code rounds exactly as in sRGB space. */
typedef struct { float lin[256], mid[255]; } yapf__srgb_tab;

static void yapf__srgb_init(yapf__srgb_tab *t) {
    for (int i = 0; i < 256; i++) t->lin[i] = (float)yapf__srgb_to_lin(i / 255.0);
    for (int i = 0; i < 255; i++) t->mid[i] = (float)yapf__srgb_to_lin((i + 0.5) / 255.0);
}

static float yapf__lin_to_srgb(const yapf__srgb_tab *t, float v) {
    int lo = 0, hi = 255;                      /* first code whose mid >= v */
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (t->mid[m] < v) lo = m + 1; else hi = m;
    }
    return (float)lo / 255.0f;
}

/* Index of the alpha channel, or -1. */
static int yapf__alpha_channel(const yapf_texture_t *t) {
    for (int c = 0; c < t->channels; c++)
        if (t->channel_map[c] == YAPF_CH_ALPHA || t->channel_map[c] == YAPF_CH_OPACITY) return c;
    if (t->channel_map[t->channels - 1] == YAPF_CH_DEFAULT && (t->channels == 2 || t->channels == 4))
        return t->channels - 1;
    return -1;
}

static float yapf__load_f(const yapf_texture_t *t, const uint8_t *p) {
    uint32_t v = yapf__get_sample(p, t->sample);
    switch (t->sample) {
    case YAPF_SAMPLE_U8:  return (float)v / 255.0f;
    case YAPF_SAMPLE_U16: return (float)v / 65535.0f;
    case YAPF_SAMPLE_F16: return yapf_half_to_float((uint16_t)v);
    default: { float f; memcpy(&f, &v, 4); return f; }
    }
}

static void yapf__store_f(const yapf_texture_t *t, uint8_t *p, float f) {
    uint32_t v;
    switch (t->sample) {
    case YAPF_SAMPLE_U8:  v = (uint32_t)yapf__clampi(f * 255.0f, 0, 255); break;
    case YAPF_SAMPLE_U16: v = (uint32_t)yapf__clampi(f * 65535.0f, 0, 65535); break;
    case YAPF_SAMPLE_F16: v = yapf_float_to_half(f); break;
    default: memcpy(&v, &f, 4); break;
    }
    yapf__put_sample(p, t->sample, v);
}

/* Share of alpha values above ref after scaling by k, measured on the
 * values as they will be stored (q = 255 or 65535; 0 for floats). */
static double yapf__coverage(const float *a, size_t n, float ref, float k, float q) {
    size_t c = 0;
    for (size_t i = 0; i < n; i++) {
        float v = a[i] * k;
        if (q > 0) {
            v = v > 1.0f ? q : (float)(int)(v * q + 0.5f);
            if (v > ref * q) c++;
        } else if (v > ref) {
            c++;
        }
    }
    return n ? (double)c / (double)n : 0.0;
}

int yapf_tex_build_mips(yapf_texture_t *tex, uint32_t levels, uint8_t alpha_ref) {
    uint32_t S;
    if (!tex || !tex->internal_ || yapf__tex_check(tex, &S) != 0) return YAPF_ERR_INVALID;
    if (tex->payload != YAPF_PAYLOAD_PIXELS) return YAPF_ERR_INVALID;
    uint32_t full = 1;
    while (full < YAPF_MAX_MIPS &&
           ((tex->width >> full) || (tex->height >> full) ||
            (tex->shape == YAPF_SHAPE_VOLUME && (tex->depth >> full))))
        full++;
    if (levels == 0 || levels > full) levels = full;

    int ach = yapf__alpha_channel(tex);
    if (alpha_ref && ach < 0) return YAPF_ERR_INVALID;
    int srgb[4];
    for (int c = 0; c < tex->channels; c++)
        srgb[c] = (tex->flags & YAPF_FLAG_SRGB) && tex->sample == YAPF_SAMPLE_U8 && c != ach &&
                  !(tex->linear_mask & (1u << c)) &&
                  (tex->transfer == YAPF_TRANSFER_UNSPECIFIED || tex->transfer == YAPF_TRANSFER_SRGB);

    yapf__srgb_tab *st = (yapf__srgb_tab *)malloc(sizeof(yapf__srgb_tab));
    if (!st) return YAPF_ERR_OOM;
    yapf__srgb_init(st);

    yapf_texture_t d = *tex;
    d.mip_levels = (uint8_t)levels;
    uint32_t NS = yapf_tex_surface_count(&d);
    void   **data = (void **)calloc(NS, sizeof(void *));
    if (!data) { free(st); return YAPF_ERR_OOM; }
    int    rc = YAPF_OK;
    size_t sb = yapf__sbytes(tex->sample), ch = tex->channels;
    float  qs = tex->sample == YAPF_SAMPLE_U8 ? 255.0f : tex->sample == YAPF_SAMPLE_U16 ? 65535.0f : 0.0f;

    for (uint32_t base = 0; base < NS && rc == YAPF_OK; base += levels) {
        uint32_t old = base / levels * tex->mip_levels;   /* mip 0 of this face in tex */
        data[base] = tex->data[old];
        tex->data[old] = NULL;
        float  *alpha0 = NULL;
        double  target = 0;
        for (uint32_t m = 1; m < levels && rc == YAPF_OK; m++) {
            uint32_t pw, ph, pd, w, h, dd;
            yapf_tex_mip_size(&d, m - 1, &pw, &ph, &pd);
            yapf_tex_mip_size(&d, m, &w, &h, &dd);
            const uint8_t *src = (const uint8_t *)data[base + m - 1];
            uint8_t *dst = (uint8_t *)malloc(yapf_tex_surface_bytes(&d, m));
            float   *alpha = alpha_ref ? (float *)malloc((size_t)w * h * dd * sizeof(float)) : NULL;
            if (!dst || (alpha_ref && !alpha)) { free(dst); free(alpha); rc = YAPF_ERR_OOM; break; }
            data[base + m] = dst;
            if (alpha_ref && m == 1) {
                size_t n0 = (size_t)pw * ph * pd;
                alpha0 = (float *)malloc(n0 * sizeof(float));
                if (!alpha0) { free(alpha); rc = YAPF_ERR_OOM; break; }
                for (size_t i = 0; i < n0; i++) alpha0[i] = yapf__load_f(tex, src + (i * ch + ach) * sb);
                target = yapf__coverage(alpha0, n0, alpha_ref / 255.0f, 1.0f, qs);
            }
            for (uint32_t z = 0; z < dd; z++)
                for (uint32_t y = 0; y < h; y++)
                    for (uint32_t x = 0; x < w; x++) {
                        uint32_t xs[2] = { 2 * x < pw ? 2 * x : pw - 1, 2 * x + 1 < pw ? 2 * x + 1 : pw - 1 };
                        uint32_t ys[2] = { 2 * y < ph ? 2 * y : ph - 1, 2 * y + 1 < ph ? 2 * y + 1 : ph - 1 };
                        uint32_t zs[2] = { 2 * z < pd ? 2 * z : pd - 1, 2 * z + 1 < pd ? 2 * z + 1 : pd - 1 };
                        int      nz = d.shape == YAPF_SHAPE_VOLUME ? 2 : 1;
                        size_t   o = (((size_t)z * h + y) * w + x) * ch;
                        for (size_t c = 0; c < ch; c++) {
                            float sum = 0;
                            for (int k = 0; k < nz; k++)
                                for (int j = 0; j < 2; j++)
                                    for (int i = 0; i < 2; i++) {
                                        size_t q = (((size_t)zs[k] * ph + ys[j]) * pw + xs[i]) * ch + c;
                                        sum += srgb[c] ? st->lin[src[q]] : yapf__load_f(tex, src + q * sb);
                                    }
                            float v = sum / (float)(4 * nz);
                            if (srgb[c]) v = yapf__lin_to_srgb(st, v);
                            if (alpha && (int)c == ach) alpha[o / ch] = v;
                            yapf__store_f(tex, dst + (o + c) * sb, v);
                        }
                    }
            if (alpha) {
                /* scale alpha so coverage at alpha_ref matches mip 0 */
                size_t n = (size_t)w * h * dd;
                float  lo = 0.0f, hi = 8.0f, ref = alpha_ref / 255.0f;
                for (int it = 0; it < 20; it++) {
                    float mid = 0.5f * (lo + hi);
                    if (yapf__coverage(alpha, n, ref, mid, qs) < target) lo = mid; else hi = mid;
                }
                double clo = yapf__coverage(alpha, n, ref, lo, qs), chi = yapf__coverage(alpha, n, ref, hi, qs);
                float  k = (target - clo <= chi - target) ? lo : hi;   /* nearer bound */
                for (size_t i = 0; i < n; i++) {
                    float v = alpha[i] * k;
                    if (tex->sample != YAPF_SAMPLE_F16 && tex->sample != YAPF_SAMPLE_F32 && v > 1.0f) v = 1.0f;
                    yapf__store_f(tex, dst + (i * ch + ach) * sb, v);
                }
                free(alpha);
            }
        }
        free(alpha0);
    }
    if (rc != YAPF_OK) {
        for (uint32_t s = 0; s < NS; s++) {
            if (s % levels == 0) {             /* give mip 0 back */
                if (data[s]) tex->data[s / levels * tex->mip_levels] = data[s];
            } else {
                free(data[s]);
            }
        }
        free(data);
        free(st);
        return rc;
    }
    free(st);
    for (uint32_t s = 0; s < S; s++) free(tex->data[s]);   /* old mips (mip 0 moved) */
    free(tex->data);
    tex->data = data;
    tex->mip_levels = (uint8_t)levels;
    int any_srgb = 0;
    for (int c = 0; c < tex->channels; c++) any_srgb |= srgb[c];
    tex->mip_filter = (uint8_t)(YAPF_MIP_BOX | (any_srgb ? YAPF_MIP_LINEAR_LIGHT : 0));
    if (alpha_ref) { tex->flags |= YAPF_FLAG_COVERAGE; tex->alpha_ref = alpha_ref; }
    else tex->flags &= (uint8_t)~YAPF_FLAG_COVERAGE;
    return YAPF_OK;
}

/* ========================================================================
 *  Previews
 * ======================================================================== */

/* Linear 0..1 → sRGB code, exact to the 8-bit rounding (thresholds). */
static uint8_t yapf__lin_to_code(const yapf__srgb_tab *t, float v) {
    if (!(v > 0.0f)) return 0;                  /* also NaN */
    if (v >= 1.0f) return 255;
    return (uint8_t)(yapf__lin_to_srgb(t, v) * 255.0f + 0.5f);
}

int yapf_tex_to_rgba8(const yapf_texture_t *tex, uint32_t surface, uint32_t slice, uint8_t *out) {
    uint32_t S;
    if (!out || yapf__tex_check(tex, &S) != 0 || surface >= S || !tex->data || !tex->data[surface])
        return YAPF_ERR_INVALID;
    uint32_t m = surface % tex->mip_levels, w, h, d;
    yapf_tex_mip_size(tex, m, &w, &h, &d);
    if (slice >= d) return YAPF_ERR_INVALID;
    size_t         n = (size_t)w * h;
    const uint8_t *src = (const uint8_t *)tex->data[surface] + yapf_tex_surface_bytes(tex, m) / d * slice;

    int       ch = tex->channels, sample = tex->sample;
    uint16_t *half = NULL;
    if (tex->payload == YAPF_PAYLOAD_BLOCKS) {
        if (tex->format != YAPF_GPU_BC6H && tex->format != YAPF_GPU_BC6H_SF) {
            int rc = yapf_blocks_decode(tex->format, src, w, h, out);
            if (rc == YAPF_OK && tex->channels == 3)    /* BC1 / BC7 without alpha */
                for (size_t i = 0; i < n; i++) out[i * 4 + 3] = 255;
            return rc;
        }
        half = (uint16_t *)malloc(n * 3 * sizeof(uint16_t));
        if (!half) return YAPF_ERR_OOM;
        yapf_blocks_decode(tex->format, src, w, h, half);
        src = (const uint8_t *)half;
        ch = 3;
        sample = YAPF_SAMPLE_F16;
    }
    yapf__srgb_tab *st = NULL;
    if (sample == YAPF_SAMPLE_F16 || sample == YAPF_SAMPLE_F32) {
        st = (yapf__srgb_tab *)malloc(sizeof(yapf__srgb_tab));
        if (!st) { free(half); return YAPF_ERR_OOM; }
        yapf__srgb_init(st);
    }
    size_t sb = yapf__sbytes(sample);
    for (size_t i = 0; i < n; i++) {
        uint8_t v[4] = { 0, 0, 0, 255 };
        for (int c = 0; c < ch; c++) {
            uint32_t x = yapf__get_sample(src + (i * ch + c) * sb, sample);
            int      alpha = (ch == 2 && c == 1) || (ch == 4 && c == 3);
            uint8_t  b;
            if (sample == YAPF_SAMPLE_U8) b = (uint8_t)x;
            else if (sample == YAPF_SAMPLE_U16) b = (uint8_t)((x + 128) / 257);
            else {
                float f;
                if (sample == YAPF_SAMPLE_F16) f = yapf_half_to_float((uint16_t)x);
                else memcpy(&f, &x, 4);
                b = alpha ? (uint8_t)yapf__clampi(f * 255.0f, 0, 255) : yapf__lin_to_code(st, f);
            }
            v[ch <= 2 ? (c == 0 ? 0 : 3) : c] = b;
        }
        if (ch <= 2) v[1] = v[2] = v[0];
        memcpy(out + i * 4, v, 4);
    }
    free(st);
    free(half);
    return YAPF_OK;
}
