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

  A face has an embedding of 512 numbers (ArcFace, from the AI helper); the
  faces of one person have close embeddings. The faces are grouped by their
  average similarity (see dt_people_cluster).

  The tables, in the library:
    ls_faces       a face: photo, box (0..1), embedding, thumbnail, person
    ls_people      a person: name (none: unnamed), hidden
    ls_faces_not   a face that is not a person (told by the user)
    ls_faces_done  the photos searched, with faces or not
    ls_faces_named_old  the faces of the named people, kept by a change of the
                   face model until their photos are searched again
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

#define EMB 512               // ArcFace
#define T_SAME 0.50f          // the average similarity of two groups: one person
#define T_WEAK 0.60f          // a small or blurry face joins a person this close
#define MIN_PX 56.0f          // a smaller face (pixels in the photo at 2400) starts no person
#define MIN_SCORE 0.8f        // nor a face the detector is unsure of

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

static guint _person_tag(const int person);

// the faces of another face model (SFace, 128 numbers) cannot be compared to
// the new ones: they go, and their photos are to be searched again. The named
// people stay, with where their faces were, to be given back their faces
// (see dt_people_store); the unnamed ones go with their tags.
static void _new_model(void)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(_db(), "SELECT COUNT(*) FROM main.ls_faces WHERE LENGTH(emb) != ?1", -1, &stmt,
                              NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, EMB * sizeof(float));
  const int old = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
  sqlite3_finalize(stmt);
  if(!old) return;
  // clang-format off
  _exec("INSERT INTO main.ls_faces_named_old (imgid, x, y, w, h, person)"
        " SELECT f.imgid, f.x, f.y, f.w, f.h, f.person FROM main.ls_faces AS f"
        " JOIN main.ls_people AS p ON p.id = f.person WHERE p.name IS NOT NULL");
  // clang-format on
  GList *gone = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(_db(), "SELECT id FROM main.ls_people WHERE name IS NULL", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW) gone = g_list_prepend(gone, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  sqlite3_finalize(stmt);
  for(GList *l = gone; l; l = g_list_next(l)) dt_tag_remove(_person_tag(GPOINTER_TO_INT(l->data)), TRUE);
  g_list_free(gone);
  _exec("DELETE FROM main.ls_faces");
  _exec("DELETE FROM main.ls_faces_done");
  _exec("DELETE FROM main.ls_faces_not");
  _exec("DELETE FROM main.ls_people WHERE name IS NULL");
  dt_conf_set_bool("plugins/lighttable/people/improved", TRUE);
  dt_print(DT_DEBUG_ALWAYS, "[people] a new face model: %d faces to find again", old);
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
  _exec("CREATE TABLE IF NOT EXISTS main.ls_faces_named_old"
        " (imgid INTEGER, x REAL, y REAL, w REAL, h REAL, person INTEGER)");
  // the size of a face in pixels (added in 1.3.1)
  sqlite3_stmt *stmt = NULL;
  if(sqlite3_prepare_v2(_db(), "SELECT px FROM main.ls_faces LIMIT 0", -1, &stmt, NULL) != SQLITE_OK)
    _exec("ALTER TABLE main.ls_faces ADD COLUMN px REAL");
  sqlite3_finalize(stmt);
  _new_model();
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
// the people of the faces. Two groups are one person when their faces are
// close on average (average linkage: the similarity of every face of one to
// every face of the other, the mean of it being the dot of the sums of their
// embeddings), the closest groups first; not by the mean face or the closest
// face, which let a group of children who look alike grow. A person is once
// in a photo, never one the user said a face is not, and two named people are
// never one. The faces too small, blurry or unsure to start a person only
// join one they are very close to. The named people keep their faces; the
// others are grouped again each time, and a group keeps the id of the person
// most of its faces had.

typedef struct _face_t
{
  gint64 id;
  dt_imgid_t img;
  int old;                     // its person before, 0 none
  gboolean good;               // big, sharp and sure enough to start a person
  int cl;                      // its group, -1 none
  float e[EMB];
} _face_t;

typedef struct _cl_t
{
  float sum[EMB];
  int n;
  int named;                   // a named person, 0 not
  GHashTable *imgs;            // its photos
  GHashTable *nots;            // the people its faces are not, NULL none
  gboolean alive;
  int best;                    // its closest group it can join, -1 none
  float bsim;
} _cl_t;

static inline float _dot(const float *a, const float *b)
{
  float s = 0.0f;
  for(int k = 0; k < EMB; k++) s += a[k] * b[k];
  return s;
}

static gboolean _can_join(const _cl_t *a, const _cl_t *b)
{
  if(a->named && b->named) return FALSE;
  if(b->named && a->nots && g_hash_table_contains(a->nots, GINT_TO_POINTER(b->named))) return FALSE;
  if(a->named && b->nots && g_hash_table_contains(b->nots, GINT_TO_POINTER(a->named))) return FALSE;
  // once in a photo
  const gboolean small_a = g_hash_table_size(a->imgs) <= g_hash_table_size(b->imgs);
  GHashTable *small = small_a ? a->imgs : b->imgs, *big = small_a ? b->imgs : a->imgs;
  GHashTableIter it;
  gpointer img;
  g_hash_table_iter_init(&it, small);
  while(g_hash_table_iter_next(&it, &img, NULL))
    if(g_hash_table_contains(big, img)) return FALSE;
  return TRUE;
}

static float _link(const _cl_t *a, const _cl_t *b)
{
  if(!_can_join(a, b)) return -2.0f;
  return _dot(a->sum, b->sum) / (float)(a->n * b->n);
}

static void _find_best(_cl_t *cl, const int n, const int i)
{
  cl[i].best = -1;
  cl[i].bsim = -2.0f;
  for(int k = 0; k < n; k++)
  {
    if(k == i || !cl[k].alive) continue;
    const float s = _link(&cl[i], &cl[k]);
    if(s > cl[i].bsim)
    {
      cl[i].bsim = s;
      cl[i].best = k;
    }
  }
}

static int _root(int *parent, int k)
{
  while(parent[k] != k) k = parent[k] = parent[parent[k]];
  return k;
}

static gint _float_cmp(gconstpointer a, gconstpointer b)
{
  const float fa = *(const float *)a, fb = *(const float *)b;
  return fa < fb ? -1 : fa > fb ? 1 : 0;
}

static gint64 _pair(const gint64 face, const int person)
{
  return face * 1000003 + person;
}

// the people with no face left go, but not a named one whose faces are to be
// found again (after a change of the face model)
static void _drop_empty_people(void)
{
  _exec("DELETE FROM main.ls_people WHERE id NOT IN (SELECT DISTINCT person FROM main.ls_faces"
        " WHERE person IS NOT NULL)"
        " AND (name IS NULL OR id NOT IN (SELECT person FROM main.ls_faces_named_old))");
}

// the faces of no one join the people or become new ones: returns the people
// changed (keys of the hash table)
GHashTable *dt_people_cluster(void)
{
  GHashTable *changed = g_hash_table_new(g_direct_hash, g_direct_equal);
  sqlite3 *db = _db();
  sqlite3_stmt *stmt;

  // the faces, and their persons before
  GArray *faces = g_array_new(FALSE, TRUE, sizeof(_face_t));
  GArray *sharps = g_array_new(FALSE, FALSE, sizeof(float));
  GHashTable *named = g_hash_table_new(g_direct_hash, g_direct_equal);   // named person -> its group + 1
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(db,
                              "SELECT f.id, f.imgid, IFNULL(f.person, 0), p.name IS NOT NULL, f.emb,"
                              "       IFNULL(f.px, 0), IFNULL(f.sharp, 0), IFNULL(f.score, 1)"
                              " FROM main.ls_faces AS f LEFT JOIN main.ls_people AS p ON p.id = f.person",
                              -1, &stmt, NULL);
  // clang-format on
  GArray *px = g_array_new(FALSE, FALSE, sizeof(float)), *score = g_array_new(FALSE, FALSE, sizeof(float));
  GArray *is_named = g_array_new(FALSE, FALSE, sizeof(gboolean));
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    if(sqlite3_column_bytes(stmt, 4) != EMB * sizeof(float)) continue;
    _face_t f = { 0 };
    f.id = sqlite3_column_int64(stmt, 0);
    f.img = sqlite3_column_int(stmt, 1);
    f.old = sqlite3_column_int(stmt, 2);
    f.cl = -1;
    memcpy(f.e, sqlite3_column_blob(stmt, 4), EMB * sizeof(float));
    const gboolean nm = f.old && sqlite3_column_int(stmt, 3);
    const float p = sqlite3_column_double(stmt, 5), sh = sqlite3_column_double(stmt, 6);
    const float sc = sqlite3_column_double(stmt, 7);
    g_array_append_val(faces, f);
    g_array_append_val(sharps, sh);
    g_array_append_val(px, p);
    g_array_append_val(score, sc);
    g_array_append_val(is_named, nm);
  }
  sqlite3_finalize(stmt);
  const int nf = faces->len;

  // blurry: the fifth of the faces the least sharp
  float sharp_cut = 0.0f;
  if(sharps->len)
  {
    GArray *sorted = g_array_sized_new(FALSE, FALSE, sizeof(float), sharps->len);
    g_array_append_vals(sorted, sharps->data, sharps->len);
    g_array_sort(sorted, _float_cmp);
    sharp_cut = g_array_index(sorted, float, sorted->len / 5);
    g_array_free(sorted, TRUE);
  }
  for(int i = 0; i < nf; i++)
  {
    _face_t *f = &g_array_index(faces, _face_t, i);
    const float p = g_array_index(px, float, i);
    f->good = (p <= 0.0f || p >= MIN_PX) && g_array_index(score, float, i) >= MIN_SCORE
              && g_array_index(sharps, float, i) >= sharp_cut;
  }

  // what the user said a face is not
  GHashTable *nots = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
  GHashTable *not_of = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, (GDestroyNotify)g_list_free);
  DT_DEBUG_SQLITE3_PREPARE_V2(db, "SELECT face, person FROM main.ls_faces_not", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const gint64 face = sqlite3_column_int64(stmt, 0);
    const int person = sqlite3_column_int(stmt, 1);
    gint64 *key = g_new(gint64, 1);
    *key = _pair(face, person);
    g_hash_table_add(nots, key);
    gint64 *fk = g_new(gint64, 1);
    *fk = face;
    GList *l = g_hash_table_lookup(not_of, fk);
    g_hash_table_steal(not_of, fk);
    g_hash_table_insert(not_of, fk, g_list_prepend(l, GINT_TO_POINTER(person)));
  }
  sqlite3_finalize(stmt);

  // the groups: a named person with its faces, a face that can start a person
  _cl_t *cl = g_new0(_cl_t, MAX(1, nf));
  int *parent = g_new(int, MAX(1, nf));
  int nc = 0;
  for(int i = 0; i < nf; i++)
  {
    _face_t *f = &g_array_index(faces, _face_t, i);
    const gboolean nm = g_array_index(is_named, gboolean, i);
    int c = -1;
    if(nm)
    {
      const int in = GPOINTER_TO_INT(g_hash_table_lookup(named, GINT_TO_POINTER(f->old)));
      if(in) c = in - 1;
    }
    else if(!f->good)
      continue;
    if(c < 0)
    {
      c = nc++;
      cl[c].imgs = g_hash_table_new(g_direct_hash, g_direct_equal);
      cl[c].alive = TRUE;
      parent[c] = c;
      if(nm)
      {
        cl[c].named = f->old;
        g_hash_table_insert(named, GINT_TO_POINTER(f->old), GINT_TO_POINTER(c + 1));
      }
    }
    for(int k = 0; k < EMB; k++) cl[c].sum[k] += f->e[k];
    cl[c].n++;
    g_hash_table_add(cl[c].imgs, GINT_TO_POINTER(f->img));
    for(GList *l = g_hash_table_lookup(not_of, &f->id); l; l = g_list_next(l))
    {
      if(!cl[c].nots) cl[c].nots = g_hash_table_new(g_direct_hash, g_direct_equal);
      g_hash_table_add(cl[c].nots, l->data);
    }
    f->cl = c;
  }

  // the closest groups first, while they are close enough
  for(int i = 0; i < nc; i++) _find_best(cl, nc, i);
  while(TRUE)
  {
    int i = -1;
    float top = T_SAME;
    for(int k = 0; k < nc; k++)
      if(cl[k].alive && cl[k].best >= 0 && cl[k].bsim >= top)
      {
        top = cl[k].bsim;
        i = k;
      }
    if(i < 0) break;
    int j = cl[i].best;
    // the group of a named person takes the other one
    if(cl[j].named)
    {
      const int t = i;
      i = j;
      j = t;
    }
    _cl_t *a = &cl[i], *b = &cl[j];
    for(int k = 0; k < EMB; k++) a->sum[k] += b->sum[k];
    a->n += b->n;
    GHashTableIter it;
    gpointer key;
    g_hash_table_iter_init(&it, b->imgs);
    while(g_hash_table_iter_next(&it, &key, NULL)) g_hash_table_add(a->imgs, key);
    if(b->nots)
    {
      if(!a->nots) a->nots = g_hash_table_new(g_direct_hash, g_direct_equal);
      g_hash_table_iter_init(&it, b->nots);
      while(g_hash_table_iter_next(&it, &key, NULL)) g_hash_table_add(a->nots, key);
    }
    b->alive = FALSE;
    parent[j] = i;
    // the closest of the new group, and of the groups that had one of the two
    _find_best(cl, nc, i);
    for(int k = 0; k < nc; k++)
      if(cl[k].alive && k != i && (cl[k].best == i || cl[k].best == j)) _find_best(cl, nc, k);
  }
  for(int i = 0; i < nf; i++)
  {
    _face_t *f = &g_array_index(faces, _face_t, i);
    if(f->cl >= 0) f->cl = _root(parent, f->cl);
  }

  // the faces that cannot start a person join one they are very close to
  for(int i = 0; i < nf; i++)
  {
    _face_t *f = &g_array_index(faces, _face_t, i);
    if(f->cl >= 0) continue;
    int best = -1;
    float bsim = T_WEAK;
    for(int c = 0; c < nc; c++)
    {
      if(!cl[c].alive || g_hash_table_contains(cl[c].imgs, GINT_TO_POINTER(f->img))) continue;
      if(cl[c].named)
      {
        const gint64 key = _pair(f->id, cl[c].named);
        if(g_hash_table_contains(nots, &key)) continue;
      }
      const float s = _dot(f->e, cl[c].sum) / (float)cl[c].n;
      if(s > bsim)
      {
        bsim = s;
        best = c;
      }
    }
    if(best >= 0)
    {
      f->cl = best;
      g_hash_table_add(cl[best].imgs, GINT_TO_POINTER(f->img));
    }
  }

  // the person of each group: a named one, or the one most of its faces had
  // before (the ids stay), or a new one; a group of one photo is no one
  int *person_of = g_new0(int, MAX(1, nc));
  GArray *order = g_array_new(FALSE, FALSE, sizeof(int));
  for(int c = 0; c < nc; c++)
    if(cl[c].alive)
    {
      if(cl[c].named)
        person_of[c] = cl[c].named;
      else if(g_hash_table_size(cl[c].imgs) >= 2)
        g_array_append_val(order, c);
    }
  // the biggest groups choose first
  for(guint a = 0; a < order->len; a++)
    for(guint b = a + 1; b < order->len; b++)
      if(cl[g_array_index(order, int, b)].n > cl[g_array_index(order, int, a)].n)
      {
        const int t = g_array_index(order, int, a);
        g_array_index(order, int, a) = g_array_index(order, int, b);
        g_array_index(order, int, b) = t;
      }
  GHashTable *taken = g_hash_table_new(g_direct_hash, g_direct_equal);
  sqlite3_stmt *ins;
  DT_DEBUG_SQLITE3_PREPARE_V2(db, "INSERT INTO main.ls_people (name, hidden, time) VALUES (NULL, 0, ?1)", -1, &ins,
                              NULL);
  dt_database_start_transaction(darktable.db);
  for(guint o = 0; o < order->len; o++)
  {
    const int c = g_array_index(order, int, o);
    GHashTable *votes = g_hash_table_new(g_direct_hash, g_direct_equal);
    int winner = 0, most = 0;
    for(int i = 0; i < nf; i++)
    {
      const _face_t *f = &g_array_index(faces, _face_t, i);
      if(f->cl != c || !f->old || g_array_index(is_named, gboolean, i)
         || g_hash_table_contains(taken, GINT_TO_POINTER(f->old)))
        continue;
      const int v = GPOINTER_TO_INT(g_hash_table_lookup(votes, GINT_TO_POINTER(f->old))) + 1;
      g_hash_table_insert(votes, GINT_TO_POINTER(f->old), GINT_TO_POINTER(v));
      if(v > most)
      {
        most = v;
        winner = f->old;
      }
    }
    g_hash_table_destroy(votes);
    if(!winner)
    {
      DT_DEBUG_SQLITE3_BIND_INT64(ins, 1, g_get_real_time() / G_USEC_PER_SEC);
      sqlite3_step(ins);
      sqlite3_reset(ins);
      winner = (int)sqlite3_last_insert_rowid(db);
    }
    g_hash_table_add(taken, GINT_TO_POINTER(winner));
    person_of[c] = winner;
  }

  // the faces to their people
  sqlite3_stmt *upd;
  DT_DEBUG_SQLITE3_PREPARE_V2(db, "UPDATE main.ls_faces SET person = ?2 WHERE id = ?1", -1, &upd, NULL);
  for(int i = 0; i < nf; i++)
  {
    const _face_t *f = &g_array_index(faces, _face_t, i);
    int person = f->cl >= 0 ? person_of[f->cl] : 0;
    if(person)
    {
      const gint64 key = _pair(f->id, person);
      if(g_hash_table_contains(nots, &key)) person = 0;
    }
    if(person == f->old) continue;
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
    if(f->old) g_hash_table_add(changed, GINT_TO_POINTER(f->old));
  }
  sqlite3_finalize(ins);
  sqlite3_finalize(upd);
  _drop_empty_people();
  dt_database_release_transaction(darktable.db);

  for(int c = 0; c < nc; c++)
  {
    if(cl[c].imgs) g_hash_table_destroy(cl[c].imgs);
    if(cl[c].nots) g_hash_table_destroy(cl[c].nots);
  }
  g_free(cl);
  g_free(parent);
  g_free(person_of);
  g_array_free(order, TRUE);
  g_hash_table_destroy(taken);
  g_hash_table_destroy(named);
  g_hash_table_destroy(nots);
  g_hash_table_destroy(not_of);
  g_array_free(faces, TRUE);
  g_array_free(sharps, TRUE);
  g_array_free(px, TRUE);
  g_array_free(score, TRUE);
  g_array_free(is_named, TRUE);
  return changed;
}

