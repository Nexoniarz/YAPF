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
 *  Two versions exist.  Version 1 (below) is a single 8-bit 2D image with
 *  optional mips.  Version 2 is the texture container: GPU block payloads
 *  (BCn / ETC2 / ASTC), 16-bit and float samples, cubemaps, arrays and
 *  volumes, channel meanings, per-tile checksums, optional entropy-coded
 *  planes, notes (metadata) and animation frames.  Its complete
 *  specification is docs/FORMAT.md; a summary follows the v1 spec.
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
 *  bit 2  COVERAGE        (v2) mips keep alpha-test coverage at alpha_ref
 *
 * =========================================================================
 *  FILE FORMAT (version 2) — summary; the full text is docs/FORMAT.md
 * =========================================================================
 *
 *  HEADER (64 bytes)
 *    0x00 "YAPF"   0x04 version 2   0x05 channels   0x06 format
 *    0x07 mips     0x08 flags       0x09 payload    0x0A sample   0x0B shape
 *    0x0C width u32   0x10 height u32   0x14 depth u32
 *    0x18 layers u16  0x1A frames u16   0x1C needs u32
 *    0x20 channel map[4]  0x24 linear mask  0x25 normal  0x26 primaries
 *    0x27 transfer  0x28 mip filter  0x29 alpha ref  0x2A loop count u16
 *    0x2C notes size u32   0x30 file size u64
 *    0x38 tables check u32 (yapf-sum of bytes [0x40, data start))
 *    0x3C header check u32 (yapf-sum of bytes [0, 0x3C))
 *  SURFACE TABLE   16 bytes per surface: offset u64, size u32,
 *                  tile-table check u32.  Surface s of (frame, layer,
 *                  face, mip) is ((frame·layers + layer)·faces + face)·mips + mip
 *  FRAME TABLE     u32 duration in µs per frame (only when frames > 1)
 *  NOTES           tag[4], length u32, bytes — repeated
 *  SURFACE DATA    smallest mips first, so a file prefix is a complete
 *                  low-resolution texture.  Per surface: u32 book size,
 *                  code book (Huffman tables by slot and context, only
 *                  with entropy coding), tile table (size u32 with bit 31 =
 *                  packed, yapf-sum u32 per tile), then the tiles.  Pixel
 *                  tiles are the v1 tiles plus byte planes for wide samples
 *                  and entropy-coded planes; block tiles hold 16 × 16 GPU
 *                  blocks, raw or packed lane by lane.
 *  NEEDS           a decoder must refuse a file with a needs bit it does
 *                  not implement; everything else may be ignored safely.
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
/* v2 pixel hints for wide samples */
#define YAPF_GPU_RGBA16   0x06
#define YAPF_GPU_RGB16    0x07
#define YAPF_GPU_RG16     0x08
#define YAPF_GPU_R16      0x09
#define YAPF_GPU_RGBA16F  0x0A
#define YAPF_GPU_RGB16F   0x0B
#define YAPF_GPU_RG16F    0x0C
#define YAPF_GPU_R16F     0x0D
#define YAPF_GPU_RGBA32F  0x0E
#define YAPF_GPU_RGB32F   0x0F
#define YAPF_GPU_RG32F    0x10
#define YAPF_GPU_R32F     0x11
/* v2 GPU block formats (payload = YAPF_PAYLOAD_BLOCKS).  The sRGB flag
 * selects the _SRGB variant where the API has one. */
