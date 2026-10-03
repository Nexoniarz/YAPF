# yapf.py — YAPF in Python

```python
import yapf

img = yapf.load("texture.yapf")        # or yapf.decode(data)
print(img.width, img.height, img.channels, img.mip_levels)
img.pixels                             # bytes, rows top to bottom

yapf.save("copy.yapf", img)            # or yapf.encode(img) -> bytes
yapf.save("new.yapf", yapf.Image(w, h, 4, rgba_bytes))
```

It uses the C library through `ctypes` when it finds one:

1. the file named by the `YAPF_LIBRARY` environment variable,
2. `libyapf.so` / `libyapf.dylib` / `yapf.dll` next to `yapf.py`, or in
   `lib/<platform>/` beside it (e.g. `lib/windows-x64/yapf.dll`),
3. the system library path (`sudo make install` installs one).

Without the library it falls back to a pure-Python decoder that needs
`numpy` (decoding only; about 1.4 s for a 4K image).  `yapf.backend()`
tells you which one is in use.
