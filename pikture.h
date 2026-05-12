/*
pikture.h
Native PIKTURE format encoder and decoder library.
Version: 100
License: MIT
Made by: Nexoniarz
*/

#ifndef PIKTURE_H
#define PIKTURE_H

#include <stdint.h>
#include <stddef.h>

/* Image data structure */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t channels; /* Always 4 (RGBA) */
    uint8_t* pixels;  /* Raw RGBA pixel array */
} pikture_t;

#ifdef __cplusplus
extern "C" {
#endif

/* * Loads a PIKTURE image from the specified file path.
 * Returns a pointer to a pikture_t struct, or NULL on failure.
 */
pikture_t* pikture_load(const char* filename);

/* * Saves a PIKTURE image to the specified file path.
 * Returns 1 on success, 0 on failure.
 */
int pikture_save(const char* filename, const pikture_t* img);

/* * Frees the memory allocated for the PIKTURE image.
 */
void pikture_free(pikture_t* img);

#ifdef __cplusplus
}
#endif

#endif /* PIKTURE_H */
