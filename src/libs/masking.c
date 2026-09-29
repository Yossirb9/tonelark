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
  Lightroom-style "Masking" panel of the darkroom.

  A mask is a local adjustment made of darktable modules named "Mask N":
  - an exposure instance that holds the mask (drawn shapes: brush, linear and
    radial gradients, AI object; and/or a luminance or color range) and gives
    the Exposure slider,
  - instances of color balance rgb (temp, tint, contrast, whites, blacks,
    saturation), shadows and highlights and local contrast (texture, clarity)
    created when their slider is first moved, which use the mask of the
    exposure instance as raster mask.
  Everything is normal history: undo, copy / paste of edits and the full
  darktable modules (All tools) work as usual.
*/

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/lightspeed.h"
#include "develop/masks.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "gui/gtk.h"
#include "gui/lrpanels.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#define MASK_PREFIX "Mask "
#define D2R(x) ((x) * (float)M_PI / 180.0f)
#define R2D(x) ((x) * 180.0f / (float)M_PI)

typedef enum _ms_t
{
  MS_TEMP = 0,
  MS_TINT,
  MS_EXPOSURE,
  MS_CONTRAST,
  MS_HIGHLIGHTS,
  MS_SHADOWS,
  MS_WHITES,
  MS_BLACKS,
  MS_TEXTURE,
  MS_CLARITY,
  MS_SATURATION,
  MS_COUNT
} _ms_t;

// the local adjustments of Lightroom and the basic panel control they use
// (-1: written here)
static const struct { const char *label, *tooltip; int lsb; } _ms[MS_COUNT] =
{
  { N_("Temp"),       N_("make the masked area warmer (right) or cooler (left)"), -1 },
  { N_("Tint"),       N_("make the masked area more magenta (right) or green (left)"), -1 },
  { N_("Exposure"),   N_("brighten or darken the masked area, in EV"), -1 },
  { N_("Contrast"),   N_("contrast of the masked area"), DT_LSB_CONTRAST },
  { N_("Highlights"), N_("recover (left) or brighten (right) the bright tones of the masked area"), DT_LSB_HIGHLIGHTS },
  { N_("Shadows"),    N_("brighten (right) or darken (left) the dark tones of the masked area"), DT_LSB_SHADOWS },
  { N_("Whites"),     N_("the brightest tones of the masked area"), DT_LSB_WHITES },
  { N_("Blacks"),     N_("the darkest tones of the masked area"), DT_LSB_BLACKS },
  { N_("Texture"),    N_("fine detail of the masked area (left smooths skin)"), DT_LSB_TEXTURE },
  { N_("Clarity"),    N_("local contrast of the masked area"), DT_LSB_CLARITY },
  { N_("Saturation"), N_("color intensity of the masked area"), DT_LSB_SATURATION },
};

typedef enum _pending_t
{
  PENDING_NONE = 0,
  PENDING_SUBTRACT
} _pending_t;

typedef struct dt_lib_masking_t
{
  GtkWidget *list, *empty, *selected_box, *title;
  GtkWidget *overlay, *invert, *amount;
  GtkWidget *lum_on, *lum_from, *lum_to, *lum_smooth;
  GtkWidget *col_on, *col_hue, *col_range, *col_smooth;
  GtkWidget *slider[MS_COUNT];
  char selected[64];          // multi_name of the selected mask, "" none
  _pending_t pending;
  int pending_count;          // shapes of the mask before the pending shape
  guint idle;                 // the rebuild after the history of a new image
} dt_lib_masking_t;

const char *name(dt_lib_module_t *self)
{
  return _("masking");
}

const char *description(dt_lib_module_t *self)
{
  return _("Lightroom-style masks: create a brush, a linear or radial\n"
           "gradient, a color or luminance range or an AI object mask,\n"
           "then adjust the masked area with its own sliders");
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
  return 2;
}

// ---------------------------------------------------------------------------
// the modules of the masks

static gboolean _is_mask_name(const char *name)
{
  return g_str_has_prefix(name, MASK_PREFIX);
}

static dt_iop_module_t *_find(const char *op, const char *multi_name)
{
  for(GList *l = darktable.develop->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *m = l->data;
    if(dt_iop_module_is(m, op) && !g_strcmp0(m->multi_name, multi_name)) return m;
  }
  return NULL;
}

static dt_iop_module_t *_owner(const char *mask)
{
  return (mask && *mask) ? _find("exposure", mask) : NULL;
}

// the owners of the masks of the photo, by number
static gint _mask_cmp(gconstpointer a, gconstpointer b)
{
  const dt_iop_module_t *ma = a, *mb = b;
  return atoi(ma->multi_name + strlen(MASK_PREFIX)) - atoi(mb->multi_name + strlen(MASK_PREFIX));
}

static GList *_owners(void)
{
  GList *out = NULL;
  for(GList *l = darktable.develop->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *m = l->data;
    if(dt_iop_module_is(m, "exposure") && _is_mask_name(m->multi_name) && m->iop_order != INT_MAX)
      out = g_list_prepend(out, m);
  }
  return g_list_sort(out, _mask_cmp);
}

// the modules of a mask: the owner first
static GList *_modules_of(const char *mask)
{
  GList *out = NULL;
  const size_t n = strlen(mask);
  for(GList *l = darktable.develop->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *m = l->data;
    if(!strncmp(m->multi_name, mask, n) && (m->multi_name[n] == '\0' || m->multi_name[n] == ' ')
       && m->iop_order != INT_MAX)
      out = dt_iop_module_is(m, "exposure") && m->multi_name[n] == '\0' ? g_list_prepend(out, m)
                                                                        : g_list_append(out, m);
  }
  return out;
}

static const char *_shape_name(const int type)
{
  if(type & DT_MASKS_BRUSH) return _("Brush");
  if(type & DT_MASKS_GRADIENT) return _("Linear Gradient");
  if(type & DT_MASKS_ELLIPSE) return _("Radial Gradient");
  if(type & DT_MASKS_CIRCLE) return _("Circle");
  if(type & DT_MASKS_PATH) return _("Path");
  if(type & DT_MASKS_OBJECT) return _("Object");
  return _("Shape");
}

