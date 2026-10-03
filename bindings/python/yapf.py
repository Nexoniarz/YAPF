"""
yapf.py — Python bindings for YAPF v1 images.

Copyright 2026 Nexoniarz — Apache License 2.0.

    import yapf
    img = yapf.load("texture.yapf")          # or yapf.decode(data)
    img.width, img.height, img.channels      # pixels: bytes, row-major
    yapf.save("out.yapf", img)                # or yapf.encode(img) -> bytes

Uses the C library through ctypes when one can be found (fast, can
encode); otherwise falls back to a pure-Python decoder that needs numpy
(decode only).  The library is looked up in this order:

    1. the path in the YAPF_LIBRARY environment variable
    2. next to this file:  libyapf.so / libyapf.dylib / yapf.dll
       (also in a platform subfolder such as  lib/linux-x64/)
    3. the system library path

Build the library with:  cc -O2 -shared -fPIC yapf.c -o libyapf.so -pthread
"""

import ctypes
import os
import platform
import struct
import sys

__all__ = ["Image", "load", "save", "decode", "encode", "backend",
           "GPU_RGBA8", "GPU_RGB8", "GPU_RG8", "GPU_R8", "GPU_SRGB8_A8", "GPU_SRGB8",
           "FLAG_PREMULT_ALPHA", "FLAG_SRGB", "YapfError"]

GPU_RGBA8, GPU_RGB8, GPU_RG8, GPU_R8, GPU_SRGB8_A8, GPU_SRGB8 = range(6)
FLAG_PREMULT_ALPHA, FLAG_SRGB = 1, 2


class YapfError(Exception):
    pass


class Image:
    """A decoded image.  `pixels` holds width*height*channels bytes, rows top
    to bottom; `mips` holds every mip level (mips[0] is pixels)."""

    def __init__(self, width, height, channels, pixels,
                 gpu_format=None, flags=FLAG_SRGB, mips=None):
        self.width, self.height, self.channels = int(width), int(height), int(channels)
        self.pixels = bytes(pixels)
        if gpu_format is None:
            gpu_format = {1: GPU_R8, 2: GPU_RG8, 3: GPU_SRGB8, 4: GPU_SRGB8_A8}[self.channels]
        self.gpu_format, self.flags = gpu_format, flags
        self.mips = [self.pixels] + [bytes(m) for m in (mips or [])[1:]]

    @property
    def mip_levels(self):
        return len(self.mips)

    def __repr__(self):
        return "<yapf.Image %dx%d, %d channel(s), %d mip level(s)>" % (
            self.width, self.height, self.channels, self.mip_levels)


# ── ctypes backend ────────────────────────────────────────────────────

class _CImage(ctypes.Structure):
    _fields_ = [("width", ctypes.c_uint32), ("height", ctypes.c_uint32),
                ("channels", ctypes.c_uint8), ("gpu_format", ctypes.c_uint8),
                ("flags", ctypes.c_uint8), ("mip_levels", ctypes.c_uint8),
                ("pixels", ctypes.POINTER(ctypes.c_uint8)),
                ("mips", ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8)))]


def _platform_dir():
    machine = platform.machine().lower()
    arch = "arm64" if machine in ("arm64", "aarch64") else "x64"
    system = {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux")
    return "%s-%s" % (system, arch)


def _find_library():
    names = {"win32": ["yapf.dll", "libyapf.dll"],
             "darwin": ["libyapf.dylib"]}.get(sys.platform, ["libyapf.so"])
    here = os.path.dirname(os.path.abspath(__file__))
    candidates = []
    if os.environ.get("YAPF_LIBRARY"):
        candidates.append(os.environ["YAPF_LIBRARY"])
    for folder in (here, os.path.join(here, "lib", _platform_dir())):
        candidates += [os.path.join(folder, n) for n in names]
    candidates += names
    for path in candidates:
        try:
            lib = ctypes.CDLL(path)
            lib.yapf_load_memory_mt.restype = ctypes.POINTER(_CImage)
            lib.yapf_load_memory_mt.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_int]
            lib.yapf_free.argtypes = [ctypes.POINTER(_CImage)]
            lib.yapf_save.argtypes = [ctypes.c_char_p, ctypes.POINTER(_CImage)]
            lib.yapf_encode.argtypes = [ctypes.POINTER(_CImage), ctypes.POINTER(ctypes.c_void_p),
                                        ctypes.POINTER(ctypes.c_size_t)]
            lib.yapf_free_buffer.argtypes = [ctypes.c_void_p]
            return lib
        except (OSError, AttributeError):
            continue
    return None


