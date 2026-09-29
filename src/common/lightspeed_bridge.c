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

#include "common/lightspeed_bridge.h"
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/datetime.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/lightspeed_ai.h"
#include "common/lightspeed_version.h"
#include "common/metadata.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "common/tags.h"
#include "common/undo.h"
#include "common/utility.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "gui/accelerators.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_module.h"
#include "views/view.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#define PICK_TAG "darktable|pick"
#define NOTES_KEY "Xmp.acdsee.notes"

static gchar *_dir = NULL, *_in = NULL, *_out = NULL;
static guint _timer = 0;
static gint64 _beat = 0;

// ---------------------------------------------------------------------------
// files

static void _write_atomic(const char *path, const gchar *text)
{
  gchar *tmp = g_strconcat(path, ".tmp", NULL);
  if(g_file_set_contents(tmp, text, -1, NULL))
  {
    g_unlink(path);
    if(g_rename(tmp, path) != 0) g_unlink(tmp);
  }
  g_free(tmp);
}

static void _write_object(const char *path, JsonObject *o)
{
  JsonNode *root = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(root, o);
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, root);
  gchar *text = json_generator_to_data(gen, NULL);
  _write_atomic(path, text);
  g_free(text);
  g_object_unref(gen);
  json_node_unref(root);
}

// the answer of a request: result is taken over
static void _reply(const char *id, JsonObject *result, const char *error)
{
  JsonObject *o = json_object_new();
  json_object_set_boolean_member(o, "ok", error == NULL);
  if(error) json_object_set_string_member(o, "error", error);
  if(result) json_object_set_object_member(o, "result", result);
  gchar *name = g_strdup_printf("%s.json", id);
  gchar *path = g_build_filename(_out, name, NULL);
  _write_object(path, o);
  json_object_unref(o);
  g_free(name);
  g_free(path);
}

static void _heartbeat(void)
{
  JsonObject *o = json_object_new();
#ifdef _WIN32
  json_object_set_int_member(o, "pid", GetCurrentProcessId());
#else
  json_object_set_int_member(o, "pid", getpid());
#endif
  json_object_set_string_member(o, "version", LIGHTSPEED_VERSION);
  json_object_set_int_member(o, "time", g_get_real_time() / G_USEC_PER_SEC);
  json_object_set_string_member(o, "configdir", darktable.configdir);
  gchar *path = g_build_filename(_dir, "tonelark.json", NULL);
  _write_object(path, o);
  json_object_unref(o);
  g_free(path);
  _beat = g_get_monotonic_time();
}

// ---------------------------------------------------------------------------
// arguments

static GList *_ids(JsonObject *args)
{
  GList *ids = NULL;
  JsonArray *arr = json_object_has_member(args, "ids") ? json_object_get_array_member(args, "ids") : NULL;
  for(guint k = 0; arr && k < json_array_get_length(arr); k++)
  {
    const dt_imgid_t id = json_array_get_int_element(arr, k);
    const dt_image_t *img = dt_image_cache_get(id, 'r');
    if(img) ids = g_list_prepend(ids, GINT_TO_POINTER(id));
    dt_image_cache_read_release(img);
  }
  if(json_object_has_member(args, "id"))
  {
    const dt_imgid_t id = json_object_get_int_member(args, "id");
    const dt_image_t *img = dt_image_cache_get(id, 'r');
    if(img) ids = g_list_prepend(ids, GINT_TO_POINTER(id));
    dt_image_cache_read_release(img);
  }
  return g_list_reverse(ids);
}

static const char *_str(JsonObject *args, const char *key)
{
  return json_object_has_member(args, key) && JSON_NODE_HOLDS_VALUE(json_object_get_member(args, key))
    ? json_object_get_string_member(args, key) : NULL;
}

static GList *_strings(JsonObject *args, const char *key)
{
  GList *list = NULL;
  JsonArray *arr = json_object_has_member(args, key) ? json_object_get_array_member(args, key) : NULL;
  for(guint k = 0; arr && k < json_array_get_length(arr); k++)
  {
    const char *s = json_array_get_string_element(arr, k);
    if(s && *s) list = g_list_append(list, g_strdup(s));
  }
  return list;
}

// ---------------------------------------------------------------------------
// photos

