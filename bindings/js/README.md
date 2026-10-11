# yapf.js — YAPF in JavaScript

Encoder (version 1) and decoder (versions 1 and 2) in one dependency-free file.  Works in
browsers, Node.js, web workers, Figma plugins and VS Code webviews, and
produces exactly the same bytes as the C library.

```js
const YAPF = require('./yapf.js');            // or <script src="yapf.js"> → window.YAPF

const img = YAPF.decode(bytes);               // Uint8Array → image
// img = { width, height, channels, pixels, gpuFormat, flags, mipLevels, mips }

const file = YAPF.encode({ width, height, channels: 4, pixels });   // → Uint8Array

// draw on a canvas
ctx.putImageData(new ImageData(YAPF.toRGBA(img), img.width, img.height), 0, 0);
```

`decode` throws an `Error` for corrupt or unsupported data.  Speed: a 4K
image decodes in about 150 ms in Node / Chrome (the C library: 6 ms).

TypeScript types are in `yapf.d.ts`.

Example: `node example.mjs ../../other/YAPF.YAPF` · tests: `node test.js`.

## Textures (YAPF 2)

```js
const tex  = YAPF.decodeTexture(bytes);              // any v1 / v2 file
// tex: { width, height, depth, layers, frames, faces, shape, mipLevels,
//        payload, format, sample, channels, flags, channelMap, normal,
//        primaries, transfer, frameUs, notes, surfaces: [...] }
const s    = YAPF.surfaceIndex(tex, frame, layer, face, mip);
const view = YAPF.textureToRGBA(tex, s);             // { width, height, rgba } for a canvas
```

`surfaces[s]` is a `Uint8Array` (8-bit pixels or GPU blocks, ready for
WebGL / WebGPU compressed textures), `Uint16Array` (16-bit, half) or
`Float32Array`.  Checksums are verified (`{ verify: false }` skips them);
errors are `YAPF.YapfError` with a `code` (`corrupt`, `checksum`,
`truncated`, `not-yapf`, `version`, `unsupported`).  `decode()` also reads
8-bit version 2 files.  The decoder is checked against the C library on the
files in `test-data/`.
