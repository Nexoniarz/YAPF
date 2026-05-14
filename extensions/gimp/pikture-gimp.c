#include <libgimp/gimp.h>
#include "pikture.c"
#include <stdlib.h>

#define LOAD_PROC "file-pik-load"
#define SAVE_PROC "file-pik-save"

struct _PikturePlugin {
    GimpPlugIn parent_instance;
};

#define PIKTURE_PLUGIN_TYPE (pikture_plugin_get_type())
G_DECLARE_FINAL_TYPE (PikturePlugin, pikture_plugin, PIKTURE, PLUGIN, GimpPlugIn)

static GList * pikture_plugin_query_procedures (GimpPlugIn *plug_in);
static GimpProcedure * pikture_plugin_create_procedure (GimpPlugIn *plug_in, const gchar *name);

static GimpValueArray * pikture_load_run (GimpProcedure *procedure, GimpRunMode run_mode, GFile *file, GimpMetadata *metadata, GimpMetadataLoadFlags *flags, GimpProcedureConfig *config, gpointer run_data);
static GimpValueArray * pikture_save_run (GimpProcedure *procedure, GimpRunMode run_mode, GimpImage *image, GFile *file, GimpExportOptions *options, GimpMetadata *metadata, GimpProcedureConfig *config, gpointer run_data);

G_DEFINE_TYPE (PikturePlugin, pikture_plugin, GIMP_TYPE_PLUG_IN)

static void pikture_plugin_class_init (PikturePluginClass *klass) {
    GimpPlugInClass *plug_in_class = GIMP_PLUG_IN_CLASS (klass);
    plug_in_class->query_procedures = pikture_plugin_query_procedures;
    plug_in_class->create_procedure = pikture_plugin_create_procedure;
}

static void pikture_plugin_init (PikturePlugin *plugin) {}

static GList * pikture_plugin_query_procedures (GimpPlugIn *plug_in) {
    GList *list = NULL;
    list = g_list_append (list, g_strdup (LOAD_PROC));
    list = g_list_append (list, g_strdup (SAVE_PROC));
    return list;
}

static GimpProcedure * pikture_plugin_create_procedure (GimpPlugIn *plug_in, const gchar *name) {
    GimpProcedure *procedure = NULL;

    if (g_strcmp0 (name, LOAD_PROC) == 0) {
        procedure = gimp_load_procedure_new (plug_in, name, GIMP_PDB_PROC_TYPE_PLUGIN, pikture_load_run, NULL, NULL);
        gimp_procedure_set_menu_label (procedure, "PIKTURE image");
        gimp_procedure_set_documentation (procedure, "Loads a PIKTURE image", "Loads a PIKTURE image", NULL);
        gimp_procedure_set_attribution (procedure, "Nexoniarz", "Nexoniarz", "2026");
        gimp_file_procedure_set_mime_types (GIMP_FILE_PROCEDURE (procedure), "image/x-pikture");
        gimp_file_procedure_set_extensions (GIMP_FILE_PROCEDURE (procedure), "pik");
        gimp_file_procedure_set_magics (GIMP_FILE_PROCEDURE (procedure), "0,string,PIK!");
    } else if (g_strcmp0 (name, SAVE_PROC) == 0) {
        procedure = gimp_export_procedure_new (plug_in, name, GIMP_PDB_PROC_TYPE_PLUGIN, TRUE, (GimpRunExportFunc) pikture_save_run, NULL, NULL);
        
        gimp_procedure_set_image_types (procedure, "RGB, RGBA, GRAY, GRAYA");
        
        gimp_procedure_set_menu_label (procedure, "PIKTURE image");
        gimp_procedure_set_documentation (procedure, "Exports a PIKTURE image", "Exports a PIKTURE image", NULL);
        gimp_procedure_set_attribution (procedure, "Nexoniarz", "Nexoniarz", "2026");
        gimp_file_procedure_set_mime_types (GIMP_FILE_PROCEDURE (procedure), "image/x-pikture");
        gimp_file_procedure_set_extensions (GIMP_FILE_PROCEDURE (procedure), "pik");
    }

    return procedure;
}

