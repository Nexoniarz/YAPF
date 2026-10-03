// YAPF for Figma — plugin main thread (has the document, no DOM).
// Copyright 2026 Nexoniarz — Apache License 2.0.
// Decoding/encoding happens in the UI (ui.html), which has a DOM.

figma.showUI(__html__, { width: 380, height: 330, themeColors: true });
figma.ui.postMessage({ type: 'mode', mode: figma.command || 'import' });

function sendSelection() {
  const nodes = figma.currentPage.selection.filter((n) => 'exportAsync' in n);
  figma.ui.postMessage({ type: 'selection', names: nodes.map((n) => n.name) });
}
figma.on('selectionchange', sendSelection);
sendSelection();

figma.ui.onmessage = async (msg) => {
  if (msg.type === 'import') {
    // msg.images: [{ name, width, height, png: Uint8Array }]
    const created = [];
    const center = figma.viewport.center;
    let x = center.x;
    for (const im of msg.images) {
      try {
        const image = figma.createImage(im.png);
        const rect = figma.createRectangle();
        rect.name = im.name;
        rect.resize(im.width, im.height);
        rect.fills = [{ type: 'IMAGE', imageHash: image.hash, scaleMode: 'FILL' }];
        rect.x = Math.round(x - (created.length ? 0 : im.width / 2));
        rect.y = Math.round(center.y - im.height / 2);
        x = rect.x + im.width + 40;
        figma.currentPage.appendChild(rect);
        created.push(rect);
      } catch (e) {
        figma.notify(im.name + ': ' + (e.message || e) + ' (Figma accepts images up to 4096×4096)', { error: true });
      }
    }
    if (created.length) {
      figma.currentPage.selection = created;
      figma.viewport.scrollAndZoomIntoView(created);
      figma.notify('Imported ' + created.length + ' YAPF image' + (created.length > 1 ? 's' : ''));
    }
  }

  if (msg.type === 'export') {
    const nodes = figma.currentPage.selection.filter((n) => 'exportAsync' in n);
    if (!nodes.length) { figma.notify('Select one or more layers to export', { error: true }); return; }
    for (const node of nodes) {
      const png = await node.exportAsync({ format: 'PNG', constraint: { type: 'SCALE', value: msg.scale || 1 } });
      figma.ui.postMessage({ type: 'exported', name: node.name, png });
    }
  }

  if (msg.type === 'close') figma.closePlugin();
};
