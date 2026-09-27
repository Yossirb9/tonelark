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
  Lightroom-style "basic" panel for the darkroom.

  Every slider drives a parameter of an existing darktable processing
  module (see develop/lightspeed.c for the mapping). Moving a slider
  enables the module if needed and records a normal history item, so the
  full module stays available for fine tuning and undo works as usual.
*/

#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/lightspeed.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_basicpanel_t
{
  GtkWidget *slider[DT_LSB_COUNT];
  GtkWidget *color_btn, *bw_btn;
  GtkWidget *asshot_btn, *picker_btn;
} dt_lib_basicpanel_t;

const char *name(dt_lib_module_t *self)
{
  return _("basic");
}

const char *description(dt_lib_module_t *self)
{
  return _("Lightroom-style basic adjustments.\n"
           "each slider drives a darktable processing module,\n"
           "use the full module for fine tuning");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_DARKROOM;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_RIGHT_CENTER;
}

int position(const dt_lib_module_t *self)
{
  return 1;
}

static GtkWidget *_find_widget(dt_iop_module_t *m, const char *action_id)
{
  if(!m) return NULL;
  for(GSList *w = m->widget_list; w; w = g_slist_next(w))
  {
    const dt_action_target_t *referral = w->data;
    if(referral->action && !g_strcmp0(referral->action->id, action_id))
      return referral->target;
  }
  return NULL;
}

static dt_iop_module_t *_get_module(const dt_lsb_control_t *c, const gboolean create)
{
  dt_develop_t *dev = darktable.develop;
  dt_iop_module_t *m = dt_lsb_find_module(dev->iop, c);
  if(m || !create || !c->instance) return m;

  // dedicated instance (e.g. texture) does not exist yet, create it
  dt_iop_module_t *base = dt_iop_get_module_from_list(dev->iop, c->op);
  if(!base) return NULL;

  m = dt_iop_gui_duplicate(base, FALSE);
  if(!m) return NULL;

  g_strlcpy(m->multi_name, c->instance, sizeof(m->multi_name));
  m->multi_name_hand_edited = TRUE;
  dt_lsb_neutralize_params(m, c, m->params);
  dt_iop_gui_update(m);
  dt_iop_gui_update_header(m);
  dt_iop_gui_set_expanded(m, FALSE, FALSE);
  return m;
}

static void _basic_commit(dt_iop_module_t *m, GtkWidget *target, const gboolean force_on)
{
  const gboolean enable = force_on || !dt_lsb_params_are_neutral(m, m->params);
  if(!enable) m->enabled = FALSE;

  dt_iop_gui_update(m);
  dt_dev_add_history_item_target(darktable.develop, m, enable, target);
}

static void _slider_changed(GtkWidget *w, dt_lib_module_t *self)
{
  DT_GUARD_GUI_UPDATE();

  dt_lib_basicpanel_t *d = self->data;
  int id = -1;
  for(int i = 0; i < DT_LSB_COUNT; i++)
    if(d->slider[i] == w) id = i;
  if(id < 0) return;

  const dt_lsb_control_t *c = dt_lsb_control(id);
  const float value = dt_bauhaus_slider_get(w);
  dt_iop_module_t *m = _get_module(c, value != 0.0f);
  if(!m) return;

  if(id == DT_LSB_TEMP || id == DT_LSB_TINT)
  {
    const float temp = dt_bauhaus_slider_get(d->slider[DT_LSB_TEMP]);
    const float tint = dt_bauhaus_slider_get(d->slider[DT_LSB_TINT]);
    if(!m->enabled) dt_lsb_neutralize_params(m, c, m->params);
    dt_lsb_wb_write_params(m, m->params, temp, tint);
    _basic_commit(m, w, TRUE);
    return;
  }

  if(!m->enabled)
  {
    // nothing to do when a disabled module is set to neutral
    if(value == 0.0f) return;
    dt_lsb_neutralize_params(m, c, m->params);
  }
  dt_lsb_write_params(m, c, m->params, value);
  _basic_commit(m, w, c->neutral_from_defaults);
}

static void _set_slider(GtkWidget *w, const float v)
{
  if(fabsf(dt_bauhaus_slider_get(w) - v) > 1e-4f)
    dt_bauhaus_slider_set(w, v);
}

