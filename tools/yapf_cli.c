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
 *  yapf info <file.yapf>               print header information
 *  yapf bench <file.yapf>              time decoding
 *  yapf thumbnail <in.yapf> <out.png> [size]   small preview (default 256)
 *
 *  Reads PNG, JPEG, BMP, TGA, GIF (first frame), PSD (composite), PNM, HDR
 *  (tone-clamped) and YAPF.  Writes YAPF, PNG, BMP, TGA and JPEG.
 *
 *  Options for writing YAPF:
 *    --mips        also store a full mip chain (2×2 box filter)
 *    --linear      mark pixels as linear instead of sRGB
 *    --premult     mark alpha as premultiplied
 *  Options for reading YAPF:
 *    --mip N       export mip level N instead of the base image
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

static uint8_t gpu_hint(int ch, int srgb) {
    switch (ch) {
    case 1:  return YAPF_GPU_R8;
    case 2:  return YAPF_GPU_RG8;
    case 3:  return srgb ? YAPF_GPU_SRGB8 : YAPF_GPU_RGB8;
    default: return srgb ? YAPF_GPU_SRGB8_A8 : YAPF_GPU_RGBA8;
    }
}

/* Full mip chain with a 2×2 box filter (edges clamp for odd sizes). */
static int build_mips(yapf_image_t *img) {
    int levels = 1;
    while (levels < (int)YAPF_MAX_MIPS &&
           ((img->width >> levels) > 0 || (img->height >> levels) > 0))
        levels++;
    img->mips = (uint8_t **)calloc((size_t)levels, sizeof(uint8_t *));
    if (!img->mips) return -1;
    img->mips[0]    = img->pixels;
    img->mip_levels = (uint8_t)levels;

    int ch = img->channels;
    for (int m = 1; m < levels; m++) {
        uint32_t pw = img->width >> (m - 1), ph = img->height >> (m - 1);
        uint32_t w = img->width >> m, h = img->height >> m;
        if (pw < 1) pw = 1;
        if (ph < 1) ph = 1;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        uint8_t *src = img->mips[m - 1];
        uint8_t *dst = (uint8_t *)malloc((size_t)w * h * ch);
        if (!dst) return -1;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                for (int c = 0; c < ch; c++) {
                    uint32_t x0 = 2 * x < pw ? 2 * x : pw - 1, x1 = 2 * x + 1 < pw ? 2 * x + 1 : pw - 1;
                    uint32_t y0 = 2 * y < ph ? 2 * y : ph - 1, y1 = 2 * y + 1 < ph ? 2 * y + 1 : ph - 1;
                    unsigned s = src[((size_t)y0 * pw + x0) * ch + c] + src[((size_t)y0 * pw + x1) * ch + c]
                               + src[((size_t)y1 * pw + x0) * ch + c] + src[((size_t)y1 * pw + x1) * ch + c];
                    dst[((size_t)y * w + x) * ch + c] = (uint8_t)((s + 2) / 4);
                }
        img->mips[m] = dst;
    }
    return 0;
}

static int write_image(const char *path, const uint8_t *px, int w, int h, int ch) {
    if (ends_with(path, ".png"))  return stbi_write_png(path, w, h, ch, px, w * ch);
    if (ends_with(path, ".bmp"))  return stbi_write_bmp(path, w, h, ch, px);
    if (ends_with(path, ".tga"))  return stbi_write_tga(path, w, h, ch, px);
    if (ends_with(path, ".jpg") || ends_with(path, ".jpeg"))
        return stbi_write_jpg(path, w, h, ch, px, 95);
    fprintf(stderr, "yapf: unsupported output format: %s (use .yapf .png .bmp .tga .jpg)\n", path);
    return 0;
}

static const char *gpu_name(int g) {
    static const char *n[] = { "RGBA8", "RGB8", "RG8", "R8", "SRGB8_A8", "SRGB8" };
    return g >= 0 && g < 6 ? n[g] : "unknown";
}

