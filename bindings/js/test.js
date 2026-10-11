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

/* Version 2 fixtures written by the C library: every surface and the
 * preview must match what C decodes (FNV-1a hashes in v2-expected.json). */
const fnv = (u8) => { let h = 2166136261; for (const v of u8) h = Math.imul(h ^ v, 16777619) >>> 0; return h; };
const dataDir = path.join(__dirname, 'test-data');
const expected = JSON.parse(fs.readFileSync(path.join(dataDir, 'v2-expected.json'), 'utf8'));
for (const [name, want] of Object.entries(expected)) {
  const bytes = fs.readFileSync(path.join(dataDir, name));
  let tex = null;
  try { tex = YAPF.decodeTexture(bytes); } catch (e) { check(false, `${name}: ${e.message}`); continue; }
  const got = tex.surfaces.map((s) => fnv(new Uint8Array(s.buffer, s.byteOffset, s.byteLength)));
  check(JSON.stringify(got) === JSON.stringify(want.surfaces), `${name}: surfaces match the C decoder`);
  if (want.preview0 !== null) {
    const view = YAPF.textureToRGBA(tex, 0, 0);
    /* float previews may differ by 1 in rounding; compare only exact kinds */
    if (tex.payload === 1 ? tex.format !== 0x47 && tex.format !== 0x48 : tex.sample < 2)
      check(fnv(view.rgba) === want.preview0, `${name}: preview matches the C library`);
  }
  /* damage must be caught by the checksums */
  const broken = Uint8Array.from(bytes);
  broken[broken.length - 3] ^= 0x10;
  let code = null;
  try { YAPF.decodeTexture(broken); } catch (e) { code = e.code; }
  check(code === 'checksum', `${name}: a damaged byte is reported as a checksum error`);
}
{ /* v1 decode() reads 8-bit v2 files and names what it cannot read */
  const cube = YAPF.decode(fs.readFileSync(path.join(dataDir, 'v2-cube-mips.yapf')));
  check(cube.mipLevels === 7 && cube.width === 64, 'decode() reads face 0 of a v2 cube');
  let msg = '';
  try { YAPF.decode(fs.readFileSync(path.join(dataDir, 'v2-bc7-encoded.yapf'))); } catch (e) { msg = e.message; }
  check(/decodeTexture/.test(msg), 'decode() points GPU-block files to decodeTexture');
  const yapfSum = (t) => YAPF.checksum(new TextEncoder().encode(t));
  check(yapfSum('') === 0xd7550ed0 && yapfSum('YAPF') === 0xc8cc41d4, 'yapf-sum test vectors');
}

console.log(`${tests} tests, ${fails} failures`);
process.exit(fails ? 1 : 0);
