-- yapf.lua — YAPF for LuaJIT via its FFI.  Copyright 2026 Nexoniarz — Apache 2.0.
--   local yapf = require("yapf")
--   local img = yapf.load("texture.yapf")   -- img.width, img.height, img.channels, img.pixels (Lua string)
--   local bytes = yapf.encode(img)          -- or yapf.save(path, img)
-- Loads libyapf.so / yapf.dll / libyapf.dylib, or the path in $YAPF_LIBRARY.
local ffi = require("ffi")

ffi.cdef [[
typedef struct {
    uint32_t width, height;
    uint8_t channels, gpu_format, flags, mip_levels;
    uint8_t *pixels;
    uint8_t **mips;
} yapf_image_t;
yapf_image_t *yapf_load_memory_mt(const void *buffer, size_t size, int threads);
void yapf_free(yapf_image_t *img);
int yapf_encode(const yapf_image_t *img, void **out_data, size_t *out_size);
void yapf_free_buffer(void *data);
]]

local lib = ffi.load(os.getenv("YAPF_LIBRARY") or "yapf")
local M = { FLAG_PREMULT_ALPHA = 1, FLAG_SRGB = 2 }

-- Decode a Lua string holding a .yapf file.  threads: 1 (default) or 0 for all cores.
function M.decode(data, threads)
  local p = lib.yapf_load_memory_mt(data, #data, threads or 1)
  if p == nil then return nil, "not a valid YAPF file" end
  local img = {
    width = p.width, height = p.height, channels = p.channels,
    flags = p.flags, gpu_format = p.gpu_format,
    pixels = ffi.string(p.pixels, p.width * p.height * p.channels),
  }
  lib.yapf_free(p)
  return img
end

function M.load(path)
  local f, err = io.open(path, "rb")
  if not f then return nil, err end
  local data = f:read("*a")
  f:close()
  return M.decode(data)
end

-- img: { width, height, channels, pixels = string }  →  string with the .yapf bytes
function M.encode(img)
  local raw = ffi.new("yapf_image_t")
  local px = ffi.new("uint8_t[?]", #img.pixels)
  ffi.copy(px, img.pixels, #img.pixels)
  local table = ffi.new("uint8_t*[1]", px)
  raw.width, raw.height, raw.channels = img.width, img.height, img.channels
  raw.flags = img.flags or M.FLAG_SRGB
  raw.gpu_format = img.gpu_format or ({ 3, 2, 5, 4 })[img.channels]
  raw.mip_levels, raw.pixels, raw.mips = 1, px, table
  local out, size = ffi.new("void*[1]"), ffi.new("size_t[1]")
  if lib.yapf_encode(raw, out, size) ~= 0 then return nil, "encoding failed" end
  local bytes = ffi.string(out[0], size[0])
  lib.yapf_free_buffer(out[0])
  return bytes
end

function M.save(path, img)
  local bytes, err = M.encode(img)
  if not bytes then return nil, err end
  local f = assert(io.open(path, "wb"))
  f:write(bytes)
  f:close()
  return true
end

return M