static int cmd_info(const char *path) {
    FILE *f = fopen(path, "rb");
    uint8_t hdr[20];
    if (!f || fread(hdr, 1, 20, f) != 20 || memcmp(hdr, "YAPF", 4) != 0) {
        fprintf(stderr, "yapf: %s is not a YAPF file\n", path);
        if (f) fclose(f);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    unsigned w = hdr[12] | hdr[13] << 8 | (unsigned)hdr[14] << 16 | (unsigned)hdr[15] << 24;
    unsigned h = hdr[16] | hdr[17] << 8 | (unsigned)hdr[18] << 16 | (unsigned)hdr[19] << 24;
    printf("%s\n", path);
    printf("  format version : %u\n", hdr[4]);
    printf("  size           : %u x %u, %u channel(s)\n", w, h, hdr[5]);
    printf("  mip levels     : %u\n", hdr[7]);
    printf("  gpu format     : %s\n", gpu_name(hdr[6]));
    printf("  flags          : %s%s\n", hdr[8] & YAPF_FLAG_SRGB ? "sRGB " : "linear ",
           hdr[8] & YAPF_FLAG_PREMULT_ALPHA ? "premultiplied-alpha" : "");
    printf("  file size      : %ld bytes (%.1f%% of raw)\n", size,
           100.0 * (double)size / ((double)w * h * (hdr[5] ? hdr[5] : 1)));
    yapf_image_t *img = yapf_load(path);
    printf("  decodes        : %s\n", img ? "ok" : "FAILED (corrupt or unsupported)");
    yapf_free(img);
    return img ? 0 : 1;
}

static int cmd_bench(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "yapf: cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return 1; }
    fclose(f);
    yapf_image_t *img = yapf_load_memory(buf, (size_t)n);
    if (!img) { fprintf(stderr, "yapf: %s does not decode\n", path); free(buf); return 1; }
    double raw = (double)img->width * img->height * img->channels;
    yapf_free(img);
    for (int mt = 0; mt < 2; mt++) {
        /* Best of 15 rounds; each round repeats enough decodes to last
         * ~20 ms, so short images are measured warm and not timer-bound. */
        double one = now_s();
        yapf_free(mt ? yapf_load_memory_mt(buf, (size_t)n, 0) : yapf_load_memory(buf, (size_t)n));
        one = now_s() - one;
        int    reps = (int)(0.02 / (one > 1e-6 ? one : 1e-6)) + 1;
        double best = 1e9;
        for (int r = 0; r < 15; r++) {
            double t = now_s();
            for (int k = 0; k < reps; k++)
                yapf_free(mt ? yapf_load_memory_mt(buf, (size_t)n, 0)
                             : yapf_load_memory(buf, (size_t)n));
            t = (now_s() - t) / reps;
            if (t < best) best = t;
        }
        printf("  %-12s %8.3f ms   %6.2f GB/s\n", mt ? "all cores" : "1 thread", best * 1e3, raw / best / 1e9);
    }
    free(buf);
    return 0;
}

/* Preview no larger than size×size: start from the smallest mip that is
 * still big enough, then average boxes of source pixels. */
static int cmd_thumbnail(const char *in, const char *out, int size) {
    yapf_image_t *img = yapf_load_mt(in, 0);
    if (!img) { fprintf(stderr, "yapf: cannot read %s\n", in); return 1; }
    if (size < 1) size = 256;
    int m = 0;
    while (m + 1 < img->mip_levels &&
           (int)(img->width >> (m + 1)) >= size && (int)(img->height >> (m + 1)) >= size)
        m++;
    uint32_t sw = img->width >> m ? img->width >> m : 1, sh = img->height >> m ? img->height >> m : 1;
    const uint8_t *src = img->mips[m];
    double   k  = (double)size / (double)(sw > sh ? sw : sh);
    if (k > 1.0) k = 1.0;
    uint32_t dw = (uint32_t)(sw * k + 0.5), dh = (uint32_t)(sh * k + 0.5);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int      ch = img->channels;
    uint8_t *dst = (uint8_t *)malloc((size_t)dw * dh * ch);
    if (!dst) { yapf_free(img); return 1; }
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
                uint64_t n = (uint64_t)(y1 - y0) * (x1 - x0);
                dst[((size_t)y * dw + x) * ch + c] = (uint8_t)((sum + n / 2) / n);
            }
        }
    }
    int ok = stbi_write_png(out, (int)dw, (int)dh, ch, dst, (int)dw * ch);
    free(dst);
    yapf_free(img);
    if (!ok) fprintf(stderr, "yapf: cannot write %s\n", out);
    return ok ? 0 : 1;
}

