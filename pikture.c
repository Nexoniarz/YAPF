/*
 * pikture.c
 * Native PIKTURE format encoder and decoder library.
 * Version: 101
 * * Copyright 2026 Nexoniarz
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
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

pikture_t* pikture_load(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return NULL;

    uint8_t header[11];
    if (fread(header, 1, 11, f) != 11) {
        fclose(f);
        return NULL;
    }

    if (memcmp(header, PIKT_MAGIC, 4) != 0) {
        fclose(f);
        return NULL;
    }

    if (header[4] != PIKT_VERSION) {
        fclose(f);
        return NULL;
    }

    pikture_t* img = (pikture_t*)malloc(sizeof(pikture_t));
    if (!img) {
        fclose(f);
        return NULL;
    }

    img->endianness = header[10];

    if (img->endianness) {
        img->width = (header[5] << 8) | header[6];
        img->height = (header[7] << 8) | header[8];
    } else {
        img->width = (header[6] << 8) | header[5];
        img->height = (header[8] << 8) | header[7];
    }

    img->depth = header[9];
    img->channels = (img->depth + 7) / 8;
    if (img->channels == 0) img->channels = 1;

    size_t data_size = (size_t)img->width * img->height * img->channels;
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
                if (b1 == EOF) break;
                int8_t dg = (b0 & 0x3F) - 32;
                r = (uint8_t)(r + dg + ((b1 >> 4) & 0x0F) - 8);
                g = (uint8_t)(g + dg);
                b = (uint8_t)(b + dg + (b1 & 0x0F) - 8);
            } else if (match == 0xC0) {
                int run = (b0 & 0x3F) + 1;
                for (int i = 0; i < run && ptr < data_size; i++) {
                    img->pixels[ptr++] = r;
                    if (img->channels >= 2) img->pixels[ptr++] = (img->channels == 2) ? a : g;
                    if (img->channels >= 3) img->pixels[ptr++] = b;
                    if (img->channels >= 4) img->pixels[ptr++] = a;
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
            if (img->channels >= 2) img->pixels[ptr++] = (img->channels == 2) ? a : g;
            if (img->channels >= 3) img->pixels[ptr++] = b;
            if (img->channels >= 4) img->pixels[ptr++] = a;
        }
    }

    fclose(f);
    return img;
}

int pikture_save(const char* filename, const pikture_t* img) {
    if (!img || !img->pixels) return 0;

    FILE* f = fopen(filename, "wb");
    if (!f) return 0;

    uint8_t header[11];
    memcpy(header, PIKT_MAGIC, 4);
    header[4] = PIKT_VERSION;

    uint8_t current_endianness = system_is_be();
    if (current_endianness) {
        header[5] = (img->width >> 8) & 0xFF;
        header[6] = img->width & 0xFF;
        header[7] = (img->height >> 8) & 0xFF;
        header[8] = img->height & 0xFF;
    } else {
        header[5] = img->width & 0xFF;
        header[6] = (img->width >> 8) & 0xFF;
        header[7] = img->height & 0xFF;
        header[8] = (img->height >> 8) & 0xFF;
    }

    header[9] = img->depth ? img->depth : (img->channels * 8);
    header[10] = current_endianness;

    fwrite(header, 1, 11, f);

    pikt_pixel index[64] = {0};
    uint8_t prev_r = 0, prev_g = 0, prev_b = 0, prev_a = 255;
    int run = 0;

    size_t total_pixels = (size_t)img->width * img->height;
    size_t px_idx = 0;

#ifdef PIKT_USE_THREADS
#pragma omp parallel
#endif
    while (px_idx < total_pixels * img->channels) {
        uint8_t r = 0, g = 0, b = 0, a = 255;
        
        r = img->pixels[px_idx++];
        if (img->channels == 4) {
            g = img->pixels[px_idx++];
            b = img->pixels[px_idx++];
            a = img->pixels[px_idx++];
        } else if (img->channels == 3) {
            g = img->pixels[px_idx++];
            b = img->pixels[px_idx++];
        } else if (img->channels == 2) {
            g = r;
            b = r;
            a = img->pixels[px_idx++];
        } else if (img->channels == 1) {
            g = r;
            b = r;
        }

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