static int _shape_count(dt_iop_module_t *owner)
{
  dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, owner->blend_params->mask_id);
  return (grp && (grp->type & DT_MASKS_GROUP)) ? g_list_length(grp->points) : 0;
}

// "Brush + Linear Gradient", "Luminance Range"...
static gchar *_kind(dt_iop_module_t *owner)
{
  const dt_develop_blend_params_t *bp = owner->blend_params;
  GString *s = g_string_new("");
  if(bp->mask_mode & DEVELOP_MASK_MASK)
  {
    dt_masks_form_t *grp = dt_masks_get_from_id(darktable.develop, bp->mask_id);
    if(grp && (grp->type & DT_MASKS_GROUP))
      for(GList *p = grp->points; p; p = g_list_next(p))
      {
        const dt_masks_point_group_t *pt = p->data;
        dt_masks_form_t *f = dt_masks_get_from_id(darktable.develop, pt->formid);
        if(!f) continue;
        const char *n = _shape_name(f->type);
        if(!strstr(s->str, n)) g_string_append_printf(s, "%s%s%s", s->len ? " + " : "",
                                                      (pt->state & DT_MASKS_STATE_DIFFERENCE) ? "− " : "", n);
      }
  }
  if(bp->mask_mode & DEVELOP_MASK_CONDITIONAL)
  {
    if(bp->blendif & (1 << DEVELOP_BLENDIF_GRAY_in))
      g_string_append_printf(s, "%s%s", s->len ? " + " : "", _("Luminance Range"));
    if(bp->blendif & (1 << DEVELOP_BLENDIF_hz_in))
      g_string_append_printf(s, "%s%s", s->len ? " + " : "", _("Color Range"));
  }
  if(!s->len) g_string_append(s, _("draw the mask on the photo"));
  return g_string_free(s, FALSE);
}

// neutral for the basic panel controls and for temp and tint (the midtones
// of color balance rgb, which the basic panel does not use)
static gboolean _neutral(const dt_iop_module_t *m)
{
  if(!dt_lsb_params_are_neutral(m, m->params)) return FALSE;
  const float *C = dt_iop_module_is(m, "colorbalancergb") ? m->so->get_p(m->params, "midtones_C") : NULL;
  return !C || *C <= 0.0f;
}

static void _mask_commit(dt_iop_module_t *m, const gboolean force_on)
{
  const gboolean enable = force_on || !_neutral(m);
  if(!enable) m->enabled = FALSE;
  dt_iop_gui_update(m);
  dt_dev_add_history_item(darktable.develop, m, enable);
}

// the owner keeps the focus while its mask is selected: its shapes are
// drawn and edited on the photo
static void _focus(dt_iop_module_t *owner)
{
  if(!owner) return;
  if(darktable.develop->gui_module != owner) dt_iop_request_focus(owner);
  if(owner->blend_params->mask_mode & DEVELOP_MASK_MASK)
    dt_masks_set_edit_mode(owner, DT_MASKS_EDIT_FULL);
}

// a module of the mask for a control, created on demand with the mask of
// the owner as raster mask
static dt_iop_module_t *_follower(const char *mask, const dt_lsb_control_t *c, const gboolean create)
{
  gchar *name = c->instance ? g_strdup_printf("%s %s", mask, c->instance) : g_strdup(mask);
  dt_iop_module_t *m = _find(c->op, name);
  dt_iop_module_t *owner = _owner(mask);
  if(!m && create && owner)
  {
    dt_iop_module_t *base = dt_iop_get_module_from_list(darktable.develop->iop, c->op);
    if(base) m = dt_iop_gui_duplicate(base, FALSE);
    if(m)
    {
      g_strlcpy(m->multi_name, name, sizeof(m->multi_name));
      m->multi_name_hand_edited = TRUE;
      dt_lsb_neutralize_params(m, c, m->params);
      dt_iop_gui_update(m);
      dt_iop_gui_update_header(m);
      dt_iop_gui_blend_use_raster(m, owner);
      dt_iop_gui_set_expanded(m, FALSE, FALSE);
      _focus(owner);
    }
  }
  g_free(name);
  return m;
}

// ---------------------------------------------------------------------------
// temp and tint: the midtones of color balance rgb, along the warm / cool and
// magenta / green directions (color balance rgb hues of Lightroom's orange and magenta)

#define WARM_HUE 67.0f
#define MAGENTA_HUE 323.0f
#define TINT_CHROMA 0.25f

static void _temp_tint_write(dt_iop_module_t *m, const float temp, const float tint)
{
  float *H = m->so->get_p(m->params, "midtones_H");
  float *C = m->so->get_p(m->params, "midtones_C");
  if(!H || !C) return;
  const float w = D2R(WARM_HUE), g = D2R(MAGENTA_HUE);
  const float x = temp / 100.0f * cosf(w) + tint / 100.0f * cosf(g);
  const float y = temp / 100.0f * sinf(w) + tint / 100.0f * sinf(g);
  *C = CLAMP(hypotf(x, y) * TINT_CHROMA, 0.0f, 1.0f);
  float h = R2D(atan2f(y, x));
  if(h < 0.0f) h += 360.0f;
  *H = h;
}

static void _temp_tint_read(const dt_iop_module_t *m, float *temp, float *tint)
{
  *temp = *tint = 0.0f;
  if(!m || !m->enabled) return;
  const float *H = m->so->get_p(m->params, "midtones_H");
  const float *C = m->so->get_p(m->params, "midtones_C");
  if(!H || !C || *C <= 0.0f) return;
  const float r = *C / TINT_CHROMA;
  const float x = r * cosf(D2R(*H)), y = r * sinf(D2R(*H));
  // solve x, y = temp * warm + tint * magenta
  const float w = D2R(WARM_HUE), g = D2R(MAGENTA_HUE);
  const float det = cosf(w) * sinf(g) - sinf(w) * cosf(g);
  if(fabsf(det) < 1e-6f) return;
  *temp = 100.0f * (x * sinf(g) - y * cosf(g)) / det;
  *tint = 100.0f * (cosf(w) * y - sinf(w) * x) / det;
}