#define YAPF_GPU_BC1          0x40   /* RGB(A1), 8 bytes per 4×4      */
#define YAPF_GPU_BC2          0x41   /* RGBA, explicit 4-bit alpha    */
#define YAPF_GPU_BC3          0x42   /* RGBA                          */
#define YAPF_GPU_BC4          0x43   /* R                             */
#define YAPF_GPU_BC4_SNORM    0x44
#define YAPF_GPU_BC5          0x45   /* RG (normal maps)              */
#define YAPF_GPU_BC5_SNORM    0x46
#define YAPF_GPU_BC6H         0x47   /* RGB half float, unsigned      */
#define YAPF_GPU_BC6H_SF      0x48   /* RGB half float, signed        */
#define YAPF_GPU_BC7          0x49   /* RGB(A)                        */
#define YAPF_GPU_ETC2_RGB8    0x50
#define YAPF_GPU_ETC2_RGB8A1  0x51
#define YAPF_GPU_ETC2_RGBA8   0x52
#define YAPF_GPU_EAC_R11      0x53
#define YAPF_GPU_EAC_R11_SNORM  0x54
#define YAPF_GPU_EAC_RG11     0x55
#define YAPF_GPU_EAC_RG11_SNORM 0x56
#define YAPF_GPU_ASTC_4x4     0x60   /* ASTC: 0x60 + footprint index  */
#define YAPF_GPU_ASTC_5x4     0x61
#define YAPF_GPU_ASTC_5x5     0x62
#define YAPF_GPU_ASTC_6x5     0x63
#define YAPF_GPU_ASTC_6x6     0x64
#define YAPF_GPU_ASTC_8x5     0x65
#define YAPF_GPU_ASTC_8x6     0x66
#define YAPF_GPU_ASTC_8x8     0x67
#define YAPF_GPU_ASTC_10x5    0x68
#define YAPF_GPU_ASTC_10x6    0x69
#define YAPF_GPU_ASTC_10x8    0x6A
#define YAPF_GPU_ASTC_10x10   0x6B
#define YAPF_GPU_ASTC_12x10   0x6C
#define YAPF_GPU_ASTC_12x12   0x6D

/* ── Flags ───────────────────────────────────────────────────────────── */

#define YAPF_FLAG_PREMULT_ALPHA 0x01
#define YAPF_FLAG_SRGB          0x02
#define YAPF_FLAG_COVERAGE      0x04   /* v2: mips preserve alpha-test coverage */

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
#define YAPF_ERR_UNSUPPORTED -5   /* valid file, feature this decoder lacks  */
#define YAPF_ERR_CHECKSUM    -6   /* a yapf-sum does not match               */
#define YAPF_ERR_TRUNCATED   -7   /* data lies beyond the bytes available yet */

/* ── Limits ──────────────────────────────────────────────────────────── */

#define YAPF_MAX_DIM      65535u
#define YAPF_MAX_MIPS     16u
#define YAPF_TILE_SIZE    64u
#define YAPF_BLOCK_TILE   16u      /* v2 block tiles: 16 × 16 GPU blocks */

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

/* ── Version 2: texture description ──────────────────────────────────── */

/* payload */
#define YAPF_PAYLOAD_PIXELS  0   /* lossless YAPF tiles                     */
#define YAPF_PAYLOAD_BLOCKS  1   /* GPU blocks, uploaded as they are        */

/* sample: how one channel value is stored (pixel payload); for block
 * payloads, the type of the decoded texels (U8, or F16 for BC6H) */
#define YAPF_SAMPLE_U8   0
#define YAPF_SAMPLE_U16  1
#define YAPF_SAMPLE_F16  2       /* IEEE half, as uint16_t bits             */
#define YAPF_SAMPLE_F32  3

/* shape */
#define YAPF_SHAPE_2D      0
#define YAPF_SHAPE_CUBE    1     /* 6 faces: +X −X +Y −Y +Z −Z; with layers
                                    > 1 a cube array                        */
#define YAPF_SHAPE_VOLUME  2     /* depth slices; mips halve depth too      */

/* needs: features a decoder must implement to read the file */
#define YAPF_NEED_WIDE     0x01  /* sample other than U8 (pixel payload)    */
#define YAPF_NEED_BLOCKS   0x02  /* GPU block payload                       */
#define YAPF_NEED_ENTROPY  0x04  /* code books: entropy planes, packed blocks */
#define YAPF_NEED_LAYOUT   0x08  /* cube, volume or layers > 1              */
#define YAPF_NEED_FRAMES   0x10  /* frames > 1                              */
#define YAPF_NEED_ALL      0x1F  /* everything this library implements     */

/* channel meanings (channel_map[c]); 0 = default for the channel count */
#define YAPF_CH_DEFAULT    0
#define YAPF_CH_RED        1
#define YAPF_CH_GREEN      2
#define YAPF_CH_BLUE       3
#define YAPF_CH_ALPHA      4
#define YAPF_CH_LUMA       5
#define YAPF_CH_NORMAL_X   6
#define YAPF_CH_NORMAL_Y   7
#define YAPF_CH_NORMAL_Z   8
#define YAPF_CH_ROUGHNESS  9
#define YAPF_CH_GLOSS      10    /* smoothness = 1 − roughness              */
#define YAPF_CH_METALNESS  11
#define YAPF_CH_OCCLUSION  12    /* ambient occlusion                       */
#define YAPF_CH_HEIGHT     13
#define YAPF_CH_SPECULAR   14
#define YAPF_CH_EMISSIVE   15
#define YAPF_CH_OPACITY    16    /* alpha-test / cut-out mask               */
#define YAPF_CH_MASK       17    /* generic mask                            */
#define YAPF_CH_CAVITY     18
#define YAPF_CH_THICKNESS  19
#define YAPF_CH_ID         20    /* material or object id                   */
#define YAPF_CH_UNUSED     255