static gboolean _picked(const dt_imgid_t id)
{
  guint tagid = 0;
  if(!dt_tag_exists(PICK_TAG, &tagid)) return FALSE;
  GList *tags = NULL;
  gboolean found = FALSE;
  dt_tag_get_attached(id, &tags, FALSE);
  for(GList *l = tags; l; l = g_list_next(l))
    if(((dt_tag_t *)l->data)->id == tagid) found = TRUE;
  dt_tag_free_result(&tags);
  return found;
}

static gchar *_meta(const dt_imgid_t id, const char *key)
{
  GList *res = dt_metadata_get(id, key, NULL);
  gchar *v = res ? g_strdup(res->data) : NULL;
  g_list_free_full(res, g_free);
  return v;
}

static JsonObject *_photo(const dt_imgid_t id)
{
  const dt_image_t *img = dt_image_cache_get(id, 'r');
  if(!img) return NULL;

  JsonObject *o = json_object_new();
  json_object_set_int_member(o, "id", id);
  json_object_set_string_member(o, "file", img->filename);
  char folder[PATH_MAX] = { 0 };
  dt_image_film_roll_directory(img, folder, sizeof(folder));
  json_object_set_string_member(o, "folder", folder);
  const gboolean rejected = (img->flags & DT_IMAGE_REJECTED) != 0;
  json_object_set_int_member(o, "stars", rejected ? 0 : (img->flags & DT_VIEW_RATINGS_MASK));
  json_object_set_boolean_member(o, "rejected", rejected);
  char taken[DT_DATETIME_LENGTH] = { 0 };
  if(img->exif_datetime_taken > 0 && dt_datetime_gtimespan_to_exif(taken, sizeof(taken), img->exif_datetime_taken))
  {
    // 2024:09:09 20:13:07 -> 2024-09-09 20:13:07
    if(strlen(taken) > 7) taken[4] = taken[7] = '-';
    taken[MIN(19, sizeof(taken) - 1)] = 0;
    json_object_set_string_member(o, "taken", taken);
  }
  gchar *camera = g_strdup_printf("%s %s", img->camera_maker, img->camera_model);
  json_object_set_string_member(o, "camera", g_strstrip(camera));
  g_free(camera);
  if(*img->exif_lens) json_object_set_string_member(o, "lens", img->exif_lens);
  if(img->exif_iso > 0) json_object_set_int_member(o, "iso", (int)img->exif_iso);
  if(img->exif_aperture > 0) {
    gchar *f = g_strdup_printf("f/%.1f", img->exif_aperture);
    json_object_set_string_member(o, "aperture", f);
    g_free(f);
  }
  if(img->exif_exposure > 0)
  {
    gchar *s = dt_util_format_exposure(img->exif_exposure);
    json_object_set_string_member(o, "shutter", s);
    g_free(s);
  }
  if(img->exif_focal_length > 0) json_object_set_int_member(o, "focal_length", (int)img->exif_focal_length);
  json_object_set_int_member(o, "width", img->final_width > 0 ? img->final_width : img->width);
  json_object_set_int_member(o, "height", img->final_height > 0 ? img->final_height : img->height);
  const int group = img->group_id;
  dt_image_cache_read_release(img);

  if(group != id) json_object_set_int_member(o, "group_leader", group);
  json_object_set_boolean_member(o, "picked", _picked(id));
  json_object_set_boolean_member(o, "edited", dt_image_altered(id));

  JsonArray *labels = json_array_new();
  const int bits = dt_colorlabels_get_labels(id);
  for(int c = 0; c < 5; c++)
    if(bits & (1 << c)) json_array_add_string_element(labels, dt_colorlabels_to_string(c));
  json_object_set_array_member(o, "labels", labels);

  JsonArray *tags = json_array_new();
  GList *tl = dt_tag_get_hierarchical(id, TRUE);
  for(GList *l = tl; l; l = g_list_next(l)) json_array_add_string_element(tags, l->data);
  g_list_free_full(tl, g_free);
  json_object_set_array_member(o, "keywords", tags);

  static const struct { const char *key, *name; } meta[] =
    { { "Xmp.dc.title", "title" }, { "Xmp.dc.description", "caption" }, { NOTES_KEY, "notes" } };
  for(int k = 0; k < G_N_ELEMENTS(meta); k++)
  {
    gchar *v = _meta(id, meta[k].key);
    if(v && *v) json_object_set_string_member(o, meta[k].name, v);
    g_free(v);
  }
  return o;
}

