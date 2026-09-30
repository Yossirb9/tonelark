/*
    This file is part of Tonelark, a darktable fork.

    Tonelark is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

// The menu bar at the top of the window, like the one of Lightroom Classic:
// File, Edit, Library, Photo, View, Help. Each item runs the action of its
// keyboard shortcut, and shows the shortcut.

#pragma once

#include <gtk/gtk.h>

GtkWidget *dt_lrmenu_new(void);
