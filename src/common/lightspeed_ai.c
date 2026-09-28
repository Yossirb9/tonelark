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

#include "common/lightspeed_ai.h"
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/history_snapshot.h"
#include "common/math.h"
#include "common/tags.h"
#include "common/undo.h"
#include "develop/develop.h"
#include "develop/lightroom.h"
#include "views/view.h"
#include "common/file_location.h"
#include "common/film.h"
#include "common/grouping.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "control/conf.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "imageio/imageio_common.h"
#include "imageio/imageio_jpeg.h"
#include "imageio/imageio_module.h"

#include <glib/gstdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// the helper

static gchar *_ai_dir(void)
{
  char datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  return g_build_filename(datadir, "lightspeed", "ai", NULL);
}

static gchar *_python(void)
{
  gchar *dir = _ai_dir();
#ifdef _WIN32
  gchar *bundled = g_build_filename(dir, "python", "python.exe", NULL);
#else
  gchar *bundled = g_build_filename(dir, "python", "bin", "python3", NULL);
#endif
  g_free(dir);
  if(g_file_test(bundled, G_FILE_TEST_IS_EXECUTABLE)) return bundled;
  g_free(bundled);

  // a development setup: the Python of the system (not the Windows store stub)
  const char *names[] = { "python3", "python", "py", NULL };
  for(int k = 0; names[k]; k++)
  {
    gchar *p = g_find_program_in_path(names[k]);
    if(p && !strstr(p, "WindowsApps")) return p;
    g_free(p);
  }
  return NULL;
}

gchar *dt_lsai_tmpdir(void)
{
  return g_dir_make_tmp("lightspeed_ai_XXXXXX", NULL);
}

// the last "PROGRESS <fraction> <text>" line of the helper log
static void _read_progress(const char *log, dt_job_t *job, goffset *pos)
{
  if(!job) return;
  gchar *text = NULL;
  gsize len = 0;
  if(!g_file_get_contents(log, &text, &len, NULL)) return;
  if((goffset)len > *pos)
  {
    gchar **lines = g_strsplit(text + *pos, "\n", -1);
    for(int k = 0; lines[k]; k++)
    {
      if(g_str_has_prefix(lines[k], "PROGRESS "))
      {
        gchar *end = NULL;
        const double f = g_ascii_strtod(lines[k] + 9, &end);
        dt_control_job_set_progress(job, CLAMP(f, 0.0, 1.0));
        if(end && *end == ' ')
        {
          g_strchomp(end + 1);
          dt_control_job_set_progress_message(job, "%s", end + 1);
        }
      }
    }
    g_strfreev(lines);
    *pos = len;
  }
  g_free(text);
}

static gboolean _cancelled(dt_job_t *job)
{
  return job && dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED;
}

// run the helper, its output goes to `log`; TRUE when it ran to the end
static gboolean _spawn_wait(gchar **argv, const char *log, dt_job_t *job, GError **error)
{
  goffset pos = 0;
#ifdef _WIN32
  GString *cmd = g_string_new(NULL);
  for(int k = 0; argv[k]; k++)
    g_string_append_printf(cmd, "%s\"%s\"", k ? " " : "", argv[k]);
  wchar_t *wcmd = g_utf8_to_utf16(cmd->str, -1, NULL, NULL, NULL);
  wchar_t *wlog = g_utf8_to_utf16(log, -1, NULL, NULL, NULL);
  g_string_free(cmd, TRUE);

  SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
  HANDLE hlog = CreateFileW(wlog, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  g_free(wlog);
  // no console: an empty input, the AI tools must not wait for one
  HANDLE hnul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  STARTUPINFOW si = { 0 };
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = hnul;
  si.hStdOutput = hlog;
  si.hStdError = hlog;
  PROCESS_INFORMATION pi = { 0 };
  // the helper and the AI tools it starts live in a job object: cancelling,
  // or closing Tonelark, stops all of them
  HANDLE hjob = CreateJobObjectW(NULL, NULL);
  if(hjob)
  {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = { 0 };
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(hjob, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  }
  // no console window pops up for the helper
  const BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE,
                                 CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                                 NULL, NULL, &si, &pi);
  if(ok)
  {
    if(hjob) AssignProcessToJobObject(hjob, pi.hProcess);
    ResumeThread(pi.hThread);
  }
  g_free(wcmd);
  if(hlog != INVALID_HANDLE_VALUE) CloseHandle(hlog);
  if(hnul != INVALID_HANDLE_VALUE) CloseHandle(hnul);
  if(!ok)
  {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, _("cannot start the AI helper (error %lu)"),
                GetLastError());
    if(hjob) CloseHandle(hjob);
    return FALSE;
  }
  while(WaitForSingleObject(pi.hProcess, 300) == WAIT_TIMEOUT)
  {
    _read_progress(log, job, &pos);
    if(_cancelled(job))
    {
      if(hjob)
        TerminateJobObject(hjob, 1);
      else
        TerminateProcess(pi.hProcess, 1);
      break;
    }
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  if(hjob) CloseHandle(hjob);     // stops what is left of the tree
#else
  const int fd = g_open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  GPid pid;
  if(!g_spawn_async_with_fds(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid,
                             -1, fd, fd, error))
  {
    close(fd);
    return FALSE;
  }
  close(fd);
  int status = 0;
  while(waitpid(pid, &status, WNOHANG) == 0)
  {
    g_usleep(300000);
    _read_progress(log, job, &pos);
    if(_cancelled(job))
    {
      kill(pid, SIGTERM);
      waitpid(pid, &status, 0);
      break;
    }
  }
  g_spawn_close_pid(pid);
#endif
  _read_progress(log, job, &pos);
  if(_cancelled(job))
  {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_INTR, _("cancelled"));
    return FALSE;
  }
  return TRUE;
}

