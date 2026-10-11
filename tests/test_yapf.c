/*
 * test_yapf.c — self-contained test suite for yapf.c
 *
 *   cc -O2 -std=c99 tests/test_yapf.c yapf.c -o test_yapf -pthread
 *   ./test_yapf [path/to/other/YAPF.YAPF]
 *
 * Round-trips many sizes, channel counts and image kinds, checks mip
 * chains, single- vs multi-threaded decode, the bundled sample file, and
 * that corrupted or truncated files are rejected without crashing.
 * Version 2: wide samples, shapes, GPU blocks, notes, checksums, needs,
 * streaming, tile decode, block encoders and mip generation.
 * Exit code 0 means every test passed.
 */
#include "../yapf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* FNV-1a of the decoded pixels of other/YAPF.YAPF (the YAPF logo). */
#define SAMPLE_HASH 0x8891c2b4u

static int tests, fails;

static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

#define CHECK(cond, ...) do { tests++; if (!(cond)) { fails++; \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Five kinds of content: noise, smooth gradient, hard-edged blocks,
 * noisy curves, and an image with an opaque alpha channel. */
static void fill(uint8_t *p, int w, int h, int ch, int kind) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int c = 0; c < ch; c++) {
                uint8_t *d = p + ((size_t)y * w + x) * ch + c;
                switch (kind) {
                case 0:  *d = (uint8_t)rnd(); break;
                case 1:  *d = (uint8_t)(x * 3 + y * 5 + c * 40); break;
                case 2:  *d = ((x / 7 + y / 5 + c) & 1) ? 200 : 30; break;
                case 3:  *d = (uint8_t)((x * x + y * y) / 50 + rnd() % 5); break;
                default: *d = (c == ch - 1) ? 255 : (uint8_t)(x ^ y); break;
                }
            }
}

static uint8_t *encode_mem(const yapf_image_t *img, size_t *n) {
    void *data = NULL;
    if (yapf_encode(img, &data, n) != YAPF_OK) return NULL;
    uint8_t *copy = (uint8_t *)malloc(*n);
    if (copy) memcpy(copy, data, *n);
    yapf_free_buffer(data);
    return copy;
}

/* yapf_save() writes exactly what yapf_encode() returns. */
static void save_matches_encode(void) {
    const char *tmp = "test_yapf_tmp.yapf";
    uint8_t px[37 * 23 * 3];
    fill(px, 37, 23, 3, 3);
    yapf_image_t img;
    memset(&img, 0, sizeof(img));
    img.width = 37; img.height = 23; img.channels = 3; img.mip_levels = 1; img.pixels = px;
    size_t len = 0;
    uint8_t *mem = encode_mem(&img, &len);
    int ok = mem && yapf_save(tmp, &img) == YAPF_OK;
    FILE *f = ok ? fopen(tmp, "rb") : NULL;
    if (f) {
        uint8_t *disk = (uint8_t *)malloc(len + 1);
        ok = fread(disk, 1, len + 1, f) == len && memcmp(disk, mem, len) == 0;
        free(disk);
        fclose(f);
    }
    remove(tmp);
    free(mem);
    CHECK(ok, "yapf_save writes the same bytes as yapf_encode");
    void *d = (void *)1;
    size_t n = 1;
    CHECK(yapf_encode(NULL, &d, &n) == YAPF_ERR_INVALID && d == NULL && n == 0,
          "yapf_encode rejects a NULL image");
}

static void roundtrip(int w, int h, int ch, int kind) {
    size_t   n = (size_t)w * h * ch, len = 0;
    uint8_t *p = (uint8_t *)malloc(n);
    fill(p, w, h, ch, kind);

    yapf_image_t img;
    memset(&img, 0, sizeof(img));
    img.width = (uint32_t)w; img.height = (uint32_t)h;
    img.channels = (uint8_t)ch; img.mip_levels = 1; img.pixels = p;

    uint8_t *file = encode_mem(&img, &len);
    CHECK(file != NULL, "encode %dx%d ch%d kind%d", w, h, ch, kind);
    if (file) {
        yapf_image_t *a = yapf_load_memory(file, len);
        yapf_image_t *b = yapf_load_memory_mt(file, len, 0);
        CHECK(a && memcmp(a->pixels, p, n) == 0,
              "roundtrip %dx%d ch%d kind%d", w, h, ch, kind);
        CHECK(b && memcmp(b->pixels, p, n) == 0,
              "roundtrip (threads) %dx%d ch%d kind%d", w, h, ch, kind);
        yapf_free(a);
        yapf_free(b);
        free(file);
    }
    free(p);
}

