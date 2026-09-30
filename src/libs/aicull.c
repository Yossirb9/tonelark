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
                     score is how well each photo answers it; a red label
                     marks the candidates for deletion. Each run is kept as a
                     rating of its own: the panel lists them, puts the one
                     chosen back on the photos, and lists its photos, best
                     first, with the reason of each score
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
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "dtgtk/thumbtable.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

#include <glib/gstdio.h>

DT_MODULE(1)

#define DT_PICK_TAG "darktable|pick"
#define AI_MATCH_TAG "tonelark|ai match"
#define AI_DELETE_TAG "tonelark|ai delete"
#define NOTES_KEY "Xmp.acdsee.notes"
#define CONF "plugins/lighttable/aicull/"
#define RATINGS_MAX 500

static const char *_languages[] = { "English", "Hebrew", "Spanish", "French", "German", "Italian", "Portuguese",
                                    "Russian", "Arabic", NULL };

typedef struct dt_lib_aicull_t
{
  GtkWidget *reject, *stars, *stack, *criteria, *language, *status;
  GtkWidget *sets, *ratings, *ratings_head, *ratings_wrap;
  dt_lsai_provider_ui_t *provider;
  guint ratings_idle;
  gchar *sig;                 // what the ratings list shows
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
  GHashTable *twins;          // rate: the photo rated -> the other files of that shot
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
  if(j->twins) g_hash_table_destroy(j->twins);
  g_free(j);
}

// RAW+JPEG: one shot in two files. Rated apart, the same photo had two scores
// and two reasons, and the second one was a "near duplicate" to delete. The
// shot is rated once, on its raw file, and the other files get its result.
static GList *_one_per_shot(GList *imgs, GHashTable *twins)
{
  GHashTable *shot = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  // the raw files first: they stand for their shot
  for(int raw = 1; raw >= 0; raw--)
    for(GList *l = imgs; l; l = g_list_next(l))
    {
      const dt_imgid_t id = GPOINTER_TO_INT(l->data);
      const dt_image_t *img = dt_image_cache_get(id, 'r');
      if(!img) continue;
      const gboolean is_raw = dt_image_is_raw(img);
      gchar *stem = g_ascii_strdown(img->filename, -1);
      char *dot = strrchr(stem, '.');
      if(dot) *dot = '\0';
      gchar *key = g_strdup_printf("%d/%s", img->film_id, stem);
      dt_image_cache_read_release(img);
      g_free(stem);
      if(is_raw != raw)
      {
        g_free(key);
        continue;
      }
      gpointer first = g_hash_table_lookup(shot, key);
      if(!first)
        g_hash_table_insert(shot, key, GINT_TO_POINTER(id));
      else
      {
        GList *others = g_hash_table_lookup(twins, first);
        g_hash_table_steal(twins, first);
        g_hash_table_insert(twins, first, g_list_append(others, GINT_TO_POINTER(id)));
        g_free(key);
      }
    }
  // in the order of the photos (the bursts of a sheet follow each other)
  GHashTable *second = g_hash_table_new(g_direct_hash, g_direct_equal);
  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, twins);
  while(g_hash_table_iter_next(&it, &k, &v))
    for(GList *l = v; l; l = g_list_next(l)) g_hash_table_add(second, l->data);
  GList *out = NULL;
  for(GList *l = imgs; l; l = g_list_next(l))
    if(!g_hash_table_contains(second, l->data)) out = g_list_prepend(out, l->data);
  g_hash_table_destroy(second);
  g_hash_table_destroy(shot);
  return g_list_reverse(out);
}

static void _twins_free(gpointer data)
{
  g_list_free(data);
}

// ---------------------------------------------------------------------------
// the ratings. Each run of Rate with AI (one rating per request) and of Find
// Best Shots is kept in a table, with the score, the reason, and the stars,
// pick flag, labels and note it gave each photo. Choosing a rating puts
// them back on the photos; what the user changes meanwhile stays with the
// rating on the photo (the "applied" one). The stars a photo had before its
// first rating are a rating too, "my stars, before AI": nothing is lost.
// The labels of the AI: green for the photos that answer the request, red
// for the candidates for deletion; a tag tells them from the user's own.

typedef enum _kind_t
{
  KIND_AI = 0,       // Rate with AI, for a request ("" none)
  KIND_BEFORE = 1,   // the photo before its first rating
  KIND_CULL = 2      // Find Best Shots
} _kind_t;

typedef struct _result_t
{
  dt_imgid_t id;
  double score;          // 0 to 10
  int stars;             // 0 to 5, or DT_VIEW_REJECT
  gboolean picked, matched, trash;
  gchar *reason, *note;
} _result_t;

static const int _ai_color[2] = { 2, 0 };   // green: answers the request, red: to delete
static const char *_ai_tag[2] = { AI_MATCH_TAG, AI_DELETE_TAG };

static void _set_status(dt_lib_module_t *self, const char *text)
{
  dt_lib_aicull_t *d = self->data;
  gtk_label_set_text(GTK_LABEL(d->status), text);
  gtk_widget_set_tooltip_text(d->status, text);
}

