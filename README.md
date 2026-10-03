# YAPF — Yet Another Picture Format

**Lossless images that decode several times faster than PNG — on one core.**

YAPF is a lossless image format for photos, UI and game assets, built for
decode speed first.  There is no entropy coder, no bit reader and no context
model: every structure is byte aligned, and the decoder runs short loops that
modern CPUs execute at memory speed — with SSE / NEON where available.  The
encoder does the hard work, so the decoder only follows instructions.

- **6–10× faster decoding than PNG** (stb_image), single-threaded; a 4K image
  decodes in about 6 ms (5.6 GB/s).  Multithreaded decoding is optional.
- **Smaller than PNG** on UI, icons and screenshots; 20–45 % larger on photos
  and natural textures — the price of having no entropy coder.
- **Made for games and tools too**: mipmaps, GPU format hint, sRGB /
  premultiplied flags, and random access to 64×64 tiles.
- **Tiny and portable**: two C99 files, no dependencies.  Plus a CLI, Python,
  JavaScript and plugins for the tools you already use.

This is **YAPF version 1**, the first stable format.

---

## Use it in…

| Where | What you get | Guide | Status |
| :---- | :----------- | :---- | :----- |
| **C / C++** | `yapf.h` + `yapf.c`, drop into any project | [below](#c--c) | ✅ tested on Linux x64, Windows x64, Linux ARM64 |
| **Command line** | convert PNG/JPG/TGA/BMP/PSD/GIF ↔ YAPF, `info`, `bench`, `thumbnail` | [below](#command-line) | ✅ tested on Linux, Windows |
| **GIMP 3** | open, export (with mipmaps) | [extensions/gimp](extensions/gimp) | ✅ tested in GIMP 3.0.8 |
| **Blender 4.2+** | import, export, drag & drop, image as plane | [extensions/blender](extensions/blender) | ✅ tested in Blender 5.1 |
| **VS Code** | image preview with zoom and mip picker | [extensions/vscode](extensions/vscode) | ✅ packaged, preview tested in Chromium |
| **Figma** | import `.yapf`, export selection as `.yapf` | [extensions/figma](extensions/figma) | ✅ plugin UI tested in Chromium |
| **Photoshop** | File → Scripts → Open YAPF / Save as YAPF | [extensions/photoshop](extensions/photoshop) | ⚠️ not yet tested inside Photoshop |
| **Linux desktop** | file type + thumbnails in GNOME Files, Nemo, Caja, Thunar | [extensions/linux](extensions/linux) | ⚠️ thumbnailer tested, not in a file manager |
| **Python** | `yapf.load()` / `yapf.save()` | [bindings/python](bindings/python) | ✅ tested |
| **JavaScript** | browsers, Node.js, workers | [bindings/js](bindings/js) | ✅ tested, byte-identical to C |

Pre-built downloads (command-line tool and libraries for Windows, macOS and
Linux, plus the plugin packages) are published on the
[Releases](https://github.com/Nexoniarz/YAPF/releases) page; everything can
also be built from source as described below.

---

## Command line

```sh
yapf photo.png photo.yapf             # encode (formats by file extension)
yapf texture.png texture.yapf --mips  # with a full mip chain
yapf photo.yapf photo.png             # decode to PNG / JPG / BMP / TGA
yapf texture.yapf mip2.png --mip 2    # export one mip level
yapf info photo.yapf                  # size, channels, mips, flags, ratio
yapf bench photo.yapf                 # decode speed, 1 thread and all cores
yapf thumbnail photo.yapf thumb.png 256
```

Options when writing `.yapf`: `--mips`, `--linear` (pixels are not sRGB),
`--premult` (alpha is premultiplied).

---

## Benchmark

Single core, AMD Ryzen 7 5700G, `gcc -O2`.  PNG is decoded with `stb_image`;
the PNG files are the ones shipped with the respective projects.  "Plain C"
is the library built with `-DYAPF_NO_SIMD`; the default build uses SSE / NEON.

| Image                  | Raw     | PNG    | **YAPF** | vs PNG | PNG decode | YAPF, plain C | **YAPF (default)** |
| :--------------------- | ------: | -----: | -------: | -----: | ---------: | ------------: | -----------------: |
| Wallpaper 3840×2160    | 32400 K | 1812 K | 2267 K   | 125 %  | 44.7 ms¹   | 10.3 ms       | **5.9 ms** (5.6 GB/s) |
| UI screenshot 261×430  | 438 K   | 176 K  | 143 K    | 81 %   | 1.6 ms     | 0.31 ms       | **0.17 ms**        |
| UI screenshot 305×529  | 630 K   | 201 K  | 185 K    | 92 %   | 2.1 ms     | 0.45 ms       | **0.24 ms**        |
| App icon 128×128       | 64 K    | 24 K   | 23 K     | 93 %   | 0.26 ms    | 0.06 ms       | **0.03 ms**        |
| Rock texture 512×512   | 768 K   | 182 K  | 221 K    | 121 %  | 1.4 ms     | 0.35 ms       | **0.16 ms**        |
| Wood texture 512×512   | 768 K   | 185 K  | 234 K    | 126 %  | 1.4 ms     | 0.46 ms       | **0.16 ms**        |
| Canvas texture 512×512 | 768 K   | 171 K  | 245 K    | 143 %  | 1.0 ms     | 0.35 ms       | **0.15 ms**        |
| Pixel art 512×512      | 1024 K  | —      | 66 K     | —      | —          | 0.74 ms       | **0.32 ms**        |
| Random noise 512×512   | 1024 K  | —      | 1024 K   | —      | —          | 0.1 ms        | 0.1 ms             |

¹ PNG decode of the wallpaper measured between 44.7 and 66 ms depending on
the harness; the table uses the fastest.  `-O2` and `-O3` builds of YAPF
perform the same.

---

## C / C++

The library is two files.  Add them to your project:

```
yapf.h    — public API and the complete format specification
yapf.c    — encoder and decoder
```

```c
#include "yapf.h"

yapf_image_t *img = yapf_load("texture.yapf");      /* or yapf_load_memory(buf, len) */
if (img) {
    /* img->pixels: width × height × channels bytes, rows top to bottom */
    upload_texture(img->width, img->height, img->channels, img->pixels);
    yapf_free(img);
}

yapf_image_t out = { .width = w, .height = h, .channels = 4,
                     .mip_levels = 1, .pixels = rgba, .flags = YAPF_FLAG_SRGB };
yapf_save("out.yapf", &out);
```

Compile with any C99 compiler (`-pthread` on Unix for the `_mt` loaders):

```sh
cc -O2 -std=c99 main.c yapf.c -o app -pthread
```

SIMD is on by default: SSSE3 on x86 / x64 (chosen at run time with CPUID,
so binaries still run on older CPUs) and NEON on ARM64, with GCC, Clang,
MSVC and MinGW.

| Define              | Effect |
| :------------------ | :----- |
| `-DYAPF_NO_SIMD`    | plain C only |
| `-DYAPF_NO_THREADS` | no threads; the `_mt` loaders decode on the calling thread, and only the C standard library is needed |

## API Reference

### Types

```c
typedef struct {
    uint32_t  width;       /* pixels per row                             */
    uint32_t  height;      /* rows                                       */
    uint8_t   channels;    /* 1 gray / 2 gray+alpha / 3 RGB / 4 RGBA     */
    uint8_t   gpu_format;  /* YAPF_GPU_* hint                            */
    uint8_t   flags;       /* YAPF_FLAG_* (premultiplied alpha, sRGB)    */
    uint8_t   mip_levels;  /* 1 = base only, up to 16                    */
    uint8_t  *pixels;      /* base level, malloc-owned                   */
    uint8_t **mips;        /* mips[0] == pixels, mips[1..] smaller levels */
} yapf_image_t;
```

`pixels` is a tightly packed, row-major byte array of `width × height × channels`
bytes.  Mip level *m* is `max(1, width >> m) × max(1, height >> m)`.  The
layout per pixel matches the `channels` value:

| `channels` | Layout per pixel |
| :--------- | :--------------- |
| 1          | `Y`              |
| 2          | `Y A`            |
| 3          | `R G B`          |
| 4          | `R G B A`        |

### Functions

```c
yapf_image_t *yapf_load(const char *filename);
yapf_image_t *yapf_load_memory(const void *buffer, size_t size);
```
Decode a YAPF file from disk or from memory (e.g. a game archive).  Return a
heap-allocated image on success, `NULL` on any failure (file not found,
invalid or corrupt data, out of memory).  Corrupt input is rejected, never
read out of bounds.

```c
yapf_image_t *yapf_load_mt(const char *filename, int threads);
yapf_image_t *yapf_load_memory_mt(const void *buffer, size_t size, int threads);
```
Same, decoding tiles on up to `threads` threads (`0` = one per CPU core).
The output is identical to the single-threaded loaders.

```c
int yapf_save(const char *filename, const yapf_image_t *img);
```
Encodes an image to disk.  Writes `img->mip_levels`
levels; `img->mips` may be `NULL` when `mip_levels == 1`.  Returns `YAPF_OK`
(0) on success or a negative `YAPF_ERR_*` code on failure.

```c
void yapf_free(yapf_image_t *img);
```
Releases memory returned by the loaders.  Passing `NULL` is safe.

### Return codes

| Constant           | Value | Meaning                              |
| :----------------- | ----: | :----------------------------------- |
| `YAPF_OK`          | 0     | Success                              |
| `YAPF_ERR_INVALID` | –1    | NULL or logically invalid argument   |
| `YAPF_ERR_IO`      | –2    | File could not be opened or written  |
| `YAPF_ERR_OOM`     | –3    | Out of memory                        |

### Channel constants

```c
YAPF_CHANNELS_GRAY        1
YAPF_CHANNELS_GRAY_ALPHA  2
YAPF_CHANNELS_RGB         3
YAPF_CHANNELS_RGBA        4
```

---

## Building

| System | Command | Output |
| :----- | :------ | :----- |
| Linux, macOS, BSD | `make` · `make test` · `sudo make install` | `build/`: `libyapf.a`, `libyapf.so` / `.dylib`, `yapf` |
| Linux, macOS (LLVM) | `./build.sh` | `dist/<platform>/` incl. macOS universal binaries |
| Windows (LLVM) | `build.bat` | `dist\windows-x64\`, `dist\windows-arm64\`: `yapf.dll`, `yapf.lib`, `libyapf.a`, `yapf.exe` |
| Windows (Visual Studio) | `cl /O2 /c yapf.c` and add `yapf.obj` to your project | |
| All platforms at once | `tools/build_release.sh` (needs [zig](https://ziglang.org)) | `dist/release/`: every download on the Releases page |

The command-line tool is `tools/yapf_cli.c` + `yapf.c`; it uses the public
domain [stb](https://github.com/nothings/stb) headers in `third_party/`.

---

## How it works

1. **Two colour transforms** — per 64×64 tile, YCoCg-R or subtract-green
   (`G, R−G, B−G`), whichever codes smaller.  Both run mod 256, so every
   plane stays one byte per sample.
2. **Twelve row filters** — per band of 8 rows: Left, Gradient (`L + A − D`),
   Up-right, Up-average, or the row 1–8 rows above (catches repeating
   textures and dither patterns).  None depends on the previous sample by
   more than one add, and the vertical ones decode as plain vector code.
3. **Groups of 8** — residuals are packed in groups of 8 at the group's bit
   width: 8 values × *w* bits = exactly *w* bytes, so every group is byte
   aligned and unpacks with one 64-bit load (or one SIMD shuffle).
4. **4-bit control codes** — one per group, or one per run of up to 64 groups
   repeating the previous width; the encoder picks the cheapest sequence with
   a small dynamic program.
5. **Validate first, then run** — all sizes in a tile are checked before the
   hot loops start, so the loops need no bounds checks.  Corrupt files are
   rejected, never read out of bounds.
6. **Constant and stored tiles** — a plane that is constant in a tile costs
   one byte; a tile that would not shrink is stored raw (noise never grows).
7. **Independent tiles** — any tile can be decoded on its own, in any order,
   on any thread.

The complete specification is the comment at the top of [`yapf.h`](yapf.h):

```
HEADER (20 bytes)   "YAPF", version 1, channels, GPU hint, mip count,
                    flags, 3 reserved, width u32, height u32
MIP INDEX           per level: offset u32, size u32
LEVEL DATA          tile_size u32 × tile_count, then the tiles
TILE (64×64)        header byte: stored raw, or constant-plane mask + colour transform
                    per plane: filter byte per 8 rows, control nibbles,
                               w bytes per group of 8 values
```

---

## Testing

```sh
make test                      # C suite: 784 checks, plus JavaScript if node is installed
node bindings/js/test.js       # JavaScript: round-trips, sample file, corrupt input
```

What has been verified for this release:

- exact round-trips for 1–4 channels, odd sizes from 1×1 up, mip chains,
  single- and multi-threaded decoding;
- the same tests pass on **Linux x64**, **Windows x64** (MinGW, run under
  Wine), **Linux ARM64** (NEON, run under qemu), with and without SIMD, and
  under AddressSanitizer, UndefinedBehaviorSanitizer and ThreadSanitizer;
- thousands of corrupted and truncated files are rejected without crashing;
- the JavaScript and Python versions produce byte-identical files to C;
- the GIMP plugin and the Blender extension in the real applications.

The macOS builds are cross-compiled and not yet run on a Mac.

---

## Project layout

```
yapf.h, yapf.c          the library (format spec in yapf.h)
tools/yapf_cli.c        the `yapf` command-line tool
tools/build_release.sh  builds every download for all platforms
tests/test_yapf.c       C test suite
bindings/js/            JavaScript encoder / decoder
bindings/python/        Python module
extensions/gimp/        GIMP 3 plugin
extensions/blender/     Blender extension
extensions/vscode/      VS Code preview
extensions/figma/       Figma plugin
extensions/photoshop/   Photoshop scripts
extensions/linux/       MIME type and thumbnailer
third_party/            stb_image / stb_image_write (public domain, CLI only)
other/YAPF.YAPF         sample image (the YAPF logo)
```

---

## License

Apache License 2.0 — see [LICENSE](LICENSE).  `third_party/` contains
stb_image and stb_image_write by Sean Barrett (public domain / MIT).
