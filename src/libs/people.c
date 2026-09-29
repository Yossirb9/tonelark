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
  People (Library, left panel): the photos by person, like the People view of
  Lightroom. All of it on this computer.

  Find People looks for the faces of the photos of the collection (the AI
  helper: YuNet finds the faces, SFace gives each an embedding of 128 numbers;
  the faces of one person have close embeddings). A face joins the person it
  is closest to, and the faces of no one are grouped into new people; two
  unnamed people too close to be two are merged.

  The panel shows the people of the collection: a click shows the photos with
  that person (a rule on its tag added to the collection, "All photos" takes
  it away). A name tags the photos ("people|name": searched, filtered and
  exported like any keyword); the same name given to two people merges them.
  "Not this person" takes the selected photos out of a person, for good, and
  a person can be hidden. New imports are searched by themselves.

  The tables, in the library:
    ls_faces       a face: photo, box (0..1), embedding, thumbnail, person
    ls_people      a person: name (none: unnamed), hidden
    ls_faces_not   a face that is not a person (told by the user)
    ls_faces_done  the photos searched, with faces or not
  Every photo of a person has the tag darktable|tonelark|person|<id>, which
  the collection rule of a person uses.
*/

#include "bauhaus/bauhaus.h"
#include "common/act_on.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/database.h"
#include "common/image.h"
#include "common/lightspeed_ai.h"
#include "common/people.h"
#include "common/tags.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/signal.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/collect.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

#include <glib/gstdio.h>
#include <math.h>

DT_MODULE(1)

#define CONF "plugins/lighttable/people/"
#define PERSON_TAG "darktable|tonelark|person|"
#define CHUNK 60              // photos per run of the helper
#define TILE 48
#define GRID_ROWS 3           // rows of faces seen at once, the others scroll
#define MAX_TILES 300

typedef struct dt_lib_people_t
{
  GtkWidget *find, *auto_new, *status, *all, *grid, *grid_wrap, *actions, *name_btn, *not_btn, *hide_btn, *head;
  int shown;                  // the person shown, 0 none
  gchar *saved;               // the collection before a person was shown
  gchar *ours;                // the collection with the person rule, as set here
  gboolean busy;
  gboolean setting;           // the collection being set here: its signal is ours
  GList *imported;            // new imports to search
  guint import_timer, refresh_idle;
  gchar *sig;                 // what the grid shows
} dt_lib_people_t;

const char *name(dt_lib_module_t *self)
{
  return _("people");
}

const char *description(dt_lib_module_t *self)
{
  return _("the photos by person: find the faces, name the people,\n"
           "click a person to see their photos. on this computer");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 1005;                // under the collections
}

// ---------------------------------------------------------------------------
// the list of the photos of the collection a person is shown from

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

// ---------------------------------------------------------------------------
// Find People: the faces of the photos not searched yet

typedef struct _scan_t
{
  GList *imgs;
  dt_lib_module_t *self;
  int searched, faces;
  gchar *error;
  GHashTable *changed;
} _scan_t;

static void _scan_free(void *data)
{
  _scan_t *s = data;
  g_list_free(s->imgs);
  g_free(s->error);
  if(s->changed) g_hash_table_destroy(s->changed);
  g_free(s);
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

static void _refresh(dt_lib_module_t *self);

static gboolean _scan_done(gpointer data)
{
  _scan_t *s = data;
  dt_lib_module_t *self = s->self;
  dt_lib_people_t *d = self->data;
  d->busy = FALSE;
  gtk_widget_set_sensitive(d->find, TRUE);
  if(s->changed) dt_people_sync(s->changed);
  gchar *msg = s->error ? g_strdup(s->error)
             : g_strdup_printf(ngettext("%d photo searched, %d face found", "%d photos searched, %d faces found",
                                        s->searched), s->searched, s->faces);
  gtk_label_set_text(GTK_LABEL(d->status), msg);
  dt_control_log("%s", msg);
  g_free(msg);
  _scan_free(s);
  _refresh(self);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_TAG, NULL);
  return G_SOURCE_REMOVE;
}