/* normal: bits 0–1 convention, bits 2–3 encoding */
#define YAPF_NORMAL_NONE   0x00
#define YAPF_NORMAL_GL     0x01  /* +Y up   (OpenGL, Blender, Unity)        */
#define YAPF_NORMAL_DX     0x02  /* −Y      (DirectX, Unreal)               */
#define YAPF_NORMAL_XYZ    0x00
#define YAPF_NORMAL_XY     0x04  /* Z = sqrt(1 − x² − y²) is reconstructed  */
#define YAPF_NORMAL_OCT    0x08  /* octahedral                              */

/* primaries / transfer (0 = unspecified: the sRGB flag decides) */
#define YAPF_PRIMARIES_UNSPECIFIED 0
#define YAPF_PRIMARIES_BT709       1   /* = sRGB */
#define YAPF_PRIMARIES_DISPLAY_P3  2
#define YAPF_PRIMARIES_BT2020      3
#define YAPF_PRIMARIES_ACES_AP0    4
#define YAPF_PRIMARIES_ACES_AP1    5   /* ACEScg */
#define YAPF_PRIMARIES_ADOBE_RGB   6

#define YAPF_TRANSFER_UNSPECIFIED  0
#define YAPF_TRANSFER_SRGB         1
#define YAPF_TRANSFER_LINEAR       2
#define YAPF_TRANSFER_GAMMA22      3
#define YAPF_TRANSFER_PQ           4   /* SMPTE ST 2084 */
#define YAPF_TRANSFER_HLG          5
#define YAPF_TRANSFER_BT1886       6   /* gamma 2.4 */

/* mip filter (bits 0–6) | YAPF_MIP_LINEAR_LIGHT */
#define YAPF_MIP_UNKNOWN   0
#define YAPF_MIP_BOX       1
#define YAPF_MIP_TENT      2
#define YAPF_MIP_KAISER    3
#define YAPF_MIP_LANCZOS   4
#define YAPF_MIP_MITCHELL  5
#define YAPF_MIP_AUTHORED  6     /* made by hand / by an external tool      */
#define YAPF_MIP_LINEAR_LIGHT 0x80  /* sRGB channels were filtered in linear light */

/* load flags */
#define YAPF_LOAD_TRUST    0x01  /* skip checksum verification             */

/* encode flags */
#define YAPF_ENC_ENTROPY   0x01  /* allow entropy-coded planes and packed
                                    blocks where they are smaller: ~10 %
                                    smaller photos, ~3× smaller GPU blocks
                                    on disk; slower to decode              */

/* A note (metadata entry).  Well-known tags: "AUTH" author, "LICN"
 * licence, "SRCE" source, "TOOL" creating tool, "TITL" title, "DESC"
 * description, "DATE" ISO 8601 date (all UTF-8 text); "ICCP" ICC profile;
 * "ORNT" orientation (1 byte); "DPI " 2 × u32 milli-dots per inch;
 * "CLL " 2 × u16 max content / max frame-average light level (cd/m²).
 * Tags that start with a lowercase letter belong to applications. */
typedef struct {
    char      tag[4];
    uint32_t  size;
    uint8_t  *data;
} yapf_note_t;

/*
 * A version 2 texture.  Surfaces are numbered
 *   s = ((frame × layers + layer) × faces + face) × mip_levels + mip
 * (faces = 6 for cubes, else 1) and data[s] holds the whole surface:
 *   pixels: width_m × height_m × depth_m × channels samples, rows top to
 *           bottom, slices in order; samples in the native byte order
 *           (uint8_t, uint16_t, half bits as uint16_t, float);
 *   blocks: ceil(width_m / bw) × ceil(height_m / bh) blocks per slice,
 *           row-major — exactly what glCompressedTexImage / Vulkan take.
 * width_m = max(1, width >> m), likewise height and (volumes) depth.
 */
