# YAPF image support for Blender.
#
# Copyright 2026 Nexoniarz — Apache License 2.0.
#
#   File > Import > YAPF Image (.yapf)        open an image, optionally as a plane
#   Image Editor > Image > Open YAPF…         open into the Image Editor
#   Image Editor > Image > Save as YAPF…      export the current image
#   Drag a .yapf file into the 3D Viewport, Image Editor or a node editor.
#
# Blender cannot read .yapf files by itself, so imported images are packed
# into the .blend file; they survive saving and reopening.

bl_info = {
    "name": "YAPF Image Format",
    "author": "Nexoniarz",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "File > Import > YAPF Image, Image Editor > Image menu",
    "description": "Import and export YAPF (.yapf) lossless images",
    "doc_url": "https://github.com/Nexoniarz/YAPF",
    "category": "Import-Export",
}

import os

import bpy
import numpy as np
from bpy.props import BoolProperty, CollectionProperty, StringProperty
from bpy_extras.io_utils import ExportHelper, ImportHelper

from . import yapf


# ── conversion helpers ────────────────────────────────────────────────

def load_image(filepath):
    """Read a .yapf file into a packed bpy.types.Image."""
    img = yapf.load(filepath)
    w, h, ch = img.width, img.height, img.channels
    src = np.frombuffer(img.pixels, dtype=np.uint8).reshape(h, w, ch).astype(np.float32) / 255.0
    rgba = np.ones((h, w, 4), dtype=np.float32)
    if ch >= 3:
        rgba[..., :3] = src[..., :3]
    else:
        rgba[..., :3] = src[..., :1]
    if ch in (2, 4):
        rgba[..., 3] = src[..., ch - 1]

    name = os.path.basename(filepath)
    bimg = bpy.data.images.new(name, w, h, alpha=ch in (2, 4))
    bimg.colorspace_settings.name = "sRGB" if img.flags & yapf.FLAG_SRGB else "Non-Color"
    if img.flags & yapf.FLAG_PREMULT_ALPHA:
        bimg.alpha_mode = "PREMUL"
    bimg.pixels.foreach_set(rgba[::-1].ravel())          # Blender rows go bottom-up
    bimg.file_format = "PNG"
    bimg.pack()                                          # keep it inside the .blend
    bimg.use_fake_user = True                            # …even before anything uses it
    bimg["yapf_source"] = filepath
    bimg["yapf_alpha"] = ch in (2, 4)
    return bimg