// ---------------------------------------------------------------------------
// luminance and color ranges: the parametric mask of the owner (gray and hue
// channels of the scene-referred rgb blending)

static void _range_set(float *p, const float from, const float to, const float smooth)
{
  p[0] = CLAMP(from - smooth, 0.0f, 1.0f);
  p[1] = CLAMP(from, 0.0f, 1.0f);
  p[2] = CLAMP(to, 0.0f, 1.0f);
  p[3] = CLAMP(to + smooth, 0.0f, 1.0f);
}

// luminance percent (perceptual) to the linear gray of the blending
static float _lum_to_gray(const float percent)
{
  return powf(CLAMP(percent, 0.0f, 100.0f) / 100.0f, 2.2f);
}

static float _gray_to_lum(const float gray)
{
  return 100.0f * powf(CLAMP(gray, 0.0f, 1.0f), 1.0f / 2.2f);
}

static void _ranges_write(dt_lib_masking_t *d, dt_iop_module_t *owner)
{
  dt_develop_blend_params_t *bp = owner->blend_params;
  const int gray = DEVELOP_BLENDIF_GRAY_in, hue = DEVELOP_BLENDIF_hz_in;
  const gboolean lum = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->lum_on));
  const gboolean col = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->col_on));

  bp->blendif &= ~((1 << gray) | (1 << hue) | (1 << (gray + 16)) | (1 << (hue + 16)));
  _range_set(bp->blendif_parameters + 4 * gray, 0.0f, 1.0f, 0.0f);
  _range_set(bp->blendif_parameters + 4 * hue, 0.0f, 1.0f, 0.0f);

  if(lum)
  {
    const float from = dt_bauhaus_slider_get(d->lum_from), to = dt_bauhaus_slider_get(d->lum_to);
    const float smooth = dt_bauhaus_slider_get(d->lum_smooth) / 100.0f;
    const float lo = _lum_to_gray(MIN(from, to)), hi = _lum_to_gray(MAX(from, to));
    // the whole top end stays open for the highlights above white
    _range_set(bp->blendif_parameters + 4 * gray, lo, hi, smooth * 0.5f);
    if(MAX(from, to) >= 99.5f) bp->blendif_parameters[4 * gray + 2] = bp->blendif_parameters[4 * gray + 3] = 1.0f;
    bp->blendif |= 1 << gray;
  }
  if(col)
  {
    const float c = dt_bauhaus_slider_get(d->col_hue) / 360.0f;
    const float w = dt_bauhaus_slider_get(d->col_range) / 360.0f / 2.0f;
    const float smooth = dt_bauhaus_slider_get(d->col_smooth) / 100.0f * 0.15f;
    float lo = c - w, hi = c + w;
    if(lo < 0.0f || hi > 1.0f)
    {
      // around red: the complement of the range, with the polarity inverted
      if(lo < 0.0f) lo += 1.0f;
      if(hi > 1.0f) hi -= 1.0f;
      _range_set(bp->blendif_parameters + 4 * hue, MIN(lo, hi) + smooth, MAX(lo, hi) - smooth, smooth);
      bp->blendif |= 1 << (hue + 16);
    }
    else
      _range_set(bp->blendif_parameters + 4 * hue, lo, hi, smooth);
    bp->blendif |= 1 << hue;
  }

  if(lum || col)
  {
    if(!(bp->mask_mode & DEVELOP_MASK_CONDITIONAL))
      bp->mask_mode |= DEVELOP_MASK_ENABLED | DEVELOP_MASK_CONDITIONAL;
  }
  else if(bp->mask_mode & DEVELOP_MASK_CONDITIONAL)
  {
    bp->mask_mode &= ~DEVELOP_MASK_CONDITIONAL;
    if(!(bp->mask_mode & ~DEVELOP_MASK_ENABLED)) bp->mask_mode |= DEVELOP_MASK_MASK;
  }
  dt_iop_commit_blend_params(owner, bp);
  dt_iop_gui_update_blending(owner);
  dt_dev_add_history_item(darktable.develop, owner, TRUE);
}

// ---------------------------------------------------------------------------
// the panel

static void _rebuild(dt_lib_module_t *self);

static void _set(GtkWidget *w, const float v)
{
  if(fabsf(dt_bauhaus_slider_get(w) - v) > 1e-4f) dt_bauhaus_slider_set(w, v);
}

