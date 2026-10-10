/*
 * yapf_cli.c  —  the `yapf` command-line tool.
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
 *  yapf <input> <output> [options]     convert; formats chosen by extension
 *  yapf pack <cube|array|volume|anim> <inputs…> <out.yapf> [options]
 *  yapf info <file.yapf>               print header, tables and notes
 *  yapf verify <file.yapf>             check every checksum and decode
 *  yapf bench <file.yapf>              time decoding
 *  yapf thumbnail <in.yapf> <out.png> [size]   small preview (default 256)
 *  yapf compare <image.png> [more…]    size and decode speed: original vs YAPF
 *
 *  Reads PNG (8 / 16-bit), JPEG, BMP, TGA, GIF (first frame), PSD, PNM,
 *  HDR (float) and YAPF.  Writes YAPF, PNG, BMP, TGA, JPEG and HDR.
 *  Plain 8-bit 2D images are written as YAPF version 1; anything that
 *  needs version 2 (see `yapf` without arguments) is written as version 2.
 *
 *  Build:  cc -O2 -std=c99 -Ithird_party tools/yapf_cli.c yapf.c -o yapf -lm -pthread
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "../yapf.h"

/* Third-party, public domain; keep their warnings out of our build. */
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wunused-function"
#  pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#  pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include "../third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic pop
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

static int ends_with(const char *s, const char *ext) {
    size_t n = strlen(s), m = strlen(ext);
    if (n < m) return 0;
    for (size_t i = 0; i < m; i++)
        if (tolower((unsigned char)s[n - m + i]) != ext[i]) return 0;
    return 1;
}

static int is_yapf(const char *path) { return ends_with(path, ".yapf"); }

static double now_s(void) {
#if defined(_WIN32)
    return (double)clock() / CLOCKS_PER_SEC;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}


/* ── Options ─────────────────────────────────────────────────────────── */

typedef struct {
    int         mips, srgb, premult, v2, entropy, eight_bit, float32;
    int         coverage;            /* alpha_ref, 0 = off               */
    int         format;              /* block format, -1 = none          */
    int         normal;              /* YAPF_NORMAL_*                    */
    int         linear_alpha;
    int         primaries, transfer;
    int         have_map;
    uint8_t     map[4];
    double      frame_ms;
    int         loops;
    /* export selection */
    int         mip, layer, face, frame, slice;
    const char *author, *license, *source;
    const char *notes[16];
    int         nnotes;
} opts_t;

static const struct { const char *name; uint8_t v; } ch_names[] = {
    { "r", YAPF_CH_RED }, { "g", YAPF_CH_GREEN }, { "b", YAPF_CH_BLUE }, { "a", YAPF_CH_ALPHA },
    { "luma", YAPF_CH_LUMA }, { "nx", YAPF_CH_NORMAL_X }, { "ny", YAPF_CH_NORMAL_Y },
    { "nz", YAPF_CH_NORMAL_Z }, { "rough", YAPF_CH_ROUGHNESS }, { "gloss", YAPF_CH_GLOSS },
    { "metal", YAPF_CH_METALNESS }, { "ao", YAPF_CH_OCCLUSION }, { "height", YAPF_CH_HEIGHT },
    { "spec", YAPF_CH_SPECULAR }, { "emissive", YAPF_CH_EMISSIVE }, { "opacity", YAPF_CH_OPACITY },
    { "mask", YAPF_CH_MASK }, { "cavity", YAPF_CH_CAVITY }, { "thickness", YAPF_CH_THICKNESS },
    { "id", YAPF_CH_ID }, { "-", YAPF_CH_UNUSED }, { "default", YAPF_CH_DEFAULT }
};

static const char *ch_name(uint8_t v) {
    for (size_t i = 0; i < sizeof(ch_names) / sizeof(ch_names[0]); i++)
        if (ch_names[i].v == v) return ch_names[i].name;
    return "?";
}

static const struct { const char *name; uint8_t v; } fmt_names[] = {
    { "bc1", YAPF_GPU_BC1 }, { "bc3", YAPF_GPU_BC3 }, { "bc4", YAPF_GPU_BC4 },
    { "bc5", YAPF_GPU_BC5 }, { "bc6h", YAPF_GPU_BC6H }, { "bc7", YAPF_GPU_BC7 }
};

static const char *gpu_name(int g) {
    static const char *px[] = { "RGBA8", "RGB8", "RG8", "R8", "SRGB8_A8", "SRGB8",
        "RGBA16", "RGB16", "RG16", "R16", "RGBA16F", "RGB16F", "RG16F", "R16F",
        "RGBA32F", "RGB32F", "RG32F", "R32F" };
    static const char *bc[] = { "BC1", "BC2", "BC3", "BC4", "BC4_SNORM", "BC5", "BC5_SNORM",
        "BC6H", "BC6H_SF", "BC7" };
    static const char *etc[] = { "ETC2_RGB8", "ETC2_RGB8A1", "ETC2_RGBA8", "EAC_R11",
        "EAC_R11_SNORM", "EAC_RG11", "EAC_RG11_SNORM" };
    static const char *astc[] = { "ASTC_4x4", "ASTC_5x4", "ASTC_5x5", "ASTC_6x5", "ASTC_6x6",
        "ASTC_8x5", "ASTC_8x6", "ASTC_8x8", "ASTC_10x5", "ASTC_10x6", "ASTC_10x8",
        "ASTC_10x10", "ASTC_12x10", "ASTC_12x12" };
    if (g >= 0 && g < 18) return px[g];
    if (g >= 0x40 && g < 0x4A) return bc[g - 0x40];
    if (g >= 0x50 && g < 0x57) return etc[g - 0x50];
    if (g >= 0x60 && g < 0x6E) return astc[g - 0x60];
    return "unknown";
}

static uint8_t gpu_hint(int ch, int srgb, int sample) {
    static const uint8_t u8[2][4] = { { YAPF_GPU_R8, YAPF_GPU_RG8, YAPF_GPU_RGB8, YAPF_GPU_RGBA8 },
                                      { YAPF_GPU_R8, YAPF_GPU_RG8, YAPF_GPU_SRGB8, YAPF_GPU_SRGB8_A8 } };
    static const uint8_t wide[3][4] = {
        { YAPF_GPU_R16, YAPF_GPU_RG16, YAPF_GPU_RGB16, YAPF_GPU_RGBA16 },
        { YAPF_GPU_R16F, YAPF_GPU_RG16F, YAPF_GPU_RGB16F, YAPF_GPU_RGBA16F },
        { YAPF_GPU_R32F, YAPF_GPU_RG32F, YAPF_GPU_RGB32F, YAPF_GPU_RGBA32F } };
    if (sample == YAPF_SAMPLE_U8) return u8[srgb ? 1 : 0][ch - 1];
    return wide[sample - 1][ch - 1];
}

