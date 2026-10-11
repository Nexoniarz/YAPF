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

Example: `YAPF_LIBRARY=../../build/libyapf.so python3 example.py ../../other/YAPF.YAPF`

## Textures (YAPF 2)

```python
tex = yapf.load_texture("albedo.yapf")       # any v1 / v2 file, needs the C library
tex.payload, tex.format, tex.sample, tex.channels, tex.mip_levels
tex.shape, tex.layers, tex.frames, tex.channel_map, tex.normal, tex.notes
raw  = tex.surface(frame=0, layer=0, face=0, mip=0)   # bytes as stored (GPU blocks / samples)
rgba = tex.rgba_float()                      # numpy float32 (h, w, 4) for display
```

`yapf.load` / `yapf.decode` still return 8-bit images; on a texture they
cannot represent they raise `YapfError` saying so.  Errors name the
reason (damaged, truncated, not YAPF, newer version).
