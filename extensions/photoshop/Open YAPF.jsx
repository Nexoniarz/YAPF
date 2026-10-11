// Open YAPF — opens .yapf images in Photoshop.
// Copyright 2026 Nexoniarz — Apache License 2.0.
// Install: copy this folder's .jsx / .jsxinc files (and yapf / yapf.exe)
// into Photoshop's Presets/Scripts folder; then File > Scripts > Open YAPF.

#target photoshop
#include "yapf_common.jsxinc"

(function () {
    var filter = YAPF_IS_WINDOWS ? "YAPF images:*.yapf,All files:*.*"
                                 : function (f) { return f instanceof Folder || /\.yapf$/i.test(f.name); };
    var files = File.openDialog("Open YAPF image", filter, true);
    if (!files) return;
    if (!(files instanceof Array)) files = [files];

    for (var i = 0; i < files.length; i++) {
        // 8 / 16-bit images come through PNG (16 bits are kept); half,
        // float and BC6H HDR textures through Radiance .hdr (32-bit).
        var tmp = yapfTempFile(yapfIsHdr(files[i]) ? ".hdr" : ".png");
        if (!yapfRun([files[i].fsName, tmp.fsName], tmp)) {
            alert("Could not open " + files[i].name + ".\n\n" + yapfFailText());
            return;
        }
        var doc = app.open(tmp);
        // The temporary PNG is no longer needed once the document is open;
        // use File > Scripts > Save as YAPF to write a .yapf again.
        doc.info.caption = files[i].fsName;
        tmp.remove();
    }
})();
