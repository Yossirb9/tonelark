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
  AI Edit (Develop): mark an area of the photo and describe the change, the
  image model of Codex edits it. Only the marked area is blended into the
  full resolution photo, the result is a new photo grouped with the
  original.
*/

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/image_cache.h"
#include "common/lightspeed_ai.h"
#include "common/metadata.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"

#include <glib/gstdio.h>

DT_MODULE(1)

#define NOTES_KEY "Xmp.acdsee.notes"

typedef struct dt_lib_aiedit_t
{
  GtkWidget *select, *shape, *prompt, *generate, *status;
  dt_lsai_provider_ui_t *provider;
  gboolean dragging, has_area;
  float x0, y0, x1, y1;           // area, normalized coordinates of the processed image
  dt_imgid_t area_image;
  gboolean busy;
} dt_lib_aiedit_t;

typedef struct _job_t
{
  dt_imgid_t imgid, made;
  float rect[4];
  gboolean ellipse;
  gchar *prompt, *provider, *model, *error;
  double seconds;
  dt_lib_module_t *self;
} _job_t;

const char *name(dt_lib_module_t *self)
{
  return _("AI edit");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_DARKROOM;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 1002;
}

// ---------------------------------------------------------------------------
// the area on the photo

static void _to_image(const double x, const double y, float *u, float *v)
{
  float zx = 0.0f, zy = 0.0f, scale = 1.0f;
  dt_dev_get_pointer_zoom_pos(&darktable.develop->full, x, y, &zx, &zy, &scale);
  *u = CLAMP(zx + 0.5f, 0.0f, 1.0f);
  *v = CLAMP(zy + 0.5f, 0.0f, 1.0f);
}

// normalized image coordinates to widget coordinates: the view is a scale
// and an offset, found from two points
static void _to_widget(const float u, const float v, double *x, double *y)
{
  float zx0, zy0, zx1, zy1, s;
  dt_dev_get_pointer_zoom_pos(&darktable.develop->full, 0, 0, &zx0, &zy0, &s);
  dt_dev_get_pointer_zoom_pos(&darktable.develop->full, 1000, 1000, &zx1, &zy1, &s);
  *x = (u - 0.5f - zx0) / MAX(1e-6f, zx1 - zx0) * 1000.0;
  *y = (v - 0.5f - zy0) / MAX(1e-6f, zy1 - zy0) * 1000.0;
}

static gboolean _selecting(dt_lib_aiedit_t *d)
{
  return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->select));
}

int button_pressed(dt_lib_module_t *self, const double x, const double y, const double pressure,
                   const int which, const int type, const uint32_t state)
{
  dt_lib_aiedit_t *d = self->data;
  if(!_selecting(d) || which != GDK_BUTTON_PRIMARY || type != GDK_BUTTON_PRESS) return 0;
  _to_image(x, y, &d->x0, &d->y0);
  d->x1 = d->x0;
  d->y1 = d->y0;
  d->dragging = TRUE;
  d->has_area = FALSE;
  d->area_image = darktable.develop->image_storage.id;
  dt_control_queue_redraw_center();
  return 1;
}

int mouse_moved(dt_lib_module_t *self, const double x, const double y, const double pressure,
                const int which)
{
  dt_lib_aiedit_t *d = self->data;
  if(!d->dragging) return 0;
  _to_image(x, y, &d->x1, &d->y1);
  dt_control_queue_redraw_center();
  return 1;
}

int button_released(dt_lib_module_t *self, const double x, const double y, const int which,
                    const uint32_t state)
{
  dt_lib_aiedit_t *d = self->data;
  if(!d->dragging) return 0;
  d->dragging = FALSE;
  _to_image(x, y, &d->x1, &d->y1);
  if(d->x1 < d->x0) { const float t = d->x0; d->x0 = d->x1; d->x1 = t; }
  if(d->y1 < d->y0) { const float t = d->y0; d->y0 = d->y1; d->y1 = t; }
  d->has_area = (d->x1 - d->x0) > 0.01f && (d->y1 - d->y0) > 0.01f;
  if(d->has_area)
  {
    // done: back to the usual mouse, the area stays
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->select), FALSE);
    gtk_label_set_text(GTK_LABEL(d->status), _("describe the change, then click Generate"));
  }
  dt_control_queue_redraw_center();
  return 1;
}