static int parse_map(const char *s, uint8_t *map) {
    memset(map, 0, 4);
    for (int c = 0; c < 4 && *s; c++) {
        const char *e = strchr(s, ',');
        size_t      n = e ? (size_t)(e - s) : strlen(s);
        size_t      i;
        for (i = 0; i < sizeof(ch_names) / sizeof(ch_names[0]); i++)
            if (strlen(ch_names[i].name) == n && strncmp(ch_names[i].name, s, n) == 0) break;
        if (i == sizeof(ch_names) / sizeof(ch_names[0])) return 0;
        map[c] = ch_names[i].v;
        s += n + (e ? 1 : 0);
    }
    return 1;
}

/* ── Reading any input into a texture ────────────────────────────────── */

static yapf_texture_t *new_tex(uint32_t w, uint32_t h, int ch, int sample) {
    yapf_texture_t d;
    memset(&d, 0, sizeof(d));
    d.width = w; d.height = h; d.depth = 1; d.layers = 1; d.frames = 1; d.mip_levels = 1;
    d.channels = (uint8_t)ch; d.sample = (uint8_t)sample;
    return yapf_tex_create(&d);
}

static yapf_texture_t *read_input(const char *path, const opts_t *o, int *from_yapf) {
    *from_yapf = 0;
    if (is_yapf(path)) {
        int err = 0;
        yapf_texture_t *t = yapf_tex_load(path, 0, 0, &err);
        if (!t) fprintf(stderr, "yapf: cannot read %s (error %d)\n", path, err);
        *from_yapf = 1;
        return t;
    }
    int w, h, ch;
    yapf_texture_t *t = NULL;
    if (stbi_is_hdr(path)) {
        float *f = stbi_loadf(path, &w, &h, &ch, 0);
        if (!f) { fprintf(stderr, "yapf: cannot read %s: %s\n", path, stbi_failure_reason()); return NULL; }
        int sample = o->float32 ? YAPF_SAMPLE_F32 : YAPF_SAMPLE_F16;
        t = new_tex((uint32_t)w, (uint32_t)h, ch, sample);
        if (t) {
            size_t n = (size_t)w * h * ch;
            if (sample == YAPF_SAMPLE_F32) memcpy(t->data[0], f, n * 4);
            else for (size_t i = 0; i < n; i++) ((uint16_t *)t->data[0])[i] = yapf_float_to_half(f[i]);
            t->transfer = YAPF_TRANSFER_LINEAR;
        }
        stbi_image_free(f);
    } else if (stbi_is_16_bit(path) && !o->eight_bit) {
        stbi_us *p = stbi_load_16(path, &w, &h, &ch, 0);
        if (!p) { fprintf(stderr, "yapf: cannot read %s: %s\n", path, stbi_failure_reason()); return NULL; }
        t = new_tex((uint32_t)w, (uint32_t)h, ch, YAPF_SAMPLE_U16);
        if (t) memcpy(t->data[0], p, (size_t)w * h * ch * 2);
        stbi_image_free(p);
    } else {
        stbi_uc *p = stbi_load(path, &w, &h, &ch, 0);
        if (!p) { fprintf(stderr, "yapf: cannot read %s: %s\n", path, stbi_failure_reason()); return NULL; }
        t = new_tex((uint32_t)w, (uint32_t)h, ch, YAPF_SAMPLE_U8);
        if (t) memcpy(t->data[0], p, (size_t)w * h * ch);
        stbi_image_free(p);
    }
    if (!t) fprintf(stderr, "yapf: %s: unsupported size or out of memory\n", path);
    return t;
}

/* Several same-sized inputs into one cube / array / volume / animation. */
static yapf_texture_t *assemble(const char *kind, int n, char **paths, const opts_t *o) {
    yapf_texture_t *first = NULL, *t = NULL;
    for (int i = 0; i < n; i++) {
        int y;
        yapf_texture_t *in = read_input(paths[i], o, &y);
        if (!in) { yapf_tex_free(t); yapf_tex_free(first); return NULL; }
        if (!t) {
            yapf_texture_t d = *in;
            d.mip_levels = 1;
            if (strcmp(kind, "cube") == 0) {
                if (n % 6) { fprintf(stderr, "yapf: a cube needs 6 faces per layer (+X -X +Y -Y +Z -Z)\n"); yapf_tex_free(in); return NULL; }
                d.shape = YAPF_SHAPE_CUBE; d.layers = (uint16_t)(n / 6);
            } else if (strcmp(kind, "array") == 0) {
                d.layers = (uint16_t)n;
            } else if (strcmp(kind, "volume") == 0) {
                d.shape = YAPF_SHAPE_VOLUME; d.depth = (uint32_t)n;
            } else if (strcmp(kind, "anim") == 0) {
                d.frames = (uint16_t)n;
            } else {
                fprintf(stderr, "yapf: pack kind must be cube, array, volume or anim\n");
                yapf_tex_free(in);
                return NULL;
            }
            t = yapf_tex_create(&d);
            if (!t) { fprintf(stderr, "yapf: invalid %s (cube faces must be square)\n", kind); yapf_tex_free(in); return NULL; }
            first = in;
        } else if (in->width != t->width || in->height != t->height ||
                   in->channels != t->channels || in->sample != t->sample) {
            fprintf(stderr, "yapf: %s does not match the size and channels of %s\n", paths[i], paths[0]);
            yapf_tex_free(in); yapf_tex_free(t); yapf_tex_free(first);
            return NULL;
        }
        size_t bytes = yapf_tex_surface_bytes(in, 0);
        if (t->shape == YAPF_SHAPE_VOLUME) memcpy((uint8_t *)t->data[0] + bytes * i, in->data[0], bytes);
        else if (t->frames > 1) memcpy(t->data[yapf_tex_surface(t, (uint32_t)i, 0, 0, 0)], in->data[0], bytes);
        else if (t->shape == YAPF_SHAPE_CUBE) memcpy(t->data[yapf_tex_surface(t, 0, (uint32_t)i / 6, (uint32_t)i % 6, 0)], in->data[0], bytes);
        else memcpy(t->data[yapf_tex_surface(t, 0, (uint32_t)i, 0, 0)], in->data[0], bytes);
        if (in != first) yapf_tex_free(in);
    }
    if (t && t->frames > 1)
        for (int f = 0; f < t->frames; f++) t->frame_us[f] = (uint32_t)(o->frame_ms * 1000.0 + 0.5);
    yapf_tex_free(first);
    return t;
}

