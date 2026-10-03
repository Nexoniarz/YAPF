/*
 * test_yapf.c — self-contained test suite for yapf.c
 *
 *   cc -O2 -std=c99 tests/test_yapf.c yapf.c -o test_yapf -pthread
 *   ./test_yapf [path/to/other/YAPF.YAPF]
 *
 * Round-trips many sizes, channel counts and image kinds, checks mip
 * chains, single- vs multi-threaded decode, the bundled sample file, and
 * that corrupted or truncated files are rejected without crashing.
 * Exit code 0 means every test passed.
 */
#include "../yapf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    printf("%d tests, %d failures\n", tests, fails);
    return fails != 0;
}
