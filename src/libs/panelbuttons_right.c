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
  Lightroom-style buttons at the bottom of the right panel:
    library: "Sync Settings" (develop settings of the main photo to the selection)
    develop: "Previous" (settings of the previously edited photo) and "Reset"
*/

#include "common/act_on.h"
#include "common/darktable.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/selection.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"

DT_MODULE(1)

typedef struct dt_lib_panelbuttons_right_t
{
  GtkWidget *library_box, *develop_box;
  dt_imgid_t current, previous;
} dt_lib_panelbuttons_right_t;

const char *name(dt_lib_module_t *self)
{
  return _("panel buttons");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_DARKROOM;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_BOTTOM;
}

gboolean expandable(dt_lib_module_t *self)
{
  return FALSE;
}

int position(const dt_lib_module_t *self)
{
  return 1;
}

static void _sync_settings_clicked(GtkButton *b, dt_lib_module_t *self)
{
  const dt_imgid_t source = dt_act_on_get_main_image();
  GList *imgs = dt_act_on_get_images(TRUE, TRUE, FALSE);
  if(!dt_is_valid_imgid(source) || !imgs || !imgs->next)
  {
    dt_control_log(_("select several photos, the settings of the first one are copied to the others"));
    g_list_free(imgs);
    return;
  }
  int n = 0;
  for(GList *l = imgs; l; l = g_list_next(l))
  {
    const dt_imgid_t dest = GPOINTER_TO_INT(l->data);
    if(dest == source) continue;
    dt_history_copy_and_paste_on_image(source, dest, FALSE, NULL, TRUE, TRUE, TRUE);
    dt_mipmap_cache_remove(dest);
    n++;
  }
  g_list_free(imgs);
  dt_control_log(ngettext("settings synced to %d photo", "settings synced to %d photos", n), n);
  dt_control_queue_redraw_center();
}

static void _previous_clicked(GtkButton *b, dt_lib_module_t *self)
{
  dt_lib_panelbuttons_right_t *d = self->data;
  dt_develop_t *dev = darktable.develop;
  const dt_imgid_t imgid = dev->image_storage.id;
  if(!dt_is_valid_imgid(d->previous) || d->previous == imgid)
  {
    dt_control_log(_("no previous photo to copy the settings from"));
    return;
  }

  dt_dev_write_history(dev);
  dt_dev_undo_start_record(dev);
  dt_history_copy_and_paste_on_image(d->previous, imgid, FALSE, NULL, TRUE, TRUE, TRUE);
  dt_dev_reload_history_items(dev);
  dt_dev_undo_end_record(dev);
  dt_dev_modulegroups_set(dev, dt_dev_modulegroups_get(dev));
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
  dt_control_queue_redraw_center();
}

static void _reset_clicked(GtkButton *b, dt_lib_module_t *self)
{
  dt_develop_t *dev = darktable.develop;
  const dt_imgid_t imgid = dev->image_storage.id;
  if(!dt_is_valid_imgid(imgid)) return;

  // back to the default processing, can be undone
  dt_dev_undo_start_record(dev);
  dt_history_delete_on_image_ext(imgid, FALSE, TRUE);
  dt_dev_undo_end_record(dev);
  dt_dev_modulegroups_set(dev, dt_dev_modulegroups_get(dev));
  dt_control_queue_redraw_center();
}

static void _image_changed(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_panelbuttons_right_t *d = self->data;
  const dt_imgid_t imgid = darktable.develop->image_storage.id;
  if(imgid != d->current)
  {
    if(dt_is_valid_imgid(d->current)) d->previous = d->current;
    d->current = imgid;
  }
}

static GtkWidget *_button(const char *label, const char *tooltip, GCallback cb, dt_lib_module_t *self)
{
  GtkWidget *b = gtk_button_new_with_label(label);
  gtk_widget_set_tooltip_text(b, tooltip);
  g_signal_connect_data(b, "clicked", cb, self, NULL, 0);
  return dt_gui_expand(b);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_panelbuttons_right_t *d = g_malloc0(sizeof(dt_lib_panelbuttons_right_t));
  self->data = d;
  d->current = d->previous = NO_IMGID;

  d->library_box = dt_gui_hbox(
    _button(_("Sync Settings"),
            _("copy the develop settings of the main photo to the other selected photos"),
            G_CALLBACK(_sync_settings_clicked), self));
  d->develop_box = dt_gui_hbox(
    _button(_("Previous"), _("apply the develop settings of the previously edited photo"),
            G_CALLBACK(_previous_clicked), self),
    _button(_("Reset"), _("reset the photo to the default processing (can be undone)"),
            G_CALLBACK(_reset_clicked), self));

  self->widget = dt_gui_vbox(d->library_box, d->develop_box);
  gtk_widget_set_name(self->widget, "panel-buttons");
  gtk_widget_show_all(self->widget);
  gtk_widget_set_no_show_all(d->library_box, TRUE);
  gtk_widget_set_no_show_all(d->develop_box, TRUE);

  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _image_changed);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_INITIALIZE, _image_changed);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

void view_enter(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  dt_lib_panelbuttons_right_t *d = self->data;
  const gboolean develop = new_view && new_view->view(new_view) == DT_VIEW_DARKROOM;
  gtk_widget_set_visible(d->library_box, !develop);
  gtk_widget_set_visible(d->develop_box, develop);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
