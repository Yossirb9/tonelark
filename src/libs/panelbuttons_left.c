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
  Lightroom-style buttons at the bottom of the left panel:
    library: "Import..." and "Export..."
    develop: "Copy..." and "Paste"
*/

#include "common/darktable.h"
#include "control/control.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"

DT_MODULE(1)

typedef struct dt_lib_panelbuttons_left_t
{
  GtkWidget *library_box, *develop_box;
} dt_lib_panelbuttons_left_t;

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
  return DT_UI_CONTAINER_PANEL_LEFT_BOTTOM;
}

gboolean expandable(dt_lib_module_t *self)
{
  return FALSE;
}

int position(const dt_lib_module_t *self)
{
  return 1;
}

static void _run(const char *action)
{
  dt_action_process(action, 0, NULL, NULL, 1.0f);
}

static void _import_clicked(GtkButton *b, gpointer data)
{
  _run("lib/import/add to library...");
}

static void _export_clicked(GtkButton *b, gpointer data)
{
  // like the Lightroom export dialog: show the export settings, collapsing
  // the other panels on the right, the export starts from there
  dt_lib_module_t *exp_mod = dt_lib_get_module("export");
  if(!exp_mod) return;
  if(dt_conf_get_bool("lighttable/ui/single_module"))
    for(const GList *it = darktable.lib->plugins; it; it = g_list_next(it))
    {
      dt_lib_module_t *m = it->data;
      if(m != exp_mod && m->expander
         && dt_lib_get_container(m) == dt_lib_get_container(exp_mod)
         && m->expandable(m))
        dt_lib_gui_set_expanded(m, FALSE);
    }
  dt_lib_gui_set_expanded(exp_mod, TRUE);
  dt_control_log(_("choose the export settings on the right, then click \"start export\" (Ctrl+Shift+E)"));
}

static void _copy_clicked(GtkButton *b, gpointer data)
{
  _run("views/thumbtable/copy history parts");
}

static void _paste_clicked(GtkButton *b, gpointer data)
{
  _run("views/thumbtable/paste history");
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_panelbuttons_left_t *d = g_malloc0(sizeof(dt_lib_panelbuttons_left_t));
  self->data = d;

  // action buttons: they can also get shortcuts
  d->library_box = dt_gui_hbox(
    dt_action_button_new(self, N_("Import..."), _import_clicked, NULL,
                         _("add photos to the library (Ctrl+Shift+I)"), 0, 0),
    dt_action_button_new(self, N_("Export..."), _export_clicked, NULL,
                         _("open the export settings for the selected photos"), 0, 0));
  d->develop_box = dt_gui_hbox(
    dt_action_button_new(self, N_("Copy..."), _copy_clicked, NULL,
                         _("copy develop settings, choosing which ones (Ctrl+Shift+C)"), 0, 0),
    dt_action_button_new(self, N_("Paste"), _paste_clicked, NULL,
                         _("paste the copied develop settings (Ctrl+V)"), 0, 0));

  self->widget = dt_gui_vbox(d->library_box, d->develop_box);
  gtk_widget_set_name(self->widget, "panel-buttons");
  gtk_widget_show_all(self->widget);
  gtk_widget_set_no_show_all(d->library_box, TRUE);
  gtk_widget_set_no_show_all(d->develop_box, TRUE);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

void view_enter(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  dt_lib_panelbuttons_left_t *d = self->data;
  const gboolean develop = new_view && new_view->view(new_view) == DT_VIEW_DARKROOM;
  gtk_widget_set_visible(d->library_box, !develop);
  gtk_widget_set_visible(d->develop_box, develop);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
