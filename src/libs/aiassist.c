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
  AI assistant (Library and Develop): the AI works with the Lightroom sliders,
  the result is a normal edit that can be changed or undone.
    Auto Edit         edit with words ("warm golden hour"): the AI looks at the
                      photo and sets the sliders; for one photo it looks at the
                      result and refines it; several photos go on contact sheets
    Match Look        the look of a reference photo copied to the others, their
                      exposure and white balance matched on this computer,
                      optionally fine-tuned by the AI
    Suggest Crops     crop (any or a given aspect ratio) and straighten
    Keywords & Captions  keywords, title and caption, 12 photos per request
    Connect Chat      Claude Code / Codex / Gemini chats get the Lightspeed tools
*/

#include "bauhaus/bauhaus.h"
#include "common/act_on.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/image_cache.h"
#include "common/lightspeed_ai.h"
#include "common/metadata.h"
#include "common/tags.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"

#include <glib/gstdio.h>

DT_MODULE(1)

#define CONF "plugins/lightspeed/ai/"

static const char *_aspects[] = { "original", "free", "1:1", "4:5", "5:4", "2:3", "3:2", "16:9", "9:16", NULL };
static const char *_languages[] = { "English", "Hebrew", "Spanish", "French", "German", "Italian", "Portuguese",
                                    "Russian", "Arabic", NULL };

typedef struct dt_lib_aiassist_t
{
  GtkWidget *instruction, *refine, *reference, *match_ai, *aspect, *straighten, *language, *titles, *status;
  dt_lsai_provider_ui_t *provider;
  dt_imgid_t ref;
} dt_lib_aiassist_t;

typedef enum _task_t
{
  TASK_AUTOEDIT,
  TASK_MATCH,
  TASK_CROP,
  TASK_KEYWORDS,
  TASK_CHAT
} _task_t;

typedef struct _job_t
{
  _task_t task;
  GList *imgs;
  dt_imgid_t ref;
  gchar *provider, *model, *instruction, *aspect, *language;
  gboolean refine, match_ai, straighten, titles;
  dt_lib_module_t *self;
  // results for the gui thread
  JsonObject *result;
  gchar *message, *error;
  int changed;
} _job_t;

const char *name(dt_lib_module_t *self)
{
  return _("AI assistant");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_DARKROOM;
}

uint32_t container(dt_lib_module_t *self)
{
  return dt_view_get_current() == DT_VIEW_DARKROOM ? DT_UI_CONTAINER_PANEL_LEFT_CENTER
                                                   : DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 1003;
}

static void _job_free(void *data)
{
  _job_t *j = data;
  g_list_free(j->imgs);
  g_free(j->provider);
  g_free(j->model);
  g_free(j->instruction);
  g_free(j->aspect);
  g_free(j->language);
  g_free(j->message);
  g_free(j->error);
  if(j->result) json_object_unref(j->result);
  g_free(j);
}

static void _set_status(dt_lib_module_t *self, const char *text)
{
  dt_lib_aiassist_t *d = self->data;
  gtk_label_set_text(GTK_LABEL(d->status), text);
  gtk_widget_set_tooltip_text(d->status, text);
}

// ---------------------------------------------------------------------------
// helper files

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

static gboolean _cancelled(dt_job_t *job)
{
  return dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED;
}

// one photo for the helper: its preview, its name, and with `settings` its
// current edit
static JsonObject *_image(dt_job_t *job, const dt_imgid_t id, const char *dir, const int size,
                          const gboolean settings, const char *suffix)
{
  gchar *name = g_strdup_printf("%d%s.jpg", id, suffix ? suffix : "");
  gchar *path = g_build_filename(dir, name, NULL);
  g_free(name);
  JsonObject *o = NULL;
  if(dt_lsai_write_preview(id, size, path))
  {
    o = json_object_new();
    json_object_set_int_member(o, "id", id);
    json_object_set_string_member(o, "path", path);
    const dt_image_t *img = dt_image_cache_get(id, 'r');
    if(img)
    {
      json_object_set_string_member(o, "name", img->filename);
      // the light the camera captured (Match Look)
      JsonObject *exif = json_object_new();
      json_object_set_double_member(exif, "exposure", img->exif_exposure);
      json_object_set_double_member(exif, "aperture", img->exif_aperture);
      json_object_set_double_member(exif, "iso", img->exif_iso);
      json_object_set_object_member(o, "exif", exif);
    }
    dt_image_cache_read_release(img);
    if(settings)
    {
      JsonObject *edit = dt_lsai_read_edit(id);
      json_object_set_int_member(o, "width", json_object_get_int_member_with_default(edit, "width", 3));
      json_object_set_int_member(o, "height", json_object_get_int_member_with_default(edit, "height", 2));
      json_object_set_object_member(o, "settings", edit);
    }
  }
  g_free(path);
  return o;
}

