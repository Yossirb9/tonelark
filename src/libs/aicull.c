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

/*
  AI Culling (Library): choose the best photos.
    Find Best Shots  on this computer, no AI service: sharpness, exposure,
                     faces (eyes open, smile), bursts and the best of each
    Rate with AI     photos on numbered contact sheets sent to Claude Code,
                     Codex or Gemini (one request per sheet of 12), which
                     rates them; optional request ("looking at the camera")
    Best Take        a burst of a group: the best face of everyone blended
                     into one photo, grouped with the burst
*/

#include "common/act_on.h"
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/grouping.h"
#include "common/image_cache.h"
#include "common/lightspeed_ai.h"
#include "common/metadata.h"
#include "common/ratings.h"
#include "common/tags.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

#include <glib/gstdio.h>

DT_MODULE(1)

#define DT_PICK_TAG "darktable|pick"
#define AI_MATCH_TAG "lightspeed|ai match"
#define NOTES_KEY "Xmp.acdsee.notes"

typedef struct dt_lib_aicull_t
{
  GtkWidget *reject, *stars, *stack, *criteria, *status;
  dt_lsai_provider_ui_t *provider;
} dt_lib_aicull_t;

typedef enum _task_t
{
  TASK_CULL,
  TASK_RATE,
  TASK_BESTTAKE
} _task_t;

typedef struct _job_t
{
  _task_t task;
  GList *imgs;
  gboolean reject, stars, stack;
  gchar *provider, *model, *criteria;
  dt_lib_module_t *self;
  // results, applied in the gui thread
  JsonObject *result;
  gchar *error;
  dt_imgid_t made;
} _job_t;

const char *name(dt_lib_module_t *self)
{
  return _("AI culling");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 850;
}

static void _job_free(void *data)
{
  _job_t *j = data;
  g_list_free(j->imgs);
  g_free(j->provider);
  g_free(j->model);
  g_free(j->criteria);
  g_free(j->error);
  if(j->result) json_object_unref(j->result);
  g_free(j);
}

// ---------------------------------------------------------------------------
// applying the results (gui thread)

static void _set_status(dt_lib_module_t *self, const char *text)
{
  dt_lib_aicull_t *d = self->data;
  gtk_label_set_text(GTK_LABEL(d->status), text);
  gtk_widget_set_tooltip_text(d->status, text);
}

// the pick flags of the analysed photos are replaced by the new ones
static void _set_picks(GList *all, GList *picks)
{
  guint tagid = 0;
  dt_tag_new(DT_PICK_TAG, &tagid);
  if(all) dt_tag_detach_images(tagid, all, TRUE);
  if(picks) dt_tag_attach_images(tagid, picks, TRUE);
}

// darktable toggles the reject flag when every photo of a list is already
// rejected: only reject the others
static void _reject(GList *imgs)
{
  GList *todo = NULL;
  for(GList *l = imgs; l; l = g_list_next(l))
    if(dt_ratings_get(GPOINTER_TO_INT(l->data)) != DT_VIEW_REJECT)
      todo = g_list_prepend(todo, l->data);
  if(todo) dt_ratings_apply_on_list(todo, DT_VIEW_REJECT, TRUE);
  g_list_free(todo);
}

static void _note(const dt_imgid_t id, const char *text)
{
  if(text && *text) dt_metadata_set(id, NOTES_KEY, text, FALSE);
}

