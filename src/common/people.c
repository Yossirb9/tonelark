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
  People: the faces of the photos and the people they are (see libs/people.c
  for the panel, and the chat bridge). All of it on this computer.

  A face has an embedding of 128 numbers (SFace, from the AI helper); the
  faces of one person have close embeddings. The faces are grouped by the
  mean face of each group (see dt_people_cluster).

  The tables, in the library:
    ls_faces       a face: photo, box (0..1), embedding, thumbnail, person
    ls_people      a person: name (none: unnamed), hidden
    ls_faces_not   a face that is not a person (told by the user)
    ls_faces_done  the photos searched, with faces or not
  Every photo of a person has the tag darktable|tonelark|person|<id> (the
  collection rule of a person), and a named one people|<name>.
*/

#include "common/people.h"
#include "common/darktable.h"
#include "common/database.h"
#include "common/debug.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/lightspeed_ai.h"
#include "common/tags.h"
#include "develop/imageop_math.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_jpeg.h"
#include "control/control.h"
#include "control/signal.h"

#include <math.h>
#include <string.h>

#define EMB 128
#define T_JOIN 0.50f          // a face and the mean face of a group: the same person
#define T_MERGE 0.58f         // the mean faces of two groups: the same person

// ---------------------------------------------------------------------------
// the tables

static sqlite3 *_db(void)
{
  return dt_database_get(darktable.db);
}

static void _exec(const char *sql)
{
  char *err = NULL;
  sqlite3_exec(_db(), sql, NULL, NULL, &err);
  if(err) dt_print(DT_DEBUG_ALWAYS, "[people] %s: %s", sql, err);
  sqlite3_free(err);
}

void dt_people_init(void)
{
  // clang-format off
  _exec("CREATE TABLE IF NOT EXISTS main.ls_faces"
        " (id INTEGER PRIMARY KEY, imgid INTEGER NOT NULL, x REAL, y REAL, w REAL, h REAL,"
        "  score REAL, sharp REAL, emb BLOB, thumb BLOB, person INTEGER)");
  _exec("CREATE INDEX IF NOT EXISTS main.ls_faces_imgid ON ls_faces (imgid)");
  _exec("CREATE INDEX IF NOT EXISTS main.ls_faces_person ON ls_faces (person)");
  _exec("CREATE TABLE IF NOT EXISTS main.ls_people"
        " (id INTEGER PRIMARY KEY, name TEXT, hidden INTEGER NOT NULL DEFAULT 0, time INTEGER)");
  _exec("CREATE TABLE IF NOT EXISTS main.ls_faces_not"
        " (face INTEGER NOT NULL, person INTEGER NOT NULL, PRIMARY KEY (face, person))");
  _exec("CREATE TABLE IF NOT EXISTS main.ls_faces_done (imgid INTEGER PRIMARY KEY, time INTEGER)");
  _exec("CREATE TABLE IF NOT EXISTS memory.ls_people_base (imgid INTEGER PRIMARY KEY)");
  // the photos removed from the library
  _exec("DELETE FROM main.ls_faces WHERE imgid NOT IN (SELECT id FROM main.images)");
  _exec("DELETE FROM main.ls_faces_done WHERE imgid NOT IN (SELECT id FROM main.images)");
  _exec("DELETE FROM main.ls_faces_not WHERE face NOT IN (SELECT id FROM main.ls_faces)");
  // clang-format on
}

static guint _tag_id(const char *prefix, const char *what)
{
  gchar *name = g_strconcat(prefix, what, NULL);
  guint tagid = 0;
  dt_tag_new(name, &tagid);
  g_free(name);
  return tagid;
}

static guint _person_tag(const int person)
{
  gchar *id = g_strdup_printf("%d", person);
  const guint tagid = _tag_id(PERSON_TAG, id);
  g_free(id);
  return tagid;
}

