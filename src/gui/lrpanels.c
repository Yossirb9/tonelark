/*
    This file is part of Tonelark, a darktable fork.

    Tonelark is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "gui/lrpanels.h"
#include "common/darktable.h"
#include "control/conf.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "dtgtk/button.h"
#include "dtgtk/expander.h"
#include "dtgtk/paint.h"
#include "gui/gtk.h"

#define MAX_OPS 32

typedef struct _section_t
{
  const char *key;               // for the preference of the open menu
  const char *title;
  const char *ops[MAX_OPS];      // in display order
  const char *alts[8];           // develop tab: only when the photo uses them
  gboolean top;                  // develop tab: above the basic panel
} _section_t;

// the develop panel of Lightroom Classic: the tools of the tool strip (crop,
// remove) above the basic panel, then the panels in their order. A panel shows
// the controls of its modules at once, without their headers; a module per
// function of the panel (Lightroom's white balance is in the basic panel, its
// red eye and masking tools have no module: the masks of each module do that).
static const _section_t _develop[] =
{
  { "crop",        N_("Crop"),             { "crop" },                      { "clipping" }, TRUE },
  { "remove",      N_("Remove"),           { "retouch" },                   { "spots" }, TRUE },
  { "tonecurve",   N_("Tone Curve"),       { "rgbcurve" },                  { "tonecurve" } },
  { "hsl",         N_("HSL / Color"),      { "colorequal" },                { "colorzones" } },
  { "grading",     N_("Color Grading"),    { "colorbalancergb" },           { "splittoning" } },
  { "detail",      N_("Detail"),           { "sharpen", "denoiseprofile" }, { "nlmeans", "diffuse" } },
  { "lens",        N_("Lens Corrections"), { "lens", "cacorrectrgb" },      { "cacorrect", "defringe" } },
  { "transform",   N_("Transform"),        { "ashift" },                    { NULL } },
  { "effects",     N_("Effects"),          { "vignette", "grain" },         { NULL } },
  { "calibration", N_("Calibration"),      { "primaries" },                 { NULL } },
};

// the headings of the modules in a panel with several (as the sections of the
// Lightroom panels: Detail has Sharpening and Noise Reduction)
static const struct { const char *op, *caption; } _captions[] =
{
  { "crop",            N_("Crop") },
  { "clipping",        N_("Crop (classic)") },
  { "retouch",         N_("Heal, clone and fill") },
  { "spots",           N_("Spot removal (classic)") },
  { "rgbcurve",        N_("Point Curve") },
  { "tonecurve",       N_("Point Curve (classic)") },
  { "colorequal",      N_("HSL") },
  { "colorzones",      N_("Color Zones") },
  { "colorbalancergb", N_("Color Grading") },
  { "splittoning",     N_("Split Toning (classic)") },
  { "sharpen",         N_("Sharpening") },
  { "denoiseprofile",  N_("Noise Reduction") },
  { "nlmeans",         N_("Noise Reduction (astro)") },
  { "diffuse",         N_("Diffuse or Sharpen") },
  { "lens",            N_("Profile Corrections") },
  { "cacorrectrgb",    N_("Remove Chromatic Aberration") },
  { "cacorrect",       N_("Remove Chromatic Aberration (raw)") },
  { "defringe",        N_("Defringe") },
  { "vignette",        N_("Post-Crop Vignetting") },
  { "grain",           N_("Grain") },
};

// every module, in menus by task. modules that are in no menu go to "other".
static const _section_t _tools[] =
{
  { "light", N_("Light & tone"),
    { "exposure", "toneequal", "shadhi", "highlights", "rgbcurve", "tonecurve", "rgblevels", "levels",
      "bilat", "atrous", "sigmoid", "filmicrgb", "agx", "basecurve", "filmic", "globaltonemap",
      "tonemap", "zonesystem", "relight", "negadoctor", "invert", "basicadj", "colisa", "clahe",
      "equalizer" } },
  { "color", N_("Color"),
    { "channelmixerrgb", "temperature", "colorbalancergb", "colorequal", "colorzones", "primaries",
      "velvia", "vibrance", "colorcorrection", "colorcontrast", "colorharmonizer", "colorize",
      "monochrome", "splittoning", "lut3d", "colorchecker", "colormapping", "colortransfer",
      "colorbalance", "channelmixer", "colorreconstruct" } },
  { "detail", N_("Detail & noise"),
    { "sharpen", "diffuse", "denoiseprofile", "nlmeans", "rawdenoise", "bilateral", "hotpixels",
      "hazeremoval" } },
  { "geometry", N_("Lens & geometry"),
    { "lens", "cacorrectrgb", "cacorrect", "defringe", "ashift", "crop", "clipping", "flip",
      "rotatepixels", "scalepixels", "enlargecanvas" } },
  { "retouch", N_("Retouch"),
    { "retouch", "spots", "liquify" } },
  { "effects", N_("Effects"),
    { "vignette", "grain", "bloom", "soften", "blurs", "lowlight", "lowpass", "highpass",
      "graduatednd", "borders", "watermark", "overlay", "censorize" } },
  { "technical", N_("Technical"),
    { "rawprepare", "demosaic", "colorin", "colorout", "dither", "profile_gamma", "rasterfile",
      "finalscale", "gamma", "rawoverexposed", "overexposed" } },
  { "other", N_("Other"), { NULL } },
};

typedef struct _tab_t
{
  const char *key;
  const char *name;
  const _section_t *sections;
  const int n;
} _tab_t;

static const _tab_t _tabs[DT_LRP_TABS] =
{
  // (darktable's NC_ puts a '|' in the string, the separator of the presets)
  { "develop", "develop", _develop, G_N_ELEMENTS(_develop) },
  { "tools", "all tools", _tools, G_N_ELEMENTS(_tools) },
};

// the Lightroom names of the modules
static const struct { const char *op, *title; } _titles[] =
{
  { "rgbcurve",        N_("Tone Curve") },
  { "tonecurve",       N_("Tone Curve (classic)") },
  { "colorequal",      N_("HSL / Color") },
  { "colorbalancergb", N_("Color Grading") },
  { "splittoning",     N_("Split Toning") },
  { "sharpen",         N_("Sharpening") },
  { "denoiseprofile",  N_("Noise Reduction") },
  { "lens",            N_("Lens Profile") },
  { "cacorrectrgb",    N_("Chromatic Aberration") },
  { "ashift",          N_("Rotate & Perspective") },
  { "crop",            N_("Crop") },
  { "clipping",        N_("Crop (classic)") },
  { "vignette",        N_("Vignette") },
  { "grain",           N_("Grain") },
  { "primaries",       N_("Calibration") },
  { "retouch",         N_("Remove") },
  { "spots",           N_("Spot Removal (classic)") },
  { "hazeremoval",     N_("Dehaze") },
  { "shadhi",          N_("Shadows & Highlights") },
  { "bilat",           N_("Local Contrast") },
  { "toneequal",       N_("Tone Equalizer") },
};

// the menu headers of the right panel, [tab][section]
// (the most sections of a tab, checked below)
#define MAX_SECTIONS 16
G_STATIC_ASSERT(G_N_ELEMENTS(_develop) <= MAX_SECTIONS && G_N_ELEMENTS(_tools) <= MAX_SECTIONS);
static GtkWidget *_headers[DT_LRP_TABS][MAX_SECTIONS];
static int _tab = -1;
static int _shown_tab = -1;     // the tab laid out since entering the darkroom

gboolean dt_lrp_names(void)
{
  return dt_conf_get_bool("lightspeed/lightroom_panel_order");
}

gboolean dt_lrp_enabled(void)
{
  if(!dt_lrp_names()) return FALSE;
  // no layout chosen (or an old one's name left empty): the module groups
  // fall back to the Tonelark layout
  const char *preset = dt_conf_get_string_const("plugins/darkroom/modulegroups_preset");
  return !preset || !*preset || !strcmp(preset, _("Tonelark"));
}

const char *dt_lrp_module_title(const char *op)
{
  if(!op || !dt_lrp_names()) return NULL;
  for(int i = 0; i < G_N_ELEMENTS(_titles); i++)
    if(!strcmp(_titles[i].op, op)) return _(_titles[i].title);
  return NULL;
}

gchar *dt_lrp_title_case(const char *name)
{
  if(!name) return NULL;
  // translated names keep their case
  for(const char *c = name; *c; c++)
    if((unsigned char)*c > 127) return g_strdup(name);

  static const char *small[] = { "a", "an", "and", "for", "in", "of", "on", "or", "the", "to", "with", NULL };
  static const struct { const char *word, *title; } special[] =
    { { "rgb", "RGB" }, { "lut", "LUT" }, { "3d", "3D" }, { "agx", "AgX" }, { "hdr", "HDR" },
      { "cmy", "CMY" }, { "rgb/cmy", "RGB/CMY" } };

  gchar **words = g_strsplit(name, " ", -1);
  for(int i = 0; words[i]; i++)
  {
    gchar *w = words[i];
    gboolean done = FALSE;
    for(int k = 0; k < G_N_ELEMENTS(special) && !done; k++)
      if(!g_ascii_strcasecmp(w, special[k].word))
      {
        g_free(words[i]);
        words[i] = g_strdup(special[k].title);
        done = TRUE;
      }
    if(done) continue;
    gboolean is_small = FALSE;
    for(int k = 0; small[k] && i > 0; k++)
      if(!strcmp(w, small[k])) is_small = TRUE;
    if(is_small) continue;
    // the first letter and the letters after a slash ("black/white")
    for(char *c = w; *c; c++)
      if(c == w || c[-1] == '/') *c = g_ascii_toupper(*c);
  }
  gchar *out = g_strjoinv(" ", words);
  g_strfreev(words);
  return out;
}

const char *dt_lrp_tab_name(const int tab)
{
  return (tab >= 0 && tab < DT_LRP_TABS) ? g_dpgettext2(NULL, "modulegroup", _tabs[tab].name) : "";
}

void dt_lrp_tab_ops(const int tab, void (*add)(const char *op, gpointer data), gpointer data)
{
  if(tab < 0 || tab >= DT_LRP_TABS) return;
  for(int s = 0; s < _tabs[tab].n; s++)
  {
    const _section_t *sec = &_tabs[tab].sections[s];
    for(int k = 0; k < MAX_OPS && sec->ops[k]; k++) add(sec->ops[k], data);
    for(int k = 0; k < G_N_ELEMENTS(sec->alts) && sec->alts[k]; k++) add(sec->alts[k], data);
  }
}

// position of op in the section (ops, then alts), -1 if not in it
static int _rank(const _section_t *sec, const char *op)
{
  for(int k = 0; k < MAX_OPS && sec->ops[k]; k++)
    if(!strcmp(sec->ops[k], op)) return k;
  for(int k = 0; k < G_N_ELEMENTS(sec->alts) && sec->alts[k]; k++)
    if(!strcmp(sec->alts[k], op)) return MAX_OPS + k;
  return -1;
}

static gboolean _is_alt(const _section_t *sec, const char *op)
{
  return _rank(sec, op) >= MAX_OPS;
}

// the section of a module in the tab, -1 if none (the tools tab puts the
// modules that are in no menu in its last one)
static int _section(const int tab, const dt_iop_module_t *module)
{
  const _tab_t *t = &_tabs[tab];
  for(int s = 0; s < t->n; s++)
    if(_rank(&t->sections[s], module->op) >= 0) return s;
  return tab == 1 ? t->n - 1 : -1;
}

static gboolean _in_history(const dt_iop_module_t *module)
{
  const dt_develop_t *dev = darktable.develop;
  int i = 0;
  for(const GList *h = dev->history; h && i < dev->history_end; h = g_list_next(h), i++)
  {
    const dt_dev_history_item_t *hist = h->data;
    if(hist->module == module) return TRUE;
  }
  return FALSE;
}

gboolean dt_lrp_in_tab(const int tab, dt_iop_module_t *module)
{
  if(tab < 0 || tab >= DT_LRP_TABS) return FALSE;
  const int s = _section(tab, module);
  if(s < 0) return FALSE;
  if(tab == 0 && _is_alt(&_tabs[tab].sections[s], module->op))
    return module->enabled || _in_history(module);
  return TRUE;
}

static gchar *_conf_key(const int tab, const int s)
{
  return g_strdup_printf("lightspeed/panels/%s/%s", _tabs[tab].key, _tabs[tab].sections[s].key);
}

static void _header_paint(const int tab, const int s)
{
  GtkWidget *h = _headers[tab][s];
  if(!h) return;
  gchar *key = _conf_key(tab, s);
  const gboolean open = dt_conf_get_bool(key);
  g_free(key);
  GtkWidget *arrow = g_object_get_data(G_OBJECT(h), "arrow");
  dtgtk_button_set_paint(DTGTK_BUTTON(arrow), dtgtk_cairo_paint_solid_arrow,
                         open ? CPF_DIRECTION_DOWN : CPF_DIRECTION_RIGHT, NULL);
}

static gboolean _header_clicked(GtkWidget *w, GdkEventButton *e, gpointer data)
{
  if(e->button != 1) return FALSE;
  const int tab = GPOINTER_TO_INT(data) / 100, s = GPOINTER_TO_INT(data) % 100;
  gchar *key = _conf_key(tab, s);
  dt_conf_set_bool(key, !dt_conf_get_bool(key));
  g_free(key);
  _header_paint(tab, s);
  dt_dev_modulegroups_update_visibility(darktable.develop);
  return TRUE;
}

static GList *_section_modules(const int tab, const int s);

static gboolean _reset_clicked(GtkWidget *w, GdkEventButton *e, gpointer data)
{
  if(e->button != 1) return FALSE;
  const int tab = GPOINTER_TO_INT(data) / 100, s = GPOINTER_TO_INT(data) % 100;
  GList *modules = _section_modules(tab, s);
  for(const GList *m = modules; m; m = g_list_next(m))
  {
    dt_iop_module_t *module = m->data;
    if(module->enabled || _in_history(module)) dt_iop_gui_reset_module(module);
  }
  g_list_free(modules);
  return TRUE;
}

static GtkWidget *_header(const int tab, const int s)
{
  if(_headers[tab][s]) return _headers[tab][s];

  GtkWidget *evb = gtk_event_box_new();
  dt_gui_add_class(evb, "dt_lrp_menu");
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_name(box, "module-header");
  GtkWidget *arrow = dtgtk_button_new(dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_RIGHT, NULL);
  gtk_widget_set_can_focus(arrow, FALSE);
  g_signal_connect(arrow, "button-release-event", G_CALLBACK(_header_clicked),
                   GINT_TO_POINTER(tab * 100 + s));
  GtkWidget *label = gtk_label_new(_(_tabs[tab].sections[s].title));
  gtk_widget_set_name(label, "lib-panel-label");
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  GtkWidget *mark = gtk_label_new("");
  gtk_widget_set_name(mark, "lrp-menu-mark");
  gtk_widget_set_tooltip_text(mark, _("edited"));
  gtk_box_pack_start(GTK_BOX(box), arrow, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), label, TRUE, TRUE, 0);
  if(tab == 0)
  {
    // the panels show no module headers: their reset is here
    GtkWidget *reset = dtgtk_button_new(dtgtk_cairo_paint_reset, 0, NULL);
    gtk_widget_set_can_focus(reset, FALSE);
    gtk_widget_set_tooltip_text(reset, _("reset this panel"));
    g_signal_connect(reset, "button-release-event", G_CALLBACK(_reset_clicked),
                     GINT_TO_POINTER(tab * 100 + s));
    gtk_box_pack_end(GTK_BOX(box), reset, FALSE, FALSE, 0);
  }
  gtk_box_pack_end(GTK_BOX(box), mark, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(evb), box);
  g_object_set_data(G_OBJECT(evb), "arrow", arrow);
  g_object_set_data(G_OBJECT(evb), "mark", mark);
  g_signal_connect(evb, "button-release-event", G_CALLBACK(_header_clicked),
                   GINT_TO_POINTER(tab * 100 + s));
  gtk_widget_set_tooltip_text(evb, _("click to open or close this menu"));

  GtkBox *panel = dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);
  gtk_box_pack_start(panel, evb, FALSE, FALSE, 0);
  gtk_widget_show_all(box);
  gtk_widget_set_no_show_all(evb, TRUE);
  _headers[tab][s] = evb;
  _header_paint(tab, s);
  return evb;
}

// the modules of dev->iop in the order they are shown: the order of the ops
// in the section, the instances of a module in reverse pipe order
typedef struct _sort_t
{
  const _section_t *sec;
} _sort_t;

static gint _section_cmp(gconstpointer a, gconstpointer b, gpointer data)
{
  const _sort_t *s = data;
  const dt_iop_module_t *ma = a, *mb = b;
  const int ra = s->sec ? _rank(s->sec, ma->op) : 0;
  const int rb = s->sec ? _rank(s->sec, mb->op) : 0;
  return ra - rb;
}

static GList *_section_modules(const int tab, const int s)
{
  GList *out = NULL;
  for(const GList *m = g_list_last(darktable.develop->iop); m; m = g_list_previous(m))
  {
    dt_iop_module_t *module = m->data;
    if(module->expander && _section(tab, module) == s) out = g_list_prepend(out, module);
  }
  out = g_list_reverse(out);
  const _section_t *sec = &_tabs[tab].sections[s];
  _sort_t sd = { sec->ops[0] ? sec : NULL };
  return g_list_sort_with_data(out, _section_cmp, &sd);
}

// the rank of a module when no menus are shown: the develop panels, then the tools
static int _flat_rank(const dt_iop_module_t *module)
{
  int base = 0;
  for(int tab = 0; tab < DT_LRP_TABS; tab++)
  {
    const _tab_t *t = &_tabs[tab];
    for(int s = 0; s < t->n; s++)
    {
      const int r = _rank(&t->sections[s], module->op);
      if(r >= 0) return base + s * 2 * MAX_OPS + r;
    }
    base += t->n * 2 * MAX_OPS;
  }
  return G_MAXINT;
}

static gint _flat_cmp(gconstpointer a, gconstpointer b)
{
  const int ra = _flat_rank(a), rb = _flat_rank(b);
  return ra < rb ? -1 : ra > rb;
}

static int _pin_top(GtkBox *panel, int pos)
{
  GList *children = gtk_container_get_children(GTK_CONTAINER(panel));
  for(const GList *c = children; c; c = g_list_next(c))
    if(g_object_get_data(G_OBJECT(c->data), "dt-pin-top"))
      gtk_box_reorder_child(panel, GTK_WIDGET(c->data), pos++);
  g_list_free(children);
  return pos;
}

static void _hide_headers(const int except_tab)
{
  for(int tab = 0; tab < DT_LRP_TABS; tab++)
    if(tab != except_tab)
      for(int s = 0; s < MAX_SECTIONS; s++)
        if(_headers[tab][s]) gtk_widget_hide(_headers[tab][s]);
}

static void _hide_module(dt_iop_module_t *module)
{
  if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
  gtk_widget_hide(module->expander);
}

// the module is shown in the tab when its menu is open (as the module groups
// show a group: no hidden or unused modules, deprecated ones only when used)
static gboolean _visible_in_tab(const int tab, dt_iop_module_t *module)
{
  if(dt_iop_is_hidden(module) || module->iop_order == INT_MAX) return FALSE;
  if((module->flags() & IOP_FLAGS_DEPRECATED) && !module->enabled) return FALSE;
  return dt_lrp_in_tab(tab, module);
}

static const char *_caption(const char *op)
{
  for(int i = 0; i < G_N_ELEMENTS(_captions); i++)
    if(!strcmp(_captions[i].op, op)) return _(_captions[i].caption);
  return dt_iop_get_localized_name(op);
}

static void _keep_hidden(GtkWidget *w, gpointer data)
{
  if(g_object_get_data(G_OBJECT(w), "dt-lrp-hidden")) gtk_widget_hide(w);
}

// the rows of the module body besides its controls (masks, blending, guides):
// hidden in the develop panels (all tools has them), shown again as they were
static void _body_extras(dt_iop_module_t *module, GtkWidget *cap, const gboolean hide)
{
  GtkWidget *body = dtgtk_expander_get_body(DTGTK_EXPANDER(module->expander));
  GList *children = gtk_container_get_children(GTK_CONTAINER(body));
  for(const GList *c = children; c; c = g_list_next(c))
  {
    GtkWidget *w = c->data;
    if(w == module->widget || w == cap) continue;
    GObject *o = G_OBJECT(w);
    if(hide)
    {
      if(g_object_get_data(o, "dt-lrp-hidden")) continue;
      g_object_set_data(o, "dt-lrp-was-visible", GINT_TO_POINTER(gtk_widget_get_visible(w)));
      g_object_set_data(o, "dt-lrp-hidden", GINT_TO_POINTER(TRUE));
      if(!g_object_get_data(o, "dt-lrp-guard"))
      {
        // darktable shows the mask rows again when it updates them
        g_signal_connect_after(w, "show", G_CALLBACK(_keep_hidden), NULL);
        g_object_set_data(o, "dt-lrp-guard", GINT_TO_POINTER(TRUE));
      }
      gtk_widget_hide(w);
    }
    else if(g_object_get_data(o, "dt-lrp-hidden"))
    {
      g_object_set_data(o, "dt-lrp-hidden", NULL);
      gtk_widget_set_visible(w, GPOINTER_TO_INT(g_object_get_data(o, "dt-lrp-was-visible")));
    }
  }
  g_list_free(children);
}

// a module of a develop panel shows its controls without its header, open;
// with a heading when the panel has several modules
static void _flatten(dt_iop_module_t *module, const gboolean flat, const gboolean caption)
{
  GtkWidget *exp = module->expander;
  GtkWidget *head = dtgtk_expander_get_header_event_box(DTGTK_EXPANDER(exp));
  const gboolean was = g_object_get_data(G_OBJECT(exp), "dt-lrp-flat") != NULL;
  GtkWidget *cap = g_object_get_data(G_OBJECT(exp), "dt-lrp-caption");

  if(flat)
  {
    if(!cap)
    {
      cap = dt_ui_section_label_new(_caption(module->op));
      gtk_widget_set_name(cap, "lrp-caption");
      GtkWidget *body = dtgtk_expander_get_body(DTGTK_EXPANDER(exp));
      gtk_box_pack_start(GTK_BOX(body), cap, FALSE, FALSE, 0);
      gtk_box_reorder_child(GTK_BOX(body), cap, 0);
      gtk_widget_set_no_show_all(cap, TRUE);
      g_object_set_data(G_OBJECT(exp), "dt-lrp-caption", cap);
    }
    gtk_widget_set_visible(cap, caption);
    _body_extras(module, cap, TRUE);
    if(!was)
    {
      g_object_set_data(G_OBJECT(exp), "dt-lrp-flat", GINT_TO_POINTER(TRUE));
      gtk_widget_set_no_show_all(head, TRUE);
      gtk_widget_hide(head);
      dt_gui_add_class(exp, "dt_lrp_flat");
    }
    // open, without taking the focus (a focused crop would start cropping)
    if(!module->expanded || !dtgtk_expander_get_expanded(DTGTK_EXPANDER(exp)))
    {
      module->expanded = TRUE;
      dtgtk_expander_set_expanded_no_scroll(DTGTK_EXPANDER(exp), TRUE);
    }
  }
  else if(was)
  {
    g_object_set_data(G_OBJECT(exp), "dt-lrp-flat", NULL);
    gtk_widget_set_no_show_all(head, FALSE);
    gtk_widget_show(head);
    if(cap) gtk_widget_hide(cap);
    _body_extras(module, cap, FALSE);
    dt_gui_remove_class(exp, "dt_lrp_flat");
    if(darktable.develop->gui_module == module) dt_iop_request_focus(NULL);
    module->expanded = FALSE;
    dtgtk_expander_set_expanded_no_scroll(DTGTK_EXPANDER(exp), FALSE);
  }
}

// the header and the modules of a menu (develop: a panel) from pos
static int _section_update(GtkBox *panel, const int tab, const int s, int pos)
{
  GList *modules = _section_modules(tab, s);
  int shown = 0;
  gboolean edited = FALSE;
  for(const GList *m = modules; m; m = g_list_next(m))
  {
    dt_iop_module_t *module = m->data;
    if(_visible_in_tab(tab, module))
    {
      shown++;
      if(module->enabled && _in_history(module)) edited = TRUE;
    }
  }

  gboolean open = FALSE;
  if(shown > 0)
  {
    GtkWidget *h = _header(tab, s);
    gchar *key = _conf_key(tab, s);
    open = dt_conf_get_bool(key);
    g_free(key);
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(h), "mark")), edited ? "●" : "");
    gtk_box_reorder_child(panel, h, pos++);
    gtk_widget_show(h);
  }
  else if(_headers[tab][s])
    gtk_widget_hide(_headers[tab][s]);

  // develop: the controls of the panel, all tools: the modules of the menu
  const gboolean panels = tab == 0;
  for(const GList *m = modules; m; m = g_list_next(m))
  {
    dt_iop_module_t *module = m->data;
    gtk_box_reorder_child(panel, module->expander, pos++);
    // the modules of a panel stay open when it closes, it only hides them
    const gboolean in_tab = _visible_in_tab(tab, module);
    const gboolean visible = open && in_tab;
    _flatten(module, panels && in_tab, shown > 1);
    if(visible)
      gtk_widget_show(module->expander);
    else
      _hide_module(module);
    if(panels) dt_gui_remove_class(module->expander, "dt_lrp_menu_item");
    else dt_gui_add_class(module->expander, "dt_lrp_menu_item");
  }
  g_list_free(modules);
  return pos;
}

void dt_lrp_update(const int tab)
{
  if(!darktable.gui || !darktable.develop || !darktable.develop->gui_attached) return;
  _tab = tab;
  GtkBox *panel = dt_ui_get_container(darktable.gui->ui, DT_UI_CONTAINER_PANEL_RIGHT_CENTER);

  if(tab < 0 || tab >= DT_LRP_TABS)
  {
    // no menus: the modules with their headers, in the order of the panels
    _hide_headers(-1);
    int pos = _pin_top(panel, 0);
    GList *modules = g_list_sort(g_list_reverse(g_list_copy(darktable.develop->iop)), _flat_cmp);
    for(const GList *m = modules; m; m = g_list_next(m))
    {
      dt_iop_module_t *module = m->data;
      if(!module->expander) continue;
      _flatten(module, FALSE, FALSE);
      dt_gui_remove_class(module->expander, "dt_lrp_menu_item");
      gtk_box_reorder_child(panel, module->expander, pos++);
    }
    g_list_free(modules);
    return;
  }

  _hide_headers(tab);
  const _tab_t *t = &_tabs[tab];
  int pos = 0;
  // develop: the tools of Lightroom's tool strip above the basic panel
  for(int s = 0; s < t->n; s++)
    if(t->sections[s].top) pos = _section_update(panel, tab, s, pos);
  pos = _pin_top(panel, pos);
  for(int s = 0; s < t->n; s++)
    if(!t->sections[s].top) pos = _section_update(panel, tab, s, pos);

  // the other modules are not in this tab
  for(const GList *m = g_list_last(darktable.develop->iop); m; m = g_list_previous(m))
  {
    dt_iop_module_t *module = m->data;
    if(module->expander && _section(tab, module) < 0)
    {
      gtk_box_reorder_child(panel, module->expander, pos++);
      _flatten(module, FALSE, FALSE);
      _hide_module(module);
    }
  }

  // the darkroom and another tab start at the top of the panel (darktable
  // would scroll to the module that was open last)
  if(tab != _shown_tab)
  {
    _shown_tab = tab;
    dtgtk_expander_cancel_scroll();
    GtkWidget *sw = gtk_widget_get_ancestor(GTK_WIDGET(panel), GTK_TYPE_SCROLLED_WINDOW);
    if(sw) gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw)), 0);
  }
}

void dt_lrp_layout(void)
{
  dt_lrp_update(_tab);
}

void dt_lrp_reveal(const int tab, dt_iop_module_t *module)
{
  if(!module || tab < 0 || tab >= DT_LRP_TABS) return;
  const int s = _section(tab, module);
  if(s < 0) return;
  gchar *key = _conf_key(tab, s);
  if(!dt_conf_get_bool(key))
  {
    dt_conf_set_bool(key, TRUE);
    _header_paint(tab, s);
  }
  g_free(key);
}

void dt_lrp_cleanup(void)
{
  _shown_tab = -1;
  for(int tab = 0; tab < DT_LRP_TABS; tab++)
    for(int s = 0; s < MAX_SECTIONS; s++)
      if(_headers[tab][s])
      {
        gtk_widget_destroy(_headers[tab][s]);
        _headers[tab][s] = NULL;
      }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
