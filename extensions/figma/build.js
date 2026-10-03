// node build.js — inlines the YAPF and PNG codecs into ui.html (Figma needs one file).
const fs = require('fs');
const path = require('path');
const read = (p) => fs.readFileSync(path.join(__dirname, p), 'utf8');
const html = read('src/ui.src.html')
  .replace('/*__YAPF_JS__*/', () => read('../../bindings/js/yapf.js'))
  .replace('/*__PNG_JS__*/', () => read('src/png.js'));
fs.writeFileSync(path.join(__dirname, 'ui.html'), html);
console.log('wrote ui.html (' + html.length + ' bytes)');