gchar *dt_people_name(const int person)
{
  gchar *name = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(_db(), "SELECT name FROM main.ls_people WHERE id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, person);
  if(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0))
    name = g_strdup((const char *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return name;
}

static GList *_query_ids(const char *sql, const int arg)
{
  GList *ids = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(_db(), sql, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, arg);
  while(sqlite3_step(stmt) == SQLITE_ROW) ids = g_list_prepend(ids, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  sqlite3_finalize(stmt);
  return ids;
}

// a tag on exactly the photos given
static void _tag_exactly(const guint tagid, GList *imgs, GList **changed)
{
  GList *has = _query_ids("SELECT imgid FROM main.tagged_images WHERE tagid = ?1", tagid);
  GList *add = NULL, *del = NULL;
  for(GList *l = imgs; l; l = g_list_next(l))
    if(!g_list_find(has, l->data)) add = g_list_prepend(add, l->data);
  for(GList *l = has; l; l = g_list_next(l))
    if(!g_list_find(imgs, l->data)) del = g_list_prepend(del, l->data);
  if(add) dt_tag_attach_images(tagid, add, FALSE);
  if(del) dt_tag_detach_images(tagid, del, FALSE);
  *changed = g_list_concat(*changed, g_list_concat(add, del));
  g_list_free(has);
}

// the tags of a person on its photos: the one of the person, and its name
static void _sync_person(const int person, GList **changed)
{
  GList *imgs = _query_ids("SELECT DISTINCT imgid FROM main.ls_faces WHERE person = ?1", person);
  const guint tagid = _person_tag(person);
  _tag_exactly(tagid, imgs, changed);
  // a person merged into another one: its tag goes
  if(!imgs) dt_tag_remove(tagid, TRUE);
  gchar *name = imgs ? dt_people_name(person) : NULL;
  if(name) _tag_exactly(_tag_id(NAME_TAG, name), imgs, changed);
  g_free(name);
  g_list_free(imgs);
}

void dt_people_sync(GHashTable *people)
{
  GList *changed = NULL;
  GHashTableIter it;
  gpointer key;
  g_hash_table_iter_init(&it, people);
  while(g_hash_table_iter_next(&it, &key, NULL)) _sync_person(GPOINTER_TO_INT(key), &changed);
  if(changed)
  {
    GList *uniq = NULL;
    for(GList *l = changed; l; l = g_list_next(l))
      if(!g_list_find(uniq, l->data)) uniq = g_list_prepend(uniq, l->data);
    dt_image_synch_xmps(uniq);
    g_list_free(uniq);
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
  }
  g_list_free(changed);
}

// ---------------------------------------------------------------------------
// the people of the faces. A group is known by its mean face, not by its
// closest face: with the closest one, A close to B close to C put A with C
// (children look alike to the model). The faces of no one, the best first,
// join the group whose mean face is closest (the people and the groups made
// on the way), else start a group; then each face goes to the closest mean
// face, a few times; then two unnamed groups too close to be two are one.
// A group of two photos or more is a person, a person is once in a photo, and
// never one the user said a face is not.

typedef struct _face_t
{
  gint64 id;
  dt_imgid_t img;
  int person;                  // in the library before, 0 none
  int group;                   // index of its group, -1 none
  gboolean fixed;              // had a person: stays in it
  float q;                     // the best faces first
  float e[EMB];
} _face_t;

typedef struct _group_t
{
  int person;                  // 0: a new group
  gboolean named;
  int count;
  float sum[EMB], c[EMB];
  GHashTable *imgs;            // photo -> number of its faces in the group
} _group_t;

static inline float _dot(const float *a, const float *b)
{
  float s = 0.0f;
  for(int k = 0; k < EMB; k++) s += a[k] * b[k];
  return s;
}

static void _normalize(float *v)
{
  float n = sqrtf(_dot(v, v));
  if(n > 1e-6f)
    for(int k = 0; k < EMB; k++) v[k] /= n;
}

static gint64 _pair(const gint64 face, const int person)
{
  return face * 1000003 + person;
}

static void _group_add(_group_t *g, const _face_t *f, const int sign)
{
  for(int k = 0; k < EMB; k++) g->sum[k] += sign * f->e[k];
  g->count += sign;
  const int n = GPOINTER_TO_INT(g_hash_table_lookup(g->imgs, GINT_TO_POINTER(f->img))) + sign;
  if(n > 0)
    g_hash_table_insert(g->imgs, GINT_TO_POINTER(f->img), GINT_TO_POINTER(n));
  else
    g_hash_table_remove(g->imgs, GINT_TO_POINTER(f->img));
  memcpy(g->c, g->sum, sizeof(g->c));
  _normalize(g->c);
}

static void _group_free(gpointer data)
{
  _group_t *g = data;
  g_hash_table_destroy(g->imgs);
  g_free(g);
}

// the closest group a face can be in, and how close (-1 none)
static int _closest(GPtrArray *groups, const _face_t *f, GHashTable *nots, float *sim)
{
  int best = -1;
  *sim = -1.0f;
  for(guint k = 0; k < groups->len; k++)
  {
    _group_t *g = g_ptr_array_index(groups, k);
    if(!g->count) continue;
    // once in a photo: another face of it in the group
    const int here = GPOINTER_TO_INT(g_hash_table_lookup(g->imgs, GINT_TO_POINTER(f->img)));
    if(here > ((int)k == f->group ? 1 : 0)) continue;
    if(g->person)
    {
      const gint64 key = _pair(f->id, g->person);
      if(g_hash_table_contains(nots, &key)) continue;
    }
    const float s = _dot(f->e, g->c);
    if(s > *sim)
    {
      *sim = s;
      best = k;
    }
  }
  return best;
}

static gint _face_cmp(gconstpointer a, gconstpointer b, gpointer faces)
{
  const _face_t *fa = &g_array_index((GArray *)faces, _face_t, *(const guint *)a);
  const _face_t *fb = &g_array_index((GArray *)faces, _face_t, *(const guint *)b);
  return fa->q > fb->q ? -1 : fa->q < fb->q ? 1 : 0;
}

// the faces of no one join the people, or become new people; returns the
// people changed (keys of the hash table)
GHashTable *dt_people_cluster(void)
{
  GHashTable *changed = g_hash_table_new(g_direct_hash, g_direct_equal);
  sqlite3 *db = _db();
  sqlite3_stmt *stmt;

  // the people as groups
  GPtrArray *groups = g_ptr_array_new_with_free_func(_group_free);
  GHashTable *of_person = g_hash_table_new(g_direct_hash, g_direct_equal);
  DT_DEBUG_SQLITE3_PREPARE_V2(db, "SELECT id, name IS NOT NULL FROM main.ls_people", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    _group_t *g = g_malloc0(sizeof(_group_t));
    g->person = sqlite3_column_int(stmt, 0);
    g->named = sqlite3_column_int(stmt, 1);
    g->imgs = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_insert(of_person, GINT_TO_POINTER(g->person), GINT_TO_POINTER(groups->len));
    g_ptr_array_add(groups, g);
  }
  sqlite3_finalize(stmt);

  // the faces
  GArray *faces = g_array_new(FALSE, TRUE, sizeof(_face_t));
  DT_DEBUG_SQLITE3_PREPARE_V2(db, "SELECT id, imgid, IFNULL(person, 0), emb, score * w * h FROM main.ls_faces",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(sqlite3_column_bytes(stmt, 3) != EMB * sizeof(float)) continue;
    _face_t f = { 0 };
    f.id = sqlite3_column_int64(stmt, 0);
    f.img = sqlite3_column_int(stmt, 1);
    f.person = sqlite3_column_int(stmt, 2);
    f.q = sqlite3_column_double(stmt, 4);
    f.group = -1;
    memcpy(f.e, sqlite3_column_blob(stmt, 3), EMB * sizeof(float));
    if(f.person && g_hash_table_contains(of_person, GINT_TO_POINTER(f.person)))
    {
      f.group = GPOINTER_TO_INT(g_hash_table_lookup(of_person, GINT_TO_POINTER(f.person)));
      f.fixed = TRUE;
    }
    else
      f.person = 0;
    g_array_append_val(faces, f);
    if(f.group >= 0) _group_add(g_ptr_array_index(groups, f.group), &g_array_index(faces, _face_t, faces->len - 1), 1);
  }
  sqlite3_finalize(stmt);

  GHashTable *nots = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(db, "SELECT face, person FROM main.ls_faces_not", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    gint64 *key = g_new(gint64, 1);
    *key = _pair(sqlite3_column_int64(stmt, 0), sqlite3_column_int(stmt, 1));
    g_hash_table_add(nots, key);
  }
  sqlite3_finalize(stmt);

  // 1. the faces of no one, the best first: into the closest group, else a new one
  GArray *todo = g_array_new(FALSE, FALSE, sizeof(guint));
  for(guint i = 0; i < faces->len; i++)
    if(!g_array_index(faces, _face_t, i).fixed) g_array_append_val(todo, i);
  g_array_sort_with_data(todo, _face_cmp, faces);
  for(guint t = 0; t < todo->len; t++)
  {
    _face_t *f = &g_array_index(faces, _face_t, g_array_index(todo, guint, t));
    float sim;
    int best = _closest(groups, f, nots, &sim);
    if(best < 0 || sim < T_JOIN)
    {
      _group_t *g = g_malloc0(sizeof(_group_t));
      g->imgs = g_hash_table_new(g_direct_hash, g_direct_equal);
      g_ptr_array_add(groups, g);
      best = groups->len - 1;
    }
    f->group = best;
    _group_add(g_ptr_array_index(groups, best), f, 1);
  }

  // 2. each face to the closest mean face, a few times
  for(int pass = 0; pass < 3; pass++)
  {
    int moved = 0;
    for(guint t = 0; t < todo->len; t++)
    {
      _face_t *f = &g_array_index(faces, _face_t, g_array_index(todo, guint, t));
      _group_t *own = g_ptr_array_index(groups, f->group);
      const float own_sim = _dot(f->e, own->c);
      float sim;
      const int best = _closest(groups, f, nots, &sim);
      if(best >= 0 && best != f->group && sim >= T_JOIN && sim > own_sim + 0.03f)
      {
        _group_add(own, f, -1);
        f->group = best;
        _group_add(g_ptr_array_index(groups, best), f, 1);
        moved++;
      }
    }
    if(!moved) break;
  }

  // 3. an unnamed group too close to another one is the same person, unless
  // they are in a photo together
  for(guint a = 0; a < groups->len; a++)
  {
    _group_t *ga = g_ptr_array_index(groups, a);
    if(ga->named || !ga->count) continue;
    int into = -1;
    float best = T_MERGE;
    for(guint b = 0; b < groups->len; b++)
    {
      _group_t *gb = g_ptr_array_index(groups, b);
      if(b == a || !gb->count) continue;
      gboolean together = FALSE;
      GHashTableIter it;
      gpointer img;
      g_hash_table_iter_init(&it, ga->imgs);
      while(!together && g_hash_table_iter_next(&it, &img, NULL))
        together = g_hash_table_contains(gb->imgs, img);
      if(together) continue;
      const float s = _dot(ga->c, gb->c);
      if(s > best)
      {
        best = s;
        into = b;
      }
    }
    if(into < 0) continue;
    _group_t *gi = g_ptr_array_index(groups, into);
    for(guint i = 0; i < faces->len; i++)
    {
      _face_t *f = &g_array_index(faces, _face_t, i);
      if(f->group != (int)a) continue;
      if(gi->person)
      {
        const gint64 key = _pair(f->id, gi->person);
        if(g_hash_table_contains(nots, &key)) continue;
      }
      _group_add(ga, f, -1);
      f->group = into;
      _group_add(gi, f, 1);
    }
  }

  // the groups of two photos or more are people; a face alone is no one
  sqlite3_stmt *ins, *upd;
  DT_DEBUG_SQLITE3_PREPARE_V2(db, "INSERT INTO main.ls_people (name, hidden, time) VALUES (NULL, 0, ?1)", -1, &ins,
                              NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(db, "UPDATE main.ls_faces SET person = ?2 WHERE id = ?1", -1, &upd, NULL);
  dt_database_start_transaction(darktable.db);
  for(guint k = 0; k < groups->len; k++)
  {
    _group_t *g = g_ptr_array_index(groups, k);
    if(!g->person && g_hash_table_size(g->imgs) >= 2)
    {
      DT_DEBUG_SQLITE3_BIND_INT64(ins, 1, g_get_real_time() / G_USEC_PER_SEC);
      sqlite3_step(ins);
      sqlite3_reset(ins);
      g->person = (int)sqlite3_last_insert_rowid(db);
    }
  }
  for(guint i = 0; i < faces->len; i++)
  {
    _face_t *f = &g_array_index(faces, _face_t, i);
    const int person = f->group >= 0 ? ((_group_t *)g_ptr_array_index(groups, f->group))->person : 0;
    if(person == f->person) continue;
    DT_DEBUG_SQLITE3_BIND_INT64(upd, 1, f->id);
    if(person)
    {
      DT_DEBUG_SQLITE3_BIND_INT(upd, 2, person);
    }
    else
      sqlite3_bind_null(upd, 2);
    sqlite3_step(upd);
    sqlite3_reset(upd);
    if(person) g_hash_table_add(changed, GINT_TO_POINTER(person));
    if(f->person) g_hash_table_add(changed, GINT_TO_POINTER(f->person));
  }
  sqlite3_finalize(ins);
  sqlite3_finalize(upd);
  // the people with no face left
  _exec("DELETE FROM main.ls_people WHERE id NOT IN (SELECT DISTINCT person FROM main.ls_faces"
        " WHERE person IS NOT NULL)");
  dt_database_release_transaction(darktable.db);

  g_array_free(todo, TRUE);
  g_array_free(faces, TRUE);
  g_ptr_array_free(groups, TRUE);
  g_hash_table_destroy(of_person);
  g_hash_table_destroy(nots);
  return changed;
}

void dt_people_store(JsonObject *res)
{
  sqlite3 *db = _db();
  sqlite3_stmt *ins, *done;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(db,
                              "INSERT INTO main.ls_faces (imgid, x, y, w, h, score, sharp, emb, thumb, person)"
                              " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, NULL)",
                              -1, &ins, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(db, "INSERT OR REPLACE INTO main.ls_faces_done (imgid, time) VALUES (?1, ?2)", -1,
                              &done, NULL);
  JsonArray *images = json_object_get_array_member(res, "images");
  dt_database_start_transaction(darktable.db);
  for(guint k = 0; images && k < json_array_get_length(images); k++)
  {
    JsonObject *im = json_array_get_object_element(images, k);
    const dt_imgid_t id = json_object_get_int_member(im, "id");
    JsonArray *faces = json_object_get_array_member(im, "faces");
    for(guint i = 0; faces && i < json_array_get_length(faces); i++)
    {
      JsonObject *f = json_array_get_object_element(faces, i);
      JsonArray *box = json_object_get_array_member(f, "box");
      gsize elen = 0, tlen = 0;
      guchar *emb = g_base64_decode(json_object_get_string_member_with_default(f, "emb", ""), &elen);
      guchar *thumb = g_base64_decode(json_object_get_string_member_with_default(f, "thumb", ""), &tlen);
      if(elen == EMB * sizeof(float) && box && json_array_get_length(box) == 4)
      {
        DT_DEBUG_SQLITE3_BIND_INT(ins, 1, id);
        for(int b = 0; b < 4; b++) DT_DEBUG_SQLITE3_BIND_DOUBLE(ins, 2 + b, json_array_get_double_element(box, b));
        DT_DEBUG_SQLITE3_BIND_DOUBLE(ins, 6, json_object_get_double_member_with_default(f, "score", 0.0));
        DT_DEBUG_SQLITE3_BIND_DOUBLE(ins, 7, json_object_get_double_member_with_default(f, "sharp", 0.0));
        DT_DEBUG_SQLITE3_BIND_BLOB(ins, 8, emb, elen, SQLITE_TRANSIENT);
        DT_DEBUG_SQLITE3_BIND_BLOB(ins, 9, thumb, tlen, SQLITE_TRANSIENT);
        sqlite3_step(ins);
        sqlite3_reset(ins);
      }
      g_free(emb);
      g_free(thumb);
    }
    DT_DEBUG_SQLITE3_BIND_INT(done, 1, id);
    DT_DEBUG_SQLITE3_BIND_INT64(done, 2, g_get_real_time() / G_USEC_PER_SEC);
    sqlite3_step(done);
    sqlite3_reset(done);
  }
  dt_database_release_transaction(darktable.db);
  sqlite3_finalize(ins);
  sqlite3_finalize(done);
}

static void _merge(const int from, const int into)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(_db(), "UPDATE main.ls_faces SET person = ?2 WHERE person = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, from);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, into);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(_db(), "UPDATE main.ls_faces_not SET person = ?2 WHERE person = ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, from);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, into);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

// ---------------------------------------------------------------------------
// the photo the faces are looked for in, without the pixelpipe: right after
// an import the thumbnails of the library are made on the GPU, and a second
// pipe at the same time crashes some drivers (and is slow). The file itself
// for the formats the helper reads, the jpeg in a raw file turned as the
// photo, else the thumbnail of the library.

#define FACE_SIDE 2400

gchar *dt_people_face_file(const dt_imgid_t id, const char *dir)
{
  char filename[PATH_MAX] = { 0 };
  gboolean from_cache = FALSE;
  dt_image_full_path(id, filename, sizeof(filename), &from_cache);
  const char *ext = strrchr(filename, '.');
  static const char *direct[] = { ".jpg", ".jpeg", ".png", ".tif", ".tiff", ".webp", ".bmp", NULL };
  for(int k = 0; ext && direct[k]; k++)
    if(!g_ascii_strcasecmp(ext, direct[k]) && g_file_test(filename, G_FILE_TEST_IS_REGULAR))
      return g_strdup(filename);

  gchar *name = g_strdup_printf("%d.jpg", id);
  gchar *path = g_build_filename(dir, name, NULL);
  g_free(name);
  uint8_t *thumb = NULL;
  int32_t w = 0, h = 0;
  dt_colorspaces_color_profile_type_t cs;
  gboolean ok = FALSE;
  if(!dt_imageio_large_thumbnail(filename, &thumb, &w, &h, &cs) && thumb && w > 0 && h > 0)
  {
    uint8_t *out = dt_alloc_align_uint8((size_t)FACE_SIDE * FACE_SIDE * 4);
    uint32_t ow = 0, oh = 0;
    if(out)
    {
      dt_iop_flip_and_zoom_8(thumb, w, h, out, FACE_SIDE, FACE_SIDE, dt_image_get_orientation(id), &ow, &oh);
      ok = ow > 0 && oh > 0 && dt_imageio_jpeg_write(path, out, ow, oh, 92, NULL, 0) == 0;
      dt_free_align(out);
    }
  }
  dt_free_align(thumb);
  if(!ok) ok = dt_lsai_write_preview(id, 1600, path);
  if(!ok)
  {
    g_free(path);
    return NULL;
  }
  return path;
}

// ---------------------------------------------------------------------------
// what the user tells

int dt_people_rename(const int person, const char *new_name)
{
  gchar *name = g_strstrip(g_strdup(new_name ? new_name : ""));
  g_strdelimit(name, "|", '-');      // a level of the tags
  gchar *old = dt_people_name(person);
  GList *changed = NULL;
  // the old name off the photos
  if(old)
  {
    const guint oldtag = _tag_id(NAME_TAG, old);
    _tag_exactly(oldtag, NULL, &changed);
    dt_tag_remove(oldtag, TRUE);
  }
  // the same name as another person: the same person
  int into = 0;
  if(*name)
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(_db(), "SELECT id FROM main.ls_people WHERE name = ?1 AND id != ?2", -1, &stmt,
                                NULL);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, person);
    if(sqlite3_step(stmt) == SQLITE_ROW) into = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  GHashTable *people = g_hash_table_new(g_direct_hash, g_direct_equal);
  if(into)
  {
    _merge(person, into);
    g_hash_table_add(people, GINT_TO_POINTER(into));
    g_hash_table_add(people, GINT_TO_POINTER(person));
  }
  else
  {
    sqlite3_stmt *stmt;
    DT_DEBUG_SQLITE3_PREPARE_V2(_db(), "UPDATE main.ls_people SET name = ?2 WHERE id = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, person);
    if(*name)
    {
      DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, name, -1, SQLITE_TRANSIENT);
    }
    else
      sqlite3_bind_null(stmt, 2);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    g_hash_table_add(people, GINT_TO_POINTER(person));
  }
  if(into)
    _exec("DELETE FROM main.ls_people WHERE id NOT IN (SELECT DISTINCT person FROM main.ls_faces"
          " WHERE person IS NOT NULL)");
  dt_people_sync(people);
  g_hash_table_destroy(people);
  if(changed) dt_image_synch_xmps(changed);
  g_list_free(changed);
  g_free(old);
  g_free(name);
  return into;
}

int dt_people_not(const int person, GList *imgs)
{
  sqlite3_stmt *sel, *no, *off;
  DT_DEBUG_SQLITE3_PREPARE_V2(_db(), "SELECT id FROM main.ls_faces WHERE imgid = ?1 AND person = ?2", -1, &sel,
                              NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(_db(), "INSERT OR IGNORE INTO main.ls_faces_not (face, person) VALUES (?1, ?2)", -1,
                              &no, NULL);
  DT_DEBUG_SQLITE3_PREPARE_V2(_db(), "UPDATE main.ls_faces SET person = NULL WHERE id = ?1", -1, &off, NULL);
  int n = 0;
  for(GList *l = imgs; l; l = g_list_next(l))
  {
    DT_DEBUG_SQLITE3_BIND_INT(sel, 1, GPOINTER_TO_INT(l->data));
    DT_DEBUG_SQLITE3_BIND_INT(sel, 2, person);
    while(sqlite3_step(sel) == SQLITE_ROW)
    {
      const gint64 face = sqlite3_column_int64(sel, 0);
      DT_DEBUG_SQLITE3_BIND_INT64(no, 1, face);
      DT_DEBUG_SQLITE3_BIND_INT(no, 2, person);
      sqlite3_step(no);
      sqlite3_reset(no);
      DT_DEBUG_SQLITE3_BIND_INT64(off, 1, face);
      sqlite3_step(off);
      sqlite3_reset(off);
      n++;
    }
    sqlite3_reset(sel);
  }
  sqlite3_finalize(sel);
  sqlite3_finalize(no);
  sqlite3_finalize(off);
  // the faces taken out may be someone else
  GHashTable *changed = dt_people_cluster();
  g_hash_table_add(changed, GINT_TO_POINTER(person));
  dt_people_sync(changed);
  g_hash_table_destroy(changed);
  return n;
}

void dt_people_hide(const int person, const gboolean hidden)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(_db(), "UPDATE main.ls_people SET hidden = ?2 WHERE id = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, person);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, hidden);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
}

JsonArray *dt_people_list(void)
{
  JsonArray *arr = json_array_new();
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(_db(),
                              "SELECT p.id, p.name, p.hidden, COUNT(DISTINCT f.imgid)"
                              " FROM main.ls_people AS p JOIN main.ls_faces AS f ON f.person = p.id"
                              " GROUP BY p.id ORDER BY p.name IS NULL, p.name, COUNT(DISTINCT f.imgid) DESC",
                              -1, &stmt, NULL);
  // clang-format on
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "id", sqlite3_column_int(stmt, 0));
    if(sqlite3_column_text(stmt, 1))
      json_object_set_string_member(o, "name", (const char *)sqlite3_column_text(stmt, 1));
    else
      json_object_set_null_member(o, "name");
    json_object_set_boolean_member(o, "hidden", sqlite3_column_int(stmt, 2));
    json_object_set_int_member(o, "photos", sqlite3_column_int(stmt, 3));
    json_array_add_object_element(arr, o);
  }
  sqlite3_finalize(stmt);
  return arr;
}

GList *dt_people_images(const int person)
{
  return _query_ids("SELECT DISTINCT imgid FROM main.ls_faces WHERE person = ?1", person);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
