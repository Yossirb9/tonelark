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

#pragma once

#include "common/image.h"

#include <glib.h>
#include <json-glib/json-glib.h>

// the people of the photos (the faces found by the AI helper), see common/people.c

#define PERSON_TAG "darktable|tonelark|person|"
#define NAME_TAG "people|"

// the tables in the library
void dt_people_init(void);
// the name of a person, NULL unnamed (to free)
gchar *dt_people_name(const int person);
// the photo to look for faces in (the file itself, the jpeg of a raw file,
// else the thumbnail of the library written into dir), NULL none
gchar *dt_people_face_file(const dt_imgid_t id, const char *dir);
// the faces of a search into the library (the answer of the helper's "faces")
void dt_people_store(JsonObject *res);
// the faces of no one join the people or become new ones: the people changed
GHashTable *dt_people_cluster(void);
// the tags of the people on their photos
void dt_people_sync(GHashTable *people);
// a name (empty: none); the name of another person merges the two: that person, else 0
int dt_people_rename(const int person, const char *name);
// the photos given are not of the person, for good: the faces taken out
int dt_people_not(const int person, GList *imgs);
void dt_people_hide(const int person, const gboolean hidden);
// id, name (null unnamed), photos, hidden
JsonArray *dt_people_list(void);
GList *dt_people_images(const int person);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
