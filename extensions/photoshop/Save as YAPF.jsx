// Save as YAPF — writes the active document as a .yapf image.
// Copyright 2026 Nexoniarz — Apache License 2.0.
// The visible result is saved: a flattened 8-bit RGB (or grayscale) copy,
// transparency kept.  Your document is not modified.

#target photoshop
#include "yapf_common.jsxinc"

(function () {
    if (!app.documents.length) { alert("Open a document first."); return; }
    var doc = app.activeDocument;

    var base = doc.name.replace(/\.[^.]+$/, "");
    var start = new File((doc.saved && doc.path ? doc.path.fsName : Folder.desktop.fsName) + "/" + base + ".yapf");
    var out = start.saveDlg("Save as YAPF", YAPF_IS_WINDOWS ? "YAPF images:*.yapf" : undefined);
    if (!out) return;
    if (!/\.yapf$/i.test(out.name)) out = new File(out.fsName + ".yapf");
    var mips = confirm("Also store mipmaps (for game engines / GPUs)?", true, "YAPF");

    var tmp = yapfTempFile(".png");
    var copy = doc.duplicate(base + " (YAPF export)", true);     // merged copy
    try {
        if (copy.mode !== DocumentMode.RGB && copy.mode !== DocumentMode.GRAYSCALE)
            copy.changeMode(ChangeMode.RGB);
        if (copy.bitsPerChannel !== BitsPerChannelType.EIGHT)
            copy.bitsPerChannel = BitsPerChannelType.EIGHT;
        var opts = new PNGSaveOptions();
        opts.interlaced = false;
        copy.saveAs(tmp, opts, true, Extension.LOWERCASE);
    } finally {
        copy.close(SaveOptions.DONOTSAVECHANGES);
    }

    var args = [tmp.fsName, out.fsName];
    if (mips) args.push("--mips");
    var ok = yapfRun(args, out);
    tmp.remove();
    if (!ok) { alert("Could not write " + out.name + ".\n\n" + YAPF_TOOL_HELP); return; }
    alert("Saved " + out.fsName + " (" + Math.round(out.length / 1024) + " KB)");
})();