typedef struct {
    uint32_t  width, height, depth;  /* depth: volumes only, else 1        */
    uint16_t  layers;                /* array layers, >= 1                 */
    uint16_t  frames;                /* animation frames, >= 1             */
    uint8_t   shape;                 /* YAPF_SHAPE_*                       */
    uint8_t   mip_levels;            /* 1–16                               */
    uint8_t   payload;               /* YAPF_PAYLOAD_*                     */
    uint8_t   format;                /* YAPF_GPU_*: block format, or hint  */
    uint8_t   sample;                /* YAPF_SAMPLE_*                      */
    uint8_t   channels;              /* 1–4                                */
    uint8_t   flags;                 /* YAPF_FLAG_*                        */
    uint8_t   channel_map[4];        /* YAPF_CH_* per channel              */
    uint8_t   linear_mask;           /* bit c: channel c is linear even in
                                        an sRGB image (e.g. alpha, masks)  */
    uint8_t   normal;                /* YAPF_NORMAL_*                      */
    uint8_t   primaries, transfer;   /* YAPF_PRIMARIES_*, YAPF_TRANSFER_*  */
    uint8_t   mip_filter;            /* YAPF_MIP_*                         */
    uint8_t   alpha_ref;             /* coverage reference (with COVERAGE) */
    uint16_t  loop_count;            /* animation loops, 0 = forever       */
    uint32_t *frame_us;              /* frames durations in µs, or NULL    */
    uint32_t     note_count;
    yapf_note_t *notes;
    void    **data;                  /* one pointer per surface            */
    void     *internal_;             /* library use: file tables, ownership */
} yapf_texture_t;

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

/*
 * The v1 functions above also read version 2 files that hold 8-bit pixels
 * of a 2D, cube or array texture: they return frame 0, layer 0, face 0
 * with all its mips.  yapf_save / yapf_encode always write version 1.
 */

/* ── Version 2: textures ─────────────────────────────────────────────── */

/*
 * yapf_tex_create — a library-owned texture with the description of
 * `desc` (every field up to loop_count; notes, frame times and data are
 * not copied) and zeroed storage for every surface.  frame_us is
 * allocated (zeroed) when frames > 1.  NULL on invalid description / OOM.
 */
yapf_texture_t *yapf_tex_create(const yapf_texture_t *desc);

/* yapf_tex_free — release a texture made by the library.  NULL-safe. */
void yapf_tex_free(yapf_texture_t *tex);

/* Surface helpers. */
uint32_t yapf_tex_faces(const yapf_texture_t *tex);           /* 6 or 1 */
uint32_t yapf_tex_surface_count(const yapf_texture_t *tex);
uint32_t yapf_tex_surface(const yapf_texture_t *tex, uint32_t frame,
                          uint32_t layer, uint32_t face, uint32_t mip);
void     yapf_tex_mip_size(const yapf_texture_t *tex, uint32_t mip,
                           uint32_t *w, uint32_t *h, uint32_t *d);
size_t   yapf_tex_surface_bytes(const yapf_texture_t *tex, uint32_t mip);
size_t   yapf_sample_bytes(uint8_t sample);                   /* 1, 2 or 4 */

/* Notes: add (library-owned textures only; data is copied) and find. */
int yapf_tex_add_note(yapf_texture_t *tex, const char tag[4],
                      const void *data, uint32_t size);
const yapf_note_t *yapf_tex_find_note(const yapf_texture_t *tex, const char tag[4]);

/*
 * yapf_tex_encode / yapf_tex_save — write a version 2 file.  `flags` is
 * YAPF_ENC_*.  Works on library-owned and caller-built textures alike
 * (nothing is taken over).  Returns YAPF_OK or a YAPF_ERR_* code.
 */
int yapf_tex_encode(const yapf_texture_t *tex, int flags,
                    void **out_data, size_t *out_size);
int yapf_tex_save(const char *filename, const yapf_texture_t *tex, int flags);

/*
 * yapf_tex_load / yapf_tex_load_memory — read a version 1 or 2 file and
 * decode every surface on up to `threads` threads (0 = one per core).
 * `flags` is YAPF_LOAD_*.  On failure NULL is returned and *err (if not
 * NULL) receives the YAPF_ERR_* reason.
 */
yapf_texture_t *yapf_tex_load(const char *filename, int flags, int threads, int *err);
yapf_texture_t *yapf_tex_load_memory(const void *buffer, size_t size,
                                     int flags, int threads, int *err);

