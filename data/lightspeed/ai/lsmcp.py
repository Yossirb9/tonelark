"""Tonelark MCP server: lets a Claude Code, Codex or Gemini CLI chat work
with the photos of the running Tonelark.

    claude mcp add -s user tonelark -- <python> <this file>
    codex mcp add tonelark -- <python> <this file>

(Tonelark does it with "Connect Chat" in the AI assistant panel.)

It talks to Tonelark through request files: <config dir>/mcp/in/<id>.json,
answered in <config dir>/mcp/out/<id>.json (see src/common/lightspeed_bridge.c).
MCP over stdio: one JSON-RPC message per line.
"""
import base64
import json
import os
import subprocess
import sys
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lsedit  # noqa: E402

VERSION = '1.3.0'
PROTOCOL = '2025-06-18'


def config_dir():
    d = os.environ.get('TONELARK_CONFIGDIR') or os.environ.get('LIGHTSPEED_CONFIGDIR')
    if d:
        return d
    if os.name == 'nt':
        base = os.environ.get('LOCALAPPDATA', os.path.expanduser('~'))
    else:
        base = os.environ.get('XDG_CONFIG_HOME', os.path.join(os.path.expanduser('~'), '.config'))
    new, old = os.path.join(base, 'tonelark'), os.path.join(base, 'lightspeed')
    # the first versions were called Lightspeed
    return new if os.path.isdir(new) or not os.path.isdir(old) else old


class TonelarkError(Exception):
    pass


def _alive():
    try:
        with open(os.path.join(config_dir(), 'mcp', 'tonelark.json'), encoding='utf-8') as f:
            beat = json.load(f)
    except (OSError, ValueError):
        return None
    return beat if time.time() - float(beat.get('time', 0)) < 30 else None


def call(cmd, args=None, timeout=60):
    """one request to the running Tonelark"""
    if not _alive():
        raise TonelarkError('Tonelark is not running. Start it, or call start_tonelark.')
    base = os.path.join(config_dir(), 'mcp')
    rid = uuid.uuid4().hex
    req = os.path.join(base, 'in', rid + '.json')
    res = os.path.join(base, 'out', rid + '.json')
    with open(req + '.tmp', 'w', encoding='utf-8') as f:
        json.dump(dict(id=rid, cmd=cmd, args=args or {}), f, ensure_ascii=False)
    os.replace(req + '.tmp', req)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(res):
            try:
                with open(res, encoding='utf-8') as f:
                    answer = json.load(f)
            except (OSError, ValueError):
                time.sleep(0.1)
                continue
            try:
                os.remove(res)
            except OSError:
                pass
            if not answer.get('ok'):
                raise TonelarkError(answer.get('error') or 'Tonelark could not do it')
            return answer.get('result') or {}
        time.sleep(0.1)
    try:
        os.remove(req)
    except OSError:
        pass
    raise TonelarkError('Tonelark did not answer in %d s' % timeout)


# ---------------------------------------------------------------------------
# tools

IDS = {'type': 'array', 'items': {'type': 'integer'}, 'description': 'photo ids (from list_photos)'}
COLORS = ['red', 'yellow', 'green', 'blue', 'purple']
AI_TOOLS = {
    'find_best_shots': 'lib/aicull/Find Best Shots',
    'rate_with_ai': 'lib/aicull/Rate with AI',
    'best_take': 'lib/aicull/Best Take',
    'auto_edit': 'lib/aiassist/Auto Edit',
    'match_look': 'lib/aiassist/Match Look',
    'keywords_and_captions': 'lib/aiassist/Keywords & Captions',
    'suggest_crops': 'lib/aiassist/Suggest Crops',
}

