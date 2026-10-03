-- YAPF_LIBRARY=../../build/libyapf.so luajit example.lua ../../other/YAPF.YAPF
package.path = "./?.lua;" .. package.path
local yapf = require("yapf")
local path = arg[1] or "../../other/YAPF.YAPF"
local f = assert(io.open(path, "rb")); local data = f:read("*a"); f:close()

local img = assert(yapf.decode(data))
local best = math.huge                    -- best of 10 decodes
for _ = 1, 10 do
  local t = os.clock()
  img = yapf.decode(data)
  best = math.min(best, os.clock() - t)
end
print(string.format("%s: %dx%d, %d channels, decoded in %.2f ms", path, img.width, img.height, img.channels, best * 1000))
print(string.format("file is %d bytes, %.1f%% of the raw pixels", #data, 100 * #data / #img.pixels))
local again = yapf.encode(img)
print(string.format("re-encoded: %d bytes, identical: %s", #again, tostring(again == data)))
