def rep(p, old, new):
    s = open(p, encoding='utf-8').read()
    assert old in s, (p, old[:70])
    s = s.replace(old, new, 1)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)

src = r'C:\lightspeed\darktable\src\libs'

# Presets (darktable styles) in the develop left panel, below the navigator
rep(src + r'\styles.c', '''  return DT_VIEW_LIGHTTABLE | DT_VIEW_MULTI;''',
    '''  return DT_VIEW_LIGHTTABLE | DT_VIEW_DARKROOM | DT_VIEW_MULTI;''')
s = open(src + r'\styles.c', encoding='utf-8').read()
import re
m = re.search(r'int position\(const dt_lib_module_t \*self\)\n\{\n  return (\d+);\n\}', s)
assert m, 'styles position'
s = s.replace(m.group(0), '''int position(const dt_lib_module_t *self)
{
  // Lightroom: presets are the first panel under the navigator in develop
  return dt_view_get_current() == DT_VIEW_DARKROOM ? 1003 : %s;
}''' % m.group(1), 1)
m = re.search(r'const char \*name\(dt_lib_module_t \*self\)\n\{\n  return _\("styles"\);\n\}', s)
assert m, 'styles name'
s = s.replace(m.group(0), '''const char *name(dt_lib_module_t *self)
{
  // Lightroom calls them presets
  return _("presets");
}''', 1)
open(src + r'\styles.c', 'w', encoding='utf-8', newline='\n').write(s)

# Collections also in develop, below the history like Lightroom
rep(src + r'\collect.c', '''  return DT_VIEW_LIGHTTABLE | DT_VIEW_MAP | DT_VIEW_PRINT;''',
    '''  return DT_VIEW_LIGHTTABLE | DT_VIEW_DARKROOM | DT_VIEW_MAP | DT_VIEW_PRINT;''')
s = open(src + r'\collect.c', encoding='utf-8').read()
m = re.search(r'int position\(const dt_lib_module_t \*self\)\n\{\n  return (\d+);\n\}', s)
assert m, 'collect position'
s = s.replace(m.group(0), '''int position(const dt_lib_module_t *self)
{
  // Lightroom: collections come after snapshots and history in develop,
  // at the top of the library left panel
  return dt_view_get_current() == DT_VIEW_DARKROOM ? 890 : 1010;
}''', 1)
open(src + r'\collect.c', 'w', encoding='utf-8', newline='\n').write(s)

# the import module goes below, the "Import..." button is at the bottom of the panel
s = open(src + r'\import.c', encoding='utf-8').read()
m = re.search(r'int position\(const dt_lib_module_t \*self\)\n\{\n  return (\d+);\n\}', s)
assert m, 'import position'
s = s.replace(m.group(0), '''int position(const dt_lib_module_t *self)
{
  return 200;
}''', 1)
open(src + r'\import.c', 'w', encoding='utf-8', newline='\n').write(s)

# basic panel: sync once the preview is rendered (history fully loaded)
rep(src + r'\basicpanel.c', '''  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_INITIALIZE, _history_changed);''',
    '''  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_INITIALIZE, _history_changed);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED, _history_changed);''')

# css: padding inside the right panel, flat "Auto"
css = r'C:\lightspeed\darktable\data\themes\lightspeed.css'
s = open(css, encoding='utf-8').read()
s = s.replace('''#basicpanel
{
  padding: 0.2em 0.1em 0.5em 0.1em;
}''', '''#basicpanel
{
  padding: 0.3em 0.9em 0.6em 0.9em;
}''')
s = s.replace('''#basicpanel-auto
{''', '''#basicpanel #basicpanel-auto
{''')
s = s.replace('''#basicpanel-auto:hover
{''', '''#basicpanel #basicpanel-auto:hover
{''')
s += '''
/* breathing room between the controls and the panel edges */
.dt_plugin_ui_main
{
  padding: 0.3em 0.9em 0.5em 0.9em;
}
'''
open(css, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