// all the words of the search in the file name, folder, keywords, title, caption or notes
static gboolean _matches(JsonObject *p, gchar **words)
{
  if(!words || !*words) return TRUE;
  GString *hay = g_string_new(NULL);
  static const char *fields[] = { "file", "folder", "title", "caption", "notes", "camera", "lens", "taken" };
  for(int k = 0; k < G_N_ELEMENTS(fields); k++)
    if(json_object_has_member(p, fields[k]))
      g_string_append_printf(hay, " %s", json_object_get_string_member(p, fields[k]));
  JsonArray *tags = json_object_get_array_member(p, "keywords");
  for(guint k = 0; tags && k < json_array_get_length(tags); k++)
    g_string_append_printf(hay, " %s", json_array_get_string_element(tags, k));
  gchar *low = g_utf8_casefold(hay->str, -1);
  g_string_free(hay, TRUE);
  gboolean ok = TRUE;
  for(gchar **w = words; *w && ok; w++)
    if(**w && !strstr(low, *w)) ok = FALSE;
  g_free(low);
  return ok;
}

static JsonObject *_cmd_list(JsonObject *args, gchar **error)
{
  const char *scope = _str(args, "scope");
  GList *ids = NULL;
  if(json_object_has_member(args, "ids"))
    ids = _ids(args);
  else if(!g_strcmp0(scope, "selected"))
    ids = dt_selection_get_list(darktable.selection, FALSE, TRUE);
  else if(!g_strcmp0(scope, "all"))
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT id FROM main.images ORDER BY film_id, filename", -1, &stmt, NULL);
    while(sqlite3_step(stmt) == SQLITE_ROW) ids = g_list_prepend(ids, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
    sqlite3_finalize(stmt);
    ids = g_list_reverse(ids);
  }
  else
    ids = dt_collection_get_all(darktable.collection, -1);

  const char *search = _str(args, "search");
  gchar *low = search ? g_utf8_casefold(search, -1) : NULL;
  gchar **words = low ? g_strsplit_set(low, " \t", -1) : NULL;
  g_free(low);
  const int min_stars = json_object_get_int_member_with_default(args, "min_stars", 0);
  const char *label = _str(args, "label");
  const char *flag = _str(args, "flag");
  const int edited = json_object_has_member(args, "edited")
    ? (json_object_get_boolean_member(args, "edited") ? 1 : 0) : -1;
  const int limit = CLAMP(json_object_get_int_member_with_default(args, "limit", 50), 1, 500);
  const int offset = MAX(0, json_object_get_int_member_with_default(args, "offset", 0));

  JsonArray *photos = json_array_new();
  int total = 0;
  for(GList *l = ids; l; l = g_list_next(l))
  {
    JsonObject *p = _photo(GPOINTER_TO_INT(l->data));
    if(!p) continue;
    gboolean ok = _matches(p, words) && json_object_get_int_member(p, "stars") >= min_stars;
    if(ok && label)
    {
      gboolean has = FALSE;
      JsonArray *labels = json_object_get_array_member(p, "labels");
      for(guint k = 0; k < json_array_get_length(labels); k++)
        if(!g_ascii_strcasecmp(json_array_get_string_element(labels, k), label)) has = TRUE;
      ok = has;
    }
    if(ok && flag)
    {
      const gboolean picked = json_object_get_boolean_member(p, "picked");
      const gboolean rejected = json_object_get_boolean_member(p, "rejected");
      ok = !g_strcmp0(flag, "picked") ? picked
         : !g_strcmp0(flag, "rejected") ? rejected
         : !g_strcmp0(flag, "unflagged") ? (!picked && !rejected) : TRUE;
    }
    if(ok && edited >= 0) ok = json_object_get_boolean_member(p, "edited") == edited;
    if(ok)
    {
      if(total >= offset && total < offset + limit)
        json_array_add_object_element(photos, p);
      else
        json_object_unref(p);
      total++;
    }
    else
      json_object_unref(p);
  }
  g_list_free(ids);
  g_strfreev(words);

  JsonObject *o = json_object_new();
  json_object_set_int_member(o, "total", total);
  json_object_set_int_member(o, "offset", offset);
  json_object_set_array_member(o, "photos", photos);
  return o;
}