static void _sync_selected(dt_lib_module_t *self)
{
  dt_lib_masking_t *d = self->data;
  dt_iop_module_t *owner = _owner(d->selected);
  gtk_widget_set_visible(d->selected_box, owner != NULL);
  if(!owner) return;

  DT_ENTER_GUI_UPDATE();
  gchar *kind = _kind(owner);
  gchar *title = g_strdup_printf("%s  ·  %s", owner->multi_name, kind);
  gtk_label_set_text(GTK_LABEL(d->title), title);
  g_free(title);
  g_free(kind);

  const dt_develop_blend_params_t *bp = owner->blend_params;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->invert), bp->mask_combine & DEVELOP_COMBINE_INV);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->overlay),
                               owner->request_mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK);
  _set(d->amount, bp->opacity);

  const gboolean cond = bp->mask_mode & DEVELOP_MASK_CONDITIONAL;
  const int gray = DEVELOP_BLENDIF_GRAY_in, hue = DEVELOP_BLENDIF_hz_in;
  const gboolean lum = cond && (bp->blendif & (1 << gray));
  const gboolean col = cond && (bp->blendif & (1 << hue));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->lum_on), lum);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->col_on), col);
  if(lum)
  {
    const float *p = bp->blendif_parameters + 4 * gray;
    _set(d->lum_from, _gray_to_lum(p[1]));
    _set(d->lum_to, _gray_to_lum(p[2]));
  }
  if(col && !(bp->blendif & (1 << (hue + 16))))
  {
    const float *p = bp->blendif_parameters + 4 * hue;
    _set(d->col_hue, 180.0f * (p[1] + p[2]));
    _set(d->col_range, 360.0f * (p[2] - p[1]));
  }
  gtk_widget_set_visible(gtk_widget_get_parent(d->lum_from), lum);
  gtk_widget_set_visible(gtk_widget_get_parent(d->col_hue), col);

  for(int i = 0; i < MS_COUNT; i++)
  {
    float v = 0.0f;
    if(i == MS_EXPOSURE)
    {
      const float *e = owner->so->get_p(owner->params, "exposure");
      v = e ? *e : 0.0f;
    }
    else if(i == MS_TEMP || i == MS_TINT)
    {
      float t, n;
      _temp_tint_read(_follower(d->selected, dt_lsb_control(DT_LSB_CONTRAST), FALSE), &t, &n);
      v = i == MS_TEMP ? t : n;
    }
    else
    {
      const dt_lsb_control_t *c = dt_lsb_control(_ms[i].lsb);
      v = dt_lsb_read(_follower(d->selected, c, FALSE), c);
    }
    if(isfinite(v)) _set(d->slider[i], v);
  }
  DT_LEAVE_GUI_UPDATE();
}

static void _select(dt_lib_module_t *self, const char *mask)
{
  dt_lib_masking_t *d = self->data;
  dt_iop_module_t *old = _owner(d->selected);
  if(old && g_strcmp0(mask, d->selected))
  {
    // the overlay of the mask belongs to it
    if(old->request_mask_display)
    {
      old->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
      dt_iop_refresh_center(old);
    }
  }
  g_strlcpy(d->selected, mask ? mask : "", sizeof(d->selected));
  _focus(_owner(d->selected));
  _rebuild(self);
}

static void _row_clicked(GtkButton *b, dt_lib_module_t *self)
{
  dt_lib_masking_t *d = self->data;
  const char *mask = g_object_get_data(G_OBJECT(b), "mask");
  _select(self, !g_strcmp0(mask, d->selected) ? "" : mask);
}

static void _row_visible(GtkToggleButton *b, dt_lib_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  const char *mask = g_object_get_data(G_OBJECT(b), "mask");
  const gboolean on = gtk_toggle_button_get_active(b);
  // the owner and the modules using its mask together: without the owner
  // they would have no mask
  GList *mods = _modules_of(mask);
  for(GList *l = mods; l; l = g_list_next(l))
  {
    dt_iop_module_t *m = l->data;
    const gboolean enable = on && (l == mods || !_neutral(m));
    if(m->off && m->enabled != enable)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(m->off), enable);
  }
  g_list_free(mods);
}

static void _row_delete(GtkButton *b, dt_lib_module_t *self)
{
  dt_lib_masking_t *d = self->data;
  gchar *mask = g_strdup(g_object_get_data(G_OBJECT(b), "mask"));
  if(!g_strcmp0(mask, d->selected)) d->selected[0] = '\0';
  GList *mods = _modules_of(mask);
  // the modules using the mask first, the owner last
  for(GList *l = g_list_last(mods); l; l = g_list_previous(l))
    dt_iop_gui_delete(l->data);
  g_list_free(mods);
  g_free(mask);
  _rebuild(self);
}

static void _rebuild(dt_lib_module_t *self)
{
  dt_lib_masking_t *d = self->data;
  if(!darktable.develop || !darktable.develop->gui_attached) return;

  GList *children = gtk_container_get_children(GTK_CONTAINER(d->list));
  for(GList *c = children; c; c = g_list_next(c)) gtk_widget_destroy(c->data);
  g_list_free(children);

  GList *owners = _owners();
  gboolean found = FALSE;
  for(GList *l = owners; l; l = g_list_next(l))
  {
    dt_iop_module_t *owner = l->data;
    const gboolean sel = !g_strcmp0(owner->multi_name, d->selected);
    found |= sel;
    gchar *kind = _kind(owner);
    gchar *text = g_strdup_printf("%s  ·  %s", owner->multi_name, kind);
    GtkWidget *row = gtk_button_new_with_label(text);
    g_free(text);
    g_free(kind);
    gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(row))), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(gtk_bin_get_child(GTK_BIN(row))), 0.0f);
    gtk_widget_set_name(row, sel ? "masking-row-selected" : "masking-row");
    gtk_widget_set_tooltip_text(row, _("select the mask: its shapes can be edited on the photo"));
    g_object_set_data_full(G_OBJECT(row), "mask", g_strdup(owner->multi_name), g_free);
    g_signal_connect(row, "clicked", G_CALLBACK(_row_clicked), self);

    GtkWidget *eye = dtgtk_togglebutton_new(dtgtk_cairo_paint_eye, 0, NULL);
    gtk_widget_set_tooltip_text(eye, _("turn the mask and its adjustments on or off"));
    g_object_set_data_full(G_OBJECT(eye), "mask", g_strdup(owner->multi_name), g_free);
    DT_ENTER_GUI_UPDATE();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(eye), owner->enabled);
    DT_LEAVE_GUI_UPDATE();
    g_signal_connect(eye, "toggled", G_CALLBACK(_row_visible), self);

    GtkWidget *del = dtgtk_button_new(dtgtk_cairo_paint_remove, 0, NULL);
    gtk_widget_set_tooltip_text(del, _("delete the mask and its adjustments"));
    g_object_set_data_full(G_OBJECT(del), "mask", g_strdup(owner->multi_name), g_free);
    g_signal_connect(del, "clicked", G_CALLBACK(_row_delete), self);

    GtkWidget *box = dt_gui_hbox(dt_gui_expand(row), eye, del);
    gtk_box_pack_start(GTK_BOX(d->list), box, FALSE, FALSE, 0);
  }
  if(!found) d->selected[0] = '\0';
  gtk_widget_set_visible(d->empty, owners == NULL);
  g_list_free(owners);
  gtk_widget_show_all(d->list);
  _sync_selected(self);
}

