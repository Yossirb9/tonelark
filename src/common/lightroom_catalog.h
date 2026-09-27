/*
    This file is part of Lightspeed, a darktable fork.
    Copyright (C) 2026 Lightspeed developers.

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
  Import of Adobe Lightroom Classic catalogs (*.lrcat).

  The catalog is copied and opened read-only, the original is never
  modified. Images are added to the library in place (files are not
  moved), with:
    - ratings, pick / reject flags, color labels
    - keywords (hierarchical tags) and collections (tags under
      "Lightroom collections|...")
    - title, caption, creator, copyright, GPS location
    - virtual copies as duplicates, stacks as groups
    - develop settings converted to darktable processing history
*/

#pragma once

#include "common/darktable.h"
#include "control/jobs.h"

G_BEGIN_DECLS

typedef struct dt_lrcat_root_t
{
  int64_t id;
  gchar *absolute_path;  // as stored in the catalog, native separators
  gchar *relative_path;  // relative to the catalog folder, may be NULL
  gchar *name;
  gchar *resolved;       // folder used for the import, NULL if not found
  int n_images;
  int n_found;           // images whose file exists (with the resolved folder)
} dt_lrcat_root_t;

typedef struct dt_lrcat_t
{
  gchar *filename;       // the catalog file
  gchar *copy;           // private copy used for reading
  struct sqlite3 *db;
  GPtrArray *roots;      // dt_lrcat_root_t *
  int n_images;          // all images, including virtual copies
  int n_virtual_copies;
  int n_collections;
  int n_keywords;
  gchar *version;        // Adobe_DBVersion
} dt_lrcat_t;

typedef struct dt_lrcat_options_t
{
  gboolean develop;      // convert develop settings
  gboolean keywords;
  gboolean collections;
  gboolean metadata;     // ratings, flags, labels, title, caption, gps
  gboolean stacks;
} dt_lrcat_options_t;

/** open a catalog (through a private copy), NULL and error set on failure */
dt_lrcat_t *dt_lrcat_open(const char *filename, GError **error);

/** set the folder to use for a root folder of the catalog (e.g. photos moved
    to another drive) and recount the images found */
void dt_lrcat_set_root_folder(dt_lrcat_t *cat, dt_lrcat_root_t *root, const char *folder);

/** number of images whose files are found with the current root folders */
int dt_lrcat_count_found(const dt_lrcat_t *cat);

void dt_lrcat_close(dt_lrcat_t *cat);

/** import in a background job. takes ownership of cat. if wait is not NULL
    the call blocks until the import is done (used by darktable-cli tests) */
void dt_lrcat_import(dt_lrcat_t *cat, const dt_lrcat_options_t *options, const gboolean wait);

/** run the import in the calling thread (no progress UI). returns the number of
    imported images, used by the job and by tests */
int dt_lrcat_import_run(dt_lrcat_t *cat, const dt_lrcat_options_t *options,
                        dt_job_t *job);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