static JsonObject *_cmd_status(void)
{
  JsonObject *o = json_object_new();
  json_object_set_string_member(o, "version", LIGHTSPEED_VERSION);
  const dt_view_t *view = dt_view_manager_get_current_view(darktable.view_manager);
  json_object_set_string_member(o, "view", view ? view->module_name : "");
  if(dt_view_get_current() == DT_VIEW_DARKROOM && darktable.develop)
    json_object_set_int_member(o, "open_photo", darktable.develop->image_storage.id);
  json_object_set_int_member(o, "collection_count", dt_collection_get_count(darktable.collection));
  GList *sel = dt_selection_get_list(darktable.selection, FALSE, TRUE);
  JsonArray *arr = json_array_new();
  int n = 0;
  for(GList *l = sel; l; l = g_list_next(l), n++)
    if(n < 200) json_array_add_int_element(arr, GPOINTER_TO_INT(l->data));
  json_object_set_int_member(o, "selected_count", n);
  json_object_set_array_member(o, "selected", arr);
  g_list_free(sel);
  return o;
}

// ---------------------------------------------------------------------------
// changes (gui thread)

static void _changed(GList *ids)
{
  dt_image_synch_xmps(ids);
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                             DT_COLLECTION_PROP_UNDEF, NULL);
}

static JsonObject *_done(const int n)
{
  JsonObject *o = json_object_new();
  json_object_set_int_member(o, "changed", n);
  return o;
}

static JsonObject *_cmd_rate(JsonObject *args, gchar **error)
{
  GList *ids = _ids(args);
  const int stars = CLAMP(json_object_get_int_member_with_default(args, "stars", 0), 0, 5);
  int n = 0;
  dt_undo_start_group(darktable.undo, DT_UNDO_RATINGS);
  for(GList *l = ids; l; l = g_list_next(l), n++)
  {
    const dt_imgid_t id = GPOINTER_TO_INT(l->data);
    if(dt_ratings_get(id) != stars) dt_ratings_apply_on_image(id, stars, FALSE, TRUE, FALSE);
  }
  dt_undo_end_group(darktable.undo);
  _changed(ids);
  g_list_free(ids);
  return _done(n);
}

static JsonObject *_cmd_flag(JsonObject *args, gchar **error)
{
  GList *ids = _ids(args);
  const char *flag = _str(args, "flag");
  if(!flag || (g_strcmp0(flag, "pick") && g_strcmp0(flag, "reject") && g_strcmp0(flag, "none")))
  {
    *error = g_strdup("flag must be pick, reject or none");
    g_list_free(ids);
    return NULL;
  }
  guint tagid = 0;
  dt_tag_new(PICK_TAG, &tagid);
  dt_undo_start_group(darktable.undo, DT_UNDO_LIGHTTABLE);
  dt_tag_detach_images(tagid, ids, TRUE);
  for(GList *l = ids; l; l = g_list_next(l))
  {
    const dt_imgid_t id = GPOINTER_TO_INT(l->data);
    const gboolean rejected = dt_ratings_get(id) == DT_VIEW_REJECT;
    // applying the reject rating on a rejected photo takes the flag away
    if(rejected != !g_strcmp0(flag, "reject"))
      dt_ratings_apply_on_image(id, DT_VIEW_REJECT, FALSE, TRUE, FALSE);
  }
  if(!g_strcmp0(flag, "pick")) dt_tag_attach_images(tagid, ids, TRUE);
  dt_undo_end_group(darktable.undo);
  _changed(ids);
  const int n = g_list_length(ids);
  g_list_free(ids);
  return _done(n);
}

static int _label_index(const char *name)
{
  static const char *names[] = { "red", "yellow", "green", "blue", "purple" };
  for(int c = 0; c < 5; c++)
    if(!g_ascii_strcasecmp(name, names[c])) return c;
  return -1;
}

