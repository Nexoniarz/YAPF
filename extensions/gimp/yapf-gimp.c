/*
 * yapf-gimp.c  —  YAPF format plugin for GIMP.
 *
 * This plugin is an independent work provided under the Apache License 2.0.
 * It interfaces with GIMP, which is licensed under GPLv3.  Users may use,
 * modify, and distribute this plugin independently of GIMP's licensing terms.
 *
 * Copyright 2026 Nexoniarz
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * -------------------------------------------------------------------------
 *
 * Compilation (from this folder):
 *   gimptool --install yapf-gimp.c
 *
 * yapf.c (two folders up) is embedded via #include below because gimptool
 * compiles a single source file.  Do NOT link it separately.
 */

#include <libgimp/gimp.h>
#include <libgimp/gimpui.h>

/* Pull in the entire encoder/decoder as a single translation unit. */
#include "../../yapf.c"

/* ── GIMP procedure names ────────────────────────────────────────────── */

#define LOAD_PROC "file-yapf-load"
#define SAVE_PROC "file-yapf-save"
#define PLUG_IN_BINARY "yapf-gimp"

/* ── Plugin type boilerplate ─────────────────────────────────────────── */

struct _YapfPlugin { GimpPlugIn parent_instance; };

#define YAPF_PLUGIN_TYPE (yapf_plugin_get_type())
G_DECLARE_FINAL_TYPE(YapfPlugin, yapf_plugin, YAPF, PLUGIN, GimpPlugIn)

static GList          *yapf_plugin_query_procedures (GimpPlugIn *plug_in);
static GimpProcedure  *yapf_plugin_create_procedure (GimpPlugIn *plug_in,
                                                     const gchar *name);

static GimpValueArray *yapf_load_run (GimpProcedure        *procedure,
                                      GimpRunMode           run_mode,
                                      GFile                *file,
                                      GimpMetadata         *metadata,
                                      GimpMetadataLoadFlags *flags,
                                      GimpProcedureConfig  *config,
                                      gpointer              run_data);
static GimpValueArray *yapf_save_run (GimpProcedure        *procedure,
                                      GimpRunMode           run_mode,
                                      GimpImage            *image,
                                      GFile                *file,
                                      GimpExportOptions    *options,
                                      GimpMetadata         *metadata,
                                      GimpProcedureConfig  *config,
                                      gpointer              run_data);

G_DEFINE_TYPE(YapfPlugin, yapf_plugin, GIMP_TYPE_PLUG_IN)

static void yapf_plugin_class_init(YapfPluginClass *klass) {
    GimpPlugInClass *plug_in_class = GIMP_PLUG_IN_CLASS(klass);
    plug_in_class->query_procedures  = yapf_plugin_query_procedures;
    plug_in_class->create_procedure  = yapf_plugin_create_procedure;
}

static void yapf_plugin_init(YapfPlugin *plugin) { (void)plugin; }

/* ── Procedure list ──────────────────────────────────────────────────── */

static GList *yapf_plugin_query_procedures(GimpPlugIn *plug_in) {
    (void)plug_in;
    GList *list = NULL;
    list = g_list_append(list, g_strdup(LOAD_PROC));
    list = g_list_append(list, g_strdup(SAVE_PROC));
    return list;
}

/* ── Procedure creation ──────────────────────────────────────────────── */

