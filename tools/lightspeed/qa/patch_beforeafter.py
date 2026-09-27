def rep(p, old, new, count=1):
    s = open(p, encoding='utf-8').read()
    assert old in s, (p, old[:70])
    s = s.replace(old, new, count)
    open(p, 'w', encoding='utf-8', newline='\n').write(s)

p = r'C:\lightspeed\darktable\src\libs\snapshots.c'

rep(p, '''  /* snapshots */
  dt_lib_snapshot_t snapshot[MAX_SNAPSHOT];''', '''  /* snapshots, the extra last one is the hidden Lightroom-style "before" */
  dt_lib_snapshot_t snapshot[MAX_SNAPSHOT + 1];

  /* state saved while before/after is shown */
  int before_prev_selected;
  gboolean before_prev_vertical, before_prev_inverted, before_prev_sidebyside;
  double before_prev_x, before_prev_y;''')

rep(p, '''static void _lib_snapshots_toggle_last(dt_action_t *action)
{''', '''#define BEFORE_INDEX MAX_SNAPSHOT

// history end of the image as it was after import: the leading history items
// that are default modules or auto-applied presets
static int _before_history_end(dt_develop_t *dev)
{
  int base = 0, k = 0;
  for(GList *h = dev->history; h && k < dev->history_end; h = g_list_next(h), k++)
  {
    const dt_dev_history_item_t *hist = h->data;
    const gboolean auto_item =
      g_str_has_prefix(hist->multi_name, "_builtin_")
      || (hist->module && (hist->module->default_enabled || hist->module->hide_enable_button));
    if(!auto_item) break;
    base = k + 1;
  }
  return base;
}

static void _before_after(dt_lib_module_t *self, const gboolean split)
{
  dt_lib_snapshots_t *d = self->data;
  dt_develop_t *dev = darktable.develop;

  if(d->selected == BEFORE_INDEX)
  {
    // back to "after"
    d->selected = d->before_prev_selected;
    d->vertical = d->before_prev_vertical;
    d->inverted = d->before_prev_inverted;
    d->sidebyside = d->before_prev_sidebyside;
    d->vp_xpointer = d->before_prev_x;
    d->vp_ypointer = d->before_prev_y;
    if(d->selected >= 0) d->snap_requested = TRUE;
    darktable.lib->proxy.snapshots.enabled = d->selected >= 0;
    dt_control_log(_("after"));
    dt_control_queue_redraw_center();
    return;
  }

  const dt_imgid_t imgid = dev->image_storage.id;
  if(!dt_is_valid_imgid(imgid)) return;

  // make sure the snapshot sees the current history
  dt_dev_write_history(dev);

  dt_lib_snapshot_t *s = &d->snapshot[BEFORE_INDEX];
  g_free(s->module);
  g_free(s->label);
  s->module = s->label = NULL;
  dt_free_align(s->buf);
  s->buf = NULL;
  s->id = SNAPSHOT_ID_OFFSET | BEFORE_INDEX;
  s->imgid = imgid;
  s->history_end = _before_history_end(dev);
  s->ctx = 0;
  dt_history_snapshot_create(s->imgid, s->id, s->history_end);

  d->before_prev_selected = d->selected;
  d->before_prev_vertical = d->vertical;
  d->before_prev_inverted = d->inverted;
  d->before_prev_sidebyside = d->sidebyside;
  d->before_prev_x = d->vp_xpointer;
  d->before_prev_y = d->vp_ypointer;

  d->selected = BEFORE_INDEX;
  d->vertical = TRUE;
  d->inverted = FALSE;
  d->sidebyside = FALSE;
  d->vp_xpointer = split ? 0.5 : 1.0;
  d->vp_ypointer = 0.5;
  d->snap_requested = TRUE;
  darktable.lib->proxy.snapshots.enabled = TRUE;
  dt_control_log(split ? _("before | after") : _("before"));
  dt_control_queue_redraw_center();
}

static void _before_after_toggle(dt_action_t *action)
{
  _before_after(dt_action_lib(action), FALSE);
}

static void _before_after_split(dt_action_t *action)
{
  _before_after(dt_action_lib(action), TRUE);
}

static void _lib_snapshots_toggle_last(dt_action_t *action)
{''')

rep(p, '''  dt_action_register(DT_ACTION(self), N_("toggle last snapshot"),
                     _lib_snapshots_toggle_last, 0, 0);''', '''  dt_action_register(DT_ACTION(self), N_("toggle last snapshot"),
                     _lib_snapshots_toggle_last, 0, 0);

  // Lightroom: \\ shows the photo before the edits, Y a before / after split
  d->snapshot[BEFORE_INDEX].id = SNAPSHOT_ID_OFFSET | BEFORE_INDEX;
  _clear_snapshot_entry(&d->snapshot[BEFORE_INDEX]);
  dt_action_register(DT_ACTION(self), N_("before/after"),
                     _before_after_toggle, GDK_KEY_backslash, 0);
  dt_action_register(DT_ACTION(self), N_("before/after split"),
                     _before_after_split, GDK_KEY_y, 0);''')
print('ok')
