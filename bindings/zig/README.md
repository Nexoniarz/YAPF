# YAPF for Zig

Zig imports `yapf.h` directly — no bindings needed:

```zig
const c = @cImport(@cInclude("yapf.h"));
const img = c.yapf_load("texture.yapf") orelse return error.CannotLoad;
defer c.yapf_free(img);
```

Add `yapf.c` to your build (`exe.addCSourceFile(.{ .file = b.path("yapf.c") })`,
`exe.linkLibC()`), or run the example:
`zig run example.zig ../../yapf.c -I../.. -lc -O ReleaseFast`.