JsonObject *dt_lsai_run(const char *command, JsonObject *request, dt_job_t *job, GError **error)
{
  gchar *python = _python();
  gchar *dir = _ai_dir();
  gchar *script = g_build_filename(dir, "lsai.py", NULL);
  g_free(dir);
  if(!python || !g_file_test(script, G_FILE_TEST_EXISTS))
  {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                _("the AI helper is missing, reinstall Tonelark"));
    g_free(python);
    g_free(script);
    return NULL;
  }

  gchar *tmp = dt_lsai_tmpdir();
  gchar *req_path = g_build_filename(tmp, "request.json", NULL);
  gchar *res_path = g_build_filename(tmp, "response.json", NULL);
  gchar *log_path = g_build_filename(tmp, "helper.log", NULL);

  JsonNode *root = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(root, request);
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, root);
  json_generator_to_file(gen, req_path, NULL);
  g_object_unref(gen);
  json_node_unref(root);

  gchar *argv[] = { python, script, (gchar *)command, req_path, res_path, NULL };
  JsonObject *result = NULL;
  if(_spawn_wait(argv, log_path, job, error))
  {
    JsonParser *parser = json_parser_new();
    if(json_parser_load_from_file(parser, res_path, NULL)
       && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
    {
      JsonObject *obj = json_node_get_object(json_parser_get_root(parser));
      if(json_object_get_boolean_member_with_default(obj, "ok", FALSE))
        result = json_object_ref(obj);
      else
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s",
                    json_object_get_string_member_with_default(obj, "error", _("the AI helper failed")));
    }
    else
    {
      // no answer: the end of the log tells why
      gchar *text = NULL;
      g_file_get_contents(log_path, &text, NULL, NULL);
      const size_t n = text ? strlen(text) : 0;
      g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s",
                  n ? text + (n > 300 ? n - 300 : 0) : _("the AI helper failed"));
      g_free(text);
    }
    g_object_unref(parser);
  }

  g_unlink(req_path);
  g_unlink(res_path);
  g_unlink(log_path);
  g_rmdir(tmp);
  g_free(req_path);
  g_free(res_path);
  g_free(log_path);
  g_free(tmp);
  g_free(python);
  g_free(script);
  return result;
}

// ---------------------------------------------------------------------------
// images for the helper

gboolean dt_lsai_write_preview(const dt_imgid_t imgid, const int max_size, const char *path)
{
  dt_mipmap_buffer_t buf;
  const dt_mipmap_size_t size = dt_mipmap_cache_get_matching_size(max_size, max_size);
  dt_mipmap_cache_get(&buf, imgid, size, DT_MIPMAP_BLOCKING, 'r');
  gboolean ok = FALSE;
  if(buf.buf && buf.width > 0 && buf.height > 0)
    ok = dt_imageio_jpeg_write(path, buf.buf, buf.width, buf.height, 90, NULL, 0) == 0;
  dt_mipmap_cache_release(&buf);
  return ok;
}

