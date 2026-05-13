/*
pikture.h
Native PIKTURE format encoder and decoder library.
Version: 101
License: MIT
Made by: Nexoniarz
*/

#ifndef PIKTURE_H
#define PIKTURE_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t channels;
    uint8_t endianness;
    uint8_t* pixels;
} pikture_t;

#ifdef __cplusplus
extern "C" {
#endif

pikture_t* pikture_load(const char* filename);

int pikture_save(const char* filename, const pikture_t* img);

void pikture_free(pikture_t* img);

#ifdef __cplusplus
}
#endif

#endif
