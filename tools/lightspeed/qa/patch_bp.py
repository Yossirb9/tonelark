p = r'C:\lightspeed\darktable\src\libs\basicpanel.c'
s = open(p, encoding='utf-8').read()


def rep(old, new):
    global s
    assert old in s, old[:80]
    s = s.replace(old, new, 1)


rep('''#include "develop/lightspeed.h"
#include "gui/accelerators.h"''', '''#include "develop/lightspeed.h"
#include "develop/pixelpipe_hb.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "gui/accelerators.h"''')

rep('''typedef struct dt_lib_basicpanel_t
{
  GtkWidget *slider[DT_LSB_COUNT];
  GtkWidget *color_btn, *bw_btn;
  GtkWidget *asshot_btn, *picker_btn;
} dt_lib_basicpanel_t;''', '''typedef struct dt_lib_basicpanel_t
{
  GtkWidget *slider[DT_LSB_COUNT];
  GtkWidget *color_btn, *bw_btn;
  GtkWidget *wb_combo, *picker_btn, *auto_btn;
} dt_lib_basicpanel_t;

// Lightroom white balance presets for raw files
typedef enum _wb_preset_t
{
  WB_AS_SHOT = 0,
  WB_DAYLIGHT,
  WB_CLOUDY,
  WB_SHADE,
  WB_TUNGSTEN,
  WB_FLUORESCENT,
  WB_FLASH,
  WB_CUSTOM
} _wb_preset_t;

static const struct { const char *name; float temp, tint; } _wb_presets[] =
{
  { N_("as shot"),     0.0f,    0.0f },
  { N_("daylight"),    5500.0f, 10.0f },
  { N_("cloudy"),      6500.0f, 10.0f },
  { N_("shade"),       7500.0f, 10.0f },
  { N_("tungsten"),    2850.0f, 0.0f },
  { N_("fluorescent"), 3800.0f, 21.0f },
  { N_("flash"),       5500.0f, 0.0f },
  { N_("custom"),      0.0f,    0.0f },
};''')

rep('''  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bw_btn), bw);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->color_btn), !bw);

  DT_LEAVE_GUI_UPDATE();
}''', '''  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->bw_btn), bw);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->color_btn), !bw);

  // white balance preset shown in the combobox
  int wb = WB_AS_SHOT;
  if(cc && cc->enabled && cc->so->get_p)
  {
    const int *illuminant = cc->so->get_p(cc->params, "illuminant");
    const int *def = cc->so->get_p(cc->default_params, "illuminant");
    const float *x = cc->so->get_p(cc->params, "x");
    const float *dx = cc->so->get_p(cc->default_params, "x");
    if(illuminant && def && x && dx && (*illuminant != *def || fabsf(*x - *dx) > 1e-4f))
    {
      wb = WB_CUSTOM;
      const float temp = dt_bauhaus_slider_get(d->slider[DT_LSB_TEMP]);
      const float tint = dt_bauhaus_slider_get(d->slider[DT_LSB_TINT]);
      for(int k = WB_DAYLIGHT; k < WB_CUSTOM; k++)
        if(fabsf(temp - _wb_presets[k].temp) < 26.0f && fabsf(tint - _wb_presets[k].tint) < 1.0f)
          wb = k;
    }
  }
  dt_bauhaus_combobox_set(d->wb_combo, wb);

  DT_LEAVE_GUI_UPDATE();
}''')