static guint _tag(const char *name)
{
  guint tagid = 0;
  dt_tag_new(name, &tagid);
  return tagid;
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

static gchar *_get_note(const dt_imgid_t id)
{
  GList *res = dt_metadata_get(id, NOTES_KEY, NULL);
  gchar *note = res ? g_strdup(res->data) : NULL;
  g_list_free_full(res, g_free);
  return note;
}

static void _db_exec(const char *sql)
{
  char *err = NULL;
  sqlite3_exec(dt_database_get(darktable.db), sql, NULL, NULL, &err);
  if(err) dt_print(DT_DEBUG_ALWAYS, "[aicull] %s: %s", sql, err);
  sqlite3_free(err);
}

// a note of Rate with AI (see _rate_note): its score, request and reason
static gboolean _parse_note(const char *note, double *score, gchar **request, gchar **reason)
{
  static GRegex *re = NULL;
  if(!re)
    re = g_regex_new("^\\S+ ([0-9]+(?:[.,][0-9]+)?)/10(?: for “(.*?)”)?:\\s+(.*)$", G_REGEX_DOTALL, 0, NULL);
  GMatchInfo *m = NULL;
  const gboolean ok = re && note && g_regex_match(re, note, 0, &m);
  if(ok)
  {
    gchar *num = g_match_info_fetch(m, 1);
    g_strdelimit(num, ",", '.');
    *score = g_ascii_strtod(num, NULL);
    g_free(num);
    *request = g_match_info_fetch(m, 2);
    *reason = g_match_info_fetch(m, 3);
  }
  g_match_info_free(m);
  return ok;
}

static void _db_init(void)
{
  // clang-format off
  _db_exec("CREATE TABLE IF NOT EXISTS main.ls_ratings"
           " (imgid INTEGER NOT NULL, kind INTEGER NOT NULL, request TEXT NOT NULL,"
           "  score REAL, reason TEXT, stars INTEGER, picked INTEGER, matched INTEGER, trash INTEGER, note TEXT,"
           "  applied INTEGER NOT NULL DEFAULT 0, time INTEGER,"
           "  PRIMARY KEY (imgid, kind, request))");
  _db_exec("CREATE INDEX IF NOT EXISTS main.ls_ratings_set ON ls_ratings (kind, request)");
  _db_exec("DELETE FROM main.ls_ratings WHERE imgid NOT IN (SELECT id FROM main.images)");
  // clang-format on

  // once: the ratings of the notes written before the table, as ratings on the photos
  if(dt_conf_get_bool(CONF "ratings_table")) return;
  dt_conf_set_bool(CONF "ratings_table", TRUE);
  sqlite3_stmt *stmt, *ins;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id, value FROM main.meta_data WHERE key = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dt_metadata_get_keyid(NOTES_KEY));
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT OR IGNORE INTO main.ls_ratings"
                              " (imgid, kind, request, score, reason, stars, picked, matched, note, applied, time)"
                              " VALUES (?1, 0, ?2, ?3, ?4, ?5, ?6, ?7, ?8, 1, 0)",
                              -1, &ins, NULL);
  // clang-format on
  const guint pick = _tag(DT_PICK_TAG), match = _tag(AI_MATCH_TAG);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const dt_imgid_t id = sqlite3_column_int(stmt, 0);
    const char *note = (const char *)sqlite3_column_text(stmt, 1);
    double score = 0.0;
    gchar *request = NULL, *reason = NULL;
    if(!_parse_note(note, &score, &request, &reason)) continue;
    DT_DEBUG_SQLITE3_BIND_INT(ins, 1, id);
    DT_DEBUG_SQLITE3_BIND_TEXT(ins, 2, request, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(ins, 3, score);
    DT_DEBUG_SQLITE3_BIND_TEXT(ins, 4, reason, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(ins, 5, dt_ratings_get(id));
    DT_DEBUG_SQLITE3_BIND_INT(ins, 6, dt_is_tag_attached(pick, id));
    DT_DEBUG_SQLITE3_BIND_INT(ins, 7, dt_is_tag_attached(match, id));
    DT_DEBUG_SQLITE3_BIND_TEXT(ins, 8, note, -1, SQLITE_TRANSIENT);
    sqlite3_step(ins);
    sqlite3_reset(ins);
    sqlite3_clear_bindings(ins);
    g_free(request);
    g_free(reason);
  }
  sqlite3_finalize(ins);
  sqlite3_finalize(stmt);
}

