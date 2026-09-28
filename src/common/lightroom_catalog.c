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

#include "common/lightroom_catalog.h"
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/file_location.h"
#include "common/film.h"
#include "common/grouping.h"
#include "common/history.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/metadata.h"
#include "common/mipmap_cache.h"
#include "common/ratings.h"
#include "common/tags.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/develop.h"
#include "develop/lightroom.h"
#include "gui/gtk.h"
#include "views/view.h"

#include <gio/gio.h>
#include <sqlite3.h>
#include <string.h>
#include <zlib.h>

#define LR_PICK_TAG "darktable|pick"
#define LR_COLLECTIONS_TAG "Lightroom collections"

// ---------------------------------------------------------------------------
// small sqlite helpers

static gboolean _has_table(sqlite3 *db, const char *table)
{
  sqlite3_stmt *stmt;
  gboolean found = FALSE;
  if(sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1",
                        -1, &stmt, NULL) == SQLITE_OK)
  {
    sqlite3_bind_text(stmt, 1, table, -1, SQLITE_TRANSIENT);
    found = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
  }
  return found;
}

static gboolean _has_column(sqlite3 *db, const char *table, const char *column)
{
  gchar *q = g_strdup_printf("PRAGMA table_info(%s)", table);
  sqlite3_stmt *stmt;
  gboolean found = FALSE;
  if(sqlite3_prepare_v2(db, q, -1, &stmt, NULL) == SQLITE_OK)
  {
    while(!found && sqlite3_step(stmt) == SQLITE_ROW)
      found = !g_strcmp0((const char *)sqlite3_column_text(stmt, 1), column);
    sqlite3_finalize(stmt);
  }
  g_free(q);
  return found;
}

