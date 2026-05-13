/*
pikture.c
Native PIKTURE format encoder and decoder library.
Version: 101
License: MIT
Made by: Nexoniarz
*/

#include "pikture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PIKT_USE_SIMD
#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#include <arm_neon.h>
#endif
#endif

#ifdef PIKT_USE_THREADS
#include <omp.h>
#endif

#define PIKT_MAGIC "PIK!"
#define PIKT_VERSION 101
#define PIKT_HASH(r, g, b, a) (((r) * 3 + (g) * 5 + (b) * 7 + (a) * 11) % 64)

typedef struct {
    uint8_t r, g, b, a;
} pikt_pixel;

static uint8_t system_is_be() {
    uint16_t x = 1;
    return *((uint8_t*)&x) == 0;
}

static uint16_t read_u16_be(FILE* f) {
    uint8_t bytes[2];
    fread(bytes, 1, 2, f);
    return (bytes[0] << 8) | bytes[1];
}

static void write_u16_be(FILE* f, uint16_t value) {
    uint8_t bytes[2] = { (value >> 8) & 0xFF, value & 0xFF };
    fwrite(bytes, 1, 2, f);
}

pikture_t* pikture_load(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return NULL;

    char magic[4];
    fread(magic, 1, 4, f);
    if (memcmp(magic, PIKT_MAGIC, 4) != 0) {
        fclose(f);
        return NULL;
    }

    uint8_t version;
    fread(&version, 1, 1, f);
    if (version != PIKT_VERSION) {
        fclose(f);
        return NULL;
    }

    pikture_t* img = (pikture_t*)malloc(sizeof(pikture_t));
    if (!img) {
        fclose(f);
        return NULL;
    }

    img->width = read_u16_be(f);
    img->height = read_u16_be(f);
    
    uint8_t depth;
    fread(&depth, 1, 1, f);
    img->channels = 4;

    fread(&img->endianness, 1, 1, f);

    size_t data_size = (size_t)img->width * img->height * 4;
    img->pixels = (uint8_t*)malloc(data_size);
    if (!img->pixels) {
        free(img);
        fclose(f);
        return NULL;
    }

    pikt_pixel index[64] = {0};
    uint8_t r = 0, g = 0, b = 0, a = 255;
    
    size_t ptr = 0;

#ifdef PIKT_USE_THREADS
#pragma omp parallel
#endif
    while (ptr < data_size) {
        int b0 = fgetc(f);
        if (b0 == EOF) break;

        if (b0 == 0xFE) {
            r = fgetc(f);
            g = fgetc(f);
            b = fgetc(f);
        } else if (b0 == 0xFF) {
            r = fgetc(f);
            g = fgetc(f);
            b = fgetc(f);
            a = fgetc(f);
        } else {
            uint8_t match = b0 & 0xC0;
            if (match == 0x00) {
                pikt_pixel p = index[b0 & 0x3F];
                r = p.r; g = p.g; b = p.b; a = p.a;
            } else if (match == 0x40) {
                r = (uint8_t)(r + ((b0 >> 4) & 0x03) - 2);
                g = (uint8_t)(g + ((b0 >> 2) & 0x03) - 2);
                b = (uint8_t)(b + (b0 & 0x03) - 2);
            } else if (match == 0x80) {
                int b1 = fgetc(f);
                int8_t dg = (b0 & 0x3F) - 32;
                r = (uint8_t)(r + dg + ((b1 >> 4) & 0x0F) - 8);
                g = (uint8_t)(g + dg);
                b = (uint8_t)(b + dg + (b1 & 0x0F) - 8);
            } else if (match == 0xC0) {
                int run = (b0 & 0x3F) + 1;
                for (int i = 0; i < run && ptr < data_size; i++) {
                    img->pixels[ptr++] = r;
                    img->pixels[ptr++] = g;
                    img->pixels[ptr++] = b;
                    img->pixels[ptr++] = a;
                }
                continue;
            }
        }

        uint8_t idx_pos = PIKT_HASH(r, g, b, a);
        index[idx_pos].r = r;
        index[idx_pos].g = g;
        index[idx_pos].b = b;
        index[idx_pos].a = a;

        if (ptr < data_size) {
            img->pixels[ptr++] = r;
            img->pixels[ptr++] = g;
            img->pixels[ptr++] = b;
            img->pixels[ptr++] = a;
        }
    }

    fclose(f);
    return img;
}

