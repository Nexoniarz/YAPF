# Examples: how fast, how small

## Try it on your own images

```sh
make                                   # builds build/yapf
build/yapf compare photo.png ui.png    # any PNG / JPG / TGA / BMP
```

`yapf compare` encodes each image to YAPF in memory, checks that it decodes
back to exactly the same pixels, and times decoding the original format
against YAPF on one core and on all cores:

```
ui_reverb.png  (261 x 430, RGBA)
  raw     438.4 KB
  PNG     176.6 KB   40.3% of raw   decode    2.049 ms
  YAPF    143.7 KB   32.8% of raw   decode    0.170 ms   12.1x faster, 81% of the PNG size
  YAPF on all cores: decode 0.176 ms (2.5 GB/s), encode 8 ms, lossless: yes
```

## Results on a mixed set of images

AMD Ryzen 7 5700G, `make` (gcc -O2), PNG decoded with stb_image.  The test
images (a 4K wallpaper, UI screenshots, an app icon and seamless textures
from open-source projects) are not redistributed here; run `yapf compare`
on your own files to reproduce.

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

In short: **smaller than PNG on UI-style images, 20–50 % larger on photos
and textures, and 5–12× faster to decode** — always lossless.  The decoder
reaches 5–6 GB/s on large images on one core, and more with
`yapf_load_mt`.

## Code examples

| Language | Example |
| :------- | :------ |
| C | [`examples/c/example.c`](c/example.c) |
| C++ | [`bindings/cpp/example.cpp`](../bindings/cpp/example.cpp) |
| Python | [`bindings/python/example.py`](../bindings/python/example.py) |
| C# | [`bindings/csharp/Example`](../bindings/csharp/Example) |
| Rust | [`bindings/rust/examples/demo.rs`](../bindings/rust/examples/demo.rs) |
| Go | [`bindings/go/example`](../bindings/go/example) |
| Java | [`bindings/java/Example.java`](../bindings/java/Example.java) |
| Zig | [`bindings/zig/example.zig`](../bindings/zig/example.zig) |
| Lua | [`bindings/lua/example.lua`](../bindings/lua/example.lua) |
| JavaScript | [`bindings/js/example.mjs`](../bindings/js/example.mjs) |

Each one loads a `.yapf` file, prints its size and decode time, re-encodes
it (byte-identical to the file) and writes a new image from its own pixels.
See [docs/LANGUAGES.md](../docs/LANGUAGES.md) for setup in each language.