static int32_t _scan_run(dt_job_t *job)
{
  _scan_t *s = dt_control_job_get_params(job);
  const int n = g_list_length(s->imgs);
  GList *l = s->imgs;
  while(l && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED)
  {
    gchar *dir = dt_lsai_tmpdir();
    JsonArray *arr = json_array_new();
    for(int c = 0; l && c < CHUNK; c++, l = g_list_next(l))
    {
      const dt_imgid_t id = GPOINTER_TO_INT(l->data);
      dt_control_job_set_progress_message(job, _("people: preparing %d/%d"), s->searched + c + 1, n);
      gchar *path = dt_people_face_file(id, dir);
      if(path)
      {
        JsonObject *o = json_object_new();
        json_object_set_int_member(o, "id", id);
        json_object_set_string_member(o, "path", path);
        json_array_add_object_element(arr, o);
      }
      g_free(path);
    }
    const int chunk = json_array_get_length(arr);
    JsonObject *req = json_object_new();
    json_object_set_array_member(req, "images", arr);
    GError *error = NULL;
    JsonObject *res = dt_lsai_run("faces", req, job, &error);
    json_object_unref(req);
    _rm_dir(dir);
    g_free(dir);
    if(!res)
    {
      s->error = g_strdup(error ? error->message : _("the AI helper failed"));
      g_clear_error(&error);
      break;
    }
    dt_people_store(res);
    JsonArray *images = json_object_get_array_member(res, "images");
    for(guint k = 0; images && k < json_array_get_length(images); k++)
    {
      JsonArray *faces = json_object_get_array_member(json_array_get_object_element(images, k), "faces");
      s->faces += faces ? json_array_get_length(faces) : 0;
    }
    json_object_unref(res);
    s->searched += chunk;
    dt_control_job_set_progress(job, (double)s->searched / MAX(1, n));
  }
  dt_control_job_set_progress_message(job, _("people: grouping the faces"));
  s->changed = dt_people_cluster();
  g_idle_add(_scan_done, s);
  return 0;
}

// the photos given that were not searched yet
static void _scan(dt_lib_module_t *self, GList *imgs, const gboolean quiet)
{
  dt_lib_people_t *d = self->data;
  GList *todo = NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(_db(), "SELECT 1 FROM main.ls_faces_done WHERE imgid = ?1", -1, &stmt, NULL);
  for(GList *l = imgs; l; l = g_list_next(l))
  {
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, GPOINTER_TO_INT(l->data));
    if(sqlite3_step(stmt) != SQLITE_ROW) todo = g_list_prepend(todo, l->data);
    sqlite3_reset(stmt);
  }
  sqlite3_finalize(stmt);
  if(!todo)
  {
    if(!quiet)
    {
      gtk_label_set_text(GTK_LABEL(d->status), _("these photos were searched already"));
      // the grouping again, with what was told since
      GHashTable *changed = dt_people_cluster();
      dt_people_sync(changed);
      g_hash_table_destroy(changed);
      _refresh(self);
    }
    return;
  }
  if(d->busy)
  {
    // after the current search
    d->imported = g_list_concat(d->imported, todo);
    return;
  }
  d->busy = TRUE;
  gtk_widget_set_sensitive(d->find, FALSE);
  _scan_t *s = g_malloc0(sizeof(_scan_t));
  s->imgs = g_list_reverse(todo);
  s->self = self;
  dt_job_t *job = dt_control_job_create(_scan_run, "%s", _("people"));
  if(!job)
  {
    d->busy = FALSE;
    gtk_widget_set_sensitive(d->find, TRUE);
    _scan_free(s);
    return;
  }
  dt_control_job_add_progress(job, _("finding the people"), TRUE);
  dt_control_job_set_params(job, s, NULL);
  gtk_label_set_text(GTK_LABEL(d->status), _("finding the people..."));
  dt_control_add_job(DT_JOB_QUEUE_USER_BG, job);
}

