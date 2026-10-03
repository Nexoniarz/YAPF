// node example.mjs ../../other/YAPF.YAPF
import { readFileSync } from 'node:fs';
import { createRequire } from 'node:module';
const YAPF = createRequire(import.meta.url)('./yapf.js');

const path = process.argv[2] ?? '../../other/YAPF.YAPF';
const file = readFileSync(path);

let img = YAPF.decode(file);
let best = Infinity;                                   // best of 10 decodes
for (let i = 0; i < 10; i++) {
  const t = performance.now();
  img = YAPF.decode(file);
  best = Math.min(best, performance.now() - t);
}
console.log(`${path}: ${img.width}x${img.height}, ${img.channels} channels, decoded in ${best.toFixed(2)} ms (pure JavaScript)`);
console.log(`file is ${file.length} bytes, ${(100 * file.length / img.pixels.length).toFixed(1)}% of the raw pixels`);
const again = YAPF.encode(img);
console.log(`re-encoded: ${again.length} bytes, identical: ${Buffer.compare(Buffer.from(again), file) === 0}`);