static void _sync(dt_lib_module_t *self)
{
  dt_lib_basicpanel_t *d = self->data;
  dt_develop_t *dev = darktable.develop;
  if(!d || !dev || !dev->iop) return;

  DT_ENTER_GUI_UPDATE();

  for(int i = 0; i < DT_LSB_COUNT; i++)
  {
    const dt_lsb_control_t *c = dt_lsb_control(i);
    dt_iop_module_t *m = dt_lsb_find_module(dev->iop, c);
    const float v = dt_lsb_read(m, c);
    if(!isfinite(v)) continue;
    if(i == DT_LSB_TEMP || i == DT_LSB_TINT)
    {
      // double click resets to the image default (as shot)
      float temp = 5003.0f, tint = 0.0f;
      if(m)
      {
        const gboolean en = m->enabled;
        m->enabled = FALSE;
        dt_lsb_wb_read(m, &temp, &tint);
        m->enabled = en;
      }
      dt_bauhaus_slider_set_default(d->slider[i], i == DT_LSB_TEMP ? temp : tint);
    }
    _set_slider(d->slider[i], v);
  }

  // treatment: black & white through the color calibration gray channel
  dt_iop_module_t *cc = dt_lsb_find_module(dev->iop, dt_lsb_control(DT_LSB_TEMP));
  gboolean bw = FALSE;
  if(cc && cc->enabled && cc->so->get_p)
  {
    const float *grey = cc->so->get_p(cc->params, "grey");
    bw = grey && (grey[0] != 0.f || grey[1] != 0.f || grey[2] != 0.f);
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bw_btn), bw);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->color_btn), !bw);

  DT_LEAVE_GUI_UPDATE();
}

static void _treatment_toggled(GtkToggleButton *btn, dt_lib_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_lib_basicpanel_t *d = self->data;

  // behave like radio buttons: clicking the active one does nothing
  if(!gtk_toggle_button_get_active(btn))
  {
    _sync(self);
    return;
  }
  const gboolean bw = GTK_WIDGET(btn) == d->bw_btn;

  const dt_lsb_control_t *c = dt_lsb_control(DT_LSB_TEMP);
  dt_iop_module_t *m = _get_module(c, TRUE);
  if(!m || !m->so->get_p)
  {
    _sync(self);
    return;
  }

  if(!m->enabled) dt_lsb_neutralize_params(m, c, m->params);
  float *grey = m->so->get_p(m->params, "grey");
  gboolean *normalize = m->so->get_p(m->params, "normalize_grey");
  if(grey)
  {
    grey[0] = 0.f;
    grey[1] = bw ? 1.f : 0.f;
    grey[2] = 0.f;
  }
  if(normalize) *normalize = TRUE;
  _basic_commit(m, GTK_WIDGET(btn), TRUE);
  _sync(self);
}

static void _bw_action(dt_action_t *action)
{
  dt_lib_basicpanel_t *d = dt_action_lib(action)->data;
  const gboolean bw = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->bw_btn));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bw ? d->color_btn : d->bw_btn), TRUE);
}

static void _asshot_clicked(GtkButton *button, dt_lib_module_t *self)
{
  const dt_lsb_control_t *c = dt_lsb_control(DT_LSB_TEMP);
  dt_iop_module_t *m = _get_module(c, TRUE);
  if(!m) return;

  // back to the default (as shot) illuminant, keep the other settings
  void *def = g_malloc(m->params_size);
  dt_lsb_neutralize_params(m, c, def);
  if(m->enabled && m->so->get_p)
  {
    static const char *fields[] = { "illuminant", "illum_fluo", "illum_led",
                                    "adaptation", "x", "y", "temperature", NULL };
    for(int i = 0; fields[i]; i++)
    {
      void *dst = m->so->get_p(m->params, fields[i]);
      const void *src = m->so->get_p(def, fields[i]);
      if(dst && src) memcpy(dst, src, sizeof(float));
    }
  }
  else
    memcpy(m->params, def, m->params_size);
  g_free(def);

  _basic_commit(m, GTK_WIDGET(button), TRUE);
  _sync(self);
}

static void _picker_clicked(GtkButton *button, dt_lib_module_t *self)
{
  const dt_lsb_control_t *c = dt_lsb_control(DT_LSB_TEMP);
  dt_iop_module_t *m = _get_module(c, TRUE);
  GtkWidget *picker = _find_widget(m, "picker");
  if(!picker || !GTK_IS_TOGGLE_BUTTON(picker)) return;

  if(!m->enabled)
  {
    dt_lsb_neutralize_params(m, c, m->params);
    _basic_commit(m, GTK_WIDGET(button), TRUE);
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(picker),
                               !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(picker)));
}

static void _history_changed(gpointer instance, dt_lib_module_t *self)
{
  _sync(self);
}

void gui_reset(dt_lib_module_t *self)
{
  dt_develop_t *dev = darktable.develop;

  for(int i = 0; i < DT_LSB_COUNT; i++)
  {
    const dt_lsb_control_t *c = dt_lsb_control(i);
    dt_iop_module_t *m = dt_lsb_find_module(dev->iop, c);
    if(!m || !m->enabled || i == DT_LSB_TEMP || i == DT_LSB_TINT) continue;
    if(fabsf(dt_lsb_read(m, c)) < 1e-4f) continue;
    dt_lsb_write_params(m, c, m->params, 0.0f);
    _basic_commit(m, NULL, c->neutral_from_defaults);
  }
  _sync(self);
}