static JsonArray *_images(_job_t *j, dt_job_t *job, GList *imgs, const char *dir, const int size,
                          const gboolean settings, const char *suffix, const float from, const float to)
{
  JsonArray *arr = json_array_new();
  const int n = g_list_length(imgs);
  int k = 0;
  for(GList *l = imgs; l && !_cancelled(job); l = g_list_next(l), k++)
  {
    dt_control_job_set_progress_message(job, _("preparing %d/%d"), k + 1, n);
    dt_control_job_set_progress(job, from + (to - from) * k / MAX(1, n));
    JsonObject *o = _image(job, GPOINTER_TO_INT(l->data), dir, size, settings, suffix);
    if(o) json_array_add_object_element(arr, o);
  }
  return arr;
}

// apply the "edit" of every result, returns how many photos changed
static int _apply(JsonObject *result, GString *summary)
{
  JsonArray *arr = json_object_get_array_member(result, "images");
  int n = 0;
  dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
  for(guint k = 0; arr && k < json_array_get_length(arr); k++)
  {
    JsonObject *o = json_array_get_object_element(arr, k);
    JsonObject *edit = json_object_has_member(o, "edit") ? json_object_get_object_member(o, "edit") : NULL;
    if(edit && json_object_get_size(edit) > 0
       && dt_lsai_apply_edit(json_object_get_int_member(o, "id"), edit))
      n++;
    const char *s = json_object_get_string_member_with_default(o, "summary",
                    json_object_get_string_member_with_default(o, "reason", ""));
    if(summary && s && *s && summary->len < 300)
      g_string_append_printf(summary, "%s%s", summary->len ? "; " : "", s);
  }
  dt_undo_end_group(darktable.undo);
  return n;
}

static gchar *_first_error(JsonObject *result)
{
  JsonArray *errors = json_object_has_member(result, "errors") ? json_object_get_array_member(result, "errors")
                                                               : NULL;
  return errors && json_array_get_length(errors) ? g_strdup(json_array_get_string_element(errors, 0)) : NULL;
}

// ---------------------------------------------------------------------------
// the jobs

static JsonObject *_request(_job_t *j)
{
  JsonObject *req = json_object_new();
  json_object_set_string_member(req, "provider", j->provider ? j->provider : "claude");
  json_object_set_string_member(req, "model", j->model ? j->model : "");
  return req;
}

static void _autoedit(_job_t *j, dt_job_t *job, const char *dir, GError **error)
{
  const gboolean single = j->imgs && !j->imgs->next;
  JsonObject *req = _request(j);
  json_object_set_string_member(req, "instruction", j->instruction ? j->instruction : "");
  json_object_set_array_member(req, "images", _images(j, job, j->imgs, dir, single ? 1600 : 900, TRUE, NULL,
                                                      0.0f, 0.2f));
  JsonObject *res = dt_lsai_run("autoedit", req, job, error);
  json_object_unref(req);
  if(!res) return;

  GString *summary = g_string_new(NULL);
  j->changed = _apply(res, summary);
  j->error = j->changed ? NULL : _first_error(res);
  json_object_unref(res);

  // one photo: the AI looks at the result and refines it
  if(single && j->refine && j->changed && !_cancelled(job))
  {
    dt_control_job_set_progress_message(job, _("the AI looks at the result"));
    req = _request(j);
    json_object_set_string_member(req, "instruction", j->instruction ? j->instruction : "");
    json_object_set_int_member(req, "round", 2);
    json_object_set_array_member(req, "images", _images(j, job, j->imgs, dir, 1600, TRUE, "_r", 0.5f, 0.6f));
    GError *err = NULL;
    res = dt_lsai_run("autoedit", req, job, &err);
    json_object_unref(req);
    if(res)
    {
      GString *more = g_string_new(NULL);
      if(_apply(res, more) && more->len) g_string_append_printf(summary, " / %s", more->str);
      g_string_free(more, TRUE);
      json_object_unref(res);
    }
    g_clear_error(&err);
  }
  j->message = j->changed
    ? g_strdup_printf(ngettext("%d photo edited: %s", "%d photos edited: %s", j->changed), j->changed,
                      summary->str)
    : g_strdup(_("the AI left the photos as they are"));
  g_string_free(summary, TRUE);
}

