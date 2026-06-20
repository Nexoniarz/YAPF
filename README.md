# YAPF — Yet Another Picture Format

**Fast. Small. Easy.**

YAPF is a lossless image codec built around a single principle: decoding should
be as close to a memory copy as possible.  Instead of running a general-purpose
compressor like DEFLATE over pixel data, YAPF encodes images with five cheap
operations — a pixel cache lookup, two flavours of delta encoding, run-length
encoding, and verbatim pixel storage — producing files that are smaller than
uncompressed formats while decoding nearly as fast as `memcpy`.

---

## Why YAPF?

Standard lossless formats like PNG compress well, but decompressing DEFLATE
streams is CPU-intensive.  On large images (4K and above) or in
latency-sensitive applications (game engines, real-time pipelines, embedded
systems) this is a meaningful bottleneck.

YAPF trades a modest compression ratio advantage for dramatically lower decode
complexity.  There are no Huffman tables to build, no sliding windows to
maintain, and no bit-level unpacking.  The decoder is a tight byte-dispatch
loop that any modern CPU handles at memory bandwidth.

### Performance

Benchmark: loading a 5K professional photograph (5120 × 2880 px).

| Format | Load time   |
| :----- | ----------: |
| **YAPF** | **0.111 s** |
| PNG    | 0.194 s     |

YAPF decodes approximately **1.7× faster** than PNG on the same hardware
without SIMD or threading.

### When YAPF excels

- **Pixel art, sprites, UI textures** — large flat regions of identical colour
  compress to single RUN bytes; palette-like images benefit heavily from the
  cache.
- **Screenshots and diagrams** — solid backgrounds and repeated gradients hit
  DIFF/LUMA and the cache constantly.
- **Game asset pipelines** — load textures at startup without paying a
  decompression tax every frame.
- **Embedded / resource-constrained targets** — the entire codec fits in ~200
  lines of portable C with zero external dependencies.

---

## Integration

The core library is two files.  Drop them into your project and compile:

```
yapf.h    — public API and format documentation
yapf.c    — encoder / decoder implementation
```

```c
#include "yapf.h"

int main(void) {
    /* Load */
    yapf_image_t *img = yapf_load("photo.yapf");
    if (!img) { /* file not found or not a valid YAPF stream */ }

    printf("%u x %u, %u channel(s)\n",
           img->width, img->height, img->channels);

    /* Save */
    int rc = yapf_save("output.yapf", img);
    if (rc != YAPF_OK) { /* write error */ }

    yapf_free(img);
    return 0;
}
```

Compile with any C99-capable compiler — no flags required for the baseline:

```sh
gcc -O2 -std=c99 main.c yapf.c -o myapp
```

---

## API Reference

### Types

```c
typedef struct {
    uint32_t  width;     /* pixels per row                             */
    uint32_t  height;    /* rows                                       */
    uint8_t   channels;  /* 1 gray / 2 gray+alpha / 3 RGB / 4 RGBA    */
    uint8_t  *pixels;    /* raw pixel buffer, malloc-owned             */
} yapf_image_t;
```

`pixels` is a tightly packed, row-major byte array of `width × height × channels`
bytes.  The layout per pixel matches the `channels` value:

| `channels` | Layout per pixel |
| :--------- | :--------------- |
| 1          | `Y`              |
| 2          | `Y A`            |
| 3          | `R G B`          |
| 4          | `R G B A`        |

### Functions

```c
yapf_image_t *yapf_load(const char *filename);
```
Decodes a YAPF file from disk.  Returns a heap-allocated `yapf_image_t` on
success, `NULL` on any failure (file not found, invalid magic, unsupported
version, out of memory).  Free with `yapf_free()`.

```c
int yapf_save(const char *filename, const yapf_image_t *img);
```
Encodes an image to disk.  Returns `YAPF_OK` (0) on success or a negative
`YAPF_ERR_*` code on failure.

```c
void yapf_free(yapf_image_t *img);
```
Releases memory returned by `yapf_load()`.  Passing `NULL` is safe.

### Return codes

| Constant          | Value | Meaning                              |
| :---------------- | ----: | :----------------------------------- |
| `YAPF_OK`         | 0     | Success                              |
| `YAPF_ERR_INVALID`| –1    | NULL or logically invalid argument   |
| `YAPF_ERR_IO`     | –2    | File could not be opened or written  |

### Channel constants

```c
YAPF_CHANNELS_GRAY        1
YAPF_CHANNELS_GRAY_ALPHA  2
YAPF_CHANNELS_RGB         3
YAPF_CHANNELS_RGBA        4
```

