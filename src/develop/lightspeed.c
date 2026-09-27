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

#include "develop/lightspeed.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/image.h"
#include "common/illuminants.h"

#include <math.h>
#include <string.h>

// keep in sync with iop/bilat.c
#define LSB_BILAT_MODE_BILATERAL 0

// keep in sync with iop/channelmixerrgb.c
#define LSB_ADAPTATION_CAT16 1
#define LSB_ADAPTATION_RGB 4

#define LSB_TEXTURE_INSTANCE "texture"

// Lightroom tint units per CIE 1960 uv distance from the daylight/planckian locus
#define LSB_TINT_SCALE 3000.0f
#define LSB_TEMP_MIN 2000.0f
#define LSB_TEMP_MAX 25000.0f

static const dt_lsb_control_t _controls[DT_LSB_COUNT] =
{
  { DT_LSB_TEMP, DT_LSB_SECTION_WB, N_("Temp"),
    N_("color temperature of the light in the scene, in Kelvin.\n"
       "higher values make the image warmer"),
    "channelmixerrgb", NULL, NULL,
    2000.0f, 25000.0f, 2000.0f, 12000.0f, 0, 1.0f, 1.0f, FALSE },
  { DT_LSB_TINT, DT_LSB_SECTION_WB, N_("Tint"),
    N_("white balance tint, from green (negative) to magenta (positive)"),
    "channelmixerrgb", NULL, NULL,
    -150.0f, 150.0f, -100.0f, 100.0f, 0, 1.0f, 1.0f, FALSE },
  { DT_LSB_EXPOSURE, DT_LSB_SECTION_TONE, N_("Exposure"),
    N_("overall brightness of the image in EV"),
    "exposure", NULL, "exposure",
    -10.0f, 10.0f, -5.0f, 5.0f, 2, 1.0f, 1.0f, TRUE },
  { DT_LSB_CONTRAST, DT_LSB_SECTION_TONE, N_("Contrast"),
    N_("contrast around middle gray"),
    "colorbalancergb", NULL, "contrast",
    -100.0f, 100.0f, -100.0f, 100.0f, 0, 0.004f, 0.004f, FALSE },
  { DT_LSB_HIGHLIGHTS, DT_LSB_SECTION_TONE, N_("Highlights"),
    N_("recover (negative) or brighten (positive) the bright areas of the image"),
    "shadhi", NULL, "highlights",
    -100.0f, 100.0f, -100.0f, 100.0f, 0, 0.8f, 0.8f, FALSE },
  { DT_LSB_SHADOWS, DT_LSB_SECTION_TONE, N_("Shadows"),
    N_("brighten (positive) or darken (negative) the dark areas of the image"),
    "shadhi", NULL, "shadows",
    -100.0f, 100.0f, -100.0f, 100.0f, 0, 0.8f, 0.8f, FALSE },
  { DT_LSB_WHITES, DT_LSB_SECTION_TONE, N_("Whites"),
    N_("adjust the brightest tones of the image"),
    "colorbalancergb", NULL, "highlights_Y",
    -100.0f, 100.0f, -100.0f, 100.0f, 0, 0.003f, 0.003f, FALSE },
  { DT_LSB_BLACKS, DT_LSB_SECTION_TONE, N_("Blacks"),
    N_("adjust the darkest tones of the image"),
    "colorbalancergb", NULL, "global_Y",
    -100.0f, 100.0f, -100.0f, 100.0f, 0, 0.0002f, 0.0002f, FALSE },
  { DT_LSB_TEXTURE, DT_LSB_SECTION_PRESENCE, N_("Texture"),
    N_("enhance (positive) or smooth (negative) fine and medium details"),
    "bilat", LSB_TEXTURE_INSTANCE, "detail",
    -100.0f, 100.0f, -100.0f, 100.0f, 0, 0.008f, 0.009f, FALSE },
  { DT_LSB_CLARITY, DT_LSB_SECTION_PRESENCE, N_("Clarity"),
    N_("local contrast of medium and large structures"),
    "bilat", NULL, "detail",
    -100.0f, 100.0f, -100.0f, 100.0f, 0, 0.0075f, 0.009f, FALSE },
  { DT_LSB_DEHAZE, DT_LSB_SECTION_PRESENCE, N_("Dehaze"),
    N_("remove (positive) or add (negative) atmospheric haze"),
    "hazeremoval", NULL, "strength",
    -100.0f, 100.0f, -100.0f, 100.0f, 0, 0.008f, 0.008f, FALSE },
  { DT_LSB_VIBRANCE, DT_LSB_SECTION_PRESENCE, N_("Vibrance"),
    N_("saturation boost of the less saturated colors"),
    "colorbalancergb", NULL, "vibrance",
    -100.0f, 100.0f, -100.0f, 100.0f, 0, 0.005f, 0.005f, FALSE },
  { DT_LSB_SATURATION, DT_LSB_SECTION_PRESENCE, N_("Saturation"),
    N_("saturation of all colors, -100 gives a monochrome image"),
    "colorbalancergb", NULL, "chroma_global",
    -100.0f, 100.0f, -100.0f, 100.0f, 0, 0.006f, 0.01f, FALSE },
};

