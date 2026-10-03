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
        if (xf) {                                         /* subtract-green */
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
        if (xf) {
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
    int ch)
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

    if (yapf__reserve(o, 1 + 4) != 0) return -1;
    o->buf[o->len++] = (uint8_t)(mask[t] | (t ? YAPF__SUBGREEN : 0));
    for (int c = 0; c < ch; c++)
        if (mask[t] & (1u << c)) o->buf[o->len++] = sc->plane[t][c][0];
    for (int c = 0; c < ch; c++)
        if (!(mask[t] & (1u << c)) &&
            yapf__emit_plane(o, sc->z[t][c], sc->filt[t][c], tw, th) != 0) return -1;

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

static int yapf__decode_tile(
    const uint8_t *data, size_t len,
    uint8_t *pixels, uint32_t img_w,
    uint32_t tx, uint32_t ty, uint32_t tw, uint32_t th,
    int ch, int simd)
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
    if (hdr & 0xE0) return -1;                      /* reserved bits set */
    uint8_t xf = hdr & YAPF__SUBGREEN;
    hdr &= 0x0F;
    if ((hdr >> ch) || (xf && ch < 3)) return -1;   /* unknown bits set */

    for (int c = 0; c < ch; c++) {
        if (!(hdr & (1u << c))) continue;
        if (p >= end) return -1;
        memset(rows[c], *p++, sizeof(rows[c]));
    }
    for (int c = 0; c < ch; c++) {
        if (hdr & (1u << c)) continue;
        p = yapf__unpack_plane(p, end, z[c], &filt[c], tw, th, simd);
        if (!p) return -1;
        /* SIMD row kernels read whole 16-sample chunks; keep the unused
         * tail of narrow tiles defined. */
        if (tw != YAPF_TILE_SIZE) {
            size_t used = ((size_t)tw * th + YAPF__GROUP - 1) / YAPF__GROUP * YAPF__GROUP;
            memset(z[c] + used, 0, sizeof(z[c]) - used);
        }
    }
    if (p != end) return -1;

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
            if (xf) {
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
            rc = yapf__encode_tile(o, sc, pixels, mw, x0, y0, tw, th, ch);
            if (rc == 0) yapf__w32le(o->buf + t * 4, (uint32_t)(o->len - start));
        }
    }
    free(sc);
    return rc;
}

/* A level whose tile table has been validated; tiles can then be decoded
 * in any order, from any thread. */
typedef struct {
    const uint8_t *data;
    size_t        *offs;     /* tcount + 1 byte offsets into data */
    uint8_t       *pixels;
    uint32_t       mw, mh, tcx, tcy;
    int            ch;
    int            simd;     /* SIMD kernels usable on this CPU */
} yapf__level;

static int yapf__level_init(yapf__level *lv, const uint8_t *data, size_t len,
                            uint8_t *pixels, uint32_t mw, uint32_t mh, int ch)
{
    lv->data   = data;
    lv->pixels = pixels;
    lv->mw     = mw;
    lv->mh     = mh;
    lv->tcx    = yapf__tiles(mw);
    lv->tcy    = yapf__tiles(mh);
    lv->ch     = ch;

    size_t tcount = (size_t)lv->tcx * lv->tcy;
    if (len / 4 < tcount) return -1;
    lv->offs = (size_t *)malloc((tcount + 1) * sizeof(size_t));
    if (!lv->offs) return -1;

    size_t pos = tcount * 4;
    for (size_t t = 0; t < tcount; t++) {
        uint32_t sz = yapf__r32le(data + t * 4);
        if (sz > len - pos) return -1;
        lv->offs[t] = pos;
        pos += sz;
    }
    lv->offs[tcount] = pos;
    return 0;
}

/* Decode tile rows first_row, first_row + step, ... */
static int yapf__level_decode_rows(const yapf__level *lv,
                                   uint32_t first_row, uint32_t step)
{
    for (uint32_t ty = first_row; ty < lv->tcy; ty += step) {
        for (uint32_t tx = 0; tx < lv->tcx; tx++) {
            size_t   t  = (size_t)ty * lv->tcx + tx;
            uint32_t x0 = tx * YAPF_TILE_SIZE, y0 = ty * YAPF_TILE_SIZE;
            uint32_t tw = lv->mw - x0 < YAPF_TILE_SIZE ? lv->mw - x0 : YAPF_TILE_SIZE;
            uint32_t th = lv->mh - y0 < YAPF_TILE_SIZE ? lv->mh - y0 : YAPF_TILE_SIZE;
            if (yapf__decode_tile(lv->data + lv->offs[t],
                                  lv->offs[t + 1] - lv->offs[t],
                                  lv->pixels, lv->mw, x0, y0, tw, th, lv->ch,
                                  lv->simd) != 0)
                return -1;
        }
    }
    return 0;
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
    if ((uint32_t)n > levels[0].tcy) n = (int)levels[0].tcy;
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
        if (jobs[i].rc != 0) return -1;
    return 0;
}

/* ========================================================================
 *  yapf_load_memory / yapf_load
 * ======================================================================== */

static yapf_image_t *yapf__load(const uint8_t *buf, size_t size, int threads) {
    if (!buf || size < YAPF__HDR_LEN) return NULL;
    if (memcmp(buf, YAPF__MAGIC, 4) != 0) return NULL;

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

static yapf_image_t *yapf__load_file(const char *filename, int threads) {
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
