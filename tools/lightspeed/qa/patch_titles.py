import re

def sub_all(p, pairs):
    s = open(p, encoding='utf-8').read()
    for old, new in pairs:
        assert old in s, (p, old)
        s = s.replace(old, new)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)

ls = r'C:\lightspeed\darktable\src\develop\lightspeed.c'
sub_all(ls, [(f'N_("{w}")', f'N_("{w.capitalize()}")') for w in
             ["temp", "tint", "exposure", "contrast", "highlights", "shadows", "whites",
              "blacks", "texture", "clarity", "dehaze", "vibrance", "saturation"]])

bp = r'C:\lightspeed\darktable\src\libs\basicpanel.c'
sub_all(bp, [
    ('N_("as shot"),     0.0f', 'N_("As Shot"),     0.0f'),
    ('N_("daylight")', 'N_("Daylight")'),
    ('N_("cloudy")', 'N_("Cloudy")'),
    ('N_("shade")', 'N_("Shade")'),
    ('N_("tungsten")', 'N_("Tungsten")'),
    ('N_("fluorescent")', 'N_("Fluorescent")'),
    ('N_("flash")', 'N_("Flash")'),
    ('N_("custom")', 'N_("Custom")'),
    ('gtk_toggle_button_new_with_label(_("color"))', 'gtk_toggle_button_new_with_label(_("Color"))'),
    ('gtk_toggle_button_new_with_label(_("black & white"))', 'gtk_toggle_button_new_with_label(_("Black & White"))'),
    ('gtk_label_new(_("treatment :"))', 'gtk_label_new(_("Treatment :"))'),
    ('dt_bauhaus_widget_set_label(d->wb_combo, NULL, N_("WB"));', 'dt_bauhaus_widget_set_label(d->wb_combo, NULL, N_("WB :"));'),
    ('dt_action_button_new(self, N_("auto"), _auto_tone_clicked', 'dt_action_button_new(self, N_("Auto"), _auto_tone_clicked'),
    ('_section(C_("section", "tone"))', '_section(C_("section", "Tone"))'),
    ('_section(C_("section", "presence"))', '_section(C_("section", "Presence"))'),
])

cm = r'C:\lightspeed\darktable\src\libs\CMakeLists.txt'
sub_all(cm, [
    (' ioporder basicpanel)', ' ioporder basicpanel panelbuttons_left panelbuttons_right)'),
    ('add_library(basicpanel MODULE "basicpanel.c")',
     'add_library(basicpanel MODULE "basicpanel.c")\nadd_library(panelbuttons_left MODULE "panelbuttons_left.c")\nadd_library(panelbuttons_right MODULE "panelbuttons_right.c")'),
])

css = r'C:\lightspeed\darktable\data\themes\lightspeed.css'
s = open(css, encoding='utf-8').read()
s += '''
/* ------------------------------------ Lightroom panel bottom buttons */

#panel-buttons
{
  background-color: @ls_window;
  padding: 0.35em 0.2em;
}

#panel-buttons button
{
  background-color: @ls_button;
  border: 1px solid #6a6a6a;
  border-radius: 2px;
  color: #e0e0e0;
  padding: 0.25em 0.5em;
  margin: 0 0.25em;
}

#panel-buttons button:hover
{
  background-color: #6e6e6e;
}

/* "Auto" in the basic panel: a plain text button */
#basicpanel-auto
{
  background-color: transparent;
  border: none;
  color: #d8d8d8;
  padding: 0 0.4em;
}

#basicpanel-auto:hover
{
  color: #ffffff;
}

#basicpanel-picker
{
  margin-right: 0.3em;
}
'''
open(css, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
