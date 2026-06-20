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
 *  Space savings come from two cheap mathematical steps — no entropy coder,
 *  no lookup tables, no opcode dispatch.  Decode is straight arithmetic:
 *
 *    1. YCoCg-R color transform (RGB↔Y/Co/Cg, fully reversible integers)
 *    2. 2-D neighbor prediction + per-tile adaptive bit-packing
 *
 *  The file carries GPU metadata (format hint, mip levels, flags) so a
 *  loader can upload the decoded buffer straight to a texture without any
 *  additional transcoding step.
 *
 * =========================================================================
 *  FILE FORMAT (version 2)
 * =========================================================================
 *
 *  All multi-byte integers are little-endian.
 *
 *  ┌─────────────────────────────────────────────────────────────────────┐
 *  │  HEADER — 20 bytes                                                  │
 *  ├────────┬──────┬────────────────────────────────────────────────────┤
 *  │ Offset │ Size │ Field                                               │
 *  ├────────┼──────┼────────────────────────────────────────────────────┤
 *  │  0x00  │  4   │ Magic: "YAPF"                                       │
 *  │  0x04  │  1   │ Version: 2                                          │
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
 *
 *  ┌─────────────────────────────────────────────────────────────────────┐
 *  │  PIXEL DATA — per mip level, referenced by the mip index           │
 *  ├─────────────────────────────────────────────────────────────────────┤
 *  │  Pixels are divided into 32×32 tiles, stored in row-major order.   │
 *  │  The last column / row of tiles may be smaller (no padding).       │
 *  │                                                                     │
 *  │  Encoding pipeline (encoder):                                       │
 *  │    a) RGB→YCoCg-R  (skipped for gray/gray-alpha channels)          │
 *  │    b) 2-D prediction residuals  (see below)                         │
 *  │    c) Determine per-channel bit depth from max |residual| in tile   │
 *  │    d) Write tile header + packed residuals                          │
 *  │                                                                     │
 *  │  Tile layout:                                                       │
 *  │    [bit_depth per channel]  1 byte each, channels bytes total       │
 *  │    [first pixel, raw]       1 byte per channel                      │
 *  │    [packed residuals]       bit_depth[c] bits per sample,           │
 *  │                             stored MSB-first, zero-padded to the    │
 *  │                             next byte boundary at the end of tile   │
 *  │                                                                     │
 *  │  Prediction (lossless 2-D):                                         │
 *  │    pixel (0,0) in tile → stored raw, no residual                   │
 *  │    pixel (x,0)         → predict = left                             │
 *  │    pixel (0,y)         → predict = above                            │
 *  │    pixel (x,y)         → predict = left + above − diag             │
 *  │    residual = (actual − predict) wrapped mod 256, stored signed     │
 *  │                                                                     │
 *  │  Bit packing:                                                       │
 *  │    bits_needed = bit_depth[c]  (0 means all residuals are 0)       │
 *  │    Each residual is stored as a (bits_needed)-bit two's-complement  │
 *  │    signed integer, MSB first, packed across byte boundaries.        │
 *  │    bit_depth 1 → only the sign bit (value is −1 or 0)              │
 *  │    bit_depth 0 → no bits written; all samples in channel = 0       │
 *  │                                                                     │
 *  │  YCoCg-R forward transform (applied before prediction):            │
 *  │    Co = R − B                                                       │
 *  │    t  = B + (Co >> 1)                                               │
 *  │    Cg = G − t                                                       │
 *  │    Y  = t + (Cg >> 1)                                               │
 *  │    Stored channel order: Y, Co, Cg  (replaces R, G, B)             │
 *  │    Alpha is left unchanged.                                         │
 *  │                                                                     │
 *  │  YCoCg-R inverse transform (applied after prediction):             │
 *  │    t  = Y − (Cg >> 1)                                               │
 *  │    G  = Cg + t                                                      │
 *  │    B  = t  − (Co >> 1)                                              │
 *  │    R  = B + Co                                                      │
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
#define YAPF_TILE_SIZE    32u

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
 * yapf_save — encode an image to disk.
 * img->mip_levels controls how many levels are written; set to 1 to write
 * only the base level.  img->mips may be NULL when mip_levels == 1.
 * Returns YAPF_OK or a negative YAPF_ERR_* code.
 */
int yapf_save(const char *filename, const yapf_image_t *img);

/*
 * yapf_free — release all memory owned by an image returned by yapf_load().
 * NULL-safe.
 */
void yapf_free(yapf_image_t *img);

#ifdef __cplusplus
}
#endif

#endif /* YAPF_H */