gboolean dt_lsai_export_tiff(const dt_imgid_t imgid, const char *path)
{
  dt_imageio_module_format_t *format = dt_imageio_get_format_by_name("tiff");
  if(!format) return FALSE;

  // the tiff module takes its parameters from the configuration
  const char *keys[] = { "plugins/imageio/format/tiff/bpp", "plugins/imageio/format/tiff/pixelformat",
                         "plugins/imageio/format/tiff/compress", "plugins/imageio/format/tiff/shortfile" };
  const int values[] = { 16, 0, 0, 0 };
  int saved[4];
  for(int k = 0; k < 4; k++)
  {
    saved[k] = dt_conf_get_int(keys[k]);
    dt_conf_set_int(keys[k], values[k]);
  }
  dt_imageio_module_data_t *fdata = format->get_params(format);
  for(int k = 0; k < 4; k++) dt_conf_set_int(keys[k], saved[k]);
  if(!fdata) return FALSE;

  fdata->max_width = fdata->max_height = 0;
  fdata->width = fdata->height = 0;
  fdata->style[0] = '\0';
  const gboolean failed = dt_imageio_export(imgid, path, format, fdata, TRUE, FALSE, FALSE, 1.0,
                                            FALSE, FALSE, DT_COLORSPACE_SRGB, NULL, DT_INTENT_PERCEPTUAL,
                                            NULL, NULL, 1, 1, NULL);
  format->free_params(format, fdata);
  return !failed && g_file_test(path, G_FILE_TEST_EXISTS);
}

dt_imgid_t dt_lsai_import_derived(const dt_imgid_t source, const char *path)
{
  gchar *dir = g_path_get_dirname(path);
  dt_film_t film;
  dt_film_init(&film);
  const dt_filmid_t filmid = dt_film_new(&film, dir);
  g_free(dir);
  dt_film_cleanup(&film);
  if(!dt_is_valid_filmid(filmid)) return NO_IMGID;

  const dt_imgid_t id = dt_image_import(filmid, path, TRUE, TRUE);
  if(dt_is_valid_imgid(id) && dt_is_valid_imgid(source))
  {
    const dt_image_t *img = dt_image_cache_get(source, 'r');
    const dt_imgid_t group = img ? img->group_id : source;
    dt_image_cache_read_release(img);
    dt_grouping_add_to_group(group, id);
  }
  return id;
}

// ---------------------------------------------------------------------------
// edits

typedef struct _gui_call_t
{
  GSourceFunc func;
  gpointer data;
  gboolean done;
  GMutex lock;
  GCond cond;
} _gui_call_t;

static gboolean _gui_call_run(gpointer p)
{
  _gui_call_t *c = p;
  c->func(c->data);
  g_mutex_lock(&c->lock);
  c->done = TRUE;
  g_cond_signal(&c->cond);
  g_mutex_unlock(&c->lock);
  return G_SOURCE_REMOVE;
}

void dt_lsai_in_gui(GSourceFunc func, gpointer data)
{
  if(!darktable.control || pthread_equal(pthread_self(), darktable.control->gui_thread))
  {
    func(data);
    return;
  }
  _gui_call_t c = { .func = func, .data = data, .done = FALSE };
  g_mutex_init(&c.lock);
  g_cond_init(&c.cond);
  g_main_context_invoke(NULL, _gui_call_run, &c);
  g_mutex_lock(&c.lock);
  while(!c.done) g_cond_wait(&c.cond, &c.lock);
  g_mutex_unlock(&c.lock);
  g_mutex_clear(&c.lock);
  g_cond_clear(&c.cond);
}

static gboolean _in_darkroom(const dt_imgid_t imgid)
{
  return dt_view_get_current() == DT_VIEW_DARKROOM && dt_dev_is_current_image(darktable.develop, imgid);
}

// first instance of a module in a develop
static dt_iop_module_t *_instance(dt_develop_t *dev, const char *op)
{
  for(GList *l = dev->iop; l; l = g_list_next(l))
  {
    dt_iop_module_t *m = l->data;
    if(dt_iop_module_is(m, op) && m->iop_order != INT_MAX) return m;
  }
  return NULL;
}

static float *_pf(dt_iop_module_t *m, void *params, const char *field)
{
  return m && m->so->get_p ? m->so->get_p(params, field) : NULL;
}

// size of the displayed image before the crop (the flip applied)
static void _display_size(const dt_image_t *img, int *w, int *h)
{
  *w = img->p_width > 0 ? img->p_width : img->width;
  *h = img->p_height > 0 ? img->p_height : img->height;
  if(dt_image_orientation(img) & ORIENTATION_SWAP_XY)
  {
    const int t = *w;
    *w = *h;
    *h = t;
  }
}