static void _match(_job_t *j, dt_job_t *job, const char *dir, GError **error)
{
  const int n = g_list_length(j->imgs);
  JsonObject *ref = _image(job, j->ref, dir, 900, TRUE, "_ref");
  if(!ref)
  {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, _("cannot render the reference photo"));
    return;
  }

  // the white balance of every photo before it gets the look
  GHashTable *before = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)json_object_unref);
  int k = 0;
  for(GList *l = j->imgs; l && !_cancelled(job); l = g_list_next(l), k++)
  {
    dt_control_job_set_progress_message(job, _("copying the look %d/%d"), k + 1, n);
    dt_control_job_set_progress(job, 0.4f * k / n);
    g_hash_table_insert(before, l->data, dt_lsai_read_edit(GPOINTER_TO_INT(l->data)));
    dt_lsai_copy_edit(j->ref, GPOINTER_TO_INT(l->data));
  }

  // exposure (camera settings, or the rendered photos) and white balance
  JsonObject *req = json_object_new();
  json_object_set_object_member(req, "reference", json_object_ref(ref));
  JsonArray *images = _images(j, job, j->imgs, dir, 900, TRUE, "_m", 0.4f, 0.6f);
  for(guint i = 0; i < json_array_get_length(images); i++)
  {
    JsonObject *o = json_array_get_object_element(images, i);
    JsonObject *b = g_hash_table_lookup(before, GINT_TO_POINTER(json_object_get_int_member(o, "id")));
    if(b) json_object_set_object_member(o, "before", json_object_ref(b));
  }
  json_object_set_array_member(req, "images", images);
  JsonObject *res = dt_lsai_run("match", req, job, error);
  json_object_unref(req);
  g_hash_table_destroy(before);
  const gboolean matched = res != NULL;
  if(res)
  {
    j->changed = _apply(res, NULL);
    json_object_unref(res);
  }

  if(matched && j->match_ai && !_cancelled(job))
  {
    dt_control_job_set_progress_message(job, _("the AI compares the photos"));
    req = _request(j);
    json_object_set_string_member(req, "instruction",
                                  "make each numbered photo match the look of the reference photo R (brightness,"
                                  " contrast, color, white balance, mood); they already have its settings, give"
                                  " only the small corrections that make the set consistent");
    json_object_set_object_member(req, "reference", json_object_ref(ref));
    json_object_set_array_member(req, "images", _images(j, job, j->imgs, dir, 900, TRUE, "_ai", 0.8f, 0.85f));
    GError *err = NULL;
    res = dt_lsai_run("autoedit", req, job, &err);
    json_object_unref(req);
    if(res)
    {
      _apply(res, NULL);
      json_object_unref(res);
    }
    else
      j->error = g_strdup(err ? err->message : _("the AI did not answer"));
    g_clear_error(&err);
  }
  json_object_unref(ref);
  j->message = g_strdup_printf(ngettext("%d photo matched to the reference", "%d photos matched to the reference",
                                        n), n);
}