static int _count(sqlite3 *db, const char *query)
{
  sqlite3_stmt *stmt;
  int n = 0;
  if(sqlite3_prepare_v2(db, query, -1, &stmt, NULL) == SQLITE_OK)
  {
    if(sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  return n;
}

// Lightroom stores numbers as REAL, text columns may be compressed blobs:
// 4 bytes big-endian uncompressed size followed by a zlib stream (Lr 9+)
static gchar *_column_text(sqlite3_stmt *stmt, const int col, size_t *len)
{
  const int type = sqlite3_column_type(stmt, col);
  if(type == SQLITE_NULL) return NULL;

  const unsigned char *data = type == SQLITE_BLOB
    ? sqlite3_column_blob(stmt, col)
    : sqlite3_column_text(stmt, col);
  const int size = sqlite3_column_bytes(stmt, col);
  if(!data || size <= 0) return NULL;

  const gboolean plain = type == SQLITE_TEXT || data[0] == '<' || data[0] == 's'
                         || data[0] == ' ' || data[0] == '\n' || data[0] == 0xEF;
  if(plain || size < 8)
  {
    if(len) *len = size;
    return g_strndup((const char *)data, size);
  }

  const uLongf expected = ((uLongf)data[0] << 24) | ((uLongf)data[1] << 16)
                          | ((uLongf)data[2] << 8) | (uLongf)data[3];
  if(expected == 0 || expected > 64 * 1024 * 1024) return NULL;

  uLongf out_len = expected;
  gchar *out = g_malloc(expected + 1);
  if(uncompress((Bytef *)out, &out_len, data + 4, size - 4) != Z_OK)
  {
    g_free(out);
    return NULL;
  }
  out[out_len] = '\0';
  if(len) *len = out_len;
  return out;
}

// ---------------------------------------------------------------------------
// paths

static gchar *_native_path(const char *path)
{
  gchar *p = g_strdup(path);
#ifdef _WIN32
  for(gchar *c = p; *c; c++)
    if(*c == '/') *c = '\\';
#endif
  return p;
}

static gchar *_image_path(const dt_lrcat_root_t *root, const char *from_root,
                          const char *base, const char *ext)
{
  if(!root->resolved) return NULL;
  gchar *file = ext && *ext ? g_strdup_printf("%s.%s", base, ext) : g_strdup(base);
  gchar *rel = _native_path(from_root ? from_root : "");
  gchar *path = g_build_filename(root->resolved, rel, file, NULL);
  g_free(file);
  g_free(rel);
  return path;
}

static dt_lrcat_root_t *_find_root(const dt_lrcat_t *cat, const int64_t id)
{
  for(guint k = 0; k < cat->roots->len; k++)
  {
    dt_lrcat_root_t *r = g_ptr_array_index(cat->roots, k);
    if(r->id == id) return r;
  }
  return NULL;
}

static void _count_root(dt_lrcat_t *cat, dt_lrcat_root_t *root)
{
  root->n_found = 0;
  if(!root->resolved) return;

  sqlite3_stmt *stmt;
  if(sqlite3_prepare_v2(cat->db,
                        "SELECT fo.pathFromRoot, fi.baseName, fi.extension"
                        " FROM Adobe_images i"
                        " JOIN AgLibraryFile fi ON fi.id_local = i.rootFile"
                        " JOIN AgLibraryFolder fo ON fo.id_local = fi.folder"
                        " WHERE fo.rootFolder = ?1",
                        -1, &stmt, NULL) != SQLITE_OK)
    return;
  sqlite3_bind_int64(stmt, 1, root->id);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    gchar *path = _image_path(root, (const char *)sqlite3_column_text(stmt, 0),
                              (const char *)sqlite3_column_text(stmt, 1),
                              (const char *)sqlite3_column_text(stmt, 2));
    if(path && g_file_test(path, G_FILE_TEST_EXISTS)) root->n_found++;
    g_free(path);
  }
  sqlite3_finalize(stmt);
}

void dt_lrcat_set_root_folder(dt_lrcat_t *cat, dt_lrcat_root_t *root, const char *folder)
{
  g_free(root->resolved);
  root->resolved = folder && *folder ? g_strdup(folder) : NULL;
  _count_root(cat, root);
}

int dt_lrcat_count_found(const dt_lrcat_t *cat)
{
  int n = 0;
  for(guint k = 0; k < cat->roots->len; k++)
    n += ((dt_lrcat_root_t *)g_ptr_array_index(cat->roots, k))->n_found;
  return n;
}

// ---------------------------------------------------------------------------
// open / close

static void _root_free(gpointer data)
{
  dt_lrcat_root_t *r = data;
  g_free(r->absolute_path);
  g_free(r->relative_path);
  g_free(r->name);
  g_free(r->resolved);
  g_free(r);
}

static gboolean _copy_file(const char *from, const char *to, GError **error)
{
  GFile *src = g_file_new_for_path(from);
  GFile *dst = g_file_new_for_path(to);
  const gboolean ok = g_file_copy(src, dst, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, error);
  g_object_unref(src);
  g_object_unref(dst);
  return ok;
}

void dt_lrcat_close(dt_lrcat_t *cat)
{
  if(!cat) return;
  if(cat->db) sqlite3_close(cat->db);
  if(cat->copy)
  {
    static const char *suffix[] = { "", "-wal", "-shm", "-journal", NULL };
    for(int k = 0; suffix[k]; k++)
    {
      gchar *f = g_strconcat(cat->copy, suffix[k], NULL);
      g_unlink(f);
      g_free(f);
    }
  }
  if(cat->roots) g_ptr_array_free(cat->roots, TRUE);
  g_free(cat->filename);
  g_free(cat->copy);
  g_free(cat->version);
  g_free(cat);
}

dt_lrcat_t *dt_lrcat_open(const char *filename, GError **error)
{
  if(!filename || !g_file_test(filename, G_FILE_TEST_IS_REGULAR))
  {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT, _("catalog `%s' not found"), filename);
    return NULL;
  }

  dt_lrcat_t *cat = g_malloc0(sizeof(dt_lrcat_t));
  cat->filename = g_strdup(filename);
  cat->roots = g_ptr_array_new_with_free_func(_root_free);

  // work on a private copy: Lightroom may have the catalog open, and the
  // original must never be touched
  char tmpdir[PATH_MAX] = { 0 };
  dt_loc_get_tmp_dir(tmpdir, sizeof(tmpdir));
  gchar *name = g_strdup_printf("tonelark-import-%u.lrcat", g_random_int());
  cat->copy = g_build_filename(tmpdir, name, NULL);
  g_free(name);

  if(!_copy_file(filename, cat->copy, error))
  {
    dt_lrcat_close(cat);
    return NULL;
  }
  // a write-ahead log may hold the latest changes
  static const char *suffix[] = { "-wal", "-shm", NULL };
  for(int k = 0; suffix[k]; k++)
  {
    gchar *from = g_strconcat(filename, suffix[k], NULL);
    gchar *to = g_strconcat(cat->copy, suffix[k], NULL);
    if(g_file_test(from, G_FILE_TEST_EXISTS)) _copy_file(from, to, NULL);
    g_free(from);
    g_free(to);
  }

  if(sqlite3_open_v2(cat->copy, &cat->db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK
     || !_has_table(cat->db, "Adobe_images")
     || !_has_table(cat->db, "AgLibraryFile")
     || !_has_table(cat->db, "AgLibraryFolder")
     || !_has_table(cat->db, "AgLibraryRootFolder"))
  {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                _("`%s' is not a Lightroom catalog"), filename);
    dt_lrcat_close(cat);
    return NULL;
  }

  sqlite3_stmt *stmt;
  if(_has_table(cat->db, "Adobe_variablesTable")
     && sqlite3_prepare_v2(cat->db,
                           "SELECT value FROM Adobe_variablesTable WHERE name='Adobe_DBVersion'",
                           -1, &stmt, NULL) == SQLITE_OK)
  {
    if(sqlite3_step(stmt) == SQLITE_ROW)
      cat->version = g_strdup((const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
  }

  cat->n_images = _count(cat->db, "SELECT COUNT(*) FROM Adobe_images");
  if(_has_column(cat->db, "Adobe_images", "masterImage"))
    cat->n_virtual_copies =
      _count(cat->db, "SELECT COUNT(*) FROM Adobe_images WHERE masterImage IS NOT NULL");
  if(_has_table(cat->db, "AgLibraryCollection"))
    cat->n_collections =
      _count(cat->db, "SELECT COUNT(*) FROM AgLibraryCollection"
                      " WHERE creationId = 'com.adobe.ag.library.collection'");
  if(_has_table(cat->db, "AgLibraryKeyword"))
    cat->n_keywords = _count(cat->db, "SELECT COUNT(*) FROM AgLibraryKeyword WHERE name IS NOT NULL");

  // root folders, resolved to the stored path or relative to the catalog
  gchar *catdir = g_path_get_dirname(filename);
  const gboolean has_rel = _has_column(cat->db, "AgLibraryRootFolder", "relativePathFromCatalog");
  gchar *q = g_strdup_printf("SELECT id_local, absolutePath, name, %s FROM AgLibraryRootFolder",
                             has_rel ? "relativePathFromCatalog" : "NULL");
  if(sqlite3_prepare_v2(cat->db, q, -1, &stmt, NULL) == SQLITE_OK)
  {
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      dt_lrcat_root_t *r = g_malloc0(sizeof(dt_lrcat_root_t));
      r->id = sqlite3_column_int64(stmt, 0);
      r->absolute_path = _native_path((const char *)sqlite3_column_text(stmt, 1));
      r->name = g_strdup((const char *)sqlite3_column_text(stmt, 2));
      if(sqlite3_column_type(stmt, 3) != SQLITE_NULL)
        r->relative_path = _native_path((const char *)sqlite3_column_text(stmt, 3));

      if(r->absolute_path && g_file_test(r->absolute_path, G_FILE_TEST_IS_DIR))
        r->resolved = g_strdup(r->absolute_path);
      else if(r->relative_path)
      {
        gchar *p = g_build_filename(catdir, r->relative_path, NULL);
        GFile *gf = g_file_new_for_path(p); // resolves the ".." parts
        gchar *canonical = g_file_get_path(gf);
        g_object_unref(gf);
        if(g_file_test(canonical, G_FILE_TEST_IS_DIR)) r->resolved = g_strdup(canonical);
        g_free(p);
        g_free(canonical);
      }
      g_ptr_array_add(cat->roots, r);
    }
    sqlite3_finalize(stmt);
  }
  g_free(q);
  g_free(catdir);

  for(guint k = 0; k < cat->roots->len; k++)
  {
    dt_lrcat_root_t *r = g_ptr_array_index(cat->roots, k);
    r->n_images = 0;
    gchar *cq = g_strdup_printf("SELECT COUNT(*) FROM Adobe_images i"
                                " JOIN AgLibraryFile fi ON fi.id_local = i.rootFile"
                                " JOIN AgLibraryFolder fo ON fo.id_local = fi.folder"
                                " WHERE fo.rootFolder = %" G_GINT64_FORMAT, r->id);
    r->n_images = _count(cat->db, cq);
    g_free(cq);
    _count_root(cat, r);
  }

  return cat;
}

// ---------------------------------------------------------------------------
// Lightroom develop settings (Lua table) -> XMP packet understood by
// develop/lightroom.c

typedef enum _lua_type_t
{
  LUA_T_NIL,
  LUA_T_NUMBER,
  LUA_T_STRING,
  LUA_T_BOOL,
  LUA_T_TABLE
} _lua_type_t;

typedef struct _lua_value_t
{
  _lua_type_t type;
  double number;
  gboolean boolean;
  gchar *string;
  GPtrArray *keys;   // gchar *, NULL for positional items
  GPtrArray *values; // _lua_value_t *
} _lua_value_t;

static void _lua_free(gpointer data)
{
  _lua_value_t *v = data;
  if(!v) return;
  g_free(v->string);
  if(v->keys) g_ptr_array_free(v->keys, TRUE);
  if(v->values) g_ptr_array_free(v->values, TRUE);
  g_free(v);
}

typedef struct _lua_parser_t
{
  const char *p, *end;
  int depth;
} _lua_parser_t;

static void _lua_skip(_lua_parser_t *ps)
{
  while(ps->p < ps->end)
  {
    if(g_ascii_isspace(*ps->p))
      ps->p++;
    else if(ps->p + 1 < ps->end && ps->p[0] == '-' && ps->p[1] == '-')
    {
      while(ps->p < ps->end && *ps->p != '\n') ps->p++;
    }
    else
      break;
  }
}

static _lua_value_t *_lua_value(_lua_parser_t *ps);

static gchar *_lua_string(_lua_parser_t *ps)
{
  const char quote = *ps->p++;
  GString *s = g_string_new(NULL);
  while(ps->p < ps->end && *ps->p != quote)
  {
    if(*ps->p == '\\' && ps->p + 1 < ps->end)
    {
      ps->p++;
      switch(*ps->p)
      {
        case 'n': g_string_append_c(s, '\n'); break;
        case 't': g_string_append_c(s, '\t'); break;
        case 'r': g_string_append_c(s, '\r'); break;
        case '\n': g_string_append_c(s, '\n'); break;
        default:
          if(g_ascii_isdigit(*ps->p))
          {
            int c = 0, n = 0;
            while(n < 3 && ps->p < ps->end && g_ascii_isdigit(*ps->p))
            {
              c = c * 10 + (*ps->p - '0');
              ps->p++;
              n++;
            }
            g_string_append_c(s, (char)c);
            continue;
          }
          g_string_append_c(s, *ps->p);
      }
      ps->p++;
    }
    else
      g_string_append_c(s, *ps->p++);
  }
  if(ps->p < ps->end) ps->p++; // closing quote
  return g_string_free(s, FALSE);
}

static gboolean _lua_ident(_lua_parser_t *ps, gchar **ident)
{
  const char *start = ps->p;
  if(ps->p >= ps->end || !(g_ascii_isalpha(*ps->p) || *ps->p == '_')) return FALSE;
  while(ps->p < ps->end && (g_ascii_isalnum(*ps->p) || *ps->p == '_')) ps->p++;
  *ident = g_strndup(start, ps->p - start);
  return TRUE;
}

static _lua_value_t *_lua_table(_lua_parser_t *ps)
{
  if(++ps->depth > 32) return NULL;
  ps->p++; // {
  _lua_value_t *t = g_malloc0(sizeof(_lua_value_t));
  t->type = LUA_T_TABLE;
  t->keys = g_ptr_array_new_with_free_func(g_free);
  t->values = g_ptr_array_new_with_free_func(_lua_free);

  while(TRUE)
  {
    _lua_skip(ps);
    if(ps->p >= ps->end) break;
    if(*ps->p == '}')
    {
      ps->p++;
      break;
    }

    gchar *key = NULL;
    if(*ps->p == '[')
    {
      ps->p++;
      _lua_skip(ps);
      _lua_value_t *k = _lua_value(ps);
      if(k && k->type == LUA_T_STRING) key = g_strdup(k->string);
      else if(k && k->type == LUA_T_NUMBER) key = g_strdup_printf("%g", k->number);
      _lua_free(k);
      _lua_skip(ps);
      if(ps->p < ps->end && *ps->p == ']') ps->p++;
      _lua_skip(ps);
      if(ps->p < ps->end && *ps->p == '=') ps->p++;
    }
    else
    {
      const char *save = ps->p;
      gchar *ident = NULL;
      if(_lua_ident(ps, &ident))
      {
        _lua_skip(ps);
        if(ps->p < ps->end && *ps->p == '=' && (ps->p + 1 >= ps->end || ps->p[1] != '='))
        {
          ps->p++;
          key = ident;
        }
        else
        {
          g_free(ident);
          ps->p = save;
        }
      }
    }

    _lua_skip(ps);
    _lua_value_t *v = _lua_value(ps);
    if(!v)
    {
      g_free(key);
      break;
    }
    g_ptr_array_add(t->keys, key);
    g_ptr_array_add(t->values, v);

    _lua_skip(ps);
    if(ps->p < ps->end && (*ps->p == ',' || *ps->p == ';')) ps->p++;
  }
  ps->depth--;
  return t;
}

static _lua_value_t *_lua_value(_lua_parser_t *ps)
{
  _lua_skip(ps);
  if(ps->p >= ps->end) return NULL;

  if(*ps->p == '{') return _lua_table(ps);

  _lua_value_t *v = g_malloc0(sizeof(_lua_value_t));
  if(*ps->p == '"' || *ps->p == '\'')
  {
    v->type = LUA_T_STRING;
    v->string = _lua_string(ps);
  }
  else if(g_str_has_prefix(ps->p, "true"))
  {
    v->type = LUA_T_BOOL;
    v->boolean = TRUE;
    ps->p += 4;
  }
  else if(g_str_has_prefix(ps->p, "false"))
  {
    v->type = LUA_T_BOOL;
    v->boolean = FALSE;
    ps->p += 5;
  }
  else if(g_str_has_prefix(ps->p, "nil"))
  {
    v->type = LUA_T_NIL;
    ps->p += 3;
  }
  else
  {
    char *endp = NULL;
    v->number = g_ascii_strtod(ps->p, &endp);
    if(!endp || endp == ps->p)
    {
      g_free(v);
      return NULL;
    }
    v->type = LUA_T_NUMBER;
    ps->p = endp;
  }
  return v;
}

static _lua_value_t *_lua_parse_settings(const char *text)
{
  if(!text) return NULL;
  // "s = { ... }"
  const char *brace = strchr(text, '{');
  if(!brace) return NULL;
  _lua_parser_t ps = { .p = brace, .end = text + strlen(text), .depth = 0 };
  _lua_value_t *v = _lua_table(&ps);
  if(v && v->type != LUA_T_TABLE)
  {
    _lua_free(v);
    return NULL;
  }
  return v;
}

static void _xml_escape_append(GString *s, const char *text)
{
  gchar *e = g_markup_escape_text(text, -1);
  g_string_append(s, e);
  g_free(e);
}

static void _xmp_number(GString *s, const double v)
{
  char buf[G_ASCII_DTOSTR_BUF_SIZE];
  if(v == (double)(int64_t)v)
    g_string_append_printf(s, "%" G_GINT64_FORMAT, (int64_t)v);
  else
    g_string_append(s, g_ascii_formatd(buf, sizeof(buf), "%.6g", v));
}

static int _orientation_code(const char *code)
{
  static const char *codes[] = { "AB", "BA", "CD", "DC", "AD", "BC", "CB", "DA", NULL };
  for(int k = 0; code && codes[k]; k++)
    if(!g_strcmp0(code, codes[k])) return k + 1;
  return 0;
}

// build a crs: XMP packet from the Lua develop settings
static gchar *_settings_to_xmp(const _lua_value_t *t,
                               const int orientation,
                               const int width,
                               const int height)
{
  GString *attrs = g_string_new(NULL);
  GString *elems = g_string_new(NULL);

  for(guint k = 0; k < t->values->len; k++)
  {
    const char *key = g_ptr_array_index(t->keys, k);
    const _lua_value_t *v = g_ptr_array_index(t->values, k);
    if(!key || !g_ascii_isalpha(key[0])) continue;

    switch(v->type)
    {
      case LUA_T_NUMBER:
        g_string_append_printf(attrs, "\n   crs:%s=\"", key);
        _xmp_number(attrs, v->number);
        g_string_append_c(attrs, '"');
        break;
      case LUA_T_BOOL:
        g_string_append_printf(attrs, "\n   crs:%s=\"%s\"", key, v->boolean ? "True" : "False");
        break;
      case LUA_T_STRING:
        g_string_append_printf(attrs, "\n   crs:%s=\"", key);
        _xml_escape_append(attrs, v->string);
        g_string_append_c(attrs, '"');
        break;
      case LUA_T_TABLE:
      {
        const gboolean curve = g_str_has_prefix(key, "ToneCurvePV2012");
        const gboolean retouch = !g_strcmp0(key, "RetouchInfo");
        if(!curve && !retouch) break;

        g_string_append_printf(elems, "\n   <crs:%s>\n    <rdf:Seq>", key);
        if(curve)
        {
          // flat list x0, y0, x1, y1...
          for(guint i = 0; i + 1 < v->values->len; i += 2)
          {
            const _lua_value_t *x = g_ptr_array_index(v->values, i);
            const _lua_value_t *y = g_ptr_array_index(v->values, i + 1);
            if(x->type != LUA_T_NUMBER || y->type != LUA_T_NUMBER) continue;
            g_string_append_printf(elems, "\n     <rdf:li>%d, %d</rdf:li>",
                                   (int)x->number, (int)y->number);
          }
        }
        else
        {
          for(guint i = 0; i < v->values->len; i++)
          {
            const _lua_value_t *spot = g_ptr_array_index(v->values, i);
            if(spot->type == LUA_T_STRING)
            {
              g_string_append(elems, "\n     <rdf:li>");
              _xml_escape_append(elems, spot->string);
              g_string_append(elems, "</rdf:li>");
            }
            else if(spot->type == LUA_T_TABLE)
            {
              // same text form as the XMP sidecars
              static const char *fields[] = { "centerX", "centerY", "radius", "sourceState",
                                              "sourceX", "sourceY", NULL };
              GString *li = g_string_new(NULL);
              for(int f = 0; fields[f]; f++)
                for(guint j = 0; j < spot->values->len; j++)
                  if(!g_strcmp0(g_ptr_array_index(spot->keys, j), fields[f]))
                  {
                    const _lua_value_t *fv = g_ptr_array_index(spot->values, j);
                    if(li->len) g_string_append(li, ", ");
                    g_string_append_printf(li, "%s = ", fields[f]);
                    if(fv->type == LUA_T_NUMBER)
                      _xmp_number(li, fv->number);
                    else if(fv->type == LUA_T_STRING)
                      g_string_append(li, fv->string);
                  }
              g_string_append(elems, "\n     <rdf:li>");
              _xml_escape_append(elems, li->str);
              g_string_append(elems, "</rdf:li>");
              g_string_free(li, TRUE);
            }
          }
        }
        g_string_append_printf(elems, "\n    </rdf:Seq>\n   </crs:%s>", key);
        break;
      }
      default:
        break;
    }
  }

  GString *xmp = g_string_new(
    "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
    " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
    "  <rdf:Description rdf:about=\"\"\n"
    "   xmlns:crs=\"http://ns.adobe.com/camera-raw-settings/1.0/\"\n"
    "   xmlns:tiff=\"http://ns.adobe.com/tiff/1.0/\"");
  if(orientation > 0) g_string_append_printf(xmp, "\n   tiff:Orientation=\"%d\"", orientation);
  if(width > 0) g_string_append_printf(xmp, "\n   tiff:ImageWidth=\"%d\"", width);
  if(height > 0) g_string_append_printf(xmp, "\n   tiff:ImageLength=\"%d\"", height);
  g_string_append(xmp, attrs->str);
  g_string_append(xmp, ">");
  g_string_append(xmp, elems->str);
  g_string_append(xmp, "\n  </rdf:Description>\n </rdf:RDF>\n</x:xmpmeta>\n");

  g_string_free(attrs, TRUE);
  g_string_free(elems, TRUE);
  return g_string_free(xmp, FALSE);
}

static double _lua_number(const _lua_value_t *t, const char *key, const double def)
{
  for(guint k = 0; t && k < t->values->len; k++)
    if(!g_strcmp0(g_ptr_array_index(t->keys, k), key))
    {
      const _lua_value_t *v = g_ptr_array_index(t->values, k);
      if(v->type == LUA_T_NUMBER) return v->number;
      if(v->type == LUA_T_BOOL) return v->boolean ? 1.0 : 0.0;
    }
  return def;
}

static const char *_lua_string_value(const _lua_value_t *t, const char *key)
{
  for(guint k = 0; t && k < t->values->len; k++)
    if(!g_strcmp0(g_ptr_array_index(t->keys, k), key))
    {
      const _lua_value_t *v = g_ptr_array_index(t->values, k);
      if(v->type == LUA_T_STRING) return v->string;
    }
  return NULL;
}

static gboolean _lua_has_table(const _lua_value_t *t, const char *key)
{
  for(guint k = 0; t && k < t->values->len; k++)
    if(!g_strcmp0(g_ptr_array_index(t->keys, k), key))
    {
      const _lua_value_t *v = g_ptr_array_index(t->values, k);
      return v->type == LUA_T_TABLE && v->values->len > 0;
    }
  return FALSE;
}

// does the image carry develop changes darktable can reproduce? Lightroom
// default values (e.g. the standard sharpening) are left to darktable.
static gboolean _settings_are_edited(const _lua_value_t *t, const int lr_orientation,
                                     const dt_image_orientation_t image_orientation)
{
  static const char *zero_keys[] =
  {
    "Exposure2012", "Contrast2012", "Highlights2012", "Shadows2012", "Whites2012",
    "Blacks2012", "Texture", "Clarity2012", "Dehaze", "Vibrance", "Saturation",
    "ParametricShadows", "ParametricDarks", "ParametricLights", "ParametricHighlights",
    "PostCropVignetteAmount", "GrainAmount", "LuminanceSmoothing",
    "SplitToningShadowSaturation", "SplitToningHighlightSaturation",
    "ColorGradeMidtoneSat", "ColorGradeGlobalSat", "ColorGradeShadowLum",
    "ColorGradeMidtoneLum", "ColorGradeHighlightLum", "ColorGradeGlobalLum",
    NULL
  };
  for(int k = 0; zero_keys[k]; k++)
    if(_lua_number(t, zero_keys[k], 0.0) != 0.0) return TRUE;

  static const char *colors[] = { "Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta", NULL };
  for(int k = 0; colors[k]; k++)
  {
    char key[64];
    snprintf(key, sizeof(key), "HueAdjustment%s", colors[k]);
    if(_lua_number(t, key, 0.0) != 0.0) return TRUE;
    snprintf(key, sizeof(key), "SaturationAdjustment%s", colors[k]);
    if(_lua_number(t, key, 0.0) != 0.0) return TRUE;
    snprintf(key, sizeof(key), "LuminanceAdjustment%s", colors[k]);
    if(_lua_number(t, key, 0.0) != 0.0) return TRUE;
  }

  const char *wb = _lua_string_value(t, "WhiteBalance");
  if(wb && g_strcmp0(wb, "As Shot")) return TRUE;
  if(_lua_number(t, "HasCrop", 0.0) != 0.0) return TRUE;
  if(_lua_number(t, "CropAngle", 0.0) != 0.0) return TRUE;
  if(_lua_number(t, "ConvertToGrayscale", 0.0) != 0.0) return TRUE;
  if(_lua_number(t, "LensProfileEnable", 0.0) != 0.0) return TRUE;
  if(_lua_number(t, "AutoLateralCA", 0.0) != 0.0) return TRUE;
  if(_lua_number(t, "Sharpness", 40.0) != 40.0) return TRUE;
  if(_lua_has_table(t, "RetouchInfo")) return TRUE;
  const char *curve = _lua_string_value(t, "ToneCurveName2012");
  if(curve && g_strcmp0(curve, "Linear")) return TRUE;
  if(lr_orientation > 0
     && dt_image_orientation_to_flip_bits(lr_orientation) != image_orientation)
    return TRUE;
  return FALSE;
}

// ---------------------------------------------------------------------------
// keywords & collections

static gchar *_hierarchy_path(GHashTable *names, GHashTable *parents, const int64_t id, int depth)
{
  const char *name = g_hash_table_lookup(names, &id);
  if(!name || depth > 64) return NULL;
  const int64_t *parent = g_hash_table_lookup(parents, &id);
  gchar *up = parent ? _hierarchy_path(names, parents, *parent, depth + 1) : NULL;
  gchar *path = up ? g_strdup_printf("%s|%s", up, name) : g_strdup(name);
  g_free(up);
  return path;
}

static int64_t *_id_new(const int64_t id)
{
  int64_t *p = g_new(int64_t, 1);
  *p = id;
  return p;
}

// image id -> GPtrArray of tag paths
static GHashTable *_load_image_tags(sqlite3 *db, const gboolean keywords, const gboolean collections)
{
  GHashTable *image_tags = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free,
                                                 (GDestroyNotify)g_ptr_array_unref);
  sqlite3_stmt *stmt;

  if(keywords && _has_table(db, "AgLibraryKeyword") && _has_table(db, "AgLibraryKeywordImage"))
  {
    GHashTable *names = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    GHashTable *parents = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    if(sqlite3_prepare_v2(db, "SELECT id_local, name, parent FROM AgLibraryKeyword",
                          -1, &stmt, NULL) == SQLITE_OK)
    {
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        const int64_t id = sqlite3_column_int64(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        if(name && *name)
        {
          // '|' is the darktable hierarchy separator
          gchar *clean = g_strdelimit(g_strdup(name), "|", '/');
          g_hash_table_insert(names, _id_new(id), clean);
        }
        if(sqlite3_column_type(stmt, 2) != SQLITE_NULL)
          g_hash_table_insert(parents, _id_new(id), _id_new(sqlite3_column_int64(stmt, 2)));
      }
      sqlite3_finalize(stmt);
    }
    if(sqlite3_prepare_v2(db, "SELECT image, tag FROM AgLibraryKeywordImage",
                          -1, &stmt, NULL) == SQLITE_OK)
    {
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        const int64_t image = sqlite3_column_int64(stmt, 0);
        gchar *path = _hierarchy_path(names, parents, sqlite3_column_int64(stmt, 1), 0);
        if(!path) continue;
        GPtrArray *tags = g_hash_table_lookup(image_tags, &image);
        if(!tags)
        {
          tags = g_ptr_array_new_with_free_func(g_free);
          g_hash_table_insert(image_tags, _id_new(image), tags);
        }
        g_ptr_array_add(tags, path);
      }
      sqlite3_finalize(stmt);
    }
    g_hash_table_destroy(names);
    g_hash_table_destroy(parents);
  }

  if(collections && _has_table(db, "AgLibraryCollection") && _has_table(db, "AgLibraryCollectionImage"))
  {
    GHashTable *names = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    GHashTable *parents = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
    if(sqlite3_prepare_v2(db, "SELECT id_local, name, parent, creationId FROM AgLibraryCollection",
                          -1, &stmt, NULL) == SQLITE_OK)
    {
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        const int64_t id = sqlite3_column_int64(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        const char *creation = (const char *)sqlite3_column_text(stmt, 3);
        // regular collections, collection sets and the quick collection
        if(!name || !*name
           || !(g_strcmp0(creation, "com.adobe.ag.library.collection") == 0
                || g_strcmp0(creation, "com.adobe.ag.library.group") == 0))
          continue;
        gchar *clean = g_strdelimit(g_strdup(name), "|", '/');
        g_hash_table_insert(names, _id_new(id), clean);
        if(sqlite3_column_type(stmt, 2) != SQLITE_NULL)
          g_hash_table_insert(parents, _id_new(id), _id_new(sqlite3_column_int64(stmt, 2)));
      }
      sqlite3_finalize(stmt);
    }
    if(sqlite3_prepare_v2(db, "SELECT image, collection FROM AgLibraryCollectionImage",
                          -1, &stmt, NULL) == SQLITE_OK)
    {
      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        const int64_t image = sqlite3_column_int64(stmt, 0);
        gchar *path = _hierarchy_path(names, parents, sqlite3_column_int64(stmt, 1), 0);
        if(!path) continue;
        GPtrArray *tags = g_hash_table_lookup(image_tags, &image);
        if(!tags)
        {
          tags = g_ptr_array_new_with_free_func(g_free);
          g_hash_table_insert(image_tags, _id_new(image), tags);
        }
        g_ptr_array_add(tags, g_strdup_printf("%s|%s", LR_COLLECTIONS_TAG, path));
        g_free(path);
      }
      sqlite3_finalize(stmt);
    }
    g_hash_table_destroy(names);
    g_hash_table_destroy(parents);
  }

  return image_tags;
}

