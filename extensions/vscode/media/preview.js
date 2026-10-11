// Webview side of the YAPF preview: decode, draw, zoom, surface pickers, info.
(function () {
  const vscode = typeof acquireVsCodeApi === 'function' ? acquireVsCodeApi() : null;
  const canvas = document.getElementById('canvas');
  const info = document.getElementById('info');
  const controls = document.getElementById('controls');
  const errorBox = document.getElementById('error');
  const PIXEL_HINTS = ['RGBA8', 'RGB8', 'RG8', 'R8', 'SRGB8_A8', 'SRGB8', 'RGBA16', 'RGB16', 'RG16', 'R16',
                       'RGBA16F', 'RGB16F', 'RG16F', 'R16F', 'RGBA32F', 'RGB32F', 'RG32F', 'R32F'];
  const BLOCKS = { 0x40: 'BC1', 0x41: 'BC2', 0x42: 'BC3', 0x43: 'BC4', 0x44: 'BC4 snorm', 0x45: 'BC5',
                   0x46: 'BC5 snorm', 0x47: 'BC6H', 0x48: 'BC6H signed', 0x49: 'BC7', 0x50: 'ETC2 RGB',
                   0x51: 'ETC2 RGB A1', 0x52: 'ETC2 RGBA', 0x53: 'EAC R11', 0x54: 'EAC R11 snorm',
                   0x55: 'EAC RG11', 0x56: 'EAC RG11 snorm' };
  ['4×4', '5×4', '5×5', '6×5', '6×6', '8×5', '8×6', '8×8', '10×5', '10×6', '10×8', '10×10', '12×10', '12×12']
    .forEach((n, i) => { BLOCKS[0x60 + i] = 'ASTC ' + n; });
  const CH_NAMES = ['', 'Gray', 'Gray + Alpha', 'RGB', 'RGBA'];
  const SAMPLES = ['8-bit', '16-bit', 'half float', 'float'];
  const FACES = ['+X', '−X', '+Y', '−Y', '+Z', '−Z'];

  let tex = null, fit = true, zoom = 1;
  const sel = { mip: 0, face: 0, layer: 0, frame: 0, slice: 0 };

  function kb(n) { return n < 1024 * 1024 ? (n / 1024).toFixed(1) + ' KB' : (n / 1048576).toFixed(2) + ' MB'; }

  function mipDims(m) {
    return [Math.max(1, tex.width >>> m), Math.max(1, tex.height >>> m),
            tex.shape === 2 ? Math.max(1, tex.depth >>> m) : 1];
  }

  function draw() {
    const s = YAPF.surfaceIndex(tex, sel.frame, sel.layer, sel.face, sel.mip);
    sel.slice = Math.min(sel.slice, mipDims(sel.mip)[2] - 1);
    try {
      const view = YAPF.textureToRGBA(tex, s, sel.slice);
      canvas.width = view.width; canvas.height = view.height;
      canvas.getContext('2d').putImageData(new ImageData(view.rgba, view.width, view.height), 0, 0);
      errorBox.hidden = true;
    } catch (e) {
      errorBox.hidden = false;
      errorBox.textContent = e.message;
    }
    layout();
  }

  function layout() {
    if (!tex) return;
    const w = canvas.width, h = canvas.height;
    const stage = document.getElementById('stage');
    const scale = fit ? Math.min(1, (stage.clientWidth - 32) / w, (stage.clientHeight - 32) / h) : zoom;
    canvas.style.width = Math.max(1, Math.round(w * scale)) + 'px';
    canvas.style.height = Math.max(1, Math.round(h * scale)) + 'px';
    canvas.classList.toggle('pixelated', scale >= 2);
    document.body.classList.toggle('fit', fit);
    stage.title = fit ? 'Click for 100%, Ctrl+scroll to zoom' : 'Click to fit';
  }

  function picker(key, count, label) {
    if (count <= 1) return;
    const el = document.createElement('select');
    for (let i = 0; i < count; i++) {
      const o = document.createElement('option');
      o.value = i;
      o.textContent = label(i);
      el.appendChild(o);
    }
    sel[key] = Math.min(sel[key], count - 1);
    el.value = sel[key];
    el.onchange = () => { sel[key] = +el.value; if (key === 'mip') buildControls(); draw(); };
    controls.appendChild(el);
  }

  function buildControls() {
    controls.textContent = '';
    picker('mip', tex.mipLevels, (m) => { const [w, h, d] = mipDims(m); return 'Mip ' + m + ' (' + w + '×' + h + (d > 1 ? '×' + d : '') + ')'; });
    picker('face', tex.faces, (f) => 'Face ' + FACES[f]);
    picker('layer', tex.layers, (l) => 'Layer ' + l);
    picker('frame', tex.frames, (f) => 'Frame ' + (f + 1) + (tex.frameUs[f] ? ' (' + Math.round(tex.frameUs[f] / 1000) + ' ms)' : ''));
    picker('slice', mipDims(sel.mip)[2], (z) => 'Slice ' + z);
  }

  function show(name, bytes) {
    errorBox.hidden = true;
    const t = performance.now();
    try {
      tex = YAPF.decodeTexture(bytes);
    } catch (e) {
      tex = null;
      errorBox.hidden = false;
      errorBox.textContent = name + ': ' + e.message.replace(/^YAPF: /, '');
      info.textContent = '';
      controls.textContent = '';
      return;
    }
    const ms = performance.now() - t;
    let raw = 0;
    for (let m = 0; m < tex.mipLevels; m++) {
      const [w, h, d] = mipDims(m);
      raw += w * h * d * (tex.payload ? 4 : tex.channels * [1, 2, 2, 4][tex.sample]);
    }
    raw *= tex.frames * tex.layers * tex.faces;
    const [w0, h0, d0] = mipDims(0);
    const shape = tex.shape === 1 ? (tex.layers > 1 ? 'cube array of ' + tex.layers : 'cubemap')
                : tex.shape === 2 ? 'volume' : tex.layers > 1 ? 'array of ' + tex.layers : '';
    const storage = tex.payload === 1 ? 'GPU ' + (BLOCKS[tex.format] || '?')
                  : CH_NAMES[tex.channels] + ' ' + SAMPLES[tex.sample] + ', GPU ' + (PIXEL_HINTS[tex.format] || '?');
    const parts = [
      w0 + ' × ' + h0 + (d0 > 1 ? ' × ' + d0 : ''),
      shape,
      tex.frames > 1 ? tex.frames + ' frames' : '',
      storage,
      (tex.flags & 2 ? 'sRGB' : 'linear') + (tex.flags & 1 ? ', premultiplied' : ''),
      tex.normal ? 'normal map (' + ((tex.normal & 3) === 2 ? 'DirectX' : (tex.normal & 3) === 1 ? 'OpenGL' : '?') + ')' : '',
      tex.version === 2 ? 'YAPF 2' : 'YAPF 1',
      kb(bytes.length) + (tex.payload ? '' : ' (' + (100 * bytes.length / raw).toFixed(1) + '% of raw)'),
      'decoded in ' + ms.toFixed(1) + ' ms',
    ];
    const author = tex.notes.find((n) => n.tag === 'AUTH');
    if (author) parts.push('by ' + new TextDecoder().decode(author.data));
    info.textContent = parts.filter(Boolean).join('  ·  ');
    buildControls();
    draw();
  }

  document.getElementById('stage').addEventListener('click', () => { fit = !fit; zoom = 1; layout(); });
  document.getElementById('stage').addEventListener('wheel', (e) => {
    if (!e.ctrlKey || !tex) return;
    e.preventDefault();
    fit = false;
    zoom = Math.min(64, Math.max(1 / 64, zoom * (e.deltaY < 0 ? 1.25 : 0.8)));
    layout();
  }, { passive: false });
  window.addEventListener('resize', layout);

  window.addEventListener('message', (e) => {
    const m = e.data;
    if (m.type === 'file') show(m.name, m.bytes instanceof Uint8Array ? m.bytes : new Uint8Array(m.bytes));
    if (m.type === 'error') { errorBox.hidden = false; errorBox.textContent = m.message; }
  });
  if (vscode) vscode.postMessage({ type: 'ready' });
  window.yapfPreviewShow = show;   // used by the browser test page
})();
