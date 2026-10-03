/*
 * yapf.h  —  Yet Another Picture Format
 * Public API and format specification.
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
 *
 * -------------------------------------------------------------------------
 *
 *  YAPF — lossless, GPU-ready image format.
 *
 *  Designed for single-core decode speed.  Everything is byte aligned:
 *  no bit reader, no entropy coder, no context model.  Per row the decoder
 *  dispatches one filter; per 8 samples it reads a 4-bit control code and
 *  exactly w bytes of payload, unpacked with one 64-bit load and shifts.
 *  The encoder does the hard work (filter search, colour transform choice,
 *  optimal control codes); the decoder only follows instructions.
 *
 *    1. Per tile: YCoCg-R or subtract-green colour transform, 8-bit modular
 *    2. Per band of 8 rows: one of 12 row filters
 *    3. Residuals in groups of 8, each group at its bit width (0–8);
 *       4-bit control codes, with codes that repeat the previous width
 *    4. Independent 64×64 tiles with a size table → random access and
 *       optional parallel decode
 *
 *  The file carries GPU metadata (format hint, mip levels, flags) so a
 *  loader can upload the decoded buffer straight to a texture without any
 *  additional transcoding step.
 *
 * =========================================================================
 *  FILE FORMAT (version 1)
 * =========================================================================
 *
 *  All multi-byte integers are little-endian.  All arithmetic on samples
 *  is mod 256.
 *
 *  ┌─────────────────────────────────────────────────────────────────────┐
 *  │  HEADER — 20 bytes                                                  │
 *  ├────────┬──────┬────────────────────────────────────────────────────┤
 *  │ Offset │ Size │ Field                                               │
 *  ├────────┼──────┼────────────────────────────────────────────────────┤
 *  │  0x00  │  4   │ Magic: "YAPF"                                       │
 *  │  0x04  │  1   │ Version: 1                                          │
 *  │  0x05  │  1   │ Channels: 1 gray / 2 gray+A / 3 RGB / 4 RGBA       │
 *  │  0x06  │  1   │ GPU format hint  (YAPF_GPU_* constant)              │
 *  │  0x07  │  1   │ Mip levels (1 = base only, max 16)                 │
 *  │  0x08  │  1   │ Flags  (see YAPF_FLAG_* bitmask)                   │
 *  │  0x09  │  3   │ Reserved (write 0, ignore on read)                  │
 *  │  0x0C  │  4   │ Base width  (uint32, pixels)                        │
 *  │  0x10  │  4   │ Base height (uint32, pixels)                        │
 *  └────────┴──────┴────────────────────────────────────────────────────┘
 *
 *  ┌─────────────────────────────────────────────────────────────────────┐
 *  │  MIP INDEX — 8 bytes × mip_levels, immediately after header        │
 *  ├────────┬──────┬────────────────────────────────────────────────────┤
 *  │  +0    │  4   │ Byte offset of mip data from start of file (uint32) │
 *  │  +4    │  4   │ Byte size of mip data (uint32)                      │
 *  └────────┴──────┴────────────────────────────────────────────────────┘
 *  Mip m has size max(1, width >> m) × max(1, height >> m).
 *
 *  ┌─────────────────────────────────────────────────────────────────────┐
 *  │  MIP LEVEL DATA                                                     │
 *  ├─────────────────────────────────────────────────────────────────────┤
 *  │  Pixels are divided into 64×64 tiles in row-major order; the last  │
 *  │  column / row of tiles may be smaller (no padding).                │
 *  │                                                                     │
 *  │    uint32 tile_size[tile_count]    byte size of each tile           │
 *  │    tile data, back to back                                          │
 *  │                                                                     │
 *  │  TILE                                                               │
 *  │    1 byte header                                                    │
 *  │      0x80        stored: tile_w × tile_h × channels raw bytes       │
 *  │                  follow, interleaved, in the original channel order │
 *  │      otherwise   coded:                                             │
 *  │        bits 0–3  bit c set = plane c is constant (only bits below   │
 *  │                  the channel count may be set)                      │
 *  │        bit 4     colour transform: 0 = YCoCg-R, 1 = subtract-green  │
 *  │                  (RGB / RGBA only; must be 0 for gray)              │
 *  │        bits 5–7  must be 0                                          │
 *  │    coded tiles then hold one value byte per constant plane, and a   │
 *  │    plane payload per other plane, both in plane order.  The tile    │
 *  │    must end exactly where its last payload ends.                    │
 *  │                                                                     │
 *  │  PLANES                                                             │
 *  │    gray / gray+A: the channels as stored.                           │
 *  │    RGB(A), YCoCg-R (>> is an arithmetic shift of the value as int8):│
 *  │      planes  Y, Co, Cg (+ A)                                        │
 *  │      forward:  o = R − B;  t = B + (o >> 1);  q = G − t;            │
 *  │                Y = t + (q >> 1);  Co = o + 128;  Cg = q + 128       │
 *  │      inverse:  o = Co − 128;  q = Cg − 128;  t = Y − (q >> 1);      │
 *  │                G = q + t;  B = t − (o >> 1);  R = B + o             │
 *  │    RGB(A), subtract-green:                                          │
 *  │      planes  G, R − G + 128, B − G + 128 (+ A)                      │
 *  │                                                                     │
 *  │  PLANE PAYLOAD                                                      │
 *  │    filter[ceil(tile_h / 8)]   one byte per band of 8 rows.          │
 *  │      L / A / D / AR = left, above, above-left, above-right sample;  │
 *  │      samples outside the tile read as 0, except that AR in the      │
 *  │      last column reads as A.                                        │
 *  │        0         Left        pred = L                               │
 *  │        1         Gradient    pred = L + A − D                       │
 *  │        2         Up-right    pred = AR                              │
 *  │        3         Up-average  pred = (A + AR) >> 1   (not mod 256)   │
 *  │        16 + k−1  Up k        pred = sample k rows above, k = 1..8   │
 *  │    control nibbles, low nibble of each byte first, covering         │
 *  │      exactly ceil(tile_w × tile_h / 8) groups of 8 consecutive      │
 *  │      residuals in raster order:                                     │
 *  │        0–8       one group of width w                               │
 *  │        9–15      2, 3, 4, 8, 16, 32 or 64 groups at the width of    │
 *  │                  the last 0–8 code in this plane (0 if none yet)    │
 *  │      an odd nibble count is padded with a zero high nibble          │
 *  │    group data: w bytes per group, the 8 values packed little-       │
 *  │      endian, value i in bits [i·w, i·w + w)                         │
 *  │    Residual r = (sample − pred) mod 256 is stored folded:           │
 *  │      v = (r << 1) ^ (r >> 7)    (0, −1, 1, −2, … → 0, 1, 2, 3, …)   │
 *  │    The last group is padded with zero values.                       │
 *  └─────────────────────────────────────────────────────────────────────┘
 *
 *  GPU FORMAT HINTS  (YAPF_GPU_* values for the gpu_format field)
 *  ──────────────────────────────────────────────────────────────
 *  0x00  RGBA8       GL_RGBA8 / VK_FORMAT_R8G8B8A8_UNORM
 *  0x01  RGB8        GL_RGB8  / VK_FORMAT_R8G8B8_UNORM
 *  0x02  RG8         GL_RG8   / VK_FORMAT_R8G8_UNORM
 *  0x03  R8          GL_R8    / VK_FORMAT_R8_UNORM
 *  0x04  SRGB8_A8    GL_SRGB8_ALPHA8
 *  0x05  SRGB8       GL_SRGB8
 *
 *  FLAGS  (YAPF_FLAG_* bitmask for the flags field)
 *  ─────────────────────────────────────────────────
 *  bit 0  PREMULT_ALPHA   alpha is premultiplied into RGB
 *  bit 1  SRGB            pixels are in sRGB gamma (informational only)
 */

