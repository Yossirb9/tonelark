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
  Lightroom-style "Basic" adjustments.

  Each Lightroom Basic slider (Temp, Tint, Exposure, Contrast, Highlights,
  Shadows, Whites, Blacks, Texture, Clarity, Dehaze, Vibrance, Saturation)
  is mapped onto a parameter of an existing darktable processing module.
  The mapping is shared by the darkroom "basic adjustments" panel and by
  the Lightroom catalog importer, so an imported Lightroom edit shows up
  with the same numbers in the panel.
*/

#pragma once

#include "develop/develop.h"
#include "develop/imageop.h"

G_BEGIN_DECLS

typedef enum dt_lsb_id_t
{
  DT_LSB_TEMP = 0,
  DT_LSB_TINT,
  DT_LSB_EXPOSURE,
  DT_LSB_CONTRAST,
  DT_LSB_HIGHLIGHTS,
  DT_LSB_SHADOWS,
  DT_LSB_WHITES,
  DT_LSB_BLACKS,
  DT_LSB_TEXTURE,
  DT_LSB_CLARITY,
  DT_LSB_DEHAZE,
  DT_LSB_VIBRANCE,
  DT_LSB_SATURATION,
  DT_LSB_COUNT
} dt_lsb_id_t;

typedef enum dt_lsb_section_t
{
  DT_LSB_SECTION_WB = 0,
  DT_LSB_SECTION_TONE,
  DT_LSB_SECTION_PRESENCE
} dt_lsb_section_t;

typedef struct dt_lsb_control_t
{
  dt_lsb_id_t id;
  dt_lsb_section_t section;
  const char *label;      // english label, translate with _()
  const char *tooltip;    // english tooltip, translate with _()
  const char *op;         // darktable module operation
  const char *instance;   // dedicated instance name, NULL for the main instance
  const char *field;      // params field, or widget action id for white balance
  float hard_min, hard_max;
  float soft_min, soft_max;
  int digits;
  float k_pos, k_neg;     // module value = neutral + k * lightroom value
  gboolean neutral_from_defaults; // neutral is the module default (exposure)
} dt_lsb_control_t;

/** the control description for one Lightroom slider */
const dt_lsb_control_t *dt_lsb_control(const dt_lsb_id_t id);

/** find the module instance used by a control, NULL if it does not exist */
dt_iop_module_t *dt_lsb_find_module(GList *iop, const dt_lsb_control_t *c);

/** reset a params buffer to the "Lightroom neutral" state of the control's module */
void dt_lsb_neutralize_params(const dt_iop_module_t *m,
                              const dt_lsb_control_t *c,
                              void *params);

/** Lightroom value currently represented by the module (neutral when disabled) */
float dt_lsb_read(const dt_iop_module_t *m, const dt_lsb_control_t *c);

/** write a Lightroom value into a params buffer of the module. returns FALSE
    for controls that are not parameter based (temperature, tint) */
gboolean dt_lsb_write_params(const dt_iop_module_t *m,
                             const dt_lsb_control_t *c,
                             void *params,
                             const float lr);

/** is the params buffer neutral for all the Lightroom controls using this module
    instance? used to switch a module off when every basic slider went back to 0 */
gboolean dt_lsb_params_are_neutral(const dt_iop_module_t *m, const void *params);

/** white balance through the color calibration module: Lightroom-like
    temperature (K) and tint (-150..150, magenta positive) <-> illuminant xy */
void dt_lsb_wb_xy_to_temp_tint(const float x, const float y, float *temp, float *tint);
void dt_lsb_wb_temp_tint_to_xy(const float temp, const float tint, float *x, float *y);

/** temperature and tint currently used by a color calibration instance */
gboolean dt_lsb_wb_read(const dt_iop_module_t *m, float *temp, float *tint);

/** write a custom illuminant for temperature and tint into color calibration params */
void dt_lsb_wb_write_params(const dt_iop_module_t *m,
                            void *params,
                            const float temp,
                            const float tint);

G_END_DECLS

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