JsonObject *dt_lsai_read_edit(const dt_imgid_t imgid)
{
  JsonObject *o = json_object_new();
  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_dev_load_image(&dev, imgid);
  // the modules with the parameters of the history
  dt_dev_pop_history_items_ext(&dev, dev.history_end);

  GHashTable *crs = dt_lightroom_read_develop(&dev);
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init(&it, crs);
  while(g_hash_table_iter_next(&it, &key, &value))
  {
    char *end = NULL;
    const double v = g_ascii_strtod(value, &end);
    if(end && *end == '\0' && end != value)
      json_object_set_double_member(o, key, v);
    else
      json_object_set_string_member(o, key, value);
  }
  g_hash_table_destroy(crs);

  json_object_set_boolean_member(o, "raw", dt_image_is_raw(&dev.image_storage));
  int w = 0, h = 0;
  _display_size(&dev.image_storage, &w, &h);
  json_object_set_int_member(o, "width", w);
  json_object_set_int_member(o, "height", h);

  float l = 0.0f, t = 0.0f, r = 1.0f, b = 1.0f, angle = 0.0f;
  dt_iop_module_t *crop = _instance(&dev, "crop");
  if(crop && crop->enabled)
  {
    float *f;
    if((f = _pf(crop, crop->params, "cx"))) l = *f;
    if((f = _pf(crop, crop->params, "cy"))) t = *f;
    if((f = _pf(crop, crop->params, "cw"))) r = *f;
    if((f = _pf(crop, crop->params, "ch"))) b = *f;
  }
  dt_iop_module_t *ashift = _instance(&dev, "ashift");
  if(ashift && ashift->enabled)
  {
    float *f = _pf(ashift, ashift->params, "rotation");
    if(f) angle = *f;
  }
  json_object_set_double_member(o, "CropLeft", l);
  json_object_set_double_member(o, "CropTop", t);
  json_object_set_double_member(o, "CropRight", r);
  json_object_set_double_member(o, "CropBottom", b);
  json_object_set_double_member(o, "Straighten", angle);
  dt_dev_cleanup(&dev);
  return o;
}

static gboolean _member_float(JsonObject *o, const char *key, float *v)
{
  JsonNode *n = json_object_get_member(o, key);
  if(!n || !JSON_NODE_HOLDS_VALUE(n)) return FALSE;
  const GType t = json_node_get_value_type(n);
  if(t == G_TYPE_STRING)
  {
    char *end = NULL;
    const char *s = json_node_get_string(n);
    *v = g_ascii_strtod(s, &end);
    return end != s;
  }
  *v = json_node_get_double(n);
  return TRUE;
}

// crop (normalized rectangle of the displayed image, crop module) and
// straighten (rotate and perspective, cropped to the original format)
static gboolean _apply_crop(dt_develop_t *dev, JsonObject *edit)
{
  gboolean changed = FALSE;
  float angle = 0.0f;
  if(_member_float(edit, "Straighten", &angle))
  {
    dt_iop_module_t *m = _instance(dev, "ashift");
    if(m && (fabsf(angle) > 0.01f || m->enabled))
    {
      if(!m->enabled) memcpy(m->params, m->default_params, m->params_size);
      float *rotation = _pf(m, m->params, "rotation");
      int *cropmode = (int *)_pf(m, m->params, "cropmode");
      float *cl = _pf(m, m->params, "cl"), *cr = _pf(m, m->params, "cr");
      float *ct = _pf(m, m->params, "ct"), *cb = _pf(m, m->params, "cb");
      if(rotation && cropmode && cl && cr && ct && cb)
      {
        *rotation = CLAMP(angle, -45.0f, 45.0f);
        // the largest rectangle of the original format inside the rotated
        // image (the module computes it in its gui only)
        const float W = dev->image_storage.p_width > 0 ? dev->image_storage.p_width : dev->image_storage.width;
        const float H = dev->image_storage.p_height > 0 ? dev->image_storage.p_height : dev->image_storage.height;
        const float a = fabsf(*rotation) * M_PI_F / 180.0f;
        const float c = cosf(a), s = sinf(a);
        const float BW = W * c + H * s, BH = W * s + H * c;
        const float k = fminf(W / BW, H / BH);
        *cropmode = 2;   // original format
        *cl = 0.5f - k * W / (2.0f * BW);
        *cr = 1.0f - *cl;
        *ct = 0.5f - k * H / (2.0f * BH);
        *cb = 1.0f - *ct;
        m->enabled = TRUE;
        dt_dev_add_history_item_ext(dev, m, TRUE, TRUE);
        changed = TRUE;
      }
    }
  }

  float l = 0.0f, t = 0.0f, r = 1.0f, b = 1.0f;
  if(_member_float(edit, "CropLeft", &l) && _member_float(edit, "CropTop", &t)
     && _member_float(edit, "CropRight", &r) && _member_float(edit, "CropBottom", &b))
  {
    dt_iop_module_t *m = _instance(dev, "crop");
    l = CLAMP(l, 0.0f, 0.95f);
    t = CLAMP(t, 0.0f, 0.95f);
    r = CLAMP(r, l + 0.05f, 1.0f);
    b = CLAMP(b, t + 0.05f, 1.0f);
    const gboolean full = l < 1e-3f && t < 1e-3f && r > 0.999f && b > 0.999f;
    if(m && (!full || m->enabled))
    {
      if(!m->enabled) memcpy(m->params, m->default_params, m->params_size);
      float *cx = _pf(m, m->params, "cx"), *cy = _pf(m, m->params, "cy");
      float *cw = _pf(m, m->params, "cw"), *ch = _pf(m, m->params, "ch");
      int *rn = (int *)_pf(m, m->params, "ratio_n"), *rd = (int *)_pf(m, m->params, "ratio_d");
      if(cx && cy && cw && ch)
      {
        *cx = l;
        *cy = t;
        *cw = r;
        *ch = b;
        // free aspect: the crop tool shows the rectangle as it is
        if(rn) *rn = 0;
        if(rd) *rd = 0;
        m->enabled = !full;
        dt_dev_add_history_item_ext(dev, m, m->enabled, TRUE);
        changed = TRUE;
      }
    }
  }
  return changed;
}

