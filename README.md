# PIKTURE (PIK)
**Fast. Small. Easy.**

PIKTURE is a high-performance, lossless image format designed for systems where decoding speed and binary simplicity are critical. It is built on the **FSE** philosophy: **F**ast execution, **S**mall file size, and **E**asy integration.

## 🚀 Why PIKTURE?
Standard formats like PNG are often bottlenecked by heavy decompression algorithms (DEFLATE/ZIP). PIKTURE replaces complex bit-shuffling with a hardware-friendly **INDEX + Delta** encoding. This allows the CPU to stream pixels directly to memory with minimal overhead, acting closer to a memory copy operation than a math-heavy decompression.

### ✨ Key Features
- **Lossless:** 100% bit-perfect reproduction of image data.
- **Dynamic Color Depth:** Full support for 1, 2, 4, 8, 16, 24, and 32-bit depths, natively handling Grayscale, Grayscale+Alpha, RGB, and RGBA formats without bloat.
- **Ultra-Fast:** Near-memcpy decoding speeds utilizing cache-friendly algorithms.
- **Hardware Acceleration:** Optional SIMD (AVX/NEON) and OpenMP multithreading support.
- **Small Footprint:** Two-file C library (`.c` / `.h`) with zero external dependencies.
- **Strict Endian-Awareness:** Active cross-architecture compatibility (Little-Endian / Big-Endian) built directly into the parser.

---

## 📊 Performance Comparison
Benchmark results for loading a 5K professional image (Sample dimensions: 5120x2880):

| Format | Load Time | Dimensions |
| :--- | :--- | :--- |
| **PIK** | **0.111735 sec** | 5120x2880 |
| PNG | 0.193990 sec | 5120x2880 |

*PIKTURE decodes nearly 2x faster than standard PNG implementations on the same hardware without SIMD or OpenMP.*

---

## 🛠️ Implementation & Usage

The core library consists of only two files:
* `pikture.h` - Contains the data structures (`pikture_t`) and public API declarations.
* `pikture.c` - Contains the core encoding/decoding logic, hashing, and byte-stream processing.

### Basic API Example
Integrating PIKTURE into your C/C++ project is trivial:

```c
#include "pikture.h"
#include <stdio.h>

int main() {
    // Load an image
    pikture_t* img = pikture_load("image.pik");
    if (!img) {
        printf("Failed to load image!\n");
        return 1;
    }

    printf("Loaded %dx%d image with %d channels.\n", img->width, img->height, img->channels);

    // Save an image
    pikture_save("output.pik", img);

    // Free memory
    pikture_free(img);
    return 0;
}

```

---

## ⚙️ Building & Compilation

The project is designed to be highly portable. You can compile it using standard GCC/Clang or cross-compile it using the **Zig** toolchain.

### Build Scripts

```bash
# Run the build script to generate static and shared libraries
./build.sh   # UNIX (macOS / Linux)
./build.bat  # WINDOWS

```

### Advanced Compilation Flags

You can enable hardware-specific optimizations by passing the following preprocessor directives during compilation:

* `-DPIKT_USE_SIMD` : Enables vectorized instructions (AVX/SSE for x86_64, NEON for ARM64) to process multiple pixels per clock cycle.
* `-DPIKT_USE_THREADS` : Enables OpenMP multithreading. Divides the decoding/encoding workload across multiple CPU cores (perfect for large 4K/8K images).

**Example compilation with optimizations (GCC):**

```bash
gcc -O3 -DPIKT_USE_SIMD -DPIKT_USE_THREADS -fopenmp main.c pikture.c -o myapp

```

---

## 🔬 Technical Specification

PIKTURE is a binary format. The 11-byte header is dynamically interpreted based on the Endianness flag to ensure universal portability.

### 1. File Header (11 Bytes)

| Offset | Size | Type | Description |
| --- | --- | --- | --- |
| `0x00` | 4 bytes | `char[4]` | Magic bytes: `"PIK!"` |
| `0x04` | 1 byte | `uint8_t` | Version number (Currently `101`) |
| `0x05` | 2 bytes | `uint16_t` | Image Width |
| `0x07` | 2 bytes | `uint16_t` | Image Height |
| `0x09` | 1 byte | `uint8_t` | Bit Depth (Supports 1/2/4/8/16/24/32-bit depths) |
| `0x0A` | 1 byte | `uint8_t` | Endianness flag (`0` = Little, `1` = Big). Dictates Width/Height reading logic. |

### 2. Byte-Stream Protocol

Immediately following the header is the compressed pixel data payload. PIKTURE uses a custom command-byte protocol combined with a 64-color Least Recently Used (LRU) cache:

* `0x00 - 0x3F` (**INDEX**): Fetch the exact pixel from the 64-color LRU cache.
* `0x40 - 0x7F` (**DIFF**): Apply small RGB deltas (differences from the previous pixel).
* `0x80 - 0xBF` (**LUMA**): Apply green-relative color deltas (highly efficient for gradients).
* `0xC0 - 0xFD` (**RUN**): Repeat the exact same previous pixel `N` times (Run-Length Encoding).
* `0xFE` (**RGB**): Full 24-bit color change (Alpha remains unchanged). Followed by 3 bytes (R, G, B).
* `0xFF` (**RGBA**): Full 32-bit color change. Followed by 4 bytes (R, G, B, A).

*Note: For depths lower than 32-bit (e.g., Grayscale or 24-bit RGB), the decoder dynamically maps the byte-stream to the correct channel layout.*

---

## 📜 License

This project is licensed under the **MIT License**. Free to use, modify, and distribute for personal and commercial purposes.
