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
  The flag filter of the filtering module, like the flag filter of the
  Lightroom library: picked (the tag darktable|pick), unflagged, rejected.
*/

typedef struct _widgets_flag_t
{
  dt_lib_filtering_rule_t *rule;

  GtkWidget *combo;
} _widgets_flag_t;

typedef enum _flag_type_t
{
  _FLAG_ALL = 0,
  _FLAG_PICKED,
  _FLAG_UNFLAGGED,
  _FLAG_REJECTED,
  _FLAG_NOT_REJECTED
} _flag_type_t;

static const char *_flag_names[]
    = { N_("all images"), N_("picked"), N_("unflagged"), N_("rejected"), N_("all but rejected"), NULL };
static const char *_flag_codes[] = { "", "$PICKED", "$UNFLAGGED", "$REJECTED", "$NOT_REJECTED" };

static void _flag_synchronise(_widgets_flag_t *source)
{
  _widgets_flag_t *dest = source == source->rule->w_specific_top ? source->rule->w_specific
                                                                  : source->rule->w_specific_top;
  if(dest)
  {
    source->rule->manual_widget_set++;
    dt_bauhaus_combobox_set(dest->combo, dt_bauhaus_combobox_get(source->combo));
    source->rule->manual_widget_set--;
  }
}

static int _flag_decode(const gchar *txt)
{
  for(int k = 1; k < G_N_ELEMENTS(_flag_codes); k++)
    if(!g_strcmp0(txt, _flag_codes[k])) return k;
  return _FLAG_ALL;
}

static void _flag_changed(GtkWidget *widget, gpointer user_data)
{
  _widgets_flag_t *flag = (_widgets_flag_t *)user_data;
  if(flag->rule->manual_widget_set) return;

  const int k = CLAMP(dt_bauhaus_combobox_get(flag->combo), 0, G_N_ELEMENTS(_flag_codes) - 1);
  _rule_set_raw_text(flag->rule, _flag_codes[k], TRUE);
  _flag_synchronise(flag);
}

static gboolean _flag_update(dt_lib_filtering_rule_t *rule)
{
  if(!rule->w_specific) return FALSE;
  const int val = _flag_decode(rule->raw_text);

  rule->manual_widget_set++;
  _widgets_flag_t *flag = (_widgets_flag_t *)rule->w_specific;

  // the number of photos of each kind in the rest of the collection
  char query[1024] = { 0 };
  // clang-format off
  g_snprintf(query, sizeof(query),
             "SELECT CASE"
             "         WHEN (flags & %d) THEN 2"
             "         WHEN id IN (SELECT ti.imgid FROM main.tagged_images AS ti"
             "                     JOIN data.tags AS t ON t.id = ti.tagid"
             "                     WHERE t.name = 'darktable|pick') THEN 0"
             "         ELSE 1"
             "       END AS fl, COUNT(*) AS count"
             " FROM main.images AS mi"
             " WHERE %s"
             " GROUP BY fl ORDER BY fl ASC",
             DT_IMAGE_REJECTED, rule->lib->last_where_ext);
  // clang-format on
  int counts[3] = { 0 };
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int i = sqlite3_column_int(stmt, 0);
    if(i >= 0 && i < 3) counts[i] = sqlite3_column_int(stmt, 1);
  }
  sqlite3_finalize(stmt);

  const int shown[] = { counts[0], counts[1], counts[2], counts[0] + counts[1] };
  for(int i = 0; i < 4; i++)
  {
    gchar *item = g_strdup_printf("%s (%d)", _(_flag_names[i + 1]), shown[i]);
    dt_bauhaus_combobox_set_entry_label(flag->combo, i + 1, item);
    g_free(item);
  }

  dt_bauhaus_combobox_set(flag->combo, val);
  _flag_synchronise(flag);
  rule->manual_widget_set--;

  return TRUE;
}

static void _flag_widget_init(dt_lib_filtering_rule_t *rule, const dt_collection_properties_t prop,
                              const gchar *text, dt_lib_module_t *self, const gboolean top)
{
  _widgets_flag_t *flag = g_malloc0(sizeof(_widgets_flag_t));
  flag->rule = rule;

  flag->combo = dt_bauhaus_combobox_new_full(DT_ACTION(self), N_("rules"), N_("flag"),
                                             _("show the picked, unflagged or rejected photos"), 0,
                                             (GtkCallback)_flag_changed, flag, _flag_names);
  dt_bauhaus_widget_hide_label(flag->combo);

  gtk_box_pack_start(GTK_BOX(top ? rule->w_special_box_top : rule->w_special_box), flag->combo, TRUE, TRUE, 0);
  if(top) dt_gui_add_class(flag->combo, "dt_quick_filter");

  if(top)
    rule->w_specific_top = flag;
  else
    rule->w_specific = flag;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