TOOLS = [
    dict(name='tonelark_status', description='Is Tonelark running, which view is open (lighttable = library, darkroom = develop), which photo is open, how many photos are in the current collection and which are selected.',
         inputSchema={'type': 'object', 'properties': {}}),
    dict(name='start_tonelark', description='Start Tonelark when it is not running.',
         inputSchema={'type': 'object', 'properties': {}}),
    dict(name='list_photos', description='List photos with their id, file, folder, date, camera settings, stars, pick/reject flag, color labels, keywords, title, caption and AI notes. Filters are optional.',
         inputSchema={'type': 'object', 'properties': {
             'scope': {'type': 'string', 'enum': ['collection', 'selected', 'all'], 'description': 'collection = what the library shows now (default), selected, or all photos of the catalog'},
             'search': {'type': 'string', 'description': 'words to find in file name, folder, keywords, title, caption, notes, camera, date'},
             'min_stars': {'type': 'integer', 'minimum': 0, 'maximum': 5},
             'label': {'type': 'string', 'enum': COLORS},
             'flag': {'type': 'string', 'enum': ['picked', 'rejected', 'unflagged']},
             'edited': {'type': 'boolean'},
             'limit': {'type': 'integer', 'default': 50, 'maximum': 500},
             'offset': {'type': 'integer', 'default': 0}}}),
    dict(name='get_photo', description='Details of one photo and its current edit as Lightroom-style settings (Exposure, Contrast, Temperature...) and crop.',
         inputSchema={'type': 'object', 'properties': {'id': {'type': 'integer'}}, 'required': ['id']}),
    dict(name='view_photos', description='See photos as they look with their edits. One id: the photo itself. Several ids (up to 12): one contact sheet where each photo has a yellow number, which is much cheaper than viewing them one by one.',
         inputSchema={'type': 'object', 'properties': {'ids': dict(IDS, maxItems=12), 'size': {'type': 'integer', 'default': 1024, 'description': 'longest side of a single photo in pixels (256..2048)'}}, 'required': ['ids']}),
    dict(name='rate_photos', description='Set the star rating (0 to 5).',
         inputSchema={'type': 'object', 'properties': {'ids': IDS, 'stars': {'type': 'integer', 'minimum': 0, 'maximum': 5}}, 'required': ['ids', 'stars']}),
    dict(name='flag_photos', description='Pick, reject or unflag photos.',
         inputSchema={'type': 'object', 'properties': {'ids': IDS, 'flag': {'type': 'string', 'enum': ['pick', 'reject', 'none']}}, 'required': ['ids', 'flag']}),
    dict(name='label_photos', description='Color labels: set exactly these labels, or add / remove them.',
         inputSchema={'type': 'object', 'properties': {'ids': IDS, 'labels': {'type': 'array', 'items': {'type': 'string', 'enum': COLORS}}, 'mode': {'type': 'string', 'enum': ['set', 'add', 'remove'], 'default': 'set'}}, 'required': ['ids', 'labels']}),
    dict(name='tag_photos', description='Add or remove keywords.',
         inputSchema={'type': 'object', 'properties': {'ids': IDS, 'add': {'type': 'array', 'items': {'type': 'string'}}, 'remove': {'type': 'array', 'items': {'type': 'string'}}}, 'required': ['ids']}),
    dict(name='describe_photos', description='Set the title and/or caption of photos.',
         inputSchema={'type': 'object', 'properties': {'ids': IDS, 'title': {'type': 'string'}, 'caption': {'type': 'string'}}, 'required': ['ids']}),
    dict(name='select_photos', description='Select photos in Tonelark (so the user sees them, or for run_ai_tool).',
         inputSchema={'type': 'object', 'properties': {'ids': IDS}, 'required': ['ids']}),
    dict(name='open_photo', description='Open a photo in the Develop (darkroom) view.',
         inputSchema={'type': 'object', 'properties': {'id': {'type': 'integer'}}, 'required': ['id']}),
    dict(name='edit_photos', description='Edit photos with Lightroom-style sliders, non-destructively (the user can change or undo it). Only the given settings change; values are absolute, not added. Settings: ' + lsedit.VOCABULARY,
         inputSchema={'type': 'object', 'properties': {'ids': IDS, 'settings': {'type': 'object', 'description': 'e.g. {"Exposure": 0.3, "Highlights": -40, "Temperature": 5800, "HSL": {"Orange": {"sat": -10}}}'}}, 'required': ['ids', 'settings']}),
    dict(name='crop_photo', description='Crop and straighten a photo. The rectangle is in percent of the photo as it is shown now (view it first).',
         inputSchema={'type': 'object', 'properties': {
             'id': {'type': 'integer'},
             'left': {'type': 'number'}, 'top': {'type': 'number'}, 'right': {'type': 'number'}, 'bottom': {'type': 'number'},
             'angle': {'type': 'number', 'description': 'degrees to rotate to level the photo, positive = clockwise (0 = no change)'},
             'aspect': {'type': 'string', 'description': 'original, free, or width:height like 1:1, 4:5, 16:9, 9:16', 'default': 'free'}},
             'required': ['id', 'left', 'top', 'right', 'bottom']}),
    dict(name='copy_edit', description='Copy the whole look of one photo to others (like Lightroom sync settings; crop and retouching are not copied).',
         inputSchema={'type': 'object', 'properties': {'from_id': {'type': 'integer'}, 'to_ids': IDS}, 'required': ['from_id', 'to_ids']}),
    dict(name='export_photos', description='Export photos as JPEG files with their edits.',
         inputSchema={'type': 'object', 'properties': {'ids': IDS, 'folder': {'type': 'string', 'description': 'absolute folder path'}, 'max_size': {'type': 'integer', 'description': 'longest side in pixels, 0 = full size', 'default': 0}, 'quality': {'type': 'integer', 'default': 92}}, 'required': ['ids', 'folder']}),
    dict(name='run_ai_tool', description='Run one of the AI tools of Tonelark on photos (they work on contact sheets, cheaply, and the results appear in Tonelark: stars, flags, notes, edits, keywords). find_best_shots works on this computer without AI. The photos must be in the collection Tonelark shows (list_photos scope collection). Check the results later with list_photos.',
         inputSchema={'type': 'object', 'properties': {'tool': {'type': 'string', 'enum': sorted(AI_TOOLS)}, 'ids': IDS, 'instruction': {'type': 'string', 'description': 'for auto_edit: the look to make'}}, 'required': ['tool', 'ids']}),
]

