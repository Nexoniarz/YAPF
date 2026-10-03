/*
 * yapf.js — YAPF v1 encoder / decoder in plain JavaScript.
 *
 * Copyright 2026 Nexoniarz — Apache License 2.0.
 *
 * Works in browsers, Node.js, Figma plugins and VS Code webviews; no
 * dependencies.  Produces exactly the same bytes as the C library.
 *
 *   const img  = YAPF.decode(bytes);    // Uint8Array → image
 *   const file = YAPF.encode(img);      // image → Uint8Array
 *   const rgba = YAPF.toRGBA(img);      // any channel count → RGBA pixels
 *
 * An image is { width, height, channels (1–4), pixels: Uint8Array,
 *               gpuFormat, flags, mipLevels, mips: [Uint8Array, …] }.
 * The specification is the comment at the top of yapf.h.
 */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.YAPF = factory();
}(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  const TILE = 64, TILE_PX = TILE * TILE, GROUP = 8, BAND = 8, RING = 16;
  const MAX_GROUPS = TILE_PX / GROUP, MAX_DIM = 65535, MAX_MIPS = 16;
  const STORED = 0x80, SUBGREEN = 0x10, NIB_REPEAT = 9;
  const F_LEFT = 0, F_GRAD = 1, F_UPRIGHT = 2, F_UPAVG = 3, F_UP = 16;
  const NIB_GROUPS = [1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 3, 4, 8, 16, 32, 64];
  const RUN_LENS = [2, 3, 4, 8, 16, 32, 64];
  /* Encoder try order: cheapest to decode first, ties go to the earlier. */
  const FILTERS = [F_UP, F_UP + 1, F_UP + 2, F_UP + 3, F_UP + 4, F_UP + 5,
                   F_UP + 6, F_UP + 7, F_UPRIGHT, F_UPAVG, F_LEFT, F_GRAD];

  const GPU = { RGBA8: 0, RGB8: 1, RG8: 2, R8: 3, SRGB8_A8: 4, SRGB8: 5 };
  const FLAG = { PREMULT_ALPHA: 1, SRGB: 2 };

  const ZERO_ROW = new Uint8Array(TILE + 1);

  const zz   = (r) => ((r << 1) ^ ((r << 24) >> 31)) & 255;  /* r is 0..255 */
  const unzz = (z) => ((z >>> 1) ^ -(z & 1)) & 255;
  const width = (z) => (z === 0 ? 0 : 32 - Math.clz32(z));
  const s8 = (v) => (v << 24) >> 24;
  const filterValid = (f) => f <= F_UPAVG || (f >= F_UP && f < F_UP + 8);
  const upK = (f) => (f >= F_UP ? f - F_UP + 1 : 1);

  function corrupt() { throw new Error('YAPF: corrupt or unsupported file'); }

  /* ── Decoder ──────────────────────────────────────────────────────── */

  function unpackPlane(b, p, end, z, tw, th) {
    const total = tw * th, ng = (total + GROUP - 1) >> 3, nb = (th + BAND - 1) >> 3;
    if (end - p < nb) corrupt();
    const filt = b.subarray(p, p + nb);
    p += nb;
    for (let i = 0; i < nb; i++) if (!filterValid(filt[i])) corrupt();

    const gw = new Uint8Array(ng + 64);
    let data = 0, g = 0, hi = 0, w = 0;
    while (g < ng) {
      if (p >= end) corrupt();
      const c = (b[p] >> (hi * 4)) & 15, n = NIB_GROUPS[c];
      p += hi; hi ^= 1;
      if (c < NIB_REPEAT) w = c;
      gw.fill(w, g, g + n);
      data += w * n; g += n;
    }
    if (g !== ng) corrupt();
    if (hi) { if (b[p] >> 4) corrupt(); p++; }
    if (end - p < data) corrupt();

    for (g = 0; g < ng; g++) {
      w = gw[g];
      const o = g * GROUP, mask = (1 << w) - 1;
      let acc = 0, bits = 0;
      for (let i = 0; i < GROUP; i++) {
        while (bits < w) { acc |= b[p++] << bits; bits += 8; }
        z[o + i] = acc & mask;
        acc >>>= w; bits -= w;
      }
    }
    return { p, filt };
  }

  function unfilterRow(f, R, r0, U, u0, K, k0, Z, z0, tw) {
    if (f === F_LEFT) {
      let L = 0;
      for (let x = 0; x < tw; x++) R[r0 + x] = L = (L + unzz(Z[z0 + x])) & 255;
    } else if (f === F_GRAD) {
      let L = (U[u0] + unzz(Z[z0])) & 255;
      R[r0] = L;
      for (let x = 1; x < tw; x++)
        R[r0 + x] = L = (L + U[u0 + x] - U[u0 + x - 1] + unzz(Z[z0 + x])) & 255;
    } else {
      /* pred = (a + b) >> 1: Up k a = b = K;  Up-right a = b = U + 1;
       * Up-average a = U, b = U + 1.  Rows carry one extra sample. */
      let A, a0, B, b0;
      if (f >= F_UP) { A = K; a0 = k0; B = K; b0 = k0; }
      else if (f === F_UPAVG) { A = U; a0 = u0; B = U; b0 = u0 + 1; }
      else { A = U; a0 = u0 + 1; B = U; b0 = u0 + 1; }
      for (let x = 0; x < tw; x++)
        R[r0 + x] = (((A[a0 + x] + B[b0 + x]) >> 1) + unzz(Z[z0 + x])) & 255;
    }
    R[r0 + tw] = R[r0 + tw - 1];
  }

  function decodeTile(b, p, end, out, imgW, tx, ty, tw, th, ch) {
    if (end <= p) corrupt();
    let hdr = b[p++];
    if (hdr === STORED) {
      const rowb = tw * ch;
      if (end - p !== rowb * th) corrupt();
      for (let y = 0; y < th; y++)
        out.set(b.subarray(p + rowb * y, p + rowb * (y + 1)), ((ty + y) * imgW + tx) * ch);
      return;
    }
    if (hdr & 0xE0) corrupt();                         /* reserved bits set */
    const xf = hdr & SUBGREEN;
    hdr &= 0x0F;
    if ((hdr >> ch) || (xf && ch < 3)) corrupt();

    const rowLen = TILE + 1, planeLen = RING * rowLen;
    const rows = new Uint8Array(4 * planeLen);
    const z = [], filt = [null, null, null, null];
    for (let c = 0; c < ch; c++) {
      if (!(hdr & (1 << c))) continue;
      if (p >= end) corrupt();
      rows.fill(b[p++], c * planeLen, (c + 1) * planeLen);
    }
    for (let c = 0; c < ch; c++) {
      if (hdr & (1 << c)) continue;
      z[c] = new Uint8Array(TILE_PX + GROUP);
      const r = unpackPlane(b, p, end, z[c], tw, th);
      p = r.p; filt[c] = r.filt;
    }
    if (p !== end) corrupt();

    for (let y = 0; y < th; y++) {
      const ri = (y % RING) * rowLen;
      for (let c = 0; c < ch; c++) {
        if (!filt[c]) continue;
        const f = filt[c][y >> 3], k = upK(f), base = c * planeLen;
        unfilterRow(f, rows, base + ri,
                    y ? rows : ZERO_ROW, y ? base + ((y - 1) % RING) * rowLen : 0,
                    y >= k ? rows : ZERO_ROW, y >= k ? base + ((y - k) % RING) * rowLen : 0,
                    z[c], y * tw, tw);
      }
      let d = ((ty + y) * imgW + tx) * ch;
      const P0 = ri, P1 = planeLen + ri, P2 = 2 * planeLen + ri, P3 = 3 * planeLen + ri;
      for (let x = 0; x < tw; x++, d += ch) {
        if (ch >= 3) {
          if (xf) {
            const g = rows[P0 + x];
            out[d] = rows[P1 + x] + g - 128;
            out[d + 1] = g;
            out[d + 2] = rows[P2 + x] + g - 128;
          } else {
            const co = (rows[P1 + x] - 128) & 255, cg = (rows[P2 + x] - 128) & 255;
            const t = (rows[P0 + x] - (s8(cg) >> 1)) & 255;
            const bb = (t - (s8(co) >> 1)) & 255;
            out[d] = bb + co;
            out[d + 1] = cg + t;
            out[d + 2] = bb;
          }
          if (ch === 4) out[d + 3] = rows[P3 + x];
        } else {
          out[d] = rows[P0 + x];
          if (ch === 2) out[d + 1] = rows[P1 + x];
        }
      }
    }
  }

  const r32 = (b, p) => (b[p] | (b[p + 1] << 8) | (b[p + 2] << 16) | (b[p + 3] << 24)) >>> 0;

  function decode(input) {
    const b = input instanceof Uint8Array ? input : new Uint8Array(input);
    if (b.length < 20 || b[0] !== 0x59 || b[1] !== 0x41 || b[2] !== 0x50 || b[3] !== 0x46)
      throw new Error('YAPF: not a YAPF file');
    if (b[4] !== 1) throw new Error('YAPF: unsupported format version ' + b[4]);
    const ch = b[5], mipLevels = b[7], W = r32(b, 12), H = r32(b, 16);
    if (ch < 1 || ch > 4 || mipLevels < 1 || mipLevels > MAX_MIPS ||
        W < 1 || W > MAX_DIM || H < 1 || H > MAX_DIM) corrupt();
    const idxEnd = 20 + 8 * mipLevels;
    if (b.length < idxEnd) corrupt();

    const mips = [];
    for (let m = 0; m < mipLevels; m++) {
      const off = r32(b, 20 + 8 * m), len = r32(b, 24 + 8 * m);
      const mw = Math.max(1, W >>> m), mh = Math.max(1, H >>> m);
      if (off < idxEnd || off > b.length || len > b.length - off) corrupt();
      const out = new Uint8Array(mw * mh * ch);
      const tcx = Math.ceil(mw / TILE), tcy = Math.ceil(mh / TILE), tcount = tcx * tcy;
      if (len / 4 < tcount) corrupt();
      let pos = off + tcount * 4;
      for (let t = 0; t < tcount; t++) {
        const sz = r32(b, off + t * 4);
        if (sz > off + len - pos) corrupt();
        const tx = (t % tcx) * TILE, ty = Math.floor(t / tcx) * TILE;
        decodeTile(b, pos, pos + sz, out, mw, tx, ty,
                   Math.min(TILE, mw - tx), Math.min(TILE, mh - ty), ch);
        pos += sz;
      }
      mips.push(out);
    }
    return { width: W, height: H, channels: ch, gpuFormat: b[6], flags: b[8],
             mipLevels, pixels: mips[0], mips };
  }

  /* ── Encoder ──────────────────────────────────────────────────────── */

  class Out {
    constructor(n) { this.buf = new Uint8Array(Math.max(n, 1024)); this.len = 0; }
    reserve(n) {
      if (this.len + n <= this.buf.length) return;
      let c = this.buf.length * 2;
      while (c < this.len + n) c *= 2;
      const nb = new Uint8Array(c); nb.set(this.buf.subarray(0, this.len)); this.buf = nb;
    }
    push(v) { this.reserve(1); this.buf[this.len++] = v; }
  }

  function predictRow(f, P, r0, u0, k0, pred, tw, U, K) {
    switch (f) {
      case F_LEFT: pred[0] = 0; for (let x = 1; x < tw; x++) pred[x] = P[r0 + x - 1]; break;
      case F_GRAD:
        pred[0] = U[u0];
        for (let x = 1; x < tw; x++) pred[x] = (P[r0 + x - 1] + U[u0 + x] - U[u0 + x - 1]) & 255;
        break;
      case F_UPRIGHT:
        for (let x = 0; x + 1 < tw; x++) pred[x] = U[u0 + x + 1];
        pred[tw - 1] = U[u0 + tw - 1];
        break;
      case F_UPAVG:
        for (let x = 0; x + 1 < tw; x++) pred[x] = (U[u0 + x] + U[u0 + x + 1]) >> 1;
        pred[tw - 1] = U[u0 + tw - 1];
        break;
      default: for (let x = 0; x < tw; x++) pred[x] = K[k0 + x];
    }
  }

  /* Picks a filter per band; fills z and filt; returns estimated cost. */
  function chooseFilters(P, tw, th, z, filt) {
    const total = tw * th, nb = (th + BAND - 1) >> 3;
    const tmp = new Uint8Array(BAND * TILE + GROUP), pred = new Uint8Array(TILE);
    let sum = 0, prevFi = 0;
    for (let b = 0; b < nb; b++) {
      const y0 = b * BAND, y1 = Math.min(y0 + BAND, th), n = (y1 - y0) * tw;
      let bestCost = Infinity, bestFi = 0;
      for (let t = 0; t < FILTERS.length; t++) {
        const fi = t === 0 ? prevFi : (t <= prevFi ? t - 1 : t);
        const f = FILTERS[fi], k = upK(f), serial = f === F_LEFT || f === F_GRAD;
        let cost = 0, done = 0, lost = false;
        for (let y = y0; y < y1 && !lost; y++) {
          const r0 = y * tw;
          const U = y ? P : ZERO_ROW, u0 = y ? r0 - tw : 0;
          const K = y >= k ? P : ZERO_ROW, k0 = y >= k ? r0 - k * tw : 0;
          predictRow(f, P, r0, u0, k0, pred, tw, U, K);
          const T = (y - y0) * tw;
          for (let x = 0; x < tw; x++) tmp[T + x] = zz((P[r0 + x] - pred[x]) & 255);
          const last = y + 1 === y1, end = last ? n : (y + 1 - y0) * tw;
          if (last) tmp.fill(0, n, n + GROUP);
          for (; done + GROUP <= end || (last && done < end); done += GROUP) {
            let m = 0;
            for (let i = 0; i < GROUP; i++) m |= tmp[done + i];
            const w = width(m);
            cost += w ? 16 * w + 8 : 2;
          }
          const c = serial ? cost + Math.floor(cost / 25) : cost;
          if (c > bestCost || (c === bestCost && fi > bestFi)) lost = true;
        }
        if (lost) continue;
        if (serial) cost += Math.floor(cost / 25);
        if (cost < bestCost || (cost === bestCost && fi < bestFi)) {
          bestCost = cost; bestFi = fi;
          z.set(tmp.subarray(0, n), y0 * tw);
        }
      }
      prevFi = bestFi;
      filt[b] = FILTERS[bestFi];
      sum += bestCost;
    }
    z.fill(0, total, total + GROUP);
    return sum;
  }

  function emitPlane(o, z, filt, tw, th) {
    const total = tw * th, ng = (total + GROUP - 1) >> 3, nb = (th + BAND - 1) >> 3;
    o.reserve(nb + ng + ng * 8);
    for (let i = 0; i < nb; i++) o.buf[o.len++] = filt[i];

    const need = new Uint8Array(ng);
    for (let g = 0; g < ng; g++) {
      let m = 0;
      for (let i = 0; i < GROUP; i++) m |= z[g * GROUP + i];
      need[g] = width(m);
    }
    /* span[j][g] = widest need in [g, g + 2^j) */
    const span = [need.slice()];
    for (let j = 1; j < 7; j++) {
      const prev = span[j - 1], cur = new Uint8Array(ng), h = 1 << (j - 1);
      for (let g = 0; g + (1 << j) <= ng; g++) cur[g] = Math.max(prev[g], prev[g + h]);
      span.push(cur);
    }
    const cost = new Uint32Array((ng + 1) * 9), pick = new Uint8Array(ng * 9);
    const mx = new Uint8Array(7);
    for (let g = ng - 1; g >= 0; g--) {
      let lc = Infinity, lk = 0;
      for (let w = need[g]; w <= 8; w++) {
        const c = 1 + 2 * w + cost[(g + 1) * 9 + w];
        if (c < lc) { lc = c; lk = w; }
      }
      let nl = 0;
      for (; nl < 7 && g + RUN_LENS[nl] <= ng; nl++) {
        const L = RUN_LENS[nl];
        mx[nl] = L === 3 ? Math.max(span[1][g], need[g + 2]) : span[31 - Math.clz32(L)][g];
      }
      for (let pw = 0; pw <= 8; pw++) {
        let bc = lc, bk = lk;
        for (let li = 0; li < nl && mx[li] <= pw; li++) {
          const L = RUN_LENS[li], c = 1 + 2 * pw * L + cost[(g + L) * 9 + pw];
          if (c < bc) { bc = c; bk = NIB_REPEAT + li; }
        }
        cost[g * 9 + pw] = bc;
        pick[g * 9 + pw] = bk;
      }
    }
    const gw = new Uint8Array(ng), nib = [];
    let pw = 0;
    for (let g = 0; g < ng;) {
      const k = pick[g * 9 + pw];
      nib.push(k);
      if (k < NIB_REPEAT) { pw = k; gw[g++] = k; }
      else for (let n = 0; n < NIB_GROUPS[k]; n++) gw[g++] = pw;
    }
    for (let k = 0; k < nib.length; k += 2)
      o.buf[o.len++] = nib[k] | (k + 1 < nib.length ? nib[k + 1] << 4 : 0);

    for (let g = 0; g < ng; g++) {
      const w = gw[g];
      let acc = 0, bits = 0;
      for (let i = 0; i < GROUP; i++) {
        acc |= z[g * GROUP + i] << bits; bits += w;
        while (bits >= 8) { o.buf[o.len++] = acc & 255; acc >>>= 8; bits -= 8; }
      }
    }
  }

  function encodeTile(o, sc, px, imgW, tx, ty, tw, th, ch) {
    const total = tw * th, start = o.len, raw = total * ch, tries = ch >= 3 ? 2 : 1;
    const mask = [0, 0], est = [0, 0];
    for (let t = 0; t < tries; t++) {
      const plane = sc.plane[t];
      for (let y = 0; y < th; y++) {
        let s = ((ty + y) * imgW + tx) * ch;
        for (let x = 0; x < tw; x++, s += ch) {
          const i = y * tw + x;
          if (ch < 3) { for (let c = 0; c < ch; c++) plane[c][i] = px[s + c]; continue; }
          const r = px[s], g = px[s + 1], b = px[s + 2];
          if (t === 0) {
            const oo = (r - b) & 255, tt = (b + (s8(oo) >> 1)) & 255, q = (g - tt) & 255;
            plane[0][i] = (tt + (s8(q) >> 1)) & 255;
            plane[1][i] = (oo + 128) & 255;
            plane[2][i] = (q + 128) & 255;
          } else {
            plane[0][i] = g;
            plane[1][i] = (r - g + 128) & 255;
            plane[2][i] = (b - g + 128) & 255;
          }
          if (ch === 4) plane[3][i] = px[s + 3];
        }
      }
      for (let c = 0; c < ch; c++) {
        const P = plane[c];
        let i = 1;
        while (i < total && P[i] === P[0]) i++;
        if (i === total) { mask[t] |= 1 << c; est[t] += 16; continue; }
        est[t] += chooseFilters(P, tw, th, sc.z[t][c], sc.filt[t][c]);
      }
    }
    const t = tries === 2 && est[1] < est[0] ? 1 : 0;
    o.reserve(5);
    o.buf[o.len++] = mask[t] | (t ? SUBGREEN : 0);
    for (let c = 0; c < ch; c++) if (mask[t] & (1 << c)) o.buf[o.len++] = sc.plane[t][c][0];
    for (let c = 0; c < ch; c++)
      if (!(mask[t] & (1 << c))) emitPlane(o, sc.z[t][c], sc.filt[t][c], tw, th);

    if (o.len - start > raw + 1) {
      o.len = start;
      o.reserve(raw + 1);
      o.buf[o.len++] = STORED;
      for (let y = 0; y < th; y++) {
        const s = ((ty + y) * imgW + tx) * ch;
        o.buf.set(px.subarray(s, s + tw * ch), o.len);
        o.len += tw * ch;
      }
    }
  }

  function encodeLevel(px, mw, mh, ch) {
    const tcx = Math.ceil(mw / TILE), tcy = Math.ceil(mh / TILE), tcount = tcx * tcy;
    const o = new Out(tcount * 4 + (mw * mh * ch >> 1));
    o.reserve(tcount * 4);
    o.len = tcount * 4;
    const mk = (n) => [0, 1, 2, 3].map(() => new Uint8Array(n));
    const sc = {
      plane: [mk(TILE_PX), mk(TILE_PX)],
      z: [mk(TILE_PX + GROUP), mk(TILE_PX + GROUP)],
      filt: [mk(TILE / BAND), mk(TILE / BAND)],
    };
    let t = 0;
    for (let ty = 0; ty < tcy; ty++)
      for (let tx = 0; tx < tcx; tx++, t++) {
        const x0 = tx * TILE, y0 = ty * TILE, start = o.len;
        encodeTile(o, sc, px, mw, x0, y0, Math.min(TILE, mw - x0), Math.min(TILE, mh - y0), ch);
        const n = o.len - start;
        o.buf[t * 4] = n & 255; o.buf[t * 4 + 1] = (n >>> 8) & 255;
        o.buf[t * 4 + 2] = (n >>> 16) & 255; o.buf[t * 4 + 3] = (n >>> 24) & 255;
      }
    return o.buf.subarray(0, o.len);
  }

  function encode(img) {
    const { width: W, height: H, channels: ch } = img;
    const mips = img.mips && img.mips.length ? img.mips : [img.pixels];
    const mipLevels = img.mipLevels || mips.length;
    if (!(W >= 1 && W <= MAX_DIM && H >= 1 && H <= MAX_DIM && ch >= 1 && ch <= 4 &&
          mipLevels >= 1 && mipLevels <= MAX_MIPS && mips.length >= mipLevels))
      throw new Error('YAPF: invalid image');
    const levels = [];
    for (let m = 0; m < mipLevels; m++) {
      const mw = Math.max(1, W >>> m), mh = Math.max(1, H >>> m);
      const px = m === 0 ? img.pixels : mips[m];
      if (!px || px.length < mw * mh * ch) throw new Error('YAPF: mip ' + m + ' has too few pixels');
      levels.push(encodeLevel(px, mw, mh, ch));
    }
    const idxEnd = 20 + 8 * mipLevels;
    let total = idxEnd;
    for (const l of levels) total += l.length;
    const out = new Uint8Array(total);
    out.set([0x59, 0x41, 0x50, 0x46, 1, ch, img.gpuFormat || 0, mipLevels, img.flags || 0]);
    const w32 = (p, v) => { out[p] = v & 255; out[p + 1] = (v >>> 8) & 255;
                            out[p + 2] = (v >>> 16) & 255; out[p + 3] = (v >>> 24) & 255; };
    w32(12, W); w32(16, H);
    let off = idxEnd;
    levels.forEach((l, m) => { w32(20 + 8 * m, off); w32(24 + 8 * m, l.length); out.set(l, off); off += l.length; });
    return out;
  }

  /* Any channel count → RGBA (for canvas ImageData). */
  function toRGBA(img, level) {
    const m = level || 0, px = img.mips ? img.mips[m] : img.pixels;
    const w = Math.max(1, img.width >>> m), h = Math.max(1, img.height >>> m), ch = img.channels;
    if (ch === 4) return new Uint8ClampedArray(px.buffer, px.byteOffset, w * h * 4);
    const out = new Uint8ClampedArray(w * h * 4);
    for (let i = 0, s = 0; i < w * h; i++, s += ch) {
      const o = i * 4;
      if (ch >= 3) { out[o] = px[s]; out[o + 1] = px[s + 1]; out[o + 2] = px[s + 2]; out[o + 3] = 255; }
      else { out[o] = out[o + 1] = out[o + 2] = px[s]; out[o + 3] = ch === 2 ? px[s + 1] : 255; }
    }
    return out;
  }

  return { decode, encode, toRGBA, GPU, FLAG, VERSION: 1 };
}));