static void mips(void) {
    const int w = 300, h = 200, levels = 9;
    uint8_t  *mp[9];
    yapf_image_t img;
    memset(&img, 0, sizeof(img));
    img.width = w; img.height = h; img.channels = 4; img.mip_levels = levels;
    for (int l = 0; l < levels; l++) {
        int mw = w >> l ? w >> l : 1, mh = h >> l ? h >> l : 1;
        mp[l] = (uint8_t *)malloc((size_t)mw * mh * 4);
        fill(mp[l], mw, mh, 4, l % 5);
    }
    img.pixels = mp[0];
    img.mips = mp;

    size_t len = 0;
    uint8_t *file = encode_mem(&img, &len);
    yapf_image_t *d = file ? yapf_load_memory_mt(file, len, 3) : NULL;
    int ok = d && d->mip_levels == levels;
    for (int l = 0; ok && l < levels; l++) {
        int mw = w >> l ? w >> l : 1, mh = h >> l ? h >> l : 1;
        ok = memcmp(d->mips[l], mp[l], (size_t)mw * mh * 4) == 0;
    }
    CHECK(ok, "mip chain of %d levels", levels);
    yapf_free(d);
    free(file);
    for (int l = 0; l < levels; l++) free(mp[l]);
}

/* Flip bits, truncate, scribble: the decoder must never crash, and a
 * truncated file must be rejected. */
