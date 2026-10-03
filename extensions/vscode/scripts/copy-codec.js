// Copies the shared JavaScript codec into media/ so the extension is self-contained.
const fs = require('fs');
const path = require('path');
const src = path.join(__dirname, '..', '..', '..', 'bindings', 'js', 'yapf.js');
fs.copyFileSync(src, path.join(__dirname, '..', 'media', 'yapf.js'));
console.log('copied', path.relative(process.cwd(), src), '-> media/yapf.js');