int pikture_save(const char* filename, const pikture_t* img) {
    if (!img || !img->pixels) return 0;

    FILE* f = fopen(filename, "wb");
    if (!f) return 0;

    fwrite(PIKT_MAGIC, 1, 4, f);
    uint8_t version = PIKT_VERSION;
    fwrite(&version, 1, 1, f);
    write_u16_be(f, img->width);
    write_u16_be(f, img->height);
    uint8_t depth = 32;
    fwrite(&depth, 1, 1, f);

    uint8_t current_endianness = system_is_be();
    fwrite(&current_endianness, 1, 1, f);

    pikt_pixel index[64] = {0};
    uint8_t prev_r = 0, prev_g = 0, prev_b = 0, prev_a = 255;
    int run = 0;

    size_t total_pixels = (size_t)img->width * img->height;
    size_t px_idx = 0;

#ifdef PIKT_USE_THREADS
#pragma omp parallel
#endif
    while (px_idx < total_pixels * 4) {
        uint8_t r = img->pixels[px_idx++];
        uint8_t g = img->pixels[px_idx++];
        uint8_t b = img->pixels[px_idx++];
        uint8_t a = img->pixels[px_idx++];

        if (r == prev_r && g == prev_g && b == prev_b && a == prev_a) {
            run++;
            if (run == 62) {
                fputc(0xC0 | (run - 1), f);
                run = 0;
            }
            continue;
        }

        if (run > 0) {
            fputc(0xC0 | (run - 1), f);
            run = 0;
        }

        uint8_t idx_pos = PIKT_HASH(r, g, b, a);
        if (index[idx_pos].r == r && index[idx_pos].g == g && 
            index[idx_pos].b == b && index[idx_pos].a == a) {
            fputc(0x00 | idx_pos, f);
        } else {
            index[idx_pos].r = r;
            index[idx_pos].g = g;
            index[idx_pos].b = b;
            index[idx_pos].a = a;

            if (a == prev_a) {
                int8_t s_dr = (int8_t)(r - prev_r);
                int8_t s_dg = (int8_t)(g - prev_g);
                int8_t s_db = (int8_t)(b - prev_b);

                if (s_dr >= -2 && s_dr <= 1 && s_dg >= -2 && s_dg <= 1 && s_db >= -2 && s_db <= 1) {
                    fputc(0x40 | ((s_dr + 2) << 4) | ((s_dg + 2) << 2) | (s_db + 2), f);
                } else {
                    int8_t dr_dg = s_dr - s_dg;
                    int8_t db_dg = s_db - s_dg;

                    if (s_dg >= -32 && s_dg <= 31 && dr_dg >= -8 && dr_dg <= 7 && db_dg >= -8 && db_dg <= 7) {
                        fputc(0x80 | (s_dg + 32), f);
                        fputc(((dr_dg + 8) << 4) | (db_dg + 8), f);
                    } else {
                        fputc(0xFE, f);
                        fputc(r, f); fputc(g, f); fputc(b, f);
                    }
                }
            } else {
                fputc(0xFF, f);
                fputc(r, f); fputc(g, f); fputc(b, f); fputc(a, f);
            }
        }
        prev_r = r; prev_g = g; prev_b = b; prev_a = a;
    }

    if (run > 0) {
        fputc(0xC0 | (run - 1), f);
    }

    fclose(f);
    return 1;
}

void pikture_free(pikture_t* img) {
    if (img) {
        if (img->pixels) {
            free(img->pixels);
        }
        free(img);
    }
}