rep('''  _basic_commit(m, GTK_WIDGET(button), TRUE);
  _sync(self);
}

static void _picker_clicked(GtkButton *button, dt_lib_module_t *self)''', '''  _basic_commit(m, button ? GTK_WIDGET(button) : NULL, TRUE);
  _sync(self);
}

static void _wb_preset_changed(GtkWidget *w, dt_lib_module_t *self)
{
  DT_GUARD_GUI_UPDATE();
  dt_lib_basicpanel_t *d = self->data;
  const int k = dt_bauhaus_combobox_get(w);

  if(k == WB_AS_SHOT)
    _asshot_clicked(NULL, self);
  else if(k > WB_AS_SHOT && k < WB_CUSTOM)
  {
    // set the temperature quietly, the tint change then writes both
    DT_ENTER_GUI_UPDATE();
    dt_bauhaus_slider_set(d->slider[DT_LSB_TEMP], _wb_presets[k].temp);
    dt_bauhaus_slider_set(d->slider[DT_LSB_TINT], _wb_presets[k].tint + 1.0f);
    DT_LEAVE_GUI_UPDATE();
    dt_bauhaus_slider_set(d->slider[DT_LSB_TINT], _wb_presets[k].tint);
  }
}

// Lightroom "Auto" tone: look at the rendered preview and set exposure,
// highlights and shadows so that the image is reasonably exposed
static void _auto_tone_clicked(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_basicpanel_t *d = self->data;
  dt_dev_pixelpipe_t *pipe = darktable.develop->preview_pipe;
  if(!pipe) return;

  uint32_t hist[256] = { 0 };
  uint64_t n = 0;
  dt_pthread_mutex_lock(&pipe->backbuf_mutex);
  if(pipe->backbuf && pipe->backbuf_width > 0 && pipe->backbuf_height > 0)
  {
    const uint8_t *buf = pipe->backbuf;
    const size_t npix = (size_t)pipe->backbuf_width * pipe->backbuf_height;
    for(size_t k = 0; k < npix; k++)
    {
      // cairo BGRA
      const int l = (buf[4 * k + 0] * 18 + buf[4 * k + 1] * 183 + buf[4 * k + 2] * 55) >> 8;
      hist[MIN(l, 255)]++;
      n++;
    }
  }
  dt_pthread_mutex_unlock(&pipe->backbuf_mutex);
  if(n == 0)
  {
    dt_control_log(_("no preview available yet"));
    return;
  }

  // percentiles of the display luminance
  int p10 = 0, p50 = 0;
  uint64_t acc = 0;
  for(int k = 0; k < 256; k++)
  {
    acc += hist[k];
    if(acc < n / 10) p10 = k;
    if(acc < n / 2) p50 = k;
  }
  uint64_t clipped = 0;
  for(int k = 250; k < 256; k++) clipped += hist[k];
  const float clip = (float)clipped / n;

  // display value -> linear, bring the median to about 45% display gray
  const float lin50 = powf(MAX(p50, 1) / 255.0f, 2.2f);
  const float target = powf(0.45f, 2.2f);
  const float delta = CLAMP(0.8f * log2f(target / lin50), -2.0f, 2.0f);

  const float exposure = dt_bauhaus_slider_get(d->slider[DT_LSB_EXPOSURE]) + delta;
  const float highlights = clip > 0.005f ? -CLAMP(clip * 2000.0f, 10.0f, 80.0f) : 0.0f;
  const float shadows = p10 < 25 ? CLAMP((25 - p10) * 2.5f, 0.0f, 60.0f) : 0.0f;

  dt_bauhaus_slider_set(d->slider[DT_LSB_EXPOSURE], exposure);
  dt_bauhaus_slider_set(d->slider[DT_LSB_HIGHLIGHTS], highlights);
  dt_bauhaus_slider_set(d->slider[DT_LSB_SHADOWS], shadows);
}

static void _picker_clicked(GtkButton *button, dt_lib_module_t *self)''')

rep('''static GtkWidget *_section(const char *label)
{
  GtkWidget *l = dt_ui_section_label_new(label);
  gtk_label_set_xalign(GTK_LABEL(l), 0.0f);''', '''static GtkWidget *_section(const char *label)
{
  GtkWidget *l = dt_ui_section_label_new(label);
  gtk_label_set_xalign(GTK_LABEL(l), 0.5f);''')

rep('''  GtkWidget *treatment = gtk_label_new(_("treatment"));''',
    '''  GtkWidget *treatment = gtk_label_new(_("treatment :"));''')

rep('''      if(section == DT_LSB_SECTION_WB)
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
        dt_gui_box_add(self->widget, _section(C_("section", "presence")));''', '''      if(section == DT_LSB_SECTION_WB)
      {
        // eyedropper, "WB" and the white balance presets
        d->picker_btn = dtgtk_button_new(dtgtk_cairo_paint_colorpicker, 0, NULL);
        gtk_widget_set_tooltip_text(d->picker_btn,
                                    _("white balance selector: pick a neutral area of the image"));
        g_signal_connect(d->picker_btn, "clicked", G_CALLBACK(_picker_clicked), self);
        gtk_widget_set_name(d->picker_btn, "basicpanel-picker");

        d->wb_combo = dt_bauhaus_combobox_new_action(DT_ACTION(self));
        dt_bauhaus_widget_set_label(d->wb_combo, NULL, N_("WB"));
        for(int k = 0; k < G_N_ELEMENTS(_wb_presets); k++)
          dt_bauhaus_combobox_add(d->wb_combo, _(_wb_presets[k].name));
        gtk_widget_set_tooltip_text(d->wb_combo, _("white balance preset"));
        g_signal_connect(d->wb_combo, "value-changed", G_CALLBACK(_wb_preset_changed), self);
        dt_gui_box_add(self->widget, dt_gui_hbox(d->picker_btn, dt_gui_expand(d->wb_combo)));
      }
      else if(section == DT_LSB_SECTION_TONE)
      {
        d->auto_btn = dt_action_button_new(self, N_("auto"), _auto_tone_clicked, self,
                                           _("automatic exposure, highlights and shadows"), 0, 0);
        gtk_widget_set_name(d->auto_btn, "basicpanel-auto");
        dt_gui_box_add(self->widget,
                       dt_gui_hbox(dt_gui_expand(gtk_label_new("")),
                                   _section(C_("section", "tone")),
                                   dt_gui_expand(gtk_label_new("")), d->auto_btn));
      }
      else
        dt_gui_box_add(self->widget, _section(C_("section", "presence")));''')

open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