static void _find_clicked(GtkButton *b, dt_lib_module_t *self)
{
  GList *imgs = dt_collection_get_all(darktable.collection, -1);
  if(!imgs)
  {
    dt_control_log(_("no photos in the collection"));
    return;
  }
  _scan(self, imgs, FALSE);
  g_list_free(imgs);
}

// ---------------------------------------------------------------------------
// showing the photos of a person: a rule on its tag added to the collection

static void _set_collection(dt_lib_module_t *self, const char *serialized)
{
  dt_lib_people_t *d = self->data;
  // the collection changed signal comes during the deserialize, before ours is known
  d->setting = TRUE;
  dt_collection_deserialize(serialized, FALSE);
  d->setting = FALSE;
  char buf[4096];
  dt_collection_serialize(buf, sizeof(buf), FALSE);
  g_free(d->ours);
  d->ours = g_strdup(buf);
}

static void _show(dt_lib_module_t *self, const int person)
{
  dt_lib_people_t *d = self->data;
  if(person == d->shown) return;
  if(!d->shown)
  {
    // the collection to come back to, and its photos for the list of people
    char buf[4096];
    dt_collection_serialize(buf, sizeof(buf), FALSE);
    g_free(d->saved);
    d->saved = g_strdup(buf);
    _exec("DELETE FROM memory.ls_people_base");
    _exec("INSERT INTO memory.ls_people_base (imgid) SELECT imgid FROM memory.collected_images");
  }
  d->shown = person;
  if(person)
  {
    const int rules = atoi(d->saved);
    const char *rest = strchr(d->saved, ':');
    gchar *coll = g_strdup_printf("%d:%s%d:%d:%s%d$", rules + 1, rest ? rest + 1 : "", DT_LIB_COLLECT_MODE_AND,
                                  DT_COLLECTION_PROP_TAG, PERSON_TAG, person);
    _set_collection(self, coll);
    g_free(coll);
  }
  else
  {
    d->setting = TRUE;
    dt_collection_deserialize(d->saved, FALSE);
    d->setting = FALSE;
    g_free(d->saved);
    g_free(d->ours);
    d->saved = d->ours = NULL;
    _exec("DELETE FROM memory.ls_people_base");
  }
  _refresh(self);
}

static void _all_clicked(GtkButton *b, dt_lib_module_t *self)
{
  _show(self, 0);
}

// ---------------------------------------------------------------------------
// naming, "not this person", hiding

static void _rename(dt_lib_module_t *self, const int person, const char *new_name)
{
  dt_lib_people_t *d = self->data;
  const int into = dt_people_rename(person, new_name);
  if(into)
  {
    // the photos of the merged person, on the same collection
    if(d->shown == person) _show(self, into);
    gchar *name = dt_people_name(into);
    gchar *msg = g_strdup_printf(_("the same person as %s: merged"), name ? name : "");
    dt_control_log("%s", msg);
    g_free(msg);
    g_free(name);
  }
  _refresh(self);
}