static void _crop(_job_t *j, dt_job_t *job, const char *dir, GError **error)
{
  const gboolean single = j->imgs && !j->imgs->next;
  JsonObject *req = _request(j);
  json_object_set_string_member(req, "aspect", j->aspect ? j->aspect : "original");
  json_object_set_boolean_member(req, "straighten", j->straighten);
  json_object_set_array_member(req, "images", _images(j, job, j->imgs, dir, single ? 1600 : 900, TRUE, NULL,
                                                      0.0f, 0.2f));
  JsonObject *res = dt_lsai_run("crop", req, job, error);
  json_object_unref(req);
  if(!res) return;
  GString *summary = g_string_new(NULL);
  j->changed = _apply(res, summary);
  j->message = j->changed
    ? g_strdup_printf(ngettext("%d photo cropped: %s", "%d photos cropped: %s", j->changed), j->changed,
                      summary->str)
    : g_strdup(_("the AI would keep the photos as they are"));
  if(!j->changed) j->error = _first_error(res);
  g_string_free(summary, TRUE);
  json_object_unref(res);
}

static void _keywords(_job_t *j, dt_job_t *job, const char *dir, GError **error)
{
  JsonObject *req = _request(j);
  json_object_set_string_member(req, "language", j->language ? j->language : "English");
  json_object_set_boolean_member(req, "titles", j->titles);
  json_object_set_int_member(req, "per_sheet", 12);
  json_object_set_array_member(req, "images", _images(j, job, j->imgs, dir, 900, FALSE, NULL, 0.0f, 0.2f));
  j->result = dt_lsai_run("keywords", req, job, error);
  json_object_unref(req);
}

static void _chat(_job_t *j, dt_job_t *job, GError **error)
{
  JsonObject *req = json_object_new();
  JsonObject *res = dt_lsai_run("mcp", req, job, error);
  json_object_unref(req);
  if(!res) return;
  JsonObject *tools = json_object_get_object_member(res, "tools");
  GString *msg = g_string_new(NULL);
  static const struct { const char *id, *name; } names[] =
    { { "claude", N_("Claude Code") }, { "codex", N_("Codex") }, { "gemini", N_("Gemini") } };
  int ok = 0;
  for(int k = 0; k < G_N_ELEMENTS(names); k++)
  {
    const char *state = tools ? json_object_get_string_member_with_default(tools, names[k].id, "") : "";
    if(!g_strcmp0(state, "connected")) ok++;
    g_string_append_printf(msg, "%s%s: %s", msg->len ? "\n" : "", _(names[k].name),
                           !g_strcmp0(state, "connected") ? _("connected")
                           : !g_strcmp0(state, "not installed") ? _("not installed") : state);
  }
  j->message = ok ? g_strdup_printf(_("chats can now work with Lightspeed (open a new chat)\n%s"), msg->str)
                  : g_strdup_printf(_("no AI tool could be connected\n%s"), msg->str);
  g_string_free(msg, TRUE);
  json_object_unref(res);
}

static gboolean _job_done(gpointer data)
{
  _job_t *j = data;
  if(j->task == TASK_KEYWORDS && j->result)
  {
    JsonArray *arr = json_object_get_array_member(j->result, "images");
    GList *all = NULL;
    int n = 0;
    dt_undo_start_group(darktable.undo, DT_UNDO_TAGS);
    for(guint k = 0; arr && k < json_array_get_length(arr); k++)
    {
      JsonObject *o = json_array_get_object_element(arr, k);
      const dt_imgid_t id = json_object_get_int_member(o, "id");
      GList *one = g_list_prepend(NULL, GINT_TO_POINTER(id));
      JsonArray *kws = json_object_get_array_member(o, "keywords");
      for(guint i = 0; kws && i < json_array_get_length(kws); i++)
      {
        guint tagid = 0;
        dt_tag_new(json_array_get_string_element(kws, i), &tagid);
        if(tagid) dt_tag_attach_images(tagid, one, TRUE);
      }
      g_list_free(one);
      const char *title = json_object_get_string_member_with_default(o, "title", "");
      const char *caption = json_object_get_string_member_with_default(o, "caption", "");
      if(*title) dt_metadata_set(id, "Xmp.dc.title", title, TRUE);
      if(*caption) dt_metadata_set(id, "Xmp.dc.description", caption, TRUE);
      all = g_list_prepend(all, GINT_TO_POINTER(id));
      n++;
    }
    dt_undo_end_group(darktable.undo);
    dt_image_synch_xmps(all);
    g_list_free(all);
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
    g_free(j->message);
    j->message = n ? g_strdup_printf(ngettext("keywords, title and caption for %d photo",
                                              "keywords, title and caption for %d photos", n), n)
                   : NULL;
    if(!n && !j->error) j->error = _first_error(j->result);
  }

  const char *text = j->message ? j->message : j->error ? j->error : _("the AI did not answer");
  gchar *full = j->message && j->error ? g_strdup_printf("%s (%s)", j->message, j->error) : g_strdup(text);
  _set_status(j->self, full);
  dt_control_log("%s", full);
  g_free(full);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                             DT_COLLECTION_PROP_UNDEF, NULL);
  _job_free(j);
  return G_SOURCE_REMOVE;
}