static GimpProcedure *yapf_plugin_create_procedure(GimpPlugIn  *plug_in,
                                                   const gchar *name) {
    GimpProcedure *procedure = NULL;

    if (g_strcmp0(name, LOAD_PROC) == 0) {
        procedure = gimp_load_procedure_new(plug_in, name,
                        GIMP_PDB_PROC_TYPE_PLUGIN,
                        yapf_load_run, NULL, NULL);
        gimp_procedure_set_menu_label(procedure, "YAPF image");
        gimp_procedure_set_documentation(procedure,
            "Loads a YAPF (Yet Another Picture Format) image",
            "Loads a YAPF image from disk", NULL);
        gimp_procedure_set_attribution(procedure,
            "Nexoniarz", "Nexoniarz", "2026");
        gimp_file_procedure_set_mime_types(GIMP_FILE_PROCEDURE(procedure),
            "image/x-yapf");
        gimp_file_procedure_set_extensions(GIMP_FILE_PROCEDURE(procedure),
            "yapf");
        gimp_file_procedure_set_magics(GIMP_FILE_PROCEDURE(procedure),
            "0,string,YAPF");

    } else if (g_strcmp0(name, SAVE_PROC) == 0) {
        procedure = gimp_export_procedure_new(plug_in, name,
                        GIMP_PDB_PROC_TYPE_PLUGIN, TRUE,
                        (GimpRunExportFunc)yapf_save_run, NULL, NULL);
        gimp_procedure_set_image_types(procedure, "RGB, RGBA, GRAY, GRAYA");
        gimp_procedure_set_menu_label(procedure, "YAPF image");
        gimp_procedure_set_documentation(procedure,
            "Exports a YAPF (Yet Another Picture Format) image",
            "Exports a YAPF image to disk", NULL);
        gimp_procedure_set_attribution(procedure,
            "Nexoniarz", "Nexoniarz", "2026");
        gimp_file_procedure_set_mime_types(GIMP_FILE_PROCEDURE(procedure),
            "image/x-yapf");
        gimp_file_procedure_set_extensions(GIMP_FILE_PROCEDURE(procedure),
            "yapf");
        /* GIMP flattens / converts a copy of the image for us as needed. */
        gimp_export_procedure_set_capabilities(GIMP_EXPORT_PROCEDURE(procedure),
            GIMP_EXPORT_CAN_HANDLE_RGB | GIMP_EXPORT_CAN_HANDLE_GRAY |
            GIMP_EXPORT_CAN_HANDLE_ALPHA, NULL, NULL, NULL);
        gimp_procedure_add_boolean_argument(procedure, "mipmaps",
            "Store _mipmaps",
            "Also store a full mip chain (for game engines and GPUs)",
            FALSE, G_PARAM_READWRITE);
    }

    return procedure;
}

static GimpValueArray *yapf_error(GimpProcedure *procedure, const gchar *msg);

/* ── Load handler ────────────────────────────────────────────────────── */

/* Why a file could not be opened, in words. */
static gchar *yapf_load_reason(const gchar *path, int err) {
    FILE   *f = fopen(path, "rb");
    uint8_t h[8] = {0};
    size_t  n = f ? fread(h, 1, sizeof(h), f) : 0;
    if (f) fclose(f);
    if (!f || err == YAPF_ERR_IO)
        return g_strdup("The file cannot be read.");
    if (n < 5 || memcmp(h, "YAPF", 4) != 0)
        return g_strdup("This is not a YAPF file (it does not start with \"YAPF\").");
    if (h[4] != 1 && h[4] != 2)
        return g_strdup_printf("This YAPF file is format version %u; this plug-in reads "
                               "versions 1 and 2.  Update the plug-in.", h[4]);
    switch (err) {
    case YAPF_ERR_UNSUPPORTED:
        return g_strdup("This YAPF file needs a feature this plug-in does not have yet. "
                        "Update the plug-in.");
    case YAPF_ERR_CHECKSUM:
        return g_strdup("This YAPF file is damaged: a checksum does not match.");
    case YAPF_ERR_TRUNCATED:
        return g_strdup("This YAPF file is incomplete (truncated).");
    case YAPF_ERR_OOM:
        return g_strdup("Out of memory.");
    default:
        return g_strdup_printf("This YAPF file (version %u) is corrupt, or was written by a "
                               "tool that does not follow the YAPF specification.", h[4]);
    }
}

static const gchar *yapf_face_name(uint32_t f) {
    static const gchar *n[6] = { "+X", "-X", "+Y", "-Y", "+Z", "-Z" };
    return f < 6 ? n[f] : "?";
}