static JsonObject *_cmd_label(JsonObject *args, gchar **error)
{
  GList *ids = _ids(args);
  GList *labels = _strings(args, "labels");
  const char *mode = _str(args, "mode");
  if(!mode) mode = "set";
  int bits = 0;
  for(GList *l = labels; l; l = g_list_next(l))
  {
    const int c = _label_index(l->data);
    if(c < 0)
    {
      *error = g_strdup_printf("unknown color label '%s' (red, yellow, green, blue, purple)", (char *)l->data);
      g_list_free_full(labels, g_free);
      g_list_free(ids);
      return NULL;
    }
    bits |= 1 << c;
  }
  g_list_free_full(labels, g_free);
  for(GList *l = ids; l; l = g_list_next(l))
  {
    const dt_imgid_t id = GPOINTER_TO_INT(l->data);
    const int now = dt_colorlabels_get_labels(id);
    const int want = !g_strcmp0(mode, "add") ? (now | bits) : !g_strcmp0(mode, "remove") ? (now & ~bits) : bits;
    for(int c = 0; c < 5; c++)
    {
      if((want & (1 << c)) && !(now & (1 << c))) dt_colorlabels_set_label(id, c);
      if(!(want & (1 << c)) && (now & (1 << c))) dt_colorlabels_remove_label(id, c);
    }
  }
  _changed(ids);
  const int n = g_list_length(ids);
  g_list_free(ids);
  return _done(n);
}

static JsonObject *_cmd_tag(JsonObject *args, gchar **error)
{
  GList *ids = _ids(args);
  GList *add = _strings(args, "add"), *remove = _strings(args, "remove");
  dt_undo_start_group(darktable.undo, DT_UNDO_TAGS);
  for(GList *l = add; l; l = g_list_next(l))
  {
    guint tagid = 0;
    if(g_str_has_prefix(l->data, "darktable|")) continue;
    dt_tag_new(l->data, &tagid);
    if(tagid) dt_tag_attach_images(tagid, ids, TRUE);
  }
  for(GList *l = remove; l; l = g_list_next(l))
  {
    guint tagid = 0;
    if(dt_tag_exists(l->data, &tagid)) dt_tag_detach_images(tagid, ids, TRUE);
  }
  dt_undo_end_group(darktable.undo);
  _changed(ids);
  const int n = g_list_length(ids);
  g_list_free_full(add, g_free);
  g_list_free_full(remove, g_free);
  g_list_free(ids);
  return _done(n);
}

static JsonObject *_cmd_describe(JsonObject *args, gchar **error)
{
  GList *ids = _ids(args);
  static const struct { const char *arg, *key; } meta[] =
    { { "title", "Xmp.dc.title" }, { "caption", "Xmp.dc.description" }, { "notes", NOTES_KEY } };
  dt_undo_start_group(darktable.undo, DT_UNDO_METADATA);
  for(GList *l = ids; l; l = g_list_next(l))
    for(int k = 0; k < G_N_ELEMENTS(meta); k++)
    {
      const char *v = _str(args, meta[k].arg);
      if(v) dt_metadata_set(GPOINTER_TO_INT(l->data), meta[k].key, v, TRUE);
    }
  dt_undo_end_group(darktable.undo);
  _changed(ids);
  const int n = g_list_length(ids);
  g_list_free(ids);
  return _done(n);
}

static JsonObject *_cmd_select(JsonObject *args, gchar **error)
{
  GList *ids = _ids(args);
  dt_selection_clear(darktable.selection);
  if(ids) dt_selection_select_list(darktable.selection, ids);
  const int n = g_list_length(ids);
  g_list_free(ids);
  return _done(n);
}

static JsonObject *_cmd_open(JsonObject *args, gchar **error)
{
  GList *ids = _ids(args);
  if(!ids)
  {
    *error = g_strdup("no such photo");
    return NULL;
  }
  const dt_imgid_t id = GPOINTER_TO_INT(ids->data);
  if(dt_view_get_current() == DT_VIEW_DARKROOM)
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, id);
  else
  {
    // as a double click in the library: the darkroom opens the photo under the mouse
    dt_selection_select_single(darktable.selection, id);
    dt_control_set_mouse_over_id(id);
    dt_view_manager_switch(darktable.view_manager, "darkroom");
  }
  g_list_free(ids);
  return _done(1);
}