static void _apply_cull(_job_t *j)
{
  JsonArray *arr = json_object_get_array_member(j->result, "images");
  GList *picks = NULL, *rejects = NULL, *all = NULL;
  GHashTable *best_of = g_hash_table_new(g_direct_hash, g_direct_equal);
  int bursts = 0;

  // the best of each burst first
  for(guint k = 0; arr && k < json_array_get_length(arr); k++)
  {
    JsonObject *o = json_array_get_object_element(arr, k);
    const int burst = json_object_get_int_member(o, "burst");
    if(json_object_get_boolean_member(o, "best") && json_object_get_int_member(o, "burst_size") > 1)
    {
      g_hash_table_insert(best_of, GINT_TO_POINTER(burst + 1),
                          GINT_TO_POINTER((int)json_object_get_int_member(o, "id")));
      bursts++;
    }
  }

  dt_undo_start_group(darktable.undo, DT_UNDO_LIGHTTABLE);
  for(guint k = 0; arr && k < json_array_get_length(arr); k++)
  {
    JsonObject *o = json_array_get_object_element(arr, k);
    const dt_imgid_t id = json_object_get_int_member(o, "id");
    const int burst = json_object_get_int_member(o, "burst");
    const gboolean in_burst = json_object_get_int_member(o, "burst_size") > 1;
    all = g_list_prepend(all, GINT_TO_POINTER(id));
    if(j->stars)
      dt_ratings_apply_on_image(id, json_object_get_int_member(o, "stars"), FALSE, TRUE, FALSE);
    if(in_burst)
    {
      const dt_imgid_t best = GPOINTER_TO_INT(g_hash_table_lookup(best_of, GINT_TO_POINTER(burst + 1)));
      if(id == best)
        picks = g_list_prepend(picks, GINT_TO_POINTER(id));
      else
      {
        if(j->reject) rejects = g_list_prepend(rejects, GINT_TO_POINTER(id));
        if(j->stack && dt_is_valid_imgid(best)) dt_grouping_add_to_group(best, id);
      }
    }
    gchar *note = g_strdup_printf(_("Lightspeed quality %d%%%s%s"),
                                  (int)(json_object_get_double_member(o, "score") * 100.0),
                                  *json_object_get_string_member_with_default(o, "note", "") ? ": " : "",
                                  json_object_get_string_member_with_default(o, "note", ""));
    _note(id, note);
    g_free(note);
  }
  _set_picks(all, picks);
  _reject(rejects);
  dt_undo_end_group(darktable.undo);

  gchar *msg = g_strdup_printf(_("%d photos, %d bursts: the best of each is picked%s"),
                               g_list_length(all), bursts,
                               rejects ? _(", the others rejected") : "");
  _set_status(j->self, msg);
  dt_control_log("%s", msg);
  g_free(msg);

  dt_image_synch_xmps(all);
  g_list_free(all);
  g_list_free(picks);
  g_list_free(rejects);
  g_hash_table_destroy(best_of);
}

static void _apply_rate(_job_t *j)
{
  JsonArray *arr = json_object_get_array_member(j->result, "images");
  GList *picks = NULL, *rejects = NULL, *matches = NULL, *all = NULL;
  const char *who = json_object_get_string_member_with_default(j->result, "provider", "AI");

  dt_undo_start_group(darktable.undo, DT_UNDO_LIGHTTABLE);
  for(guint k = 0; arr && k < json_array_get_length(arr); k++)
  {
    JsonObject *o = json_array_get_object_element(arr, k);
    const dt_imgid_t id = json_object_get_int_member(o, "id");
    const double score = json_object_get_double_member(o, "score");
    const gboolean keep = json_object_get_boolean_member_with_default(o, "keep", FALSE);
    all = g_list_prepend(all, GINT_TO_POINTER(id));
    if(j->stars)
      dt_ratings_apply_on_image(id, CLAMP((int)(score / 2.0 + 0.5), 1, 5), FALSE, TRUE, FALSE);
    if(keep)
      picks = g_list_prepend(picks, GINT_TO_POINTER(id));
    else if(j->reject)
      rejects = g_list_prepend(rejects, GINT_TO_POINTER(id));
    JsonNode *m = json_object_get_member(o, "match");
    if(m && JSON_NODE_HOLDS_VALUE(m) && json_node_get_boolean(m))
      matches = g_list_prepend(matches, GINT_TO_POINTER(id));
    gchar *note = g_strdup_printf("%s %.1f/10: %s", who, score,
                                  json_object_get_string_member_with_default(o, "reason", ""));
    _note(id, note);
    g_free(note);
  }
  _set_picks(all, picks);
  _reject(rejects);
  if(matches)
  {
    guint tagid = 0;
    dt_tag_new(AI_MATCH_TAG, &tagid);
    dt_tag_attach_images(tagid, matches, TRUE);
    dt_colorlabels_set_labels(matches, 2, FALSE, TRUE);   // green
  }
  dt_undo_end_group(darktable.undo);

  JsonArray *errors = json_object_get_array_member(j->result, "errors");
  const int nerr = errors ? json_array_get_length(errors) : 0;
  gchar *msg;
  if(!all && nerr)
    msg = g_strdup_printf(_("the AI did not answer: %s"), json_array_get_string_element(errors, 0));
  else if(j->criteria && *j->criteria)
    msg = g_strdup_printf(_("%d photos rated, %d kept, %d match \"%s\" (green label)"),
                          g_list_length(all), g_list_length(picks), g_list_length(matches), j->criteria);
  else
    msg = g_strdup_printf(_("%d photos rated, %d kept (pick flag)"), g_list_length(all), g_list_length(picks));
  _set_status(j->self, msg);
  dt_control_log("%s", msg);
  g_free(msg);

  dt_image_synch_xmps(all);
  g_list_free(all);
  g_list_free(picks);
  g_list_free(rejects);
  g_list_free(matches);
}

