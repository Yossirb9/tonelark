"""Lightspeed AI helper.

Runs next to Lightspeed (darktable) and does the work that needs computer
vision or an AI model:

  cull      local culling: quality of every photo (sharpness, exposure, faces:
            eyes open, smile, face sharpness), bursts and the best of each
  rate      AI culling: photos are packed on numbered contact sheets, one call
            per sheet to Claude Code, Codex or Gemini CLI (their subscription,
            no API key), the model answers with JSON
  besttake  group photos: the best face of every person across a burst,
            aligned and blended into one image
  genedit   generative edit of a region with Codex's image model, only the
            region is blended back into the full resolution image
  providers which command line AI tools are installed and logged in

    python lsai.py <command> <request.json> <response.json>

Progress goes to stderr as "PROGRESS <0..1> <text>" lines.
"""
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

import numpy as np
import cv2

HERE = os.path.dirname(os.path.abspath(__file__))
MODELS = os.path.join(HERE, 'models')
YUNET = os.path.join(MODELS, 'face_detection_yunet_2023mar.onnx')
FER = os.path.join(MODELS, 'facial_expression_recognition_mobilefacenet_2022july.onnx')

ANALYSIS_SIDE = 1600      # longest side used to analyse photos


def progress(fraction, text=''):
    print('PROGRESS %.3f %s' % (max(0.0, min(1.0, fraction)), text), file=sys.stderr, flush=True)


def log(text):
    print(text, file=sys.stderr, flush=True)


# ---------------------------------------------------------------------------
# image files (np.fromfile/imdecode: OpenCV cannot open non ASCII paths on Windows)

def imread(path, flags=cv2.IMREAD_UNCHANGED):
    data = np.fromfile(path, dtype=np.uint8)
    img = cv2.imdecode(data, flags)
    if img is None:
        raise ValueError('cannot read ' + path)
    if img.ndim == 2:
        img = cv2.cvtColor(img, cv2.COLOR_GRAY2BGR)
    if img.shape[2] == 4:
        img = img[:, :, :3]
    return img


def imwrite(path, img, params=()):
    ext = os.path.splitext(path)[1]
    ok, buf = cv2.imencode(ext, img, list(params))
    if not ok:
        raise ValueError('cannot write ' + path)
    buf.tofile(path)


def to8(img):
    if img.dtype == np.uint8:
        return img
    if img.dtype == np.uint16:
        return (img >> 8).astype(np.uint8)
    return np.clip(img * 255.0 + 0.5, 0, 255).astype(np.uint8)


def to_float(img):
    if img.dtype == np.uint8:
        return img.astype(np.float32) / 255.0
    if img.dtype == np.uint16:
        return img.astype(np.float32) / 65535.0
    return img.astype(np.float32)


def from_float(img, dtype):
    if dtype == np.uint8:
        return np.clip(img * 255.0 + 0.5, 0, 255).astype(np.uint8)
    if dtype == np.uint16:
        return np.clip(img * 65535.0 + 0.5, 0, 65535).astype(np.uint16)
    return img.astype(dtype)


def downscale(img, side):
    h, w = img.shape[:2]
    s = side / float(max(h, w))
    if s >= 1.0:
        return img, 1.0
    return cv2.resize(img, (int(round(w * s)), int(round(h * s))), interpolation=cv2.INTER_AREA), s


# ---------------------------------------------------------------------------
# faces