_lib = _find_library()


def backend():
    """'c' when the C library is used, 'python' for the fallback decoder."""
    return "c" if _lib else "python"


def _c_decode(data):
    data = bytes(data)
    p = _lib.yapf_load_memory_mt(data, len(data), 0)
    if not p:
        raise YapfError("not a valid YAPF v1 file")
    try:
        c = p.contents
        mips = []
        for m in range(c.mip_levels):
            w, h = max(1, c.width >> m), max(1, c.height >> m)
            mips.append(ctypes.string_at(c.mips[m], w * h * c.channels))
        return Image(c.width, c.height, c.channels, mips[0], c.gpu_format, c.flags, mips)
    finally:
        _lib.yapf_free(p)


def _c_encode(img):
    bufs = [ctypes.create_string_buffer(m, len(m)) for m in img.mips]
    ptrs = (ctypes.POINTER(ctypes.c_uint8) * len(bufs))(
        *[ctypes.cast(b, ctypes.POINTER(ctypes.c_uint8)) for b in bufs])
    c = _CImage(img.width, img.height, img.channels, img.gpu_format, img.flags,
                len(bufs), ptrs[0], ptrs)
    data, size = ctypes.c_void_p(), ctypes.c_size_t()
    rc = _lib.yapf_encode(ctypes.byref(c), ctypes.byref(data), ctypes.byref(size))
    if rc != 0:
        raise YapfError("yapf_encode failed with code %d" % rc)
    try:
        return ctypes.string_at(data, size.value)
    finally:
        _lib.yapf_free_buffer(data)


# ── pure-Python fallback decoder (numpy) ──────────────────────────────

_NIB_GROUPS = [1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 3, 4, 8, 16, 32, 64]