static void corruption(void) {
    const int w = 130, h = 70, ch = 4;
    uint8_t *p = (uint8_t *)malloc((size_t)w * h * ch);
    fill(p, w, h, ch, 3);
    yapf_image_t img;
    memset(&img, 0, sizeof(img));
    img.width = w; img.height = h; img.channels = ch; img.mip_levels = 1; img.pixels = p;
    size_t len = 0;
    uint8_t *file = encode_mem(&img, &len);
    uint8_t *bad = (uint8_t *)malloc(len);
    int truncated_ok = 1;
    for (int it = 0; it < 3000 && file; it++) {
        memcpy(bad, file, len);
        size_t n = len;
        switch (it % 3) {
        case 0: for (int k = 0; k < 1 + it % 7; k++) bad[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: n = rnd() % len; break;
        default: bad[rnd() % len] = (uint8_t)rnd(); break;
        }
        yapf_image_t *d = (it & 1) ? yapf_load_memory_mt(bad, n, 2) : yapf_load_memory(bad, n);
        if (it % 3 == 1 && d) truncated_ok = 0;
        yapf_free(d);
    }
    CHECK(file != NULL, "corruption test setup");
    CHECK(truncated_ok, "truncated files are rejected");
    free(bad);
    free(file);
    free(p);
}

static uint32_t fnv(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}


/* ======================================================================
 *  Version 2
 * ====================================================================== */

static yapf_texture_t desc(uint32_t w, uint32_t h, int ch, int sample) {
    yapf_texture_t d;
    memset(&d, 0, sizeof(d));
    d.width = w; d.height = h; d.depth = 1; d.layers = 1; d.frames = 1;
    d.mip_levels = 1; d.channels = (uint8_t)ch; d.sample = (uint8_t)sample;
    return d;
}

/* Fills every surface: noise, gradient or noisy curves, in the sample
 * type; block payloads get bytes with some structure. */
static void tex_fill(yapf_texture_t *t, int kind) {
    uint32_t S = yapf_tex_surface_count(t);
    for (uint32_t s = 0; s < S; s++) {
        uint32_t m = s % t->mip_levels, w, h, d;
        yapf_tex_mip_size(t, m, &w, &h, &d);
        uint8_t *p = (uint8_t *)t->data[s];
        if (t->payload == YAPF_PAYLOAD_BLOCKS) {
            size_t n = yapf_tex_surface_bytes(t, m);
            for (size_t i = 0; i < n; i++)
                p[i] = kind ? (uint8_t)(i % 16 < 8 ? (i / 64) & 0xFF : rnd()) : (uint8_t)rnd();
            continue;
        }
        size_t cnt = (size_t)w * h * d * t->channels;
        for (size_t i = 0; i < cnt; i++) {
            size_t   px = i / t->channels;
            uint32_t x = (uint32_t)(px % w), y = (uint32_t)((px / w) % h);
            int      c = (int)(i % t->channels);
            double   v = kind == 0 ? (rnd() % 1000) / 1000.0
                       : kind == 1 ? (x * 3 + y * 5 + c * 40) % 600 / 600.0
                       : ((x * x + y * y) / 50 % 700 + rnd() % 5) / 705.0;
            switch (t->sample) {
            case YAPF_SAMPLE_U8:  p[i] = (uint8_t)(v * 255); break;
            case YAPF_SAMPLE_U16: ((uint16_t *)p)[i] = (uint16_t)(v * 65535); break;
            case YAPF_SAMPLE_F16: ((uint16_t *)p)[i] = yapf_float_to_half((float)(v * 20 - 2)); break;
            default:              ((float *)p)[i] = (float)(v * 100 - 10) + (kind == 0 ? (float)rnd() : 0.0f); break;
            }
        }
    }
}

static int tex_equal(const yapf_texture_t *a, const yapf_texture_t *b) {
    uint32_t S = yapf_tex_surface_count(a);
    if (S != yapf_tex_surface_count(b)) return 0;
    for (uint32_t s = 0; s < S; s++)
        if (!a->data[s] || !b->data[s] ||
            memcmp(a->data[s], b->data[s], yapf_tex_surface_bytes(a, s % a->mip_levels)) != 0)
            return 0;
    return 1;
}

/* Encode, decode, compare; returns the encoded file (caller frees). */
static uint8_t *tex_roundtrip(yapf_texture_t *t, int flags, size_t *len, const char *what) {
    void *buf = NULL;
    *len = 0;
    int   e = yapf_tex_encode(t, flags, &buf, len);
    CHECK(e == YAPF_OK, "v2 encode %s (%d)", what, e);
    if (e != YAPF_OK) return NULL;
    int err = 0;
    yapf_texture_t *a = yapf_tex_load_memory(buf, *len, 0, 1, &err);
    yapf_texture_t *b = yapf_tex_load_memory(buf, *len, 0, 0, &err);
    CHECK(a && tex_equal(a, t), "v2 roundtrip %s", what);
    CHECK(b && tex_equal(b, t), "v2 roundtrip (threads) %s", what);
    yapf_tex_free(a);
    yapf_tex_free(b);
    uint8_t *copy = (uint8_t *)malloc(*len);
    memcpy(copy, buf, *len);
    yapf_free_buffer(buf);
    return copy;
}

static void v2_pixels(void) {
    static const int sizes[][2] = { {1, 1}, {7, 3}, {64, 64}, {65, 63}, {130, 70}, {200, 201} };
    static const char *sn[] = { "u8", "u16", "f16", "f32" };
    for (size_t si = 0; si < sizeof(sizes) / sizeof(sizes[0]); si++)
        for (int ch = 1; ch <= 4; ch++)
            for (int sample = 0; sample < 4; sample++)
                for (int kind = 0; kind < 3; kind++)
                    for (int fl = 0; fl < 2; fl++) {
                        yapf_texture_t  d = desc((uint32_t)sizes[si][0], (uint32_t)sizes[si][1], ch, sample);
                        yapf_texture_t *t = yapf_tex_create(&d);
                        char what[96];
                        sprintf(what, "%dx%d ch%d %s kind%d%s", sizes[si][0], sizes[si][1], ch,
                                sn[sample], kind, fl ? " entropy" : "");
                        tex_fill(t, kind);
                        size_t len;
                        free(tex_roundtrip(t, fl ? YAPF_ENC_ENTROPY : 0, &len, what));
                        yapf_tex_free(t);
                    }
}

/* Shapes, frames, notes and every description field survive. */
static void v2_layout(void) {
    struct { int w, h, ch, sample, shape, layers, frames, mips, depth; } c[] = {
        { 64, 64, 4, 0, YAPF_SHAPE_CUBE, 1, 1, 7, 1 },
        { 32, 32, 3, 2, YAPF_SHAPE_CUBE, 2, 1, 6, 1 },
        { 40, 30, 4, 0, YAPF_SHAPE_2D, 5, 1, 3, 1 },
        { 33, 17, 1, 1, YAPF_SHAPE_VOLUME, 1, 1, 4, 9 },
        { 50, 50, 4, 0, YAPF_SHAPE_2D, 1, 6, 1, 1 },
    };
    for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        yapf_texture_t d = desc((uint32_t)c[i].w, (uint32_t)c[i].h, c[i].ch, c[i].sample);
        d.shape = (uint8_t)c[i].shape; d.layers = (uint16_t)c[i].layers; d.frames = (uint16_t)c[i].frames;
        d.mip_levels = (uint8_t)c[i].mips; d.depth = (uint32_t)c[i].depth;
        d.channel_map[0] = YAPF_CH_NORMAL_X; d.channel_map[1] = YAPF_CH_ROUGHNESS;
        d.linear_mask = 0x08; d.normal = YAPF_NORMAL_DX | YAPF_NORMAL_XY;
        d.primaries = YAPF_PRIMARIES_DISPLAY_P3; d.transfer = YAPF_TRANSFER_PQ;
        d.mip_filter = YAPF_MIP_KAISER; d.alpha_ref = 77; d.loop_count = 3;
        d.flags = YAPF_FLAG_SRGB | YAPF_FLAG_COVERAGE;
        yapf_texture_t *t = yapf_tex_create(&d);
        tex_fill(t, 2);
        if (t->frames > 1) for (int f = 0; f < t->frames; f++) t->frame_us[f] = 16667u * (uint32_t)(f + 1);
        yapf_tex_add_note(t, "AUTH", "Keel", 4);
        yapf_tex_add_note(t, "ICCP", "\0\1\2", 3);
        size_t len;
        char what[32];
        sprintf(what, "layout case %d", (int)i);
        uint8_t *file = tex_roundtrip(t, (int)(i & 1), &len, what);
        yapf_texture_t *b = file ? yapf_tex_load_memory(file, len, 0, 1, NULL) : NULL;
        int ok = b && b->shape == d.shape && b->layers == d.layers && b->frames == d.frames &&
                 b->depth == d.depth && b->mip_levels == d.mip_levels &&
                 memcmp(b->channel_map, d.channel_map, 4) == 0 && b->linear_mask == 0x08 &&
                 b->normal == d.normal && b->primaries == d.primaries && b->transfer == d.transfer &&
                 b->mip_filter == d.mip_filter && b->alpha_ref == 77 && b->loop_count == 3 &&
                 b->flags == d.flags && b->note_count == 2;
        const yapf_note_t *n = b ? yapf_tex_find_note(b, "ICCP") : NULL;
        ok = ok && n && n->size == 3 && memcmp(n->data, "\0\1\2", 3) == 0;
        if (ok && b->frames > 1) ok = b->frame_us[b->frames - 1] == 16667u * b->frames;
        CHECK(ok, "v2 fields, notes and frame times survive (%s)", what);
        yapf_tex_free(b);
        free(file);
        yapf_tex_free(t);
    }
}