static GimpValueArray *yapf_load_run(
        GimpProcedure         *procedure,
        GimpRunMode            run_mode,
        GFile                 *file,
        GimpMetadata          *metadata,
        GimpMetadataLoadFlags *flags,
        GimpProcedureConfig   *config,
        gpointer               run_data)
{
    (void)run_mode; (void)metadata; (void)flags; (void)config; (void)run_data;

    gchar          *path = g_file_get_path(file);
    int             err  = 0;
    yapf_texture_t *t    = yapf_tex_load(path, 0, 0, &err);   /* v1 and v2, all cores */
    if (!t) {
        gchar *why = yapf_load_reason(path, err);
        g_free(path);
        GError *e = g_error_new(GIMP_PLUG_IN_ERROR, 0, "Failed to load YAPF file: %s", why);
        g_free(why);
        return gimp_procedure_new_return_values(procedure, GIMP_PDB_EXECUTION_ERROR, e);
    }
    g_free(path);

    /* What the pixels look like after decoding: GPU blocks decode to
     * RGBA 8-bit, BC6H to RGB half float. */
    gboolean blocks = t->payload == YAPF_PAYLOAD_BLOCKS;
    gboolean bc6    = blocks && (t->format == YAPF_GPU_BC6H || t->format == YAPF_GPU_BC6H_SF);
    if (blocks && t->format >= YAPF_GPU_ETC2_RGB8) {
        yapf_tex_free(t);
        return yapf_error(procedure, "Failed to load YAPF file: it holds ETC2 / EAC / ASTC "
                                     "GPU blocks, which this plug-in cannot show yet.");
    }
    int ch     = blocks ? (bc6 ? 3 : 4) : t->channels;
    int sample = blocks ? (bc6 ? YAPF_SAMPLE_F16 : YAPF_SAMPLE_U8) : t->sample;
    /* sRGB data is gamma encoded; floats are always linear */
    gboolean gamma = (t->flags & YAPF_FLAG_SRGB) &&
                     (sample == YAPF_SAMPLE_U8 || sample == YAPF_SAMPLE_U16);

    static const gchar *chn[2][4] = { { "Y", "YA", "RGB", "RGBA" }, { "Y'", "Y'A", "R'G'B'", "R'G'B'A" } };
    static const gchar *typ[4]    = { "u8", "u16", "half", "float" };
    gchar *fmt_name = g_strdup_printf("%s %s", chn[gamma ? 1 : 0][ch - 1], typ[sample]);
    const Babl *fmt = babl_format(fmt_name);
    g_free(fmt_name);

    GimpPrecision prec;
    switch (sample) {
    case YAPF_SAMPLE_U16: prec = gamma ? GIMP_PRECISION_U16_NON_LINEAR : GIMP_PRECISION_U16_LINEAR; break;
    case YAPF_SAMPLE_F16: prec = GIMP_PRECISION_HALF_LINEAR; break;
    case YAPF_SAMPLE_F32: prec = GIMP_PRECISION_FLOAT_LINEAR; break;
    default:              prec = gamma ? GIMP_PRECISION_U8_NON_LINEAR : GIMP_PRECISION_U8_LINEAR; break;
    }
    GimpImageBaseType base  = ch <= 2 ? GIMP_GRAY : GIMP_RGB;
    GimpImageType     ltype = ch == 1 ? GIMP_GRAY_IMAGE : ch == 2 ? GIMP_GRAYA_IMAGE
                            : ch == 3 ? GIMP_RGB_IMAGE : GIMP_RGBA_IMAGE;

    /* Mip 0 of every frame, layer, face and slice becomes one GIMP layer
     * (the first at the bottom, as GIMP animations expect). */
    uint32_t   w, h, d, faces = yapf_tex_faces(t);
    yapf_tex_mip_size(t, 0, &w, &h, &d);
    GimpImage *image = gimp_image_new_with_precision(w, h, base, prec);
    size_t     texel = (size_t)ch * yapf_sample_bytes((uint8_t)sample);
    void      *tmp   = blocks ? g_malloc((size_t)w * h * texel) : NULL;
    gint       count = (gint)(t->frames * t->layers * faces * d);

    for (uint32_t fr = 0; fr < t->frames; fr++)
        for (uint32_t ly = 0; ly < t->layers; ly++)
            for (uint32_t fc = 0; fc < faces; fc++)
                for (uint32_t z = 0; z < d; z++) {
                    uint32_t s = yapf_tex_surface(t, fr, ly, fc, 0);
                    size_t   slice = yapf_tex_surface_bytes(t, 0) / d;
                    const uint8_t *src = (const uint8_t *)t->data[s] + slice * z;
                    if (blocks) {
                        yapf_blocks_decode(t->format, src, w, h, tmp);
                        src = (const uint8_t *)tmp;
                    }
                    GString *name = g_string_new(NULL);
                    if (t->frames > 1) {
                        g_string_append_printf(name, "Frame %u", fr + 1);
                        if (t->frame_us)
                            g_string_append_printf(name, " (%ums)", (t->frame_us[fr] + 500) / 1000);
                    }
                    if (t->layers > 1) g_string_append_printf(name, "%sLayer %u", name->len ? ", " : "", ly);
                    if (faces > 1)     g_string_append_printf(name, "%sFace %s", name->len ? ", " : "", yapf_face_name(fc));
                    if (d > 1)         g_string_append_printf(name, "%sSlice %u", name->len ? ", " : "", z);
                    if (!name->len)    g_string_append(name, "Background");

                    GimpLayer *layer = gimp_layer_new(image, name->str, (gint)w, (gint)h, ltype,
                                                      100.0, gimp_image_get_default_new_layer_mode(image));
                    g_string_free(name, TRUE);
                    gimp_image_insert_layer(image, layer, NULL, 0);
                    if (count > 1 && !(fr == 0 && ly == 0 && fc == 0 && z == 0))
                        gimp_item_set_visible(GIMP_ITEM(layer), FALSE);
                    GeglBuffer *buffer = gimp_drawable_get_buffer(GIMP_DRAWABLE(layer));
                    gegl_buffer_set(buffer, GEGL_RECTANGLE(0, 0, (gint)w, (gint)h), 0, fmt,
                                    src, GEGL_AUTO_ROWSTRIDE);
                    g_object_unref(buffer);
                }
    g_free(tmp);
    yapf_tex_free(t);

    GimpValueArray *ret = gimp_procedure_new_return_values(procedure,
                              GIMP_PDB_SUCCESS, NULL);
    g_value_set_object(gimp_value_array_index(ret, 1), image);
    return ret;
}

