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
  faces of one person have close embeddings. A face of no one joins the
  person whose mean face is closest, the faces of no one close to each other
  become a new person, and two unnamed people too close to be two are one.

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
#include "common/tags.h"
#include "control/control.h"
#include "control/signal.h"

#include <math.h>
#include <string.h>

#define EMB 128
#define T_ASSIGN 0.40f        // a face and the mean face of a person: the same person
#define T_GROUP 0.48f         // two faces of no one: the same person
#define T_MERGE 0.55f         // two people: the same person

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
// the people of the faces

typedef struct _face_t
{
  gint64 id;
  dt_imgid_t img;
  int person;
  float e[EMB];
} _face_t;

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

static int _find(int *parent, int i)
{
  while(parent[i] != i)
  {
    parent[i] = parent[parent[i]];
    i = parent[i];
  }
  return i;
}

static gint64 _pair(const gint64 face, const int person)
{
  return face * 1000003 + person;
}

typedef struct _person_t
{
  int id;
  gboolean named;
  int count;
  float c[EMB];
  GHashTable *imgs;            // the photos of the person
} _person_t;

static void _person_free(gpointer data)
{
  _person_t *p = data;
  g_hash_table_destroy(p->imgs);
  g_free(p);
}

// the faces of no one join the people, or become new people; returns the
// people changed (keys of the hash table)
GHashTable *dt_people_cluster(void)
{
  GHashTable *changed = g_hash_table_new(g_direct_hash, g_direct_equal);
  sqlite3 *db = _db();

  // the faces
  GArray *faces = g_array_new(FALSE, TRUE, sizeof(_face_t));
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(db, "SELECT id, imgid, IFNULL(person, 0), emb FROM main.ls_faces", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(sqlite3_column_bytes(stmt, 3) != EMB * sizeof(float)) continue;
    _face_t f;
    f.id = sqlite3_column_int64(stmt, 0);
    f.img = sqlite3_column_int(stmt, 1);
    f.person = sqlite3_column_int(stmt, 2);
    memcpy(f.e, sqlite3_column_blob(stmt, 3), EMB * sizeof(float));
    g_array_append_val(faces, f);
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

  // the people: the mean of their faces, their photos
  GHashTable *people = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, _person_free);
  DT_DEBUG_SQLITE3_PREPARE_V2(db, "SELECT id, name IS NOT NULL FROM main.ls_people", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    _person_t *p = g_malloc0(sizeof(_person_t));
    p->id = sqlite3_column_int(stmt, 0);
    p->named = sqlite3_column_int(stmt, 1);
    p->imgs = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_insert(people, GINT_TO_POINTER(p->id), p);
  }
  sqlite3_finalize(stmt);
  for(guint i = 0; i < faces->len; i++)
  {
    _face_t *f = &g_array_index(faces, _face_t, i);
    _person_t *p = f->person ? g_hash_table_lookup(people, GINT_TO_POINTER(f->person)) : NULL;
    if(!p)
    {
      f->person = 0;
      continue;
    }
    for(int k = 0; k < EMB; k++) p->c[k] += f->e[k];
    p->count++;
    g_hash_table_add(p->imgs, GINT_TO_POINTER(f->img));
  }
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init(&it, people);
  while(g_hash_table_iter_next(&it, &key, &value)) _normalize(((_person_t *)value)->c);

  sqlite3_stmt *upd;
  DT_DEBUG_SQLITE3_PREPARE_V2(db, "UPDATE main.ls_faces SET person = ?2 WHERE id = ?1", -1, &upd, NULL);
#define SET_PERSON(face, pid)                                                                                    \
  {                                                                                                              \
    DT_DEBUG_SQLITE3_BIND_INT64(upd, 1, (face)->id);                                                             \
    DT_DEBUG_SQLITE3_BIND_INT(upd, 2, pid);                                                                      \
    sqlite3_step(upd);                                                                                           \
    sqlite3_reset(upd);                                                                                          \
    (face)->person = pid;                                                                                        \
    g_hash_table_add(changed, GINT_TO_POINTER(pid));                                                             \
  }

  dt_database_start_transaction(darktable.db);

  // 1. a face of no one joins the closest person: not one it is told it is
  // not, not one already in its photo
  for(guint i = 0; i < faces->len; i++)
  {
    _face_t *f = &g_array_index(faces, _face_t, i);
    if(f->person) continue;
    _person_t *best = NULL;
    float best_sim = T_ASSIGN;
    g_hash_table_iter_init(&it, people);
    while(g_hash_table_iter_next(&it, &key, &value))
    {
      _person_t *p = value;
      if(!p->count || g_hash_table_contains(p->imgs, GINT_TO_POINTER(f->img))) continue;
      const gint64 k = _pair(f->id, p->id);
      if(g_hash_table_contains(nots, &k)) continue;
      const float sim = _dot(f->e, p->c);
      if(sim > best_sim)
      {
        best_sim = sim;
        best = p;
      }
    }
    if(best)
    {
      SET_PERSON(f, best->id);
      g_hash_table_add(best->imgs, GINT_TO_POINTER(f->img));
    }
  }

  // 2. the faces of no one grouped among themselves, a person once in a photo
  GArray *rest = g_array_new(FALSE, FALSE, sizeof(guint));
  for(guint i = 0; i < faces->len; i++)
    if(!g_array_index(faces, _face_t, i).person) g_array_append_val(rest, i);
  const int n = rest->len;
  if(n > 1)
  {
    int *parent = g_new(int, n);
    for(int i = 0; i < n; i++) parent[i] = i;
    for(int i = 0; i < n; i++)
    {
      _face_t *a = &g_array_index(faces, _face_t, g_array_index(rest, guint, i));
      for(int j = i + 1; j < n; j++)
      {
        _face_t *b = &g_array_index(faces, _face_t, g_array_index(rest, guint, j));
        if(a->img == b->img || _dot(a->e, b->e) < T_GROUP) continue;
        const int ra = _find(parent, i), rb = _find(parent, j);
        if(ra != rb) parent[rb] = ra;
      }
    }
    // the groups of two photos or more become people
    GHashTable *groups = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)g_list_free);
    for(int i = 0; i < n; i++)
    {
      const int r = _find(parent, i);
      GList *members = g_hash_table_lookup(groups, GINT_TO_POINTER(r));
      g_hash_table_steal(groups, GINT_TO_POINTER(r));
      g_hash_table_insert(groups, GINT_TO_POINTER(r), g_list_prepend(members, GINT_TO_POINTER(i)));
    }
    sqlite3_stmt *ins;
    DT_DEBUG_SQLITE3_PREPARE_V2(db, "INSERT INTO main.ls_people (name, hidden, time) VALUES (NULL, 0, ?1)", -1,
                                &ins, NULL);
    g_hash_table_iter_init(&it, groups);
    while(g_hash_table_iter_next(&it, &key, &value))
    {
      GHashTable *imgs = g_hash_table_new(g_direct_hash, g_direct_equal);
      for(GList *l = value; l; l = g_list_next(l))
        g_hash_table_add(imgs, GINT_TO_POINTER(g_array_index(faces, _face_t,
                                                            g_array_index(rest, guint, GPOINTER_TO_INT(l->data))).img));
      if(g_hash_table_size(imgs) >= 2)
      {
        DT_DEBUG_SQLITE3_BIND_INT64(ins, 1, g_get_real_time() / G_USEC_PER_SEC);
        sqlite3_step(ins);
        sqlite3_reset(ins);
        const int pid = (int)sqlite3_last_insert_rowid(db);
        GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
        for(GList *l = value; l; l = g_list_next(l))
        {
          _face_t *f = &g_array_index(faces, _face_t, g_array_index(rest, guint, GPOINTER_TO_INT(l->data)));
          if(g_hash_table_contains(seen, GINT_TO_POINTER(f->img))) continue;   // once in a photo
          g_hash_table_add(seen, GINT_TO_POINTER(f->img));
          SET_PERSON(f, pid);
        }
        g_hash_table_destroy(seen);
      }
      g_hash_table_destroy(imgs);
    }
    sqlite3_finalize(ins);
    g_hash_table_destroy(groups);
    g_free(parent);
  }
  g_array_free(rest, TRUE);

  // 3. an unnamed person too close to another one is the same: merged, unless
  // they are in a photo together
  g_hash_table_remove_all(people);
  DT_DEBUG_SQLITE3_PREPARE_V2(db, "SELECT id, name IS NOT NULL FROM main.ls_people", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    _person_t *p = g_malloc0(sizeof(_person_t));
    p->id = sqlite3_column_int(stmt, 0);
    p->named = sqlite3_column_int(stmt, 1);
    p->imgs = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_hash_table_insert(people, GINT_TO_POINTER(p->id), p);
  }
  sqlite3_finalize(stmt);
  for(guint i = 0; i < faces->len; i++)
  {
    _face_t *f = &g_array_index(faces, _face_t, i);
    _person_t *p = f->person ? g_hash_table_lookup(people, GINT_TO_POINTER(f->person)) : NULL;
    if(!p) continue;
    for(int k = 0; k < EMB; k++) p->c[k] += f->e[k];
    p->count++;
    g_hash_table_add(p->imgs, GINT_TO_POINTER(f->img));
  }
  GList *list = g_hash_table_get_values(people);
  for(GList *l = list; l; l = g_list_next(l)) _normalize(((_person_t *)l->data)->c);
  for(GList *a = list; a; a = g_list_next(a))
  {
    _person_t *pa = a->data;
    if(pa->named || !pa->count) continue;
    _person_t *into = NULL;
    float best = T_MERGE;
    for(GList *b = list; b; b = g_list_next(b))
    {
      _person_t *pb = b->data;
      if(pb == pa || !pb->count) continue;
      gboolean together = FALSE;
      GHashTableIter ii;
      gpointer img;
      g_hash_table_iter_init(&ii, pa->imgs);
      while(!together && g_hash_table_iter_next(&ii, &img, NULL))
        together = g_hash_table_contains(pb->imgs, img);
      if(together) continue;
      const float sim = _dot(pa->c, pb->c);
      if(sim > best)
      {
        best = sim;
        into = pb;
      }
    }
    if(!into) continue;
    for(guint i = 0; i < faces->len; i++)
    {
      _face_t *f = &g_array_index(faces, _face_t, i);
      if(f->person != pa->id) continue;
      const gint64 k = _pair(f->id, into->id);
      if(g_hash_table_contains(nots, &k)) continue;
      SET_PERSON(f, into->id);
      g_hash_table_add(into->imgs, GINT_TO_POINTER(f->img));
    }
    g_hash_table_add(changed, GINT_TO_POINTER(pa->id));
    // the mean face of the merged person, for the next ones
    for(int k = 0; k < EMB; k++) into->c[k] = into->c[k] * into->count + pa->c[k] * pa->count;
    into->count += pa->count;
    _normalize(into->c);
    pa->count = 0;
  }
  g_list_free(list);
#undef SET_PERSON
  sqlite3_finalize(upd);

  // the people with no face left
  _exec("DELETE FROM main.ls_people WHERE id NOT IN (SELECT DISTINCT person FROM main.ls_faces"
        " WHERE person IS NOT NULL)");
  dt_database_release_transaction(darktable.db);

  g_hash_table_destroy(people);
  g_hash_table_destroy(nots);
  g_array_free(faces, TRUE);
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