static void _attach_tag(const char *name, const dt_imgid_t imgid, GHashTable *tag_cache)
{
  guint tagid = GPOINTER_TO_UINT(g_hash_table_lookup(tag_cache, name));
  if(!tagid)
  {
    if(!dt_tag_new(name, &tagid) && !tagid) return;
    // collection tags are for organizing, keep them out of exported keywords
    if(g_str_has_prefix(name, LR_COLLECTIONS_TAG "|"))
      dt_tag_set_flags(tagid, DT_TF_CATEGORY | DT_TF_PRIVATE);
    g_hash_table_insert(tag_cache, g_strdup(name), GUINT_TO_POINTER(tagid));
  }
  dt_tag_attach(tagid, imgid, FALSE, FALSE);
}

// ---------------------------------------------------------------------------
// import

typedef struct _lr_image_t
{
  int64_t id, master;
  gchar *copy_name;
  int64_t root;
  gchar *from_root, *base, *ext;
  double rating;
  gboolean has_rating;
  int pick;
  gchar *label;
  gchar *orientation;
  int width, height;
} _lr_image_t;

static void _lr_image_free(gpointer data)
{
  _lr_image_t *i = data;
  g_free(i->copy_name);
  g_free(i->from_root);
  g_free(i->base);
  g_free(i->ext);
  g_free(i->label);
  g_free(i->orientation);
  g_free(i);
}

