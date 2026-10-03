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

/* ── Load handler ────────────────────────────────────────────────────── */

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

    gchar *path = g_file_get_path(file);
    yapf_image_t *img = yapf_load_mt(path, 0);  /* all cores */
    g_free(path);

    if (!img) {
        GError *err = g_error_new(GIMP_PLUG_IN_ERROR, 0,
                                  "Failed to load YAPF file.");
        return gimp_procedure_new_return_values(procedure,
                   GIMP_PDB_EXECUTION_ERROR, err);
    }

    /* Map YAPF channel count to GIMP image / layer types.
     * Use gamma-corrected babl formats (R'G'B') when the file carries the
     * sRGB flag; fall back to linear (RGB) otherwise. */
    gboolean          is_srgb    = (img->flags & YAPF_FLAG_SRGB) != 0;
    GimpImageBaseType base_type  = GIMP_RGB;
    GimpImageType     layer_type = GIMP_RGBA_IMAGE;
    const gchar      *babl_fmt   = is_srgb ? "R'G'B'A u8" : "RGBA u8";

    switch (img->channels) {
        case YAPF_CHANNELS_GRAY:
            base_type  = GIMP_GRAY;
            layer_type = GIMP_GRAY_IMAGE;
            babl_fmt   = is_srgb ? "Y' u8" : "Y u8";
            break;
        case YAPF_CHANNELS_GRAY_ALPHA:
            base_type  = GIMP_GRAY;
            layer_type = GIMP_GRAYA_IMAGE;
            babl_fmt   = is_srgb ? "Y'A u8" : "YA u8";
            break;
        case YAPF_CHANNELS_RGB:
            base_type  = GIMP_RGB;
            layer_type = GIMP_RGB_IMAGE;
            babl_fmt   = is_srgb ? "R'G'B' u8" : "RGB u8";
            break;
        default: /* RGBA */
            break;
    }

    GimpImage *image = gimp_image_new(img->width, img->height, base_type);
    GimpLayer *layer = gimp_layer_new(image, "Background",
                           img->width, img->height, layer_type,
                           100.0,
                           gimp_image_get_default_new_layer_mode(image));
    gimp_image_insert_layer(image, layer, NULL, 0);

    GeglBuffer *buffer = gimp_drawable_get_buffer(GIMP_DRAWABLE(layer));
    gegl_buffer_set(buffer,
        GEGL_RECTANGLE(0, 0, (gint)img->width, (gint)img->height),
        0, babl_format(babl_fmt),
        img->pixels, GEGL_AUTO_ROWSTRIDE);
    g_object_unref(buffer);

    yapf_free(img);

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