static gboolean _job_done(gpointer data)
{
  _job_t *j = data;
  if(j->error)
  {
    _set_status(j->self, j->error);
    dt_control_log("%s", j->error);
  }
  else if(j->task == TASK_CULL)
    _apply_cull(j);
  else if(j->task == TASK_RATE)
    _apply_rate(j);
  else
  {
    const int n = json_object_get_int_member(j->result, "replaced");
    gchar *msg = n > 0
      ? g_strdup_printf(ngettext("Best Take: %d face replaced, the new photo is on top of the burst group",
                                 "Best Take: %d faces replaced, the new photo is on top of the burst group", n), n)
      : g_strdup(_("Best Take: every face is already at its best in one photo, it is on top of the burst group"));
    _set_status(j->self, msg);
    dt_control_log("%s", msg);
    g_free(msg);
  }
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                             DT_COLLECTION_PROP_UNDEF, NULL);
  _job_free(j);
  return G_SOURCE_REMOVE;
}

// ---------------------------------------------------------------------------
// the jobs

static JsonArray *_previews(_job_t *j, dt_job_t *job, const char *dir, const int size)
{
  JsonArray *arr = json_array_new();
  const int n = g_list_length(j->imgs);
  int k = 0;
  for(GList *l = j->imgs; l; l = g_list_next(l), k++)
  {
    if(dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED) break;
    const dt_imgid_t id = GPOINTER_TO_INT(l->data);
    dt_control_job_set_progress_message(job, _("preparing %d/%d"), k + 1, n);
    dt_control_job_set_progress(job, 0.3 * k / MAX(1, n));
    gchar *name = g_strdup_printf("%d.jpg", id);
    gchar *path = g_build_filename(dir, name, NULL);
    g_free(name);
    if(dt_lsai_write_preview(id, size, path))
    {
      JsonObject *o = json_object_new();
      json_object_set_int_member(o, "id", id);
      json_object_set_string_member(o, "path", path);
      const dt_image_t *img = dt_image_cache_get(id, 'r');
      if(img && img->exif_datetime_taken)
        json_object_set_double_member(o, "time", (double)img->exif_datetime_taken / G_TIME_SPAN_SECOND);
      dt_image_cache_read_release(img);
      json_array_add_object_element(arr, o);
    }
    g_free(path);
  }
  return arr;
}

static void _rm_dir(const char *dir)
{
  GDir *d = g_dir_open(dir, 0, NULL);
  if(d)
  {
    const char *f;
    while((f = g_dir_read_name(d)))
    {
      gchar *p = g_build_filename(dir, f, NULL);
      g_unlink(p);
      g_free(p);
    }
    g_dir_close(d);
  }
  g_rmdir(dir);
}

// bursts by capture time, for the AI prompt
static JsonArray *_time_bursts(JsonArray *images)
{
  JsonArray *bursts = json_array_new();
  JsonArray *current = NULL;
  double last = -1e9;
  for(guint k = 0; k < json_array_get_length(images); k++)
  {
    JsonObject *o = json_array_get_object_element(images, k);
    const double t = json_object_get_double_member_with_default(o, "time", -1e9);
    if(!current || t < 0 || t - last > 2.0)
    {
      if(current) json_array_add_array_element(bursts, current);
      current = json_array_new();
    }
    json_array_add_int_element(current, json_object_get_int_member(o, "id"));
    last = t;
  }
  if(current) json_array_add_array_element(bursts, current);
  return bursts;
}