const dt_lsb_control_t *dt_lsb_control(const dt_lsb_id_t id)
{
  if(id < 0 || id >= DT_LSB_COUNT) return NULL;
  return &_controls[id];
}

static gboolean _is_texture_instance(const dt_iop_module_t *m)
{
  return !g_strcmp0(m->multi_name, LSB_TEXTURE_INSTANCE);
}

static gboolean _is_wb(const dt_lsb_control_t *c)
{
  return c->id == DT_LSB_TEMP || c->id == DT_LSB_TINT;
}

dt_iop_module_t *dt_lsb_find_module(GList *iop, const dt_lsb_control_t *c)
{
  if(!c) return NULL;

  for(GList *l = iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *m = l->data;
    if(!dt_iop_module_is(m, c->op) || m->iop_order == INT_MAX) continue;

    if(c->instance)
    {
      if(!g_strcmp0(m->multi_name, c->instance)) return m;
    }
    else if(!_is_texture_instance(m))
      return m;
  }
  return NULL;
}

static void *_field(const dt_iop_module_t *m, const void *params, const char *field)
{
  if(!m || !m->so || !m->so->get_p || !params || !field) return NULL;
  return m->so->get_p(params, field);
}

static float _neutral(const dt_iop_module_t *m, const dt_lsb_control_t *c)
{
  if(c->neutral_from_defaults)
  {
    const float *f = _field(m, m->default_params, c->field);
    return f ? *f : 0.0f;
  }
  return 0.0f;
}

static void _set_float(const dt_iop_module_t *m, void *params, const char *field, const float v)
{
  float *f = _field(m, params, field);
  if(f) *f = v;
}

static void _set_int(const dt_iop_module_t *m, void *params, const char *field, const int v)
{
  int *f = _field(m, params, field);
  if(f) *f = v;
}

void dt_lsb_neutralize_params(const dt_iop_module_t *m,
                              const dt_lsb_control_t *c,
                              void *params)
{
  if(!m || !params) return;
  memcpy(params, m->default_params, m->params_size);

  if(dt_iop_module_is(m, "shadhi"))
  {
    _set_float(m, params, "shadows", 0.0f);
    _set_float(m, params, "highlights", 0.0f);
  }
  else if(dt_iop_module_is(m, "bilat"))
  {
    _set_float(m, params, "detail", 0.0f);
    if((c && c->instance) || _is_texture_instance(m))
    {
      // fine scale bilateral local contrast for texture
      _set_int(m, params, "mode", LSB_BILAT_MODE_BILATERAL);
      _set_float(m, params, "sigma_s", 6.0f);
      _set_float(m, params, "sigma_r", 20.0f);
    }
  }
  else if(dt_iop_module_is(m, "hazeremoval"))
  {
    _set_float(m, params, "strength", 0.0f);
  }
  else if(dt_iop_module_is(m, "channelmixerrgb"))
  {
    int *adaptation = _field(m, params, "adaptation");
    if(adaptation && *adaptation == LSB_ADAPTATION_RGB) *adaptation = LSB_ADAPTATION_CAT16;
  }
}