static void _name_clicked(GtkButton *b, dt_lib_module_t *self)
{
  dt_lib_people_t *d = self->data;
  if(d->shown <= 0) return;
  gchar *old = dt_people_name(d->shown);
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("name the person"), GTK_WINDOW(win),
                                                  GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                  _("_cancel"), GTK_RESPONSE_CANCEL, _("_save"),
                                                  GTK_RESPONSE_ACCEPT, NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(entry), old ? old : "");
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), _("name"));
  // the names given already: the same one merges the two people
  GtkListStore *names = gtk_list_store_new(1, G_TYPE_STRING);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(_db(), "SELECT DISTINCT name FROM main.ls_people WHERE name IS NOT NULL ORDER BY name",
                              -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    GtkTreeIter it;
    gtk_list_store_append(names, &it);
    gtk_list_store_set(names, &it, 0, (const char *)sqlite3_column_text(stmt, 0), -1);
  }
  sqlite3_finalize(stmt);
  GtkEntryCompletion *completion = gtk_entry_completion_new();
  gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(names));
  gtk_entry_completion_set_text_column(completion, 0);
  gtk_entry_completion_set_inline_completion(completion, TRUE);
  gtk_entry_set_completion(GTK_ENTRY(entry), completion);
  g_object_unref(completion);
  g_object_unref(names);
  GtkWidget *hint = gtk_label_new(_("a name given to another person already: the two are the same person, merged"));
  gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
  gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_box_pack_start(GTK_BOX(area), entry, FALSE, FALSE, 4);
  gtk_box_pack_start(GTK_BOX(area), hint, FALSE, FALSE, 4);
  gtk_widget_set_size_request(dialog, DT_PIXEL_APPLY_DPI(360), -1);
  gtk_widget_show_all(dialog);
  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *name = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
    gtk_widget_destroy(dialog);
    _rename(self, d->shown, name);
    g_free(name);
  }
  else
    gtk_widget_destroy(dialog);
  g_free(old);
}

static void _not_clicked(GtkButton *b, dt_lib_module_t *self)
{
  dt_lib_people_t *d = self->data;
  if(d->shown <= 0) return;
  GList *imgs = dt_act_on_get_images(FALSE, TRUE, FALSE);
  if(!imgs)
  {
    dt_control_log(_("select the photos this person is not in"));
    return;
  }
  const int n = dt_people_not(d->shown, imgs);
  g_list_free(imgs);
  gchar *msg = g_strdup_printf(ngettext("%d photo taken out of this person", "%d photos taken out of this person", n),
                               n);
  dt_control_log("%s", msg);
  g_free(msg);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_TAG, NULL);
  _refresh(self);
}

static void _hide_clicked(GtkButton *b, dt_lib_module_t *self)
{
  dt_lib_people_t *d = self->data;
  if(d->shown <= 0) return;
  dt_people_hide(d->shown, TRUE);
  dt_control_log(_("the person is hidden from the panel"));
  _show(self, 0);
}

// ---------------------------------------------------------------------------
// the panel

static void _tile_activated(GtkFlowBox *box, GtkFlowBoxChild *child, dt_lib_module_t *self)
{
  dt_lib_people_t *d = self->data;
  const int person = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(child), "person"));
  _show(self, person == d->shown ? 0 : person);
}

static gboolean _tile_pressed(GtkWidget *w, GdkEventButton *e, dt_lib_module_t *self)
{
  // a double click names the person
  if(e->button == GDK_BUTTON_PRIMARY && e->type == GDK_2BUTTON_PRESS)
  {
    dt_lib_people_t *d = self->data;
    const int person = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "person"));
    if(d->shown != person) _show(self, person);
    _name_clicked(NULL, self);
    return TRUE;
  }
  return FALSE;
}

static GtkWidget *_face_image(const void *blob, const int len)
{
  GdkPixbuf *pix = NULL;
  if(blob && len > 0)
  {
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    if(gdk_pixbuf_loader_write(loader, blob, len, NULL) && gdk_pixbuf_loader_close(loader, NULL))
    {
      GdkPixbuf *p = gdk_pixbuf_loader_get_pixbuf(loader);
      const int size = DT_PIXEL_APPLY_DPI(TILE);
      if(p) pix = gdk_pixbuf_scale_simple(p, size, size, GDK_INTERP_BILINEAR);
    }
    else
      gdk_pixbuf_loader_close(loader, NULL);
    g_object_unref(loader);
  }
  GtkWidget *img = pix ? gtk_image_new_from_pixbuf(pix) : gtk_image_new();
  if(pix) g_object_unref(pix);
  gtk_widget_set_size_request(img, DT_PIXEL_APPLY_DPI(TILE), DT_PIXEL_APPLY_DPI(TILE));
  return img;
}