static int32_t _run_job(dt_job_t *job)
{
  _job_t *j = dt_control_job_get_params(job);
  gchar *dir = dt_lsai_tmpdir();
  JsonObject *req = json_object_new();
  GError *error = NULL;

  if(j->task == TASK_BESTTAKE)
  {
    // full resolution, with the edits
    JsonArray *frames = json_array_new();
    const int n = g_list_length(j->imgs);
    int k = 0;
    for(GList *l = j->imgs; l; l = g_list_next(l), k++)
    {
      const dt_imgid_t id = GPOINTER_TO_INT(l->data);
      dt_control_job_set_progress_message(job, _("exporting %d/%d"), k + 1, n);
      dt_control_job_set_progress(job, 0.4 * k / n);
      gchar *name = g_strdup_printf("%d.tif", id);
      gchar *path = g_build_filename(dir, name, NULL);
      g_free(name);
      if(dt_lsai_export_tiff(id, path))
      {
        JsonObject *o = json_object_new();
        json_object_set_int_member(o, "id", id);
        json_object_set_string_member(o, "path", path);
        json_array_add_object_element(frames, o);
      }
      g_free(path);
    }
    json_object_set_array_member(req, "frames", frames);

    // next to the first photo: <name>_besttake.tif
    const dt_imgid_t first = GPOINTER_TO_INT(j->imgs->data);
    char src[PATH_MAX] = { 0 };
    gboolean from_cache = FALSE;
    dt_image_full_path(first, src, sizeof(src), &from_cache);
    gchar *base = g_strdup(src);
    char *dot = strrchr(base, '.');
    if(dot) *dot = '\0';
    gchar *out = g_strdup_printf("%s_besttake.tif", base);
    for(int v = 2; g_file_test(out, G_FILE_TEST_EXISTS); v++)
    {
      g_free(out);
      out = g_strdup_printf("%s_besttake_%d.tif", base, v);
    }
    g_free(base);
    json_object_set_string_member(req, "output", out);
    j->result = dt_lsai_run("besttake", req, job, &error);
    if(j->result)
    {
      j->made = dt_lsai_import_derived(first, out);
      if(dt_is_valid_imgid(j->made))
      {
        dt_metadata_set(j->made, NOTES_KEY, _("Best Take: the best face of everyone in the burst"), FALSE);
        // one group: the burst under the Best Take
        for(GList *l = j->imgs; l; l = g_list_next(l))
          dt_grouping_add_to_group(j->made, GPOINTER_TO_INT(l->data));
        dt_grouping_change_representative(j->made);
      }
    }
    g_free(out);
  }
  else
  {
    JsonArray *images = _previews(j, job, dir, j->task == TASK_CULL ? 1600 : 900);
    if(j->task == TASK_RATE)
    {
      json_object_set_array_member(req, "bursts", _time_bursts(images));
      json_object_set_string_member(req, "provider", j->provider);
      json_object_set_string_member(req, "model", j->model ? j->model : "");
      json_object_set_string_member(req, "criteria", j->criteria ? j->criteria : "");
      json_object_set_int_member(req, "per_sheet", 12);
    }
    json_object_set_array_member(req, "images", images);
    j->result = dt_lsai_run(j->task == TASK_CULL ? "cull" : "rate", req, job, &error);
  }

  if(!j->result)
    j->error = g_strdup(error ? error->message : _("the AI helper failed"));
  g_clear_error(&error);
  json_object_unref(req);
  _rm_dir(dir);
  g_free(dir);
  g_idle_add(_job_done, j);
  return 0;
}

