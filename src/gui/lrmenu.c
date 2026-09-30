/*
    This file is part of Tonelark, a darktable fork.

    Tonelark is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "gui/lrmenu.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "control/control.h"
#include "gui/about.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/preferences.h"
#include "libs/lib.h"
#include "views/view.h"

// what an item does: an action (the path of its shortcut, with its element
// and effect), or one of the special ones
typedef enum _special_t
{
  _ACTION = 0,
  _UNDO,
  _REDO,
  _EXPORT_DIALOG,
  _PREFERENCES,
  _SHORTCUTS,
  _GUIDE,
  _ABOUT,
} _special_t;

typedef struct _item_t
{
  const char *label;          // NULL: a separator
  _special_t special;
  const char *path, *element, *effect;
  guint key;                  // the shortcut, shown only
  GdkModifierType mods;
  gboolean library;           // runs in the library (the modules of the lighttable)
} _item_t;

#define SEP { NULL }
#define CTRL GDK_CONTROL_MASK
#define SHIFT GDK_SHIFT_MASK

static const _item_t _file[] = {
  { N_("Import Photos..."), _ACTION, "lib/import/add to library...", NULL, NULL, GDK_KEY_i, CTRL | SHIFT, TRUE },
  { N_("Copy & Import..."), _ACTION, "lib/import/copy & import...", NULL, NULL, 0, 0, TRUE },
  { N_("Open Lightroom Catalog..."), _ACTION, "lib/import/Lightroom catalog...", NULL, NULL, 0, 0, TRUE },
  SEP,
  { N_("Export..."), _EXPORT_DIALOG, NULL, NULL, NULL, 0, 0, TRUE },
  { N_("Export with Previous Settings"), _ACTION, "lib/export/start export", NULL, NULL, GDK_KEY_e, CTRL | SHIFT,
    TRUE },
  SEP,
  { N_("Quit"), _ACTION, "global/quit", NULL, NULL, GDK_KEY_q, CTRL, FALSE },
  { NULL, -1 } };

static const _item_t _edit[] = {
  { N_("Undo"), _UNDO, NULL, NULL, NULL, GDK_KEY_z, CTRL, FALSE },
  { N_("Redo"), _REDO, NULL, NULL, NULL, GDK_KEY_z, CTRL | SHIFT, FALSE },
  SEP,
  { N_("Select All"), _ACTION, "lib/select/select all", NULL, NULL, GDK_KEY_a, CTRL, TRUE },
  { N_("Select None"), _ACTION, "lib/select/select none", NULL, NULL, GDK_KEY_a, CTRL | SHIFT, TRUE },
  { N_("Invert Selection"), _ACTION, "lib/select/invert selection", NULL, NULL, GDK_KEY_i, CTRL, TRUE },
  SEP,
  { N_("Copy Settings"), _ACTION, "views/thumbtable/copy history", NULL, NULL, GDK_KEY_c, CTRL, FALSE },
  { N_("Copy Some Settings..."), _ACTION, "views/thumbtable/copy history parts", NULL, NULL, GDK_KEY_c,
    CTRL | SHIFT, FALSE },
  { N_("Paste Settings"), _ACTION, "views/thumbtable/paste history", NULL, NULL, GDK_KEY_v, CTRL, FALSE },
  SEP,
  { N_("Preferences..."), _PREFERENCES },
  { N_("Keyboard Shortcuts..."), _SHORTCUTS },
  { NULL, -1 } };

static const _item_t _library[] = {
  { N_("Find People"), _ACTION, "lib/people/Find People", NULL, NULL, 0, 0, TRUE },
  SEP,
  { N_("Find Best Shots"), _ACTION, "lib/aicull/Find Best Shots", NULL, NULL, 0, 0, TRUE },
  { N_("Rate with AI"), _ACTION, "lib/aicull/Rate with AI", NULL, NULL, 0, 0, TRUE },
  { N_("Best Take"), _ACTION, "lib/aicull/Best Take", NULL, NULL, 0, 0, TRUE },
  { NULL, -1 } };

static const _item_t _photo[] = {
  { N_("Open in Develop"), _ACTION, "global/switch views/darkroom", NULL, NULL, GDK_KEY_d, 0, FALSE },
  SEP,
  { N_("Flag as Pick"), _ACTION, "views/thumbtable/flag as pick", NULL, NULL, GDK_KEY_p, 0, FALSE },
  { N_("Unflag"), _ACTION, "views/thumbtable/unflag", NULL, NULL, GDK_KEY_u, 0, FALSE },
  { N_("Flag as Rejected"), _ACTION, "views/thumbtable/flag as rejected", NULL, NULL, GDK_KEY_x, 0, FALSE },
  SEP,
  { N_("No Stars"), _ACTION, "views/thumbtable/rating", "zero", NULL, GDK_KEY_0, 0, FALSE },
  { N_("1 Star"), _ACTION, "views/thumbtable/rating", "one", NULL, GDK_KEY_1, 0, FALSE },
  { N_("2 Stars"), _ACTION, "views/thumbtable/rating", "two", NULL, GDK_KEY_2, 0, FALSE },
  { N_("3 Stars"), _ACTION, "views/thumbtable/rating", "three", NULL, GDK_KEY_3, 0, FALSE },
  { N_("4 Stars"), _ACTION, "views/thumbtable/rating", "four", NULL, GDK_KEY_4, 0, FALSE },
  { N_("5 Stars"), _ACTION, "views/thumbtable/rating", "five", NULL, GDK_KEY_5, 0, FALSE },
  SEP,
  { N_("Red Label"), _ACTION, "views/thumbtable/color label", "red", NULL, GDK_KEY_6, 0, FALSE },
  { N_("Yellow Label"), _ACTION, "views/thumbtable/color label", "yellow", NULL, GDK_KEY_7, 0, FALSE },
  { N_("Green Label"), _ACTION, "views/thumbtable/color label", "green", NULL, GDK_KEY_8, 0, FALSE },
  { N_("Blue Label"), _ACTION, "views/thumbtable/color label", "blue", NULL, GDK_KEY_9, 0, FALSE },
  { N_("Purple Label"), _ACTION, "views/thumbtable/color label", "purple", NULL, 0, 0, FALSE },
  { N_("No Label"), _ACTION, "views/thumbtable/color label", "clear", NULL, 0, 0, FALSE },
  SEP,
  { N_("Rotate Left"), _ACTION, "lib/image/rotate selected images 90 degrees CCW", NULL, NULL, 0, 0, TRUE },
  { N_("Rotate Right"), _ACTION, "lib/image/rotate selected images 90 degrees CW", NULL, NULL, 0, 0, TRUE },
  { N_("Create Virtual Copy"), _ACTION, "lib/image/duplicate", NULL, NULL, GDK_KEY_apostrophe, CTRL, TRUE },
  { N_("Remove from Library..."), _ACTION, "lib/image/remove", NULL, NULL, 0, 0, TRUE },
  SEP,
  // stacks: the groups of darktable (a burst, a RAW+JPEG shot)
  { N_("Group into Stack"), _ACTION, "lib/image/group", NULL, NULL, GDK_KEY_g, CTRL, TRUE },
  { N_("Unstack"), _ACTION, "lib/image/ungroup", NULL, NULL, GDK_KEY_g, CTRL | SHIFT, TRUE },
  { N_("Collapse All Stacks"), _ACTION, "global/grouping", NULL, "on", 0, 0, FALSE },
  { N_("Expand All Stacks"), _ACTION, "global/grouping", NULL, "off", 0, 0, FALSE },
  { NULL, -1 } };

static const _item_t _view[] = {
  { N_("Library"), _ACTION, "global/switch views/lighttable", NULL, NULL, GDK_KEY_g, 0, FALSE },
  { N_("Develop"), _ACTION, "global/switch views/darkroom", NULL, NULL, GDK_KEY_d, 0, FALSE },
  { N_("Slideshow"), _ACTION, "global/switch views/slideshow", NULL, NULL, 0, 0, FALSE },
  SEP,
  { N_("Compare"), _ACTION, "views/lighttable/toggle culling mode", NULL, "toggle", GDK_KEY_c, 0, TRUE },
  SEP,
  { N_("Show or Hide the Side Panels"), _ACTION, "global/panels/all", NULL, NULL, GDK_KEY_Tab, 0, FALSE },
  { N_("Show or Hide the Filmstrip"), _ACTION, "global/panels/filmstrip and timeline", NULL, NULL, GDK_KEY_f,
    CTRL, FALSE },
  { N_("Full Screen"), _ACTION, "global/fullscreen", NULL, NULL, GDK_KEY_F11, 0, FALSE },
  { NULL, -1 } };

static const _item_t _help[] = {
  { N_("Tonelark Guide (Hebrew)"), _GUIDE },
  { N_("Keyboard Shortcuts..."), _SHORTCUTS },
  SEP,
  { N_("About Tonelark"), _ABOUT },
  { NULL, -1 } };

static gboolean _in_library(void)
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  return cv && !g_strcmp0(cv->module_name, "lighttable");
}

static const char *_view_name(void)
{
  const dt_view_t *cv = dt_view_manager_get_current_view(darktable.view_manager);
  return cv ? cv->module_name : "lighttable";
}

static void _shortcuts_dialog(void)
{
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("shortcuts"), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
                                                  GTK_DIALOG_DESTROY_WITH_PARENT, NULL, NULL);
  gtk_window_set_default_size(GTK_WINDOW(dialog),
                              DT_PIXEL_APPLY_DPI(dt_conf_get_int("ui_last/shortcuts_dialog_width")),
                              DT_PIXEL_APPLY_DPI(dt_conf_get_int("ui_last/shortcuts_dialog_height")));
  dt_gui_dialog_add(GTK_DIALOG(dialog), dt_shortcuts_prefs(NULL));
  gtk_widget_show_all(dialog);
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

static void _guide(void)
{
  char share[PATH_MAX] = { 0 };
  dt_loc_get_sharedir(share, sizeof(share));
  gchar *path = g_build_filename(share, "doc", "lightspeed", "guide.html", NULL);
  gchar *uri = g_filename_to_uri(path, NULL, NULL);
  if(uri && g_file_test(path, G_FILE_TEST_EXISTS))
    dt_open_url(uri);
  else
    dt_control_log(_("the guide is not installed: %s"), path);
  g_free(uri);
  g_free(path);
}

// like the Export... button of the library: the export settings on the right
static void _export_dialog(void)
{
  dt_action_process("lib/panelbuttons_left/Export...", 0, NULL, NULL, 1.0f);
}

static void _run(const _item_t *it)
{
  switch(it->special)
  {
    case _UNDO:
    case _REDO:
    {
      gchar *path = g_strdup_printf("views/%s/%s", _view_name(), it->special == _UNDO ? "undo" : "redo");
      dt_action_process(path, 0, NULL, NULL, 1.0f);
      g_free(path);
      break;
    }
    case _EXPORT_DIALOG:
      _export_dialog();
      break;
    case _PREFERENCES:
      dt_gui_preferences_show();
      break;
    case _SHORTCUTS:
      _shortcuts_dialog();
      break;
    case _GUIDE:
      _guide();
      break;
    case _ABOUT:
      darktable_show_about_dialog();
      break;
    default:
      dt_action_process(it->path, 0, it->element, it->effect, 1.0f);
  }
}

static gboolean _run_later(gpointer data)
{
  _run(data);
  return G_SOURCE_REMOVE;
}

static void _activated(GtkMenuItem *mi, gpointer data)
{
  const _item_t *it = data;
  // the tools of the library: go there first, like Lightroom does
  if(it->library && !_in_library())
  {
    dt_ctl_switch_mode_to("lighttable");
    g_timeout_add(400, _run_later, data);
  }
  else
    _run(it);
}

static GtkWidget *_menu(GtkWidget *bar, const char *title, const _item_t *items)
{
  GtkWidget *top = gtk_menu_item_new_with_mnemonic(_(title));
  GtkWidget *menu = gtk_menu_new();
  gtk_widget_set_name(menu, "tonelark-menu");
  for(const _item_t *it = items; it->label || it->special != (_special_t)-1; it++)
  {
    GtkWidget *mi;
    if(!it->label)
      mi = gtk_separator_menu_item_new();
    else
    {
      mi = gtk_menu_item_new_with_label(_(it->label));
      // the shortcut, shown on the right (the shortcuts themselves are darktable's)
      if(it->key)
        gtk_accel_label_set_accel(GTK_ACCEL_LABEL(gtk_bin_get_child(GTK_BIN(mi))), it->key, it->mods);
      g_signal_connect(mi, "activate", G_CALLBACK(_activated), (gpointer)it);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
  }
  gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), menu);
  gtk_menu_shell_append(GTK_MENU_SHELL(bar), top);
  return top;
}

// QA: with TONELARK_MENU_SELFTEST set, the actions of the items are looked up
// once the modules are loaded (nothing is run) and the missing ones written to
// <configdir>/menu_selftest.txt
static gboolean _selftest(gpointer data)
{
  const _item_t *menus[] = { _file, _edit, _library, _photo, _view, _help };
  GString *out = g_string_new("");
  int missing = 0;
  for(int m = 0; m < G_N_ELEMENTS(menus); m++)
    for(const _item_t *it = menus[m]; it->label || it->special != (_special_t)-1; it++)
    {
      if(!it->label || it->special != _ACTION) continue;
      gchar **path = g_strsplit(it->path, "/", 0);
      const gboolean found = dt_action_locate(NULL, path, FALSE) != NULL;
      g_strfreev(path);
      missing += !found;
      g_string_append_printf(out, "%s\t%s\t%s\n", found ? "ok" : "MISSING", it->label, it->path);
    }
  g_string_append_printf(out, "missing %d\n", missing);
  gchar *file = g_build_filename(darktable.configdir, "menu_selftest.txt", NULL);
  g_file_set_contents(file, out->str, -1, NULL);
  g_free(file);
  g_string_free(out, TRUE);
  return G_SOURCE_REMOVE;
}

GtkWidget *dt_lrmenu_new(void)
{
  // (late: the modules are loaded, and the thumbnails of the library made)
  if(g_getenv("TONELARK_MENU_SELFTEST")) g_timeout_add_seconds(150, _selftest, NULL);
  GtkWidget *bar = gtk_menu_bar_new();
  gtk_widget_set_name(bar, "tonelark-menubar");
  _menu(bar, N_("_File"), _file);
  _menu(bar, N_("_Edit"), _edit);
  _menu(bar, N_("_Library"), _library);
  _menu(bar, N_("_Photo"), _photo);
  _menu(bar, N_("_View"), _view);
  _menu(bar, N_("_Help"), _help);
  gtk_widget_show_all(bar);
  return bar;
}
