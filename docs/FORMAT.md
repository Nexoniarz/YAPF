# YAPF file format

This is the complete specification of YAPF files.  `yapf.c` is the
reference implementation; where this text and the code disagree, that is a
bug in one of them.

YAPF has two versions:

| Version | What it holds | Who writes it |
| :------ | :------------ | :------------ |
| **1** | one 8-bit image (1–4 channels) with optional mips | `yapf_save` / `yapf_encode`, and the CLI for plain 8-bit images |
| **2** | a texture: GPU blocks or lossless pixels in 8/16-bit or float, cubes, arrays, volumes, animation, channel meanings, colour description, notes, checksums, optional entropy coding | `yapf_tex_save` / `yapf_tex_encode`, and the CLI whenever a v2 feature is used |

Version 1 is specified at the top of [`yapf.h`](../yapf.h) and is not
repeated here, except that version 2 reuses its tiles, filters and plane
payloads (the "group form") byte for byte.

---

## Design: how YAPF 2 does things its own way

YAPF 2 is not a wrapper around someone else's container.  A few ideas
carry the whole format:

1. **Everything is tiles.**  Pixels are cut into 64 × 64 tiles, GPU
   blocks into tiles of 16 × 16 blocks.  Each tile has its own size and
   checksum in a table, so any tile can be found, checked and decoded on its
   own: for multithreaded loading, for partial updates, and for virtual
   texturing / "megatexture" streaming.
2. **Small mips first.**  Surface data is stored from the smallest mip
   to the largest.  The first *N* bytes of a file are a complete, lower
   resolution texture, so an engine can show something after reading a
   few kilobytes and stream in the large mips later.  The header carries
   the file size, so a loader knows how much is still to come.
3. **Needs, not versions.**  New capabilities are announced by *needs*
   bits.  A decoder refuses a file with a needs bit it does not know, and
   can ignore everything else safely.  No more version bumps that break
   every reader.
4. **Wide samples are byte planes.**  16-bit and float samples are split
   into byte planes (high bytes, low bytes) and coded with the same fast
   8-bit machinery as version 1: smooth high bytes compress very well, and
   the decoder stays branch-light and SIMD friendly.
5. **Shared code books.**  The optional entropy stage keeps its Huffman
   tables once per surface, keyed by a small neighbourhood context, so
   tiles stay independent and their own headers stay tiny.  For GPU
   blocks the tables are per byte lane, so endpoint bytes and index bytes
   are coded separately.
6. **Meaning travels with the data.**  Channel meanings, the normal-map
   convention, linear channels inside sRGB images, colour primaries and
   transfer, and how the mips were made are header fields, not file-name
   suffixes.

---

## Conventions

All integers are little-endian.  `u8`, `u16`, `u32` and `u64` are unsigned
integers of that many bits.  Offsets are from the start of the file.

### yapf-sum

The checksum of a byte string of length *n*.  `K = 0x9E3779B97F4A7C15`;
all arithmetic is modulo 2⁶⁴.

```
lane[i] = (i + 1)·K  XOR  n                       for i = 0, 1, 2, 3
for each 8-byte word w_j of the input (little-endian; the last word is
padded with zero bytes):
    a = lane[j mod 4]
    a = (a XOR w_j) · K
    a = a XOR (a >> 32)
    lane[j mod 4] = a
h = lane[0] XOR rotl(lane[1], 16) XOR rotl(lane[2], 32) XOR rotl(lane[3], 48)
h = h · K
h = h XOR (h >> 32)
result = low 32 bits of h
```

Test vectors: `""` → `0xd7550ed0`, `"YAPF"` → `0xc8cc41d4`,
`"The quick brown fox jumps over the lazy dog"` → `0xbce4f410`.

---

## Version 2 layout

```
HEADER            64 bytes
SURFACE TABLE     16 bytes per surface
FRAME TABLE       4 bytes per frame, only when frames > 1
NOTES             notes size bytes
SURFACE DATA      in stream order: smallest mip first
```

The *front matter* is everything before the surface data:
`64 + 16·surfaces + (frames > 1 ? 4·frames : 0) + notes size` bytes.

### Header