static gchar *_new_name(void)
{
  int n = 1;
  gchar *name = NULL;
  do
  {
    g_free(name);
    name = g_strdup_printf(MASK_PREFIX "%d", n++);
  } while(_owner(name));
  return name;
}

// a new mask: an exposure instance (neutral, the mask holder)
static dt_iop_module_t *_new_mask(dt_lib_module_t *self)
{
  dt_lib_masking_t *d = self->data;
  dt_iop_module_t *base = dt_iop_get_module_from_list(darktable.develop->iop, "exposure");
  if(!base) return NULL;
  dt_iop_module_t *owner = dt_iop_gui_duplicate(base, FALSE);
  if(!owner) return NULL;

  gchar *name = _new_name();
  g_strlcpy(owner->multi_name, name, sizeof(owner->multi_name));
  owner->multi_name_hand_edited = TRUE;
  g_free(name);

  // no exposure change of its own (the defaults compensate the camera exposure)
  int *mode = owner->so->get_p(owner->params, "mode");
  float *exposure = owner->so->get_p(owner->params, "exposure");
  float *black = owner->so->get_p(owner->params, "black");
  gboolean *bias = owner->so->get_p(owner->params, "compensate_exposure_bias");
  if(mode) *mode = 0;
  if(exposure) *exposure = 0.0f;
  if(black) *black = 0.0f;
  if(bias) *bias = FALSE;
  dt_iop_gui_update(owner);
  dt_iop_gui_update_header(owner);
  dt_iop_gui_set_expanded(owner, FALSE, FALSE);
  dt_dev_add_history_item(darktable.develop, owner, TRUE);

  g_strlcpy(d->selected, owner->multi_name, sizeof(d->selected));
  return owner;
}

static void _create_shape(dt_lib_module_t *self, const int type)
{
#ifdef HAVE_AI
  if(type == DT_MASKS_OBJECT && !dt_masks_object_available())
  {
    dt_control_log(_("the AI object mask needs its model: download it in preferences > AI"));
    return;
  }
#endif
  dt_iop_module_t *owner = _new_mask(self);
  if(!owner) return;
  dt_iop_gui_blend_add_shape(owner, type);
  _rebuild(self);
}

static void _create_brush(GtkButton *b, dt_lib_module_t *self) { _create_shape(self, DT_MASKS_BRUSH); }
static void _create_linear(GtkButton *b, dt_lib_module_t *self) { _create_shape(self, DT_MASKS_GRADIENT); }
static void _create_radial(GtkButton *b, dt_lib_module_t *self) { _create_shape(self, DT_MASKS_ELLIPSE); }
static void _create_object(GtkButton *b, dt_lib_module_t *self) { _create_shape(self, DT_MASKS_OBJECT); }

static void _create_range(dt_lib_module_t *self, const gboolean color)
{
  dt_lib_masking_t *d = self->data;
  dt_iop_module_t *owner = _new_mask(self);
  if(!owner) return;
  DT_ENTER_GUI_UPDATE();
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(color ? d->col_on : d->lum_on), TRUE);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(color ? d->lum_on : d->col_on), FALSE);
  if(color)
  {
    dt_bauhaus_slider_set(d->col_hue, 220.0f);
    dt_bauhaus_slider_set(d->col_range, 60.0f);
    dt_bauhaus_slider_set(d->col_smooth, 50.0f);
  }
  else
  {
    dt_bauhaus_slider_set(d->lum_from, 60.0f);
    dt_bauhaus_slider_set(d->lum_to, 100.0f);
    dt_bauhaus_slider_set(d->lum_smooth, 30.0f);
  }
  DT_LEAVE_GUI_UPDATE();
  _ranges_write(d, owner);
  _focus(owner);
  _rebuild(self);
}

static void _create_color(GtkButton *b, dt_lib_module_t *self) { _create_range(self, TRUE); }
static void _create_luminance(GtkButton *b, dt_lib_module_t *self) { _create_range(self, FALSE); }

// add or subtract a shape of the selected mask
static void _shape_to_selected(dt_lib_module_t *self, const int type, const gboolean subtract)
{
  dt_lib_masking_t *d = self->data;
  dt_iop_module_t *owner = _owner(d->selected);
  if(!owner) return;
  d->pending = subtract ? PENDING_SUBTRACT : PENDING_NONE;
  d->pending_count = _shape_count(owner);
  dt_iop_gui_blend_add_shape(owner, type);
}

static void _add_brush(GtkButton *b, dt_lib_module_t *self) { _shape_to_selected(self, DT_MASKS_BRUSH, FALSE); }
static void _add_linear(GtkButton *b, dt_lib_module_t *self) { _shape_to_selected(self, DT_MASKS_GRADIENT, FALSE); }
static void _add_radial(GtkButton *b, dt_lib_module_t *self) { _shape_to_selected(self, DT_MASKS_ELLIPSE, FALSE); }
static void _sub_brush(GtkButton *b, dt_lib_module_t *self) { _shape_to_selected(self, DT_MASKS_BRUSH, TRUE); }
static void _sub_linear(GtkButton *b, dt_lib_module_t *self) { _shape_to_selected(self, DT_MASKS_GRADIENT, TRUE); }
static void _sub_radial(GtkButton *b, dt_lib_module_t *self) { _shape_to_selected(self, DT_MASKS_ELLIPSE, TRUE); }