static gchar *_query_text(sqlite3 *db, const char *query, const int64_t id, const int col, size_t *len)
{
  sqlite3_stmt *stmt;
  gchar *res = NULL;
  if(sqlite3_prepare_v2(db, query, -1, &stmt, NULL) == SQLITE_OK)
  {
    sqlite3_bind_int64(stmt, 1, id);
    if(sqlite3_step(stmt) == SQLITE_ROW) res = _column_text(stmt, col, len);
    sqlite3_finalize(stmt);
  }
  return res;
}

static void _apply_develop(const dt_imgid_t imgid, const char *xmp, const size_t len)
{
  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_dev_load_image(&dev, imgid);
  dt_lightroom_import_xmp_buffer(imgid, &dev, xmp, len,
                                 DT_LR_IMPORT_DEVELOP | DT_LR_IMPORT_FROM_CATALOG);
  dt_dev_cleanup(&dev);

  dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);
  guint tagid = 0;
  if(dt_tag_new("darktable|changed", &tagid) || tagid)
    dt_tag_attach(tagid, imgid, FALSE, FALSE);
  dt_image_cache_set_change_timestamp(imgid);
  dt_mipmap_cache_remove(imgid);
  dt_image_update_final_size(imgid);
}

static int _history_count(const dt_imgid_t imgid)
{
  sqlite3_stmt *stmt;
  int n = 0;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT COUNT(*) FROM main.history WHERE imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if(sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

// duplicate of master already carrying the name of the virtual copy (e.g.
// re-created from darktable sidecars when importing on another computer)
static dt_imgid_t _find_duplicate(const dt_imgid_t master, const char *name, GHashTable *used)
{
  sqlite3_stmt *stmt;
  dt_imgid_t found = NO_IMGID;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT i.id, IFNULL(m.value, '')"
                              " FROM main.images i"
                              " JOIN main.images mi ON mi.id = ?1"
                              " LEFT JOIN main.meta_data m ON m.id = i.id AND m.key = ?2"
                              " WHERE i.film_id = mi.film_id AND i.filename = mi.filename"
                              "   AND i.id != mi.id"
                              " ORDER BY i.version",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, master);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, dt_metadata_get_keyid("Xmp.darktable.version_name"));
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const dt_imgid_t id = sqlite3_column_int(stmt, 0);
    const char *vname = (const char *)sqlite3_column_text(stmt, 1);
    if(g_hash_table_contains(used, GINT_TO_POINTER(id))) continue;
    if(!g_strcmp0(vname, name ? name : ""))
    {
      found = id;
      break;
    }
  }
  sqlite3_finalize(stmt);
  return found;
}

