/* node bindings/js/test.js — round-trips, the sample file, and corrupt input. */
'use strict';
const fs = require('fs');
const path = require('path');
const YAPF = require('./yapf.js');

let tests = 0, fails = 0;
const check = (ok, msg) => { tests++; if (!ok) { fails++; console.log('FAIL:', msg); } };

let seed = 12345;
const rnd = () => (seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) >>> 8;

function fill(w, h, ch, kind) {
  const p = new Uint8Array(w * h * ch);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) for (let c = 0; c < ch; c++) {
    const i = (y * w + x) * ch + c;
    p[i] = kind === 0 ? rnd() : kind === 1 ? x * 3 + y * 5 + c * 40
         : kind === 2 ? (((x / 7 | 0) + (y / 5 | 0) + c) & 1 ? 200 : 30)
         : kind === 3 ? ((x * x + y * y) / 50 | 0) + rnd() % 5
         : c === ch - 1 ? 255 : x ^ y;
  }
  return p;
}

for (const [w, h] of [[1, 1], [7, 1], [1, 9], [63, 65], [64, 64], [97, 61], [200, 130]])
  for (let ch = 1; ch <= 4; ch++) for (let kind = 0; kind < 5; kind++) {
    const pixels = fill(w, h, ch, kind);
    const back = YAPF.decode(YAPF.encode({ width: w, height: h, channels: ch, pixels }));
    check(back.width === w && back.height === h && back.channels === ch &&
          Buffer.compare(Buffer.from(back.pixels), Buffer.from(pixels)) === 0,
          `roundtrip ${w}x${h} ch${ch} kind${kind}`);
  }

/* The bundled sample: decode to the reference pixels, re-encode to the same bytes. */
const samplePath = path.join(__dirname, '..', '..', 'other', 'YAPF.YAPF');
if (fs.existsSync(samplePath)) {
  const file = fs.readFileSync(samplePath);
  const img = YAPF.decode(file);
  let h = 2166136261;
  for (const v of img.pixels) h = Math.imul(h ^ v, 16777619) >>> 0;
  check(h === 0x8891c2b4, 'sample decodes to the reference pixels');
  check(Buffer.compare(Buffer.from(YAPF.encode(img)), file) === 0,
        'sample re-encodes to byte-identical output (matches the C encoder)');
}

/* Corrupt input must throw, never hang or return garbage silently. */
const good = YAPF.encode({ width: 130, height: 70, channels: 4, pixels: fill(130, 70, 4, 3) });
let threwOnTruncation = true;
for (let i = 0; i < 2000; i++) {
  const bad = good.slice(0, i % 3 === 1 ? rnd() % good.length : good.length);
  if (i % 3 !== 1) bad[rnd() % bad.length] ^= 1 << (rnd() % 8);
  try { YAPF.decode(bad); if (i % 3 === 1) threwOnTruncation = false; } catch (e) { /* expected */ }
}
check(threwOnTruncation, 'truncated files are rejected');

console.log(`${tests} tests, ${fails} failures`);
process.exit(fails ? 1 : 0);
