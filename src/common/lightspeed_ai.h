/*
    This file is part of Lightspeed, a darktable fork.
    Copyright (C) 2026 Lightspeed developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

/*
  AI features of Lightspeed. The work is done by a helper script
  (share/darktable/lightspeed/ai/lsai.py, run by a bundled Python): local
  culling, Best Take, and requests to the AI command line tools installed on
  the computer (Claude Code, Codex, Gemini CLI), which use the user's
  subscription instead of an API key.
*/

#include "common/darktable.h"
#include "control/jobs.h"

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* run a helper command (cull, rate, besttake, genedit, providers) with a JSON
   request, from a job thread: progress lines update the job, cancelling the
   job stops the helper. Returns the response object (json_object_unref it) or
   NULL with *error set. */
JsonObject *dt_lsai_run(const char *command,
                        JsonObject *request,
                        dt_job_t *job,
                        GError **error);

/* the processed image (with its edits) as a JPEG, from the thumbnail cache */
gboolean dt_lsai_write_preview(const dt_imgid_t imgid,
                               const int max_size,
                               const char *path);

/* the processed image at full resolution, 16 bit sRGB TIFF */
gboolean dt_lsai_export_tiff(const dt_imgid_t imgid, const char *path);

/* import a file made from `source` (Best Take, AI edit) and group it with it */
dt_imgid_t dt_lsai_import_derived(const dt_imgid_t source, const char *path);

/* a new temporary folder for the helper files, g_free() it */
gchar *dt_lsai_tmpdir(void);

/* ---- edits made by the AI or a chat (non destructive, with undo) ---- */

/* the current edit of an image as Lightroom settings (Camera Raw names:
   Exposure2012, Temperature, HueAdjustmentRed...), with "raw" (TRUE for a raw
   file), "width"/"height" (the displayed image before the crop), the crop
   (CropLeft, CropTop, CropRight, CropBottom, 0..1) and Straighten (degrees).
   Loads the image: call it from a job. json_object_unref() it. */
JsonObject *dt_lsai_read_edit(const dt_imgid_t imgid);

/* change the edit of an image: Lightroom settings (only the given ones
   change) and/or the crop and straighten keys above, as new history items
   with undo. The darkroom follows when the image is open there. Call it
   from a job or the gui thread. */
gboolean dt_lsai_apply_edit(const dt_imgid_t imgid, JsonObject *edit);

/* copy the edit of an image to another one, the crop, straighten and
   retouching excluded (Match Look) */
gboolean dt_lsai_copy_edit(const dt_imgid_t src, const dt_imgid_t dst);

/* run a function in the gui thread and wait for it (from a job) */
void dt_lsai_in_gui(GSourceFunc func, gpointer data);

/* ---- choice of the AI tool, shared by the AI panels ---- */

typedef struct dt_lsai_provider_ui_t dt_lsai_provider_ui_t;

/* combobox of the AI tools (Claude Code, Codex, Gemini), the model, the
   state, and an Install / Connect button when a tool is missing or logged
   out. images_only: only tools that can create images. The choice is kept
   in conf_key. */
dt_lsai_provider_ui_t *dt_lsai_provider_ui_new(const char *conf_key,
                                               const char *default_provider,
                                               const gboolean images_only);
GtkWidget *dt_lsai_provider_ui_widget(dt_lsai_provider_ui_t *ui);
void dt_lsai_provider_ui_free(dt_lsai_provider_ui_t *ui);

/* chosen tool ("claude", "codex", "gemini") and model ("" = default),
   g_free() them; FALSE when the tool is not ready (message in *why) */
gboolean dt_lsai_provider_ui_get(dt_lsai_provider_ui_t *ui,
                                 gchar **provider,
                                 gchar **model,
                                 gchar **why);

/* check the installed tools again (after installing or connecting one) */
void dt_lsai_providers_refresh(void);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