float dt_lsb_read(const dt_iop_module_t *m, const dt_lsb_control_t *c)
{
  if(!c) return 0.0f;
  if(_is_wb(c))
  {
    float temp = 5003.0f, tint = 0.0f;
    dt_lsb_wb_read(m, &temp, &tint);
    return c->id == DT_LSB_TEMP ? temp : tint;
  }
  if(!m || !m->enabled) return 0.0f;

  const float *f = _field(m, m->params, c->field);
  if(!f) return 0.0f;

  const float d = *f - _neutral(m, c);
  const float k = d >= 0.0f ? c->k_pos : c->k_neg;
  return k > 0.0f ? d / k : 0.0f;
}

gboolean dt_lsb_write_params(const dt_iop_module_t *m,
                             const dt_lsb_control_t *c,
                             void *params,
                             const float lr)
{
  if(!m || !c || !params) return FALSE;
  if(_is_wb(c)) return FALSE;

  float *f = _field(m, params, c->field);
  if(!f) return FALSE;

  const float v = CLAMP(lr, c->hard_min, c->hard_max);
  const float k = v >= 0.0f ? c->k_pos : c->k_neg;
  *f = _neutral(m, c) + k * v;
  return TRUE;
}

gboolean dt_lsb_params_are_neutral(const dt_iop_module_t *m, const void *params)
{
  if(!m || !params) return FALSE;

  const dt_lsb_control_t *c = NULL;
  for(int i = 0; i < DT_LSB_COUNT; i++)
    if(dt_iop_module_is(m, _controls[i].op)
       && (_controls[i].instance != NULL) == _is_texture_instance(m))
      c = &_controls[i];

  void *neutral = g_malloc(m->params_size);
  dt_lsb_neutralize_params(m, c, neutral);
  const gboolean same = memcmp(neutral, params, m->params_size) == 0;
  g_free(neutral);
  return same;
}

// --- white balance through color calibration (channelmixerrgb) --------------

static void _xy_to_uv(const float x, const float y, float *u, float *v)
{
  const float d = -2.0f * x + 12.0f * y + 3.0f;
  *u = 4.0f * x / d;
  *v = 6.0f * y / d;
}

static void _uv_to_xy(const float u, const float v, float *x, float *y)
{
  const float d = 2.0f * u - 8.0f * v + 4.0f;
  *x = 3.0f * u / d;
  *y = 2.0f * v / d;
}

// point of the reference locus (daylight above 4000 K, black body below) in uv
static void _locus_uv(const float T, float *u, float *v)
{
  const float t = CLAMP(T, 1667.0f, LSB_TEMP_MAX);
  float x = 0.f, y = 0.f;
  if(t >= 4000.0f)
    CCT_to_xy_daylight(t, &x, &y);
  else
    CCT_to_xy_blackbody(t, &x, &y);
  _xy_to_uv(x, y, u, v);
}

// unit normal of the locus at T, pointing to the magenta side (smaller v)
static void _locus_normal(const float T, float *nu, float *nv)
{
  float u0, v0, u1, v1;
  _locus_uv(fmaxf(T * 0.99f, 1667.0f), &u0, &v0);
  _locus_uv(fminf(T * 1.01f, LSB_TEMP_MAX), &u1, &v1);
  const float tu = u1 - u0, tv = v1 - v0;
  const float n = sqrtf(tu * tu + tv * tv);
  *nu = n > 0.f ? -tv / n : 0.f;
  *nv = n > 0.f ? tu / n : -1.f;
  if(*nv > 0.f)
  {
    *nu = -*nu;
    *nv = -*nv;
  }
}