static void v2_blocks(void) {
    static const uint8_t fmts[] = { YAPF_GPU_BC1, YAPF_GPU_BC7, YAPF_GPU_BC4, YAPF_GPU_ASTC_6x6,
                                    YAPF_GPU_ASTC_12x12, YAPF_GPU_ETC2_RGBA8 };
    for (size_t f = 0; f < sizeof(fmts); f++)
        for (int fl = 0; fl < 2; fl++) {
            yapf_texture_t d = desc(300, 200, 4, 0);
            d.payload = YAPF_PAYLOAD_BLOCKS; d.format = fmts[f]; d.mip_levels = 3;
            if (f == 3) { d.shape = YAPF_SHAPE_VOLUME; d.depth = 5; }
            yapf_texture_t *t = yapf_tex_create(&d);
            tex_fill(t, 1);
            char what[48];
            sprintf(what, "blocks 0x%02x%s", fmts[f], fl ? " packed" : "");
            size_t len;
            free(tex_roundtrip(t, fl, &len, what));
            yapf_tex_free(t);
        }
}

/* Recomputes both header checks after editing a file. */
static void reseal(uint8_t *f, size_t front) {
    uint32_t a = yapf_checksum(f + 64, front - 64), b;
    f[0x38] = (uint8_t)a; f[0x39] = (uint8_t)(a >> 8); f[0x3A] = (uint8_t)(a >> 16); f[0x3B] = (uint8_t)(a >> 24);
    b = yapf_checksum(f, 0x3C);
    f[0x3C] = (uint8_t)b; f[0x3D] = (uint8_t)(b >> 8); f[0x3E] = (uint8_t)(b >> 16); f[0x3F] = (uint8_t)(b >> 24);
}

/* Flips, truncation, scribbles: never crash; truncation always fails. */
static void v2_fuzz(const uint8_t *file, size_t len, const char *what) {
    uint8_t *bad = (uint8_t *)malloc(len);
    int      truncated_ok = 1, err;
    for (int it = 0; it < 2000; it++) {
        memcpy(bad, file, len);
        size_t n = len;
        switch (it % 3) {
        case 0: for (int k = 0; k < 1 + it % 7; k++) bad[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8)); break;
        case 1: n = rnd() % len; break;
        default: bad[rnd() % len] = (uint8_t)rnd(); break;
        }
        int fl = (it & 4) ? YAPF_LOAD_TRUST : 0;
        yapf_texture_t *x = yapf_tex_load_memory(bad, n, fl, (it & 1) ? 2 : 1, &err);
        if (it % 3 == 1 && x) truncated_ok = 0;
        yapf_tex_free(x);
        if (it % 50 == 0) yapf_free(yapf_load_memory(bad, n));
    }
    CHECK(truncated_ok, "truncated v2 files are rejected (%s)", what);
    free(bad);
}