void gui_post_expose(dt_lib_module_t *self, cairo_t *cr, const int32_t width, const int32_t height,
                     const int32_t pointerx, const int32_t pointery)
{
  dt_lib_aiedit_t *d = self->data;
  if(!(d->dragging || d->has_area) || d->area_image != darktable.develop->image_storage.id) return;
  double ax, ay, bx, by;
  _to_widget(MIN(d->x0, d->x1), MIN(d->y0, d->y1), &ax, &ay);
  _to_widget(MAX(d->x0, d->x1), MAX(d->y0, d->y1), &bx, &by);
  const gboolean ellipse = dt_bauhaus_combobox_get(d->shape) == 1;

  cairo_save(cr);
  // dim what is left untouched
  cairo_set_fill_rule(cr, CAIRO_FILL_RULE_EVEN_ODD);
  cairo_rectangle(cr, 0, 0, width, height);
  if(ellipse)
  {
    cairo_save(cr);
    cairo_translate(cr, (ax + bx) / 2.0, (ay + by) / 2.0);
    cairo_scale(cr, MAX(1.0, (bx - ax) / 2.0), MAX(1.0, (by - ay) / 2.0));
    cairo_arc(cr, 0, 0, 1.0, 0, 2 * M_PI);
    cairo_restore(cr);
  }
  else
    cairo_rectangle(cr, ax, ay, bx - ax, by - ay);
  cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
  cairo_fill_preserve(cr);
  cairo_new_path(cr);
  // the outline
  if(ellipse)
  {
    cairo_save(cr);
    cairo_translate(cr, (ax + bx) / 2.0, (ay + by) / 2.0);
    cairo_scale(cr, MAX(1.0, (bx - ax) / 2.0), MAX(1.0, (by - ay) / 2.0));
    cairo_arc(cr, 0, 0, 1.0, 0, 2 * M_PI);
    cairo_restore(cr);
  }
  else
    cairo_rectangle(cr, ax, ay, bx - ax, by - ay);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2.0));
  cairo_set_source_rgba(cr, 1.0, 0.75, 0.2, 0.95);
  cairo_stroke(cr);
  cairo_restore(cr);
}

// ---------------------------------------------------------------------------
// generating

static void _job_free(_job_t *j)
{
  g_free(j->prompt);
  g_free(j->provider);
  g_free(j->model);
  g_free(j->error);
  g_free(j);
}

static gboolean _job_done(gpointer data)
{
  _job_t *j = data;
  dt_lib_aiedit_t *d = j->self->data;
  d->busy = FALSE;
  gtk_widget_set_sensitive(d->generate, TRUE);
  gchar *msg;
  if(j->error)
    msg = g_strdup(j->error);
  else
  {
    msg = g_strdup_printf(_("AI edit done in %.0f s: a new photo grouped with the original"), j->seconds);
    d->has_area = FALSE;
    if(dt_is_valid_imgid(j->made) && dt_view_get_current() == DT_VIEW_DARKROOM)
      DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, j->made);
  }
  gtk_label_set_text(GTK_LABEL(d->status), msg);
  gtk_widget_set_tooltip_text(d->status, msg);
  dt_control_log("%s", msg);
  g_free(msg);
  dt_control_queue_redraw_center();
  _job_free(j);
  return G_SOURCE_REMOVE;
}