static void _after_change(const dt_imgid_t imgid)
{
  dt_history_hash_write_from_history(imgid, DT_HISTORY_HASH_CURRENT);
  guint tagid = 0;
  dt_tag_new("darktable|changed", &tagid);
  dt_tag_attach(tagid, imgid, FALSE, FALSE);
  dt_image_cache_set_change_timestamp(imgid);
  dt_mipmap_cache_remove(imgid);
  dt_image_update_final_size(imgid);
  dt_image_synch_xmp(imgid);
  DT_CONTROL_SIGNAL_RAISE(DT_SIGNAL_DEVELOP_MIPMAP_UPDATED, imgid);
}

static const char *_crop_keys[] = { "CropLeft", "CropTop", "CropRight", "CropBottom", "Straighten",
                                    "raw", "width", "height", NULL };

static gboolean _apply_edit_db(const dt_imgid_t imgid, JsonObject *edit)
{
  // Lightroom settings: every member but the crop, as text
  GHashTable *crs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  GList *members = json_object_get_members(edit);
  for(GList *l = members; l; l = g_list_next(l))
  {
    const char *key = l->data;
    if(g_strv_contains((const gchar *const *)_crop_keys, key)) continue;
    JsonNode *n = json_object_get_member(edit, key);
    if(!n || !JSON_NODE_HOLDS_VALUE(n)) continue;
    const GType t = json_node_get_value_type(n);
    gchar *text = NULL;
    if(t == G_TYPE_STRING)
      text = g_strdup(json_node_get_string(n));
    else if(t == G_TYPE_BOOLEAN)
      text = g_strdup(json_node_get_boolean(n) ? "True" : "False");
    else
    {
      char buf[G_ASCII_DTOSTR_BUF_SIZE];
      text = g_strdup(g_ascii_dtostr(buf, sizeof(buf), json_node_get_double(n)));
    }
    g_hash_table_replace(crs, g_strdup(key), text);
  }
  g_list_free(members);

  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dt_dev_load_image(&dev, imgid);
  // the modules with the parameters of the history
  dt_dev_pop_history_items_ext(&dev, dev.history_end);
  gboolean changed = g_hash_table_size(crs) > 0 && dt_lightroom_update_develop(&dev, crs);
  changed |= _apply_crop(&dev, edit);
  if(changed) dt_dev_write_history(&dev);
  dt_dev_cleanup(&dev);
  g_hash_table_destroy(crs);

  if(changed) _after_change(imgid);
  return changed;
}

typedef struct _edit_call_t
{
  dt_imgid_t imgid, src;
  JsonObject *edit;
  gboolean ok;
} _edit_call_t;

static gboolean _copy_db(const dt_imgid_t src, const dt_imgid_t dst)
{
  // the whole look, not what belongs to one photo
  static const char *skip[] = { "crop", "ashift", "clipping", "flip", "retouch", "spots", "liquify",
                                "rawprepare", "mask_manager", NULL };
  GList *ops = NULL;
  GList *items = dt_history_get_items(src, FALSE, FALSE, FALSE);
  for(GList *l = items; l; l = g_list_next(l))
  {
    const dt_history_item_t *item = l->data;
    if(!g_strv_contains((const gchar *const *)skip, item->op))
      ops = g_list_append(ops, GUINT_TO_POINTER(item->num));
  }
  g_list_free_full(items, dt_history_item_free);
  if(!ops) return FALSE;
  dt_history_copy_and_paste_on_image(src, dst, TRUE, ops, FALSE, FALSE, TRUE);
  g_list_free(ops);
  return TRUE;
}

