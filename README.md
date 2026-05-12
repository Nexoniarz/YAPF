# PIKTURE (PIK)
**Fast. Small. Easy.**

PIKTURE is a high-performance, lossless image format designed for systems where decoding speed and binary simplicity are critical. It is built on the FSE philosophy: Fast execution, Small file size, and Easy integration.

## Why PIKTURE?
Standard formats like PNG are often bottlenecked by heavy decompression algorithms (DEFLATE/ZIP). PIKTURE replaces complex bit-shuffling with a hardware-friendly **INDEX + Delta** encoding, allowing the CPU to stream pixels directly to memory with minimal overhead.

### Key Features
- **Lossless:** 100% bit-perfect reproduction of RGBA data.
- **Ultra-Fast:** Near-memcpy decoding speeds.
- **Small Footprint:** Single-header C99 library with zero dependencies.
- **Smart Alpha:** Automatically optimizes storage when transparency is not used.

## Performance Comparison
Benchmark results for a 5K professional image (sample 5120x3840):

| Format | File Size | Difference | Decoding Complexity |
| :--- | :--- | :--- | :--- |
| **PNG** | 16.2 MB | Baseline | High (CPU Intensive) |
| **PIKTURE (PIK)** | **10.1 MB** | **- 6.1 MB (37%)** | **Minimal (Linear Stream)** |

## Implementation
The core library consists of only two files: `pikture.h` and `pikture.c`.

### Building
The project is designed to be cross-compiled using the **Zig** toolchain for maximum portability across Linux, Windows, and macOS.

```bash
# Run the build script to generate static and shared libraries
./build.sh | UNIX
./build.bat | WINDOWS