static int32_t _run_job(dt_job_t *job)
{
  _job_t *j = dt_control_job_get_params(job);
  gchar *dir = dt_lsai_tmpdir();
  gchar *full = g_build_filename(dir, "photo.tif", NULL);
  GError *error = NULL;

  dt_control_job_set_progress_message(job, _("exporting the photo"));
  if(!dt_lsai_export_tiff(j->imgid, full))
    j->error = g_strdup(_("cannot export the photo"));
  else
  {
    char src[PATH_MAX] = { 0 };
    gboolean from_cache = FALSE;
    dt_image_full_path(j->imgid, src, sizeof(src), &from_cache);
    gchar *base = g_strdup(src);
    char *dot = strrchr(base, '.');
    if(dot) *dot = '\0';
    gchar *out = g_strdup_printf("%s_ai.tif", base);
    for(int v = 2; g_file_test(out, G_FILE_TEST_EXISTS); v++)
    {
      g_free(out);
      out = g_strdup_printf("%s_ai_%d.tif", base, v);
    }
    g_free(base);

    JsonObject *req = json_object_new();
    json_object_set_string_member(req, "image", full);
    JsonArray *rect = json_array_new();
    for(int k = 0; k < 4; k++) json_array_add_double_element(rect, j->rect[k]);
    json_object_set_array_member(req, "rect", rect);
    json_object_set_string_member(req, "shape", j->ellipse ? "ellipse" : "rect");
    json_object_set_string_member(req, "prompt", j->prompt);
    json_object_set_string_member(req, "provider", j->provider);
    json_object_set_string_member(req, "model", j->model ? j->model : "");
    json_object_set_string_member(req, "output", out);
    JsonObject *res = dt_lsai_run("genedit", req, job, &error);
    json_object_unref(req);
    if(res)
    {
      j->seconds = json_object_get_double_member_with_default(res, "seconds", 0.0);
      j->made = dt_lsai_import_derived(j->imgid, out);
      if(dt_is_valid_imgid(j->made))
      {
        gchar *note = g_strdup_printf(_("AI edit (%s): %s"), j->provider, j->prompt);
        dt_metadata_set(j->made, NOTES_KEY, note, FALSE);
        g_free(note);
      }
      json_object_unref(res);
    }
    else
      j->error = g_strdup(error ? error->message : _("the AI edit failed"));
    g_clear_error(&error);
    g_free(out);
  }
  g_unlink(full);
  g_rmdir(dir);
  g_free(full);
  g_free(dir);
  g_idle_add(_job_done, j);
  return 0;
}

static void _generate_clicked(GtkButton *b, dt_lib_module_t *self)
{
  dt_lib_aiedit_t *d = self->data;
  dt_develop_t *dev = darktable.develop;
  if(d->busy) return;
  if(!d->has_area || d->area_image != dev->image_storage.id)
  {
    dt_control_log(_("mark the area first: click \"Select Area\" and drag on the photo"));
    return;
  }
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(d->prompt));
  GtkTextIter a, e;
  gtk_text_buffer_get_bounds(buffer, &a, &e);
  gchar *prompt = gtk_text_buffer_get_text(buffer, &a, &e, FALSE);
  g_strstrip(prompt);
  if(!*prompt)
  {
    dt_control_log(_("describe the change, e.g. \"remove the people\" or \"a clear blue sky\""));
    g_free(prompt);
    return;
  }

  _job_t *j = g_malloc0(sizeof(_job_t));
  gchar *why = NULL;
  if(!dt_lsai_provider_ui_get(d->provider, &j->provider, &j->model, &why))
  {
    dt_control_log("%s", why);
    gtk_label_set_text(GTK_LABEL(d->status), why);
    g_free(why);
    g_free(prompt);
    _job_free(j);
    return;
  }
  j->self = self;
  j->imgid = dev->image_storage.id;
  j->prompt = prompt;
  j->rect[0] = d->x0;
  j->rect[1] = d->y0;
  j->rect[2] = d->x1;
  j->rect[3] = d->y1;
  j->ellipse = dt_bauhaus_combobox_get(d->shape) == 1;

  // the export reads the history from the library
  dt_dev_write_history(dev);

  dt_job_t *job = dt_control_job_create(_run_job, "%s", _("AI edit"));
  if(!job)
  {
    _job_free(j);
    return;
  }
  dt_control_job_add_progress(job, _("AI edit"), TRUE);
  dt_control_job_set_params(job, j, NULL);
  d->busy = TRUE;
  gtk_widget_set_sensitive(d->generate, FALSE);
  gtk_label_set_text(GTK_LABEL(d->status), _("the AI is editing the area, this can take a few minutes..."));
  dt_control_add_job(DT_JOB_QUEUE_USER_BG, job);
}