/*
 * Streaming and partial decoding.
 *
 * yapf_tex_open — read only the header, tables and notes.  `avail` may be
 * less than the whole file (a download in progress): it must cover the
 * front matter, else YAPF_ERR_TRUNCATED.  All data[] pointers are NULL.
 *
 * yapf_tex_stream_bytes — how many leading bytes of the file hold every
 * surface of mip `first_mip` and smaller (the file stores small mips
 * first, so these are a usable texture on their own).
 *
 * yapf_tex_decode_surface — decode surface s into tex->data[s]
 * (allocated).  `buffer`/`avail` is the file or a prefix of it; returns
 * YAPF_ERR_TRUNCATED when the surface is not in it yet.
 *
 * yapf_tex_tile_grid / yapf_tex_decode_tile — virtual texturing: the
 * surfaces of mip m are cut into tiles (64 × 64 pixels, or 16 × 16
 * blocks), tx across, ty down, per slice.  decode_tile writes one tile
 * into `out`, rows `stride` bytes apart, and reports its pixel rectangle.
 */
yapf_texture_t *yapf_tex_open(const void *buffer, size_t avail, int flags, int *err);
uint64_t yapf_tex_stream_bytes(const yapf_texture_t *tex, uint32_t first_mip);
int yapf_tex_decode_surface(yapf_texture_t *tex, const void *buffer, size_t avail,
                            uint32_t surface, int threads);
void yapf_tex_tile_grid(const yapf_texture_t *tex, uint32_t mip,
                        uint32_t *tiles_x, uint32_t *tiles_y);
int yapf_tex_decode_tile(const yapf_texture_t *tex, const void *buffer, size_t avail,
                         uint32_t surface, uint32_t tx, uint32_t ty, uint32_t slice,
                         void *out, size_t stride,
                         uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h);

/*
 * yapf_tex_build_mips — replace the mips of a library-owned pixel texture
 * with a chain made from mip 0: `levels` levels (0 = down to 1 × 1).
 * 2 × 2 (× 2 for volumes) box filter; sRGB channels are averaged in
 * linear light.  alpha_ref > 0 rescales alpha on every mip so the share
 * of texels above alpha_ref stays that of mip 0 (fences and foliage do
 * not thin out); it sets YAPF_FLAG_COVERAGE.  Records the filter used.
 */
int yapf_tex_build_mips(yapf_texture_t *tex, uint32_t levels, uint8_t alpha_ref);

/* ── GPU blocks ──────────────────────────────────────────────────────── */

/* Block footprint and size of a YAPF_GPU_* block format; 0 if not one. */
int yapf_block_info(uint8_t format, uint32_t *block_w, uint32_t *block_h,
                    uint32_t *block_bytes);

/*
 * yapf_tex_compress — a new library-owned texture holding `src` (pixel
 * payload) as GPU blocks.  Built-in encoders: BC1, BC3, BC4, BC5, BC7 and
 * BC6H (unsigned).  LDR formats take 8-bit pixels; BC6H takes 16-bit,
 * half or float pixels.  For other formats bring blocks from your own
 * encoder: a payload = BLOCKS texture with data[] filled.
 */
int yapf_tex_compress(const yapf_texture_t *src, uint8_t format,
                      yapf_texture_t **out);

/*
 * yapf_blocks_decode — decode a w × h image of BC1–BC7 blocks (previews,
 * tools).  LDR formats write RGBA8 (BC4 / BC5: R / RG, then 0, 255;
 * snorm values shifted by +128); BC6H writes RGB half floats.  Returns
 * YAPF_ERR_UNSUPPORTED for ETC2 / EAC / ASTC.
 */
int yapf_blocks_decode(uint8_t format, const void *blocks, uint32_t w, uint32_t h,
                       void *out);

/*
 * yapf_tex_to_rgba8 — one slice of a decoded surface as 8-bit RGBA for
 * display (thumbnails, previews): gray is expanded, 16-bit is rounded to
 * 8 bits, float and half are clamped to 0..1 and sRGB encoded (they are
 * linear), GPU blocks are decoded.  out holds w × h × 4 bytes of that
 * mip.  Returns YAPF_ERR_UNSUPPORTED for ETC2 / EAC / ASTC blocks.
 */
int yapf_tex_to_rgba8(const yapf_texture_t *tex, uint32_t surface, uint32_t slice,
                      uint8_t *out);

/* yapf-sum, the checksum used by version 2 files. */
uint32_t yapf_checksum(const void *data, size_t size);

/* Half float conversion helpers. */
uint16_t yapf_float_to_half(float f);
float    yapf_half_to_float(uint16_t h);

#ifdef __cplusplus
}
#endif

#endif /* YAPF_H */