/* ── Writing .yapf ───────────────────────────────────────────────────── */

static int needs_v2(const yapf_texture_t *t, const opts_t *o) {
    return o->v2 || o->entropy || o->format >= 0 || o->coverage || o->normal || o->have_map ||
           o->linear_alpha || o->primaries || o->transfer || o->author || o->license ||
           o->source || o->nnotes || t->sample != YAPF_SAMPLE_U8 || t->shape != YAPF_SHAPE_2D ||
           t->layers > 1 || t->frames > 1 || t->payload != YAPF_PAYLOAD_PIXELS || t->note_count;
}

static int write_yapf(yapf_texture_t *t, const char *out, const opts_t *o, int from_yapf) {
    if (!from_yapf) {
        t->flags = (uint8_t)((o->srgb && t->sample == YAPF_SAMPLE_U8 && !o->normal ? YAPF_FLAG_SRGB : 0) |
                             (o->premult ? YAPF_FLAG_PREMULT_ALPHA : 0));
        t->format = gpu_hint(t->channels, t->flags & YAPF_FLAG_SRGB, t->sample);
    }
    if (o->normal) {
        t->normal = (uint8_t)o->normal;
        t->channel_map[0] = YAPF_CH_NORMAL_X;
        t->channel_map[1] = YAPF_CH_NORMAL_Y;
        if (t->channels >= 3) t->channel_map[2] = YAPF_CH_NORMAL_Z;
        else t->normal |= YAPF_NORMAL_XY;
        t->flags &= (uint8_t)~YAPF_FLAG_SRGB;
    }
    if (o->have_map) memcpy(t->channel_map, o->map, 4);
    if (o->linear_alpha && (t->channels == 2 || t->channels == 4))
        t->linear_mask |= (uint8_t)(1u << (t->channels - 1));
    if (o->primaries) t->primaries = (uint8_t)o->primaries;
    if (o->transfer) t->transfer = (uint8_t)o->transfer;
    if (o->frame_ms > 0 && t->frames > 1)
        for (int f = 0; f < t->frames; f++) t->frame_us[f] = (uint32_t)(o->frame_ms * 1000.0 + 0.5);
    if (o->loops >= 0) t->loop_count = (uint16_t)o->loops;

    if (o->mips && t->payload == YAPF_PAYLOAD_PIXELS) {
        int rc = yapf_tex_build_mips(t, 0, (uint8_t)o->coverage);
        if (rc != YAPF_OK) {
            fprintf(stderr, "yapf: cannot build mips%s\n", o->coverage ? " (--coverage needs an alpha channel)" : "");
            return 1;
        }
    }
    int v2 = needs_v2(t, o);
    if (o->format >= 0) {
        yapf_texture_t *c = NULL;
        int rc = yapf_tex_compress(t, (uint8_t)o->format, &c);
        if (rc != YAPF_OK) {
            fprintf(stderr, "yapf: cannot compress to %s: %s\n", gpu_name(o->format),
                    o->format == YAPF_GPU_BC6H ? "BC6H needs a 16-bit or HDR (float) image"
                                               : "LDR block formats need an 8-bit image");
            return 1;
        }
        opts_t rest = *o;                 /* the blocks are made: write them */
        rest.format = -1;
        rest.mips = 0;
        rest.v2 = 1;
        int r2 = write_yapf(c, out, &rest, 1);
        yapf_tex_free(c);
        return r2;
    }
    if (!v2) {
        /* 8-bit 2D: version 1, readable by every YAPF decoder */
        yapf_image_t img;
        memset(&img, 0, sizeof(img));
        img.width = t->width; img.height = t->height; img.channels = t->channels;
        img.gpu_format = t->format; img.flags = t->flags; img.mip_levels = t->mip_levels;
        img.mips = (uint8_t **)t->data;
        img.pixels = (uint8_t *)t->data[0];
        if (yapf_save(out, &img) != YAPF_OK) { fprintf(stderr, "yapf: cannot write %s\n", out); return 1; }
        return 0;
    }
    if (!yapf_tex_find_note(t, "TOOL")) yapf_tex_add_note(t, "TOOL", "yapf command-line tool", 22);
    if (o->author)  yapf_tex_add_note(t, "AUTH", o->author, (uint32_t)strlen(o->author));
    if (o->license) yapf_tex_add_note(t, "LICN", o->license, (uint32_t)strlen(o->license));
    if (o->source)  yapf_tex_add_note(t, "SRCE", o->source, (uint32_t)strlen(o->source));
    for (int i = 0; i < o->nnotes; i++) {
        const char *eq = strchr(o->notes[i], '=');
        if (!eq || eq - o->notes[i] != 4) { fprintf(stderr, "yapf: --note wants TAG=text with a 4-letter tag\n"); return 1; }
        yapf_tex_add_note(t, o->notes[i], eq + 1, (uint32_t)strlen(eq + 1));
    }
    if (yapf_tex_save(out, t, o->entropy ? YAPF_ENC_ENTROPY : 0) != YAPF_OK) {
        fprintf(stderr, "yapf: cannot write %s\n", out);
        return 1;
    }
    return 0;
}

/* ── Exporting a surface to a common format ──────────────────────────── */