static void _refresh(dt_lib_module_t *self)
{
  dt_lib_people_t *d = self->data;
  if(!d || !d->grid) return;

  // the people of the collection (of the one before a person is shown), the
  // named first, then by their number of photos; the face of each: its sharpest big face
  gboolean has_base = FALSE;
  if(d->shown)
  {
    sqlite3_stmt *b;
    DT_DEBUG_SQLITE3_PREPARE_V2(_db(), "SELECT 1 FROM memory.ls_people_base LIMIT 1", -1, &b, NULL);
    has_base = sqlite3_step(b) == SQLITE_ROW;
    sqlite3_finalize(b);
  }
  const char *base = has_base ? "memory.ls_people_base" : "memory.collected_images";
  gchar *sql = g_strdup_printf(
    "SELECT p.id, p.name, COUNT(DISTINCT f.imgid),"
    " (SELECT thumb FROM main.ls_faces WHERE person = p.id ORDER BY w * h * MIN(sharp, 400) DESC LIMIT 1)"
    " FROM main.ls_people AS p JOIN main.ls_faces AS f ON f.person = p.id"
    " WHERE p.hidden = 0 AND f.imgid IN (SELECT imgid FROM %s)"
    " GROUP BY p.id ORDER BY p.name IS NULL, p.name COLLATE NOCASE, COUNT(DISTINCT f.imgid) DESC, p.id",
    base);
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(_db(), sql, -1, &stmt, NULL);
  g_free(sql);

  GString *sig = g_string_new("");
  g_string_append_printf(sig, "%d\n", d->shown);
  GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
  typedef struct
  {
    int id, count;
    gchar *name;
    GBytes *thumb;
  } _tile_t;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    _tile_t *t = g_malloc0(sizeof(_tile_t));
    t->id = sqlite3_column_int(stmt, 0);
    t->name = sqlite3_column_text(stmt, 1) ? g_strdup((const char *)sqlite3_column_text(stmt, 1)) : NULL;
    t->count = sqlite3_column_int(stmt, 2);
    t->thumb = g_bytes_new(sqlite3_column_blob(stmt, 3), sqlite3_column_bytes(stmt, 3));
    g_ptr_array_add(rows, t);
    g_string_append_printf(sig, "%d|%s|%d|%d\n", t->id, t->name ? t->name : "", t->count,
                           (int)g_bytes_get_size(t->thumb));
  }
  sqlite3_finalize(stmt);

  if(g_strcmp0(sig->str, d->sig))
  {
    g_free(d->sig);
    d->sig = g_string_free(sig, FALSE);
    GList *children = gtk_container_get_children(GTK_CONTAINER(d->grid));
    for(GList *l = children; l; l = g_list_next(l)) gtk_widget_destroy(l->data);
    g_list_free(children);
    for(guint k = 0; k < rows->len && k < MAX_TILES; k++)
    {
      _tile_t *t = g_ptr_array_index(rows, k);
      gsize len = 0;
      const void *blob = g_bytes_get_data(t->thumb, &len);
      GtkWidget *img = _face_image(blob, len);
      // no room for "unnamed" under a face: its tooltip says it
      GtkWidget *label = gtk_label_new(t->name ? t->name : "?");
      gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
      gtk_label_set_max_width_chars(GTK_LABEL(label), 1);
      gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
      gtk_widget_set_name(label, t->name ? "people-name" : "people-unnamed");
      gchar *c = g_strdup_printf("%d", t->count);
      GtkWidget *count = gtk_label_new(c);
      g_free(c);
      gtk_widget_set_name(count, "people-count");
      // the name and the number of photos on one line, under the face
      GtkWidget *box = dt_gui_vbox(img, dt_gui_hbox(dt_gui_expand(label), count));
      gtk_widget_set_size_request(box, DT_PIXEL_APPLY_DPI(TILE), -1);
      GtkWidget *ev = gtk_event_box_new();
      gtk_container_add(GTK_CONTAINER(ev), box);
      g_object_set_data(G_OBJECT(ev), "person", GINT_TO_POINTER(t->id));
      gtk_widget_add_events(ev, GDK_BUTTON_PRESS_MASK);
      g_signal_connect(ev, "button-press-event", G_CALLBACK(_tile_pressed), self);
      gchar *tip = g_strdup_printf(ngettext("%s: %d photo\nclick: show the photos, click again: all photos\n"
                                            "double-click: name the person",
                                            "%s: %d photos\nclick: show the photos, click again: all photos\n"
                                            "double-click: name the person", t->count),
                                   t->name ? t->name : _("unnamed"), t->count);
      gtk_widget_set_tooltip_text(ev, tip);
      g_free(tip);
      GtkWidget *child = gtk_flow_box_child_new();
      gtk_container_add(GTK_CONTAINER(child), ev);
      g_object_set_data(G_OBJECT(child), "person", GINT_TO_POINTER(t->id));
      gtk_widget_set_name(child, t->id == d->shown ? "people-tile-shown" : "people-tile");
      gtk_container_add(GTK_CONTAINER(d->grid), child);
    }
    gtk_widget_show_all(d->grid);
  }
  else
    g_string_free(sig, TRUE);
  for(guint k = 0; k < rows->len; k++) g_bytes_unref(((_tile_t *)g_ptr_array_index(rows, k))->thumb);
  for(guint k = 0; k < rows->len; k++) g_free(((_tile_t *)g_ptr_array_index(rows, k))->name);

  gchar *name = d->shown > 0 ? dt_people_name(d->shown) : NULL;
  gchar *head = d->shown > 0 ? g_strdup_printf(_("the photos of %s"), name ? name : _("this person"))
              : rows->len ? g_strdup_printf(ngettext("%d person in this collection", "%d people in this collection",
                                                     rows->len), rows->len)
              : g_strdup(_("no people found in this collection yet: Find People"));
  gtk_label_set_text(GTK_LABEL(d->head), head);
  g_free(head);
  g_free(name);
  g_ptr_array_free(rows, TRUE);
  gtk_widget_set_visible(d->grid_wrap, rows->len > 0);
  gtk_widget_set_visible(d->all, d->shown > 0);
  gtk_widget_set_visible(d->actions, d->shown > 0);
}