#ifndef YAPF_H
#define YAPF_H

#include <stdint.h>
#include <stddef.h>

/* ── GPU format hints ────────────────────────────────────────────────── */

#define YAPF_GPU_RGBA8    0x00
#define YAPF_GPU_RGB8     0x01
#define YAPF_GPU_RG8      0x02
#define YAPF_GPU_R8       0x03
#define YAPF_GPU_SRGB8_A8 0x04
#define YAPF_GPU_SRGB8    0x05

/* ── Flags ───────────────────────────────────────────────────────────── */

#define YAPF_FLAG_PREMULT_ALPHA 0x01
#define YAPF_FLAG_SRGB          0x02

/* ── Channel constants ───────────────────────────────────────────────── */

#define YAPF_CHANNELS_GRAY       1
#define YAPF_CHANNELS_GRAY_ALPHA 2
#define YAPF_CHANNELS_RGB        3
#define YAPF_CHANNELS_RGBA       4

/* ── Return codes ────────────────────────────────────────────────────── */

#define YAPF_OK           0
#define YAPF_ERR_INVALID -1
#define YAPF_ERR_IO      -2
#define YAPF_ERR_OOM     -3
#define YAPF_ERR_CORRUPT -4

/* ── Limits ──────────────────────────────────────────────────────────── */

