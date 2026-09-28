/*
    This file is part of darktable,
    Copyright (C) 2013-2020 darktable developers.

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

#pragma once

#include "develop/imageop.h"

/* what to import from a Lightroom XMP packet */
typedef enum dt_lightroom_import_flags_t
{
  DT_LR_IMPORT_DEVELOP      = 1 << 0, // processing settings, needs a develop
  DT_LR_IMPORT_TAGS         = 1 << 1, // keywords
  DT_LR_IMPORT_RATING       = 1 << 2,
  DT_LR_IMPORT_LABEL        = 1 << 3, // color label
  DT_LR_IMPORT_GEOTAG       = 1 << 4,
  DT_LR_IMPORT_METADATA     = 1 << 5, // title, description, creator, rights
  DT_LR_IMPORT_FROM_CATALOG = 1 << 6, // packet read from a Lightroom catalog
  DT_LR_IMPORT_ALL_META     = DT_LR_IMPORT_TAGS | DT_LR_IMPORT_RATING | DT_LR_IMPORT_LABEL
                              | DT_LR_IMPORT_GEOTAG | DT_LR_IMPORT_METADATA
} dt_lightroom_import_flags_t;

/* Import some lightroom develop options
   When called from lightable : dev == NULL, in this case only the tags are imported
   When called from darkroom  : dev != NULL, in this case only develop data are imported
*/
gboolean dt_lightroom_import(dt_imgid_t imgid, dt_develop_t *dev, gboolean iauto);

/* import from an XMP packet in memory (e.g. read from a Lightroom catalog).
   the develop settings are appended to the image history, dev must be a develop
   with the image loaded when DT_LR_IMPORT_DEVELOP is requested */
gboolean dt_lightroom_import_xmp_buffer(const dt_imgid_t imgid,
                                        dt_develop_t *dev,
                                        const char *buffer,
                                        const size_t size,
                                        const dt_lightroom_import_flags_t flags);

/* change the edit of the image loaded in dev with Lightroom settings (Camera
   Raw names and values: Exposure2012, Temperature, HueAdjustmentRed...): only
   the given settings change, the other parameters of the modules are kept.
   New history items are added to dev, the caller writes the history. */
gboolean dt_lightroom_update_develop(dt_develop_t *dev, GHashTable *crs);

/* the current edit of the image loaded in dev as Lightroom settings (basic
   panel, HSL, color grading), a new hash table of Camera Raw names -> values */
GHashTable *dt_lightroom_read_develop(dt_develop_t *dev);

/* darktable color label (0 red .. 4 purple) of a Lightroom label text, -1 if unknown */
int dt_lightroom_color_label(const char *label);

/* returns NULL if not found, or g_strdup'ed pathname, the caller should g_free it. */
char *dt_get_lightroom_xmp(dt_imgid_t imgid);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