static gboolean _edit_darkroom(gpointer data)
{
  _edit_call_t *c = data;
  // what is still in the darkroom goes to the database first
  dt_dev_write_history(darktable.develop);
  dt_dev_undo_start_record(darktable.develop);
  c->ok = c->edit ? _apply_edit_db(c->imgid, c->edit) : _copy_db(c->src, c->imgid);
  // the darkroom takes the new history (a paste reloads it itself)
  if(c->ok && c->edit && dt_dev_is_current_image(darktable.develop, c->imgid))
  {
    dt_dev_reload_history_items(darktable.develop);
    dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
  }
  dt_dev_undo_end_record(darktable.develop);
  return G_SOURCE_REMOVE;
}

gboolean dt_lsai_apply_edit(const dt_imgid_t imgid, JsonObject *edit)
{
  if(!dt_is_valid_imgid(imgid) || !edit) return FALSE;

  _edit_call_t c = { .imgid = imgid, .edit = edit, .ok = FALSE };
  if(_in_darkroom(imgid))
  {
    dt_lsai_in_gui(_edit_darkroom, &c);
    return c.ok;
  }

  dt_undo_lt_history_t *hist = dt_history_snapshot_item_init();
  hist->imgid = imgid;
  dt_history_snapshot_undo_create(imgid, &hist->before, &hist->before_history_end);
  c.ok = _apply_edit_db(imgid, edit);
  if(!c.ok)
  {
    dt_history_snapshot_undo_lt_history_data_free(hist);
    return FALSE;
  }
  dt_history_snapshot_undo_create(imgid, &hist->after, &hist->after_history_end);
  dt_undo_start_group(darktable.undo, DT_UNDO_LT_HISTORY);
  dt_undo_record(darktable.undo, NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                 dt_history_snapshot_undo_pop, dt_history_snapshot_undo_lt_history_data_free);
  dt_undo_end_group(darktable.undo);
  return TRUE;
}

gboolean dt_lsai_copy_edit(const dt_imgid_t src, const dt_imgid_t dst)
{
  if(!dt_is_valid_imgid(src) || !dt_is_valid_imgid(dst) || src == dst) return FALSE;
  _edit_call_t c = { .imgid = dst, .src = src, .edit = NULL, .ok = FALSE };
  if(_in_darkroom(dst) || _in_darkroom(src))
    dt_lsai_in_gui(_edit_darkroom, &c);
  else
    c.ok = _copy_db(src, dst);   // with its own undo
  return c.ok;
}

// ---------------------------------------------------------------------------
// AI tools: state and choice

typedef struct _provider_t
{
  gchar *id, *name, *install, *connect;
  gboolean installed, logged_in, images;
  GPtrArray *models;
} _provider_t;

static const char *_ids[] = { "claude", "codex", "gemini" };
static const char *_names[] = { N_("Claude Code"), N_("Codex"), N_("Gemini") };
#define N_PROVIDERS 3

static _provider_t _providers[N_PROVIDERS];
static gboolean _known = FALSE, _checking = FALSE;
static GList *_uis = NULL;

struct dt_lsai_provider_ui_t
{
  GtkWidget *box, *combo, *model, *state, *button;
  gchar *conf_key;
  gboolean images_only;
  gboolean clicked;      // the button was used, it now checks again
};

static void _ui_update(dt_lsai_provider_ui_t *ui);

static gboolean _providers_ready(gpointer data)
{
  _checking = FALSE;
  for(GList *l = _uis; l; l = g_list_next(l)) _ui_update(l->data);
  return G_SOURCE_REMOVE;
}

// the answer of the helper, applied in the gui thread
static gboolean _providers_apply(gpointer data)
{
  JsonObject *res = data;
  if(res)
  {
    JsonArray *arr = json_object_get_array_member(res, "providers");
    for(guint k = 0; arr && k < json_array_get_length(arr); k++)
    {
      JsonObject *o = json_array_get_object_element(arr, k);
      const char *id = json_object_get_string_member_with_default(o, "id", "");
      for(int p = 0; p < N_PROVIDERS; p++)
      {
        if(strcmp(id, _ids[p])) continue;
        _provider_t *pr = &_providers[p];
        g_free(pr->install);
        g_free(pr->connect);
        pr->install = g_strdup(json_object_get_string_member_with_default(o, "install", ""));
        pr->connect = g_strdup(json_object_get_string_member_with_default(o, "connect", ""));
        pr->installed = json_object_get_boolean_member_with_default(o, "installed", FALSE);
        pr->logged_in = json_object_get_boolean_member_with_default(o, "logged_in", FALSE);
        pr->images = json_object_get_boolean_member_with_default(o, "images", FALSE);
        if(pr->models) g_ptr_array_free(pr->models, TRUE);
        pr->models = g_ptr_array_new_with_free_func(g_free);
        JsonArray *models = json_object_get_array_member(o, "models");
        for(guint m = 0; models && m < json_array_get_length(models); m++)
          g_ptr_array_add(pr->models, g_strdup(json_array_get_string_element(models, m)));
      }
    }
    _known = TRUE;
    json_object_unref(res);
  }
  return _providers_ready(NULL);
}