// a button of an AI panel, on the given photos
static JsonObject *_cmd_action(JsonObject *args, gchar **error)
{
  const char *path = _str(args, "action");
  static const char *allowed[] = { "lib/aicull/", "lib/aiassist/", NULL };
  gboolean ok = FALSE;
  for(int k = 0; path && allowed[k]; k++)
    if(g_str_has_prefix(path, allowed[k])) ok = TRUE;
  if(!ok)
  {
    *error = g_strdup("unknown tool");
    return NULL;
  }
  if(json_object_has_member(args, "ids"))
  {
    GList *ids = _ids(args);
    // the tools work on the selection as the library shows it: photos out
    // of the current collection would be left out without a word
    GList *shown = dt_collection_get_all(darktable.collection, -1);
    int missing = 0;
    for(GList *l = ids; l; l = g_list_next(l))
      if(!g_list_find(shown, l->data)) missing++;
    g_list_free(shown);
    if(missing)
    {
      *error = g_strdup_printf("%d of the photos are not in the collection shown in Tonelark: show their"
                               " folder (Collections) and try again", missing);
      g_list_free(ids);
      return NULL;
    }
    dt_selection_clear(darktable.selection);
    if(ids) dt_selection_select_list(darktable.selection, ids);
    g_list_free(ids);
    // the tools act on the photo under the mouse before the selection: not
    // on the one the user happens to point at
    dt_control_set_mouse_over_id(NO_IMGID);
  }
  // the look of Auto Edit, or what Rate with AI looks for
  const char *text = _str(args, "text");
  if(text)
    dt_conf_set_string(g_str_has_prefix(path, "lib/aicull/") ? "plugins/lighttable/aicull/criteria"
                                                              : "plugins/lightspeed/ai/instruction", text);
  dt_action_process(path, 0, NULL, NULL, 1.0f);
  JsonObject *o = json_object_new();
  json_object_set_string_member(o, "started", path);
  return o;
}

// ---------------------------------------------------------------------------
// jobs: what renders or loads images

typedef struct _req_t
{
  gchar *id, *cmd;
  JsonObject *args;
} _req_t;

static void _req_free(void *data)
{
  _req_t *r = data;
  g_free(r->id);
  g_free(r->cmd);
  json_object_unref(r->args);
  g_free(r);
}

static gboolean _export_jpeg(const dt_imgid_t id, const char *path, const int size, const int quality)
{
  dt_imageio_module_format_t *format = dt_imageio_get_format_by_name("jpeg");
  if(!format) return FALSE;
  const char *key = "plugins/imageio/format/jpeg/quality";
  const int saved = dt_conf_get_int(key);
  dt_conf_set_int(key, CLAMP(quality, 50, 100));
  dt_imageio_module_data_t *fdata = format->get_params(format);
  dt_conf_set_int(key, saved);
  if(!fdata) return FALSE;
  fdata->max_width = fdata->max_height = MAX(0, size);
  fdata->width = fdata->height = 0;
  fdata->style[0] = '\0';
  const gboolean failed = dt_imageio_export(id, path, format, fdata, TRUE, FALSE, TRUE, 1.0,
                                            TRUE, FALSE, DT_COLORSPACE_SRGB, NULL, DT_INTENT_PERCEPTUAL,
                                            NULL, NULL, 1, 1, NULL);
  format->free_params(format, fdata);
  return !failed && g_file_test(path, G_FILE_TEST_EXISTS);
}

static JsonObject *_cmd_export(_req_t *r, gchar **error)
{
  GList *ids = _ids(r->args);
  const char *folder = _str(r->args, "folder");
  if(!folder || !*folder || !g_path_is_absolute(folder))
  {
    *error = g_strdup("give an absolute folder");
    g_list_free(ids);
    return NULL;
  }
  g_mkdir_with_parents(folder, 0755);
  const int size = json_object_get_int_member_with_default(r->args, "max_size", 0);
  const int quality = json_object_get_int_member_with_default(r->args, "quality", 92);
  JsonArray *files = json_array_new();
  for(GList *l = ids; l; l = g_list_next(l))
  {
    const dt_imgid_t id = GPOINTER_TO_INT(l->data);
    const dt_image_t *img = dt_image_cache_get(id, 'r');
    gchar *base = g_strdup(img ? img->filename : "photo");
    dt_image_cache_read_release(img);
    char *dot = strrchr(base, '.');
    if(dot) *dot = '\0';
    gchar *name = g_strdup_printf("%s.jpg", base);
    gchar *path = g_build_filename(folder, name, NULL);
    for(int v = 2; g_file_test(path, G_FILE_TEST_EXISTS); v++)
    {
      g_free(name);
      g_free(path);
      name = g_strdup_printf("%s_%d.jpg", base, v);
      path = g_build_filename(folder, name, NULL);
    }
    if(_export_jpeg(id, path, size, quality)) json_array_add_string_element(files, path);
    g_free(base);
    g_free(name);
    g_free(path);
  }
  g_list_free(ids);
  JsonObject *o = json_object_new();
  json_object_set_array_member(o, "files", files);
  return o;
}

