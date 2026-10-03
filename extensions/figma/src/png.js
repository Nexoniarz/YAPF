/*
 * png.js — exact 8-bit PNG encode / decode for the browser and Node 18+.
 * Uses the platform's CompressionStream / DecompressionStream for zlib.
 * Canvas round-trips premultiply alpha and are not lossless; this is.
 * Copyright 2026 Nexoniarz — Apache License 2.0.
 */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.PNG = factory();
}(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const CRC = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    CRC[n] = c >>> 0;
  }
  function crc32(bytes, start, end) {
    let c = 0xffffffff;
    for (let i = start; i < end; i++) c = CRC[(c ^ bytes[i]) & 255] ^ (c >>> 8);
    return (c ^ 0xffffffff) >>> 0;
  }

  async function zlib(data, mode) {
    const stream = new Blob([data]).stream().pipeThrough(
      mode === 'deflate' ? new CompressionStream('deflate') : new DecompressionStream('deflate'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }

  const COLOR_TYPE = { 1: 0, 2: 4, 3: 2, 4: 6 };          // channels → PNG colour type
  const CHANNELS = { 0: 1, 4: 2, 2: 3, 6: 4 };

  function paeth(a, b, c) {
    const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
  }

  /* pixels: Uint8Array of w*h*ch bytes, rows top to bottom. */
  async function encode(pixels, w, h, ch) {
    const stride = w * ch, raw = new Uint8Array((stride + 1) * h), row = new Uint8Array(stride);
    for (let y = 0; y < h; y++) {
      const cur = pixels.subarray(y * stride, (y + 1) * stride);
      const prev = y ? pixels.subarray((y - 1) * stride, y * stride) : null;
      let best = 0, bestSum = Infinity;
      for (let f = 0; f < 5; f++) {                         // pick the filter with the smallest sum
        let sum = 0;
        for (let i = 0; i < stride; i++) {
          const a = i >= ch ? cur[i - ch] : 0, b = prev ? prev[i] : 0, c = prev && i >= ch ? prev[i - ch] : 0;
          const v = (cur[i] - [0, a, b, (a + b) >> 1, paeth(a, b, c)][f]) & 255;
          sum += v < 128 ? v : 256 - v;
        }
        if (sum < bestSum) { bestSum = sum; best = f; }
      }
      const o = y * (stride + 1);
      raw[o] = best;
      for (let i = 0; i < stride; i++) {
        const a = i >= ch ? cur[i - ch] : 0, b = prev ? prev[i] : 0, c = prev && i >= ch ? prev[i - ch] : 0;
        row[i] = (cur[i] - [0, a, b, (a + b) >> 1, paeth(a, b, c)][best]) & 255;
      }
      raw.set(row, o + 1);
    }
    const idat = await zlib(raw, 'deflate');
    const chunks = [];
    const chunk = (type, data) => {
      const c = new Uint8Array(12 + data.length), dv = new DataView(c.buffer);
      dv.setUint32(0, data.length);
      for (let i = 0; i < 4; i++) c[4 + i] = type.charCodeAt(i);
      c.set(data, 8);
      dv.setUint32(8 + data.length, crc32(c, 4, 8 + data.length));
      chunks.push(c);
    };
    const ihdr = new Uint8Array(13), dv = new DataView(ihdr.buffer);
    dv.setUint32(0, w); dv.setUint32(4, h);
    ihdr.set([8, COLOR_TYPE[ch], 0, 0, 0], 8);
    chunk('IHDR', ihdr); chunk('IDAT', idat); chunk('IEND', new Uint8Array(0));
    const total = 8 + chunks.reduce((n, c) => n + c.length, 0), out = new Uint8Array(total);
    out.set([137, 80, 78, 71, 13, 10, 26, 10]);
    let p = 8;
    for (const c of chunks) { out.set(c, p); p += c.length; }
    return out;
  }

  /* Returns { width, height, channels, pixels } or throws for PNGs it does
   * not handle (16-bit, interlaced); callers can fall back to a canvas. */
  async function decode(bytes) {
    const sig = [137, 80, 78, 71, 13, 10, 26, 10];
    if (bytes.length < 8 || sig.some((v, i) => bytes[i] !== v)) throw new Error('not a PNG');
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let w = 0, h = 0, depth = 0, type = 0, interlace = 0, palette = null, trns = null;
    const idat = [];
    for (let p = 8; p + 8 <= bytes.length;) {
      const len = dv.getUint32(p), name = String.fromCharCode(...bytes.subarray(p + 4, p + 8));
      const data = bytes.subarray(p + 8, p + 8 + len);
      if (name === 'IHDR') { w = dv.getUint32(p + 8); h = dv.getUint32(p + 12); depth = data[8]; type = data[9]; interlace = data[12]; }
      else if (name === 'PLTE') palette = data;
      else if (name === 'tRNS') trns = data;
      else if (name === 'IDAT') idat.push(data);
      else if (name === 'IEND') break;
      p += 12 + len;
    }
    if (depth !== 8 || interlace !== 0 || !(type in CHANNELS || type === 3))
      throw new Error('unsupported PNG (needs 8-bit, non-interlaced)');
    const inCh = type === 3 ? 1 : CHANNELS[type], stride = w * inCh;
    const raw = await zlib(new Uint8Array(await new Blob(idat).arrayBuffer()), 'inflate');
    const px = new Uint8Array(stride * h);
    for (let y = 0; y < h; y++) {
      const f = raw[y * (stride + 1)], src = y * (stride + 1) + 1, o = y * stride;
      for (let i = 0; i < stride; i++) {
        const a = i >= inCh ? px[o + i - inCh] : 0, b = y ? px[o - stride + i] : 0;
        const c = y && i >= inCh ? px[o - stride + i - inCh] : 0;
        px[o + i] = (raw[src + i] + [0, a, b, (a + b) >> 1, paeth(a, b, c)][f]) & 255;
      }
    }
    if (type !== 3) return { width: w, height: h, channels: inCh, pixels: px };
    const ch = trns ? 4 : 3, out = new Uint8Array(w * h * ch);     // expand a palette
    for (let i = 0; i < w * h; i++) {
      const k = px[i];
      out.set(palette.subarray(k * 3, k * 3 + 3), i * ch);
      if (trns) out[i * ch + 3] = k < trns.length ? trns[k] : 255;
    }
    return { width: w, height: h, channels: ch, pixels: out };
  }

  return { encode, decode };
}));
