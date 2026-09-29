/*
    This file is part of Tonelark, a darktable fork.

    Tonelark is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

// The processing modules of the darkroom shown like the Lightroom develop
// panels: a "develop" tab with the Lightroom panels (tone curve, HSL / color,
// color grading, detail, lens corrections, transform, effects, calibration,
// healing) under the Lightroom names, and an "all tools" tab with every
// module in collapsible menus by task.

#pragma once

#include <glib.h>

struct dt_iop_module_t;

#define DT_LRP_TABS 2

// Lightroom names for the modules and the Lightroom panel layout (preference)
gboolean dt_lrp_names(void);
// the panels are shown: the preference and the Tonelark module group layout
gboolean dt_lrp_enabled(void);

// the Lightroom name of a module (translated), NULL if it keeps its name
const char *dt_lrp_module_title(const char *op);
// a module name in title case like the Lightroom panels ("rgb levels" -> "RGB Levels")
gchar *dt_lrp_title_case(const char *name);

// the tabs of the Tonelark module group layout, for its preset: name and modules
const char *dt_lrp_tab_name(const int tab);
void dt_lrp_tab_ops(const int tab, void (*add)(const char *op, gpointer data), gpointer data);

// the module belongs to the tab (0-based), the develop tab shows the older
// modules of a panel (classic crop, spot removal...) only when the photo uses them
gboolean dt_lrp_in_tab(const int tab, struct dt_iop_module_t *module);

// order the right panel for the tab (-1: no tab, the modules in panel order
// without menus), show the menu headers and hide the modules of closed menus.
// call it after the visibility of the modules is set.
void dt_lrp_update(const int tab);
// order the right panel again for the current tab (the modules changed)
void dt_lrp_layout(void);
// open the menu of a module in the tab
void dt_lrp_reveal(const int tab, struct dt_iop_module_t *module);
// remove the menu headers (leaving the darkroom)
void dt_lrp_cleanup(void);