| Offset | Size | Field | Notes |
| -----: | ---: | :---- | :---- |
| 0x00 | 4 | magic | `"YAPF"` |
| 0x04 | 1 | version | 2 |
| 0x05 | 1 | channels | 1–4 |
| 0x06 | 1 | format | block format (block payload) or GPU hint (pixel payload); see [GPU formats](#gpu-formats) |
| 0x07 | 1 | mip levels | 1–16 |
| 0x08 | 1 | flags | bit 0 premultiplied alpha, bit 1 sRGB, bit 2 coverage-preserving mips; other bits 0 |
| 0x09 | 1 | payload | 0 pixels (lossless tiles), 1 GPU blocks |
| 0x0A | 1 | sample | pixel payload: 0 u8, 1 u16, 2 half float, 3 float.  Block payload: type of the decoded texels (0, or 2 for BC6H) |
| 0x0B | 1 | shape | 0 2D, 1 cube, 2 volume |
| 0x0C | 4 | width | 1–65535 |
| 0x10 | 4 | height | 1–65535; cubes: width = height |
| 0x14 | 4 | depth | volumes: 1–65535; otherwise 1 |
| 0x18 | 2 | layers | ≥ 1 (array layers; with a cube, a cube array); volumes: 1 |
| 0x1A | 2 | frames | ≥ 1 (animation frames) |
| 0x1C | 4 | needs | see [Needs](#needs) |
| 0x20 | 4 | channel map | meaning of channels 0–3, see [Channel meanings](#channel-meanings) |
| 0x24 | 1 | linear mask | bit *c*: channel *c* is linear even when the sRGB flag is set |
| 0x25 | 1 | normal | bits 0–1 convention (0 none, 1 OpenGL +Y, 2 DirectX −Y); bits 2–3 encoding (0 XYZ, 1 XY with Z reconstructed, 2 octahedral) |
| 0x26 | 1 | primaries | 0 unspecified, 1 BT.709/sRGB, 2 Display P3, 3 BT.2020, 4 ACES AP0, 5 ACEScg (AP1), 6 Adobe RGB |
| 0x27 | 1 | transfer | 0 unspecified (the sRGB flag decides), 1 sRGB, 2 linear, 3 gamma 2.2, 4 PQ (ST 2084), 5 HLG, 6 BT.1886 |
| 0x28 | 1 | mip filter | bits 0–6: 0 unknown, 1 box, 2 tent, 3 Kaiser, 4 Lanczos, 5 Mitchell, 6 authored; bit 7: sRGB channels were filtered in linear light |
| 0x29 | 1 | alpha ref | with the coverage flag: alpha-test reference 0–255 |
| 0x2A | 2 | loop count | animation repeats, 0 = forever |
| 0x2C | 4 | notes size | bytes of the notes area |
| 0x30 | 8 | file size | total bytes of the file |
| 0x38 | 4 | tables check | yapf-sum of bytes [0x40, front matter end) |
| 0x3C | 4 | header check | yapf-sum of bytes [0x00, 0x3C) |

The header check covers the tables check, so the whole front matter is
protected.

### Surfaces

A *surface* is one mip level of one face of one layer of one frame.  With
`faces = 6` for cubes (order +X, −X, +Y, −Y, +Z, −Z) and 1 otherwise,
surface number

```
s = ((frame · layers + layer) · faces + face) · mip_levels + mip
```

Mip *m* is `max(1, width >> m) × max(1, height >> m)`, and for volumes
`max(1, depth >> m)` slices deep.  A volume surface holds all its slices.

**Surface table entry** (16 bytes, one per surface, in surface order):

| Offset | Size | Field |
| -----: | ---: | :---- |
| +0 | 8 | offset of the surface data |
| +8 | 4 | size of the surface data |
| +12 | 4 | tile-table check: yapf-sum of the surface's book size, book and tile table |

**Stream order.**  Writers store surface data for mip `mip_levels − 1`
first (all frames, layers and faces in surface order), then the next larger
mip, and so on, ending with mip 0.  Readers must use the offsets and must not
rely on the order.  The number of leading bytes that hold every surface of
mip *m* and smaller is the largest `offset + size` over those surfaces.

### Frame table

When `frames > 1`: one `u32` per frame, its display time in microseconds.

### Notes

A sequence of entries that exactly fills the notes area:

```
tag      4 bytes
length   u32
data     length bytes
```

| Tag | Content |
| :-- | :------ |
| `AUTH` | author (UTF-8) |
| `LICN` | licence (UTF-8, e.g. an SPDX id) |
| `SRCE` | source / origin (UTF-8, e.g. a URL or path) |
| `TOOL` | the tool that wrote the file (UTF-8) |
| `TITL` | title (UTF-8) |
| `DESC` | description (UTF-8) |
| `DATE` | date, ISO 8601 (UTF-8) |
| `ICCP` | an ICC colour profile |
| `ORNT` | orientation, 1 byte: bits 0–1 clockwise quarter turns, bit 2 mirror left–right after turning |
| `DPI ` | 2 × u32: horizontal and vertical dots per inch × 1000 |
| `CLL ` | 2 × u16: maximum content light level, maximum frame-average light level (cd/m²) |

Tags that begin with a lowercase letter belong to applications (for
example `keel`).  Readers ignore tags they do not know.  Tags may repeat;
order is kept.

### Needs

| Bit | Name | Set when |
| --: | :--- | :------- |
| 0 | WIDE | pixel payload with sample ≠ u8 |
| 1 | BLOCKS | block payload |
| 2 | ENTROPY | any surface has a code book |
| 3 | LAYOUT | shape ≠ 2D, or layers > 1 |
| 4 | FRAMES | frames > 1 |

A decoder must refuse a file that sets a needs bit it does not implement
(this library: any bit above 4).  A file that uses a feature without its
bit is corrupt.  Everything that is not a needs feature — channel map,
colour fields, mip filter, notes, frame times — may be ignored.

A decoder that only implements 8-bit 2D pixels can therefore read every
version 2 file whose needs are 0, and cleanly refuse the rest.

---

## Surface data

```
book size   u32            0 when the surface has no code book
book        book size bytes
tile table  8 bytes per tile:  u32 size (bit 31: packed),  u32 yapf-sum of the tile bytes
tiles       back to back; their sizes must add up exactly to the rest of the surface
```

Tiles are numbered slice by slice, rows top to bottom, tiles left to right.
For pixels the grid is `ceil(w / 64) × ceil(h / 64)` tiles per slice (edge
tiles are smaller, no padding); for blocks it is
`ceil(bx / 16) × ceil(by / 16)` tiles of 16 × 16 blocks, where
`bx = ceil(w / block width)` and `by = ceil(h / block height)`.

Bit 31 of a tile size (*packed*) is allowed only in block tiles of a
surface with a code book.

---

## Pixel tiles

Let *B* be the bytes per sample (1, 2 or 4), *ch* the channels, and the
tile `tw × th` pixels.

### Tile header

```
byte 0     0x80          stored: the raw samples follow, interleaved in
                         channel order, each B bytes little-endian;
                         exactly tw·th·ch·B bytes
           otherwise     coded:
             bits 0–3    B = 1: bit c set = plane c is constant;  B > 1: 0
             bit 4       colour transform subtract-green
             bit 5       an entropy mask follows
             bit 6       no colour transform
             bit 7       0
           bits 4 and 6 are exclusive and need ch >= 3; with neither
           set, an RGB(A) tile uses YCoCg-R
B > 1      u16 constant mask, bit p = plane p is constant
bit 5      entropy mask: u8 (B = 1) or u16 (B > 1); bit p = plane p is
           entropy coded.  It must not include constant planes.
then       one value byte per constant plane, in plane order;
           one payload per other plane, in plane order;
           the tile ends exactly where its last payload ends
```

With B = 1 and neither bit 5 nor bit 6 set, a coded tile is exactly a
version 1 tile.

### Planes

The colour transform turns R, G, B (+ A) into transformed channels; it is
skipped for 1–2 channels and with bit 6.

* **YCoCg-R** at the sample width *n* = 8·B bits, all arithmetic mod 2ⁿ,
  `>>` an arithmetic shift of the *n*-bit two's complement value, `h = 2ⁿ⁻¹`:

  ```
  forward:  o = R − B;  t = B + (o >> 1);  q = G − t;
            Y = t + (q >> 1);  Co = o + h;  Cg = q + h
  inverse:  o = Co − h;  q = Cg − h;  t = Y − (q >> 1);
            G = q + t;  B = t − (o >> 1);  R = B + o
  ```
* **subtract-green**: `G, R − G + h, B − G + h` (+ A), mod 2ⁿ.

Before the transform, float samples (half or float) are mapped to
integers that keep their order: if the sign bit is set every bit is
flipped, otherwise the sign bit is set.  The decoder undoes this after the
inverse transform.

Transformed channel *c* gives B planes: plane `c·B + k` holds byte
`B − 1 − k` of the value (the most significant byte first).  For B = 1
the planes are the transformed channels.

### Plane payloads

A plane payload has one of two forms, chosen per plane by the entropy mask.

**Group form** — exactly the version 1 plane payload: a filter byte per
band of 8 rows, 4-bit control codes, and groups of 8 residuals at the
group's bit width.  See `yapf.h`.

**Entropy form**

```
filter[ceil(th / 8)]     as in the group form
u16 L                    bytes of the bit stream
L bytes                  the folded residuals of the plane in raster order
```

Each residual is coded with table `(slot, context)` of the surface's code
book:

* `slot = t · 16 + p`, where *p* is the plane and *t* is 1 with
  subtract-green, 2 with no transform, else 0;
* `context = min(6, floor(log2(s + 1)))`, `s = left + above`, the folded
  residuals of the left and upper neighbours in this plane (0 outside the
  tile) — so 0, 1–2, 3–6, 7–14, 15–30, 31–62, 63 and up.

The stream must end in its last byte.  Folded residuals then go through
the filters exactly as in the group form.

---

## Block tiles

A block tile of `tbw × tbh` blocks of *S* bytes is either

* **raw**: the blocks, rows top to bottom, exactly `tbw·tbh·S` bytes — as
  the GPU takes them; or
* **packed** (bit 31 of its size): one bit stream holding byte 0 of every
  block (in raster order), then byte 1 of every block, …, byte S − 1.
  Byte lane *k* is coded with table `(slot k, context 0)` of the surface's
  code book.  The stream must end in the last byte of the tile.

Decoded block surfaces are row-major blocks, slice after slice, ready for
`glCompressedTexImage*` / `vkCmdCopyBufferToImage`.

---

## Code book

```
u16 count                      number of tables, 1–336
count × {
    u8 slot                    0–47
    u8 context                 0–6
    code table
}
```

Each `(slot, context)` pair appears at most once.  The book must fill its
size exactly.

**Code table**: `u8 n − 1`, then *n* 4-bit code lengths (symbols
0 … n − 1, low nibble first, an odd count padded with a zero nibble).
Length 0 = no code; otherwise 1–11.  At least one code; Kraft sum ≤ 1.
Codes are canonical — by length, then by symbol, the first code of each
length one more than the last code of the previous length, shifted left —
and stored least significant bit first (the first bit of a code is the
lowest unread bit of the stream), so a decoder can look up 11 bits at a
time.

---

## GPU formats

Pixel payload hints (`format` field):

| Value | Name | Vulkan |
| ----: | :--- | :----- |
| 0x00 | RGBA8 | R8G8B8A8_UNORM |
| 0x01 | RGB8 | R8G8B8_UNORM |
| 0x02 | RG8 | R8G8_UNORM |
| 0x03 | R8 | R8_UNORM |
| 0x04 | SRGB8_A8 | R8G8B8A8_SRGB |
| 0x05 | SRGB8 | R8G8B8_SRGB |
| 0x06–0x09 | RGBA16, RGB16, RG16, R16 | R16G16B16A16_UNORM … R16_UNORM |
| 0x0A–0x0D | RGBA16F, RGB16F, RG16F, R16F | R16G16B16A16_SFLOAT … R16_SFLOAT |
| 0x0E–0x11 | RGBA32F, RGB32F, RG32F, R32F | R32G32B32A32_SFLOAT … R32_SFLOAT |

Block formats (block payload).  With the sRGB flag, use the `_SRGB`
variant where one exists.

| Value | Name | Block | Bytes | Vulkan / DXGI |
| ----: | :--- | :---: | ----: | :------------ |
| 0x40 | BC1 | 4×4 | 8 | BC1_RGB(A)_UNORM / BC1_UNORM |
| 0x41 | BC2 | 4×4 | 16 | BC2_UNORM |
| 0x42 | BC3 | 4×4 | 16 | BC3_UNORM |
| 0x43 | BC4 | 4×4 | 8 | BC4_UNORM |
| 0x44 | BC4_SNORM | 4×4 | 8 | BC4_SNORM |
| 0x45 | BC5 | 4×4 | 16 | BC5_UNORM |
| 0x46 | BC5_SNORM | 4×4 | 16 | BC5_SNORM |
| 0x47 | BC6H | 4×4 | 16 | BC6H_UFLOAT / BC6H_UF16 |
| 0x48 | BC6H_SF | 4×4 | 16 | BC6H_SFLOAT / BC6H_SF16 |
| 0x49 | BC7 | 4×4 | 16 | BC7_UNORM |
| 0x50 | ETC2_RGB8 | 4×4 | 8 | ETC2_R8G8B8_UNORM |
| 0x51 | ETC2_RGB8A1 | 4×4 | 8 | ETC2_R8G8B8A1_UNORM |
| 0x52 | ETC2_RGBA8 | 4×4 | 16 | ETC2_R8G8B8A8_UNORM |
| 0x53 | EAC_R11 | 4×4 | 8 | EAC_R11_UNORM |
| 0x54 | EAC_R11_SNORM | 4×4 | 8 | EAC_R11_SNORM |
| 0x55 | EAC_RG11 | 4×4 | 16 | EAC_R11G11_UNORM |
| 0x56 | EAC_RG11_SNORM | 4×4 | 16 | EAC_R11G11_SNORM |
| 0x60–0x6D | ASTC 4×4, 5×4, 5×5, 6×5, 6×6, 8×5, 8×6, 8×8, 10×5, 10×6, 10×8, 10×10, 12×10, 12×12 | as named | 16 | ASTC_*_UNORM (HDR: transfer linear, sample half) |

---

## Channel meanings

| Value | Meaning | Value | Meaning |
| ----: | :------ | ----: | :------ |
| 0 | default for the channel count (Y / YA / RGB / RGBA) | 11 | metalness |
| 1 | red | 12 | ambient occlusion |
| 2 | green | 13 | height |
| 3 | blue | 14 | specular |
| 4 | alpha | 15 | emissive |
| 5 | luma | 16 | opacity (alpha-test mask) |
| 6 | normal X | 17 | mask |
| 7 | normal Y | 18 | cavity |
| 8 | normal Z | 19 | thickness |
| 9 | roughness | 20 | id |
| 10 | gloss (1 − roughness) | 255 | unused |

Alpha (an explicit 4 or 16, or the last channel of a 2- or 4-channel image
with default meanings) is always linear.  The linear mask marks further
channels that are linear inside an sRGB image, such as masks packed next to
colour.

---

## Mips

`mip filter` records how the mips were made.  This library's
`yapf_tex_build_mips` uses a 2 × 2 box (2 × 2 × 2 for volumes, edges
repeat), averages sRGB 8-bit channels in linear light (bit 7), and with an
alpha reference rescales alpha on every mip so that the share of texels
with alpha *above* the reference stays that of mip 0 (coverage flag,
`alpha ref`).  Alpha-tested foliage and fences then keep their density at
a distance instead of thinning out.

---

## Validation summary

A decoder rejects a file when:

* the magic, version, a size, shape or count is out of range, or a cube is
  not square;
* the header or tables check does not match (unless the reader was told to
  trust the file);
* a needs bit is unknown (*unsupported*), or a feature is used without its
  needs bit (*corrupt*);
* a surface lies outside the file, its book is malformed or present without
  ENTROPY, or its tiles do not fill it exactly;
* a tile's yapf-sum does not match, or the tile does not decode to exactly
  its size (reserved bits, invalid filters, codes, masks or lengths).

Because every structure is validated before use, corrupt or hostile input
is rejected, never read out of bounds.