/* One surface slice as 8-bit pixels (*hdr = 0) or float RGB(A) (*hdr = 1). */
static void *surface_view(const yapf_texture_t *t, uint32_t s, uint32_t slice,
                          uint32_t *w, uint32_t *h, int *ch, int *hdr)
{
    uint32_t m = s % t->mip_levels, d;
    yapf_tex_mip_size(t, m, w, h, &d);
    if (slice >= d) return NULL;
    size_t n = (size_t)*w * *h, slice_bytes = yapf_tex_surface_bytes(t, m) / d;
    const uint8_t *src = (const uint8_t *)t->data[s] + slice_bytes * slice;
    *hdr = 0;
    if (t->payload == YAPF_PAYLOAD_BLOCKS) {
        int is6 = t->format == YAPF_GPU_BC6H || t->format == YAPF_GPU_BC6H_SF;
        void *out = malloc(n * (is6 ? 6 : 4));
        if (!out) return NULL;
        if (yapf_blocks_decode(t->format, src, *w, *h, out) != YAPF_OK) { free(out); return NULL; }
        if (!is6) { *ch = 4; return out; }
        float *f = (float *)malloc(n * 3 * sizeof(float));
        for (size_t i = 0; f && i < n * 3; i++) f[i] = yapf_half_to_float(((uint16_t *)out)[i]);
        free(out);
        *ch = 3; *hdr = 1;
        return f;
    }
    *ch = t->channels;
    if (t->sample == YAPF_SAMPLE_U8) {
        uint8_t *o = (uint8_t *)malloc(n * *ch);
        if (o) memcpy(o, src, n * *ch);
        return o;
    }
    if (t->sample == YAPF_SAMPLE_U16) {
        uint8_t *o = (uint8_t *)malloc(n * *ch);
        for (size_t i = 0; o && i < n * *ch; i++) o[i] = (uint8_t)((((const uint16_t *)src)[i] + 128) / 257);
        return o;
    }
    float *f = (float *)malloc(n * *ch * sizeof(float));
    for (size_t i = 0; f && i < n * *ch; i++)
        f[i] = t->sample == YAPF_SAMPLE_F16 ? yapf_half_to_float(((const uint16_t *)src)[i])
                                            : ((const float *)src)[i];
    *hdr = 1;
    return f;
}

static uint8_t *to8(const float *f, size_t n) {
    uint8_t *o = (uint8_t *)malloc(n);
    for (size_t i = 0; o && i < n; i++) {
        float v = f[i] < 0 ? 0 : f[i] > 1 ? 1 : f[i];
        o[i] = (uint8_t)(v * 255.0f + 0.5f);
    }
    return o;
}

static int write_image(const char *path, const void *px, int w, int h, int ch, int hdr) {
    if (ends_with(path, ".hdr")) {
        if (!hdr) {
            float *f = (float *)malloc((size_t)w * h * ch * sizeof(float));
            for (size_t i = 0; f && i < (size_t)w * h * ch; i++) f[i] = ((const uint8_t *)px)[i] / 255.0f;
            int ok = f && stbi_write_hdr(path, w, h, ch, f);
            free(f);
            return ok;
        }
        return stbi_write_hdr(path, w, h, ch, (const float *)px);
    }
    uint8_t *tmp = NULL;
    if (hdr) {
        tmp = to8((const float *)px, (size_t)w * h * ch);
        if (!tmp) return 0;
        fprintf(stderr, "yapf: note: float pixels clamped to 0..1 for %s (use .hdr to keep them)\n", path);
        px = tmp;
    }
    int ok = 0;
    if (ends_with(path, ".png"))       ok = stbi_write_png(path, w, h, ch, px, w * ch);
    else if (ends_with(path, ".bmp"))  ok = stbi_write_bmp(path, w, h, ch, px);
    else if (ends_with(path, ".tga"))  ok = stbi_write_tga(path, w, h, ch, px);
    else if (ends_with(path, ".jpg") || ends_with(path, ".jpeg")) ok = stbi_write_jpg(path, w, h, ch, px, 95);
    else fprintf(stderr, "yapf: unsupported output format: %s (use .yapf .png .bmp .tga .jpg .hdr)\n", path);
    free(tmp);
    return ok;
}

static int export_image(const yapf_texture_t *t, const char *in, const char *out, const opts_t *o) {
    uint32_t faces = yapf_tex_faces(t);
    if ((uint32_t)o->mip >= t->mip_levels || (uint32_t)o->layer >= t->layers ||
        (uint32_t)o->face >= faces || (uint32_t)o->frame >= t->frames) {
        fprintf(stderr, "yapf: %s has %u mip(s), %u layer(s), %u face(s), %u frame(s)\n",
                in, t->mip_levels, t->layers, faces, t->frames);
        return 1;
    }
    uint32_t s = yapf_tex_surface(t, (uint32_t)o->frame, (uint32_t)o->layer, (uint32_t)o->face, (uint32_t)o->mip);
    uint32_t w, h;
    int      ch, hdr;
    void    *px = surface_view(t, s, (uint32_t)o->slice, &w, &h, &ch, &hdr);
    if (!px) {
        fprintf(stderr, "yapf: cannot export %s: %s\n", in,
                t->payload == YAPF_PAYLOAD_BLOCKS ? "no built-in decoder for this block format (ETC2 / ASTC)"
                                                  : "no such slice, or out of memory");
        return 1;
    }
    if (t->sample == YAPF_SAMPLE_U16 && !ends_with(out, ".hdr"))
        fprintf(stderr, "yapf: note: 16-bit samples written as 8-bit\n");
    int ok = write_image(out, px, (int)w, (int)h, ch, hdr);
    free(px);
    if (!ok) { fprintf(stderr, "yapf: cannot write %s\n", out); return 1; }
    return 0;
}

/* ── info / verify / bench / thumbnail ───────────────────────────────── */

static uint8_t *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    uint8_t *buf = sz >= 0 ? (uint8_t *)malloc((size_t)sz + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); buf = NULL; }
    fclose(f);
    *n = buf ? (size_t)sz : 0;
    return buf;
}

static const char *err_name(int e) {
    switch (e) {
    case YAPF_ERR_INVALID: return "invalid argument";
    case YAPF_ERR_IO: return "cannot read the file";
    case YAPF_ERR_OOM: return "out of memory";
    case YAPF_ERR_CORRUPT: return "corrupt or not a YAPF file";
    case YAPF_ERR_UNSUPPORTED: return "needs a feature this decoder does not have";
    case YAPF_ERR_CHECKSUM: return "checksum mismatch (damaged file)";
    case YAPF_ERR_TRUNCATED: return "file is truncated";
    default: return "error";
    }
}