// a virtual copy gets its own labels and keywords, not the master's
static void _clear_inherited(const dt_imgid_t imgid)
{
  dt_colorlabels_remove_all_labels(imgid);
  GList *tags = NULL;
  if(dt_tag_get_attached(imgid, &tags, TRUE))
    for(GList *t = tags; t; t = g_list_next(t))
      dt_tag_detach(((dt_tag_t *)t->data)->id, imgid, FALSE, FALSE);
  dt_tag_free_result(&tags);
  guint pick = 0;
  if(dt_tag_exists(LR_PICK_TAG, &pick)) dt_tag_detach(pick, imgid, FALSE, FALSE);
}

int dt_lrcat_import_run(dt_lrcat_t *cat, const dt_lrcat_options_t *options, dt_job_t *job)
{
  sqlite3 *db = cat->db;
  const gboolean has_master = _has_column(db, "Adobe_images", "masterImage");
  const gboolean has_copyname = _has_column(db, "Adobe_images", "copyName");
  const gboolean has_dims = _has_column(db, "Adobe_images", "fileWidth");
  const gboolean has_dev_cache = _has_column(db, "Adobe_images", "developSettingsIDCache");

  // all images, masters before their virtual copies
  gchar *q = g_strdup_printf(
    "SELECT i.id_local, %s, %s, i.rating, i.pick, i.colorLabels, fo.rootFolder,"
    "       fo.pathFromRoot, fi.baseName, fi.extension, i.orientation, %s, %s"
    " FROM Adobe_images i"
    " JOIN AgLibraryFile fi ON fi.id_local = i.rootFile"
    " JOIN AgLibraryFolder fo ON fo.id_local = fi.folder"
    " ORDER BY %s, fo.rootFolder, fo.pathFromRoot, fi.baseName, i.id_local",
    has_master ? "i.masterImage" : "NULL",
    has_copyname ? "i.copyName" : "NULL",
    has_dims ? "i.fileWidth" : "0",
    has_dims ? "i.fileHeight" : "0",
    has_master ? "(i.masterImage IS NOT NULL)" : "0");

  GPtrArray *images = g_ptr_array_new_with_free_func(_lr_image_free);
  sqlite3_stmt *stmt;
  if(sqlite3_prepare_v2(db, q, -1, &stmt, NULL) == SQLITE_OK)
  {
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      _lr_image_t *i = g_malloc0(sizeof(_lr_image_t));
      i->id = sqlite3_column_int64(stmt, 0);
      i->master = sqlite3_column_type(stmt, 1) == SQLITE_NULL ? 0 : sqlite3_column_int64(stmt, 1);
      i->copy_name = g_strdup((const char *)sqlite3_column_text(stmt, 2));
      i->has_rating = sqlite3_column_type(stmt, 3) != SQLITE_NULL;
      i->rating = sqlite3_column_double(stmt, 3);
      i->pick = (int)lround(sqlite3_column_double(stmt, 4));
      i->label = g_strdup((const char *)sqlite3_column_text(stmt, 5));
      i->root = sqlite3_column_int64(stmt, 6);
      i->from_root = g_strdup((const char *)sqlite3_column_text(stmt, 7));
      i->base = g_strdup((const char *)sqlite3_column_text(stmt, 8));
      i->ext = g_strdup((const char *)sqlite3_column_text(stmt, 9));
      i->orientation = g_strdup((const char *)sqlite3_column_text(stmt, 10));
      i->width = (int)sqlite3_column_double(stmt, 11);
      i->height = (int)sqlite3_column_double(stmt, 12);
      g_ptr_array_add(images, i);
    }
    sqlite3_finalize(stmt);
  }
  g_free(q);

  GHashTable *image_tags = _load_image_tags(db, options->keywords, options->collections);
  GHashTable *tag_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GHashTable *film_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GHashTable *lr2dt = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
  GHashTable *used_duplicates = g_hash_table_new(g_direct_hash, g_direct_equal);
  // images whose darktable edit (from a sidecar) must be kept
  GHashTable *keep_develop = g_hash_table_new(g_direct_hash, g_direct_equal);

  const gboolean has_iptc = options->metadata && _has_table(db, "AgLibraryIPTC");
  const gboolean has_exif = options->metadata && _has_table(db, "AgHarvestedExifMetadata");
  const gboolean has_additional = _has_table(db, "Adobe_AdditionalMetadata");
  const gboolean has_settings = _has_table(db, "Adobe_imageDevelopSettings");

  gchar *settings_query = has_dev_cache
    ? g_strdup("SELECT s.text FROM Adobe_images i"
               " JOIN Adobe_imageDevelopSettings s ON s.id_local = i.developSettingsIDCache"
               " WHERE i.id_local = ?1")
    : g_strdup("SELECT text FROM Adobe_imageDevelopSettings WHERE image = ?1 ORDER BY id_local DESC");

  int imported = 0, missing = 0, developed = 0, existing = 0, copies = 0;
  guint pick_tag = 0;
  if(options->metadata) dt_tag_new(LR_PICK_TAG, &pick_tag);

  GList *new_images = NULL;
  const guint total = images->len;
  double last_update = 0.0;

  for(guint n = 0; n < total; n++)
  {
    if(job && dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED) break;

    const double now = dt_get_wtime();
    if(job && now - last_update > 0.5)
    {
      last_update = now;
      dt_control_job_set_progress(job, (double)n / (double)MAX(total, 1));
      dt_control_job_set_progress_message(job, _("importing Lightroom catalog: %u/%u"), n, total);
    }

    _lr_image_t *li = g_ptr_array_index(images, n);
    dt_imgid_t imgid = NO_IMGID;

    if(li->master)
    {
      // virtual copy -> darktable duplicate of the imported master
      const dt_imgid_t *master = g_hash_table_lookup(lr2dt, &li->master);
      if(!master) continue;
      imgid = _find_duplicate(*master, li->copy_name, used_duplicates);
      if(dt_is_valid_imgid(imgid))
      {
        if(_history_count(imgid) > 0)
          g_hash_table_add(keep_develop, GINT_TO_POINTER(imgid));
      }
      else
      {
        imgid = dt_image_duplicate(*master);
        if(!dt_is_valid_imgid(imgid)) continue;
        dt_history_delete_on_image_ext(imgid, FALSE, FALSE);
        if(li->copy_name && *li->copy_name)
          dt_metadata_set(imgid, "Xmp.darktable.version_name", li->copy_name, FALSE);
      }
      g_hash_table_add(used_duplicates, GINT_TO_POINTER(imgid));
      _clear_inherited(imgid);
      copies++;
    }
    else
    {
      dt_lrcat_root_t *root = _find_root(cat, li->root);
      gchar *path = root ? _image_path(root, li->from_root, li->base, li->ext) : NULL;
      if(!path || !g_file_test(path, G_FILE_TEST_IS_REGULAR))
      {
        missing++;
        g_free(path);
        continue;
      }

      // do not touch images already in the library (e.g. a second import)
      const dt_imgid_t known = dt_image_get_id_full_path(path);
      if(dt_is_valid_imgid(known))
      {
        existing++;
        dt_imgid_t *v = g_new(dt_imgid_t, 1);
        *v = known;
        g_hash_table_insert(lr2dt, _id_new(li->id), v);
        g_free(path);
        continue;
      }

      gchar *dir = g_path_get_dirname(path);
      dt_filmid_t filmid = GPOINTER_TO_INT(g_hash_table_lookup(film_cache, dir));
      if(!filmid)
      {
        dt_film_t film;
        dt_film_init(&film);
        filmid = dt_film_new(&film, dir);
        dt_film_cleanup(&film);
        g_hash_table_insert(film_cache, g_strdup(dir), GINT_TO_POINTER(filmid));
      }
      g_free(dir);

      imgid = dt_image_import(filmid, path, TRUE, FALSE);
      g_free(path);
      if(!dt_is_valid_imgid(imgid)) continue;
      new_images = g_list_prepend(new_images, GINT_TO_POINTER(imgid));
      // darktable sidecar found (image already edited with Tonelark/darktable)
      if(_history_count(imgid) > 0)
        g_hash_table_add(keep_develop, GINT_TO_POINTER(imgid));
    }

    dt_imgid_t *v = g_new(dt_imgid_t, 1);
    *v = imgid;
    g_hash_table_insert(lr2dt, _id_new(li->id), v);
    imported++;

    // ratings, flags, labels
    if(options->metadata)
    {
      if(li->pick < 0)
      {
        // rejecting an already rejected image would toggle the flag off
        if(dt_ratings_get(imgid) != DT_VIEW_REJECT)
          dt_ratings_apply_on_image(imgid, DT_VIEW_REJECT, FALSE, FALSE, FALSE);
      }
      else if(li->has_rating)
        dt_ratings_apply_on_image(imgid, CLAMP((int)lround(li->rating), 0, 5), FALSE, FALSE, FALSE);
      else
        dt_ratings_apply_on_image(imgid, 0, FALSE, FALSE, FALSE);
      if(li->pick > 0 && pick_tag) dt_tag_attach(pick_tag, imgid, FALSE, FALSE);

      if(li->label && *li->label)
      {
        const int color = dt_lightroom_color_label(li->label);
        if(color >= 0)
          dt_colorlabels_set_label(imgid, color);
        else
        {
          gchar *t = g_strdup_printf("Lightroom|label|%s", li->label);
          _attach_tag(t, imgid, tag_cache);
          g_free(t);
        }
      }
    }

    // keywords and collections
    GPtrArray *tags = g_hash_table_lookup(image_tags, &li->id);
    for(guint k = 0; tags && k < tags->len; k++)
      _attach_tag(g_ptr_array_index(tags, k), imgid, tag_cache);

    // xmp packet: title, creator etc. and develop settings for old catalogs
    size_t xmp_len = 0;
    gchar *xmp = has_additional
      ? _query_text(db, "SELECT xmp FROM Adobe_AdditionalMetadata WHERE image = ?1", li->id, 0, &xmp_len)
      : NULL;

    if(options->metadata)
    {
      if(xmp)
        dt_lightroom_import_xmp_buffer(imgid, NULL, xmp, xmp_len,
                                       DT_LR_IMPORT_METADATA | DT_LR_IMPORT_FROM_CATALOG);
      if(has_iptc)
      {
        gchar *caption = _query_text(db, "SELECT caption FROM AgLibraryIPTC WHERE image = ?1",
                                     li->id, 0, NULL);
        gchar *rights = _query_text(db, "SELECT copyright FROM AgLibraryIPTC WHERE image = ?1",
                                    li->id, 0, NULL);
        if(caption && *caption) dt_metadata_set(imgid, "Xmp.dc.description", caption, FALSE);
        if(rights && *rights) dt_metadata_set(imgid, "Xmp.dc.rights", rights, FALSE);
        g_free(caption);
        g_free(rights);
      }
      if(has_exif
         && sqlite3_prepare_v2(db, "SELECT hasGPS, gpsLatitude, gpsLongitude"
                                   " FROM AgHarvestedExifMetadata WHERE image = ?1",
                               -1, &stmt, NULL) == SQLITE_OK)
      {
        sqlite3_bind_int64(stmt, 1, li->id);
        if(sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_double(stmt, 0) > 0.0
           && sqlite3_column_type(stmt, 1) != SQLITE_NULL)
        {
          const dt_image_geoloc_t geoloc = { .latitude = sqlite3_column_double(stmt, 1),
                                             .longitude = sqlite3_column_double(stmt, 2),
                                             .elevation = NAN };
          dt_image_set_location(imgid, &geoloc, FALSE, FALSE);
        }
        sqlite3_finalize(stmt);
      }
    }

    // develop settings
    if(options->develop && !g_hash_table_contains(keep_develop, GINT_TO_POINTER(imgid)))
    {
      gchar *text = has_settings ? _query_text(db, settings_query, li->id, 0, NULL) : NULL;
      _lua_value_t *settings = _lua_parse_settings(text);

      const dt_image_t *img = dt_image_cache_get(imgid, 'r');
      const dt_image_orientation_t image_orientation = img ? dt_image_orientation(img) : ORIENTATION_NONE;
      dt_image_cache_read_release(img);
      const int lr_orientation = _orientation_code(li->orientation);

      gchar *develop_xmp = NULL;
      gboolean edited = FALSE;
      if(settings)
      {
        edited = _settings_are_edited(settings, lr_orientation, image_orientation);
        if(edited)
          develop_xmp = _settings_to_xmp(settings, lr_orientation, li->width, li->height);
      }
      else if(xmp && strstr(xmp, "crs:"))
      {
        // no readable settings table, use the packet (sidecar format)
        edited = TRUE;
        develop_xmp = g_strdup(xmp);
      }

      if(edited && develop_xmp)
      {
        _apply_develop(imgid, develop_xmp, strlen(develop_xmp));
        developed++;
      }
      g_free(develop_xmp);
      _lua_free(settings);
      g_free(text);
    }
    g_free(xmp);

    dt_image_synch_xmp(imgid);
  }

  // stacks -> groups
  if(options->stacks && _has_table(db, "AgLibraryFolderStackImage")
     && sqlite3_prepare_v2(db, "SELECT stack, image FROM AgLibraryFolderStackImage"
                               " ORDER BY stack, position",
                           -1, &stmt, NULL) == SQLITE_OK)
  {
    int64_t current = -1;
    dt_imgid_t leader = NO_IMGID;
    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int64_t stack = sqlite3_column_int64(stmt, 0);
      const int64_t image = sqlite3_column_int64(stmt, 1);
      const dt_imgid_t *imgid = g_hash_table_lookup(lr2dt, &image);
      if(stack != current)
      {
        current = stack;
        leader = imgid ? *imgid : NO_IMGID;
      }
      else if(imgid && dt_is_valid_imgid(leader))
        dt_grouping_add_to_group(leader, *imgid);
    }
    sqlite3_finalize(stmt);
  }

  g_free(settings_query);
  g_hash_table_destroy(image_tags);
  g_hash_table_destroy(tag_cache);
  g_hash_table_destroy(film_cache);
  g_hash_table_destroy(lr2dt);
  g_hash_table_destroy(used_duplicates);
  g_hash_table_destroy(keep_develop);
  g_ptr_array_free(images, TRUE);

  dt_control_log(_("Lightroom catalog: %d images imported (%d virtual copies, %d with develop "
                   "settings), %d already in the library, %d files not found"),
                 imported - copies, copies, developed, existing, missing);
  dt_print(DT_DEBUG_ALWAYS,
           "[lightroom catalog] imported %d, virtual copies %d, developed %d, existing %d, missing %d",
           imported - copies, copies, developed, existing, missing);

  if(new_images)
  {
    DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_GEOTAG_CHANGED, g_list_copy(new_images), 0);
    g_list_free(new_images);
  }
  return imported;
}