// as many faces in a row as the panel holds: the height of the grid is the
// one of its rows then, not of a face per row
static void _grid_allocated(GtkWidget *w, GdkRectangle *a, dt_lib_module_t *self)
{
  dt_lib_people_t *d = self->data;
  const int cols = CLAMP((a->width - 4) / (DT_PIXEL_APPLY_DPI(TILE) + 6), 2, 12);
  if(cols != (int)gtk_flow_box_get_min_children_per_line(GTK_FLOW_BOX(d->grid)))
  {
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(d->grid), cols);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(d->grid), cols);
  }
}

static gboolean _refresh_idle(gpointer data)
{
  dt_lib_module_t *self = data;
  dt_lib_people_t *d = self->data;
  d->refresh_idle = 0;
  _refresh(self);
  return G_SOURCE_REMOVE;
}

static void _collection_changed(gpointer instance, dt_collection_change_t query_change,
                                dt_collection_properties_t changed_property, gpointer imgs, const int next,
                                dt_lib_module_t *self)
{
  dt_lib_people_t *d = self->data;
  // another collection chosen while a person was shown: not shown anymore
  if(d->shown && d->ours && !d->setting)
  {
    char buf[4096];
    dt_collection_serialize(buf, sizeof(buf), FALSE);
    if(g_strcmp0(buf, d->ours))
    {
      d->shown = 0;
      g_free(d->saved);
      g_free(d->ours);
      d->saved = d->ours = NULL;
      _exec("DELETE FROM memory.ls_people_base");
    }
  }
  if(!d->refresh_idle) d->refresh_idle = g_timeout_add(300, _refresh_idle, self);
}