static int32_t _job_run(dt_job_t *job)
{
  _req_t *r = dt_control_job_get_params(job);
  JsonObject *res = NULL;
  gchar *error = NULL;

  if(!g_strcmp0(r->cmd, "photo"))
  {
    GList *ids = _ids(r->args);
    if(ids)
    {
      res = _photo(GPOINTER_TO_INT(ids->data));
      json_object_set_object_member(res, "edit", dt_lsai_read_edit(GPOINTER_TO_INT(ids->data)));
    }
    else
      error = g_strdup("no such photo");
    g_list_free(ids);
  }
  else if(!g_strcmp0(r->cmd, "preview"))
  {
    GList *ids = _ids(r->args);
    const int size = CLAMP(json_object_get_int_member_with_default(r->args, "size", 1024), 256, 2560);
    JsonArray *files = json_array_new();
    for(GList *l = ids; l; l = g_list_next(l))
    {
      gchar *name = g_strdup_printf("%s_%d.jpg", r->id, GPOINTER_TO_INT(l->data));
      gchar *path = g_build_filename(_out, name, NULL);
      JsonObject *f = json_object_new();
      json_object_set_int_member(f, "id", GPOINTER_TO_INT(l->data));
      if(dt_lsai_write_preview(GPOINTER_TO_INT(l->data), size, path))
        json_object_set_string_member(f, "path", path);
      json_array_add_object_element(files, f);
      g_free(name);
      g_free(path);
    }
    g_list_free(ids);
    res = json_object_new();
    json_object_set_array_member(res, "previews", files);
  }
  else if(!g_strcmp0(r->cmd, "edit"))
  {
    GList *ids = _ids(r->args);
    JsonObject *edit = json_object_has_member(r->args, "edit")
      ? json_object_get_object_member(r->args, "edit") : NULL;
    int n = 0;
    if(!edit)
      error = g_strdup("no settings");
    else
    {
      dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
      for(GList *l = ids; l; l = g_list_next(l))
        if(dt_lsai_apply_edit(GPOINTER_TO_INT(l->data), edit)) n++;
      dt_undo_end_group(darktable.undo);
      res = _done(n);
    }
    g_list_free(ids);
  }
  else if(!g_strcmp0(r->cmd, "copy_edit"))
  {
    const dt_imgid_t src = json_object_get_int_member_with_default(r->args, "from", NO_IMGID);
    GList *ids = _ids(r->args);
    int n = 0;
    for(GList *l = ids; l; l = g_list_next(l))
      if(dt_lsai_copy_edit(src, GPOINTER_TO_INT(l->data))) n++;
    g_list_free(ids);
    res = _done(n);
  }
  else if(!g_strcmp0(r->cmd, "export"))
    res = _cmd_export(r, &error);
  else
    error = g_strdup_printf("unknown command '%s'", r->cmd);

  _reply(r->id, res, error);
  g_free(error);
  return 0;
}

// ---------------------------------------------------------------------------
// requests