// ---------------------------------------------------------------------------
// job

typedef struct _lrcat_job_t
{
  dt_lrcat_t *cat;
  dt_lrcat_options_t options;
  gboolean *wait;
} _lrcat_job_t;

static int32_t _lrcat_job_run(dt_job_t *job)
{
  _lrcat_job_t *p = dt_control_job_get_params(job);
  dt_control_job_set_progress_message(job, _("importing Lightroom catalog"));
  dt_lrcat_import_run(p->cat, &p->options, job);

  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_TAG_CHANGED);
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_FILMROLLS_CHANGED);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD,
                             DT_COLLECTION_PROP_UNDEF, NULL);
  dt_control_queue_redraw_center();
  if(p->wait) *p->wait = FALSE;
  return 0;
}

static void _lrcat_job_cleanup(void *data)
{
  _lrcat_job_t *p = data;
  dt_lrcat_close(p->cat);
  g_free(p);
}

void dt_lrcat_import(dt_lrcat_t *cat, const dt_lrcat_options_t *options, const gboolean wait)
{
  dt_job_t *job = dt_control_job_create(&_lrcat_job_run, "import Lightroom catalog");
  if(!job)
  {
    dt_lrcat_close(cat);
    return;
  }
  _lrcat_job_t *p = g_malloc0(sizeof(_lrcat_job_t));
  p->cat = cat;
  p->options = *options;
  gboolean waiting = wait;
  p->wait = wait ? &waiting : NULL;
  dt_control_job_add_progress(job, _("import Lightroom catalog"), TRUE);
  dt_control_job_set_params(job, p, _lrcat_job_cleanup);
  dt_control_add_job(DT_JOB_QUEUE_USER_BG, job);

  // used by scripts and tests, as the import dialog does
  while(waiting) g_usleep(100000);
}