static int32_t _run_job(dt_job_t *job)
{
  _job_t *j = dt_control_job_get_params(job);
  gchar *dir = dt_lsai_tmpdir();
  GError *error = NULL;
  switch(j->task)
  {
    case TASK_AUTOEDIT: _autoedit(j, job, dir, &error); break;
    case TASK_MATCH: _match(j, job, dir, &error); break;
    case TASK_CROP: _crop(j, job, dir, &error); break;
    case TASK_KEYWORDS: _keywords(j, job, dir, &error); break;
    case TASK_CHAT: _chat(j, job, &error); break;
  }
  if(error)
  {
    g_free(j->error);
    j->error = g_strdup(error->message);
  }
  g_clear_error(&error);
  _rm_dir(dir);
  g_free(dir);
  g_idle_add(_job_done, j);
  return 0;
}

// the photos to work on: the open photo in the darkroom, else the selection
static GList *_photos(void)
{
  if(dt_view_get_current() == DT_VIEW_DARKROOM && darktable.develop
     && dt_is_valid_imgid(darktable.develop->image_storage.id))
    return g_list_prepend(NULL, GINT_TO_POINTER(darktable.develop->image_storage.id));
  return dt_act_on_get_images(FALSE, TRUE, TRUE);
}

static void _update_reference(dt_lib_module_t *self)
{
  dt_lib_aiassist_t *d = self->data;
  gchar *text = NULL;
  const dt_image_t *img = dt_is_valid_imgid(d->ref) ? dt_image_cache_get(d->ref, 'r') : NULL;
  if(img)
    text = g_strdup_printf(_("reference: %s"), img->filename);
  else
    d->ref = NO_IMGID;
  dt_image_cache_read_release(img);
  gtk_label_set_text(GTK_LABEL(d->reference), text ? text : _("reference: none"));
  g_free(text);
}

static void _start(dt_lib_module_t *self, const _task_t task)
{
  dt_lib_aiassist_t *d = self->data;
  _job_t *j = g_malloc0(sizeof(_job_t));
  j->task = task;
  j->self = self;

  if(task != TASK_CHAT)
  {
    j->imgs = _photos();
    if(task == TASK_MATCH)
    {
      _update_reference(self);
      if(!dt_is_valid_imgid(d->ref))
      {
        dt_control_log(_("choose the reference photo first (Set Reference)"));
        _job_free(j);
        return;
      }
      j->imgs = g_list_remove(j->imgs, GINT_TO_POINTER(d->ref));
      j->ref = d->ref;
    }
    if(!j->imgs)
    {
      dt_control_log(task == TASK_MATCH ? _("select the photos to match to the reference")
                                        : _("select the photos first"));
      _job_free(j);
      return;
    }
  }

  const gboolean ai = task == TASK_AUTOEDIT || task == TASK_CROP || task == TASK_KEYWORDS
                      || (task == TASK_MATCH && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->match_ai)));
  if(ai)
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
  }

  // the instruction may come from a chat (conf) or from the entry
  gchar *instruction = dt_conf_get_string(CONF "instruction");
  if(g_strcmp0(instruction, gtk_entry_get_text(GTK_ENTRY(d->instruction))))
    gtk_entry_set_text(GTK_ENTRY(d->instruction), instruction);
  j->instruction = instruction;
  j->refine = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->refine));
  j->match_ai = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->match_ai));
  j->straighten = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->straighten));
  j->titles = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->titles));
  j->aspect = g_strdup(_aspects[CLAMP(dt_bauhaus_combobox_get(d->aspect), 0, G_N_ELEMENTS(_aspects) - 2)]);
  j->language = g_strdup(_languages[CLAMP(dt_bauhaus_combobox_get(d->language), 0,
                                          G_N_ELEMENTS(_languages) - 2)]);
  dt_conf_set_bool(CONF "refine", j->refine);
  dt_conf_set_bool(CONF "match_ai", j->match_ai);
  dt_conf_set_bool(CONF "straighten", j->straighten);
  dt_conf_set_bool(CONF "titles", j->titles);
  dt_conf_set_string(CONF "crop_aspect", j->aspect);
  dt_conf_set_string(CONF "language", j->language);

  const char *what = task == TASK_AUTOEDIT ? _("AI edit")
                   : task == TASK_MATCH ? _("matching the look")
                   : task == TASK_CROP ? _("AI crop")
                   : task == TASK_KEYWORDS ? _("AI keywords")
                   : _("connecting the chats");
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

