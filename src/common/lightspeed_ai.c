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

#include "common/lightspeed_ai.h"
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
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
  // no console window pops up for the helper
  const BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                 NULL, NULL, &si, &pi);
  g_free(wcmd);
  if(hlog != INVALID_HANDLE_VALUE) CloseHandle(hlog);
  if(hnul != INVALID_HANDLE_VALUE) CloseHandle(hnul);
  if(!ok)
  {
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, _("cannot start the AI helper (error %lu)"),
                GetLastError());
    return FALSE;
  }
  while(WaitForSingleObject(pi.hProcess, 300) == WAIT_TIMEOUT)
  {
    _read_progress(log, job, &pos);
    if(_cancelled(job))
    {
      TerminateProcess(pi.hProcess, 1);
      break;
    }
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
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
                _("the AI helper is missing, reinstall Lightspeed"));
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

static int32_t _providers_job(dt_job_t *job)
{
  JsonObject *req = json_object_new();
  GError *error = NULL;
  JsonObject *res = dt_lsai_run("providers", req, NULL, &error);
  json_object_unref(req);
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
  else
  {
    dt_print(DT_DEBUG_ALWAYS, "[lightspeed ai] %s", error ? error->message : "providers failed");
    g_clear_error(&error);
  }
  g_idle_add(_providers_ready, NULL);
  return 0;
}

void dt_lsai_providers_refresh(void)
{
  if(_checking) return;
  _checking = TRUE;
  for(GList *l = _uis; l; l = g_list_next(l)) _ui_update(l->data);
  dt_job_t *job = dt_control_job_create(_providers_job, "%s", _("checking the AI tools"));
  if(job) dt_control_add_job(DT_JOB_QUEUE_SYSTEM_BG, job);
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