// ---------------------------------------------------------------------------
// the import dialog: summary, new location of the photo folders, options

typedef struct _lrcat_dialog_t
{
  dt_lrcat_t *cat;
  GtkWidget *found_label;
  GPtrArray *root_labels;
} _lrcat_dialog_t;

static void _lrcat_update_found(_lrcat_dialog_t *dd)
{
  const int found = dt_lrcat_count_found(dd->cat);
  const int masters = dd->cat->n_images - dd->cat->n_virtual_copies;
  gchar *txt = g_strdup_printf(_("%d of %d photos found on this computer"), found, masters);
  gtk_label_set_text(GTK_LABEL(dd->found_label), txt);
  g_free(txt);

  for(guint k = 0; k < dd->cat->roots->len; k++)
  {
    dt_lrcat_root_t *r = g_ptr_array_index(dd->cat->roots, k);
    GtkWidget *l = g_ptr_array_index(dd->root_labels, k);
    gchar *t = g_strdup_printf(_("%d / %d found"), r->n_found, r->n_images);
    gtk_label_set_text(GTK_LABEL(l), t);
    g_free(t);
  }
}

static void _lrcat_root_folder_set(GtkFileChooserButton *button, _lrcat_dialog_t *dd)
{
  dt_lrcat_root_t *r = g_object_get_data(G_OBJECT(button), "lrcat-root");
  gchar *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(button));
  if(r && folder)
  {
    dt_lrcat_set_root_folder(dd->cat, r, folder);
    _lrcat_update_found(dd);
  }
  g_free(folder);
}

