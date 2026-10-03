"""
example.py — YAPF from Python.

    make                      # builds build/libyapf.so (or get it from Releases)
    YAPF_LIBRARY=../../build/libyapf.so python3 example.py ../../other/YAPF.YAPF
"""
import sys
import time

import yapf

path = sys.argv[1] if len(sys.argv) > 1 else "../../other/YAPF.YAPF"
data = open(path, "rb").read()

img = yapf.decode(data)
ms = float("inf")                       # best of 10, as in a running program
for _ in range(10):
    t = time.perf_counter()
    img = yapf.decode(data)
    ms = min(ms, (time.perf_counter() - t) * 1000)
raw = img.width * img.height * img.channels
print("%s: %dx%d, %d channels, decoded in %.2f ms (%s backend)"
      % (path, img.width, img.height, img.channels, ms, yapf.backend()))
print("file is %d bytes, %.1f%% of the raw pixels" % (len(data), 100.0 * len(data) / raw))

if yapf.backend() == "c":
    again = yapf.encode(img)
    print("re-encoded:", len(again), "bytes, identical:", again == data)

    w = h = 128                                     # your own pixels → .yapf
    rgb = bytes(v for y in range(h) for x in range(w) for v in (x * 2, y * 2, 128))
    yapf.save("gradient.yapf", yapf.Image(w, h, 3, rgb))
    print("wrote gradient.yapf")