static void _autoedit_clicked(GtkButton *b, dt_lib_module_t *self) { _start(self, TASK_AUTOEDIT); }
static void _match_clicked(GtkButton *b, dt_lib_module_t *self) { _start(self, TASK_MATCH); }
static void _crop_clicked(GtkButton *b, dt_lib_module_t *self) { _start(self, TASK_CROP); }
static void _keywords_clicked(GtkButton *b, dt_lib_module_t *self) { _start(self, TASK_KEYWORDS); }
static void _chat_clicked(GtkButton *b, dt_lib_module_t *self) { _start(self, TASK_CHAT); }

static void _reference_clicked(GtkButton *b, dt_lib_module_t *self)
{
  dt_lib_aiassist_t *d = self->data;
  GList *imgs = _photos();
  if(!imgs || imgs->next)
  {
    dt_control_log(_("select one photo, edited the way you like, as the reference"));
    g_list_free(imgs);
    return;
  }
  d->ref = GPOINTER_TO_INT(imgs->data);
  g_list_free(imgs);
  dt_conf_set_int(CONF "reference", d->ref);
  _update_reference(self);
}

static void _instruction_changed(GtkEntry *e, dt_lib_module_t *self)
{
  dt_conf_set_string(CONF "instruction", gtk_entry_get_text(e));
}

static GtkWidget *_check(const char *label, const char *tooltip, const char *conf, const gboolean def)
{
  GtkWidget *w = gtk_check_button_new_with_label(label);
  gtk_widget_set_tooltip_text(w, tooltip);
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(w))), PANGO_ELLIPSIZE_END);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), dt_conf_key_exists(conf) ? dt_conf_get_bool(conf) : def);
  return w;
}

