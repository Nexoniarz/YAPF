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
- **A real texture format (YAPF 2)**: GPU-ready BC1–BC7 / BC6H / ETC2 /
  ASTC blocks, 16-bit and float pixels, cubemaps, arrays and volumes,
  channel meanings (normal X/Y, roughness, AO, …), coverage-preserving
  mips, small-mips-first streaming, per-tile checksums, notes and
  animation — see [Textures](#textures-yapf-2).
- **Tiny and portable**: two C99 files, no dependencies.  Plus a CLI, Python,
  JavaScript and plugins for the tools you already use.

Two format versions: **version 1** (an 8-bit image, the first stable
format) and **version 2** (the texture container).  Plain 8-bit images are
still written as version 1, so every existing YAPF reader keeps working.

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
`--premult` (alpha is premultiplied), and the texture options below.

---

## Textures (YAPF 2)

Version 2 turns YAPF into a texture container for engines, designed our own
way rather than copied from KTX2 or DDS (the design and the full
specification are in [docs/FORMAT.md](docs/FORMAT.md)):

| Need | YAPF 2 |
| :--- | :----- |
| **GPU-compressed payloads** | BC1, BC2, BC3, BC4, BC5, BC6H, BC7, ETC2 / EAC and ASTC 4×4–12×12 blocks, stored as the GPU takes them.  Built-in encoders for BC1, BC3, BC4, BC5, BC7 and BC6H; or bring blocks from your own encoder. |
| **More than 8 bits** | lossless 16-bit, half and float pixels, coded as byte planes with the same fast machinery |
| **Cubes, arrays, volumes** | cubemaps and cube arrays (reflection probes, skies), texture arrays (terrain layers), 3D textures (colour-grading LUTs) |
| **Channel meaning** | per channel: normal X/Y/Z, roughness, metalness, AO, height, opacity, …; OpenGL or DirectX normal convention; linear channels inside sRGB images |
| **Streaming** | small mips are stored first: a file prefix is a complete low-resolution texture (`yapf info` prints the prefix sizes); single 64 × 64 tiles decode on their own for virtual texturing |
| **Alpha-tested mips** | `--coverage REF` keeps fences and foliage from thinning out with distance; the filter used is recorded |
| **Entropy coding (optional)** | `--entropy`: shared per-surface Huffman code books with a neighbourhood context — about 10 % smaller photos, about 3× smaller GPU blocks on disk; tiles stay independent |
| **Metadata** | notes (author, licence, source, tool, ICC profile, orientation, DPI, HDR light levels, your own tags), colour primaries and transfer (sRGB, Display P3, BT.2020, PQ, HLG, …) |
| **Integrity** | yapf-sum checksums on the header, tables and every tile; *needs* bits make an older decoder refuse cleanly what it cannot read |
| **Animation** | frames with per-frame durations and a loop count |

```sh
yapf albedo.png albedo.yapf --format bc7 --mips               # GPU-ready colour
yapf normal.png normal.yapf --format bc5 --mips --normal dx   # normal map, DirectX Y
yapf orm.png orm.yapf --map ao,rough,metal --linear --mips    # packed masks
yapf foliage.png foliage.yapf --format bc7 --coverage 128     # alpha-tested mips
yapf sky.hdr sky.yapf --format bc6h --mips                    # HDR, no RGBM tricks
yapf sky.hdr sky_lossless.yapf                                # lossless half float
yapf pack cube px.png nx.png py.png ny.png pz.png nz.png probe.yapf --mips
yapf pack array grass.png rock.png sand.png terrain.yapf --format bc7 --mips
yapf pack volume lut_00.png lut_01.png ... grading.yapf --linear
yapf pack anim f0.png f1.png f2.png fx.yapf --frame-ms 50
yapf albedo.png albedo.yapf --entropy --author "Keel" --license CC-BY-4.0
yapf info albedo.yapf       # everything in the header, notes, stream prefix sizes
yapf verify albedo.yapf     # check every checksum
yapf probe.yapf face2.png --face 2 --mip 1    # export one surface
```

In C:

```c
int err;
yapf_texture_t *t = yapf_tex_load("albedo.yapf", 0, 0, &err);
for (uint32_t m = 0; m < t->mip_levels; m++) {
    uint32_t s = yapf_tex_surface(t, 0, 0, 0, m), w, h;
    yapf_tex_mip_size(t, m, &w, &h, NULL);
    if (t->payload == YAPF_PAYLOAD_BLOCKS)        /* e.g. YAPF_GPU_BC7 */
        glCompressedTexImage2D(GL_TEXTURE_2D, m, gl_format(t->format, t->flags),
                               w, h, 0, (GLsizei)yapf_tex_surface_bytes(t, m), t->data[s]);
}
yapf_tex_free(t);
```

Every extension opens version 2 files: GIMP (at full 16-bit / float
precision, faces and frames as layers), Blender (HDR as float images),
KDE and Linux thumbnails, VS Code (with face / layer / frame / slice
pickers), Figma and Photoshop.  The Python binding (`yapf.load_texture`)
and the JavaScript port (`YAPF.decodeTexture`) read every texture; the
other bindings wrap the v1 functions, which also read 8-bit version 2
files (2D, cube face 0, array layer 0).

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

### Textures (version 2)

```c
yapf_texture_t *yapf_tex_load(const char *file, int flags, int threads, int *err);
yapf_texture_t *yapf_tex_load_memory(const void *buf, size_t size, int flags, int threads, int *err);
int  yapf_tex_save(const char *file, const yapf_texture_t *t, int flags);
int  yapf_tex_encode(const yapf_texture_t *t, int flags, void **out, size_t *size);
yapf_texture_t *yapf_tex_create(const yapf_texture_t *desc);   /* zeroed storage */
void yapf_tex_free(yapf_texture_t *t);
```
A texture describes its shape (width, height, depth, layers, frames,
cube / volume, mips), storage (pixels with u8 / u16 / half / float samples,
or GPU blocks), meaning (channel map, linear mask, normal convention,
primaries, transfer, mip filter, alpha reference) and holds `data[s]` per
surface `s = yapf_tex_surface(t, frame, layer, face, mip)`.  Load flag
`YAPF_LOAD_TRUST` skips checksum checks; encode flag `YAPF_ENC_ENTROPY`
allows the entropy stage.

```c
yapf_texture_t *yapf_tex_open(const void *buf, size_t avail, int flags, int *err);
uint64_t yapf_tex_stream_bytes(const yapf_texture_t *t, uint32_t first_mip);
int  yapf_tex_decode_surface(yapf_texture_t *t, const void *buf, size_t avail, uint32_t s, int threads);
int  yapf_tex_decode_tile(const yapf_texture_t *t, const void *buf, size_t avail, uint32_t s,
                          uint32_t tx, uint32_t ty, uint32_t slice, void *out, size_t stride,
                          uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h);
```
Streaming and virtual texturing: open a file from its first bytes, ask how
many bytes hold mip *m* and smaller, decode surfaces as their bytes arrive
(`YAPF_ERR_TRUNCATED` until then), or decode single tiles.

```c
int yapf_tex_build_mips(yapf_texture_t *t, uint32_t levels, uint8_t alpha_ref);
int yapf_tex_compress(const yapf_texture_t *src, uint8_t format, yapf_texture_t **out);
int yapf_blocks_decode(uint8_t format, const void *blocks, uint32_t w, uint32_t h, void *out);
int yapf_tex_add_note(yapf_texture_t *t, const char tag[4], const void *data, uint32_t size);
const yapf_note_t *yapf_tex_find_note(const yapf_texture_t *t, const char tag[4]);
```
Mip chains (linear-light sRGB, optional coverage preservation), the
built-in BCn encoders, BC1–BC7 decoding for previews, and notes.  The
complete list, with sizes and helpers, is in [`yapf.h`](yapf.h).

### Return codes

| Constant           | Value | Meaning                              |
| :----------------- | ----: | :----------------------------------- |
| `YAPF_OK`          | 0     | Success                              |
| `YAPF_ERR_INVALID` | –1    | NULL or logically invalid argument   |
| `YAPF_ERR_IO`      | –2    | File could not be opened or written  |
| `YAPF_ERR_OOM`     | –3    | Out of memory                        |
| `YAPF_ERR_CORRUPT` | –4    | Corrupt or not a YAPF file           |
| `YAPF_ERR_UNSUPPORTED` | –5 | Needs a feature this decoder lacks  |
| `YAPF_ERR_CHECKSUM` | –6   | A checksum does not match (v2)       |
| `YAPF_ERR_TRUNCATED` | –7  | The data is not (yet) all there      |

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
| NixOS / Nix | `nix-build` (or `nix-env -if .`) — see [extensions/linux](extensions/linux#nixos) | `result/`: `bin/yapf`, `lib/`, `include/`, MIME type, thumbnailer |
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

The version 1 specification is the comment at the top of [`yapf.h`](yapf.h);
version 2 (textures) is specified in [docs/FORMAT.md](docs/FORMAT.md).  Version 1:

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
make test                      # C suite: 2631 checks, plus JavaScript if node is installed
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
- version 2: lossless round-trips of u8 / u16 / half / float pixels with 1–4
  channels, with and without entropy coding; cubes, cube arrays, arrays,
  volumes and animations with notes and every description field; GPU block
  payloads; checksum, needs-bit and truncation handling; streaming from file
  prefixes and single-tile decoding; BCn encoder quality; linear-light and
  coverage-preserving mips; fuzzing of entropy-coded and packed-block files;
- the BC1–BC7 decoders were checked against an independent reference decoder
  (bcdec) on 300,000 random blocks: BC6H and BC7 bit-exact, BC1–BC5 within
  the ±1 rounding the formats allow (a development check, not part of
  `make test`);
- every language binding runs its example; JavaScript, Python, C++, C#,
  Rust, Go, Java and Lua re-encode the sample to byte-identical output;
- the GIMP plugin in GIMP 3.0.8 and the Blender extension in Blender 5.1;
- the KDE thumbnailer through KDE's own plugin loader.

**Not yet verified:** running on a Mac (macOS builds are cross-compiled
only), Windows on ARM, and running inside Photoshop, VS Code, Figma or a
live Dolphin / GNOME file manager.  The version 2 code has been run on
Linux x64 (with and without SIMD, as C and C++, under ASan, UBSan and
TSan); its NEON path is compiled for ARM64 but has not been run yet, and
the Windows builds have not been rerun.  ETC2 / EAC / ASTC blocks are
stored and loaded, but there is no built-in encoder or preview decoder
for them.

---

## Project layout

```
yapf.h, yapf.c          the library (v1 spec in yapf.h)
docs/FORMAT.md          the full format specification, version 2
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