// what the photos have now goes back into the rating on them, or becomes
// their own rating ("before AI") when they have none
static void _save_current(GList *imgs)
{
  sqlite3 *db = dt_database_get(darktable.db);
  sqlite3_stmt *upd, *ins;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(db,
                              "UPDATE main.ls_ratings SET stars = ?2, picked = ?3, note = ?6,"
                              "  matched = CASE kind WHEN 1 THEN ?4 ELSE ?5 END,"
                              "  trash = CASE kind WHEN 1 THEN ?8 ELSE ?9 END"
                              " WHERE imgid = ?1 AND applied = 1",
                              -1, &upd, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(db,
                              "INSERT OR IGNORE INTO main.ls_ratings"
                              " (imgid, kind, request, stars, picked, matched, trash, note, applied, time)"
                              " SELECT ?1, 1, '', ?2, ?3, ?4, ?8, ?6, 1, ?7"
                              " WHERE NOT EXISTS (SELECT 1 FROM main.ls_ratings WHERE imgid = ?1)",
                              -1, &ins, NULL);
  // clang-format on
  const guint pick = _tag(DT_PICK_TAG), match = _tag(AI_MATCH_TAG), trash = _tag(AI_DELETE_TAG);
  const gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  for(GList *l = imgs; l; l = g_list_next(l))
  {
    const dt_imgid_t id = GPOINTER_TO_INT(l->data);
    const int labels = dt_colorlabels_get_labels(id);
    const gboolean green = (labels & (1 << _ai_color[0])) != 0, red = (labels & (1 << _ai_color[1])) != 0;
    gchar *note = _get_note(id);
    for(int k = 0; k < 2; k++)
    {
      sqlite3_stmt *s = k ? ins : upd;
      DT_DEBUG_SQLITE3_BIND_INT(s, 1, id);
      DT_DEBUG_SQLITE3_BIND_INT(s, 2, dt_ratings_get(id));
      DT_DEBUG_SQLITE3_BIND_INT(s, 3, dt_is_tag_attached(pick, id));
      DT_DEBUG_SQLITE3_BIND_INT(s, 4, green);
      DT_DEBUG_SQLITE3_BIND_INT(s, 5, green && dt_is_tag_attached(match, id));
      DT_DEBUG_SQLITE3_BIND_TEXT(s, 6, note, -1, SQLITE_TRANSIENT);
      if(k) DT_DEBUG_SQLITE3_BIND_INT64(s, 7, now);
      DT_DEBUG_SQLITE3_BIND_INT(s, 8, red);
      if(!k) DT_DEBUG_SQLITE3_BIND_INT(s, 9, red && dt_is_tag_attached(trash, id));
      sqlite3_step(s);
      sqlite3_reset(s);
      sqlite3_clear_bindings(s);
      if(!k && sqlite3_changes(db) > 0) break;   // it had a rating on it
    }
    g_free(note);
  }
  sqlite3_finalize(upd);
  sqlite3_finalize(ins);
}

static GList *_set_images(const int kind, const char *request)
{
  GList *imgs = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid FROM main.ls_ratings WHERE kind = ?1 AND request = ?2", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, kind);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, request, -1, SQLITE_TRANSIENT);
  while(sqlite3_step(stmt) == SQLITE_ROW) imgs = g_list_prepend(imgs, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  sqlite3_finalize(stmt);
  return imgs;
}

// the stars, pick flags, labels and notes of a rating on its photos
static int _apply_set(const int kind, const char *request)
{
  const guint pick = _tag(DT_PICK_TAG);
  const guint tag[2] = { _tag(_ai_tag[0]), _tag(_ai_tag[1]) };
  GList *all = NULL, *picks = NULL, *rejects = NULL;
  GList *label_on[2] = { NULL }, *tag_on[2] = { NULL }, *tag_off[2] = { NULL };
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT imgid, stars, picked, matched, trash, note FROM main.ls_ratings"
                              " WHERE kind = ?1 AND request = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, kind);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, request, -1, SQLITE_TRANSIENT);

  dt_undo_start_group(darktable.undo, DT_UNDO_LIGHTTABLE);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const dt_imgid_t id = sqlite3_column_int(stmt, 0);
    const int stars = sqlite3_column_int(stmt, 1);
    const char *note = (const char *)sqlite3_column_text(stmt, 5);
    const int labels = dt_colorlabels_get_labels(id);
    all = g_list_prepend(all, GINT_TO_POINTER(id));

    if(stars == DT_VIEW_REJECT)
      rejects = g_list_prepend(rejects, GINT_TO_POINTER(id));
    else
      _set_stars(id, stars);
    if(sqlite3_column_int(stmt, 2)) picks = g_list_prepend(picks, GINT_TO_POINTER(id));

    // green and red: of the AI in a rating, the photo's own before AI
    for(int c = 0; c < 2; c++)
    {
      const gboolean want = sqlite3_column_int(stmt, 3 + c);
      const gboolean has = (labels & (1 << _ai_color[c])) != 0;
      const gboolean ai = dt_is_tag_attached(tag[c], id);
      if(kind == KIND_BEFORE)
      {
        if(ai) tag_off[c] = g_list_prepend(tag_off[c], GINT_TO_POINTER(id));
        if(want && !has) label_on[c] = g_list_prepend(label_on[c], GINT_TO_POINTER(id));
        else if(!want && has && ai) dt_colorlabels_remove_label(id, _ai_color[c]);
      }
      else if(want)
      {
        if(!has) label_on[c] = g_list_prepend(label_on[c], GINT_TO_POINTER(id));
        if(!ai) tag_on[c] = g_list_prepend(tag_on[c], GINT_TO_POINTER(id));
      }
      else if(ai)
      {
        tag_off[c] = g_list_prepend(tag_off[c], GINT_TO_POINTER(id));
        if(has) dt_colorlabels_remove_label(id, _ai_color[c]);
      }
    }
    dt_metadata_set(id, NOTES_KEY, note ? note : "", FALSE);
  }
  sqlite3_finalize(stmt);

  if(all) dt_tag_detach_images(pick, all, TRUE);
  if(picks) dt_tag_attach_images(pick, picks, TRUE);
  _reject(rejects);
  for(int c = 0; c < 2; c++)
  {
    if(label_on[c]) dt_colorlabels_set_labels(label_on[c], 1 << _ai_color[c], FALSE, TRUE);   // a mask
    if(tag_on[c]) dt_tag_attach_images(tag[c], tag_on[c], TRUE);
    if(tag_off[c]) dt_tag_detach_images(tag[c], tag_off[c], TRUE);
    g_list_free(label_on[c]);
    g_list_free(tag_on[c]);
    g_list_free(tag_off[c]);
  }
  dt_undo_end_group(darktable.undo);

  // clang-format off
  sqlite3_stmt *upd;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE main.ls_ratings"
                              " SET applied = (kind = ?1 AND request = ?2)"
                              " WHERE imgid IN (SELECT imgid FROM main.ls_ratings WHERE kind = ?1 AND request = ?2)",
                              -1, &upd, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(upd, 1, kind);
  DT_DEBUG_SQLITE3_BIND_TEXT(upd, 2, request, -1, SQLITE_TRANSIENT);
  sqlite3_step(upd);
  sqlite3_finalize(upd);

  dt_conf_set_int(CONF "active_kind", kind);
  dt_conf_set_string(CONF "active_request", request);

  const int n = g_list_length(all);
  dt_image_synch_xmps(all);
  g_list_free(all);
  g_list_free(picks);
  g_list_free(rejects);
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
  return n;
}

// the results of a run become a rating, and go on the photos
static void _store(const int kind, const char *request, _result_t *res, const int n)
{
  GList *imgs = NULL;
  for(int k = 0; k < n; k++) imgs = g_list_prepend(imgs, GINT_TO_POINTER(res[k].id));
  _save_current(imgs);
  g_list_free(imgs);

  sqlite3_stmt *ins;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT OR REPLACE INTO main.ls_ratings"
                              " (imgid, kind, request, score, reason, stars, picked, matched, trash, note,"
                              "  applied, time)"
                              " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?11, ?9, 0, ?10)",
                              -1, &ins, NULL);
  // clang-format on
  const gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  for(int k = 0; k < n; k++)
  {
    DT_DEBUG_SQLITE3_BIND_INT(ins, 1, res[k].id);
    DT_DEBUG_SQLITE3_BIND_INT(ins, 2, kind);
    DT_DEBUG_SQLITE3_BIND_TEXT(ins, 3, request, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_DOUBLE(ins, 4, res[k].score);
    DT_DEBUG_SQLITE3_BIND_TEXT(ins, 5, res[k].reason, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(ins, 6, res[k].stars);
    DT_DEBUG_SQLITE3_BIND_INT(ins, 7, res[k].picked);
    DT_DEBUG_SQLITE3_BIND_INT(ins, 8, res[k].matched);
    DT_DEBUG_SQLITE3_BIND_TEXT(ins, 9, res[k].note, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT64(ins, 10, now);
    DT_DEBUG_SQLITE3_BIND_INT(ins, 11, res[k].trash);
    sqlite3_step(ins);
    sqlite3_reset(ins);
    sqlite3_clear_bindings(ins);
  }
  sqlite3_finalize(ins);
  _apply_set(kind, request);
}

static void _results_free(_result_t *res, const int n)
{
  for(int k = 0; k < n; k++)
  {
    g_free(res[k].reason);
    g_free(res[k].note);
  }
  g_free(res);
}

static void _ratings_queue(dt_lib_module_t *self);

// a photo of a burst, with the other files of its shot (RAW+JPEG), in the
// group of the best photo of the burst (a stack, Lightroom's name)
static void _to_group(const dt_imgid_t group, const dt_imgid_t id)
{
  // not again in its own group: taking out a leader regroups the others
  const dt_image_t *img = dt_image_cache_get(id, 'r');
  if(!img) return;
  const gboolean in = img->group_id == group;
  dt_image_cache_read_release(img);
  if(!in) dt_grouping_add_to_group(group, id);
}

static void _stack(const dt_imgid_t best, const dt_imgid_t id, GHashTable *twins)
{
  const dt_image_t *img = dt_image_cache_get(best, 'r');
  if(!img) return;
  const dt_imgid_t group = img->group_id;
  dt_image_cache_read_release(img);
  _to_group(group, id);
  for(GList *l = twins ? g_hash_table_lookup(twins, GINT_TO_POINTER(id)) : NULL; l; l = g_list_next(l))
    _to_group(group, GPOINTER_TO_INT(l->data));
}

static void _apply_cull(_job_t *j)
{
  JsonArray *arr = json_object_get_array_member(j->result, "images");
  const int n = arr ? json_array_get_length(arr) : 0;
  // with the other files of each shot (RAW+JPEG), which get its result
  int total = n;
  for(int k = 0; k < n; k++)
  {
    const dt_imgid_t id = json_object_get_int_member(json_array_get_object_element(arr, k), "id");
    total += j->twins ? g_list_length(g_hash_table_lookup(j->twins, GINT_TO_POINTER(id))) : 0;
  }
  _result_t *res = g_new0(_result_t, MAX(1, total));
  int t = n;
  GHashTable *best_of = g_hash_table_new(g_direct_hash, g_direct_equal);
  GList *stacks = NULL;
  int bursts = 0, rejects = 0;

  // the best of each burst first
  for(int k = 0; k < n; k++)
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

  for(int k = 0; k < n; k++)
  {
    JsonObject *o = json_array_get_object_element(arr, k);
    _result_t *r = &res[k];
    r->id = json_object_get_int_member(o, "id");
    r->score = json_object_get_double_member(o, "score") * 10.0;
    r->stars = j->stars ? json_object_get_int_member(o, "stars") : dt_ratings_get(r->id);
    if(json_object_get_int_member(o, "burst_size") > 1)
    {
      const int burst = json_object_get_int_member(o, "burst");
      const dt_imgid_t best = GPOINTER_TO_INT(g_hash_table_lookup(best_of, GINT_TO_POINTER(burst + 1)));
      if(r->id == best)
        r->picked = TRUE;
      else if(j->reject)
        r->stars = DT_VIEW_REJECT;
      if(j->stack && dt_is_valid_imgid(best))
      {
        _stack(best, r->id, j->twins);
        if(r->id == best) stacks = g_list_prepend(stacks, GINT_TO_POINTER(best));
      }
    }
    rejects += r->stars == DT_VIEW_REJECT;
    const char *what = json_object_get_string_member_with_default(o, "note", "");
    r->reason = g_strdup(what);
    r->note = g_strdup_printf(_("Tonelark quality %d%%%s%s"), (int)(r->score * 10.0 + 0.5), *what ? ": " : "",
                              what);
    for(GList *l = j->twins ? g_hash_table_lookup(j->twins, GINT_TO_POINTER(r->id)) : NULL; l; l = g_list_next(l))
    {
      _result_t *c = &res[t++];
      *c = *r;
      c->id = GPOINTER_TO_INT(l->data);
      if(!j->stars) c->stars = dt_ratings_get(c->id);
      if(!c->picked && r->stars == DT_VIEW_REJECT) c->stars = DT_VIEW_REJECT;
      c->reason = g_strdup(r->reason);
      c->note = g_strdup(r->note);
      rejects += c->stars == DT_VIEW_REJECT;
    }
  }
  // the best photo on top of each stack
  for(GList *l = stacks; l; l = g_list_next(l)) dt_grouping_change_representative(GPOINTER_TO_INT(l->data));
  _store(KIND_CULL, "", res, t);
  _results_free(res, t);
  g_hash_table_destroy(best_of);

  // the stacks closed, to see only the best of each burst
  if(stacks && !darktable.gui->grouping)
    dt_action_process("global/grouping", 0, NULL, "on", 1.0f);
  const gboolean stacked = stacks != NULL;
  g_list_free(stacks);

  gchar *msg = g_strdup_printf(_("%d photos, %d bursts: the best of each is picked%s%s"), n, bursts,
                               rejects ? _(", the others rejected") : "",
                               stacked ? _(", each burst in a stack (the number on a photo opens it)") : "");
  _set_status(j->self, msg);
  dt_control_log("%s", msg);
  g_free(msg);
  _ratings_queue(j->self);
}

// "Codex 8.5/10 for “the best portrait”:" and the reason on a line of its own
// (a reason in Hebrew reads right to left, in the tooltips too)
static gchar *_rate_note(const char *who, const double score, const char *criteria, const char *reason)
{
  char num[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(num, sizeof(num), "%.1f", score);
  return criteria && *criteria ? g_strdup_printf("%s %s/10 for “%s”:\n%s", who, num, criteria, reason)
                               : g_strdup_printf("%s %s/10:\n%s", who, num, reason);
}

static void _apply_rate(_job_t *j)
{
  JsonArray *arr = json_object_get_array_member(j->result, "images");
  int n = arr ? json_array_get_length(arr) : 0;
  const char *who = json_object_get_string_member_with_default(j->result, "provider", "AI");
  const char *request = j->criteria ? j->criteria : "";
  int picks = 0, matches = 0, trash = 0;

  // with the other files of each shot rated (RAW+JPEG)
  int total = n;
  for(int k = 0; k < n; k++)
  {
    const dt_imgid_t id = json_object_get_int_member(json_array_get_object_element(arr, k), "id");
    total += j->twins ? g_list_length(g_hash_table_lookup(j->twins, GINT_TO_POINTER(id))) : 0;
  }
  _result_t *res = g_new0(_result_t, MAX(1, total));
  int t = n;

  for(int k = 0; k < n; k++)
  {
    JsonObject *o = json_array_get_object_element(arr, k);
    _result_t *r = &res[k];
    r->id = json_object_get_int_member(o, "id");
    r->score = json_object_get_double_member(o, "score");
    r->picked = json_object_get_boolean_member_with_default(o, "keep", FALSE);
    r->stars = j->stars ? CLAMP((int)(r->score / 2.0 + 0.5), 1, 5) : dt_ratings_get(r->id);
    if(!r->picked && j->reject) r->stars = DT_VIEW_REJECT;
    JsonNode *m = json_object_get_member(o, "match");
    r->matched = *request && m && JSON_NODE_HOLDS_VALUE(m) && json_node_get_boolean(m);
    JsonNode *del = json_object_get_member(o, "delete");
    r->trash = del && JSON_NODE_HOLDS_VALUE(del) && json_node_get_boolean(del);
    r->reason = g_strdup(json_object_get_string_member_with_default(o, "reason", ""));
    r->note = _rate_note(who, r->score, request, r->reason);
    picks += r->picked;
    matches += r->matched;
    trash += r->trash;
    for(GList *l = j->twins ? g_hash_table_lookup(j->twins, GINT_TO_POINTER(r->id)) : NULL; l; l = g_list_next(l))
    {
      _result_t *c = &res[t++];
      *c = *r;
      c->id = GPOINTER_TO_INT(l->data);
      if(!j->stars) c->stars = dt_ratings_get(c->id);
      if(!c->picked && j->reject) c->stars = DT_VIEW_REJECT;
      c->reason = g_strdup(r->reason);
      c->note = g_strdup(r->note);
      picks += c->picked;
      matches += c->matched;
      trash += c->trash;
    }
  }
  if(t) _store(KIND_AI, request, res, t);
  _results_free(res, t);
  n = t;

  JsonArray *errors = json_object_get_array_member(j->result, "errors");
  const int nerr = errors ? json_array_get_length(errors) : 0;
  gchar *msg;
  if(!n && nerr)
    msg = g_strdup_printf(_("the AI did not answer: %s"), json_array_get_string_element(errors, 0));
  else if(*request)
    msg = g_strdup_printf(_("%d photos rated for \"%s\": %d kept, %d match it (green label),"
                            " %d to delete (red label)"), n, request, picks, matches, trash);
  else
    msg = g_strdup_printf(_("%d photos rated, %d kept (pick flag), %d to delete (red label)"), n, picks, trash);
  _set_status(j->self, msg);
  dt_control_log("%s", msg);
  g_free(msg);
  _ratings_queue(j->self);
}

// ---------------------------------------------------------------------------
// the ratings of the collection, to choose one, and the list of its photos,
// best first, each with the reason of its score

typedef struct _row_t
{
  dt_imgid_t id;
  double score;
  int stars;
  gboolean trash;
  gchar *reason, *file;
} _row_t;

static void _row_free(gpointer data)
{
  _row_t *r = data;
  g_free(r->reason);
  g_free(r->file);
  g_free(r);
}

static gint _row_cmp(gconstpointer a, gconstpointer b)
{
  const _row_t *ra = a, *rb = b;
  if(ra->score != rb->score) return ra->score < rb->score ? 1 : -1;
  if(ra->stars != rb->stars) return ra->stars < rb->stars ? 1 : -1;
  return g_strcmp0(ra->file, rb->file);
}

static gchar *_set_name(const int kind, const char *request)
{
  if(kind == KIND_BEFORE) return g_strdup(_("my stars, before AI"));
  if(kind == KIND_CULL) return g_strdup(_("Find Best Shots"));
  return *request ? g_strdup_printf("“%s”", request) : g_strdup(_("quality, no request"));
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

static void _set_clicked(GtkButton *b, dt_lib_module_t *self)
{
  const int kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "kind"));
  const char *request = g_object_get_data(G_OBJECT(b), "request");
  // the photos keep what they have now with the rating on them
  GList *imgs = _set_images(kind, request);
  _save_current(imgs);
  g_list_free(imgs);
  const int n = _apply_set(kind, request);
  gchar *name = _set_name(kind, request);
  gchar *msg = g_strdup_printf(ngettext("%s: its stars, pick flags and labels are on %d photo",
                                        "%s: its stars, pick flags and labels are on %d photos", n),
                               name, n);
  _set_status(self, msg);
  dt_control_log("%s", msg);
  g_free(msg);
  g_free(name);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL);
  _ratings_queue(self);
}

static void _set_delete(GtkButton *b, dt_lib_module_t *self)
{
  const int kind = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "kind"));
  const char *request = g_object_get_data(G_OBJECT(b), "request");
  gchar *name = _set_name(kind, request);
  const gboolean yes = dt_gui_show_yes_no_dialog(_("delete the rating"), "",
                                                 _("delete the rating %s with its scores and reasons?\n"
                                                   "the photos keep the stars they have now"), name);
  g_free(name);
  if(!yes) return;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "DELETE FROM main.ls_ratings WHERE kind = ?1 AND request = ?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, kind);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, request, -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  _ratings_queue(self);
}