// the people changed elsewhere (the chat tools): the panel again
static void _tags_changed(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_people_t *d = self->data;
  if(!d->refresh_idle) d->refresh_idle = g_timeout_add(300, _refresh_idle, self);
}

// new imports: searched a moment after the import
static gboolean _import_timeout(gpointer data)
{
  dt_lib_module_t *self = data;
  dt_lib_people_t *d = self->data;
  if(d->busy) return G_SOURCE_CONTINUE;
  d->import_timer = 0;
  GList *imgs = d->imported;
  d->imported = NULL;
  if(imgs) _scan(self, imgs, TRUE);
  g_list_free(imgs);
  return G_SOURCE_REMOVE;
}

// the photos of a film roll just imported, and of its folders (the imports raise no
// signal per photo)
static void _filmroll_imported(gpointer instance, const int filmid, dt_lib_module_t *self)
{
  dt_lib_people_t *d = self->data;
  if(!dt_conf_get_bool(CONF "auto")) return;
  // only once people were looked for
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(_db(), "SELECT 1 FROM main.ls_faces_done LIMIT 1", -1, &stmt, NULL);
  const gboolean used = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  if(!used) return;
  DT_DEBUG_SQLITE3_PREPARE_V2(_db(),
                              "SELECT id FROM main.images"
                              " WHERE film_id IN (SELECT id FROM main.film_rolls WHERE folder LIKE"
                              "   (SELECT folder FROM main.film_rolls WHERE id = ?1) || '%')"
                              " AND id NOT IN (SELECT imgid FROM main.ls_faces_done)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);
  while(sqlite3_step(stmt) == SQLITE_ROW)
    d->imported = g_list_append(d->imported, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  sqlite3_finalize(stmt);
  if(!d->imported) return;
  if(d->import_timer) g_source_remove(d->import_timer);
  d->import_timer = g_timeout_add_seconds(3, _import_timeout, self);
}

static void _auto_toggled(GtkToggleButton *b, dt_lib_module_t *self)
{
  dt_conf_set_bool(CONF "auto", gtk_toggle_button_get_active(b));
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_people_t *d = g_malloc0(sizeof(dt_lib_people_t));
  self->data = d;
  dt_people_init();

  d->find = dt_action_button_new(self, N_("Find People"), _find_clicked, self,
                                 _("look for the faces of the photos of the collection and group them by person,"
                                   " on this computer (the photos searched already are skipped)"),
                                 0, 0);
  d->auto_new = gtk_check_button_new_with_label(_("find people in new imports"));
  gtk_widget_set_tooltip_text(d->auto_new, _("the photos imported are searched for the known people by themselves"));
  if(!dt_conf_key_exists(CONF "auto")) dt_conf_set_bool(CONF "auto", TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->auto_new), dt_conf_get_bool(CONF "auto"));
  g_signal_connect(d->auto_new, "toggled", G_CALLBACK(_auto_toggled), self);
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(d->auto_new))), PANGO_ELLIPSIZE_END);

  d->status = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(d->status), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(d->status), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(d->status), 1);
  gtk_widget_set_name(d->status, "lsai-state");

  d->head = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(d->head), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(d->head), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(d->head), 1);
  gtk_widget_set_name(d->head, "people-head");

  d->all = dt_action_button_new(self, N_("All photos"), _all_clicked, self,
                                _("the photos of the collection again, everyone"), 0, 0);

  d->grid = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(d->grid), GTK_SELECTION_NONE);
  gtk_flow_box_set_activate_on_single_click(GTK_FLOW_BOX(d->grid), TRUE);
  gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(d->grid), TRUE);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(d->grid), 3);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(d->grid), 3);
  // the rows as high as the faces, the room left under them
  gtk_widget_set_valign(d->grid, GTK_ALIGN_START);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(d->grid), 2);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(d->grid), 2);
  g_signal_connect(d->grid, "child-activated", G_CALLBACK(_tile_activated), self);
  // as high as the faces, up to a few rows
  d->grid_wrap = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(d->grid_wrap), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(d->grid_wrap), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(d->grid_wrap),
                                             DT_PIXEL_APPLY_DPI(GRID_ROWS * (TILE + 24)));
  gtk_container_add(GTK_CONTAINER(d->grid_wrap), d->grid);
  g_signal_connect(d->grid_wrap, "size-allocate", G_CALLBACK(_grid_allocated), self);

  d->name_btn = dt_action_button_new(self, N_("Name..."), _name_clicked, self,
                                     _("the name of this person: its photos get the keyword people|name."
                                       " the name of another person merges the two"), 0, 0);
  d->not_btn = dt_action_button_new(self, N_("Not This Person"), _not_clicked, self,
                                    _("take the selected photos out of this person, for good"), 0, 0);
  d->hide_btn = dt_action_button_new(self, N_("Hide"), _hide_clicked, self,
                                     _("hide this person from the panel (strangers, faces in the background)"), 0, 0);
  d->actions = dt_gui_hbox(dt_gui_expand(d->name_btn), dt_gui_expand(d->not_btn), dt_gui_expand(d->hide_btn));

  // the faces keep their place when a person is shown: the buttons of the
  // person under them
  self->widget = dt_gui_vbox(d->find, d->auto_new, d->status, d->head, d->grid_wrap, d->all, d->actions);
  gtk_widget_show_all(self->widget);
  gtk_widget_set_no_show_all(d->all, TRUE);
  gtk_widget_set_no_show_all(d->actions, TRUE);
  gtk_widget_set_no_show_all(d->grid_wrap, TRUE);
  gtk_widget_hide(d->all);
  gtk_widget_hide(d->actions);

  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_COLLECTION_CHANGED, _collection_changed);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_FILMROLLS_IMPORTED, _filmroll_imported);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_TAG_CHANGED, _tags_changed);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_people_t *d = self->data;
  if(d->import_timer) g_source_remove(d->import_timer);
  if(d->refresh_idle) g_source_remove(d->refresh_idle);
  g_list_free(d->imported);
  g_free(d->saved);
  g_free(d->ours);
  g_free(d->sig);
  g_free(self->data);
  self->data = NULL;
}