// a subtracted shape: once drawn, it takes the difference mode
static void _pending_apply(dt_lib_module_t *self)
{
  dt_lib_masking_t *d = self->data;
  if(d->pending == PENDING_NONE) return;
  dt_iop_module_t *owner = _owner(d->selected);
  dt_masks_form_t *grp = owner ? dt_masks_get_from_id(darktable.develop, owner->blend_params->mask_id) : NULL;
  if(!grp || !(grp->type & DT_MASKS_GROUP) || g_list_length(grp->points) <= d->pending_count) return;
  dt_masks_point_group_t *pt = g_list_last(grp->points)->data;
  pt->state = (pt->state & ~DT_MASKS_STATE_OP) & ~(DT_MASKS_STATE_UNION | DT_MASKS_STATE_INTERSECTION
                                                   | DT_MASKS_STATE_EXCLUSION | DT_MASKS_STATE_SUM);
  pt->state |= DT_MASKS_STATE_DIFFERENCE;
  d->pending = PENDING_NONE;
  dt_dev_add_masks_history_item(darktable.develop, owner, TRUE);
}

static void _overlay_toggled(GtkToggleButton *b, dt_lib_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_lib_masking_t *d = self->data;
  dt_iop_module_t *owner = _owner(d->selected);
  if(!owner) return;
  _focus(owner);
  owner->request_mask_display = gtk_toggle_button_get_active(b) ? DT_DEV_PIXELPIPE_DISPLAY_MASK
                                                                : DT_DEV_PIXELPIPE_DISPLAY_NONE;
  dt_iop_refresh_center(owner);
}

static void _invert_toggled(GtkToggleButton *b, dt_lib_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_lib_masking_t *d = self->data;
  dt_iop_module_t *owner = _owner(d->selected);
  if(!owner) return;
  if(gtk_toggle_button_get_active(b))
    owner->blend_params->mask_combine |= DEVELOP_COMBINE_INV;
  else
    owner->blend_params->mask_combine &= ~DEVELOP_COMBINE_INV;
  dt_iop_gui_update_blending(owner);
  dt_dev_add_history_item(darktable.develop, owner, TRUE);
}

static void _amount_changed(GtkWidget *w, dt_lib_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_lib_masking_t *d = self->data;
  dt_iop_module_t *owner = _owner(d->selected);
  if(!owner) return;
  // the opacity of the owner's mask, which the other modules of the mask use too
  owner->blend_params->opacity = dt_bauhaus_slider_get(w);
  dt_iop_gui_update_blending(owner);
  dt_dev_add_history_item(darktable.develop, owner, TRUE);
}

static void _range_changed(GtkWidget *w, dt_lib_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_lib_masking_t *d = self->data;
  dt_iop_module_t *owner = _owner(d->selected);
  if(!owner) return;
  _ranges_write(d, owner);
  gtk_widget_set_visible(gtk_widget_get_parent(d->lum_from),
                         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->lum_on)));
  gtk_widget_set_visible(gtk_widget_get_parent(d->col_hue),
                         gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->col_on)));
}

static void _slider_changed(GtkWidget *w, dt_lib_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_lib_masking_t *d = self->data;
  dt_iop_module_t *owner = _owner(d->selected);
  if(!owner) return;

  int id = -1;
  for(int i = 0; i < MS_COUNT; i++)
    if(d->slider[i] == w) id = i;
  if(id < 0) return;
  const float value = dt_bauhaus_slider_get(w);

  if(id == MS_EXPOSURE)
  {
    float *e = owner->so->get_p(owner->params, "exposure");
    if(e) *e = value;
    _mask_commit(owner, TRUE);
  }
  else if(id == MS_TEMP || id == MS_TINT)
  {
    const dt_lsb_control_t *c = dt_lsb_control(DT_LSB_CONTRAST);
    dt_iop_module_t *m = _follower(d->selected, c, value != 0.0f);
    if(!m) return;
    if(!m->enabled) dt_lsb_neutralize_params(m, c, m->params);
    _temp_tint_write(m, dt_bauhaus_slider_get(d->slider[MS_TEMP]), dt_bauhaus_slider_get(d->slider[MS_TINT]));
    _mask_commit(m, FALSE);
  }
  else
  {
    const dt_lsb_control_t *c = dt_lsb_control(_ms[id].lsb);
    dt_iop_module_t *m = _follower(d->selected, c, value != 0.0f);
    if(!m) return;
    if(!m->enabled)
    {
      if(value == 0.0f) return;
      dt_lsb_neutralize_params(m, c, m->params);
    }
    dt_lsb_write_params(m, c, m->params, value);
    _mask_commit(m, FALSE);
  }
  _focus(owner);
}

static void _reset_adjustments(GtkButton *b, dt_lib_module_t *self)
{
  dt_lib_masking_t *d = self->data;
  if(!_owner(d->selected)) return;
  for(int i = 0; i < MS_COUNT; i++) dt_bauhaus_widget_reset(d->slider[i]);
}

static void _advanced(GtkButton *b, dt_lib_module_t *self)
{
  dt_lib_masking_t *d = self->data;
  dt_iop_module_t *owner = _owner(d->selected);
  if(!owner) return;
  // the full darktable module of the mask, with all its mask settings
  dt_lrp_reveal(1, owner);
  dt_dev_modulegroups_set(darktable.develop, 2);
  dt_iop_gui_set_expanded(owner, TRUE, FALSE);
  dt_iop_request_focus(owner);
}

static void _history_changed(gpointer instance, dt_lib_module_t *self)
{
  _pending_apply(self);
  _rebuild(self);
}

static gboolean _rebuild_idle(gpointer user_data)
{
  dt_lib_module_t *self = user_data;
  dt_lib_masking_t *d = self->data;
  d->idle = 0;
  _rebuild(self);
  return G_SOURCE_REMOVE;
}

// a new image: the darkroom raises the signal before it gives the modules
// the params of the history, so the list is built again once that is done
static void _image_changed(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_masking_t *d = self->data;
  _history_changed(instance, self);
  if(!d->idle) d->idle = g_idle_add(_rebuild_idle, self);
}