# what each tool does, for the approval prompts of the chat tools
READ_ONLY = {'tonelark_status', 'list_photos', 'get_photo', 'view_photos'}
for _t in TOOLS:
    _ro = _t['name'] in READ_ONLY
    _t['annotations'] = dict(readOnlyHint=_ro, destructiveHint=False, openWorldHint=False,
                             idempotentHint=_ro or _t['name'] not in ('run_ai_tool', 'export_photos', 'start_tonelark'))

INSTRUCTIONS = """Tonelark is the photo editor (a Lightroom replacement) running on this computer. Photos have integer ids: find them with list_photos. To look at photos use view_photos: several ids at once give one numbered contact sheet, which is much cheaper. Edits are non-destructive Lightroom-style sliders (edit_photos, crop_photo, copy_edit); the user can see and undo them in Tonelark. Ratings, flags, labels, keywords and captions work like in Lightroom."""


def _text(obj):
    return [dict(type='text', text=obj if isinstance(obj, str) else json.dumps(obj, ensure_ascii=False, indent=1))]


def _image(path):
    with open(path, 'rb') as f:
        data = base64.b64encode(f.read()).decode('ascii')
    return dict(type='image', data=data, mimeType='image/jpeg')


def _current(pid):
    return call('photo', dict(id=pid), 120)


def tool_view(a):
    ids = [int(i) for i in a.get('ids') or []][:12]
    if not ids:
        raise TonelarkError('give photo ids')
    single = len(ids) == 1
    size = max(256, min(2048, int(a.get('size', 1024)))) if single else 520
    res = call('preview', dict(ids=ids, size=size), 180)
    files = [p for p in res.get('previews', []) if p.get('path')]
    if not files:
        raise TonelarkError('no preview could be made')
    try:
        if single:
            return [_image(files[0]['path'])] + _text('photo id %d' % files[0]['id'])
        import lsai  # numpy / OpenCV: only for sheets
        sheet = os.path.join(os.path.dirname(files[0]['path']), uuid.uuid4().hex + '_sheet.jpg')
        lsai.make_sheet([f['path'] for f in files], sheet, cols=4, cell=420)
        legend = ', '.join('%d = id %d' % (k + 1, f['id']) for k, f in enumerate(files))
        out = [_image(sheet)] + _text('contact sheet: ' + legend)
        os.remove(sheet)
        return out
    finally:
        for f in files:
            try:
                os.remove(f['path'])
            except OSError:
                pass


def tool_edit(a):
    ids = [int(i) for i in a.get('ids') or []]
    settings = a.get('settings') or {}
    current = {}
    low = {str(k).lower() for k in settings}
    if ids and ('temperature' in low) != ('tint' in low):
        current = _current(ids[0]).get('edit', {})
    crs = lsedit.to_crs(settings, current)
    if not crs:
        raise TonelarkError('no known setting in ' + json.dumps(settings) + '. Settings: ' + lsedit.VOCABULARY)
    res = call('edit', dict(ids=ids, edit=crs), 60 + 20 * len(ids))
    res['applied'] = crs
    return _text(res)


def tool_crop(a):
    pid = int(a['id'])
    edit = _current(pid).get('edit', {})
    rect = [float(a.get(k, d)) / 100.0 for k, d in (('left', 0), ('top', 0), ('right', 100), ('bottom', 100))]
    angle = a.get('angle')
    crop = lsedit.crop_edit(edit.get('width', 3), edit.get('height', 2), rect, a.get('aspect') or 'free', edit,
                            float(angle) if angle not in (None, '') else None)
    res = call('edit', dict(ids=[pid], edit=crop), 120)
    res['applied'] = crop
    return _text(res)