void dt_lsb_wb_xy_to_temp_tint(const float x, const float y, float *temp, float *tint)
{
  float u, v;
  _xy_to_uv(x, y, &u, &v);

  // golden section search of the closest locus point, in mired space
  float a = 1e6f / LSB_TEMP_MAX, b = 1e6f / LSB_TEMP_MIN;
  const float gr = 0.6180339887f;
  for(int i = 0; i < 64; i++)
  {
    const float c = b - gr * (b - a);
    const float d = a + gr * (b - a);
    float uc, vc, ud, vd;
    _locus_uv(1e6f / c, &uc, &vc);
    _locus_uv(1e6f / d, &ud, &vd);
    const float dc = (u - uc) * (u - uc) + (v - vc) * (v - vc);
    const float dd = (u - ud) * (u - ud) + (v - vd) * (v - vd);
    if(dc < dd)
      b = d;
    else
      a = c;
  }
  const float T = 1e6f / (0.5f * (a + b));

  float u0, v0, nu, nv;
  _locus_uv(T, &u0, &v0);
  _locus_normal(T, &nu, &nv);

  *temp = T;
  *tint = LSB_TINT_SCALE * ((u - u0) * nu + (v - v0) * nv);
}

void dt_lsb_wb_temp_tint_to_xy(const float temp, const float tint, float *x, float *y)
{
  const float T = CLAMP(temp, LSB_TEMP_MIN, LSB_TEMP_MAX);
  float u0, v0, nu, nv;
  _locus_uv(T, &u0, &v0);
  _locus_normal(T, &nu, &nv);
  const float d = tint / LSB_TINT_SCALE;
  _uv_to_xy(u0 + d * nu, v0 + d * nv, x, y);
}

static gboolean _wb_params_xy(const dt_iop_module_t *m, const void *params, float *x, float *y)
{
  const int *illuminant = _field(m, params, "illuminant");
  const int *pfluo = _field(m, params, "illum_fluo");
  const int *pled = _field(m, params, "illum_led");
  const float *px = _field(m, params, "x");
  const float *py = _field(m, params, "y");
  const float *pt = _field(m, params, "temperature");
  if(!illuminant || !px || !py || !pt) return FALSE;

  switch(*illuminant)
  {
    case DT_ILLUMINANT_CUSTOM:
    case DT_ILLUMINANT_CAMERA:
    case DT_ILLUMINANT_DETECT_EDGES:
    case DT_ILLUMINANT_DETECT_SURFACES:
      *x = *px;
      *y = *py;
      return *x > 0.f && *y > 0.f;
    default:
      return illuminant_to_xy(*illuminant, NULL, NULL, x, y, *pt,
                              pfluo ? *pfluo : 0, pled ? *pled : 0);
  }
}

gboolean dt_lsb_wb_read(const dt_iop_module_t *m, float *temp, float *tint)
{
  float x = D50xyY.x, y = D50xyY.y;

  if(m && m->so && m->so->get_p)
  {
    // disabled or bypassed: show the default illuminant of the image
    const void *params = m->enabled ? m->params : m->default_params;
    const int *adaptation = _field(m, params, "adaptation");
    if(adaptation && *adaptation == LSB_ADAPTATION_RGB) params = m->default_params;

    if(!_wb_params_xy(m, params, &x, &y))
    {
      x = D50xyY.x;
      y = D50xyY.y;
    }
  }
  dt_lsb_wb_xy_to_temp_tint(x, y, temp, tint);
  return TRUE;
}

void dt_lsb_wb_write_params(const dt_iop_module_t *m,
                            void *params,
                            const float temp,
                            const float tint)
{
  if(!m || !params) return;
  float x, y;
  dt_lsb_wb_temp_tint_to_xy(temp, tint, &x, &y);

  int *illuminant = _field(m, params, "illuminant");
  int *adaptation = _field(m, params, "adaptation");
  float *px = _field(m, params, "x");
  float *py = _field(m, params, "y");
  float *pt = _field(m, params, "temperature");
  if(illuminant) *illuminant = DT_ILLUMINANT_CUSTOM;
  if(adaptation && *adaptation == LSB_ADAPTATION_RGB) *adaptation = LSB_ADAPTATION_CAT16;
  if(px) *px = x;
  if(py) *py = y;
  if(pt) *pt = CLAMP(temp, LSB_TEMP_MIN, LSB_TEMP_MAX);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
