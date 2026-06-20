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

#include "yapf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Constants ───────────────────────────────────────────────────────── */

#define YAPF__MAGIC     "YAPF"
#define YAPF__VERSION   2
#define YAPF__HDR_LEN   20
#define YAPF__MIP_ENTRY  8
#define YAPF__MAX_TILE_PX  (YAPF_TILE_SIZE * YAPF_TILE_SIZE)  /* 1024 */

/* ── Tile pixel type ─────────────────────────────────────────────────── */
/*
 * After YCoCg-R, channels 1 (Co) and 2 (Cg) span [-255, 255] and need
 * 9 bits, so we use int16_t for all channels internally within a tile.
 * Grayscale and alpha channels stay in [0, 255].
 */
typedef struct { int16_t v[4]; } yapf__tpx;

/* ── I/O helpers ─────────────────────────────────────────────────────── */

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

/* ── Bit-writer ──────────────────────────────────────────────────────── */

typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    uint8_t  acc;   /* accumulator */
    int      bits;  /* bits filled in acc (0-7) */
} yapf__bw;

static int yapf__bw_init(yapf__bw *w, size_t cap) {
    w->buf  = (uint8_t *)malloc(cap ? cap : 64);
    if (!w->buf) return -1;
    w->cap  = cap ? cap : 64;
    w->len  = 0;
    w->acc  = 0;
    w->bits = 0;
    return 0;
}

static int yapf__bw_grow(yapf__bw *w, size_t need) {
    if (w->len + need <= w->cap) return 0;
    size_t nc = w->cap * 2 + need;
    uint8_t *nb = (uint8_t *)realloc(w->buf, nc);
    if (!nb) return -1;
    w->buf = nb;
    w->cap = nc;
    return 0;
}

/* Write n bits from v, MSB first; n in [1, 32]. */
static int yapf__bw_put(yapf__bw *w, uint32_t v, int n) {
    if (yapf__bw_grow(w, (size_t)(n + 7) / 8 + 2) != 0) return -1;
    for (int i = n - 1; i >= 0; i--) {
        w->acc = (uint8_t)((w->acc << 1) | ((v >> i) & 1u));
        if (++w->bits == 8) {
            w->buf[w->len++] = w->acc;
            w->acc  = 0;
            w->bits = 0;
        }
    }
    return 0;
}

/* Flush partial byte (zero-padded) and reset to byte boundary. */
static void yapf__bw_flush(yapf__bw *w) {
    if (w->bits > 0) {
        w->buf[w->len++] = (uint8_t)(w->acc << (8 - w->bits));
        w->acc  = 0;
        w->bits = 0;
    }
}

/* ── Bit-reader ──────────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
    uint8_t        acc;
    int            bits;
} yapf__br;

static void yapf__br_init(yapf__br *r, const uint8_t *buf, size_t len) {
    r->buf  = buf;
    r->len  = len;
    r->pos  = 0;
    r->acc  = 0;
    r->bits = 0;
}

/* Read n bits MSB-first into *out. Returns 0 on success, -1 on underflow. */
static int yapf__br_get(yapf__br *r, int n, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        if (r->bits == 0) {
            if (r->pos >= r->len) return -1;
            r->acc  = r->buf[r->pos++];
            r->bits = 8;
        }
        v = (v << 1) | ((r->acc >> 7) & 1u);
        r->acc  = (uint8_t)(r->acc << 1);
        r->bits--;
    }
    *out = v;
    return 0;
}

/* Discard remaining bits in current byte (tile boundary alignment). */
static void yapf__br_align(yapf__br *r) { r->acc = 0; r->bits = 0; }

/* ── YCoCg-R transform ───────────────────────────────────────────────── */
/*
 * Lossless, integer, fully reversible.
 * Y ∈ [0,255],  Co ∈ [-255,255],  Cg ∈ [-255,255].
 * Alpha passes through unchanged.
 */
static void yapf__forward(yapf__tpx *dst, const uint8_t *src, int ch) {
    if (ch >= 3) {
        int r = src[0], g = src[1], b = src[2];
        int co = r - b;
        int t  = b + (co >> 1);
        int cg = g - t;
        int y  = t + (cg >> 1);
        dst->v[0] = (int16_t)y;
        dst->v[1] = (int16_t)co;
        dst->v[2] = (int16_t)cg;
        if (ch == 4) dst->v[3] = (int16_t)src[3];
    } else {
        for (int c = 0; c < ch; c++) dst->v[c] = (int16_t)src[c];
    }
}