static GtkWidget *_combo(dt_lib_module_t *self, const char *label, const char *tooltip, const char **items,
                         const char *conf, const gboolean translate)
{
  GtkWidget *w = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(w, NULL, label);
  gtk_widget_set_tooltip_text(w, tooltip);
  gchar *now = dt_conf_get_string(conf);
  int sel = 0;
  for(int k = 0; items[k]; k++)
  {
    dt_bauhaus_combobox_add(w, translate ? _(items[k]) : items[k]);
    if(!g_strcmp0(now, items[k])) sel = k;
  }
  g_free(now);
  dt_bauhaus_combobox_set(w, sel);
  return w;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_aiassist_t *d = g_malloc0(sizeof(dt_lib_aiassist_t));
  self->data = d;
  d->ref = dt_conf_key_exists(CONF "reference") ? dt_conf_get_int(CONF "reference") : NO_IMGID;

  d->provider = dt_lsai_provider_ui_new(CONF "provider", "claude", FALSE);

  d->instruction = gtk_entry_new();
  gchar *instruction = dt_conf_get_string(CONF "instruction");
  gtk_entry_set_text(GTK_ENTRY(d->instruction), instruction);
  g_free(instruction);
  gtk_entry_set_placeholder_text(GTK_ENTRY(d->instruction), _("the look, e.g. warm golden hour (optional)"));
  gtk_widget_set_tooltip_text(d->instruction, _("what the edit should do, in your words: \"bright and airy\","
                                                " \"moody film look\", \"fix the white balance\". Empty: a clean,"
                                                " balanced edit"));
  g_signal_connect(d->instruction, "changed", G_CALLBACK(_instruction_changed), self);
  d->refine = _check(_("look at the result and refine"),
                     _("one photo: the AI sees the edited photo and corrects it (a second request)"),
                     CONF "refine", TRUE);
  GtkWidget *autoedit = dt_action_button_new(self, N_("Auto Edit"), _autoedit_clicked, self,
                                             _("the AI sets the Lightroom sliders (exposure, white balance,"
                                               " tone, color, grading): a normal edit you can change or undo."
                                               " Several photos go 12 per request on contact sheets"), 0, 0);

  d->reference = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(d->reference), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(d->reference), PANGO_ELLIPSIZE_MIDDLE);
  GtkWidget *setref = dt_action_button_new(self, N_("Set Reference"), _reference_clicked, self,
                                           _("the photo whose look the others get"), 0, 0);
  GtkWidget *match = dt_action_button_new(self, N_("Match Look"), _match_clicked, self,
                                          _("the selected photos get the look of the reference, then their"
                                            " exposure and white balance are matched to it"), 0, 0);
  d->match_ai = _check(_("fine-tune with AI"), _("the AI compares every photo with the reference and"
                                                  " corrects what still differs"),
                       CONF "match_ai", FALSE);

  d->aspect = _combo(self, _("aspect"), _("aspect ratio of the crop"), _aspects, CONF "crop_aspect", TRUE);
  d->straighten = _check(_("straighten"), _("level tilted horizons and verticals"), CONF "straighten", TRUE);
  GtkWidget *crop = dt_action_button_new(self, N_("Suggest Crops"), _crop_clicked, self,
                                         _("the AI crops for a stronger composition: a normal crop you can"
                                           " change in the crop tool"), 0, 0);

  d->language = _combo(self, _("language"), _("language of the keywords, titles and captions"), _languages,
                       CONF "language", FALSE);
  d->titles = _check(_("title and caption"), _("also write a title and a caption"), CONF "titles", TRUE);
  GtkWidget *keywords = dt_action_button_new(self, N_("Keywords & Captions"), _keywords_clicked, self,
                                             _("keywords for searching, a title and a caption for every photo"
                                               " (12 photos per request)"), 0, 0);

  GtkWidget *chat = dt_action_button_new(self, N_("Connect Chat"), _chat_clicked, self,
                                         _("lets your Claude Code, Codex or Gemini chats see and edit the"
                                           " photos of Lightspeed (\"rate the sharpest photos of today\","
                                           " \"make these warmer\")"), 0, 0);

  d->status = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(d->status), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(d->status), TRUE);
  // wrap in the width of the panel instead of widening it
  gtk_label_set_line_wrap_mode(GTK_LABEL(d->status), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(d->status), 1);
  gtk_widget_set_name(d->status, "lsai-state");
  _update_reference(self);

  self->widget = dt_gui_vbox(
    dt_lsai_provider_ui_widget(d->provider),
    dt_ui_section_label_new(C_("section", "edit with words")),
    d->instruction, d->refine, autoedit,
    dt_ui_section_label_new(C_("section", "match look")),
    d->reference, dt_gui_hbox(dt_gui_expand(setref), dt_gui_expand(match)), d->match_ai,
    dt_ui_section_label_new(C_("section", "crop")),
    d->aspect, d->straighten, crop,
    dt_ui_section_label_new(C_("section", "keywords")),
    d->language, d->titles, keywords,
    dt_ui_section_label_new(C_("section", "chat")),
    chat, d->status);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_aiassist_t *d = self->data;
  dt_lsai_provider_ui_free(d->provider);
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
