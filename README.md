# YAPF — Yet Another Picture Format

**Lossless images that decode several times faster than PNG — on one core.**

YAPF is a lossless image format for photos, UI and game assets, built for
decode speed first.  There is no entropy coder, no bit reader and no context
model: every structure is byte aligned, and the decoder runs short loops that
modern CPUs execute at memory speed — with SSE / NEON where available.  The
encoder does the hard work, so the decoder only follows instructions.

- **5–12× faster decoding than PNG** (stb_image), single-threaded; a 4K image
  decodes in about 6–7 ms (around 5 GB/s).  Multithreaded decoding is optional.
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
| **C / C++** | `yapf.h` + `yapf.c`, drop into any project (compiles as C or C++) | [below](#c--c) | ✅ tested — see [platforms](#platforms) |
| **Command line** | convert PNG/JPG/TGA/BMP/PSD/GIF ↔ YAPF, `compare`, `info`, `bench`, `thumbnail` | [below](#command-line) | ✅ tested on Linux and Windows |
| **Your language** | Python, C#, Rust, Go, Java, Zig, Lua, JavaScript / TypeScript, … | [docs/LANGUAGES.md](docs/LANGUAGES.md) | ✅ every binding tested |
| **GIMP 3** | open, export (flattened, optional mipmaps) | [extensions/gimp](extensions/gimp) | ✅ tested in GIMP 3.0.8 |
| **Blender 4.2+** | import, export, drag & drop, image as plane | [extensions/blender](extensions/blender) | ✅ tested in Blender 5.1 |
| **KDE Dolphin** | thumbnails for `.yapf` files | [extensions/kde](extensions/kde) | ⚠️ tested through KDE's plugin loader (KF6); not yet inside a running Dolphin |
| **Linux desktop** | file type + thumbnails in GNOME Files, Nemo, Caja, Thunar | [extensions/linux](extensions/linux) | ⚠️ thumbnail command tested; not yet in a file manager |
| **VS Code** | image preview with zoom and mip picker | [extensions/vscode](extensions/vscode) | ⚠️ packaged; preview code tested in a browser engine (Chromium); **not yet run inside VS Code** |
| **Figma** | import `.yapf`, export selection as `.yapf` | [extensions/figma](extensions/figma) | ⚠️ plugin UI tested in a browser engine with a simulated Figma; **not yet run inside Figma** |
| **Photoshop** | File → Scripts → Open YAPF / Save as YAPF | [extensions/photoshop](extensions/photoshop) | ⚠️ **syntax-checked only; not yet run in Photoshop** |

Pre-built downloads (command-line tool and libraries for Windows, macOS and
Linux, plus the plugin packages) are published on the
[Releases](https://github.com/Nexoniarz/YAPF/releases) page; everything can
also be built from source as described below.

### Platforms

| Platform | Status |
| :------- | :----- |
| Linux x64 | ✅ built and fully tested |
| Windows x64 | ✅ built and fully tested (MinGW and Zig builds, run under Wine) |
| Linux ARM64 | ✅ built and fully tested (NEON, run under qemu) |
| Windows ARM64 | ⚠️ built only, not run |
| macOS x64 / ARM64 | ⚠️ **cross-compiled only, not yet run on a Mac** |

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
yapf compare photo.png ui.png         # size and decode speed vs the original format
```

Options when writing `.yapf`: `--mips`, `--linear` (pixels are not sRGB),
`--premult` (alpha is premultiplied).

---

## Benchmark

`yapf compare` on a mixed set of images: one binary (`make`, gcc -O2) for
both formats, PNG decoded with stb_image, one core, AMD Ryzen 7 5700G.
Every result is checked to decode back to the exact original pixels.

| Image | PNG | YAPF | YAPF / PNG size | PNG decode | YAPF decode | Faster |
| :---- | --: | ---: | --------------: | ---------: | ----------: | -----: |
| UI screenshot 261×430 | 176.6 KB | 143.7 KB | **81 %** | 2.05 ms | 0.17 ms | **12×** |
| UI screenshot 305×529 | 201.5 KB | 185.0 KB | **92 %** | 2.55 ms | 0.24 ms | **11×** |
| App icon 128×128 | 24.9 KB | 23.1 KB | **93 %** | 0.30 ms | 0.026 ms | **11×** |
| Wallpaper 3840×2160 | 1.77 MB | 2.21 MB | 125 % | 73.7 ms | 6.8 ms | **11×** |
| Rock texture 512×512 | 182.6 KB | 221.8 KB | 121 % | 1.64 ms | 0.17 ms | **10×** |
| Wood texture 512×512 | 185.8 KB | 234.1 KB | 126 % | 1.64 ms | 0.17 ms | **10×** |
| Canvas texture 512×512 | 171.5 KB | 245.6 KB | 143 % | 1.36 ms | 0.16 ms | **9×** |
| YAPF logo 512×512 | 60.5 KB | 89.3 KB | 148 % | 1.25 ms | 0.25 ms | **5×** |

Smaller than PNG on UI-style images, 20–50 % larger on photos and textures
(the price of having no entropy coder), and 5–12× faster to decode.  More
in [examples/](examples) — and run `yapf compare` on your own images.

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
int  yapf_encode(const yapf_image_t *img, void **out_data, size_t *out_size);
void yapf_free_buffer(void *data);
```
Encodes into memory: `*out_data` receives a complete `.yapf` file of
`*out_size` bytes, to be released with `yapf_free_buffer()`.

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
make test                      # C suite: 786 checks, plus JavaScript if node is installed
node bindings/js/test.js       # JavaScript: round-trips, sample file, corrupt input
```

What has been verified:

- exact round-trips for 1–4 channels, odd sizes from 1×1 up, mip chains,
  single- and multi-threaded decoding, in-memory and file encoding;
- the same tests pass on Linux x64, Windows x64 (run under Wine) and Linux
  ARM64 (NEON, run under qemu), with and without SIMD, compiled as C and as
  C++, and under AddressSanitizer, UndefinedBehaviorSanitizer and
  ThreadSanitizer;
- thousands of corrupted and truncated files are rejected without crashing;
- every language binding runs its example; JavaScript, Python, C++, C#,
  Rust, Go, Java and Lua re-encode the sample to byte-identical output;
- the GIMP plugin in GIMP 3.0.8 and the Blender extension in Blender 5.1;
- the KDE thumbnailer through KDE's own plugin loader.

**Not yet verified:** running on a Mac (macOS builds are cross-compiled
only), Windows on ARM, and running inside Photoshop, VS Code, Figma or a
live Dolphin / GNOME file manager.

---

## Project layout

```
yapf.h, yapf.c          the library (format spec in yapf.h)
tools/yapf_cli.c        the `yapf` command-line tool
tools/build_release.sh  builds every download for all platforms
tests/test_yapf.c       C test suite
examples/               speed and size comparison, C example
docs/LANGUAGES.md       using YAPF from other languages
bindings/               cpp, python, csharp, rust, go, java, zig, lua, js
extensions/gimp/        GIMP 3 plugin
extensions/blender/     Blender extension
extensions/kde/         Dolphin thumbnail plugin (KF6)
extensions/linux/       MIME type and freedesktop thumbnailer
extensions/vscode/      VS Code preview
extensions/figma/       Figma plugin
extensions/photoshop/   Photoshop scripts
third_party/            stb_image / stb_image_write (public domain, CLI only)
other/YAPF.YAPF         sample image (the YAPF logo)
```

---

## License

Apache License 2.0 — see [LICENSE](LICENSE).  `third_party/` contains
stb_image and stb_image_write by Sean Barrett (public domain / MIT).
