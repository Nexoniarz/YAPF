# YAPF for Photoshop

Two scripts that appear under **File → Scripts**:

- **Open YAPF** — opens one or more `.yapf` files as documents.
- **Save as YAPF** — saves the visible image of the active document (a
  flattened 8-bit copy; your document is not changed), keeping
  transparency, with an optional mip chain.

They convert through the `yapf` command-line tool, so Photoshop needs no
native plugin.

## Install

1. Download `yapf-<your platform>.zip` from the
   [Releases](https://github.com/Nexoniarz/YAPF/releases) (or build the tool:
   `make` → `build/yapf`).
2. Copy `Open YAPF.jsx`, `Save as YAPF.jsx`, `yapf_common.jsxinc` **and**
   `yapf` / `yapf.exe` into Photoshop's scripts folder:
   - Windows: `C:\Program Files\Adobe\Adobe Photoshop <version>\Presets\Scripts`
   - macOS: `/Applications/Adobe Photoshop <version>/Presets/Scripts`
3. Restart Photoshop.  The scripts are under **File → Scripts**; you can give
   them keyboard shortcuts in **Edit → Keyboard Shortcuts** or record them
   in Actions.

Instead of copying `yapf` next to the scripts you can put it on your `PATH`.

## Status

The scripts are written for Photoshop CC's ExtendScript and are not yet
tested inside Photoshop (the converter itself is fully tested).  A native
format plugin (`.8bi` / `.plugin`, so `.yapf` shows up directly in the Open
and Save dialogs) needs Adobe's Photoshop SDK and is planned.