static int32_t _providers_job(dt_job_t *job)
{
  JsonObject *req = json_object_new();
  GError *error = NULL;
  JsonObject *res = dt_lsai_run("providers", req, NULL, &error);
  json_object_unref(req);
  if(!res)
  {
    dt_print(DT_DEBUG_ALWAYS, "[lightspeed ai] %s", error ? error->message : "providers failed");
    g_clear_error(&error);
  }
  g_idle_add(_providers_apply, res);
  return 0;
}

void dt_lsai_providers_refresh(void)
{
  if(_checking) return;
  _checking = TRUE;
  for(GList *l = _uis; l; l = g_list_next(l)) _ui_update(l->data);
  dt_job_t *job = dt_control_job_create(_providers_job, "%s", _("checking the AI tools"));
  if(job)
    dt_control_add_job(DT_JOB_QUEUE_SYSTEM_BG, job);
  else
    _checking = FALSE;
}

static int _ui_selected(dt_lsai_provider_ui_t *ui)
{
  return CLAMP(dt_bauhaus_combobox_get(ui->combo), 0, N_PROVIDERS - 1);
}

static gchar *_model_key(dt_lsai_provider_ui_t *ui, const int p)
{
  return g_strdup_printf("%s_model_%s", ui->conf_key, _ids[p]);
}

static void _ui_update(dt_lsai_provider_ui_t *ui)
{
  const int p = _ui_selected(ui);
  const _provider_t *pr = &_providers[p];

  // models of the chosen tool, the saved one selected
  ++darktable.gui->reset;
  dt_bauhaus_combobox_clear(ui->model);
  dt_bauhaus_combobox_add(ui->model, _("default"));
  gchar *key = _model_key(ui, p);
  const char *saved = dt_conf_get_string_const(key);
  int sel = 0;
  for(guint m = 0; pr->models && m < pr->models->len; m++)
  {
    const char *name = g_ptr_array_index(pr->models, m);
    if(!*name) continue;
    dt_bauhaus_combobox_add(ui->model, name);
    if(!g_strcmp0(saved, name)) sel = dt_bauhaus_combobox_length(ui->model) - 1;
  }
  g_free(key);
  dt_bauhaus_combobox_set(ui->model, sel);
  --darktable.gui->reset;

  const char *state;
  const char *button = NULL;
  if(_checking && !_known)
    state = _("checking...");
  else if(!pr->installed)
  {
    state = _("not installed");
    button = _("Install...");
  }
  else if(!pr->logged_in)
  {
    state = _("not connected");
    button = _("Connect...");
  }
  else if(ui->images_only && !pr->images)
    state = _("cannot create images, choose Codex");
  else
    state = _("connected");
  if(ui->clicked && button) button = _("Check Again");
  gtk_label_set_text(GTK_LABEL(ui->state), state);
  gtk_widget_set_visible(ui->button, button != NULL);
  if(button) gtk_button_set_label(GTK_BUTTON(ui->button), button);
}

static void _open_console(const char *command)
{
#ifdef _WIN32
  // a visible console: the user logs in there (and in the browser it opens)
  gchar *params = g_strdup_printf("/k %s", command);
  wchar_t *wparams = g_utf8_to_utf16(params, -1, NULL, NULL, NULL);
  ShellExecuteW(NULL, L"open", L"cmd.exe", wparams, NULL, SW_SHOWNORMAL);
  g_free(wparams);
  g_free(params);
#else
  gchar *argv[] = { "x-terminal-emulator", "-e", "sh", "-c", (gchar *)command, NULL };
  g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
#endif
}

static void _button_clicked(GtkButton *button, dt_lsai_provider_ui_t *ui)
{
  const _provider_t *pr = &_providers[_ui_selected(ui)];
  if(ui->clicked)
  {
    ui->clicked = FALSE;
    dt_lsai_providers_refresh();
    return;
  }
  const char *cmd = !pr->installed ? pr->install : pr->connect;
  if(cmd && *cmd)
  {
    _open_console(cmd);
    dt_control_log(!pr->installed
                   ? _("installing %s in the window that opened, then click \"Check Again\"")
                   : _("log in to %s in the window that opened, then click \"Check Again\""),
                   _(_names[_ui_selected(ui)]));
  }
  ui->clicked = TRUE;
  _ui_update(ui);
}