static void _start(dt_lib_module_t *self, const _task_t task)
{
  dt_lib_aicull_t *d = self->data;
  GList *imgs = dt_act_on_get_images(FALSE, TRUE, TRUE);
  if(!imgs)
  {
    dt_control_log(_("select the photos first"));
    return;
  }
  if(task == TASK_BESTTAKE && !imgs->next)
  {
    dt_control_log(_("Best Take: select the photos of one burst (2 or more)"));
    g_list_free(imgs);
    return;
  }

  _job_t *j = g_malloc0(sizeof(_job_t));
  j->task = task;
  j->imgs = imgs;
  j->self = self;
  j->reject = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->reject));
  j->stars = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->stars));
  j->stack = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->stack));
  dt_conf_set_bool("plugins/lighttable/aicull/reject", j->reject);
  dt_conf_set_bool("plugins/lighttable/aicull/stars", j->stars);
  dt_conf_set_bool("plugins/lighttable/aicull/stack", j->stack);

  if(task == TASK_RATE)
  {
    gchar *why = NULL;
    if(!dt_lsai_provider_ui_get(d->provider, &j->provider, &j->model, &why))
    {
      dt_control_log("%s", why);
      _set_status(self, why);
      g_free(why);
      _job_free(j);
      return;
    }
    j->criteria = g_strdup(gtk_entry_get_text(GTK_ENTRY(d->criteria)));
  }

  const char *what = task == TASK_CULL ? _("finding the best shots")
                   : task == TASK_RATE ? _("rating with AI") : _("Best Take");
  dt_job_t *job = dt_control_job_create(_run_job, "%s", what);
  if(!job)
  {
    _job_free(j);
    return;
  }
  dt_control_job_add_progress(job, what, TRUE);
  dt_control_job_set_params(job, j, NULL);
  _set_status(self, what);
  dt_control_add_job(DT_JOB_QUEUE_USER_BG, job);
}

static void _cull_clicked(GtkButton *b, dt_lib_module_t *self)
{
  _start(self, TASK_CULL);
}

static void _rate_clicked(GtkButton *b, dt_lib_module_t *self)
{
  _start(self, TASK_RATE);
}

static void _besttake_clicked(GtkButton *b, dt_lib_module_t *self)
{
  _start(self, TASK_BESTTAKE);
}

static GtkWidget *_check(const char *label, const char *tooltip, const char *conf, const gboolean def)
{
  GtkWidget *w = gtk_check_button_new_with_label(label);
  gtk_widget_set_tooltip_text(w, tooltip);
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(w))), PANGO_ELLIPSIZE_END);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), dt_conf_key_exists(conf) ? dt_conf_get_bool(conf) : def);
  return w;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_aicull_t *d = g_malloc0(sizeof(dt_lib_aicull_t));
  self->data = d;

  GtkWidget *cull = dt_action_button_new(self, N_("Find Best Shots"), _cull_clicked, self,
                                         _("on this computer, no AI service: sharpness, exposure, eyes"
                                           " open and smiles, bursts and the best of each"), 0, 0);
  d->reject = _check(_("reject the rest"), _("reject the other photos of each burst, or what the AI would"
                                             " not keep"), "plugins/lighttable/aicull/reject", FALSE);
  d->stars = _check(_("set stars"), _("rate the photos from the quality (1 to 5 stars)"),
                    "plugins/lighttable/aicull/stars", TRUE);
  d->stack = _check(_("stack bursts"), _("group each burst with its best photo on top"),
                    "plugins/lighttable/aicull/stack", FALSE);

  d->provider = dt_lsai_provider_ui_new("plugins/lighttable/aicull/provider", "claude", FALSE);
  d->criteria = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(d->criteria), _("looking for... (optional)"));
  gtk_widget_set_tooltip_text(d->criteria, _("what you are looking for, e.g. \"she looks at the camera\":"
                                             " the matching photos get a green label"));
  GtkWidget *rate = dt_action_button_new(self, N_("Rate with AI"), _rate_clicked, self,
                                         _("the photos go to the AI on numbered contact sheets (12 per"
                                           " request): stars, pick flags and a note for every photo"), 0, 0);
  GtkWidget *besttake = dt_action_button_new(self, N_("Best Take"), _besttake_clicked, self,
                                             _("select the photos of a group burst: the best face of"
                                               " everyone is blended into a new photo"), 0, 0);
  d->status = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(d->status), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(d->status), TRUE);
  // wrap in the width of the panel instead of widening it
  gtk_label_set_line_wrap_mode(GTK_LABEL(d->status), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(d->status), 1);
  gtk_widget_set_name(d->status, "lsai-state");

  self->widget = dt_gui_vbox(
    dt_ui_section_label_new(C_("section", "on this computer")),
    dt_gui_hbox(dt_gui_expand(d->stars), dt_gui_expand(d->reject)),
    d->stack, cull,
    dt_ui_section_label_new(C_("section", "AI assistant")),
    dt_lsai_provider_ui_widget(d->provider), d->criteria, rate,
    dt_ui_section_label_new(C_("section", "group photo")),
    besttake, d->status);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_aicull_t *d = self->data;
  dt_lsai_provider_ui_free(d->provider);
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