static int cmd_info(const char *path) {
    size_t   n;
    uint8_t *buf = read_all(path, &n);
    int      err = 0;
    yapf_texture_t *t = buf ? yapf_tex_open(buf, n, 0, &err) : NULL;
    if (!t) {
        fprintf(stderr, "yapf: %s: %s\n", path, buf ? err_name(err) : "cannot open");
        free(buf);
        return 1;
    }
    static const char *shape[] = { "2D", "cube", "volume" };
    static const char *sample[] = { "8-bit", "16-bit", "half float", "float" };
    static const char *prim[] = { "unspecified", "BT.709 / sRGB", "Display P3", "BT.2020",
                                  "ACES AP0", "ACEScg", "Adobe RGB" };
    static const char *tf[] = { "unspecified", "sRGB", "linear", "gamma 2.2", "PQ", "HLG", "BT.1886" };
    static const char *mf[] = { "unknown", "box", "tent", "Kaiser", "Lanczos", "Mitchell", "authored" };
    uint32_t w, h, d;
    yapf_tex_mip_size(t, 0, &w, &h, &d);
    printf("%s\n", path);
    printf("  format version : %u\n", buf[4]);
    printf("  size           : %u x %u%s", w, h, t->shape == YAPF_SHAPE_VOLUME ? "" : "\n");
    if (t->shape == YAPF_SHAPE_VOLUME) printf(" x %u\n", d);
    printf("  shape          : %s, %u layer(s), %u frame(s)\n", shape[t->shape], t->layers, t->frames);
    printf("  mip levels     : %u\n", t->mip_levels);
    if (t->payload == YAPF_PAYLOAD_BLOCKS)
        printf("  payload        : GPU blocks, %s (%u channel(s))\n", gpu_name(t->format), t->channels);
    else
        printf("  payload        : lossless pixels, %u channel(s), %s, GPU hint %s\n",
               t->channels, sample[t->sample], gpu_name(t->format));
    printf("  flags          : %s%s%s\n", t->flags & YAPF_FLAG_SRGB ? "sRGB" : "linear",
           t->flags & YAPF_FLAG_PREMULT_ALPHA ? ", premultiplied alpha" : "",
           t->flags & YAPF_FLAG_COVERAGE ? ", coverage-preserving mips" : "");
    if (buf[4] == 2) {
        uint32_t needs = (uint32_t)buf[0x1C] | (uint32_t)buf[0x1D] << 8;
        printf("  needs          : %s%s%s%s%s%s\n", needs ? "" : "nothing beyond 8-bit 2D",
               needs & YAPF_NEED_WIDE ? "wide " : "", needs & YAPF_NEED_BLOCKS ? "blocks " : "",
               needs & YAPF_NEED_ENTROPY ? "entropy " : "", needs & YAPF_NEED_LAYOUT ? "layout " : "",
               needs & YAPF_NEED_FRAMES ? "frames" : "");
        printf("  channels       :");
        for (int c = 0; c < t->channels; c++)
            printf(" %s%s", ch_name(t->channel_map[c]), t->linear_mask & (1u << c) ? "(linear)" : "");
        printf("\n");
        if (t->normal)
            printf("  normal map     : %s, %s\n",
                   (t->normal & 3) == YAPF_NORMAL_GL ? "OpenGL (+Y)" : (t->normal & 3) == YAPF_NORMAL_DX ? "DirectX (-Y)" : "no convention",
                   t->normal & YAPF_NORMAL_XY ? "XY (Z reconstructed)" : t->normal & YAPF_NORMAL_OCT ? "octahedral" : "XYZ");
        printf("  colour         : primaries %s, transfer %s\n",
               t->primaries < 7 ? prim[t->primaries] : "?", t->transfer < 7 ? tf[t->transfer] : "?");
        if (t->mip_levels > 1)
            printf("  mip filter     : %s%s%s\n", (t->mip_filter & 0x7F) < 7 ? mf[t->mip_filter & 0x7F] : "?",
                   t->mip_filter & YAPF_MIP_LINEAR_LIGHT ? ", linear light" : "",
                   t->flags & YAPF_FLAG_COVERAGE ? "" : "");
        if (t->flags & YAPF_FLAG_COVERAGE) printf("  alpha ref      : %u\n", t->alpha_ref);
        if (t->frames > 1)
            printf("  animation      : %u frames, first %.1f ms, loops %s\n", t->frames,
                   t->frame_us ? t->frame_us[0] / 1000.0 : 0.0, t->loop_count ? "" : "forever");
        for (uint32_t i = 0; i < t->note_count; i++) {
            const yapf_note_t *nt = &t->notes[i];
            int text = 1;
            for (uint32_t k = 0; k < nt->size; k++) if (nt->data[k] < 9) text = 0;
            if (text && nt->size < 200) printf("  note %.4s      : %.*s\n", nt->tag, (int)nt->size, nt->data);
            else printf("  note %.4s      : %u bytes\n", nt->tag, nt->size);
        }
        printf("  stream prefix  :");
        for (uint32_t m = t->mip_levels; m-- > 0; ) {
            uint32_t mw;
            yapf_tex_mip_size(t, m, &mw, NULL, NULL);
            printf(" %u px: %llu B%s", mw, (unsigned long long)yapf_tex_stream_bytes(t, m), m ? "," : "\n");
        }
    }
    size_t raw = 0;
    for (uint32_t s = 0; s < yapf_tex_surface_count(t); s++) {
        uint32_t mw, mh, md;
        yapf_tex_mip_size(t, s % t->mip_levels, &mw, &mh, &md);
        raw += (size_t)mw * mh * md * t->channels * (t->payload ? 1 : yapf_sample_bytes(t->sample));
    }
    printf("  file size      : %zu bytes (%.1f%% of raw)\n", n, 100.0 * (double)n / (double)raw);
    yapf_tex_free(t);
    t = yapf_tex_load_memory(buf, n, 0, 0, &err);
    printf("  decodes        : %s\n", t ? "ok (checksums verified)" : err_name(err));
    yapf_tex_free(t);
    free(buf);
    return t ? 0 : 1;
}

static int cmd_verify(const char *path) {
    size_t   n;
    uint8_t *buf = read_all(path, &n);
    int      err = YAPF_ERR_IO;
    yapf_texture_t *t = buf ? yapf_tex_load_memory(buf, n, 0, 0, &err) : NULL;
    if (t) printf("%s: ok%s\n", path, buf[4] == 2 ? " (header, tables and every tile checksum match)"
                                                  : " (version 1: decodes; no checksums in v1)");
    else   printf("%s: FAILED — %s\n", path, err_name(err));
    yapf_tex_free(t);
    free(buf);
    return t ? 0 : 1;
}