static GtkWidget *_heading(const char *label)
{
  GtkWidget *l = dt_ui_section_label_new(label);
  gtk_label_set_xalign(GTK_LABEL(l), 0.5f);
  gtk_widget_set_name(l, "basicpanel-section");
  return l;
}

static GtkWidget *_button(const char *label, const char *tooltip, GCallback cb, dt_lib_module_t *self)
{
  GtkWidget *b = gtk_button_new_with_label(label);
  gtk_widget_set_tooltip_text(b, tooltip);
  gtk_widget_set_name(b, "masking-button");
  g_signal_connect(b, "clicked", G_CALLBACK(cb), self);
  return b;
}

static GtkWidget *_slider(dt_lib_module_t *self, const char *label, const float min, const float max,
                          const float step, const float def, const int digits, const char *tooltip,
                          GCallback cb)
{
  GtkWidget *w = dt_bauhaus_slider_new_action(DT_ACTION(self), min, max, step, def, digits);
  dt_bauhaus_widget_set_label(w, NULL, label);
  gtk_widget_set_tooltip_text(w, tooltip);
  g_signal_connect(G_OBJECT(w), "value-changed", G_CALLBACK(cb), self);
  return w;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_masking_t *d = g_malloc0(sizeof(dt_lib_masking_t));
  self->data = d;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(self->widget, "basicpanel");

  // create
  dt_gui_box_add(self->widget, _heading(_("Create New Mask")));
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 3);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 3);
  gtk_grid_attach(GTK_GRID(grid), _button(_("Brush"), _("paint the mask with a brush on the photo"),
                                          G_CALLBACK(_create_brush), self), 0, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), _button(_("Linear"), _("linear gradient: drag on the photo, "
                                                          "the effect fades along the line"),
                                          G_CALLBACK(_create_linear), self), 1, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), _button(_("Radial"), _("radial gradient: click on the photo, "
                                                          "an ellipse that fades at its edge"),
                                          G_CALLBACK(_create_radial), self), 2, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), _button(_("Color"), _("color range: the areas of a color, "
                                                         "chosen with the hue slider"),
                                          G_CALLBACK(_create_color), self), 0, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), _button(_("Luminance"), _("luminance range: the bright or the dark areas"),
                                          G_CALLBACK(_create_luminance), self), 1, 1, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), _button(_("Object"), _("AI object: click an object or a person on the "
                                                          "photo (needs the AI model, preferences > AI)"),
                                          G_CALLBACK(_create_object), self), 2, 1, 1, 1);
  dt_gui_box_add(self->widget, grid);

  // the masks of the photo
  dt_gui_box_add(self->widget, _heading(_("Masks")));
  d->list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  dt_gui_box_add(self->widget, d->list);
  d->empty = gtk_label_new(_("no masks on this photo yet"));
  gtk_widget_set_name(d->empty, "basicpanel-label");
  gtk_widget_set_no_show_all(d->empty, TRUE);
  dt_gui_box_add(self->widget, d->empty);

  // the selected mask
  d->selected_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  d->title = gtk_label_new("");
  gtk_label_set_ellipsize(GTK_LABEL(d->title), PANGO_ELLIPSIZE_END);
  gtk_widget_set_name(d->title, "masking-title");
  dt_gui_box_add(d->selected_box, d->title);

  GtkWidget *add = dt_gui_hbox(gtk_label_new(_("Add")),
                               dt_gui_expand(_button(_("Brush"), _("add brush strokes to the mask"),
                                                     G_CALLBACK(_add_brush), self)),
                               dt_gui_expand(_button(_("Linear"), _("add a linear gradient to the mask"),
                                                     G_CALLBACK(_add_linear), self)),
                               dt_gui_expand(_button(_("Radial"), _("add a radial gradient to the mask"),
                                                     G_CALLBACK(_add_radial), self)));
  GtkWidget *sub = dt_gui_hbox(gtk_label_new(_("Subtract")),
                               dt_gui_expand(_button(_("Brush"), _("erase from the mask with a brush"),
                                                     G_CALLBACK(_sub_brush), self)),
                               dt_gui_expand(_button(_("Linear"), _("subtract a linear gradient from the mask"),
                                                     G_CALLBACK(_sub_linear), self)),
                               dt_gui_expand(_button(_("Radial"), _("subtract a radial gradient from the mask"),
                                                     G_CALLBACK(_sub_radial), self)));
  dt_gui_box_add(d->selected_box, add);
  dt_gui_box_add(d->selected_box, sub);

  d->overlay = gtk_toggle_button_new_with_label(_("Show Overlay"));
  gtk_widget_set_tooltip_text(d->overlay, _("show the mask in yellow on the photo"));
  g_signal_connect(d->overlay, "toggled", G_CALLBACK(_overlay_toggled), self);
  d->invert = gtk_toggle_button_new_with_label(_("Invert"));
  gtk_widget_set_tooltip_text(d->invert, _("adjust everything except the masked area"));
  g_signal_connect(d->invert, "toggled", G_CALLBACK(_invert_toggled), self);
  dt_gui_box_add(d->selected_box, dt_gui_hbox(dt_gui_expand(d->overlay), dt_gui_expand(d->invert)));
  d->amount = _slider(self, _("Amount"), 0.0f, 100.0f, 1.0f, 100.0f, 0, _("strength of all the adjustments of the mask"),
                      G_CALLBACK(_amount_changed));
  dt_bauhaus_slider_set_format(d->amount, "%");
  dt_gui_box_add(d->selected_box, d->amount);

  // ranges (a range mask, or a drawn mask limited to a range)
  d->lum_on = gtk_check_button_new_with_label(_("Luminance range"));
  gtk_widget_set_tooltip_text(d->lum_on, _("limit the mask to a range of brightness"));
  g_signal_connect(d->lum_on, "toggled", G_CALLBACK(_range_changed), self);
  d->lum_from = _slider(self, _("from"), 0.0f, 100.0f, 1.0f, 60.0f, 0, _("the darkest tone in the mask"),
                        G_CALLBACK(_range_changed));
  d->lum_to = _slider(self, _("to"), 0.0f, 100.0f, 1.0f, 100.0f, 0, _("the brightest tone in the mask"),
                      G_CALLBACK(_range_changed));
  d->lum_smooth = _slider(self, _("smoothness"), 0.0f, 100.0f, 1.0f, 30.0f, 0,
                          _("how softly the mask fades at the ends of the range"), G_CALLBACK(_range_changed));
  dt_gui_box_add(d->selected_box, d->lum_on);
  GtkWidget *lum = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_box_add(lum, d->lum_from, d->lum_to, d->lum_smooth);
  gtk_widget_show_all(lum);
  gtk_widget_set_no_show_all(lum, TRUE);
  gtk_widget_hide(lum);
  dt_gui_box_add(d->selected_box, lum);

  d->col_on = gtk_check_button_new_with_label(_("Color range"));
  gtk_widget_set_tooltip_text(d->col_on, _("limit the mask to a range of colors"));
  g_signal_connect(d->col_on, "toggled", G_CALLBACK(_range_changed), self);
  d->col_hue = _slider(self, _("hue"), 0.0f, 360.0f, 1.0f, 220.0f, 0, _("the color in the mask"),
                       G_CALLBACK(_range_changed));
  dt_bauhaus_slider_set_format(d->col_hue, "°");
  for(int k = 0; k <= 6; k++)
  {
    // a hue scale (the hues of the color range, approximately)
    const float h = k / 6.0f;
    dt_aligned_pixel_t rgb;
    hsl2rgb(rgb, h, 0.8f, 0.5f);
    dt_bauhaus_slider_set_stop(d->col_hue, h, rgb[0], rgb[1], rgb[2]);
  }
  d->col_range = _slider(self, _("range"), 5.0f, 180.0f, 1.0f, 60.0f, 0, _("how many neighbouring colors are in the mask"),
                         G_CALLBACK(_range_changed));
  dt_bauhaus_slider_set_format(d->col_range, "°");
  d->col_smooth = _slider(self, _("smoothness"), 0.0f, 100.0f, 1.0f, 50.0f, 0,
                          _("how softly the mask fades at the ends of the range"), G_CALLBACK(_range_changed));
  dt_gui_box_add(d->selected_box, d->col_on);
  GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_box_add(col, d->col_hue, d->col_range, d->col_smooth);
  gtk_widget_show_all(col);
  gtk_widget_set_no_show_all(col, TRUE);
  gtk_widget_hide(col);
  dt_gui_box_add(d->selected_box, col);

  // the adjustments of the mask
  dt_gui_box_add(d->selected_box, _heading(_("Adjustments")));
  for(int i = 0; i < MS_COUNT; i++)
  {
    const gboolean ev = i == MS_EXPOSURE;
    GtkWidget *w = _slider(self, _(_ms[i].label), ev ? -5.0f : -100.0f, ev ? 5.0f : 100.0f, ev ? 0.05f : 1.0f,
                           0.0f, ev ? 2 : 0, _(_ms[i].tooltip), G_CALLBACK(_slider_changed));
    if(ev)
    {
      dt_bauhaus_slider_set_soft_range(w, -4.0f, 4.0f);
      dt_bauhaus_slider_set_format(w, " EV");
    }
    else if(i == MS_TEMP)
    {
      dt_bauhaus_slider_set_stop(w, 0.0f, 0.25f, 0.45f, 1.0f);
      dt_bauhaus_slider_set_stop(w, 0.5f, 0.85f, 0.85f, 0.85f);
      dt_bauhaus_slider_set_stop(w, 1.0f, 1.0f, 0.8f, 0.15f);
    }
    else if(i == MS_TINT)
    {
      dt_bauhaus_slider_set_stop(w, 0.0f, 0.3f, 0.85f, 0.3f);
      dt_bauhaus_slider_set_stop(w, 0.5f, 0.85f, 0.85f, 0.85f);
      dt_bauhaus_slider_set_stop(w, 1.0f, 0.9f, 0.3f, 0.9f);
    }
    d->slider[i] = w;
    dt_gui_box_add(d->selected_box, w);
  }
  dt_gui_box_add(d->selected_box,
                 dt_gui_hbox(dt_gui_expand(_button(_("Reset"), _("set all the adjustments of the mask to zero"),
                                                   G_CALLBACK(_reset_adjustments), self)),
                             dt_gui_expand(_button(_("All Mask Settings"),
                                                   _("the darktable module of the mask in All tools: blend modes, "
                                                     "feathering, every range channel, raster masks"),
                                                   G_CALLBACK(_advanced), self))));
  gtk_widget_show_all(d->selected_box);
  gtk_widget_set_no_show_all(d->selected_box, TRUE);
  gtk_widget_hide(d->selected_box);
  dt_gui_box_add(self->widget, d->selected_box);

  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_HISTORY_CHANGE, _history_changed);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _image_changed);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_INITIALIZE, _image_changed);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_masking_t *d = self->data;
  if(d->idle) g_source_remove(d->idle);
  g_free(self->data);
  self->data = NULL;
}

void gui_reset(dt_lib_module_t *self)
{
  _reset_adjustments(NULL, self);
}

void view_enter(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  // the place of the panel in the develop tab, see gui/lrpanels.c
  GtkWidget *w = self->expander ? self->expander : self->widget;
  g_object_set_data(G_OBJECT(w), "dt-lrp-masking", GINT_TO_POINTER(TRUE));
  dt_lib_masking_t *d = self->data;
  d->selected[0] = '\0';
  d->pending = PENDING_NONE;
  _rebuild(self);
}

void view_leave(dt_lib_module_t *self, dt_view_t *old_view, dt_view_t *new_view)
{
  dt_lib_masking_t *d = self->data;
  dt_iop_module_t *owner = _owner(d->selected);
  if(owner) owner->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  d->selected[0] = '\0';
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