static void yapf__inverse(uint8_t *dst, const yapf__tpx *src, int ch) {
    if (ch >= 3) {
        int y  = src->v[0];
        int co = src->v[1];
        int cg = src->v[2];
        int t  = y  - (cg >> 1);
        int g  = cg + t;
        int b  = t  - (co >> 1);
        int r  = b  + co;
        dst[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
        dst[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
        dst[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
        if (ch == 4) dst[3] = (uint8_t)src->v[3];
    } else {
        for (int c = 0; c < ch; c++) dst[c] = (uint8_t)src->v[c];
    }
}

/* ── Predictor ───────────────────────────────────────────────────────── */
/* left + above - diag, unclipped (works for signed int16 channels). */
static int32_t yapf__predict(int16_t L, int16_t A, int16_t D) {
    return (int32_t)L + (int32_t)A - (int32_t)D;
}

/* ── Signed bit width ────────────────────────────────────────────────── */
/* Minimum two's-complement bits to represent v.  Returns 0 if v == 0. */
static int yapf__sbits(int v) {
    if (v == 0) return 0;
    if (v < 0) v = ~v;   /* largest positive needing same width */
    int b = 1;           /* sign bit */
    while (v > 0) { v >>= 1; b++; }
    return b;
}

/* First-pixel bit widths: Co (ch1) and Cg (ch2) need 9 bits each because
 * YCoCg-R maps them to [-255, 255]; all other channels stay at 8 bits. */
static int yapf__first_bits(int c, int ch) {
    return (ch >= 3 && (c == 1 || c == 2)) ? 9 : 8;
}

/* ── Tile encode ─────────────────────────────────────────────────────── */

static int yapf__encode_tile(
    yapf__bw *w,
    const uint8_t *pixels, uint32_t img_w,
    uint32_t tx, uint32_t ty, uint32_t tw, uint32_t th,
    int ch)
{
    yapf__tpx raw[YAPF__MAX_TILE_PX];
    int       max_abs[4] = {0, 0, 0, 0};
    int       bd[4];
    size_t    total = (size_t)tw * th;

    /* Gather + transform */
    for (uint32_t y = 0; y < th; y++) {
        for (uint32_t x = 0; x < tw; x++) {
            const uint8_t *src =
                pixels + ((size_t)(ty + y) * img_w + (tx + x)) * (size_t)ch;
            yapf__forward(&raw[y * tw + x], src, ch);
        }
    }

    /* Compute residuals and track max |residual| per channel.
     * Pixel (0,0) is stored raw; all others use 2-D prediction. */
    for (size_t i = 1; i < total; i++) {
        uint32_t x = (uint32_t)(i % tw);
        uint32_t y = (uint32_t)(i / tw);

        for (int c = 0; c < ch; c++) {
            int16_t L = (x > 0)           ? raw[y * tw + (x-1)].v[c]     : 0;
            int16_t A = (y > 0)           ? raw[(y-1)*tw + x].v[c]        : 0;
            int16_t D = (x > 0 && y > 0) ? raw[(y-1)*tw + (x-1)].v[c]   : 0;
            int32_t pred = yapf__predict(L, A, D);
            int     res  = (int)raw[i].v[c] - (int)pred;
            int     ab   = res < 0 ? -res : res;
            if (ab > max_abs[c]) max_abs[c] = ab;
        }
    }

    /* Write per-channel residual bit depths */
    for (int c = 0; c < ch; c++) {
        bd[c] = yapf__sbits(max_abs[c]);
        if (yapf__bw_put(w, (uint32_t)bd[c], 8) != 0) return -1;
    }

    /* Write first pixel with channel-appropriate width */
    for (int c = 0; c < ch; c++) {
        int nbits = yapf__first_bits(c, ch);
        uint32_t bits = (uint32_t)((int32_t)raw[0].v[c] & ((1 << nbits) - 1));
        if (yapf__bw_put(w, bits, nbits) != 0) return -1;
    }

    /* Write packed residuals for pixels 1 .. total-1 */
    for (size_t i = 1; i < total; i++) {
        uint32_t x = (uint32_t)(i % tw);
        uint32_t y = (uint32_t)(i / tw);

        for (int c = 0; c < ch; c++) {
            if (bd[c] == 0) continue;
            int16_t L = (x > 0)           ? raw[y * tw + (x-1)].v[c]   : 0;
            int16_t A = (y > 0)           ? raw[(y-1)*tw + x].v[c]      : 0;
            int16_t D = (x > 0 && y > 0) ? raw[(y-1)*tw + (x-1)].v[c] : 0;
            int32_t pred = yapf__predict(L, A, D);
            int     res  = (int)raw[i].v[c] - (int)pred;
            uint32_t bits = (uint32_t)(res & ((1 << bd[c]) - 1));
            if (yapf__bw_put(w, bits, bd[c]) != 0) return -1;
        }
    }

    yapf__bw_flush(w);
    return 0;
}

/* ── Tile decode ─────────────────────────────────────────────────────── */

static int yapf__decode_tile(
    yapf__br *r,
    uint8_t *pixels, uint32_t img_w,
    uint32_t tx, uint32_t ty, uint32_t tw, uint32_t th,
    int ch)
{
    yapf__tpx raw[YAPF__MAX_TILE_PX];
    int       bd[4];
    uint32_t  bits;
    size_t    total = (size_t)tw * th;

    /* Read residual bit depths */
    for (int c = 0; c < ch; c++) {
        if (yapf__br_get(r, 8, &bits) != 0) return -1;
        bd[c] = (int)bits;
    }

    /* Read first pixel.
     * 8-bit channels (Y, A, grayscale) are unsigned [0,255] — no sign-extend.
     * 9-bit channels (Co, Cg) are signed [-255,255] — two's complement. */
    for (int c = 0; c < ch; c++) {
        int nbits = yapf__first_bits(c, ch);
        if (yapf__br_get(r, nbits, &bits) != 0) return -1;
        int v;
        if (nbits == 9) {
            v = (int)(bits & 0xFFu);
            if (bits & 0x100u) v -= 256;
        } else {
            v = (int)bits;  /* unsigned: 0–255 */
        }
        raw[0].v[c] = (int16_t)v;
    }

    /* Decode pixels 1 .. total-1 */
    for (size_t i = 1; i < total; i++) {
        uint32_t x = (uint32_t)(i % tw);
        uint32_t y = (uint32_t)(i / tw);

        for (int c = 0; c < ch; c++) {
            int16_t L = (x > 0)           ? raw[y * tw + (x-1)].v[c]   : 0;
            int16_t A = (y > 0)           ? raw[(y-1)*tw + x].v[c]      : 0;
            int16_t D = (x > 0 && y > 0) ? raw[(y-1)*tw + (x-1)].v[c] : 0;
            int32_t pred = yapf__predict(L, A, D);
            int     res  = 0;

            if (bd[c] > 0) {
                if (yapf__br_get(r, bd[c], &bits) != 0) return -1;
                int sb = 1 << (bd[c] - 1);
                res = (int)(bits & (uint32_t)(sb - 1));
                if (bits & (uint32_t)sb) res -= sb;
            }
            raw[i].v[c] = (int16_t)((int)pred + res);
        }
    }

    yapf__br_align(r);

    /* Inverse transform and write to output buffer */
    for (uint32_t y = 0; y < th; y++) {
        for (uint32_t x = 0; x < tw; x++) {
            uint8_t *dst =
                pixels + ((size_t)(ty + y) * img_w + (tx + x)) * (size_t)ch;
            yapf__inverse(dst, &raw[y * tw + x], ch);
        }
    }
    return 0;
}

/* ── Level encode / decode ───────────────────────────────────────────── */

static int yapf__encode_level(
    yapf__bw *w, const uint8_t *pixels, uint32_t mw, uint32_t mh, int ch)
{
    uint32_t tx_count = (mw + YAPF_TILE_SIZE - 1) / YAPF_TILE_SIZE;
    uint32_t ty_count = (mh + YAPF_TILE_SIZE - 1) / YAPF_TILE_SIZE;

    for (uint32_t ty = 0; ty < ty_count; ty++) {
        for (uint32_t tx = 0; tx < tx_count; tx++) {
            uint32_t tw = (tx + 1) * YAPF_TILE_SIZE <= mw
                        ? YAPF_TILE_SIZE : mw - tx * YAPF_TILE_SIZE;
            uint32_t th = (ty + 1) * YAPF_TILE_SIZE <= mh
                        ? YAPF_TILE_SIZE : mh - ty * YAPF_TILE_SIZE;
            if (yapf__encode_tile(w, pixels, mw,
                                  tx * YAPF_TILE_SIZE,
                                  ty * YAPF_TILE_SIZE,
                                  tw, th, ch) != 0)
                return -1;
        }
    }
    return 0;
}

static int yapf__decode_level(
    const uint8_t *data, size_t data_len,
    uint8_t *pixels, uint32_t mw, uint32_t mh, int ch)
{
    yapf__br r;
    yapf__br_init(&r, data, data_len);

    uint32_t tx_count = (mw + YAPF_TILE_SIZE - 1) / YAPF_TILE_SIZE;
    uint32_t ty_count = (mh + YAPF_TILE_SIZE - 1) / YAPF_TILE_SIZE;

    for (uint32_t ty = 0; ty < ty_count; ty++) {
        for (uint32_t tx = 0; tx < tx_count; tx++) {
            uint32_t tw = (tx + 1) * YAPF_TILE_SIZE <= mw
                        ? YAPF_TILE_SIZE : mw - tx * YAPF_TILE_SIZE;
            uint32_t th = (ty + 1) * YAPF_TILE_SIZE <= mh
                        ? YAPF_TILE_SIZE : mh - ty * YAPF_TILE_SIZE;
            if (yapf__decode_tile(&r, pixels, mw,
                                  tx * YAPF_TILE_SIZE,
                                  ty * YAPF_TILE_SIZE,
                                  tw, th, ch) != 0)
                return -1;
        }
    }
    return 0;
}

/* ========================================================================
 *  yapf_load
 * ======================================================================== */

yapf_image_t *yapf_load(const char *filename) {
    if (!filename) return NULL;

    FILE *f = fopen(filename, "rb");
    if (!f) return NULL;

    uint8_t hdr[YAPF__HDR_LEN];
    if (fread(hdr, 1, YAPF__HDR_LEN, f) != YAPF__HDR_LEN) goto fail_f;
    if (memcmp(hdr, YAPF__MAGIC, 4) != 0 || hdr[4] != YAPF__VERSION) goto fail_f;

    uint8_t  channels   = hdr[5];
    uint8_t  gpu_fmt    = hdr[6];
    uint8_t  mip_levels = hdr[7];
    uint8_t  flags      = hdr[8];
    uint32_t base_w     = yapf__r32le(hdr + 12);
    uint32_t base_h     = yapf__r32le(hdr + 16);

    if (channels < 1 || channels > 4) goto fail_f;
    if (mip_levels < 1 || mip_levels > YAPF_MAX_MIPS) goto fail_f;
    if (base_w == 0 || base_w > YAPF_MAX_DIM) goto fail_f;
    if (base_h == 0 || base_h > YAPF_MAX_DIM) goto fail_f;

    /* Mip index */
    size_t idx_bytes = (size_t)mip_levels * YAPF__MIP_ENTRY;
    uint8_t *mip_idx = (uint8_t *)malloc(idx_bytes);
    if (!mip_idx) goto fail_f;
    if (fread(mip_idx, 1, idx_bytes, f) != idx_bytes) { free(mip_idx); goto fail_f; }

    yapf_image_t *img = (yapf_image_t *)calloc(1, sizeof(yapf_image_t));
    if (!img) { free(mip_idx); goto fail_f; }

    img->width      = base_w;
    img->height     = base_h;
    img->channels   = channels;
    img->gpu_format = gpu_fmt;
    img->flags      = flags;
    img->mip_levels = mip_levels;

    img->mips = (uint8_t **)calloc(mip_levels, sizeof(uint8_t *));
    if (!img->mips) { free(mip_idx); goto fail_img; }

    for (uint8_t m = 0; m < mip_levels; m++) {
        uint32_t offset = yapf__r32le(mip_idx + m * YAPF__MIP_ENTRY);
        uint32_t size   = yapf__r32le(mip_idx + m * YAPF__MIP_ENTRY + 4);
        uint32_t mw     = base_w  >> m; if (mw < 1) mw = 1;
        uint32_t mh     = base_h  >> m; if (mh < 1) mh = 1;

        uint8_t *data = (uint8_t *)malloc(size);
        if (!data) { free(mip_idx); goto fail_mips; }

        if (fseek(f, (long)offset, SEEK_SET) != 0 ||
            fread(data, 1, size, f) != size) {
            free(data); free(mip_idx); goto fail_mips;
        }

        size_t px_bytes = (size_t)mw * mh * channels;
        img->mips[m] = (uint8_t *)malloc(px_bytes);
        if (!img->mips[m]) { free(data); free(mip_idx); goto fail_mips; }

        if (yapf__decode_level(data, size, img->mips[m], mw, mh, channels) != 0) {
            free(data); free(mip_idx); goto fail_mips;
        }
        free(data);
    }

    free(mip_idx);
    img->pixels = img->mips[0];
    fclose(f);
    return img;

fail_mips:
    for (uint8_t m = 0; m < mip_levels; m++) free(img->mips[m]);
    free(img->mips);
fail_img:
    free(img);
fail_f:
    fclose(f);
    return NULL;
}

/* ========================================================================
 *  yapf_save
 * ======================================================================== */

int yapf_save(const char *filename, const yapf_image_t *img) {
    if (!filename || !img || !img->pixels)        return YAPF_ERR_INVALID;
    if (img->width == 0 || img->width > YAPF_MAX_DIM)   return YAPF_ERR_INVALID;
    if (img->height == 0 || img->height > YAPF_MAX_DIM) return YAPF_ERR_INVALID;
    if (img->channels < 1 || img->channels > 4)   return YAPF_ERR_INVALID;
    if (img->mip_levels < 1 || img->mip_levels > YAPF_MAX_MIPS) return YAPF_ERR_INVALID;

    int     ch         = (int)img->channels;
    uint8_t mip_levels = img->mip_levels;

    /* Encode each mip level */
    yapf__bw bws[YAPF_MAX_MIPS];
    memset(bws, 0, sizeof(bws));

    for (uint8_t m = 0; m < mip_levels; m++) {
        uint32_t mw = img->width  >> m; if (mw < 1) mw = 1;
        uint32_t mh = img->height >> m; if (mh < 1) mh = 1;

        const uint8_t *src = (m == 0)
            ? img->pixels
            : (img->mips ? img->mips[m] : NULL);
        if (!src) {
            for (uint8_t j = 0; j < m; j++) free(bws[j].buf);
            return YAPF_ERR_INVALID;
        }

        size_t est = (size_t)mw * mh * (size_t)ch + 256;
        if (yapf__bw_init(&bws[m], est) != 0) {
            for (uint8_t j = 0; j < m; j++) free(bws[j].buf);
            return YAPF_ERR_OOM;
        }
        if (yapf__encode_level(&bws[m], src, mw, mh, ch) != 0) {
            for (uint8_t j = 0; j <= m; j++) free(bws[j].buf);
            return YAPF_ERR_OOM;
        }
    }

    /* Compute mip index offsets */
    uint32_t offsets[YAPF_MAX_MIPS];
    uint32_t cur = YAPF__HDR_LEN + (uint32_t)mip_levels * YAPF__MIP_ENTRY;
    for (uint8_t m = 0; m < mip_levels; m++) {
        offsets[m] = cur;
        cur += (uint32_t)bws[m].len;
    }

    FILE *f = fopen(filename, "wb");
    if (!f) {
        for (uint8_t m = 0; m < mip_levels; m++) free(bws[m].buf);
        return YAPF_ERR_IO;
    }

    /* Header */
    uint8_t hdr[YAPF__HDR_LEN];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, YAPF__MAGIC, 4);
    hdr[4]  = YAPF__VERSION;
    hdr[5]  = img->channels;
    hdr[6]  = img->gpu_format;
    hdr[7]  = mip_levels;
    hdr[8]  = img->flags;
    yapf__w32le(hdr + 12, img->width);
    yapf__w32le(hdr + 16, img->height);
    fwrite(hdr, 1, YAPF__HDR_LEN, f);

    /* Mip index */
    for (uint8_t m = 0; m < mip_levels; m++) {
        uint8_t entry[YAPF__MIP_ENTRY];
        yapf__w32le(entry + 0, offsets[m]);
        yapf__w32le(entry + 4, (uint32_t)bws[m].len);
        fwrite(entry, 1, YAPF__MIP_ENTRY, f);
    }

    /* Payload */
    int err = YAPF_OK;
    for (uint8_t m = 0; m < mip_levels; m++) {
        if (fwrite(bws[m].buf, 1, bws[m].len, f) != bws[m].len)
            err = YAPF_ERR_IO;
        free(bws[m].buf);
    }

    fclose(f);
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