static GtkWidget *_section(const char *label)
{
  GtkWidget *l = dt_ui_section_label_new(label);
  gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
  gtk_widget_set_name(l, "basicpanel-section");
  return l;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_basicpanel_t *d = g_malloc0(sizeof(dt_lib_basicpanel_t));
  self->data = d;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(self->widget, "basicpanel");

  // treatment
  d->color_btn = gtk_toggle_button_new_with_label(_("color"));
  d->bw_btn = gtk_toggle_button_new_with_label(_("black & white"));
  gtk_widget_set_tooltip_text(d->bw_btn, _("convert to black & white with the color calibration module"));
  g_signal_connect(d->color_btn, "toggled", G_CALLBACK(_treatment_toggled), self);
  g_signal_connect(d->bw_btn, "toggled", G_CALLBACK(_treatment_toggled), self);
  GtkWidget *treatment = gtk_label_new(_("treatment"));
  gtk_widget_set_name(treatment, "basicpanel-label");
  GtkWidget *tbox = dt_gui_hbox(treatment, dt_gui_expand(d->color_btn), dt_gui_expand(d->bw_btn));
  gtk_widget_set_name(tbox, "basicpanel-treatment");
  dt_gui_box_add(self->widget, tbox);

  dt_lsb_section_t section = -1;
  for(int i = 0; i < DT_LSB_COUNT; i++)
  {
    const dt_lsb_control_t *c = dt_lsb_control(i);

    if(c->section != section)
    {
      section = c->section;
      if(section == DT_LSB_SECTION_WB)
      {
        d->asshot_btn = dt_action_button_new(self, N_("as shot"), _asshot_clicked, self,
                                             _("reset white balance to the camera setting"), 0, 0);
        d->picker_btn = dt_action_button_new(self, N_("pick"), _picker_clicked, self,
                                             _("set white balance from an area of the image"), 0, 0);
        GtkWidget *label = _section(C_("section", "white balance"));
        dt_gui_box_add(self->widget, dt_gui_hbox(dt_gui_expand(label), d->asshot_btn, d->picker_btn));
      }
      else if(section == DT_LSB_SECTION_TONE)
        dt_gui_box_add(self->widget, _section(C_("section", "tone")));
      else
        dt_gui_box_add(self->widget, _section(C_("section", "presence")));
    }

    const float def = i == DT_LSB_TEMP ? 5003.0f : 0.0f;
    const float step = i == DT_LSB_TEMP ? 50.0f : (c->digits > 0 ? 0.05f : 1.0f);
    GtkWidget *w = dt_bauhaus_slider_new_action(DT_ACTION(self), c->hard_min, c->hard_max,
                                                step, def, c->digits);
    dt_bauhaus_widget_set_label(w, NULL, c->label);
    dt_bauhaus_slider_set_soft_range(w, c->soft_min, c->soft_max);
    gtk_widget_set_tooltip_text(w, _(c->tooltip));

    if(i == DT_LSB_TEMP)
    {
      dt_bauhaus_slider_set_format(w, " K");
      dt_bauhaus_slider_set_digits(w, 0);
      dt_bauhaus_slider_set_log_curve(w);
      dt_bauhaus_slider_set_stop(w, 0.0f, 0.25f, 0.45f, 1.0f);
      dt_bauhaus_slider_set_stop(w, 0.5f, 0.85f, 0.85f, 0.85f);
      dt_bauhaus_slider_set_stop(w, 1.0f, 1.0f, 0.8f, 0.15f);
    }
    else if(i == DT_LSB_TINT)
    {
      dt_bauhaus_slider_set_stop(w, 0.0f, 0.3f, 0.85f, 0.3f);
      dt_bauhaus_slider_set_stop(w, 0.5f, 0.85f, 0.85f, 0.85f);
      dt_bauhaus_slider_set_stop(w, 1.0f, 0.9f, 0.3f, 0.9f);
    }
    else if(i == DT_LSB_EXPOSURE)
      dt_bauhaus_slider_set_format(w, " EV");

    g_signal_connect(G_OBJECT(w), "value-changed", G_CALLBACK(_slider_changed), self);
    d->slider[i] = w;
    dt_gui_box_add(self->widget, w);
  }

  // Lightroom: V toggles black & white
  dt_action_register(DT_ACTION(self), N_("black & white"), _bw_action, GDK_KEY_v, 0);

  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_HISTORY_CHANGE, _history_changed);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _history_changed);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_INITIALIZE, _history_changed);
}

void gui_cleanup(dt_lib_module_t *self)
{
  g_free(self->data);
  self->data = NULL;
}

void view_enter(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  // keep the panel on top of the processing modules, see dt_dev_reorder_gui_module_list()
  GtkWidget *w = self->expander ? self->expander : self->widget;
  g_object_set_data(G_OBJECT(w), "dt-pin-top", GINT_TO_POINTER(TRUE));
  _sync(self);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
