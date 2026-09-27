def rep(p, old, new, count=1):
    s = open(p, encoding='utf-8').read()
    assert old in s, (p, old[:70])
    s = s.replace(old, new, count)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)

d = r'C:\lightspeed\darktable\src'

# paint function: Lightroom pick flag
rep(d + r'\dtgtk\paint.h',
    'void dtgtk_cairo_paint_altered(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);',
    'void dtgtk_cairo_paint_altered(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);\n'
    '/** Lightroom-style pick flag */\n'
    'void dtgtk_cairo_paint_pick_flag(cairo_t *cr, gint x, gint y, gint w, gint h, gint flags, void *data);')
rep(d + r'\dtgtk\paint.c',
    'void dtgtk_cairo_paint_tags(cairo_t *cr, const gint x, const gint y, const gint w, const gint h, gint flags, void *data)\n{',
    '''void dtgtk_cairo_paint_pick_flag(cairo_t *cr, const gint x, const gint y, const gint w, const gint h, gint flags, void *data)
{
  PREAMBLE(1, 1, 0, 0)

  // pole
  cairo_move_to(cr, 0.25, 0.08);
  cairo_line_to(cr, 0.25, 0.95);
  cairo_stroke(cr);
  // flag
  cairo_move_to(cr, 0.25, 0.1);
  cairo_line_to(cr, 0.85, 0.3);
  cairo_line_to(cr, 0.25, 0.52);
  cairo_close_path(cr);
  cairo_fill(cr);

  FINISH
}

void dtgtk_cairo_paint_tags(cairo_t *cr, const gint x, const gint y, const gint w, const gint h, gint flags, void *data)
{''')

# thumbnail: field and widget
rep(d + r'\dtgtk\thumbnail.h', '''  gboolean is_altered;
  gboolean has_audio;''', '''  gboolean is_altered;
  gboolean is_picked;   // Lightroom-style pick flag (tag darktable|pick)
  gboolean has_audio;''')
s = open(d + r'\dtgtk\thumbnail.h', encoding='utf-8').read()
import re
m = re.search(r'\n(\s*)GtkWidget \*w_altered;', s)
assert m, 'w_altered decl'
s = s.replace(m.group(0), m.group(0) + '\n' + m.group(1) + 'GtkWidget *w_pick;', 1)
open(d + r'\dtgtk\thumbnail.h', 'w', encoding='utf-8', newline='\n').write(s)

th = d + r'\dtgtk\thumbnail.c'
rep(th, '''  // altered
  thumb->is_altered = dt_image_altered(thumb->imgid);
''', '''  // altered
  thumb->is_altered = dt_image_altered(thumb->imgid);

  // Lightroom pick flag
  static guint pick_tag = 0;
  if(!pick_tag) dt_tag_exists("darktable|pick", &pick_tag);
  thumb->is_picked = pick_tag && dt_is_tag_attached(pick_tag, thumb->imgid);
''')
rep(th, '''  gtk_widget_set_visible(thumb->w_altered, thumb->is_altered);
  _thumb_update_tags_tooltip(thumb);''', '''  gtk_widget_set_visible(thumb->w_altered, thumb->is_altered);
  gtk_widget_set_visible(thumb->w_pick, thumb->is_picked);
  _thumb_update_tags_tooltip(thumb);''')
rep(th, '''  gtk_widget_hide(thumb->w_altered);
  gtk_widget_hide(thumb->w_tags);''', '''  gtk_widget_hide(thumb->w_altered);
  gtk_widget_hide(thumb->w_pick);
  gtk_widget_hide(thumb->w_tags);''')
rep(th, '''    // the tags icon
    thumb->w_tags = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_tags, 0, NULL);''', '''    // the Lightroom pick flag
    thumb->w_pick = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_pick_flag, 0, NULL);
    gtk_widget_set_name(thumb->w_pick, "thumb-pick");
    gtk_widget_set_tooltip_text(thumb->w_pick, _("picked (P), unflag with U"));
    gtk_widget_set_valign(thumb->w_pick, GTK_ALIGN_START);
    gtk_widget_set_halign(thumb->w_pick, GTK_ALIGN_START);
    gtk_widget_set_no_show_all(thumb->w_pick, TRUE);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlays_parent), thumb->w_pick);

    // the tags icon
    thumb->w_tags = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_tags, 0, NULL);''')
rep(th, '''    // the tags icon
    gtk_widget_set_size_request(thumb->w_tags, 2.0 * r1, 2.0 * r1);''', '''    // the pick flag, top left like Lightroom
    gtk_widget_set_size_request(thumb->w_pick, 2.0 * r1, 2.0 * r1);
    gtk_widget_set_margin_top(thumb->w_pick, thumb->img_margin->top);
    gtk_widget_set_margin_start(thumb->w_pick, thumb->img_margin->left);

    // the tags icon
    gtk_widget_set_size_request(thumb->w_tags, 2.0 * r1, 2.0 * r1);''')
s = open(th, encoding='utf-8').read()
if '#include "common/tags.h"' not in s:
    s = s.replace('#include "dtgtk/thumbnail.h"\n', '#include "dtgtk/thumbnail.h"\n#include "common/tags.h"\n', 1)
open(th, 'w', encoding='utf-8', newline='\n').write(s)

css = r'C:\lightspeed\darktable\data\themes\lightspeed.css'
s = open(css, encoding='utf-8').read()
s += '''
/* Lightroom pick flag on thumbnails */
#thumb-pick,
#thumb-main:hover #thumb-pick
{
  color: #f4f4f4;
  background-color: transparent;
}
'''
open(css, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
