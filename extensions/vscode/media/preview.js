// Webview side of the YAPF preview: decode, draw, zoom, mip picker, info.
(function () {
  const vscode = typeof acquireVsCodeApi === 'function' ? acquireVsCodeApi() : null;
  const canvas = document.getElementById('canvas');
  const info = document.getElementById('info');
  const controls = document.getElementById('controls');
  const errorBox = document.getElementById('error');
  const GPU_NAMES = ['RGBA8', 'RGB8', 'RG8', 'R8', 'SRGB8_A8', 'SRGB8'];
  const CH_NAMES = ['', 'Gray', 'Gray + Alpha', 'RGB', 'RGBA'];

  let image = null, level = 0, fit = true, zoom = 1;

  function kb(n) { return n < 1024 * 1024 ? (n / 1024).toFixed(1) + ' KB' : (n / 1048576).toFixed(2) + ' MB'; }

  function draw() {
    const w = Math.max(1, image.width >>> level), h = Math.max(1, image.height >>> level);
    canvas.width = w; canvas.height = h;
    canvas.getContext('2d').putImageData(new ImageData(YAPF.toRGBA(image, level), w, h), 0, 0);
    layout();
  }

  function layout() {
    if (!image) return;
    const w = canvas.width, h = canvas.height;
    const stage = document.getElementById('stage');
    const scale = fit ? Math.min(1, (stage.clientWidth - 32) / w, (stage.clientHeight - 32) / h) : zoom;
    canvas.style.width = Math.max(1, Math.round(w * scale)) + 'px';
    canvas.style.height = Math.max(1, Math.round(h * scale)) + 'px';
    canvas.classList.toggle('pixelated', scale >= 2);
    document.body.classList.toggle('fit', fit);
    stage.title = fit ? 'Click for 100%, Ctrl+scroll to zoom' : 'Click to fit';
  }

  function show(name, bytes) {
    errorBox.hidden = true;
    const t = performance.now();
    try {
      image = YAPF.decode(bytes);
    } catch (e) {
      image = null;
      errorBox.hidden = false;
      errorBox.textContent = name + ': ' + e.message;
      info.textContent = '';
      return;
    }
    const ms = performance.now() - t;
    const raw = image.width * image.height * image.channels;
    level = Math.min(level, image.mipLevels - 1);
    info.textContent = [
      image.width + ' × ' + image.height,
      CH_NAMES[image.channels],
      (image.flags & 2 ? 'sRGB' : 'linear') + (image.flags & 1 ? ', premultiplied' : ''),
      'GPU ' + (GPU_NAMES[image.gpuFormat] || '?'),
      kb(bytes.length) + ' (' + (100 * bytes.length / raw).toFixed(1) + '% of raw)',
      'decoded in ' + ms.toFixed(1) + ' ms',
    ].join('  ·  ');

    controls.textContent = '';
    if (image.mipLevels > 1) {
      const sel = document.createElement('select');
      for (let m = 0; m < image.mipLevels; m++) {
        const o = document.createElement('option');
        o.value = m;
        o.textContent = 'Mip ' + m + ' (' + Math.max(1, image.width >>> m) + '×' + Math.max(1, image.height >>> m) + ')';
        sel.appendChild(o);
      }
      sel.value = level;
      sel.onchange = () => { level = +sel.value; draw(); };
      controls.appendChild(sel);
    }
    draw();
  }

  document.getElementById('stage').addEventListener('click', () => { fit = !fit; zoom = 1; layout(); });
  document.getElementById('stage').addEventListener('wheel', (e) => {
    if (!e.ctrlKey || !image) return;
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