---

## Building the libraries

The build scripts uses the [LLVM](https://releases.llvm.org/) compiler

```sh
# Unix (Linux / macOS)
chmod +x build.sh
./build.sh

# Windows
build.bat
```

Output goes to `dist/` with one subdirectory per platform:

```
dist/
├── windows-x64/    yapf.dll  yapf.lib
├── windows-arm64/  yapf.dll  yapf.lib
├── macos-x64/      libyapf.dylib
├── macos-arm64/    libyapf.dylib
├── linux-x64/      libyapf.so  libyapf.a
└── linux-arm64/    libyapf.so  libyapf.a
```

### Optional build flags

| Flag              | Effect                                              |
| :---------------- | :-------------------------------------------------- |
| `-DYAPF_USE_SIMD` | Reserved for future SIMD acceleration paths         |

The interactive build script will prompt for these options.

---

## Technical Specification

### File header — 14 bytes

| Offset | Size | Type       | Description                             |
| :----- | ---: | :--------- | :-------------------------------------- |
| 0x00   | 4    | `char[4]`  | Magic: `"YAPF"`                         |
| 0x04   | 1    | `uint8_t`  | Format version (currently `1`)          |
| 0x05   | 4    | `uint32_t` | Width in pixels, little-endian          |
| 0x09   | 4    | `uint32_t` | Height in pixels, little-endian         |
| 0x0D   | 1    | `uint8_t`  | Channel count: 1 / 2 / 3 / 4           |

All multi-byte integers are always stored little-endian, regardless of host
architecture.

### Byte-stream opcodes

The payload begins immediately after the header and is decoded left-to-right.
A 64-slot pixel cache (zero-initialised) and a running "previous pixel" state
(`r=0, g=0, b=0, a=255`) are maintained throughout.

| Byte range  | Opcode | Size    | Description                                   |
| :---------- | :----- | ------: | :-------------------------------------------- |
| 0x00–0x3F   | INDEX  | 1 byte  | Restore pixel from cache slot `byte & 0x3F`  |
| 0x40–0x7F   | DIFF   | 1 byte  | Per-channel deltas: dr = bits[5:4]–2, dg = bits[3:2]–2, db = bits[1:0]–2. Range –2..+1. Alpha unchanged. |
| 0x80–0xBF   | LUMA   | 2 bytes | Green delta dg = (byte & 0x3F)–32. Second byte: dr = ((b1>>4)&0xF)–8+dg, db = (b1&0xF)–8+dg. Alpha unchanged. |
| 0xC0–0xFD   | RUN    | 1 byte  | Repeat previous pixel (byte&0x3F)+1 times (1–62). Cache not updated. |
| 0xFE        | RGB    | 4 bytes | Next 3 bytes = R, G, B. Alpha unchanged.      |
| 0xFF        | RGBA   | 5 bytes | Next 4 bytes = R, G, B, A.                   |

After every non-RUN opcode, the cache slot `HASH(r,g,b,a)` is updated and the
pixel becomes the new previous pixel.

```
HASH(r,g,b,a) = (r×3 + g×5 + b×7 + a×11) mod 64
```

The stream ends when exactly `width × height` pixels have been decoded; no
end-of-stream marker is required.

### Pixel buffer layout

Within the decoded buffer, each pixel occupies `channels` consecutive bytes in
the order shown in the API table above.  Grayscale images store a single
luminance byte per pixel; the encoder internally promotes gray values to
`r = g = b = Y` for uniform opcode selection.

---

## GIMP Plugin

A plugin for GIMP is located in `extensions/gimp/`.  See the
[build instructions](extensions/gimp/howtocompile.txt) for details.

Supported colour modes: Gray, Gray+Alpha, RGB, RGBA.  
File extension: `.yapf`.  MIME type: `image/x-yapf`.

---

## Platforms

| Platform       | Architectures | Library format      |
| :------------- | :------------ | :------------------ |
| Windows        | x64, ARM64    | `.dll` + `.lib`     |
| macOS          | ARM64         | `.dylib`            |
| Linux (glibc)  | x64, ARM64    | `.so`               |
| Linux (musl)   | x64, ARM64    | `.a` (static)       |

The core library (`yapf.c` / `yapf.h`) uses only the C99 standard library
(`<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<stdint.h>`) and compiles cleanly
on any conforming C99 or C++ toolchain with no configuration required.

---

## License

Apache License 2.0 — see [LICENSE](LICENSE).