void dt_people_store(JsonObject *res)
{
  sqlite3 *db = _db();
  sqlite3_stmt *ins, *done;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(db,
                              "INSERT INTO main.ls_faces (imgid, x, y, w, h, score, sharp, emb, thumb, person, px)"
                              " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, NULL, ?10)",
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
        DT_DEBUG_SQLITE3_BIND_DOUBLE(ins, 10, json_object_get_double_member_with_default(f, "px", 0.0));
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
  // the named people get back their faces, where they were (a face model
  // changed): the same photo, boxes over each other
  // clang-format off
  _exec("UPDATE main.ls_faces SET person ="
        " (SELECT o.person FROM main.ls_faces_named_old AS o WHERE o.imgid = ls_faces.imgid"
        "   AND MAX(0, MIN(o.x + o.w, ls_faces.x + ls_faces.w) - MAX(o.x, ls_faces.x))"
        "     * MAX(0, MIN(o.y + o.h, ls_faces.y + ls_faces.h) - MAX(o.y, ls_faces.y))"
        "     > 0.4 * MIN(o.w * o.h, ls_faces.w * ls_faces.h) LIMIT 1)"
        " WHERE person IS NULL AND imgid IN (SELECT imgid FROM main.ls_faces_named_old)");
  _exec("DELETE FROM main.ls_faces_named_old WHERE imgid IN (SELECT imgid FROM main.ls_faces_done)");
  // clang-format on
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
  if(into) _drop_empty_people();
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