static void v2_integrity(void) {
    yapf_texture_t  d = desc(150, 90, 4, YAPF_SAMPLE_F16);
    yapf_texture_t *t = yapf_tex_create(&d);
    tex_fill(t, 2);
    size_t   len;
    uint8_t *file = tex_roundtrip(t, YAPF_ENC_ENTROPY, &len, "integrity setup");
    if (!file) { yapf_tex_free(t); return; }
    uint8_t *bad = (uint8_t *)malloc(len);
    size_t   front = 64 + 16;                           /* header + one surface entry */
    int      err;

    /* a flipped bit in tile data is caught by its yapf-sum ... */
    memcpy(bad, file, len);
    bad[len - 10] ^= 0x10;
    CHECK(!yapf_tex_load_memory(bad, len, 0, 1, &err) && err == YAPF_ERR_CHECKSUM,
          "corrupt tile data reports YAPF_ERR_CHECKSUM (%d)", err);
    yapf_texture_t *trusted = yapf_tex_load_memory(bad, len, YAPF_LOAD_TRUST, 1, &err);
    yapf_tex_free(trusted);          /* may or may not decode; must not crash */
    /* ... and in the header by the header check */
    memcpy(bad, file, len);
    bad[0x0C] ^= 1;
    CHECK(!yapf_tex_load_memory(bad, len, 0, 1, &err) && err == YAPF_ERR_CHECKSUM,
          "corrupt header reports YAPF_ERR_CHECKSUM");
    /* an unknown needs bit: refused as unsupported */
    memcpy(bad, file, len);
    bad[0x1F] |= 0x80;
    reseal(bad, front);
    CHECK(!yapf_tex_load_memory(bad, len, 0, 1, &err) && err == YAPF_ERR_UNSUPPORTED,
          "unknown needs bit is refused (%d)", err);
    /* a feature in use but not announced: corrupt */
    memcpy(bad, file, len);
    bad[0x1C] &= (uint8_t)~YAPF_NEED_WIDE;
    reseal(bad, front);
    CHECK(!yapf_tex_load_memory(bad, len, 0, 1, &err) && err == YAPF_ERR_CORRUPT,
          "unannounced feature is refused (%d)", err);
    /* the old API refuses what it cannot represent */
    CHECK(yapf_load_memory(file, len) == NULL, "yapf_load refuses half-float textures");

    free(bad);
    v2_fuzz(file, len, "half float + entropy");
    free(file);
    yapf_tex_free(t);

    /* 8-bit RGBA with entropy planes (decoded several streams at once) */
    d = desc(140, 75, 4, 0);
    t = yapf_tex_create(&d);
    tex_fill(t, 2);
    file = tex_roundtrip(t, YAPF_ENC_ENTROPY, &len, "fuzz setup rgba8");
    if (file) v2_fuzz(file, len, "rgba8 + entropy");
    free(file);
    /* BC7 blocks with packed tiles */
    yapf_texture_t *c = NULL;
    if (yapf_tex_compress(t, YAPF_GPU_BC7, &c) == YAPF_OK) {
        file = tex_roundtrip(c, YAPF_ENC_ENTROPY, &len, "fuzz setup bc7");
        if (file) v2_fuzz(file, len, "packed BC7");
        free(file);
    }
    yapf_tex_free(c);
    yapf_tex_free(t);
}

/* Streaming: a file prefix holds the small mips; tiles decode alone. */
static void v2_streaming(void) {
    yapf_texture_t d = desc(300, 260, 4, 0);
    d.shape = YAPF_SHAPE_CUBE; d.width = d.height = 260; d.mip_levels = 5;
    yapf_texture_t *t = yapf_tex_create(&d);
    tex_fill(t, 2);
    void  *buf = NULL;
    size_t len = 0;
    yapf_tex_encode(t, 0, &buf, &len);
    uint8_t *file = (uint8_t *)buf;
    int ok = 1, err = 0;
    for (uint32_t m = 0; m < 5 && ok; m++) {
        yapf_texture_t *probe = yapf_tex_open(file, len, 0, &err);
        uint64_t need = probe ? yapf_tex_stream_bytes(probe, m) : 0;
        yapf_tex_free(probe);
        yapf_texture_t *s = yapf_tex_open(file, (size_t)need, 0, &err);
        ok = s != NULL && need <= len && (m > 0 || need == len);
        for (uint32_t f = 0; ok && f < 6; f++)
            for (uint32_t mm = 0; ok && mm < 5; mm++) {
                uint32_t sf = yapf_tex_surface(s, 0, 0, f, mm);
                int rc = yapf_tex_decode_surface(s, file, (size_t)need, sf, 1);
                if (mm >= m) ok = rc == YAPF_OK &&
                    memcmp(s->data[sf], t->data[sf], yapf_tex_surface_bytes(t, mm)) == 0;
                else ok = rc == YAPF_ERR_TRUNCATED;
            }
        yapf_tex_free(s);
    }
    CHECK(ok, "stream prefixes decode exactly their small mips");

    /* tile decode equals the matching part of the full surface */
    yapf_texture_t *s = yapf_tex_open(file, len, 0, &err);
    uint32_t sf = yapf_tex_surface(s, 0, 0, 3, 0), tx, ty;
    yapf_tex_tile_grid(s, 0, &tx, &ty);
    uint8_t tile[64 * 64 * 4];
    ok = s && tx == 5 && ty == 5;
    for (uint32_t y = 0; ok && y < ty; y++)
        for (uint32_t x = 0; ok && x < tx; x++) {
            uint32_t px, py, pw, ph;
            ok = yapf_tex_decode_tile(s, file, len, sf, x, y, 0, tile, 64 * 4, &px, &py, &pw, &ph) == YAPF_OK;
            for (uint32_t r = 0; ok && r < ph; r++)
                ok = memcmp(tile + r * 64 * 4, (uint8_t *)t->data[sf] + ((size_t)(py + r) * 260 + px) * 4,
                            (size_t)pw * 4) == 0;
        }
    CHECK(ok, "single tiles decode to the same pixels");
    yapf_tex_free(s);
    yapf_free_buffer(buf);
    yapf_tex_free(t);
}

