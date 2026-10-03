# YAPF for Figma

- **Plugins → YAPF Image Format → Import YAPF image…** — choose or drop
  `.yapf` files; each becomes a rectangle filled with the image.
- **Plugins → YAPF Image Format → Export selection as YAPF…** — every
  selected layer is exported at 1×, 2× or 4× and downloaded as `.yapf`.

Both directions are pixel-exact: the plugin carries its own small PNG
encoder/decoder instead of going through a canvas, which would alter
semi-transparent pixels.

## Install (development plugin)

1. Use the **Figma desktop app**.
2. **Plugins → Development → Import plugin from manifest…** and choose
   `extensions/figma/manifest.json` (or the one inside `yapf-figma.zip`
   from the [Releases](https://github.com/Nexoniarz/YAPF/releases)).
3. Run it from **Plugins → Development → YAPF Image Format**.

To publish it to the Figma Community, use **Publish** in the same menu;
Figma then assigns the plugin its real `id`.

## Build

`ui.html` is generated: it inlines `bindings/js/yapf.js` and `src/png.js`
into `src/ui.src.html`, because Figma loads the UI as a single file.

```sh
node build.js
```

Figma limits images to 4096 × 4096; larger `.yapf` files report an error.
