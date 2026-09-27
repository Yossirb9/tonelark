p = r'C:\lightspeed\darktable\src\common\darktable.c'
s = open(p, encoding='utf-8').read()

old_call = '''    if(lightroom_catalog)
    {
      GError *error = NULL;
      dt_lrcat_t *cat = dt_lrcat_open(lightroom_catalog, &error);
      if(cat)
      {
        const dt_lrcat_options_t options = { .develop = TRUE, .metadata = TRUE,
                                             .keywords = TRUE, .collections = TRUE,
                                             .stacks = TRUE };
        dt_lrcat_import(cat, &options, FALSE);
      }
      else
      {
        dt_print(DT_DEBUG_ALWAYS, "[lightroom] %s", error ? error->message : lightroom_catalog);
        g_clear_error(&error);
      }
    }
'''
new_call = '''    // import once the user interface is running
    if(lightroom_catalog)
      g_timeout_add(1000, _import_lightroom_catalog, g_strdup(lightroom_catalog));
'''
assert old_call in s
s = s.replace(old_call, new_call, 1)

helper = '''static gboolean _import_lightroom_catalog(gpointer data)
{
  gchar *filename = data;
  GError *error = NULL;
  dt_lrcat_t *cat = dt_lrcat_open(filename, &error);
  if(cat)
  {
    const dt_lrcat_options_t options = { .develop = TRUE, .metadata = TRUE,
                                         .keywords = TRUE, .collections = TRUE,
                                         .stacks = TRUE };
    dt_lrcat_import(cat, &options, FALSE);
  }
  else
  {
    dt_print(DT_DEBUG_ALWAYS, "[lightroom] %s", error ? error->message : filename);
    g_clear_error(&error);
  }
  g_free(filename);
  return G_SOURCE_REMOVE;
}

int dt_init(int argc,'''
assert 'int dt_init(int argc,' in s
s = s.replace('int dt_init(int argc,', helper, 1)
open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
