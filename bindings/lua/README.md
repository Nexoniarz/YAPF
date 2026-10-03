# YAPF for Lua (LuaJIT)

`yapf.lua` uses LuaJIT's FFI with the YAPF shared library (`libyapf.so`,
`yapf.dll`, `libyapf.dylib`, or the path in `$YAPF_LIBRARY`).  Works in
LÖVE (love2d), which runs on LuaJIT.

```lua
local yapf = require("yapf")
local img = yapf.load("texture.yapf")     -- width, height, channels, pixels (string)
yapf.save("out.yapf", img)
```

Example: `YAPF_LIBRARY=../../build/libyapf.so luajit example.lua`
