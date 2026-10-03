# Using YAPF from your language

YAPF is a small C library with six functions, so every language that can
call C can use it.  Ready-made bindings, each with a runnable example:

| Language | Folder | How it works | Tested | Decode¹ |
| :------- | :----- | :----------- | :----- | ------: |
| **C** | [`yapf.h`](../yapf.h), [`examples/c`](../examples/c) | the library itself | ✅ | 0.26 ms |
| **C++** | [`bindings/cpp`](../bindings/cpp) | header-only RAII wrapper (C++17) | ✅ g++ | 0.25 ms |
| **Python** | [`bindings/python`](../bindings/python) | ctypes; numpy fallback without the library | ✅ CPython 3.13 | 0.23 ms |
| **C# / .NET** | [`bindings/csharp`](../bindings/csharp) | P/Invoke, single file | ✅ .NET 8 | 0.30 ms |
| **Rust** | [`bindings/rust`](../bindings/rust) | crate, builds the C code with `cc` | ✅ Rust 1.95 | 0.28 ms |
| **Go** | [`bindings/go`](../bindings/go) | cgo package | ✅ Go 1.26 | 0.31 ms |
| **Java / Kotlin** | [`bindings/java`](../bindings/java) | Foreign Function & Memory API (Java 22+) | ✅ JDK 25 | 0.42 ms |
| **Zig** | [`bindings/zig`](../bindings/zig) | `@cImport` of `yapf.h` | ✅ Zig 0.16 | 0.28 ms |
| **Lua** | [`bindings/lua`](../bindings/lua) | LuaJIT FFI (also LÖVE) | ✅ LuaJIT 2.1 | 0.28 ms |
| **JavaScript / TypeScript** | [`bindings/js`](../bindings/js) | pure JS port, `.d.ts` types | ✅ Node 24, Chromium | 5.9 ms |

¹ Decoding the 512×512 RGBA YAPF logo (`other/YAPF.YAPF`), best of 10 after
a warm-up, on an AMD Ryzen 7 5700G.  Every binding except JavaScript runs
the same native code, so they are all about equally fast; the JavaScript
port needs no native library at all.  Every example also re-encodes the
image and checks that the result is byte-identical to the original file.

## Getting the native library

Bindings other than C, C++, Rust, Go, Zig and JavaScript load the shared
library at run time:

| System  | File            | Get it |
| :------ | :-------------- | :----- |
| Windows | `yapf.dll`      | [Releases](https://github.com/Nexoniarz/YAPF/releases) or `build.bat` |
| Linux   | `libyapf.so`    | Releases, `make`, or `sudo make install` |
| macOS   | `libyapf.dylib` | Releases or `make` |

Put it next to your program, on the library path (`PATH`,
`LD_LIBRARY_PATH`, `DYLD_LIBRARY_PATH`), or pass its path explicitly
(`YAPF_LIBRARY` for Python and Lua, `-Dyapf.library=` for Java).

## Quick look

**C**
```c
yapf_image_t *img = yapf_load("texture.yapf");
/* img->width, img->height, img->channels, img->pixels */
yapf_free(img);
```

**C++**
```cpp
auto img = yapf::Image::load("texture.yapf");
auto bytes = img.encode();
```

**Python**
```python
img = yapf.load("texture.yapf")
yapf.save("copy.yapf", img)
```

**C#**
```csharp
var img = YapfImage.Load("texture.yapf");
File.WriteAllBytes("copy.yapf", img.Encode());
```

**Rust**
```rust
let img = yapf::Image::load("texture.yapf")?;
img.save("copy.yapf")?;
```

**Go**
```go
img, err := yapf.Load("texture.yapf")
data, err := yapf.Encode(img)
```

**Java**
```java
Yapf.Image img = Yapf.load(Path.of("texture.yapf"));
Yapf.save(Path.of("copy.yapf"), img);
```

**Zig**
```zig
const img = c.yapf_load("texture.yapf") orelse return error.CannotLoad;
defer c.yapf_free(img);
```

**Lua**
```lua
local img = yapf.load("texture.yapf")
yapf.save("copy.yapf", img)
```

**JavaScript**
```js
const img = YAPF.decode(bytes);
const file = YAPF.encode(img);
```

## Any other language

Swift, Kotlin/Native, Dart, PHP, Ruby, Julia, Nim, D, Odin, Haskell, … —
anything with a C foreign-function interface can call the library
directly.  These are the whole API:

```c
typedef struct {
    uint32_t  width, height;
    uint8_t   channels;     /* 1 gray, 2 gray+alpha, 3 RGB, 4 RGBA */
    uint8_t   gpu_format;   /* hint, see yapf.h */
    uint8_t   flags;        /* 1 premultiplied alpha, 2 sRGB */
    uint8_t   mip_levels;
    uint8_t  *pixels;       /* width*height*channels bytes, rows top to bottom */
    uint8_t **mips;         /* mips[0] == pixels */
} yapf_image_t;             /* 32 bytes on 64-bit systems: pixels at offset 16, mips at 24 */

yapf_image_t *yapf_load_memory_mt(const void *data, size_t size, int threads);  /* threads: 1 or 0 = all cores */
void          yapf_free(yapf_image_t *img);
int           yapf_encode(const yapf_image_t *img, void **out_data, size_t *out_size);
void          yapf_free_buffer(void *data);
yapf_image_t *yapf_load(const char *path);
int           yapf_save(const char *path, const yapf_image_t *img);
```

Returned images and buffers must be released with `yapf_free` /
`yapf_free_buffer` from the same library.  Ready-made bindings for these
languages are not included yet; the snippets below show the starting point
and are **not tested**.

**Swift** — add a module map for `yapf.h` (or a bridging header) and compile
`yapf.c` in the target; then `let img = yapf_load(path)` works as is.

**Dart** — `dart:ffi`: `DynamicLibrary.open('libyapf.so')` and
`lookupFunction<Pointer<YapfImage> Function(Pointer<Uint8>, IntPtr, Int32), …>('yapf_load_memory_mt')`.

**PHP 7.4+** — `$ffi = FFI::cdef(file_get_contents('yapf_api.h'), 'libyapf.so');`
with the declarations above in `yapf_api.h`.

**Ruby** — `Fiddle::Function.new(lib['yapf_load'], [Fiddle::TYPE_VOIDP], Fiddle::TYPE_VOIDP)`.

**Julia** — `ccall((:yapf_load, "libyapf"), Ptr{Cvoid}, (Cstring,), path)`.