def _py_decode(data):
    try:
        import numpy as np
    except ImportError:
        raise YapfError("the YAPF C library was not found and numpy is not "
                        "installed; build the library (see yapf.py) or install numpy")
    b = np.frombuffer(bytes(data), dtype=np.uint8)
    if len(b) < 20 or bytes(b[:4]) != b"YAPF":
        raise YapfError("not a YAPF file")
    if b[4] != 1:
        raise YapfError("unsupported YAPF version %d" % b[4])
    ch, gpu, mip_levels, flags = int(b[5]), int(b[6]), int(b[7]), int(b[8])
    W, H = struct.unpack_from("<II", bytes(b[12:20]))
    if not (1 <= ch <= 4 and 1 <= mip_levels <= 16 and 1 <= W <= 65535 and 1 <= H <= 65535):
        raise YapfError("corrupt header")

    def bad():
        raise YapfError("corrupt YAPF data")

    mips = []
    for m in range(mip_levels):
        off, ln = struct.unpack_from("<II", bytes(b[20 + 8 * m: 28 + 8 * m]))
        mw, mh = max(1, W >> m), max(1, H >> m)
        if off > len(b) or ln > len(b) - off:
            bad()
        out = np.zeros((mh, mw, ch), dtype=np.uint8)
        tcx, tcy = (mw + 63) // 64, (mh + 63) // 64
        sizes = np.frombuffer(bytes(b[off: off + 4 * tcx * tcy]), dtype="<u4")
        pos = off + 4 * tcx * tcy
        for t in range(tcx * tcy):
            tx, ty = (t % tcx) * 64, (t // tcx) * 64
            tw, th = min(64, mw - tx), min(64, mh - ty)
            end = pos + int(sizes[t])
            if end > off + ln:
                bad()
            try:
                out[ty:ty + th, tx:tx + tw] = _py_tile(np, b, pos, end, tw, th, ch, bad)
            except (IndexError, ValueError):
                bad()
            pos = end
        mips.append(out.tobytes())
    return Image(W, H, ch, mips[0], gpu, flags, mips)


def _py_tile(np, b, p, end, tw, th, ch, bad):
    hdr = int(b[p]); p += 1
    if hdr == 0x80:
        if end - p != tw * th * ch:
            bad()
        return b[p:end].reshape(th, tw, ch)
    if hdr & 0xE0:
        bad()
    xf, mask = hdr & 0x10, hdr & 0x0F
    planes = np.zeros((ch, th, tw), dtype=np.uint8)
    for c in range(ch):
        if mask >> c & 1:
            planes[c, :, :] = b[p]; p += 1
    for c in range(ch):
        if mask >> c & 1:
            continue
        nb, ng = (th + 7) // 8, (tw * th + 7) // 8
        filt = b[p:p + nb]; p += nb
        widths, g, hi, w = [], 0, 0, 0
        while g < ng:
            code = (int(b[p]) >> (4 * hi)) & 15
            p += hi; hi ^= 1
            if code < 9:
                w = code
            n = _NIB_GROUPS[code]
            widths += [w] * n; g += n
        if g != ng:
            bad()
        if hi:
            p += 1
        z = np.zeros(ng * 8, dtype=np.uint8)
        widths = np.array(widths, dtype=np.int64)
        starts = p + np.concatenate(([0], np.cumsum(widths)[:-1]))
        p += int(widths.sum())
        if p > end:
            bad()
        for wv in range(1, 9):
            sel = np.nonzero(widths == wv)[0]
            if not len(sel):
                continue
            raw = b[starts[sel][:, None] + np.arange(wv)].astype(np.uint64)
            word = (raw << (8 * np.arange(wv, dtype=np.uint64))).sum(axis=1, dtype=np.uint64)
            shifts = np.arange(8, dtype=np.uint64) * np.uint64(wv)
            vals = (word[:, None] >> shifts) & np.uint64((1 << wv) - 1)
            z[(sel[:, None] * 8 + np.arange(8)).ravel()] = vals.ravel().astype(np.uint8)
        r = ((z[:tw * th] >> 1) ^ (0 - (z[:tw * th] & 1)).astype(np.uint8)).reshape(th, tw).astype(np.int64)
        P = planes[c].astype(np.int64)
        for y in range(th):
            f = int(filt[y // 8])
            U = P[y - 1] if y else np.zeros(tw, np.int64)
            if f == 0:
                P[y] = np.cumsum(r[y]) & 255
            elif f == 1:
                inc = r[y] + U - np.concatenate(([0], U[:-1]))
                P[y] = np.cumsum(inc) & 255
            elif f in (2, 3):
                AR = np.concatenate((U[1:], U[-1:]))
                pred = AR if f == 2 else (U + AR) >> 1
                P[y] = (pred + r[y]) & 255
            elif 16 <= f < 24:
                k = f - 15
                K = P[y - k] if y >= k else np.zeros(tw, np.int64)
                P[y] = (K + r[y]) & 255
            else:
                bad()
        planes[c] = P.astype(np.uint8)
    if p != end:
        bad()
    if ch < 3:
        return planes.transpose(1, 2, 0)
    P0, P1, P2 = (planes[i].astype(np.int64) for i in range(3))
    if xf:
        R, G, B = P1 + P0 - 128, P0, P2 + P0 - 128
    else:
        co, cg = (P1 - 128) & 255, (P2 - 128) & 255
        s8 = lambda v: np.where(v > 127, v - 256, v)
        t = (P0 - (s8(cg) >> 1)) & 255
        B = (t - (s8(co) >> 1)) & 255
        R, G = B + co, cg + t
    out = [R & 255, G & 255, B & 255] + ([planes[3].astype(np.int64)] if ch == 4 else [])
    return np.stack(out, axis=-1).astype(np.uint8)


# ── public API ────────────────────────────────────────────────────────

def decode(data):
    """Decode YAPF bytes into an Image."""
    return _c_decode(data) if _lib else _py_decode(data)


def load(path):
    """Read and decode a .yapf file."""
    with open(path, "rb") as f:
        return decode(f.read())


def encode(img):
    """Encode an Image (or anything with width/height/channels/pixels) to
    YAPF bytes.  Needs the C library."""
    if not _lib:
        raise YapfError("saving .yapf needs the YAPF native library, which was not found "
                        "for this platform; see https://github.com/Nexoniarz/YAPF#python")
    if not isinstance(img, Image):
        img = Image(img.width, img.height, img.channels, img.pixels)
    expected = img.width * img.height * img.channels
    if len(img.pixels) != expected:
        raise YapfError("expected %d pixel bytes, got %d" % (expected, len(img.pixels)))
    return _c_encode(img)


def save(path, img):
    """Encode an Image to a .yapf file.  Needs the C library."""
    data = encode(img)
    with open(path, "wb") as f:
        f.write(data)


if __name__ == "__main__":
    for path in sys.argv[1:]:
        print(path, load(path), "backend:", backend())
