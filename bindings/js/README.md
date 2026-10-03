# yapf.js — YAPF in JavaScript

Encoder and decoder for YAPF v1 in one dependency-free file.  Works in
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

Tests: `node test.js`.