static void _dispatch(const char *id, const char *cmd, JsonObject *args)
{
  typedef JsonObject *(*handler_t)(JsonObject *, gchar **);
  static const struct { const char *cmd; handler_t f; } gui[] =
  {
    { "list", _cmd_list }, { "rate", _cmd_rate }, { "flag", _cmd_flag }, { "label", _cmd_label }, { "tag", _cmd_tag },
    { "describe", _cmd_describe }, { "select", _cmd_select }, { "open", _cmd_open }, { "action", _cmd_action },
  };

  if(!g_strcmp0(cmd, "status"))
  {
    _reply(id, _cmd_status(), NULL);
    return;
  }
  for(int k = 0; k < G_N_ELEMENTS(gui); k++)
    if(!g_strcmp0(cmd, gui[k].cmd))
    {
      gchar *error = NULL;
      JsonObject *res = gui[k].f(args, &error);
      _reply(id, res, error ? error : (res ? NULL : "failed"));
      g_free(error);
      return;
    }

  // the others load or render images: in a job, which reads the database.
  // The history of the photo open in the darkroom goes there at once, not
  // at the next autosave
  if(dt_view_get_current() == DT_VIEW_DARKROOM && darktable.develop && darktable.develop->gui_attached)
    dt_dev_write_history(darktable.develop);

  _req_t *r = g_malloc0(sizeof(_req_t));
  r->id = g_strdup(id);
  r->cmd = g_strdup(cmd);
  r->args = json_object_ref(args);
  dt_job_t *job = dt_control_job_create(_job_run, "chat %s", cmd);
  if(!job)
  {
    _reply(id, NULL, "cannot start the job");
    _req_free(r);
    return;
  }
  dt_control_job_set_params(job, r, _req_free);
  dt_control_add_job(DT_JOB_QUEUE_USER_BG, job);
}

static gboolean _poll(gpointer data)
{
  if(g_get_monotonic_time() - _beat > 5 * G_USEC_PER_SEC) _heartbeat();

  GDir *d = g_dir_open(_in, 0, NULL);
  if(!d) return G_SOURCE_CONTINUE;
  GList *names = NULL;
  const char *f;
  while((f = g_dir_read_name(d)))
    if(g_str_has_suffix(f, ".json")) names = g_list_insert_sorted(names, g_strdup(f), (GCompareFunc)g_strcmp0);
  g_dir_close(d);

  for(GList *l = names; l; l = g_list_next(l))
  {
    gchar *path = g_build_filename(_in, l->data, NULL);
    gchar *text = NULL;
    gsize len = 0;
    const gboolean read = g_file_get_contents(path, &text, &len, NULL);
    g_unlink(path);
    g_free(path);
    if(!read) continue;

    JsonParser *parser = json_parser_new();
    if(json_parser_load_from_data(parser, text, len, NULL)
       && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
    {
      JsonObject *req = json_node_get_object(json_parser_get_root(parser));
      const char *id = json_object_get_string_member_with_default(req, "id", "");
      const char *cmd = json_object_get_string_member_with_default(req, "cmd", "");
      JsonObject *args = json_object_has_member(req, "args") ? json_object_get_object_member(req, "args") : NULL;
      // the id names the answer file: letters, digits and dashes only
      gboolean safe = *id && strlen(id) < 64;
      for(const char *c = id; *c && safe; c++)
        if(!g_ascii_isalnum(*c) && *c != '-') safe = FALSE;
      if(safe)
      {
        JsonObject *empty = args ? NULL : json_object_new();
        dt_print(DT_DEBUG_CONTROL, "[lightspeed bridge] %s", cmd);
        _dispatch(id, cmd, args ? args : empty);
        if(empty) json_object_unref(empty);
      }
    }
    g_object_unref(parser);
    g_free(text);
  }
  g_list_free_full(names, g_free);
  return G_SOURCE_CONTINUE;
}

void dt_lsbridge_start(void)
{
  if(_timer || !dt_conf_get_bool("plugins/lightspeed/ai/chat_bridge")) return;
  _dir = g_build_filename(darktable.configdir, "mcp", NULL);
  _in = g_build_filename(_dir, "in", NULL);
  _out = g_build_filename(_dir, "out", NULL);
  g_mkdir_with_parents(_in, 0700);
  g_mkdir_with_parents(_out, 0700);

  // what an earlier session left
  GDir *d = g_dir_open(_out, 0, NULL);
  const char *f;
  while(d && (f = g_dir_read_name(d)))
  {
    gchar *p = g_build_filename(_out, f, NULL);
    g_unlink(p);
    g_free(p);
  }
  if(d) g_dir_close(d);

  _heartbeat();
  _timer = g_timeout_add(250, _poll, NULL);
}

void dt_lsbridge_stop(void)
{
  if(!_timer) return;
  g_source_remove(_timer);
  _timer = 0;
  gchar *path = g_build_filename(_dir, "tonelark.json", NULL);
  g_unlink(path);
  g_free(path);
  g_free(_dir);
  g_free(_in);
  g_free(_out);
  _dir = _in = _out = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