static int cmd_bench(const char *path) {
    size_t   n;
    uint8_t *buf = read_all(path, &n);
    if (!buf) { fprintf(stderr, "yapf: cannot open %s\n", path); return 1; }
    int err;
    yapf_texture_t *t = yapf_tex_load_memory(buf, n, 0, 1, &err);
    if (!t) { fprintf(stderr, "yapf: %s: %s\n", path, err_name(err)); free(buf); return 1; }
    double raw = 0;
    for (uint32_t s = 0; s < yapf_tex_surface_count(t); s++) raw += (double)yapf_tex_surface_bytes(t, s % t->mip_levels);
    yapf_tex_free(t);
    for (int mode = 0; mode < 3; mode++) {
        int threads = mode == 1 ? 0 : 1, flags = mode == 2 ? YAPF_LOAD_TRUST : 0;
        if (mode == 2 && buf[4] != 2) break;
        /* Best of 15 rounds; each round repeats enough decodes to last
         * ~20 ms, so short images are measured warm and not timer-bound. */
        double one = now_s();
        yapf_tex_free(yapf_tex_load_memory(buf, n, flags, threads, NULL));
        one = now_s() - one;
        int    reps = (int)(0.02 / (one > 1e-6 ? one : 1e-6)) + 1;
        double best = 1e9;
        for (int r = 0; r < 15; r++) {
            double tt = now_s();
            for (int k = 0; k < reps; k++) yapf_tex_free(yapf_tex_load_memory(buf, n, flags, threads, NULL));
            tt = (now_s() - tt) / reps;
            if (tt < best) best = tt;
        }
        static const char *nm[] = { "1 thread", "all cores", "no checks" };
        printf("  %-12s %8.3f ms   %6.2f GB/s\n", nm[mode], best * 1e3, raw / best / 1e9);
    }
    free(buf);
    return 0;
}

/* Preview no larger than size×size: start from the smallest mip that is
 * still big enough, then average boxes of source pixels. */