typedef struct _set_t
{
  int kind, count;
  gboolean active;
  gchar *request;
} _set_t;

static void _set_free(gpointer data)
{
  _set_t *t = data;
  g_free(t->request);
  g_free(t);
}

static void _ratings_update(dt_lib_module_t *self)
{
  dt_lib_aicull_t *d = self->data;
  if(!d || !d->ratings) return;

  // the ratings of the photos of the collection, the latest first, and theirs before AI
  // last; the ones on all their photos are highlighted, the one chosen last is listed
  const int last_kind = dt_conf_get_int(CONF "active_kind");
  gchar *last_request = dt_conf_get_string(CONF "active_request");
  int show_kind = -1, show_rank = 0;
  const char *show_request = NULL;
  gboolean shown_active = FALSE;
  GPtrArray *sets = g_ptr_array_new_with_free_func(_set_free);
  GString *sig = g_string_new("");
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT r.kind, r.request, COUNT(*), SUM(r.applied) FROM main.ls_ratings AS r"
                              " JOIN memory.collected_images AS c ON c.imgid = r.imgid"
                              " GROUP BY r.kind, r.request"
                              " ORDER BY r.kind = 1, MAX(r.time) DESC",
                              -1, &stmt, NULL);
  // clang-format on
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    _set_t *t = g_malloc0(sizeof(_set_t));
    t->kind = sqlite3_column_int(stmt, 0);
    t->request = g_strdup((const char *)sqlite3_column_text(stmt, 1));
    t->count = sqlite3_column_int(stmt, 2);
    t->active = sqlite3_column_int(stmt, 3) == t->count;
    g_ptr_array_add(sets, t);
    // listed: the one chosen last when it is on the photos, else the first on them, else the latest
    const int rank = t->active && t->kind == last_kind && !g_strcmp0(t->request, last_request) ? 3
                   : t->active ? 2 : 1;
    if(rank > show_rank)
    {
      show_rank = rank;
      show_kind = t->kind;
      show_request = t->request;
      shown_active = t->active;
    }
    g_string_append_printf(sig, "%d|%s|%d|%d\n", t->kind, t->request, t->count, t->active);
  }
  sqlite3_finalize(stmt);
  g_free(last_request);

  // the photos of the rating chosen (or the latest), best first
  GList *list = NULL;
  if(show_kind >= 0)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT r.imgid, r.score, r.reason, r.stars, r.applied, r.note, i.filename, r.trash"
                                " FROM main.ls_ratings AS r"
                                " JOIN memory.collected_images AS c ON c.imgid = r.imgid"
                                " JOIN main.images AS i ON i.id = r.imgid"
                                " WHERE r.kind = ?1 AND r.request = ?2",
                                -1, &stmt, NULL);
    // clang-format on
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, show_kind);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, show_request, -1, SQLITE_TRANSIENT);
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      _row_t *r = g_malloc0(sizeof(_row_t));
      r->id = sqlite3_column_int(stmt, 0);
      r->score = show_kind == KIND_BEFORE ? 0.0 : sqlite3_column_double(stmt, 1);
      // the stars on the photo when the rating is on it
      r->stars = sqlite3_column_int(stmt, 4) ? dt_ratings_get(r->id) : sqlite3_column_int(stmt, 3);
      const char *reason = (const char *)sqlite3_column_text(stmt, show_kind == KIND_BEFORE ? 5 : 2);
      r->reason = g_strdup(reason ? reason : "");
      r->file = g_strdup((const char *)sqlite3_column_text(stmt, 6));
      r->trash = show_kind != KIND_BEFORE && sqlite3_column_int(stmt, 7);
      list = g_list_prepend(list, r);
    }
    sqlite3_finalize(stmt);
  }
  list = g_list_sort(list, _row_cmp);
  g_string_append_printf(sig, "%d|%s|%d\n", show_kind, show_request ? show_request : "", shown_active);
  for(GList *l = list; l; l = g_list_next(l))
  {
    const _row_t *r = l->data;
    g_string_append_printf(sig, "%d|%.2f|%d|%d|%s\n", r->id, r->score, r->stars, r->trash, r->reason);
  }

  // the same as shown: nothing to do (a row rebuilt between the two clicks
  // of a double click would lose it)
  if(!g_strcmp0(sig->str, d->sig))
  {
    g_string_free(sig, TRUE);
    g_list_free_full(list, _row_free);
    g_ptr_array_free(sets, TRUE);
    return;
  }
  g_free(d->sig);
  d->sig = g_string_free(sig, FALSE);

  for(int c = 0; c < 2; c++)
  {
    GList *children = gtk_container_get_children(GTK_CONTAINER(c ? d->ratings : d->sets));
    for(GList *l = children; l; l = g_list_next(l)) gtk_widget_destroy(l->data);
    g_list_free(children);
  }

  for(guint k = 0; k < sets->len; k++)
  {
    const _set_t *t = g_ptr_array_index(sets, k);
    gchar *name = _set_name(t->kind, t->request);
    gchar *label = g_strdup_printf("%s  ·  %d", name, t->count);
    g_free(name);
    GtkWidget *b = gtk_button_new_with_label(label);
    g_free(label);
    gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(b))), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(gtk_bin_get_child(GTK_BIN(b))), 0.0f);
    gtk_widget_set_name(b, t->active ? "masking-row-selected" : "masking-row");
    gtk_widget_set_tooltip_text(b, _("put the stars, pick flags and labels of this rating on its photos (green:"
                                     " answers the request, red: to delete), and list them below. what you"
                                     " change meanwhile stays with it"));
    g_object_set_data(G_OBJECT(b), "kind", GINT_TO_POINTER(t->kind));
    g_object_set_data_full(G_OBJECT(b), "request", g_strdup(t->request), g_free);
    g_signal_connect(b, "clicked", G_CALLBACK(_set_clicked), self);
    GtkWidget *row = dt_gui_hbox(dt_gui_expand(b));
    if(t->kind != KIND_BEFORE)
    {
      GtkWidget *del = dtgtk_button_new(dtgtk_cairo_paint_remove, 0, NULL);
      gtk_widget_set_tooltip_text(del, _("delete this rating, with its scores and reasons"));
      g_object_set_data(G_OBJECT(del), "kind", GINT_TO_POINTER(t->kind));
      g_object_set_data_full(G_OBJECT(del), "request", g_strdup(t->request), g_free);
      g_signal_connect(del, "clicked", G_CALLBACK(_set_delete), self);
      dt_gui_box_add(row, del);
    }
    dt_gui_box_add(d->sets, row);
    gtk_widget_show_all(row);
  }

  const int n = g_list_length(list);
  gchar *name = show_kind >= 0 ? _set_name(show_kind, show_request) : NULL;
  gchar *head = !n ? g_strdup(_("no photo of this collection is rated yet"))
              : shown_active ? g_strdup_printf(ngettext("%s: %d photo, best first", "%s: %d photos, best first", n),
                                               name, n)
              : g_strdup_printf(ngettext("%s: %d photo, best first (click the rating to put its stars on the photos)",
                                         "%s: %d photos, best first (click the rating to put its stars on the photos)",
                                         n), name, n);
  gtk_label_set_text(GTK_LABEL(d->ratings_head), head);
  g_free(head);
  g_free(name);

  int k = 0;
  for(GList *l = list; l && k < RATINGS_MAX; l = g_list_next(l), k++)
  {
    _row_t *r = l->data;
    GString *st = g_string_new("");
    if(r->stars == DT_VIEW_REJECT)
      g_string_append(st, _("rejected"));
    else
      for(int s = 0; s < 5; s++) g_string_append(st, s < r->stars ? "★" : "☆");
    char num[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(num, sizeof(num), "%.1f", r->score);
    gchar *markup = show_kind == KIND_BEFORE
                    ? g_markup_printf_escaped("%s  %s", st->str, r->file ? r->file : "")
                    : g_markup_printf_escaped("<b>%s</b>  %s  %s%s<span foreground=\"#e2716a\">%s</span>", num,
                                              st->str, r->file ? r->file : "", r->trash ? "  " : "",
                                              r->trash ? _("to delete?") : "");
    g_string_free(st, TRUE);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_name(title, "aicull-rating-title");

    GtkWidget *box = dt_gui_vbox(title);
    if(*r->reason)
    {
      GtkWidget *why = gtk_label_new(r->reason);
      gtk_label_set_xalign(GTK_LABEL(why), 0.0f);
      gtk_label_set_line_wrap(GTK_LABEL(why), TRUE);
      gtk_label_set_line_wrap_mode(GTK_LABEL(why), PANGO_WRAP_WORD_CHAR);
      gtk_label_set_max_width_chars(GTK_LABEL(why), 1);
      gtk_widget_set_name(why, "aicull-rating-reason");
      dt_gui_box_add(box, why);
    }

    GtkWidget *row = gtk_event_box_new();
    gtk_container_add(GTK_CONTAINER(row), box);
    gtk_widget_set_name(row, "aicull-rating");
    gtk_widget_set_tooltip_text(row, _("click: select the photo\ndouble-click: open it in Develop"));
    gtk_widget_add_events(row, GDK_BUTTON_PRESS_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(row, "button-press-event", G_CALLBACK(_rating_pressed), GINT_TO_POINTER(r->id));
    g_signal_connect(row, "enter-notify-event", G_CALLBACK(_rating_hover), GINT_TO_POINTER(r->id));
    g_signal_connect(row, "leave-notify-event", G_CALLBACK(_rating_hover), GINT_TO_POINTER(r->id));
    gtk_box_pack_start(GTK_BOX(d->ratings), row, FALSE, FALSE, 0);
  }
  g_list_free_full(list, _row_free);
  gtk_widget_set_visible(d->sets, sets->len > 0);
  gtk_widget_set_visible(d->ratings_wrap, n > 0);
  gtk_widget_show_all(d->ratings);
  g_ptr_array_free(sets, TRUE);
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
    int kept = 0;
    JsonArray *faces = json_object_get_array_member(j->result, "faces");
    for(guint k = 0; faces && k < json_array_get_length(faces); k++)
      kept += *json_object_get_string_member_with_default(json_array_get_object_element(faces, k), "kept", "") != 0;
    gchar *done = n > 0
      ? g_strdup_printf(ngettext("Best Take: %d face replaced, the new photo is on top of the burst group",
                                 "Best Take: %d faces replaced, the new photo is on top of the burst group", n), n)
      : g_strdup(_("Best Take: the best photo already has everyone at their best, it is on top of the burst group"));
    gchar *msg = kept
      ? g_strdup_printf(ngettext("%s. %d better face was left out: the person moved between the photos",
                                 "%s. %d better faces were left out: the people moved between the photos", kept),
                        done, kept)
      : g_strdup(done);
    g_free(done);
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
    if(j->provider)
    {
      json_object_set_string_member(req, "provider", j->provider);
      json_object_set_string_member(req, "model", j->model ? j->model : "");
    }

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
    GList *all = j->imgs;
    if(j->task == TASK_RATE || j->task == TASK_CULL)
    {
      // one photo for each shot: RAW+JPEG made a "burst" of two in the cull
      j->twins = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, _twins_free);
      j->imgs = _one_per_shot(all, j->twins);
    }
    JsonArray *images = _previews(j, job, dir, j->task == TASK_CULL ? 1600 : 1000);
    if(j->imgs != all)
    {
      g_list_free(j->imgs);
      j->imgs = all;
    }
    if(j->task == TASK_RATE)
    {
      json_object_set_array_member(req, "bursts", _time_bursts(images));
      json_object_set_string_member(req, "provider", j->provider);
      json_object_set_string_member(req, "model", j->model ? j->model : "");
      json_object_set_string_member(req, "criteria", j->criteria ? j->criteria : "");
      json_object_set_string_member(req, "language", j->language ? j->language : "English");
      json_object_set_int_member(req, "per_sheet", 6);
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

  if(task == TASK_BESTTAKE)
  {
    // the AI chooses the best faces when one is connected, else the measures
    gchar *why = NULL;
    if(!dt_lsai_provider_ui_get(d->provider, &j->provider, &j->model, &why))
    {
      g_free(j->provider);
      g_free(j->model);
      j->provider = j->model = NULL;
    }
    g_free(why);
  }
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
  _db_init();

  GtkWidget *cull = dt_action_button_new(self, N_("Find Best Shots"), _cull_clicked, self,
                                         _("on this computer, no AI service: sharpness, exposure, eyes"
                                           " open and smiles, bursts and the best of each"), 0, 0);
  d->reject = _check(_("reject the rest"), _("reject the other photos of each burst, or what the AI would"
                                             " not keep"), "plugins/lighttable/aicull/reject", FALSE);
  d->stars = _check(_("set stars"), _("rate the photos from the quality (1 to 5 stars)"),
                    "plugins/lighttable/aicull/stars", TRUE);
  // stacks on by default, once for a config that had them off
  if(!dt_conf_key_exists("lightspeed/stack_bursts"))
  {
    dt_conf_set_bool("plugins/lighttable/aicull/stack", TRUE);
    dt_conf_set_bool("lightspeed/stack_bursts", TRUE);
  }
  d->stack = _check(_("stack bursts"), _("each burst in a stack, its best photo on top: the number on the photo"
                                         " opens and closes the stack"),
                    "plugins/lighttable/aicull/stack", TRUE);

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
  d->sets = dt_gui_vbox();
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
    d->sets, d->ratings_head, d->ratings_wrap);
  gtk_widget_show_all(self->widget);
  gtk_widget_set_no_show_all(d->ratings_wrap, TRUE);
  gtk_widget_set_no_show_all(d->sets, TRUE);

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
  g_free(d->sig);
  dt_lsai_provider_ui_free(d->provider);
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