def save_image(bimg, filepath, mips=False):
    """Write a bpy.types.Image to a .yapf file."""
    w, h = bimg.size
    if w == 0 or h == 0:
        raise yapf.YapfError("image '%s' has no pixels" % bimg.name)
    buf = np.empty(w * h * 4, dtype=np.float32)
    bimg.pixels.foreach_get(buf)
    px = (np.clip(buf.reshape(h, w, 4)[::-1], 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)
    has_alpha = bool((px[..., 3] != 255).any())
    ch = 4 if has_alpha else 3
    px = np.ascontiguousarray(px[..., :ch])

    srgb = bimg.colorspace_settings.name not in ("Non-Color", "Linear", "Linear Rec.709", "Raw")
    flags = (yapf.FLAG_SRGB if srgb else 0) | (yapf.FLAG_PREMULT_ALPHA if bimg.alpha_mode == "PREMUL" else 0)
    levels = [px]
    if mips:
        cur = px.astype(np.uint32)
        while cur.shape[0] > 1 or cur.shape[1] > 1:
            hh, ww = max(1, cur.shape[0] // 2), max(1, cur.shape[1] // 2)
            p = np.pad(cur, ((0, cur.shape[0] % 2), (0, cur.shape[1] % 2), (0, 0)), mode="edge")
            cur = (p[0::2, 0::2] + p[1::2, 0::2] + p[0::2, 1::2] + p[1::2, 1::2] + 2) // 4
            cur = cur[:hh, :ww]
            levels.append(cur.astype(np.uint8))
            if len(levels) == 16:
                break
    out = yapf.Image(w, h, ch, levels[0].tobytes(), flags=flags,
                     mips=[l.tobytes() for l in levels])
    if srgb:
        out.gpu_format = yapf.GPU_SRGB8_A8 if ch == 4 else yapf.GPU_SRGB8
    else:
        out.gpu_format = yapf.GPU_RGBA8 if ch == 4 else yapf.GPU_RGB8
    yapf.save(filepath, out)


def add_image_plane(context, bimg):
    """A plane with the image's aspect ratio and a material that shows it."""
    w, h = bimg.size
    bpy.ops.mesh.primitive_plane_add(size=1.0, location=context.scene.cursor.location)
    obj = context.active_object
    obj.name = os.path.splitext(bimg.name)[0]
    obj.scale = (w / max(w, h), h / max(w, h), 1.0)

    mat = bpy.data.materials.new(obj.name)
    mat.use_nodes = True
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    bsdf = next(n for n in nodes if n.type == "BSDF_PRINCIPLED")
    tex = nodes.new("ShaderNodeTexImage")
    tex.image = bimg
    tex.location = (bsdf.location.x - 350, bsdf.location.y)
    links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
    if bimg.get("yapf_alpha"):
        links.new(tex.outputs["Alpha"], bsdf.inputs["Alpha"])
    obj.data.materials.append(mat)
    return obj


def show_in_image_editor(context, bimg):
    for area in context.screen.areas if context.screen else []:
        if area.type == "IMAGE_EDITOR":
            area.spaces.active.image = bimg
            return True
    return False


# ── operators ─────────────────────────────────────────────────────────

class IMPORT_IMAGE_OT_yapf(bpy.types.Operator, ImportHelper):
    """Import YAPF (.yapf) images"""
    bl_idname = "import_image.yapf"
    bl_label = "Import YAPF Image"
    bl_options = {"REGISTER", "UNDO"}

    filename_ext = ".yapf"
    filter_glob: StringProperty(default="*.yapf", options={"HIDDEN"})
    files: CollectionProperty(type=bpy.types.OperatorFileListElement,
                              options={"HIDDEN", "SKIP_SAVE"})
    directory: StringProperty(subtype="DIR_PATH", options={"HIDDEN", "SKIP_SAVE"})
    as_plane: BoolProperty(name="Create Plane",
                           description="Add a plane showing the image to the scene",
                           default=False)

    def paths(self):
        if self.files and self.directory:
            return [os.path.join(self.directory, f.name) for f in self.files if f.name]
        return [self.filepath]

    def execute(self, context):
        done = 0
        for path in self.paths():
            try:
                bimg = load_image(path)
            except (OSError, yapf.YapfError) as e:
                self.report({"ERROR"}, "%s: %s" % (os.path.basename(path), e))
                continue
            done += 1
            if self.as_plane and context.mode == "OBJECT":
                add_image_plane(context, bimg)
            else:
                show_in_image_editor(context, bimg)
        if done:
            self.report({"INFO"}, "Imported %d YAPF image(s)" % done)
        return {"FINISHED"} if done else {"CANCELLED"}

    def invoke(self, context, event):
        # Dropped files arrive with filepath/directory already set.
        if self.directory and self.files:
            if context.area and context.area.type == "VIEW_3D":
                self.as_plane = True
            return self.execute(context)
        return ImportHelper.invoke(self, context, event)


class EXPORT_IMAGE_OT_yapf(bpy.types.Operator, ExportHelper):
    """Save the current image as a YAPF (.yapf) file"""
    bl_idname = "export_image.yapf"
    bl_label = "Save as YAPF"

    filename_ext = ".yapf"
    filter_glob: StringProperty(default="*.yapf", options={"HIDDEN"})
    image_name: StringProperty(name="Image", description="Image to save (default: the one in the Image Editor)")
    mips: BoolProperty(name="Store Mipmaps", description="Also store a full mip chain", default=False)

    def target(self, context):
        if self.image_name:
            return bpy.data.images.get(self.image_name)
        space = context.space_data
        return getattr(space, "image", None)

    def invoke(self, context, event):
        img = self.target(context)
        if img is None:
            self.report({"ERROR"}, "Open an image in the Image Editor first")
            return {"CANCELLED"}
        self.image_name = img.name
        self.filepath = os.path.splitext(img.name)[0] + ".yapf"
        return ExportHelper.invoke(self, context, event)

    def execute(self, context):
        img = self.target(context)
        if img is None:
            self.report({"ERROR"}, "No image to save")
            return {"CANCELLED"}
        try:
            save_image(img, self.filepath, self.mips)
        except (OSError, yapf.YapfError) as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        self.report({"INFO"}, "Saved %s" % os.path.basename(self.filepath))
        return {"FINISHED"}


class IMAGE_FH_yapf(bpy.types.FileHandler):
    """Drag and drop .yapf files into Blender"""
    bl_idname = "IMAGE_FH_yapf"
    bl_label = "YAPF Image"
    bl_import_operator = "import_image.yapf"
    bl_file_extensions = ".yapf"

    @classmethod
    def poll_drop(cls, context):
        return context.area is not None and context.area.type in {
            "VIEW_3D", "IMAGE_EDITOR", "NODE_EDITOR"}


def menu_import(self, context):
    self.layout.operator(IMPORT_IMAGE_OT_yapf.bl_idname, text="YAPF Image (.yapf)")


def menu_image(self, context):
    self.layout.separator()
    self.layout.operator(IMPORT_IMAGE_OT_yapf.bl_idname, text="Open YAPF…")
    self.layout.operator(EXPORT_IMAGE_OT_yapf.bl_idname, text="Save as YAPF…")


classes = (IMPORT_IMAGE_OT_yapf, EXPORT_IMAGE_OT_yapf, IMAGE_FH_yapf)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.TOPBAR_MT_file_import.append(menu_import)
    bpy.types.IMAGE_MT_image.append(menu_image)


def unregister():
    bpy.types.IMAGE_MT_image.remove(menu_image)
    bpy.types.TOPBAR_MT_file_import.remove(menu_import)
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
