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
 * Compilation:
 *   gimptool --install yapf-gimp.c
 *
 * yapf.c is embedded via #include below because gimptool compiles a single
 * source file.  Do NOT link separately — the include IS the build.
 */

#include <libgimp/gimp.h>

/* Pull in the entire encoder/decoder as a single translation unit. */
#include "yapf.c"

/* ── GIMP procedure names ────────────────────────────────────────────── */

#define LOAD_PROC "file-yapf-load"
#define SAVE_PROC "file-yapf-save"

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
    yapf_image_t *img = yapf_load(path);
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
            babl_fmt   = "Y' u8";
            break;
        case YAPF_CHANNELS_GRAY_ALPHA:
            base_type  = GIMP_GRAY;
            layer_type = GIMP_GRAYA_IMAGE;
            babl_fmt   = "Y'A u8";
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
    (void)run_mode; (void)options; (void)metadata; (void)config; (void)run_data;

    /* Retrieve the top-most layer. */
    GimpLayer  **layers   = gimp_image_get_layers(image);
    gint         n_layers = 0;
    if (layers) { while (layers[n_layers]) n_layers++; }

    if (!layers || n_layers == 0) {
        g_free(layers);
        GError *err = g_error_new(GIMP_PLUG_IN_ERROR, 0,
                                  "No layers to export.");
        return gimp_procedure_new_return_values(procedure,
                   GIMP_PDB_EXECUTION_ERROR, err);
    }

    GimpDrawable *drawable = GIMP_DRAWABLE(layers[0]);
    g_free(layers);

    /* Determine channel layout and GPU format hint. */
    gboolean has_alpha = gimp_drawable_has_alpha(drawable);
    gboolean is_gray   = gimp_drawable_is_gray(drawable);
    const gchar *babl_fmt;
    uint8_t ch;
    uint8_t gpu_fmt;
    uint8_t flags = YAPF_FLAG_SRGB;  /* GIMP's native u8 formats are sRGB */

    if (is_gray) {
        if (has_alpha) {
            ch = YAPF_CHANNELS_GRAY_ALPHA; babl_fmt = "Y'A u8";
            gpu_fmt = YAPF_GPU_RG8;
            flags   = 0;  /* grayscale has no sRGB GPU hint */
        } else {
            ch = YAPF_CHANNELS_GRAY; babl_fmt = "Y' u8";
            gpu_fmt = YAPF_GPU_R8;
            flags   = 0;
        }
    } else {
        if (has_alpha) {
            ch = YAPF_CHANNELS_RGBA; babl_fmt = "R'G'B'A u8";
            gpu_fmt = YAPF_GPU_SRGB8_A8;
        } else {
            ch = YAPF_CHANNELS_RGB; babl_fmt = "R'G'B' u8";
            gpu_fmt = YAPF_GPU_SRGB8;
        }
    }

    gint w = gimp_drawable_get_width(drawable);
    gint h = gimp_drawable_get_height(drawable);

    /* Read pixel data from GIMP's buffer. */
    uint8_t *pixels = (uint8_t *)malloc((size_t)w * (size_t)h * ch);
    if (!pixels) {
        GError *err = g_error_new(GIMP_PLUG_IN_ERROR, 0, "Out of memory.");
        return gimp_procedure_new_return_values(procedure,
                   GIMP_PDB_EXECUTION_ERROR, err);
    }

    GeglBuffer *buffer = gimp_drawable_get_buffer(drawable);
    gegl_buffer_get(buffer,
        GEGL_RECTANGLE(0, 0, w, h),
        1.0, babl_format(babl_fmt),
        pixels, GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);
    g_object_unref(buffer);

    /* Build yapf_image_t — base level only, no mip chain from GIMP. */
    yapf_image_t img;
    img.width      = (uint32_t)w;
    img.height     = (uint32_t)h;
    img.channels   = ch;
    img.gpu_format = gpu_fmt;
    img.flags      = flags;
    img.mip_levels = 1;
    img.pixels     = pixels;
    img.mips       = NULL;

    gchar *path   = g_file_get_path(file);
    int    result = yapf_save(path, &img);
    g_free(path);
    free(pixels);

    if (result != YAPF_OK) {
        GError *err = g_error_new(GIMP_PLUG_IN_ERROR, 0,
                                  "Failed to export YAPF file.");
        return gimp_procedure_new_return_values(procedure,
                   GIMP_PDB_EXECUTION_ERROR, err);
    }

    return gimp_procedure_new_return_values(procedure, GIMP_PDB_SUCCESS, NULL);
}

GIMP_MAIN(YAPF_PLUGIN_TYPE)