// the collection ends with the rule of a person (Tonelark was closed while a
// person was shown): that person is shown, "All photos" takes the rule away
static void _pick_up_shown(dt_lib_module_t *self)
{
  dt_lib_people_t *d = self->data;
  if(d->shown) return;
  char buf[4096];
  dt_collection_serialize(buf, sizeof(buf), FALSE);
  const int rules = atoi(buf);
  gchar *rule = g_strdup_printf("%d:%d:%s", DT_LIB_COLLECT_MODE_AND, DT_COLLECTION_PROP_TAG, PERSON_TAG);
  char *last = rules > 1 ? g_strrstr(buf, rule) : NULL;
  if(last && last > buf && *(last - 1) == '$')
  {
    const int person = atoi(last + strlen(rule));
    if(person > 0)
    {
      *last = '\0';
      const char *rest = strchr(buf, ':');
      d->saved = g_strdup_printf("%d:%s", rules - 1, rest ? rest + 1 : "");
      dt_collection_serialize(buf, sizeof(buf), FALSE);
      d->ours = g_strdup(buf);
      d->shown = person;
      _exec("DELETE FROM memory.ls_people_base");
    }
  }
  g_free(rule);
}

void view_enter(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  dt_lib_people_t *d = self->data;
  _pick_up_shown(self);
  if(!d->refresh_idle) d->refresh_idle = g_timeout_add(300, _refresh_idle, self);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