#define YAPF_MAX_DIM      65535u
#define YAPF_MAX_MIPS     16u
#define YAPF_TILE_SIZE    64u

/* ── Image descriptor ────────────────────────────────────────────────── */

typedef struct {
    uint32_t  width;
    uint32_t  height;
    uint8_t   channels;    /* 1–4 */
    uint8_t   gpu_format;  /* YAPF_GPU_* */
    uint8_t   flags;       /* YAPF_FLAG_* bitmask */
    uint8_t   mip_levels;  /* number of mip levels stored (1 = base only) */
    uint8_t  *pixels;      /* base-level pixels, malloc-owned */
    uint8_t **mips;        /* mips[0]=base alias, mips[1..mip_levels-1] owned */
} yapf_image_t;

/* ── Public API ──────────────────────────────────────────────────────── */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * yapf_load — decode a .yapf file.
 * Returns NULL on any failure (bad magic, truncated data, OOM, I/O error).
 * The returned image must be released with yapf_free().
 */
yapf_image_t *yapf_load(const char *filename);

/*
 * yapf_load_memory — decode a .yapf file already in memory (e.g. from a
 * game archive).  Same ownership and failure rules as yapf_load().
 */
yapf_image_t *yapf_load_memory(const void *buffer, size_t size);

/*
 * yapf_load_mt / yapf_load_memory_mt — same as above, but decode tiles on
 * up to `threads` threads (0 = one per CPU core).  Output is identical.
 * Without thread support (YAPF_NO_THREADS, or an unknown platform) these
 * decode on the calling thread.
 */
yapf_image_t *yapf_load_mt(const char *filename, int threads);
yapf_image_t *yapf_load_memory_mt(const void *buffer, size_t size, int threads);

/*
 * yapf_save — encode an image to disk.
 * img->mip_levels controls how many levels are written; set to 1 to write
 * only the base level.  img->mips may be NULL when mip_levels == 1.
 * Returns YAPF_OK or a negative YAPF_ERR_* code.
 */
int yapf_save(const char *filename, const yapf_image_t *img);

/*
 * yapf_encode — encode an image into memory.
 * On success *out_data points to *out_size bytes of a complete .yapf file;
 * release it with yapf_free_buffer().  Same rules and return codes as
 * yapf_save() (no YAPF_ERR_IO).
 */
int yapf_encode(const yapf_image_t *img, void **out_data, size_t *out_size);

/* yapf_free_buffer — release memory returned by yapf_encode().  NULL-safe. */
void yapf_free_buffer(void *data);

/*
 * yapf_free — release all memory owned by an image returned by yapf_load().
 * NULL-safe.
 */
void yapf_free(yapf_image_t *img);

#ifdef __cplusplus
}
#endif

#endif /* YAPF_H */