/* ── Save handler ────────────────────────────────────────────────────── */

/* Full mip chain with a 2×2 box filter (edges clamp for odd sizes).
 * Fills img->mips / img->mip_levels; mips[0] aliases img->pixels. */
static gboolean yapf_build_mips(yapf_image_t *img) {
    int levels = 1;
    while (levels < (int)YAPF_MAX_MIPS &&
           ((img->width >> levels) > 0 || (img->height >> levels) > 0))
        levels++;
    img->mips = (uint8_t **)calloc((size_t)levels, sizeof(uint8_t *));
    if (!img->mips) return FALSE;
    img->mips[0]    = img->pixels;
    img->mip_levels = (uint8_t)levels;

    int ch = img->channels;
    for (int m = 1; m < levels; m++) {
        uint32_t pw = MAX(1u, img->width >> (m - 1)), ph = MAX(1u, img->height >> (m - 1));
        uint32_t w  = MAX(1u, img->width >> m),       h  = MAX(1u, img->height >> m);
        const uint8_t *src = img->mips[m - 1];
        uint8_t *dst = (uint8_t *)malloc((size_t)w * h * ch);
        if (!dst) return FALSE;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                for (int c = 0; c < ch; c++) {
                    uint32_t x0 = MIN(2 * x, pw - 1), x1 = MIN(2 * x + 1, pw - 1);
                    uint32_t y0 = MIN(2 * y, ph - 1), y1 = MIN(2 * y + 1, ph - 1);
                    unsigned sum = src[((size_t)y0 * pw + x0) * ch + c] + src[((size_t)y0 * pw + x1) * ch + c]
                                 + src[((size_t)y1 * pw + x0) * ch + c] + src[((size_t)y1 * pw + x1) * ch + c];
                    dst[((size_t)y * w + x) * ch + c] = (uint8_t)((sum + 2) / 4);
                }
        img->mips[m] = dst;
    }
    return TRUE;
}

static GimpValueArray *yapf_error(GimpProcedure *procedure, const gchar *msg) {
    return gimp_procedure_new_return_values(procedure, GIMP_PDB_EXECUTION_ERROR,
               g_error_new_literal(GIMP_PLUG_IN_ERROR, 0, msg));
}