class Faces:
    # reference landmarks of a 112x112 aligned face (eyes, nose, mouth corners)
    REF = np.array([[38.2946, 51.6963], [73.5318, 51.5014], [56.0252, 71.7366],
                    [41.5493, 92.3655], [70.7299, 92.2041]], dtype=np.float32)

    def __init__(self):
        self.det = None
        self.fer = None

    def detect(self, img8):
        """faces of an 8 bit BGR image: box (x, y, w, h), score, 5 landmarks"""
        if self.det is None:
            self.det = cv2.FaceDetectorYN.create(YUNET, '', (320, 320), 0.75, 0.3, 5000)
        h, w = img8.shape[:2]
        self.det.setInputSize((w, h))
        _, faces = self.det.detect(img8)
        out = []
        if faces is not None:
            for f in faces:
                box = [float(v) for v in f[0:4]]
                if box[2] < 24 or box[3] < 24:
                    continue       # too small to judge eyes and expression
                out.append(dict(box=box, lm=np.array(f[4:14], dtype=np.float32).reshape(5, 2),
                                score=float(f[14])))
        return out

    def _aligned(self, img8, lm):
        m, _ = cv2.estimateAffinePartial2D(lm, self.REF, method=cv2.LMEDS)
        if m is None:
            return None
        return cv2.warpAffine(img8, m, (112, 112), borderMode=cv2.BORDER_REPLICATE)

    def happy(self, img8, face):
        """probability of a happy expression (smile)"""
        if not os.path.exists(FER):
            return 0.0
        if self.fer is None:
            self.fer = cv2.dnn.readNet(FER)
        crop = self._aligned(img8, face['lm'])
        if crop is None:
            return 0.0
        rgb = cv2.cvtColor(crop, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
        blob = cv2.dnn.blobFromImage((rgb - 0.5) / 0.5)
        self.fer.setInput(blob)
        logits = self.fer.forward().reshape(-1)
        e = np.exp(logits - logits.max())
        p = e / e.sum()
        return float(p[3])     # angry, disgust, fearful, happy, neutral, sad, surprised

    @staticmethod
    def eyes_open(img8, face):
        """how open the eyes look, from 0 (closed) to 1: an open eye shows a
        dark iris in the middle of the eye patch, with contrast against the
        white of the eye; a closed eye is smooth skin with a lash line or a
        shadow band across the whole patch"""
        gray = cv2.cvtColor(img8, cv2.COLOR_BGR2GRAY)
        lm = face['lm']
        iod = float(np.linalg.norm(lm[0] - lm[1]))
        if iod < 8:
            return 0.5
        half = max(4, int(iod * 0.22))
        vals = []
        for k in (0, 1):
            cx, cy = int(lm[k][0]), int(lm[k][1])
            patch = gray[max(0, cy - half):cy + half, max(0, cx - half):cx + half]
            if patch.shape[0] < 6 or patch.shape[1] < 6:
                continue
            p = cv2.resize(patch, (48, 48), interpolation=cv2.INTER_AREA).astype(np.float32)
            skin = np.percentile(p, 75)
            centre = p[8:40, 14:34]
            dark = (centre < skin * 0.62).mean(axis=1)
            band = (p[8:40] < skin * 0.62).mean(axis=1)
            iris = float(((dark > 0.35) & (band < 0.8)).sum()) / 32.0
            contrast = float(centre.std() / (skin + 1.0))
            vals.append(0.6 * min(1.0, iris / 0.35) + 0.4 * min(1.0, contrast / 0.3))
        return float(np.mean(vals)) if vals else 0.5

    @staticmethod
    def sharpness(img8, face):
        x, y, w, h = [int(v) for v in face['box']]
        crop = img8[max(0, y):y + h, max(0, x):x + w]
        if crop.size == 0:
            return 0.0
        crop = cv2.resize(cv2.cvtColor(crop, cv2.COLOR_BGR2GRAY), (128, 128), interpolation=cv2.INTER_AREA)
        return float(cv2.Laplacian(crop, cv2.CV_32F).var())

    def analyse(self, img8, face):
        return dict(eyes=self.eyes_open(img8, face), happy=self.happy(img8, face),
                    sharp=self.sharpness(img8, face))


FACES = Faces()


# ---------------------------------------------------------------------------
# photo quality

def sharpness(gray):
    """sharpness of the sharpest part of the photo (shallow depth of field
    photos are sharp on the subject only): 90th percentile of the Laplacian
    energy of 32x32 tiles"""
    g, _ = downscale(gray, 1024)
    lap = np.abs(cv2.Laplacian(g.astype(np.float32), cv2.CV_32F))
    h, w = lap.shape
    t = 32
    tiles = lap[:h // t * t, :w // t * t].reshape(h // t, t, w // t, t).mean(axis=(1, 3))
    return float(np.percentile(tiles, 90))


def exposure(gray):
    """1 for a well exposed photo, lower with clipping or very dark/bright"""
    g = gray.astype(np.float32) / 255.0
    clip_hi = float((g > 0.985).mean())
    clip_lo = float((g < 0.015).mean())
    mean = float(g.mean())
    score = 1.0 - min(1.0, clip_hi * 6.0) * 0.5 - min(1.0, clip_lo * 4.0) * 0.3
    score -= min(0.5, abs(mean - 0.45) * 1.2)
    return max(0.0, score)


def signature(gray):
    s = cv2.resize(gray, (32, 32), interpolation=cv2.INTER_AREA).astype(np.float32)
    s -= s.mean()
    n = np.linalg.norm(s)
    return s / n if n > 0 else s


def analyse_photo(path):
    img = imread(path, cv2.IMREAD_COLOR)
    img8, scale = downscale(to8(img), ANALYSIS_SIDE)
    gray = cv2.cvtColor(img8, cv2.COLOR_BGR2GRAY)
    faces = []
    for f in FACES.detect(img8):
        a = FACES.analyse(img8, f)
        faces.append(dict(box=[v / scale for v in f['box']], conf=f['score'], **a))
    return dict(sharp=sharpness(gray), exposure=exposure(gray), faces=faces,
                sig=signature(gray), size=[img.shape[1], img.shape[0]])


# ---------------------------------------------------------------------------
# bursts

def find_bursts(photos, gap=2.0, similarity=0.80):
    """photos: list of dict(id, time, sig); consecutive photos taken within
    `gap` seconds and looking alike form a burst"""
    order = sorted(photos, key=lambda p: (p.get('time') or 0.0, p['id']))
    bursts, current = [], []
    for p in order:
        if current:
            q = current[-1]
            dt = abs((p.get('time') or 0.0) - (q.get('time') or 0.0))
            timed = p.get('time') and q.get('time')
            alike = float((p['sig'] * q['sig']).sum()) >= similarity
            if (timed and dt <= gap and alike) or (not timed and alike):
                current.append(p)
                continue
            bursts.append(current)
        current = [p]
    if current:
        bursts.append(current)
    return bursts


def cmd_cull(req):
    images = req['images']
    photos = []
    for k, im in enumerate(images):
        progress(k / max(1, len(images)), 'analysing %d/%d' % (k + 1, len(images)))
        try:
            a = analyse_photo(im['path'])
        except Exception as e:     # noqa: BLE001
            log('skip %s: %s' % (im['path'], e))
            continue
        a.update(id=im['id'], time=im.get('time'))
        photos.append(a)
    if not photos:
        return dict(images=[], bursts=[])

    # sharpness relative to the shoot: log scale, sharpest photo = 1
    top = max(p['sharp'] for p in photos) or 1.0
    for p in photos:
        p['sharp_n'] = max(0.0, 1.0 + math.log(max(p['sharp'], 1e-6) / top) / 2.5)

    bursts = find_bursts(photos, req.get('burst_gap', 2.0), req.get('similarity', 0.80))
    out = []
    for b, burst in enumerate(bursts):
        # faces: compare the same person across the burst
        fsharp = [f['sharp'] for p in burst for f in p['faces']] or [1.0]
        fs_top = max(fsharp) or 1.0
        for p in burst:
            fscore = None
            if p['faces']:
                vals = []
                for f in p['faces']:
                    s = max(0.0, 1.0 + math.log(max(f['sharp'], 1e-6) / fs_top) / 2.5)
                    f['sharp_rel'] = s
                    vals.append(0.5 * f['eyes'] + 0.2 * f['happy'] + 0.3 * s)
                fscore = float(min(vals) * 0.6 + np.mean(vals) * 0.4)   # the worst face matters
            if fscore is None:
                p['score'] = 0.72 * p['sharp_n'] + 0.28 * p['exposure']
            else:
                p['score'] = 0.35 * p['sharp_n'] + 0.2 * p['exposure'] + 0.45 * fscore
        best = max(burst, key=lambda p: p['score'])
        for p in burst:
            reasons = []
            if p['sharp_n'] < 0.55:
                reasons.append('soft or blurred')
            if p['exposure'] < 0.5:
                reasons.append('exposure problem')
            # a blurred face also looks like closed eyes: say which one it is
            blurred = [f for f in p['faces'] if f.get('sharp_rel', 1.0) < 0.55]
            closed = [f for f in p['faces'] if f['eyes'] < 0.35 and f.get('sharp_rel', 1.0) >= 0.55]
            if blurred:
                reasons.append('a face is blurred')
            if closed:
                reasons.append('eyes may be closed')
            out.append(dict(id=p['id'], score=round(p['score'], 3),
                            stars=int(min(5, max(1, round(p['score'] * 5)))),
                            burst=b, burst_size=len(burst), best=p is best,
                            faces=len(p['faces']), sharp=round(p['sharp_n'], 3),
                            exposure=round(p['exposure'], 3),
                            note=', '.join(reasons)))
    progress(1.0, 'done')
    return dict(images=out, bursts=[[p['id'] for p in b] for b in bursts])


# ---------------------------------------------------------------------------
# AI providers (command line tools, logged in with the user's subscription)

def _clean_env():
    env = dict(os.environ)
    for k in list(env):
        if re.match(r'^(CLAUDE|ANTHROPIC)', k, re.I) or k.upper().startswith('CODEX_'):
            env.pop(k)
    return env


def _which(name):
    return shutil.which(name) or shutil.which(name + '.cmd')


def _run(args, cwd, stdin_text, timeout):
    p = subprocess.run(args, cwd=cwd, input=stdin_text if stdin_text is not None else '',
                       capture_output=True, text=True,
                       encoding='utf-8', errors='replace', timeout=timeout, env=_clean_env(),
                       creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    return p.returncode, p.stdout, p.stderr


def _status(args, timeout=40):
    try:
        rc, so, se = _run(args, None, None, timeout)
        return rc, (so or '') + (se or '')
    except Exception as e:     # noqa: BLE001
        return -1, str(e)


def cmd_providers(req):
    """installed and logged in AI tools, with the commands that install and
    connect them (the host runs those in a visible console window)"""
    out = []
    claude = _which('claude')
    logged = False
    if claude:
        rc, text = _status([claude, 'auth', 'status', '--json'])
        try:
            logged = bool(extract_json(text).get('loggedIn'))
        except ValueError:
            logged = False
    out.append(dict(id='claude', name='Claude Code', installed=bool(claude), logged_in=logged,
                    images=False, models=['', 'sonnet', 'opus', 'haiku'],
                    install='npm install -g @anthropic-ai/claude-code',
                    connect='claude auth login --claudeai'))
    codex = _which('codex')
    logged = False
    if codex:
        rc, text = _status([codex, 'login', 'status'])
        logged = 'logged in' in text.lower() and 'not logged in' not in text.lower()
    out.append(dict(id='codex', name='Codex', installed=bool(codex), logged_in=logged,
                    images=True, models=[''],
                    install='npm install -g @openai/codex', connect='codex login'))
    gemini = _which('gemini')
    creds = os.path.join(os.path.expanduser('~'), '.gemini', 'oauth_creds.json')
    out.append(dict(id='gemini', name='Gemini', installed=bool(gemini),
                    logged_in=bool(gemini) and os.path.exists(creds),
                    images=False, models=['', 'gemini-2.5-pro', 'gemini-2.5-flash'],
                    install='npm install -g @google/gemini-cli', connect='gemini'))
    return dict(providers=out)


# only what a photo request needs: no MCP servers, plugins, browser or computer
# use of the user's setup (faster, far fewer tokens, no errors from servers
# that are not running)
CODEX_LEAN = ['--disable', 'plugins', '--disable', 'apps', '--disable', 'browser_use',
              '--disable', 'computer_use', '--disable', 'in_app_browser']
CLAUDE_LEAN = ['--strict-mcp-config', '--disable-slash-commands', '--setting-sources', 'project',
               '--exclude-dynamic-system-prompt-sections']


def _codex_mcp_off():
    """-c overrides that switch off the MCP servers of the user's Codex config"""
    path = os.path.join(os.environ.get('CODEX_HOME') or os.path.join(os.path.expanduser('~'), '.codex'),
                        'config.toml')
    out = []
    try:
        with open(path, encoding='utf-8') as f:
            for line in f:
                m = re.match(r'^\s*\[mcp_servers\.([A-Za-z0-9_-]+)\]\s*$', line)
                if m:
                    out += ['-c', 'mcp_servers.%s.enabled=false' % m.group(1)]
    except OSError:
        pass
    return out


def ask_model(provider, prompt, images, cwd, model='', timeout=600, images_out=False):
    """one request to a command line AI tool; returns the text answer (and,
    with images_out, the images Codex generated in this session)"""
    if provider == 'claude':
        exe = _which('claude')
        if not exe:
            raise RuntimeError('Claude Code is not installed')
        names = ', '.join(os.path.basename(i) for i in images)
        text = ('Read the image file(s) %s in the current folder.\n\n%s' % (names, prompt)) if images else prompt
        args = [exe, '-p', '--output-format', 'json', '--no-session-persistence',
                '--tools', 'Read', '--allowedTools', 'Read'] + CLAUDE_LEAN
        if model:
            args += ['--model', model]
        rc, so, se = _run(args, cwd, text, timeout)
        try:
            d = json.loads(so)
        except ValueError:
            raise RuntimeError((so + se).strip()[-400:] or 'no answer from Claude')
        if d.get('is_error'):
            msg = d.get('result', '')
            if 'login' in msg.lower():
                msg += ' (click "Connect..." next to the AI choice)'
            raise RuntimeError(msg)
        return (d.get('result', ''), []) if images_out else d.get('result', '')
    if provider == 'codex':
        exe = _which('codex')
        if not exe:
            raise RuntimeError('Codex CLI is not installed')
        args = [exe, 'exec', '--skip-git-repo-check', '-s', 'read-only', '--json'] + CODEX_LEAN + _codex_mcp_off()
        if not images_out:
            args += ['--disable', 'image_generation']
        if model:
            args += ['-m', model]
        args += ['--image=' + i for i in images] + ['-']
        rc, so, se = _run(args, cwd, prompt, timeout)
        thread, texts, errors = None, [], []
        for line in so.splitlines():
            try:
                ev = json.loads(line)
            except ValueError:
                continue
            if ev.get('type') == 'thread.started':
                thread = ev.get('thread_id')
            item = ev.get('item') or {}
            if ev.get('type') == 'item.completed' and item.get('type') == 'agent_message':
                texts.append(item.get('text', ''))
            if ev.get('type') in ('error', 'turn.failed'):
                errors.append(str(ev.get('message') or ev.get('error') or ev)[:300])
        if not texts and (errors or rc != 0):
            msg = '; '.join(errors) or se.strip()[-400:] or 'Codex failed'
            if 'login' in msg.lower() or 'auth' in msg.lower():
                msg += ' (click "Connect..." next to the AI choice)'
            raise RuntimeError(msg)
        text = '\n'.join(texts)
        if not images_out:
            return text
        made = []
        if thread:
            folder = os.path.join(os.path.expanduser('~'), '.codex', 'generated_images', thread)
            if os.path.isdir(folder):
                made = sorted((os.path.join(folder, f) for f in os.listdir(folder)
                               if f.lower().endswith(('.png', '.jpg', '.jpeg', '.webp'))), key=os.path.getmtime)
        return text, made
    if provider == 'gemini':
        exe = _which('gemini')
        if not exe:
            raise RuntimeError('Gemini CLI is not installed (click "Install...")')
        refs = ' '.join('@' + os.path.basename(i) for i in images)
        args = [exe, '-p', '-']
        if model:
            args += ['-m', model]
        rc, so, se = _run(args, cwd, (refs + '\n' + prompt) if refs else prompt, timeout)
        if rc != 0 and not so.strip():
            raise RuntimeError(se.strip()[-400:] or 'Gemini failed')
        return (so, []) if images_out else so
    raise RuntimeError('unknown provider ' + provider)


def extract_json(text):
    """the first JSON object in a model answer (it may add words or code fences)"""
    text = text.strip()
    try:
        return json.loads(text)
    except ValueError:
        pass
    start = text.find('{')
    while start >= 0:
        depth = 0
        for k in range(start, len(text)):
            if text[k] == '{':
                depth += 1
            elif text[k] == '}':
                depth -= 1
                if depth == 0:
                    try:
                        return json.loads(text[start:k + 1])
                    except ValueError:
                        break
        start = text.find('{', start + 1)
    raise ValueError('no JSON in the answer: ' + text[:200])


def make_sheet(paths, out, cols=4, cell=520):
    rows = (len(paths) + cols - 1) // cols
    sheet = np.full((rows * cell, cols * cell, 3), 24, np.uint8)
    for k, p in enumerate(paths):
        img = to8(imread(p, cv2.IMREAD_COLOR))
        img, _ = downscale(img, cell - 14)
        h, w = img.shape[:2]
        x0 = (k % cols) * cell + (cell - w) // 2
        y0 = (k // cols) * cell + (cell - h) // 2
        sheet[y0:y0 + h, x0:x0 + w] = img
        # the number, big and yellow, top left of the cell
        lx, ly = (k % cols) * cell + 8, (k // cols) * cell + 8
        label = str(k + 1)
        tw = 30 * len(label) + 22
        cv2.rectangle(sheet, (lx, ly), (lx + tw, ly + 50), (0, 210, 255), -1)
        cv2.putText(sheet, label, (lx + 10, ly + 40), cv2.FONT_HERSHEY_DUPLEX, 1.4, (0, 0, 0), 3, cv2.LINE_AA)
    imwrite(out, sheet, [cv2.IMWRITE_JPEG_QUALITY, 86])
    return out


RATE_PROMPT = """The image {sheet} is a contact sheet of {n} photos from one shoot, each marked with a yellow number (1 to {n}).
Act as a professional photo editor culling the shoot. For every number, rate the photo from 1 to 10: technical quality (focus on the subject, exposure, motion blur, closed eyes, awkward expressions) and appeal (moment, composition, light).
{bursts}{criteria}Answer with JSON only, no other text:
{{"photos":[{{"n":1,"score":7.5,"keep":true,{match}"reason":"max 12 words"}}]}}"""


def cmd_rate(req):
    images = req['images']
    provider = req.get('provider', 'claude')
    per = int(req.get('per_sheet', 12))
    criteria = (req.get('criteria') or '').strip()
    work = tempfile.mkdtemp(prefix='lsai_rate_')
    bursts = req.get('bursts') or []
    burst_of = {i: b for b, ids in enumerate(bursts) for i in ids if len(ids) > 1}
    sheets = [images[k:k + per] for k in range(0, len(images), per)]

    def one_sheet(s):
        chunk = sheets[s]
        name = 'sheet_%d.jpg' % (s + 1)
        make_sheet([im['path'] for im in chunk], os.path.join(work, name))
        groups = {}
        for k, im in enumerate(chunk):
            if im['id'] in burst_of:
                groups.setdefault(burst_of[im['id']], []).append(k + 1)
        btext = ''.join('Photos %s are a burst of the same moment: give the best of them the highest score.\n'
                        % ', '.join(map(str, g)) for g in groups.values() if len(g) > 1)
        ctext = ('Also say if each photo matches this request: "%s" (field "match": true or false).\n'
                 % criteria) if criteria else ''
        prompt = RATE_PROMPT.format(sheet=name, n=len(chunk), bursts=btext, criteria=ctext,
                                    match='"match":true,' if criteria else '')
        answer = ask_model(provider, prompt, [os.path.join(work, name)], work,
                           req.get('model', ''), int(req.get('timeout', 600)))
        out = []
        for r in extract_json(answer).get('photos', []):
            n = int(r.get('n', 0))
            if 1 <= n <= len(chunk):
                out.append(dict(id=chunk[n - 1]['id'], score=float(r.get('score', 0)),
                                keep=bool(r.get('keep', False)), match=r.get('match'),
                                reason=str(r.get('reason', ''))[:200]))
        return out

    results, errors, done = [], [], 0
    progress(0.02, 'asking %s: %d sheet(s) of up to %d photos' % (provider, len(sheets), per))
    from concurrent.futures import ThreadPoolExecutor, as_completed
    with ThreadPoolExecutor(max_workers=max(1, min(3, len(sheets)))) as pool:
        futures = {pool.submit(one_sheet, k): k for k in range(len(sheets))}
        for f in as_completed(futures):
            done += 1
            try:
                results += f.result()
            except Exception as e:     # noqa: BLE001
                errors.append(str(e)[:300])
                log('sheet %d: %s' % (futures[f] + 1, e))
            progress(done / len(sheets), 'asking %s, %d/%d sheets answered' % (provider, done, len(sheets)))
    shutil.rmtree(work, ignore_errors=True)
    progress(1.0, 'done')
    return dict(images=results, errors=errors, provider=provider, sheets=len(sheets))


# ---------------------------------------------------------------------------
# Best Take: the best face of everyone across a burst

def _match_faces(base_faces, faces, shift):
    """index of the face matching each base face, by position (after the
    global shift between the frames), or -1"""
    out = []
    for bf in base_faces:
        bx = bf['box'][0] + bf['box'][2] / 2.0
        by = bf['box'][1] + bf['box'][3] / 2.0
        best, bd = -1, 1e9
        for k, f in enumerate(faces):
            fx = f['box'][0] + f['box'][2] / 2.0 - shift[0]
            fy = f['box'][1] + f['box'][3] / 2.0 - shift[1]
            d = math.hypot(fx - bx, fy - by)
            if d < bd:
                best, bd = k, d
        out.append(best if bd < 0.6 * bf['box'][2] else -1)
    return out


def _shift(a8, b8):
    """translation of b relative to a (phase correlation on small grayscale)"""
    ga = cv2.cvtColor(a8, cv2.COLOR_BGR2GRAY).astype(np.float32)
    gb = cv2.cvtColor(b8, cv2.COLOR_BGR2GRAY).astype(np.float32)
    (dx, dy), _ = cv2.phaseCorrelate(ga, gb)
    return dx, dy


def _transplant(base, donor, box, shift):
    """blend the face at box (base coordinates, full size) from donor into base"""
    H, W = base.shape[:2]
    x, y, w, h = box
    cx, cy = x + w / 2.0, y + h / 2.0
    rw, rh = w * 1.9, h * 2.2                         # face, hair and chin
    x0, y0 = int(max(0, cx - rw / 2)), int(max(0, cy - rh * 0.55))
    x1, y1 = int(min(W, cx + rw / 2)), int(min(H, cy + rh * 0.45))
    if x1 - x0 < 16 or y1 - y0 < 16:
        return False
    # donor region, larger to allow the alignment
    m = int(max(w, h) * 0.35)
    dx0, dy0 = int(max(0, x0 + shift[0] - m)), int(max(0, y0 + shift[1] - m))
    dx1, dy1 = int(min(donor.shape[1], x1 + shift[0] + m)), int(min(donor.shape[0], y1 + shift[1] + m))
    tgt = base[y0:y1, x0:x1]
    src = donor[dy0:dy1, dx0:dx1]
    # affine alignment of the donor patch onto the base patch (ECC, on a small copy)
    s = min(1.0, 400.0 / max(tgt.shape[:2]))
    tg = cv2.cvtColor(to8(cv2.resize(tgt, None, fx=s, fy=s, interpolation=cv2.INTER_AREA)), cv2.COLOR_BGR2GRAY)
    sg = cv2.cvtColor(to8(cv2.resize(src, None, fx=s, fy=s, interpolation=cv2.INTER_AREA)), cv2.COLOR_BGR2GRAY)
    warp = np.array([[1, 0, (dx0 - x0) * s], [0, 1, (dy0 - y0) * s]], dtype=np.float32)
    warp_inv = cv2.invertAffineTransform(warp)
    try:
        _, warp_inv = cv2.findTransformECC(tg.astype(np.float32), sg.astype(np.float32), warp_inv,
                                           cv2.MOTION_AFFINE,
                                           (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 80, 1e-5),
                                           None, 5)
    except cv2.error:
        pass    # keep the translation from the global shift
    full = warp_inv.copy()
    full[:, 2] /= s
    aligned = cv2.warpAffine(to_float(src), full, (x1 - x0, y1 - y0),
                             flags=cv2.INTER_LINEAR | cv2.WARP_INVERSE_MAP, borderMode=cv2.BORDER_REFLECT)
    # soft elliptic mask, color matched on a ring around the face
    mask = np.zeros((y1 - y0, x1 - x0), np.float32)
    cv2.ellipse(mask, (int(cx - x0), int(cy - y0 - h * 0.08)), (int(w * 0.72), int(h * 0.9)),
                0, 0, 360, 1.0, -1)
    k = int(max(w, h) * 0.18) | 1
    mask = cv2.GaussianBlur(mask, (k, k), 0)
    tf = to_float(tgt)
    ring = (mask > 0.05) & (mask < 0.6)
    if ring.sum() > 50:
        for c in range(3):
            a = aligned[:, :, c][ring].mean()
            b = tf[:, :, c][ring].mean()
            if a > 1e-4:
                aligned[:, :, c] *= b / a
    out = tf * (1 - mask[..., None]) + aligned * mask[..., None]
    base[y0:y1, x0:x1] = from_float(out, base.dtype)
    return True


def cmd_besttake(req):
    frames = req['frames']
    if len(frames) < 2:
        raise RuntimeError('select at least two photos of the same moment')
    progress(0.05, 'reading the photos')
    full = [imread(f['path']) for f in frames]
    small, scales, faces = [], [], []
    for k, img in enumerate(full):
        s8, sc = downscale(to8(img), ANALYSIS_SIDE)
        small.append(s8)
        scales.append(sc)
        fl = []
        for f in FACES.detect(s8):
            f.update(FACES.analyse(s8, f))
            fl.append(f)
        faces.append(fl)
        progress(0.1 + 0.4 * (k + 1) / len(full), 'finding the faces %d/%d' % (k + 1, len(full)))
    if not any(faces):
        raise RuntimeError('no face found in these photos')

    shifts = [_shift(small[0], s) for s in small]           # relative to frame 0
    n_faces = [len(f) for f in faces]

    def frame_score(k):
        return sum(0.5 * f['eyes'] + 0.2 * f['happy'] for f in faces[k]) + 0.001 * n_faces[k]
    base = max(range(len(full)), key=lambda k: (n_faces[k], frame_score(k)))

    result = full[base].copy()
    report = []
    base_shift = shifts[base]
    for fi, bf in enumerate(faces[base]):
        cands = [(base, bf)]
        for k in range(len(full)):
            if k == base:
                continue
            rel = (shifts[k][0] - base_shift[0], shifts[k][1] - base_shift[1])
            idx = _match_faces([bf], faces[k], rel)[0]
            if idx >= 0:
                cands.append((k, faces[k][idx]))
        top = max(f['sharp'] for _, f in cands) or 1.0

        def score(f):
            s = max(0.0, 1.0 + math.log(max(f['sharp'], 1e-6) / top) / 2.5)
            return 0.55 * f['eyes'] + 0.2 * f['happy'] + 0.25 * s
        k_best, f_best = max(cands, key=lambda c: score(c[1]))
        entry = dict(face=fi, frame=frames[k_best]['id'], score=round(score(f_best), 3),
                     base_score=round(score(bf), 3), replaced=False)
        if k_best != base and score(f_best) > score(bf) + 0.05:
            sc = scales[base]
            box = [v / sc for v in bf['box']]
            rel = ((shifts[k_best][0] - base_shift[0]) / sc, (shifts[k_best][1] - base_shift[1]) / sc)
            entry['replaced'] = _transplant(result, full[k_best], box, rel)
        report.append(entry)
        progress(0.55 + 0.4 * (fi + 1) / len(faces[base]), 'face %d/%d' % (fi + 1, len(faces[base])))
    out = req['output']
    imwrite(out, result)
    progress(1.0, 'done')
    return dict(output=out, base=frames[base]['id'], faces=report,
                replaced=sum(1 for r in report if r['replaced']))


# ---------------------------------------------------------------------------
# generative edit of a region (Codex image model)

EDIT_PROMPT = """You get two images of the same photo. In the second one a red outline marks a region.
Use your image generation tool to edit the first image: {request}
Change only what is inside the marked region, keep everything else as it is, keep the same framing and aspect ratio. Do not draw the red outline.
Do not run shell commands and do not write files, just generate the edited image once, then answer DONE."""


def blend_generated(src, gen, rect, kind='rect', feather=0.08):
    """the region of a generated image blended into the full size source:
    colors matched outside the region, the rest of the source untouched"""
    H, W = src.shape[:2]
    pw = min(W, 1536)
    ph = int(round(H * pw / float(W)))
    prev = to_float(cv2.resize(to8(src), (pw, ph), interpolation=cv2.INTER_AREA))
    gp = to_float(cv2.resize(to8(gen), (pw, ph), interpolation=cv2.INTER_CUBIC))
    outside = _region_mask((ph, pw), rect, kind, 0.05) < 0.02
    for c in range(3):
        a = gp[:, :, c][outside]
        b = prev[:, :, c][outside]
        if a.size > 100 and a.std() > 1e-4:
            gp[:, :, c] = (gp[:, :, c] - a.mean()) * (b.std() / a.std()) + b.mean()
    gen_full = cv2.resize(gp, (W, H), interpolation=cv2.INTER_CUBIC)
    mask = _region_mask((H, W), rect, kind, feather)
    out = to_float(src) * (1 - mask[..., None]) + np.clip(gen_full, 0, 1) * mask[..., None]
    return from_float(out, src.dtype)


def _region_mask(shape, rect, kind, feather):
    h, w = shape[:2]
    x0, y0, x1, y1 = rect
    mask = np.zeros((h, w), np.float32)
    p0 = (int(x0 * w), int(y0 * h))
    p1 = (int(x1 * w), int(y1 * h))
    if kind == 'ellipse':
        c = ((p0[0] + p1[0]) // 2, (p0[1] + p1[1]) // 2)
        cv2.ellipse(mask, c, (max(1, (p1[0] - p0[0]) // 2), max(1, (p1[1] - p0[1]) // 2)), 0, 0, 360, 1.0, -1)
    else:
        cv2.rectangle(mask, p0, p1, 1.0, -1)
    k = int(max(3, feather * max(p1[0] - p0[0], p1[1] - p0[1]))) | 1
    return cv2.GaussianBlur(mask, (k, k), 0)


def cmd_genedit(req):
    src = imread(req['image'])
    rect = req['rect']
    kind = req.get('shape', 'rect')
    provider = req.get('provider', 'codex')
    if provider != 'codex':
        raise RuntimeError('only Codex can create images; choose Codex for this edit')
    work = tempfile.mkdtemp(prefix='lsai_edit_')
    prev8, _ = downscale(to8(src), int(req.get('preview_max', 1536)))
    ph, pw = prev8.shape[:2]
    imwrite(os.path.join(work, 'photo.png'), prev8)
    marked = prev8.copy()
    p0 = (int(rect[0] * pw), int(rect[1] * ph))
    p1 = (int(rect[2] * pw), int(rect[3] * ph))
    if kind == 'ellipse':
        cv2.ellipse(marked, ((p0[0] + p1[0]) // 2, (p0[1] + p1[1]) // 2),
                    ((p1[0] - p0[0]) // 2, (p1[1] - p0[1]) // 2), 0, 0, 360, (0, 0, 255), 4)
    else:
        cv2.rectangle(marked, p0, p1, (0, 0, 255), 4)
    imwrite(os.path.join(work, 'marked.png'), marked)
    progress(0.1, 'Codex is editing the region (this can take a few minutes)')
    t0 = time.time()
    answer, made = ask_model('codex', EDIT_PROMPT.format(request=req['prompt'].strip()),
                             [os.path.join(work, 'photo.png'), os.path.join(work, 'marked.png')],
                             work, req.get('model', ''), int(req.get('timeout', 900)), images_out=True)
    made += [os.path.join(work, f) for f in os.listdir(work)
             if f.lower().endswith(('.png', '.jpg', '.webp')) and f not in ('photo.png', 'marked.png')]
    if not made:
        shutil.rmtree(work, ignore_errors=True)
        raise RuntimeError('Codex did not return an image: ' + answer.strip()[-300:])
    res_path = max(made, key=os.path.getmtime)
    progress(0.9, 'blending the region into the photo')
    out = blend_generated(src, imread(res_path, cv2.IMREAD_COLOR), rect, kind, float(req.get('feather', 0.08)))
    imwrite(req['output'], out)
    shutil.rmtree(work, ignore_errors=True)
    progress(1.0, 'done')
    return dict(output=req['output'], seconds=round(time.time() - t0, 1), generated=res_path)


COMMANDS = dict(cull=cmd_cull, rate=cmd_rate, besttake=cmd_besttake, genedit=cmd_genedit,
                providers=cmd_providers)


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in COMMANDS:
        print(__doc__)
        return 2
    with open(sys.argv[2], encoding='utf-8') as f:
        req = json.load(f)
    try:
        res = COMMANDS[sys.argv[1]](req)
        res['ok'] = True
    except Exception as e:     # noqa: BLE001
        res = dict(ok=False, error=str(e))
        log('error: %s' % e)
    with open(sys.argv[3], 'w', encoding='utf-8') as f:
        json.dump(res, f, ensure_ascii=False, indent=1)
    return 0 if res.get('ok') else 1


if __name__ == '__main__':
    sys.exit(main())