/* v1 and v2 work through either API where the data allows it. */
static void v2_compat(const char *sample) {
    yapf_texture_t *t = yapf_tex_load(sample, 0, 0, NULL);
    if (t) {
        uint32_t h = fnv((const uint8_t *)t->data[0], yapf_tex_surface_bytes(t, 0));
        CHECK(h == SAMPLE_HASH, "yapf_tex_load reads the v1 sample");
        size_t   len;
        uint8_t *file = tex_roundtrip(t, YAPF_ENC_ENTROPY, &len, "sample as v2");
        yapf_image_t *img = file ? yapf_load_memory(file, len) : NULL;
        CHECK(img && img->width == 512 && img->channels == 4 &&
              fnv(img->pixels, 512u * 512u * 4u) == SAMPLE_HASH, "yapf_load reads an 8-bit v2 file");
        yapf_free(img);
        free(file);
        yapf_tex_free(t);
    }
    /* cube: the v1 API sees face 0 with its mips */
    yapf_texture_t d = desc(40, 40, 3, 0);
    d.shape = YAPF_SHAPE_CUBE; d.mip_levels = 3;
    yapf_texture_t *c = yapf_tex_create(&d);
    tex_fill(c, 1);
    size_t   len;
    uint8_t *file = tex_roundtrip(c, 0, &len, "cube for v1 API");
    yapf_image_t *img = file ? yapf_load_memory_mt(file, len, 2) : NULL;
    CHECK(img && img->mip_levels == 3 && memcmp(img->mips[2], c->data[2], 10 * 10 * 3) == 0 &&
          memcmp(img->pixels, c->data[0], 40 * 40 * 3) == 0, "yapf_load reads face 0 of a cube");
    yapf_free(img);
    free(file);
    yapf_tex_free(c);
}

static double psnr8(const uint8_t *a, int sa, const uint8_t *b, int sb, size_t n, int ch) {
    double se = 0;
    for (size_t i = 0; i < n; i++)
        for (int c = 0; c < ch; c++) {
            double v = (double)a[i * sa + c] - b[i * sb + c];
            se += v * v;
        }
    if (se == 0) return 99.0;
    return 10.0 * log10(255.0 * 255.0 * (double)(n * ch) / se);
}

static void v2_gpu(void) {
    const uint32_t w = 61, h = 47;                   /* not multiples of 4 */
    yapf_texture_t d = desc(w, h, 4, 0);
    yapf_texture_t *t = yapf_tex_create(&d);
    uint8_t *p = (uint8_t *)t->data[0];
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            uint8_t *q = p + ((size_t)y * w + x) * 4;
            q[0] = (uint8_t)(x * 4); q[1] = (uint8_t)(y * 5); q[2] = (uint8_t)(255 - x * 2);
            q[3] = (uint8_t)(((x / 8 + y / 8) & 1) ? 255 : 40 + x);
        }
    static const struct { uint8_t f; int ch; double min; } F[] = {
        { YAPF_GPU_BC1, 3, 30 }, { YAPF_GPU_BC3, 4, 30 }, { YAPF_GPU_BC7, 4, 34 },
        { YAPF_GPU_BC4, 1, 38 }, { YAPF_GPU_BC5, 2, 38 }
    };
    uint8_t *out = (uint8_t *)malloc((size_t)w * h * 4);
    for (size_t i = 0; i < sizeof(F) / sizeof(F[0]); i++) {
        yapf_texture_t *c = NULL;
        int rc = yapf_tex_compress(t, F[i].f, &c);
        int ok = rc == YAPF_OK && c->payload == YAPF_PAYLOAD_BLOCKS &&
                 yapf_blocks_decode(F[i].f, c->data[0], w, h, out) == YAPF_OK;
        double q = 0;
        if (ok && F[i].f == YAPF_GPU_BC1) {
            /* BC1 keeps alpha only as cut-out: compare opaque texels */
            for (size_t k = 0; k < (size_t)w * h; k++) if (p[k * 4 + 3] < 128) memcpy(out + k * 4, p + k * 4, 3);
        }
        if (ok) q = psnr8(p, 4, out, 4, (size_t)w * h, F[i].ch);
        CHECK(ok && q >= F[i].min, "GPU format 0x%02x encodes (PSNR %.1f dB)", F[i].f, q);
        if (c) {
            size_t len;
            char what[32];
            sprintf(what, "compressed 0x%02x", F[i].f);
            free(tex_roundtrip(c, YAPF_ENC_ENTROPY, &len, what));
        }
        yapf_tex_free(c);
    }
    CHECK(yapf_blocks_decode(YAPF_GPU_ASTC_4x4, out, 4, 4, out) == YAPF_ERR_UNSUPPORTED,
          "ASTC blocks are carried, not decoded");
    free(out);
    yapf_tex_free(t);

    /* BC6H: relative error of a smooth HDR sky gradient */
    yapf_texture_t dh = desc(32, 32, 3, YAPF_SAMPLE_F16);
    yapf_texture_t *th = yapf_tex_create(&dh), *ch = NULL;
    uint16_t *hp = (uint16_t *)th->data[0];
    for (int i = 0; i < 32 * 32; i++)
        for (int k = 0; k < 3; k++) hp[i * 3 + k] = yapf_float_to_half((0.3f + 0.4f * (float)k) *
                                                (1.0f + (float)(i % 32) * 0.02f + (float)(i / 32) * 0.15f));
    int ok = yapf_tex_compress(th, YAPF_GPU_BC6H, &ch) == YAPF_OK;
    uint16_t back[32 * 32 * 3];
    double worst = 0;
    if (ok) ok = yapf_blocks_decode(YAPF_GPU_BC6H, ch->data[0], 32, 32, back) == YAPF_OK;
    for (int i = 0; ok && i < 32 * 32 * 3; i++) {
        float a = yapf_half_to_float(hp[i]), b = yapf_half_to_float(back[i]);
        double r = (a > b ? a - b : b - a) / a;
        if (r > worst) worst = r;
    }
    CHECK(ok && worst < 0.05, "BC6H encodes HDR within 5%% (worst %.4f)", worst);
    yapf_tex_free(ch);
    yapf_tex_free(th);
}