def tool_start(a):
    if _alive():
        return _text('Tonelark is running')
    prefix = os.path.normpath(os.path.join(HERE, '..', '..', '..', '..'))
    for name in ('Tonelark.exe', 'tonelark.exe', 'darktable.exe', 'tonelark', 'darktable'):
        exe = os.path.join(prefix, 'bin', name)
        if os.path.exists(exe):
            flags = 0
            if os.name == 'nt':
                flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
            subprocess.Popen([exe], cwd=os.path.dirname(exe), close_fds=True, creationflags=flags,
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for _ in range(120):
                time.sleep(1)
                if _alive():
                    return _text('Tonelark started')
            return _text('Tonelark is starting, try again in a moment')
    raise TonelarkError('Tonelark was not found next to the MCP server')


def run_tool(name, a):
    ids = a.get('ids')
    if name == 'tonelark_status':
        return _text(call('status', {}, 20))
    if name == 'start_tonelark':
        return tool_start(a)
    if name == 'list_photos':
        keys = ('scope', 'search', 'min_stars', 'label', 'flag', 'edited', 'limit', 'offset')
        return _text(call('list', {k: a[k] for k in keys if k in a}, 60))
    if name == 'get_photo':
        res = _current(int(a['id']))
        edit = res.pop('edit', {})
        res['edit'] = lsedit.friendly(edit)
        res['crop'] = {k: edit.get(k) for k in ('CropLeft', 'CropTop', 'CropRight', 'CropBottom', 'Straighten')}
        res['raw'] = edit.get('raw')
        return _text(res)
    if name == 'view_photos':
        return tool_view(a)
    if name == 'rate_photos':
        return _text(call('rate', dict(ids=ids, stars=a.get('stars', 0))))
    if name == 'flag_photos':
        return _text(call('flag', dict(ids=ids, flag=a.get('flag'))))
    if name == 'label_photos':
        return _text(call('label', dict(ids=ids, labels=a.get('labels') or [], mode=a.get('mode', 'set'))))
    if name == 'tag_photos':
        return _text(call('tag', dict(ids=ids, add=a.get('add') or [], remove=a.get('remove') or [])))
    if name == 'describe_photos':
        return _text(call('describe', {k: a[k] for k in ('ids', 'title', 'caption') if k in a}))
    if name == 'select_photos':
        return _text(call('select', dict(ids=ids)))
    if name == 'open_photo':
        return _text(call('open', dict(id=int(a['id']))))
    if name == 'edit_photos':
        return tool_edit(a)
    if name == 'crop_photo':
        return tool_crop(a)
    if name == 'copy_edit':
        to = a.get('to_ids') or []
        return _text(call('copy_edit', {'from': int(a['from_id']), 'ids': to}, 60 + 20 * len(to)))
    if name == 'export_photos':
        return _text(call('export', {k: a[k] for k in ('ids', 'folder', 'max_size', 'quality') if k in a},
                          120 + 60 * len(ids or [])))
    if name == 'run_ai_tool':
        tool = AI_TOOLS.get(a.get('tool'))
        if not tool:
            raise TonelarkError('unknown tool, one of: ' + ', '.join(sorted(AI_TOOLS)))
        args = dict(action=tool, ids=ids or [])
        if a.get('instruction'):
            args['text'] = a['instruction']
        return _text(call('action', args, 30))
    raise TonelarkError('unknown tool ' + name)


# ---------------------------------------------------------------------------
# MCP over stdio

def send(msg):
    sys.stdout.write(json.dumps(msg, ensure_ascii=False) + '\n')
    sys.stdout.flush()


def handle(msg):
    method, mid = msg.get('method'), msg.get('id')
    if method == 'initialize':
        asked = (msg.get('params') or {}).get('protocolVersion') or PROTOCOL
        return dict(protocolVersion=asked, capabilities=dict(tools=dict(listChanged=False)),
                    serverInfo=dict(name='tonelark', version=VERSION), instructions=INSTRUCTIONS)
    if method == 'ping':
        return {}
    if method == 'tools/list':
        return dict(tools=TOOLS)
    if method == 'tools/call':
        p = msg.get('params') or {}
        try:
            return dict(content=run_tool(p.get('name'), p.get('arguments') or {}), isError=False)
        except TonelarkError as e:
            return dict(content=_text(str(e)), isError=True)
        except Exception as e:     # noqa: BLE001
            return dict(content=_text('%s: %s' % (type(e).__name__, e)), isError=True)
    if mid is not None:
        raise KeyError(method)
    return None


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', newline='\n')
        sys.stdin.reconfigure(encoding='utf-8')
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            send(dict(jsonrpc='2.0', id=None, error=dict(code=-32700, message='parse error')))
            continue
        if 'method' not in msg:
            continue       # an answer to a request of ours: none are made
        try:
            result = handle(msg)
            if msg.get('id') is not None:
                send(dict(jsonrpc='2.0', id=msg['id'], result=result))
        except KeyError as e:
            send(dict(jsonrpc='2.0', id=msg.get('id'), error=dict(code=-32601, message='unknown method %s' % e)))


if __name__ == '__main__':
    main()