static void _combo_changed(GtkWidget *w, dt_lsai_provider_ui_t *ui)
{
  if(darktable.gui->reset) return;
  dt_conf_set_string(ui->conf_key, _ids[_ui_selected(ui)]);
  ui->clicked = FALSE;
  _ui_update(ui);
}

static void _model_changed(GtkWidget *w, dt_lsai_provider_ui_t *ui)
{
  if(darktable.gui->reset) return;
  gchar *key = _model_key(ui, _ui_selected(ui));
  const int sel = dt_bauhaus_combobox_get(ui->model);
  dt_conf_set_string(key, sel <= 0 ? "" : dt_bauhaus_combobox_get_text(ui->model));
  g_free(key);
}

static void _ui_mapped(GtkWidget *w, gpointer data)
{
  if(!_known) dt_lsai_providers_refresh();
}

dt_lsai_provider_ui_t *dt_lsai_provider_ui_new(const char *conf_key,
                                               const char *default_provider,
                                               const gboolean images_only)
{
  dt_lsai_provider_ui_t *ui = g_malloc0(sizeof(dt_lsai_provider_ui_t));
  ui->conf_key = g_strdup(conf_key);
  ui->images_only = images_only;

  ui->combo = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(ui->combo, NULL, N_("AI"));
  for(int p = 0; p < N_PROVIDERS; p++) dt_bauhaus_combobox_add(ui->combo, _(_names[p]));
  gtk_widget_set_tooltip_text(ui->combo, _("the AI tool installed on this computer that does the work,"
                                           " with your subscription (no API key)"));
  const char *saved = dt_conf_key_not_empty(conf_key) ? dt_conf_get_string_const(conf_key) : default_provider;
  int sel = 0;
  for(int p = 0; p < N_PROVIDERS; p++)
    if(!g_strcmp0(saved, _ids[p])) sel = p;
  dt_bauhaus_combobox_set(ui->combo, sel);
  g_signal_connect(ui->combo, "value-changed", G_CALLBACK(_combo_changed), ui);

  ui->model = dt_bauhaus_combobox_new(NULL);
  dt_bauhaus_widget_set_label(ui->model, NULL, N_("model"));
  gtk_widget_set_tooltip_text(ui->model, _("model of the AI tool, default uses its own setting"));
  g_signal_connect(ui->model, "value-changed", G_CALLBACK(_model_changed), ui);

  ui->state = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(ui->state), 0.0f);
  gtk_widget_set_name(ui->state, "lsai-state");
  ui->button = gtk_button_new_with_label(_("Connect..."));
  gtk_widget_set_no_show_all(ui->button, TRUE);
  gtk_widget_set_tooltip_text(ui->button, _("opens a window to install the tool or to log in with your account"));
  g_signal_connect(ui->button, "clicked", G_CALLBACK(_button_clicked), ui);

  ui->box = dt_gui_vbox(ui->combo, ui->model, dt_gui_hbox(dt_gui_expand(ui->state), ui->button));

  _uis = g_list_append(_uis, ui);
  _ui_update(ui);
  // the tools are checked when a panel with the choice is shown the first
  // time, not at every start of the application
  g_signal_connect(ui->box, "map", G_CALLBACK(_ui_mapped), NULL);
  return ui;
}

GtkWidget *dt_lsai_provider_ui_widget(dt_lsai_provider_ui_t *ui)
{
  return ui->box;
}

void dt_lsai_provider_ui_free(dt_lsai_provider_ui_t *ui)
{
  if(!ui) return;
  _uis = g_list_remove(_uis, ui);
  g_free(ui->conf_key);
  g_free(ui);
}

gboolean dt_lsai_provider_ui_get(dt_lsai_provider_ui_t *ui, gchar **provider, gchar **model, gchar **why)
{
  const int p = _ui_selected(ui);
  const _provider_t *pr = &_providers[p];
  *provider = g_strdup(_ids[p]);
  gchar *key = _model_key(ui, p);
  *model = g_strdup(dt_conf_get_string_const(key));
  g_free(key);
  *why = NULL;
  if(_known && !pr->installed)
    *why = g_strdup_printf(_("%s is not installed, click \"Install...\""), _(_names[p]));
  else if(_known && !pr->logged_in)
    *why = g_strdup_printf(_("%s is not connected, click \"Connect...\""), _(_names[p]));
  else if(ui->images_only && _known && !pr->images)
    *why = g_strdup_printf(_("%s cannot create images, choose Codex"), _(_names[p]));
  return *why == NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