static void v2_mips(void) {
    /* sRGB: a black / white checker averages to sRGB 188, not 128 */
    yapf_texture_t d = desc(64, 64, 4, 0);
    d.flags = YAPF_FLAG_SRGB;
    yapf_texture_t *t = yapf_tex_create(&d);
    uint8_t *p = (uint8_t *)t->data[0];
    for (int i = 0; i < 64 * 64; i++) {
        uint8_t v = ((i % 64 + i / 64) & 1) ? 255 : 0;
        p[i * 4] = p[i * 4 + 1] = p[i * 4 + 2] = v;
        p[i * 4 + 3] = v;                                /* alpha stays linear */
    }
    int ok = yapf_tex_build_mips(t, 0, 0) == YAPF_OK && t->mip_levels == 7;
    const uint8_t *m1 = ok ? (const uint8_t *)t->data[1] : NULL;
    CHECK(ok && m1[0] == 188 && m1[3] == 128 && (t->mip_filter & YAPF_MIP_LINEAR_LIGHT),
          "sRGB mips average in linear light (got %d, alpha %d)", m1 ? m1[0] : -1, m1 ? m1[3] : -1);
    yapf_tex_free(t);

    /* alpha test: speckled foliage (30 % of texels opaque).  A plain box
     * filter thins it out with distance; with alpha_ref = 128 every mip
     * keeps about the same share of texels above 128. */
    double plain[5] = {0}, kept[5] = {0};
    for (int pass = 0; pass < 2; pass++) {
        d = desc(128, 128, 4, 0);
        t = yapf_tex_create(&d);
        p = (uint8_t *)t->data[0];
        rng = 99;
        for (int i = 0; i < 128 * 128; i++) p[i * 4 + 3] = rnd() % 100 < 30 ? 255 : 0;
        ok = yapf_tex_build_mips(t, 5, pass ? 128 : 0) == YAPF_OK && t->mip_levels == 5;
        if (pass) ok = ok && (t->flags & YAPF_FLAG_COVERAGE) && t->alpha_ref == 128;
        for (uint32_t m = 1; ok && m < 5; m++) {
            uint32_t w, h, n = 0;
            yapf_tex_mip_size(t, m, &w, &h, NULL);
            const uint8_t *q = (const uint8_t *)t->data[m];
            for (uint32_t i = 0; i < w * h; i++) n += q[i * 4 + 3] > 128;
            (pass ? kept : plain)[m] = (double)n / (w * h);
        }
        if (pass) {
            size_t len;
            free(tex_roundtrip(t, 0, &len, "mipped texture"));
        }
        yapf_tex_free(t);
    }
    for (uint32_t m = 1; ok && m < 5; m++) ok = kept[m] > 0.2 && kept[m] < 0.4;
    CHECK(ok && plain[2] < 0.15, "coverage-preserving mips keep alpha-test coverage "
          "(mip 2: %.2f, plain box %.2f)", kept[2], plain[2]);

    /* float volume: 2×2×2 box */
    d = desc(4, 4, 1, YAPF_SAMPLE_F32);
    d.shape = YAPF_SHAPE_VOLUME; d.depth = 4;
    t = yapf_tex_create(&d);
    float *f = (float *)t->data[0];
    for (int i = 0; i < 64; i++) f[i] = (float)i;
    ok = yapf_tex_build_mips(t, 0, 0) == YAPF_OK && t->mip_levels == 3;
    CHECK(ok && ((float *)t->data[1])[0] == (0 + 1 + 4 + 5 + 16 + 17 + 20 + 21) / 8.0f &&
          ((float *)t->data[2])[0] == 31.5f, "volume mips filter in 3D");
    yapf_tex_free(t);
}