static void usage(void) {
    fprintf(stderr,
        "usage: yapf <input> <output> [options]   convert (formats by extension)\n"
        "       yapf info <file.yapf>              show header information\n"
        "       yapf bench <file.yapf>             time decoding\n"
        "       yapf thumbnail <in.yapf> <out.png> [size]   preview, default 256 px\n"
        "\n"
        "reads:  .yapf .png .jpg .bmp .tga .gif .psd .pnm .hdr\n"
        "writes: .yapf .png .jpg .bmp .tga\n"
        "\n"
        "options when writing .yapf:\n"
        "  --mips      store a full mip chain\n"
        "  --linear    pixels are linear, not sRGB\n"
        "  --premult   alpha is premultiplied\n"
        "options when reading .yapf:\n"
        "  --mip N     export mip level N\n");
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "info") == 0)  return cmd_info(argv[2]);
    if (argc == 3 && strcmp(argv[1], "bench") == 0) return cmd_bench(argv[2]);
    if ((argc == 4 || argc == 5) && strcmp(argv[1], "thumbnail") == 0)
        return cmd_thumbnail(argv[2], argv[3], argc == 5 ? atoi(argv[4]) : 256);
    if (argc < 3) { usage(); return 2; }

    const char *in = argv[1], *out = argv[2];
    int mips = 0, srgb = 1, premult = 0, mip_out = 0;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--mips") == 0) mips = 1;
        else if (strcmp(argv[i], "--linear") == 0) srgb = 0;
        else if (strcmp(argv[i], "--premult") == 0) premult = 1;
        else if (strcmp(argv[i], "--mip") == 0 && i + 1 < argc) mip_out = atoi(argv[++i]);
        else { usage(); return 2; }
    }

    /* Read */
    yapf_image_t  src;
    yapf_image_t *loaded = NULL;
    uint8_t      *stb_px = NULL;
    memset(&src, 0, sizeof(src));
    if (is_yapf(in)) {
        loaded = yapf_load_mt(in, 0);
        if (!loaded) { fprintf(stderr, "yapf: cannot read %s (missing, corrupt or not YAPF v1)\n", in); return 1; }
        src = *loaded;
    } else {
        int w, h, ch;
        if (stbi_is_16_bit(in))
            fprintf(stderr, "yapf: note: %s has 16-bit channels; YAPF stores 8 bits per channel\n", in);
        stb_px = stbi_load(in, &w, &h, &ch, 0);
        if (!stb_px) { fprintf(stderr, "yapf: cannot read %s: %s\n", in, stbi_failure_reason()); return 1; }
        src.width = (uint32_t)w; src.height = (uint32_t)h; src.channels = (uint8_t)ch;
        src.pixels = stb_px; src.mip_levels = 1;
        src.flags = srgb ? YAPF_FLAG_SRGB : 0;
        src.gpu_format = gpu_hint(ch, srgb);
    }

    /* Write */
    int rc = 0;
    if (is_yapf(out)) {
        yapf_image_t dst = src;
        if (!loaded) {
            if (premult) dst.flags |= YAPF_FLAG_PREMULT_ALPHA;
        }
        dst.mips = NULL;
        dst.mip_levels = 1;
        if (mips && build_mips(&dst) != 0) { fprintf(stderr, "yapf: out of memory\n"); rc = 1; }
        if (!rc && yapf_save(out, &dst) != YAPF_OK) { fprintf(stderr, "yapf: cannot write %s\n", out); rc = 1; }
        if (dst.mips) {
            for (int m = 1; m < dst.mip_levels; m++) free(dst.mips[m]);
            free(dst.mips);
        }
    } else {
        const uint8_t *px = src.pixels;
        uint32_t w = src.width, h = src.height;
        if (mip_out > 0) {
            if (!loaded || mip_out >= src.mip_levels) {
                fprintf(stderr, "yapf: %s has no mip level %d\n", in, mip_out);
                rc = 1;
            } else {
                px = src.mips[mip_out];
                w = src.width >> mip_out ? src.width >> mip_out : 1;
                h = src.height >> mip_out ? src.height >> mip_out : 1;
            }
        }
        if (!rc && !write_image(out, px, (int)w, (int)h, src.channels)) {
            fprintf(stderr, "yapf: cannot write %s\n", out);
            rc = 1;
        }
    }

    yapf_free(loaded);
    stbi_image_free(stb_px);
    return rc;
}