static GtkWidget *_lrcat_check(const char *label, const char *conf)
{
  GtkWidget *w = gtk_check_button_new_with_label(label);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w),
                               dt_conf_key_exists(conf) ? dt_conf_get_bool(conf) : TRUE);
  g_object_set_data(G_OBJECT(w), "lrcat-conf", (gpointer)conf);
  return w;
}

static gboolean _lrcat_check_get(GtkWidget *w)
{
  const gboolean active = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
  dt_conf_set_bool(g_object_get_data(G_OBJECT(w), "lrcat-conf"), active);
  return active;
}

void dt_lrcat_import_interactive(const char *filename)
{
  GtkWindow *win = GTK_WINDOW(dt_ui_main_window(darktable.gui->ui));

  dt_control_log(_("reading Lightroom catalog..."));
  dt_gui_process_events();

  GError *error = NULL;
  dt_lrcat_t *cat = dt_lrcat_open(filename, &error);
  if(!cat)
  {
    dt_control_log("%s", error ? error->message : _("cannot read the Lightroom catalog"));
    g_clear_error(&error);
    return;
  }

  _lrcat_dialog_t dd = { .cat = cat, .root_labels = g_ptr_array_new() };

  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("Import Lightroom Catalog"), win,
                                                  GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                  _("_cancel"), GTK_RESPONSE_CANCEL,
                                                  _("_import"), GTK_RESPONSE_ACCEPT, NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_container_set_border_width(GTK_CONTAINER(area), DT_PIXEL_APPLY_DPI(10));
  gtk_box_set_spacing(GTK_BOX(area), DT_PIXEL_APPLY_DPI(6));

  gchar *base = g_path_get_basename(cat->filename);
  gchar *summary = g_strdup_printf(_("<b>%s</b>\n%d photos, %d virtual copies, %d keywords, %d collections"),
                                   base, cat->n_images - cat->n_virtual_copies,
                                   cat->n_virtual_copies, cat->n_keywords, cat->n_collections);
  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), summary);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  g_free(summary);
  g_free(base);
  gtk_box_pack_start(GTK_BOX(area), label, FALSE, FALSE, 0);

  GtkWidget *hint = gtk_label_new(_("photos stay where they are. the catalog is only read, never changed.\n"
                                    "if photos were moved (another drive or computer), choose their new folder."));
  gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
  gtk_box_pack_start(GTK_BOX(area), hint, FALSE, FALSE, 0);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(10));
  gtk_grid_set_row_spacing(GTK_GRID(grid), DT_PIXEL_APPLY_DPI(4));
  for(guint k = 0; k < cat->roots->len; k++)
  {
    dt_lrcat_root_t *r = g_ptr_array_index(cat->roots, k);
    GtkWidget *name = gtk_label_new(r->absolute_path);
    gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_START);
    gtk_label_set_max_width_chars(GTK_LABEL(name), 45);
    gtk_widget_set_tooltip_text(name, r->absolute_path);

    GtkWidget *count = gtk_label_new("");
    g_ptr_array_add(dd.root_labels, count);

    GtkWidget *button = gtk_file_chooser_button_new(_("select the folder"),
                                                    GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    if(r->resolved)
      gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(button), r->resolved);
    g_object_set_data(G_OBJECT(button), "lrcat-root", r);
    g_signal_connect(button, "file-set", G_CALLBACK(_lrcat_root_folder_set), &dd);
    gtk_widget_set_tooltip_text(button, _("folder containing these photos on this computer"));

    gtk_grid_attach(GTK_GRID(grid), name, 0, k, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), count, 1, k, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), button, 2, k, 1, 1);
  }
  gtk_box_pack_start(GTK_BOX(area), grid, FALSE, FALSE, 0);

  dd.found_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(dd.found_label), 0.0f);
  gtk_box_pack_start(GTK_BOX(area), dd.found_label, FALSE, FALSE, 0);

  GtkWidget *develop = _lrcat_check(_("develop settings (exposure, white balance, crop, curves...)"),
                                    "plugins/lighttable/lrcat/develop");
  GtkWidget *metadata = _lrcat_check(_("ratings, flags, color labels, titles, captions and locations"),
                                     "plugins/lighttable/lrcat/metadata");
  GtkWidget *keywords = _lrcat_check(_("keywords as tags"), "plugins/lighttable/lrcat/keywords");
  GtkWidget *collections = _lrcat_check(_("collections as tags (under \"Lightroom collections\")"),
                                        "plugins/lighttable/lrcat/collections");
  GtkWidget *stacks = _lrcat_check(_("stacks as groups"), "plugins/lighttable/lrcat/stacks");
  gtk_box_pack_start(GTK_BOX(area), develop, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(area), metadata, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(area), keywords, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(area), collections, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(area), stacks, FALSE, FALSE, 0);

  _lrcat_update_found(&dd);
  gtk_widget_show_all(dialog);

  const int res = gtk_dialog_run(GTK_DIALOG(dialog));
  const dt_lrcat_options_t options = { .develop = _lrcat_check_get(develop),
                                       .metadata = _lrcat_check_get(metadata),
                                       .keywords = _lrcat_check_get(keywords),
                                       .collections = _lrcat_check_get(collections),
                                       .stacks = _lrcat_check_get(stacks) };
  gtk_widget_destroy(dialog);
  g_ptr_array_free(dd.root_labels, TRUE);

  if(res == GTK_RESPONSE_ACCEPT && dt_lrcat_count_found(cat) > 0)
    dt_lrcat_import(cat, &options, FALSE);  // takes ownership
  else
  {
    if(res == GTK_RESPONSE_ACCEPT) dt_control_log(_("no photo of the catalog was found"));
    dt_lrcat_close(cat);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