static int cmd_thumbnail(const char *in, const char *out, int size) {
    int err;
    yapf_texture_t *t = yapf_tex_load(in, 0, 0, &err);
    if (!t) { fprintf(stderr, "yapf: cannot read %s: %s\n", in, err_name(err)); return 1; }
    if (size < 1) size = 256;
    int m = 0;
    while (m + 1 < t->mip_levels &&
           (int)(t->width >> (m + 1)) >= size && (int)(t->height >> (m + 1)) >= size)
        m++;
    uint32_t sw, sh;
    int      ch, hdr;
    uint8_t *src = (uint8_t *)surface_view(t, (uint32_t)m, 0, &sw, &sh, &ch, &hdr);
    yapf_tex_free(t);
    if (!src) { fprintf(stderr, "yapf: cannot preview %s\n", in); return 1; }
    if (hdr) {
        uint8_t *b = to8((const float *)src, (size_t)sw * sh * ch);
        free(src);
        if (!(src = b)) return 1;
    }
    double   k  = (double)size / (double)(sw > sh ? sw : sh);
    if (k > 1.0) k = 1.0;
    uint32_t dw = (uint32_t)(sw * k + 0.5), dh = (uint32_t)(sh * k + 0.5);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    uint8_t *dst = (uint8_t *)malloc((size_t)dw * dh * ch);
    if (!dst) { free(src); return 1; }
    for (uint32_t y = 0; y < dh; y++) {
        uint32_t y0 = (uint32_t)((uint64_t)y * sh / dh), y1 = (uint32_t)((uint64_t)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (uint32_t x = 0; x < dw; x++) {
            uint32_t x0 = (uint32_t)((uint64_t)x * sw / dw), x1 = (uint32_t)((uint64_t)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            for (int c = 0; c < ch; c++) {
                uint64_t sum = 0;
                for (uint32_t yy = y0; yy < y1; yy++)
                    for (uint32_t xx = x0; xx < x1; xx++)
                        sum += src[((size_t)yy * sw + xx) * ch + c];
                uint64_t nn = (uint64_t)(y1 - y0) * (x1 - x0);
                dst[((size_t)y * dw + x) * ch + c] = (uint8_t)((sum + nn / 2) / nn);
            }
        }
    }
    int ok = stbi_write_png(out, (int)dw, (int)dh, ch, dst, (int)dw * ch);
    free(dst);
    free(src);
    if (!ok) fprintf(stderr, "yapf: cannot write %s\n", out);
    return ok ? 0 : 1;
}

/* Best time over several rounds of a decode call, for fair small-image timing. */
#define YAPF_TIME_BEST(result, expr)                                        \
    do {                                                                    \
        double one_ = now_s(); { expr; } one_ = now_s() - one_;             \
        int reps_ = (int)(0.02 / (one_ > 1e-6 ? one_ : 1e-6)) + 1;          \
        double best_ = 1e9;                                                 \
        for (int r_ = 0; r_ < 9; r_++) {                                    \
            double t_ = now_s();                                            \
            for (int k_ = 0; k_ < reps_; k_++) { expr; }                    \
            t_ = (now_s() - t_) / reps_;                                    \
            if (t_ < best_) best_ = t_;                                     \
        }                                                                   \
        (result) = best_;                                                   \
    } while (0)

static const char *fmt_kb(double bytes, char *buf) {
    if (bytes >= 1024.0 * 1024.0) sprintf(buf, "%.2f MB", bytes / (1024.0 * 1024.0));
    else sprintf(buf, "%.1f KB", bytes / 1024.0);
    return buf;
}

static const char *ext_name(const char *path) {
    const char *dot = strrchr(path, '.');
    static char name[8];
    size_t i = 0;
    if (!dot) return "file";
    for (dot++; *dot && i < sizeof(name) - 1; dot++) name[i++] = (char)toupper((unsigned char)*dot);
    name[i] = 0;
    return name;
}

/* Original format vs YAPF: size, decode time (1 thread / all cores),
 * and a lossless check. */
static int cmd_compare(int n, char **paths) {
    int rc = 0;
    double tot_src = 0, tot_yapf = 0, tot_src_t = 0, tot_yapf_t = 0, tot_ent = 0;
    for (int i = 0; i < n; i++) {
        const char *path = paths[i];
        if (is_yapf(path)) {
            fprintf(stderr, "yapf: compare takes the original image (PNG, JPG, ...), not %s\n", path);
            rc = 1;
            continue;
        }
        FILE *f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "yapf: cannot open %s\n", path); rc = 1; continue; }
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        rewind(f);
        uint8_t *src = (uint8_t *)malloc((size_t)fsz);
        if (!src || fread(src, 1, (size_t)fsz, f) != (size_t)fsz) { fclose(f); free(src); rc = 1; continue; }
        fclose(f);

        int w, h, ch;
        uint8_t *px = stbi_load_from_memory(src, (int)fsz, &w, &h, &ch, 0);
        if (!px) { fprintf(stderr, "yapf: cannot read %s: %s\n", path, stbi_failure_reason()); free(src); rc = 1; continue; }

        yapf_image_t img;
        memset(&img, 0, sizeof(img));
        img.width = (uint32_t)w; img.height = (uint32_t)h; img.channels = (uint8_t)ch;
        img.mip_levels = 1; img.pixels = px; img.flags = YAPF_FLAG_SRGB;
        img.gpu_format = gpu_hint(ch, 1, YAPF_SAMPLE_U8);

        void  *enc = NULL;
        size_t esz = 0;
        double t_enc = now_s();
        int    erc = yapf_encode(&img, &enc, &esz);
        t_enc = now_s() - t_enc;
        if (erc != YAPF_OK) { fprintf(stderr, "yapf: cannot encode %s\n", path); stbi_image_free(px); free(src); rc = 1; continue; }

        yapf_image_t *chk = yapf_load_memory(enc, esz);
        int lossless = chk && memcmp(chk->pixels, px, (size_t)w * h * ch) == 0;
        yapf_free(chk);

        double t_src, t_one, t_all;
        int dw, dh, dc;
        YAPF_TIME_BEST(t_src, stbi_image_free(stbi_load_from_memory(src, (int)fsz, &dw, &dh, &dc, 0)));
        YAPF_TIME_BEST(t_one, yapf_free(yapf_load_memory(enc, esz)));
        YAPF_TIME_BEST(t_all, yapf_free(yapf_load_memory_mt(enc, esz, 0)));

        double raw = (double)w * h * ch;
        char a[32], b[32], c[32];
        static const char *chn[] = { "", "gray", "gray+alpha", "RGB", "RGBA" };
        printf("%s  (%d x %d, %s)\n", path, w, h, chn[ch]);
        printf("  %-5s %10s\n", "raw", fmt_kb(raw, c));
        printf("  %-5s %10s  %5.1f%% of raw   decode %8.3f ms\n", ext_name(path), fmt_kb((double)fsz, a),
               100.0 * fsz / raw, t_src * 1e3);
        printf("  %-5s %10s  %5.1f%% of raw   decode %8.3f ms   %.1fx faster, %.0f%% of the %s size\n",
               "YAPF", fmt_kb((double)esz, b), 100.0 * esz / raw, t_one * 1e3,
               t_src / t_one, 100.0 * esz / fsz, ext_name(path));
        /* version 2 with the optional Huffman planes */
        {
            yapf_texture_t v;
            void          *data[1] = { px };
            size_t         esz2 = 0;
            void          *enc2 = NULL;
            double         t_e;
            memset(&v, 0, sizeof(v));
            v.width = (uint32_t)w; v.height = (uint32_t)h; v.depth = 1; v.layers = 1; v.frames = 1;
            v.mip_levels = 1; v.channels = (uint8_t)ch; v.flags = YAPF_FLAG_SRGB; v.data = data;
            if (yapf_tex_encode(&v, YAPF_ENC_ENTROPY, &enc2, &esz2) == YAPF_OK) {
                yapf_texture_t *c2 = yapf_tex_load_memory(enc2, esz2, 0, 1, NULL);
                if (!c2 || memcmp(c2->data[0], px, (size_t)w * h * ch) != 0) lossless = 0;
                yapf_tex_free(c2);
                YAPF_TIME_BEST(t_e, yapf_tex_free(yapf_tex_load_memory(enc2, esz2, 0, 1, NULL)));
                printf("  %-5s %10s  %5.1f%% of raw   decode %8.3f ms   %.1fx faster, %.0f%% of the %s size\n",
                       "YAPF+E", fmt_kb((double)esz2, c), 100.0 * esz2 / raw, t_e * 1e3,
                       t_src / t_e, 100.0 * esz2 / fsz, ext_name(path));
                tot_ent += (double)esz2;
                yapf_free_buffer(enc2);
            }
        }
        printf("  YAPF on all cores: decode %.3f ms (%.1f GB/s), encode %.0f ms, %s\n\n",
               t_all * 1e3, raw / t_all / 1e9, t_enc * 1e3,
               lossless ? "lossless: yes" : "LOSSLESS CHECK FAILED");
        if (!lossless) rc = 1;
        tot_src += (double)fsz; tot_yapf += (double)esz; tot_src_t += t_src; tot_yapf_t += t_one;
        yapf_free_buffer(enc);
        stbi_image_free(px);
        free(src);
    }
    if (n > 1 && tot_yapf_t > 0) {
        char a[32], b[32];
        char e[32];
        printf("Total: originals %s, YAPF %s (%.0f%%), YAPF+E %s (%.0f%%); decoding %.1fx faster on one core\n",
               fmt_kb(tot_src, a), fmt_kb(tot_yapf, b), 100.0 * tot_yapf / tot_src,
               fmt_kb(tot_ent, e), 100.0 * tot_ent / tot_src, tot_src_t / tot_yapf_t);
    }
    return rc;
}

static void usage(void) {
    fprintf(stderr,
        "usage: yapf <input> <output> [options]   convert (formats by extension)\n"
        "       yapf pack <cube|array|volume|anim> <inputs...> <out.yapf> [options]\n"
        "       yapf info <file.yapf>              header, tables and notes\n"
        "       yapf verify <file.yapf>            check every checksum\n"
        "       yapf bench <file.yapf>             time decoding\n"
        "       yapf thumbnail <in.yapf> <out.png> [size]   preview, default 256 px\n"
        "       yapf compare <image.png> [more...]  size and speed vs the original format\n"
        "\n"
        "reads:  .yapf .png (8/16-bit) .jpg .bmp .tga .gif .psd .pnm .hdr\n"
        "writes: .yapf .png .jpg .bmp .tga .hdr\n"
        "\n"
        "options when writing .yapf:\n"
        "  --mips              store a full mip chain (sRGB averaged in linear light)\n"
        "  --coverage REF      mips keep alpha-test coverage at alpha REF (0-255)\n"
        "  --format F          GPU blocks: bc1 bc3 bc4 bc5 bc6h bc7\n"
        "  --entropy           Huffman-coded planes: smaller, slower to decode\n"
        "  --linear            pixels are linear, not sRGB\n"
        "  --premult           alpha is premultiplied\n"
        "  --linear-alpha      alpha is linear even in an sRGB image\n"
        "  --normal gl|dx      normal map (OpenGL +Y / DirectX -Y); implies linear\n"
        "  --map a,b,c,d       channel meanings: r g b a luma nx ny nz rough gloss metal\n"
        "                      ao height spec emissive opacity mask cavity thickness id -\n"
        "  --primaries P       bt709 p3 bt2020 aces acescg adobe\n"
        "  --transfer T        srgb linear gamma22 pq hlg bt1886\n"
        "  --8bit              read 16-bit images as 8-bit\n"
        "  --float32           keep .hdr input as 32-bit float (default half)\n"
        "  --frame-ms N        animation frame time (pack anim), --loops N\n"
        "  --author S  --license S  --source S  --note TAG=text\n"
        "  --v2                write version 2 even for a plain 8-bit image\n"
        "options when reading .yapf:\n"
        "  --mip N  --layer N  --face N  --frame N  --slice N   which image to export\n");
}

