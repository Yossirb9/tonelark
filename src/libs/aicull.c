/*
    This file is part of Tonelark, a darktable fork.
    Copyright (C) 2026 Tonelark developers.

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
                     rates them; with a request ("the best portrait") the
                     score is how well each photo answers it. The reason of
                     each score is in the notes, and the ratings list of the
                     panel shows the rated photos of the collection, best first
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
#include "common/selection.h"
#include "common/tags.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "bauhaus/bauhaus.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

#include <glib/gstdio.h>

DT_MODULE(1)

#define DT_PICK_TAG "darktable|pick"
#define AI_MATCH_TAG "tonelark|ai match"
#define NOTES_KEY "Xmp.acdsee.notes"
#define CONF "plugins/lighttable/aicull/"
#define RATINGS_MAX 500

static const char *_languages[] = { "English", "Hebrew", "Spanish", "French", "German", "Italian", "Portuguese",
                                    "Russian", "Arabic", NULL };

typedef struct dt_lib_aicull_t
{
  GtkWidget *reject, *stars, *stack, *criteria, *language, *status;
  GtkWidget *ratings, *ratings_head, *ratings_wrap;
  dt_lsai_provider_ui_t *provider;
  guint ratings_idle;
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
  gchar *provider, *model, *criteria, *language;
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
  g_free(j->language);
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

// darktable takes one star on a photo with one star as a toggle to none:
// only the photos whose stars change
static void _set_stars(const dt_imgid_t id, const int stars)
{
  if(dt_ratings_get(id) != stars) dt_ratings_apply_on_image(id, stars, FALSE, TRUE, FALSE);
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
      _set_stars(id, json_object_get_int_member(o, "stars"));
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
    gchar *note = g_strdup_printf(_("Tonelark quality %d%%%s%s"),
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

// ---------------------------------------------------------------------------
// the ratings list: the photos of the collection rated by the AI, best first,
// each with the reason of its score (the notes written by Rate with AI)

typedef struct _rating_t
{
  dt_imgid_t id;
  double score;
  gchar *request, *reason, *file;
} _rating_t;

static void _rating_free(gpointer data)
{
  _rating_t *r = data;
  g_free(r->request);
  g_free(r->reason);
  g_free(r->file);
  g_free(r);
}

static gint _rating_cmp(gconstpointer a, gconstpointer b)
{
  const _rating_t *ra = a, *rb = b;
  if(ra->score != rb->score) return ra->score < rb->score ? 1 : -1;
  return g_strcmp0(ra->file, rb->file);
}

// a note of Rate with AI (see _rate_note), NULL for any other note
static _rating_t *_rating_parse(const char *note)
{
  static GRegex *re = NULL;
  if(!re)
    re = g_regex_new("^\\S+ ([0-9]+(?:[.,][0-9]+)?)/10(?: for “(.*?)”)?: (.*)$", G_REGEX_DOTALL, 0, NULL);
  GMatchInfo *m = NULL;
  _rating_t *r = NULL;
  if(re && note && g_regex_match(re, note, 0, &m))
  {
    r = g_malloc0(sizeof(_rating_t));
    gchar *num = g_match_info_fetch(m, 1);
    g_strdelimit(num, ",", '.');
    r->score = g_ascii_strtod(num, NULL);
    g_free(num);
    r->request = g_match_info_fetch(m, 2);
    r->reason = g_match_info_fetch(m, 3);
  }
  g_match_info_free(m);
  return r;
}

static gboolean _rating_pressed(GtkWidget *w, GdkEventButton *e, gpointer data)
{
  if(e->button != GDK_BUTTON_PRIMARY) return FALSE;
  const dt_imgid_t id = GPOINTER_TO_INT(data);
  dt_selection_select_single(darktable.selection, id);
  dt_thumbtable_set_offset_image(dt_ui_thumbtable(darktable.gui->ui), id, TRUE);
  if(e->type == GDK_2BUTTON_PRESS)
  {
    dt_control_set_mouse_over_id(id);
    dt_view_manager_switch(darktable.view_manager, "darkroom");
  }
  return TRUE;
}

// the photo of the row under the mouse: shown by image information, and acted on
static gboolean _rating_hover(GtkWidget *w, GdkEventCrossing *e, gpointer data)
{
  if(e->detail == GDK_NOTIFY_INFERIOR) return FALSE;
  dt_control_set_mouse_over_id(e->type == GDK_ENTER_NOTIFY ? GPOINTER_TO_INT(data) : NO_IMGID);
  return FALSE;
}

static void _ratings_update(dt_lib_module_t *self)
{
  dt_lib_aicull_t *d = self->data;
  if(!d || !d->ratings) return;
  GList *children = gtk_container_get_children(GTK_CONTAINER(d->ratings));
  for(GList *c = children; c; c = g_list_next(c)) gtk_widget_destroy(c->data);
  g_list_free(children);

  GList *list = NULL;
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT m.id, m.value, i.filename"
                              " FROM main.meta_data AS m"
                              " JOIN memory.collected_images AS c ON c.imgid = m.id"
                              " JOIN main.images AS i ON i.id = m.id"
                              " WHERE m.key = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dt_metadata_get_keyid(NOTES_KEY));
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    _rating_t *r = _rating_parse((const char *)sqlite3_column_text(stmt, 1));
    if(!r) continue;
    r->id = sqlite3_column_int(stmt, 0);
    r->file = g_strdup((const char *)sqlite3_column_text(stmt, 2));
    list = g_list_prepend(list, r);
  }
  sqlite3_finalize(stmt);
  list = g_list_sort(list, _rating_cmp);

  // the request, when all the photos were rated for the same one
  const char *request = list ? ((_rating_t *)list->data)->request : NULL;
  for(GList *l = list; l && request; l = g_list_next(l))
    if(g_strcmp0(((_rating_t *)l->data)->request, request)) request = NULL;

  const int n = g_list_length(list);
  gchar *head = !n ? g_strdup(_("no photo of this collection is rated by AI yet"))
              : request && *request
                ? g_strdup_printf(ngettext("%d photo rated for “%s”, best first",
                                           "%d photos rated for “%s”, best first", n), n, request)
                : g_strdup_printf(ngettext("%d photo rated, best first", "%d photos rated, best first", n), n);
  gtk_label_set_text(GTK_LABEL(d->ratings_head), head);
  g_free(head);

  int k = 0;
  for(GList *l = list; l && k < RATINGS_MAX; l = g_list_next(l), k++)
  {
    _rating_t *r = l->data;
    const int stars = dt_ratings_get(r->id);
    GString *st = g_string_new("");
    if(stars == DT_VIEW_REJECT)
      g_string_append(st, _("rejected"));
    else
      for(int s = 0; s < 5; s++) g_string_append(st, s < stars ? "★" : "☆");
    char num[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(num, sizeof(num), "%.1f", r->score);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>  %s  %s", num, st->str, r->file ? r->file : "");
    g_string_free(st, TRUE);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_name(title, "aicull-rating-title");

    // a request of its own when the photos were rated for different ones
    gchar *text = !request && r->request && *r->request
                  ? g_strdup_printf(_("for “%s”: %s"), r->request, r->reason) : g_strdup(r->reason);
    GtkWidget *why = gtk_label_new(text);
    g_free(text);
    gtk_label_set_xalign(GTK_LABEL(why), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(why), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(why), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(why), 1);
    gtk_widget_set_name(why, "aicull-rating-reason");

    GtkWidget *row = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(row), dt_gui_vbox(title, why));
    gtk_widget_set_name(row, "aicull-rating");
    gtk_widget_set_tooltip_text(row, _("click: select the photo\ndouble-click: open it in Develop"));
    gtk_widget_add_events(row, GDK_BUTTON_PRESS_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(row, "button-press-event", G_CALLBACK(_rating_pressed), GINT_TO_POINTER(r->id));
    g_signal_connect(row, "enter-notify-event", G_CALLBACK(_rating_hover), GINT_TO_POINTER(r->id));
    g_signal_connect(row, "leave-notify-event", G_CALLBACK(_rating_hover), GINT_TO_POINTER(r->id));
    gtk_box_pack_start(GTK_BOX(d->ratings), row, FALSE, FALSE, 0);
  }
  g_list_free_full(list, _rating_free);
  gtk_widget_set_visible(d->ratings_wrap, n > 0);
  gtk_widget_show_all(d->ratings);
}

static gboolean _ratings_idle(gpointer data)
{
  dt_lib_module_t *self = data;
  dt_lib_aicull_t *d = self->data;
  d->ratings_idle = 0;
  _ratings_update(self);
  return G_SOURCE_REMOVE;
}

// the collection, the stars or the notes changed: the list again, once
static void _ratings_queue(dt_lib_module_t *self)
{
  dt_lib_aicull_t *d = self->data;
  if(d && !d->ratings_idle) d->ratings_idle = g_timeout_add(300, _ratings_idle, self);
}

static void _collection_changed(gpointer instance, dt_collection_change_t query_change,
                                dt_collection_properties_t changed_property, gpointer imgs, const int next,
                                dt_lib_module_t *self)
{
  _ratings_queue(self);
}

static void _metadata_changed(gpointer instance, const int type, dt_lib_module_t *self)
{
  _ratings_queue(self);
}

static void _info_changed(gpointer instance, gpointer imgs, dt_lib_module_t *self)
{
  _ratings_queue(self);
}

// "Codex 8.5/10 for “the best portrait”: the reason", read back by the ratings list
static gchar *_rate_note(const char *who, const double score, const char *criteria, const char *reason)
{
  char num[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(num, sizeof(num), "%.1f", score);
  return criteria && *criteria ? g_strdup_printf("%s %s/10 for “%s”: %s", who, num, criteria, reason)
                               : g_strdup_printf("%s %s/10: %s", who, num, reason);
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
      _set_stars(id, CLAMP((int)(score / 2.0 + 0.5), 1, 5));
    if(keep)
      picks = g_list_prepend(picks, GINT_TO_POINTER(id));
    else if(j->reject)
      rejects = g_list_prepend(rejects, GINT_TO_POINTER(id));
    JsonNode *m = json_object_get_member(o, "match");
    if(m && JSON_NODE_HOLDS_VALUE(m) && json_node_get_boolean(m))
      matches = g_list_prepend(matches, GINT_TO_POINTER(id));
    gchar *note = _rate_note(who, score, j->criteria,
                             json_object_get_string_member_with_default(o, "reason", ""));
    _note(id, note);
    g_free(note);
  }
  _set_picks(all, picks);
  _reject(rejects);
  // the green label of the matches replaces the one of an earlier request
  guint tagid = 0;
  dt_tag_new(AI_MATCH_TAG, &tagid);
  GList *unmatched = NULL;
  for(GList *l = all; l; l = g_list_next(l))
    if(!g_list_find(matches, l->data) && dt_is_tag_attached(tagid, GPOINTER_TO_INT(l->data)))
    {
      unmatched = g_list_prepend(unmatched, l->data);
      dt_colorlabels_remove_label(GPOINTER_TO_INT(l->data), 2);
    }
  if(unmatched) dt_tag_detach_images(tagid, unmatched, TRUE);
  g_list_free(unmatched);
  if(matches)
  {
    dt_tag_attach_images(tagid, matches, TRUE);
    dt_colorlabels_set_labels(matches, 1 << 2, FALSE, TRUE);   // a mask of labels: green
  }
  dt_undo_end_group(darktable.undo);

  JsonArray *errors = json_object_get_array_member(j->result, "errors");
  const int nerr = errors ? json_array_get_length(errors) : 0;
  gchar *msg;
  if(!all && nerr)
    msg = g_strdup_printf(_("the AI did not answer: %s"), json_array_get_string_element(errors, 0));
  else if(j->criteria && *j->criteria)
    msg = g_strdup_printf(_("%d photos rated for \"%s\": %d kept, %d match it (green label)"),
                          g_list_length(all), j->criteria, g_list_length(picks), g_list_length(matches));
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
  _ratings_queue(j->self);
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
      json_object_set_string_member(req, "language", j->language ? j->language : "English");
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
    gchar *asked = dt_conf_get_string(CONF "criteria");
    if(g_strcmp0(asked, gtk_entry_get_text(GTK_ENTRY(d->criteria))))
      gtk_entry_set_text(GTK_ENTRY(d->criteria), asked);
    g_free(asked);
    j->criteria = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(d->criteria))));
    j->language = g_strdup(_languages[CLAMP(dt_bauhaus_combobox_get(d->language), 0,
                                            G_N_ELEMENTS(_languages) - 2)]);
    dt_conf_set_string(CONF "language", j->language);
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

// the request, also set by the chat tools (see _cmd_action of the bridge)
static void _criteria_changed(GtkEditable *e, gpointer data)
{
  dt_conf_set_string(CONF "criteria", gtk_entry_get_text(GTK_ENTRY(e)));
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
  dt_conf_set_string(CONF "criteria", "");
  g_signal_connect(d->criteria, "changed", G_CALLBACK(_criteria_changed), NULL);
  gtk_widget_set_tooltip_text(d->criteria, _("what you are looking for, e.g. \"the best portrait\" or \"she looks"
                                             " at the camera\": the photos are rated by how well they answer"
                                             " it (a photo that does not gets 1 or 2 stars), and the ones that"
                                             " do get a green label.\nempty: rated as a shoot, by quality"));

  // the language of the reasons, the one of the AI assistant at first
  d->language = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->language, NULL, N_("reasons in"));
  gtk_widget_set_tooltip_text(d->language, _("the language of the reason written for every photo"));
  gchar *lang = dt_conf_key_exists(CONF "language") ? dt_conf_get_string(CONF "language")
                                                    : dt_conf_get_string("plugins/lightspeed/ai/language");
  int sel = 0;
  for(int k = 0; _languages[k]; k++)
  {
    dt_bauhaus_combobox_add(d->language, _languages[k]);
    if(!g_strcmp0(lang, _languages[k])) sel = k;
  }
  g_free(lang);
  dt_bauhaus_combobox_set(d->language, sel);

  GtkWidget *rate = dt_action_button_new(self, N_("Rate with AI"), _rate_clicked, self,
                                         _("the photos go to the AI on numbered contact sheets (12 per"
                                           " request): stars, pick flags, and the reason of each score in"
                                           " the notes and in the ratings list below"), 0, 0);
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

  // the rated photos of the collection, best first
  d->ratings_head = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(d->ratings_head), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(d->ratings_head), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(d->ratings_head), 1);
  gtk_widget_set_name(d->ratings_head, "aicull-ratings-head");
  d->ratings = dt_gui_vbox();
  if(!dt_conf_key_exists(CONF "ratings_height")) dt_conf_set_int(CONF "ratings_height", DT_PIXEL_APPLY_DPI(420));
  d->ratings_wrap = dt_ui_resize_wrap(d->ratings, 60, CONF "ratings_height");

  self->widget = dt_gui_vbox(
    dt_ui_section_label_new(C_("section", "on this computer")),
    dt_gui_hbox(dt_gui_expand(d->stars), dt_gui_expand(d->reject)),
    d->stack, cull,
    dt_ui_section_label_new(C_("section", "AI assistant")),
    dt_lsai_provider_ui_widget(d->provider), d->criteria, d->language, rate,
    dt_ui_section_label_new(C_("section", "group photo")),
    besttake, d->status,
    dt_ui_section_label_new(C_("section", "AI ratings")),
    d->ratings_head, d->ratings_wrap);
  gtk_widget_show_all(self->widget);
  gtk_widget_set_no_show_all(d->ratings_wrap, TRUE);

  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_COLLECTION_CHANGED, _collection_changed);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_METADATA_CHANGED, _metadata_changed);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_IMAGE_INFO_CHANGED, _info_changed);
  _ratings_update(self);
}

void view_enter(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  _ratings_queue(self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_aicull_t *d = self->data;
  if(d->ratings_idle) g_source_remove(d->ratings_idle);
  dt_lsai_provider_ui_free(d->provider);
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
