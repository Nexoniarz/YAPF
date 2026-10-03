/*
 * example.c — load, inspect, time and write YAPF images from C.
 *
 *   cc -O2 -std=c99 -I../.. example.c ../../yapf.c -o example -pthread
 *   ./example ../../other/YAPF.YAPF
 */
#include "yapf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double seconds(void) { return (double)clock() / CLOCKS_PER_SEC; }

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "../../other/YAPF.YAPF";

    /* 1. Load a file (yapf_load_memory works the same on a buffer). */
    yapf_image_t *img = yapf_load(path);
    if (!img) { fprintf(stderr, "cannot load %s\n", path); return 1; }
    double t = 1e9;                       /* decode time: best of 10 */
    for (int i = 0; i < 10; i++) {
        double t0 = seconds();
        yapf_free(yapf_load(path));
        t0 = seconds() - t0;
        if (t0 < t) t = t0;
    }
    printf("%s: %u x %u, %u channels, %u mip level(s), decoded in %.2f ms\n",
           path, img->width, img->height, img->channels, img->mip_levels, t * 1e3);

    /* 2. Pixels: width * height * channels bytes, rows top to bottom. */
    const uint8_t *p = img->pixels;
    printf("top-left pixel: %u %u %u %u\n", p[0], p[1], p[2], img->channels == 4 ? p[3] : 255);

    /* 3. Encode to memory and to a file. */
    void  *data = NULL;
    size_t size = 0;
    if (yapf_encode(img, &data, &size) == YAPF_OK) {
        printf("re-encoded: %zu bytes (%.1f%% of raw)\n", size,
               100.0 * size / ((double)img->width * img->height * img->channels));
        yapf_free_buffer(data);
    }

    /* 4. Create an image from your own pixels. */
    enum { W = 256, H = 256 };
    static uint8_t rgba[W * H * 4];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *q = rgba + (y * W + x) * 4;
            q[0] = (uint8_t)x; q[1] = (uint8_t)y; q[2] = 128; q[3] = 255;
        }
    yapf_image_t mine;
    memset(&mine, 0, sizeof(mine));
    mine.width = W; mine.height = H; mine.channels = 4; mine.mip_levels = 1;
    mine.pixels = rgba; mine.flags = YAPF_FLAG_SRGB; mine.gpu_format = YAPF_GPU_SRGB8_A8;
    if (yapf_save("gradient.yapf", &mine) == YAPF_OK) printf("wrote gradient.yapf\n");

    yapf_free(img);
    return 0;
}