static GimpValueArray * pikture_load_run (GimpProcedure *procedure, GimpRunMode run_mode, GFile *file, GimpMetadata *metadata, GimpMetadataLoadFlags *flags, GimpProcedureConfig *config, gpointer run_data) {
    GimpValueArray *return_vals;
    gchar *filename = g_file_get_path (file);
    
    pikture_t *pik = pikture_load(filename);
    g_free (filename);

    if (!pik) {
        GError *error = g_error_new (GIMP_PLUG_IN_ERROR, 0, "Failed to load PIKTURE file.");
        return gimp_procedure_new_return_values (procedure, GIMP_PDB_EXECUTION_ERROR, error);
    }

    GimpImageBaseType base_type = GIMP_RGB;
    GimpImageType layer_type = GIMP_RGBA_IMAGE;
    const gchar* b_format = "R'G'B'A u8";

    if (pik->channels == 1) {
        base_type = GIMP_GRAY;
        layer_type = GIMP_GRAY_IMAGE;
        b_format = "Y' u8";
    } else if (pik->channels == 2) {
        base_type = GIMP_GRAY;
        layer_type = GIMP_GRAYA_IMAGE;
        b_format = "Y'A u8";
    } else if (pik->channels == 3) {
        base_type = GIMP_RGB;
        layer_type = GIMP_RGB_IMAGE;
        b_format = "R'G'B' u8";
    }

    GimpImage *image = gimp_image_new (pik->width, pik->height, base_type);
    GimpLayer *layer = gimp_layer_new (image, "Background", pik->width, pik->height, layer_type, 100.0, gimp_image_get_default_new_layer_mode (image));
    gimp_image_insert_layer (image, layer, NULL, 0);

    GeglBuffer *buffer = gimp_drawable_get_buffer (GIMP_DRAWABLE (layer));
    gegl_buffer_set (buffer, GEGL_RECTANGLE (0, 0, pik->width, pik->height), 0, babl_format (b_format), pik->pixels, GEGL_AUTO_ROWSTRIDE);
    g_object_unref (buffer);

    pikture_free(pik);

    return_vals = gimp_procedure_new_return_values (procedure, GIMP_PDB_SUCCESS, NULL);
    g_value_set_object (gimp_value_array_index (return_vals, 1), image);
    return return_vals;
}

static GimpValueArray * pikture_save_run (GimpProcedure *procedure, GimpRunMode run_mode, GimpImage *image, GFile *file, GimpExportOptions *options, GimpMetadata *metadata, GimpProcedureConfig *config, gpointer run_data) {
    gchar *filename = g_file_get_path (file);

    GimpLayer **layers = gimp_image_get_layers (image);
    
    if (!layers || !layers[0]) {
        if (layers) g_free (layers);
        g_free (filename);
        GError *error = g_error_new (GIMP_PLUG_IN_ERROR, 0, "No layers to export.");
        return gimp_procedure_new_return_values (procedure, GIMP_PDB_EXECUTION_ERROR, error);
    }

    GimpDrawable *drawable = GIMP_DRAWABLE (layers[0]);

    pikture_t pik;
    pik.width = gimp_drawable_get_width (drawable);
    pik.height = gimp_drawable_get_height (drawable);

    gboolean has_alpha = gimp_drawable_has_alpha (drawable);
    gboolean is_gray = gimp_drawable_is_gray (drawable);
    const gchar* b_format = "R'G'B'A u8";

    if (is_gray) {
        if (has_alpha) {
            pik.channels = 2;
            b_format = "Y'A u8";
        } else {
            pik.channels = 1;
            b_format = "Y' u8";
        }
    } else {
        if (has_alpha) {
            pik.channels = 4;
            b_format = "R'G'B'A u8";
        } else {
            pik.channels = 3;
            b_format = "R'G'B' u8";
        }
    }

    pik.depth = pik.channels * 8;
    pik.pixels = malloc (pik.width * pik.height * pik.channels);

    GeglBuffer *buffer = gimp_drawable_get_buffer (drawable);
    gegl_buffer_get (buffer, GEGL_RECTANGLE (0, 0, pik.width, pik.height), 1.0, babl_format (b_format), pik.pixels, GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);
    g_object_unref (buffer);

    int result = pikture_save(filename, &pik);
    
    free (pik.pixels);
    g_free (filename);
    g_free (layers);

    if (!result) {
        GError *error = g_error_new (GIMP_PLUG_IN_ERROR, 0, "Failed to export PIKTURE file.");
        return gimp_procedure_new_return_values (procedure, GIMP_PDB_EXECUTION_ERROR, error);
    }

    return gimp_procedure_new_return_values (procedure, GIMP_PDB_SUCCESS, NULL);
}

GIMP_MAIN (PIKTURE_PLUGIN_TYPE)