static int parse_opts(int argc, char **argv, int start, opts_t *o) {
    static const char *prim[] = { "", "bt709", "p3", "bt2020", "aces", "acescg", "adobe" };
    static const char *tf[] = { "", "srgb", "linear", "gamma22", "pq", "hlg", "bt1886" };
    memset(o, 0, sizeof(*o));
    o->srgb = 1;
    o->format = -1;
    o->loops = -1;
    for (int i = start; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(a, "--mips") == 0) o->mips = 1;
        else if (strcmp(a, "--linear") == 0) o->srgb = 0;
        else if (strcmp(a, "--premult") == 0) o->premult = 1;
        else if (strcmp(a, "--entropy") == 0) o->entropy = 1;
        else if (strcmp(a, "--v2") == 0) o->v2 = 1;
        else if (strcmp(a, "--8bit") == 0) o->eight_bit = 1;
        else if (strcmp(a, "--float32") == 0) o->float32 = 1;
        else if (strcmp(a, "--linear-alpha") == 0) o->linear_alpha = 1;
        else if (!v) return 0;
        else if (strcmp(a, "--coverage") == 0) { o->coverage = atoi(v); o->mips = 1; i++;
            if (o->coverage < 1 || o->coverage > 255) return 0; }
        else if (strcmp(a, "--format") == 0) {
            size_t k;
            for (k = 0; k < sizeof(fmt_names) / sizeof(fmt_names[0]); k++)
                if (strcmp(v, fmt_names[k].name) == 0) o->format = fmt_names[k].v;
            if (o->format < 0) return 0;
            i++;
        }
        else if (strcmp(a, "--normal") == 0) {
            o->normal = strcmp(v, "gl") == 0 ? YAPF_NORMAL_GL : strcmp(v, "dx") == 0 ? YAPF_NORMAL_DX : 0;
            if (!o->normal) return 0;
            i++;
        }
        else if (strcmp(a, "--map") == 0) { if (!parse_map(v, o->map)) return 0; o->have_map = 1; i++; }
        else if (strcmp(a, "--primaries") == 0 || strcmp(a, "--transfer") == 0) {
            const char **names = a[2] == 'p' ? prim : tf;
            int found = 0;
            for (int k = 1; k < 7; k++) if (strcmp(v, names[k]) == 0) found = k;
            if (!found) return 0;
            if (a[2] == 'p') o->primaries = found; else o->transfer = found;
            i++;
        }
        else if (strcmp(a, "--frame-ms") == 0) { o->frame_ms = atof(v); i++; }
        else if (strcmp(a, "--loops") == 0) { o->loops = atoi(v); i++; }
        else if (strcmp(a, "--author") == 0) { o->author = v; i++; }
        else if (strcmp(a, "--license") == 0) { o->license = v; i++; }
        else if (strcmp(a, "--source") == 0) { o->source = v; i++; }
        else if (strcmp(a, "--note") == 0) { if (o->nnotes == 16) return 0; o->notes[o->nnotes++] = v; i++; }
        else if (strcmp(a, "--mip") == 0) { o->mip = atoi(v); i++; }
        else if (strcmp(a, "--layer") == 0) { o->layer = atoi(v); i++; }
        else if (strcmp(a, "--face") == 0) { o->face = atoi(v); i++; }
        else if (strcmp(a, "--frame") == 0) { o->frame = atoi(v); i++; }
        else if (strcmp(a, "--slice") == 0) { o->slice = atoi(v); i++; }
        else return 0;
    }
    if (o->mip < 0 || o->layer < 0 || o->face < 0 || o->frame < 0 || o->slice < 0) return 0;
    return 1;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "info") == 0)   return cmd_info(argv[2]);
    if (argc == 3 && strcmp(argv[1], "verify") == 0) return cmd_verify(argv[2]);
    if (argc == 3 && strcmp(argv[1], "bench") == 0)  return cmd_bench(argv[2]);
    if (argc >= 3 && strcmp(argv[1], "compare") == 0) return cmd_compare(argc - 2, argv + 2);
    if ((argc == 4 || argc == 5) && strcmp(argv[1], "thumbnail") == 0)
        return cmd_thumbnail(argv[2], argv[3], argc == 5 ? atoi(argv[4]) : 256);

    opts_t o;
    if (argc >= 5 && strcmp(argv[1], "pack") == 0) {
        int first_opt = 3;
        while (first_opt < argc && strncmp(argv[first_opt], "--", 2) != 0) first_opt++;
        int n = first_opt - 3 - 1;                /* inputs, then the output */
        if (n < 1 || !parse_opts(argc, argv, first_opt, &o)) { usage(); return 2; }
        const char *out = argv[first_opt - 1];
        if (!is_yapf(out)) { fprintf(stderr, "yapf: pack writes a .yapf file\n"); return 2; }
        if (o.frame_ms <= 0) o.frame_ms = 100;
        yapf_texture_t *t = assemble(argv[2], n, argv + 3, &o);
        if (!t) return 1;
        int rc = write_yapf(t, out, &o, 0);
        yapf_tex_free(t);
        return rc;
    }
    if (argc < 3 || !parse_opts(argc, argv, 3, &o)) { usage(); return 2; }

    const char *in = argv[1], *out = argv[2];
    int from_yapf;
    yapf_texture_t *t = read_input(in, &o, &from_yapf);
    if (!t) return 1;
    int rc;
    if (is_yapf(out)) {
        rc = write_yapf(t, out, &o, from_yapf);
    } else {
        rc = export_image(t, in, out, &o);
    }
    yapf_tex_free(t);
    return rc;
}
