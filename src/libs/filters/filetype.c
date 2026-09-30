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
  The file type filter of the filtering module, like the file type of the
  metadata filter of the Lightroom library: RAW, JPEG, HEIF, TIFF... The list
  shows the types of the photos of the collection, with their number.
*/

typedef struct _widgets_filetype_t
{
  dt_lib_filtering_rule_t *rule;

  GtkWidget *combo;
} _widgets_filetype_t;

static const char *_filetype_names[] = { N_("all file types"), NULL };

// the entries: all (no data), then the types shown (data: type + 1)
static void _filetype_fill(_widgets_filetype_t *ft, const int *counts, const int val)
{
  if(!ft) return;
  dt_bauhaus_combobox_clear(ft->combo);
  dt_bauhaus_combobox_add_full(ft->combo, _("all file types"), DT_BAUHAUS_COMBOBOX_ALIGN_MIDDLE, NULL, NULL,
                               TRUE);
  for(int t = 0; t < DT_FILETYPE_LAST; t++)
  {
    if(!counts[t] && t != val) continue;
    gchar *item = g_strdup_printf("%s (%d)", dt_collection_filetype_name(t), counts[t]);
    dt_bauhaus_combobox_add_full(ft->combo, item, DT_BAUHAUS_COMBOBOX_ALIGN_MIDDLE, GINT_TO_POINTER(t + 1),
                                 NULL, TRUE);
    g_free(item);
  }
  if(val < 0 || !dt_bauhaus_combobox_set_from_value(ft->combo, val + 1)) dt_bauhaus_combobox_set(ft->combo, 0);
}

static void _filetype_changed(GtkWidget *widget, gpointer user_data)
{
  _widgets_filetype_t *ft = (_widgets_filetype_t *)user_data;
  if(ft->rule->manual_widget_set) return;

  const int type = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(ft->combo)) - 1;
  _rule_set_raw_text(ft->rule, type < 0 ? "" : dt_collection_filetype_code(type), TRUE);

  // the same choice in the other widget of the rule (the top bar or the module)
  _widgets_filetype_t *dest = ft == ft->rule->w_specific_top ? ft->rule->w_specific : ft->rule->w_specific_top;
  if(dest)
  {
    ft->rule->manual_widget_set++;
    if(type < 0 || !dt_bauhaus_combobox_set_from_value(dest->combo, type + 1))
      dt_bauhaus_combobox_set(dest->combo, 0);
    ft->rule->manual_widget_set--;
  }
}

static gboolean _filetype_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific && !rule->w_specific_top) return FALSE;
  const int val = dt_collection_filetype_from_code(rule->raw_text);

  // the number of photos of each type in the collection
  int counts[DT_FILETYPE_LAST] = { 0 };
  gchar *query = g_strdup_printf("SELECT ls_filetype(filename) AS ft, COUNT(*) AS count"
                                 " FROM main.images AS mi"
                                 " WHERE %s"
                                 " GROUP BY ft",
                                 rule->lib->last_where_ext && *rule->lib->last_where_ext
                                 ? rule->lib->last_where_ext : "1 = 1");
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int t = sqlite3_column_int(stmt, 0);
    if(t >= 0 && t < DT_FILETYPE_LAST) counts[t] = sqlite3_column_int(stmt, 1);
  }
  sqlite3_finalize(stmt);
  g_free(query);

  rule->manual_widget_set++;
  _filetype_fill(rule->w_specific, counts, val);
  _filetype_fill(rule->w_specific_top, counts, val);
  rule->manual_widget_set--;

  return TRUE;
}

static void _filetype_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                                  const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_filetype_t *ft = g_malloc0(sizeof(_widgets_filetype_t));
  ft->rule = rule;

  ft->combo = dt_bauhaus_combobox_new_full(DT_ACTION(self), N_("rules"), N_("file type"),
                                           _("show the photos of one file type: RAW, JPEG, HEIF..."), 0,
                                           (GtkCallback)_filetype_changed, ft, _filetype_names);
  dt_bauhaus_widget_hide_label(ft->combo);

  gtk_box_pack_start(GTK_BOX(top ? rule->w_special_box_top : rule->w_special_box), ft->combo, TRUE, TRUE, 0);
  if(top) dt_gui_add_class(ft->combo, "dt_quick_filter");

  if(top)
    rule->w_specific_top = ft;
  else
    rule->w_specific = ft;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