static GimpValueArray *yapf_save_run(
        GimpProcedure       *procedure,
        GimpRunMode          run_mode,
        GimpImage           *image,
        GFile               *file,
        GimpExportOptions   *options,
        GimpMetadata        *metadata,
        GimpProcedureConfig *config,
        gpointer             run_data)
{
    (void)metadata; (void)run_data;

    if (run_mode == GIMP_RUN_INTERACTIVE) {
        gimp_ui_init(PLUG_IN_BINARY);
        GtkWidget *dialog = gimp_export_procedure_dialog_new(
            GIMP_EXPORT_PROCEDURE(procedure), config, image);
        gimp_procedure_dialog_fill(GIMP_PROCEDURE_DIALOG(dialog), NULL);
        gboolean ok = gimp_procedure_dialog_run(GIMP_PROCEDURE_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (!ok)
            return gimp_procedure_new_return_values(procedure, GIMP_PDB_CANCEL, NULL);
    }

    gboolean mipmaps = FALSE;
    g_object_get(config, "mipmaps", &mipmaps, NULL);

    /* A flattened copy when the image has several layers; the visible
     * result is what gets exported. */
    GimpExportReturn export = gimp_export_options_get_image(options, &image);
    GList *layers = gimp_image_list_layers(image);
    if (!layers) {
        if (export == GIMP_EXPORT_EXPORT) gimp_image_delete(image);
        return yapf_error(procedure, "No layers to export.");
    }
    GimpDrawable *drawable = GIMP_DRAWABLE(layers->data);
    g_list_free(layers);

    gboolean     has_alpha = gimp_drawable_has_alpha(drawable);
    gboolean     is_gray   = gimp_drawable_is_gray(drawable);
    const gchar *babl_fmt;
    uint8_t      ch, gpu_fmt;

    /* GIMP's 8-bit buffers are gamma encoded, so pixels are written as sRGB. */
    if (is_gray) {
        ch       = has_alpha ? YAPF_CHANNELS_GRAY_ALPHA : YAPF_CHANNELS_GRAY;
        babl_fmt = has_alpha ? "Y'A u8" : "Y' u8";
        gpu_fmt  = has_alpha ? YAPF_GPU_RG8 : YAPF_GPU_R8;
    } else {
        ch       = has_alpha ? YAPF_CHANNELS_RGBA : YAPF_CHANNELS_RGB;
        babl_fmt = has_alpha ? "R'G'B'A u8" : "R'G'B' u8";
        gpu_fmt  = has_alpha ? YAPF_GPU_SRGB8_A8 : YAPF_GPU_SRGB8;
    }

    gint w = gimp_drawable_get_width(drawable);
    gint h = gimp_drawable_get_height(drawable);
    uint8_t *pixels = (uint8_t *)malloc((size_t)w * (size_t)h * ch);
    if (!pixels) {
        if (export == GIMP_EXPORT_EXPORT) gimp_image_delete(image);
        return yapf_error(procedure, "Out of memory.");
    }

    GeglBuffer *buffer = gimp_drawable_get_buffer(drawable);
    gegl_buffer_get(buffer, GEGL_RECTANGLE(0, 0, w, h), 1.0,
                    babl_format(babl_fmt), pixels,
                    GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);
    g_object_unref(buffer);
    if (export == GIMP_EXPORT_EXPORT) gimp_image_delete(image);

    yapf_image_t img;
    memset(&img, 0, sizeof(img));
    img.width      = (uint32_t)w;
    img.height     = (uint32_t)h;
    img.channels   = ch;
    img.gpu_format = gpu_fmt;
    img.flags      = YAPF_FLAG_SRGB;
    img.mip_levels = 1;
    img.pixels     = pixels;

    int result = YAPF_ERR_OOM;
    if (!mipmaps || yapf_build_mips(&img)) {
        gchar *path = g_file_get_path(file);
        result = yapf_save(path, &img);
        g_free(path);
    }
    if (img.mips) {
        for (int m = 1; m < img.mip_levels; m++) free(img.mips[m]);
        free(img.mips);
    }
    free(pixels);

    if (result != YAPF_OK)
        return yapf_error(procedure, "Failed to export YAPF file.");
    return gimp_procedure_new_return_values(procedure, GIMP_PDB_SUCCESS, NULL);
}

GIMP_MAIN(YAPF_PLUGIN_TYPE)