static void _select_toggled(GtkToggleButton *b, dt_lib_module_t *self)
{
  dt_lib_aiedit_t *d = self->data;
  if(gtk_toggle_button_get_active(b))
  {
    gtk_label_set_text(GTK_LABEL(d->status), _("drag on the photo to mark the area"));
    dt_control_change_cursor("cross");
  }
  else
    dt_control_change_cursor("default");
}

static void _shape_changed(GtkWidget *w, dt_lib_module_t *self)
{
  dt_conf_set_int("plugins/darkroom/aiedit/shape", dt_bauhaus_combobox_get(w));
  dt_control_queue_redraw_center();
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_aiedit_t *d = g_malloc0(sizeof(dt_lib_aiedit_t));
  self->data = d;
  d->area_image = NO_IMGID;

  d->select = gtk_toggle_button_new_with_label(_("Select Area"));
  gtk_widget_set_tooltip_text(d->select, _("drag on the photo to mark the area the AI may change"));
  g_signal_connect(d->select, "toggled", G_CALLBACK(_select_toggled), self);

  d->shape = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(d->shape, NULL, N_("shape"));
  dt_bauhaus_combobox_add(d->shape, _("rectangle"));
  dt_bauhaus_combobox_add(d->shape, _("ellipse"));
  dt_bauhaus_combobox_set(d->shape, CLAMP(dt_conf_get_int("plugins/darkroom/aiedit/shape"), 0, 1));
  g_signal_connect(d->shape, "value-changed", G_CALLBACK(_shape_changed), self);

  d->prompt = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(d->prompt), GTK_WRAP_WORD_CHAR);
  gtk_widget_set_tooltip_text(d->prompt, _("what to change in the area, e.g. \"remove the people in the"
                                           " background\", \"a dramatic sunset sky\""));
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(scroll, -1, DT_PIXEL_APPLY_DPI(64));
  gtk_container_add(GTK_CONTAINER(scroll), d->prompt);
  gtk_widget_set_name(scroll, "aiedit-prompt");

  d->provider = dt_lsai_provider_ui_new("plugins/darkroom/aiedit/provider", "codex", TRUE);
  d->generate = dt_action_button_new(self, N_("Generate"), _generate_clicked, self,
                                     _("the AI edits the marked area, the result is a new photo grouped"
                                       " with this one"), 0, 0);
  d->status = gtk_label_new(_("mark an area, describe the change"));
  gtk_label_set_xalign(GTK_LABEL(d->status), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(d->status), TRUE);
  // wrap in the width of the panel instead of widening it
  gtk_label_set_line_wrap_mode(GTK_LABEL(d->status), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(d->status), 1);
  gtk_widget_set_name(d->status, "lsai-state");

  self->widget = dt_gui_vbox(dt_gui_hbox(dt_gui_expand(d->select), dt_gui_expand(d->shape)), scroll,
                             dt_lsai_provider_ui_widget(d->provider), d->generate, d->status);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_aiedit_t *d = self->data;
  dt_lsai_provider_ui_free(d->provider);
  g_free(self->data);
  self->data = NULL;
}

void view_leave(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  dt_lib_aiedit_t *d = self->data;
  d->dragging = FALSE;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->select), FALSE);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