static void v2_preview(void) {
    uint8_t out[4 * 4 * 4];
    /* gray + alpha u16 */
    yapf_texture_t d = desc(4, 4, 2, YAPF_SAMPLE_U16);
    yapf_texture_t *t = yapf_tex_create(&d);
    uint16_t *p = (uint16_t *)t->data[0];
    for (int i = 0; i < 16; i++) { p[2 * i] = 65535; p[2 * i + 1] = 32896; }
    int ok = yapf_tex_to_rgba8(t, 0, 0, out) == YAPF_OK &&
             out[0] == 255 && out[1] == 255 && out[2] == 255 && out[3] == 128;
    yapf_tex_free(t);
    /* linear half 0.2140 → sRGB 128, 2.0 clamps, alpha stays linear */
    d = desc(4, 4, 4, YAPF_SAMPLE_F16);
    t = yapf_tex_create(&d);
    p = (uint16_t *)t->data[0];
    for (int i = 0; i < 16; i++) {
        p[4 * i] = yapf_float_to_half(0.2158605f); p[4 * i + 1] = yapf_float_to_half(2.0f);
        p[4 * i + 2] = yapf_float_to_half(-1.0f);  p[4 * i + 3] = yapf_float_to_half(0.5f);
    }
    ok = ok && yapf_tex_to_rgba8(t, 0, 0, out) == YAPF_OK &&
         out[0] == 128 && out[1] == 255 && out[2] == 0 && out[3] == 128;
    /* BC7 and BC6H go through the block decoders */
    yapf_texture_t *c = NULL;
    ok = ok && yapf_tex_compress(t, YAPF_GPU_BC6H, &c) == YAPF_OK &&
         yapf_tex_to_rgba8(c, 0, 0, out) == YAPF_OK && out[3] == 255 && out[1] == 255;
    yapf_tex_free(c);
    yapf_tex_free(t);
    CHECK(ok, "yapf_tex_to_rgba8 converts every payload for display");
}

static void v2_misc(void) {
    int ok = 1;
    for (uint32_t h = 0; h < 0x10000 && ok; h++) {
        if ((h & 0x7C00) == 0x7C00 && (h & 0x3FF)) continue;    /* NaN payloads */
        ok = yapf_float_to_half(yapf_half_to_float((uint16_t)h)) == h;
    }
    CHECK(ok, "every half survives half -> float -> half");
    CHECK(yapf_float_to_half(1.0f) == 0x3C00 && yapf_float_to_half(65520.0f) == 0x7C00 &&
          yapf_float_to_half(1e-8f) == 0 && yapf_float_to_half(-2.0f) == 0xC000, "float -> half rounding");
    /* fixed vectors pin yapf-sum to the specification */
    CHECK(yapf_checksum("", 0) == 0xd7550ed0u && yapf_checksum("YAPF", 4) == 0xc8cc41d4u &&
          yapf_checksum("The quick brown fox jumps over the lazy dog", 43) == 0xbce4f410u,
          "yapf-sum test vectors");
    /* encoding is deterministic */
    yapf_texture_t d = desc(90, 70, 3, YAPF_SAMPLE_U16);
    yapf_texture_t *t = yapf_tex_create(&d);
    tex_fill(t, 2);
    void *a = NULL, *b = NULL;
    size_t la = 0, lb = 0;
    yapf_tex_encode(t, YAPF_ENC_ENTROPY, &a, &la);
    yapf_tex_encode(t, YAPF_ENC_ENTROPY, &b, &lb);
    CHECK(a && la == lb && memcmp(a, b, la) == 0, "v2 encoding is deterministic");
    yapf_free_buffer(a);
    yapf_free_buffer(b);
    yapf_tex_free(t);
    d = desc(10, 12, 4, 0);
    d.shape = YAPF_SHAPE_CUBE;
    CHECK(yapf_tex_create(&d) == NULL, "a cube must be square");
}

int main(int argc, char **argv) {
    static const int sizes[][2] = {
        {1, 1}, {1, 7}, {7, 1}, {63, 63}, {64, 64}, {65, 65}, {127, 1},
        {1, 129}, {97, 61}, {200, 130}, {256, 256}, {513, 77},
        {1000, 700}   /* 176 tiles: big enough for the threaded decoder */
    };
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
        for (int ch = 1; ch <= 4; ch++)
            for (int kind = 0; kind < 5; kind++)
                roundtrip(sizes[s][0], sizes[s][1], ch, kind);
    mips();
    corruption();
    save_matches_encode();

    const char *sample = argc > 1 ? argv[1] : "other/YAPF.YAPF";
    yapf_image_t *img = yapf_load(sample);
    if (img) {
        uint32_t h = fnv(img->pixels, (size_t)img->width * img->height * img->channels);
        CHECK(img->width == 512 && img->height == 512 && img->channels == 4 &&
              h == SAMPLE_HASH, "sample %s decodes to the reference pixels (hash %08x)",
              sample, (unsigned)h);
        yapf_free(img);
    } else {
        printf("note: sample %s not found, skipped\n", sample);
    }

    v2_pixels();
    v2_layout();
    v2_blocks();
    v2_integrity();
    v2_streaming();
    v2_compat(sample);
    v2_gpu();
    v2_mips();
    v2_misc();
    v2_preview();

    printf("%d tests, %d failures\n", tests, fails);
    return fails != 0;
}
