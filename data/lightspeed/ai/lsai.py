"""Tonelark AI helper.

Runs next to Tonelark (darktable) and does the work that needs computer
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
  autoedit  edit with words: the model looks at the photo(s) and answers with
            Lightroom settings, applied by Tonelark as a normal edit
  match     Match Look: exposure and white balance of photos matched to a
            reference photo (on this computer)
  keywords  keywords, title and caption of photos on contact sheets
  crop      crop and straighten suggestions
  faces     the faces of the photos, with an embedding to recognise the people
  mcp       connect Claude Code / Codex / Gemini chats to Tonelark (MCP)
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

# the embeddable Python does not look next to the script
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lsedit  # noqa: E402

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

# ArcFace ResNet100 (ONNX Model Zoo, Apache 2.0): SFace put children who look
# alike, of one age and origin, together; ArcFace keeps them apart
ARCFACE = os.path.join(MODELS, 'arcfaceresnet100-8.onnx')


FACE_SIDE = 2400         # the faces are looked for in the photo at this size


def _iou(a, b):
    ax, ay, aw, ah = a[:4]
    bx, by, bw, bh = b[:4]
    iw = max(0.0, min(ax + aw, bx + bw) - max(ax, bx))
    ih = max(0.0, min(ay + ah, by + bh) - max(ay, by))
    inter = iw * ih
    return inter / (aw * ah + bw * bh - inter + 1e-6)


def _detect_faces(img8):
    """the faces of the photo at two sizes, the photo itself for the small
    faces and a small copy for the big ones (close-ups), the same face kept
    once: YuNet rows (box, 5 landmarks, score) in the pixels of img8"""
    h, w = img8.shape[:2]
    found = []
    for side in (max(h, w), 800):
        im, s = downscale(img8, side)
        ih, iw = im.shape[:2]
        det = cv2.FaceDetectorYN.create(YUNET, '', (iw, ih), 0.6, 0.3, 5000)
        _, faces = det.detect(im)
        if faces is None:
            continue
        for f in faces:
            r = np.array(f, dtype=np.float32)
            r[:14] /= s
            found.append(r)
    found.sort(key=lambda r: -float(r[14]))
    kept = []
    for r in found:
        if all(_iou(r, k) < 0.3 for k in kept):
            kept.append(r)
    return kept


def _even_light(img8):
    """a dark face brighter (a gamma): its embedding is closer to the ones of
    the same person in good light"""
    g = float(cv2.cvtColor(img8, cv2.COLOR_BGR2GRAY).mean())
    if g >= 85:
        return img8
    gamma = math.log(110 / 255.0) / math.log(max(g, 8.0) / 255.0)
    lut = np.array([min(255, int(((i / 255.0) ** gamma) * 255 + 0.5)) for i in range(256)], np.uint8)
    return cv2.LUT(img8, lut)


def cmd_faces(req):
    """the people of the photos: every face big enough to be recognised, with
    its box (0..1 of the photo), an embedding of 512 numbers (ArcFace, the same
    person gives close embeddings: cosine similarity) and a small square
    thumbnail for the People panel. On this computer, nothing is sent."""
    import base64
    if not os.path.exists(ARCFACE):
        raise RuntimeError('the face recognition model is missing: ' + ARCFACE)
    net = cv2.dnn.readNet(ARCFACE)
    images = req['images']
    out, errors = [], []
    for k, im in enumerate(images):
        progress(k / max(1, len(images)), 'looking for faces %d/%d' % (k + 1, len(images)))
        try:
            img8, _ = downscale(to8(imread(im['path'], cv2.IMREAD_COLOR)), FACE_SIDE)
        except Exception as e:     # noqa: BLE001
            errors.append('%s: %s' % (im.get('id'), e))
            continue
        h, w = img8.shape[:2]
        faces = []
        for row in _detect_faces(img8):
            x, y, fw, fh = [float(v) for v in row[:4]]
            score = float(row[14])
            # tiny or unsure faces give embeddings that mix people up
            if fw < 28 or fh < 28 or score < 0.7:
                continue
            # the face turned upright on its eyes, nose and mouth, 112 x 112
            aligned = FACES._aligned(img8, np.array(row[4:14], dtype=np.float32).reshape(5, 2))
            if aligned is None:
                continue
            light = float(cv2.cvtColor(aligned, cv2.COLOR_BGR2GRAY).mean()) / 255.0
            # the network takes RGB 0..255 (its normalisation is inside)
            net.setInput(cv2.dnn.blobFromImage(aligned, 1.0, (112, 112), (0, 0, 0), swapRB=True))
            emb = net.forward().reshape(-1).astype(np.float32)
            n = float(np.linalg.norm(emb))
            if n <= 0:
                continue
            emb /= n
            # the thumbnail: a square around the face with some hair and chin
            side = int(max(fw, fh) * 1.6)
            cx, cy = x + fw / 2.0, y + fh / 2.0
            x0, y0 = int(max(0, cx - side / 2)), int(max(0, cy - side / 2))
            x1, y1 = int(min(w, x0 + side)), int(min(h, y0 + side))
            crop = _even_light(img8[y0:y1, x0:x1])
            thumb = cv2.resize(crop, (96, 96), interpolation=cv2.INTER_AREA)
            ok, jpg = cv2.imencode('.jpg', thumb, [cv2.IMWRITE_JPEG_QUALITY, 85])
            face = dict(box=[x, y, fw, fh])
            faces.append(dict(box=[x / w, y / h, fw / w, fh / h], score=score, light=light,
                              px=float(min(fw, fh)),
                              sharp=Faces.sharpness(img8, face),
                              emb=base64.b64encode(emb.tobytes()).decode('ascii'),
                              thumb=base64.b64encode(jpg.tobytes()).decode('ascii') if ok else ''))
        out.append(dict(id=im['id'], faces=faces))
    progress(1.0, 'done')
    return dict(images=out, errors=errors)


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


def make_sheet(paths, out, cols=4, cell=520, labels=None):
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
        label = labels[k] if labels else str(k + 1)
        tw = 30 * len(label) + 22
        cv2.rectangle(sheet, (lx, ly), (lx + tw, ly + 50), (0, 210, 255), -1)
        cv2.putText(sheet, label, (lx + 10, ly + 40), cv2.FONT_HERSHEY_DUPLEX, 1.4, (0, 0, 0), 3, cv2.LINE_AA)
    imwrite(out, sheet, [cv2.IMWRITE_JPEG_QUALITY, 86])
    return out


# the model tells what it sees in each photo before it rates it: without that
# it gave a photo the details of its neighbour on the sheet ("a raised hand"
# on a girl standing with her hands down). The scale is absolute: asked to
# spread the scores on every sheet, it gave good photos 6 and deleted a third.
RATE_PROMPT = """The image {sheet} is a contact sheet of {n} photos, each marked with a yellow number (1 to {n}) in the top left corner of its cell.
{task}
Look at each photo alone, in its own cell. First write in "see" what is in that photo: who, what they do, where they look, their hands. Then score it. The reason may only talk about what you wrote in "see" for that photo.
Most of them are raw files as the camera took them, before any editing: rate what each photo can become after a normal edit, not how it looks now. Exposure, dark faces, white balance, colors, contrast, flat light, noise and the crop are easy to fix in the editor: they do not lower the score (name them in the reason as what to fix). The score goes down only for what editing cannot fix: the main subject out of focus, motion blur, closed eyes, a poor expression or a missed moment, a cut-off or hidden subject, highlights blown beyond recovery.
The scale, the same for every sheet (a sheet may have only good photos, or only weak ones):
9-10: outstanding, a photo for the album cover or the portfolio.
8: a very good photo: sharp on the subject, a real expression or moment, good composition. Most keepers of a good shoot are 7-8.
7: a good photo with a small flaw.
5-6: usable but ordinary, or with a flaw that editing cannot fix.
3-4: weak.
1-2: unusable (out of focus, eyes closed, an accidental shot).
{bursts}"keep" is true for the photos worth keeping and editing{keep_for}.
"delete" is true only for a photo that no edit can save: the main subject clearly out of focus, motion blur, eyes closed, an accidental shot, the subject cut off; or, in a burst of the same moment, a frame clearly worse than another frame of that burst (eyes closed or blur where the other is fine). A frame about as good as another frame of the moment is not deleted. It is about the photo alone, never about the request: a good photo that does not answer the request is not deleted.
"reason" explains the score in {language}, in one or two short sentences (at most 30 words): what is good, what lowered the score{reason_for}, and what to fix in the edit if anything. The user reads it next to that photo alone: write about that photo, without mentioning the other photos or their numbers.
Answer with JSON only, no other text:
{{"photos":[{{"n":1,"see":"...","score":8,"keep":true,"delete":false,{match}"reason":"..."}}]}}"""

RATE_TASK = ("Act as a professional photo editor culling the shoot. Rate every photo from 1 to 10 for its "
             "potential: the subject in focus, no motion blur, open eyes and a good expression, the moment, "
             "the composition and the light.")

RATE_TASK_FOR = ('The user is looking for: "{criteria}". Act as a professional photo editor choosing the photos '
                 'for this request. Rate every photo from 1 to 10 first by how well it answers the request, then '
                 'by its potential (focus, expression, moment, composition, light). A photo that does not answer '
                 'the request scores 1 to 3, however good it is otherwise; a photo that answers it only in part '
                 'scores at most 6. "match" is true when the photo answers the request.')


def cmd_rate(req):
    images = req['images']
    provider = req.get('provider', 'claude')
    per = int(req.get('per_sheet', 6))
    criteria = (req.get('criteria') or '').strip()
    language = (req.get('language') or 'English').strip()
    work = tempfile.mkdtemp(prefix='lsai_rate_')
    bursts = req.get('bursts') or []
    burst_of = {i: b for b, ids in enumerate(bursts) for i in ids if len(ids) > 1}
    sheets = [images[k:k + per] for k in range(0, len(images), per)]

    def one_sheet(s):
        chunk = sheets[s]
        name = 'sheet_%d.jpg' % (s + 1)
        # 6 photos in 3 columns: each twice the size it had with 12 in 4
        make_sheet([im['path'] for im in chunk], os.path.join(work, name), cols=3 if len(chunk) > 4 else 2,
                   cell=720)
        groups = {}
        for k, im in enumerate(chunk):
            if im['id'] in burst_of:
                groups.setdefault(burst_of[im['id']], []).append(k + 1)
        btext = ''.join('Photos %s are a burst of the same moment: the best of them gets the highest score, '
                        'the others a little lower when they are only a little worse.\n'
                        % ', '.join(map(str, g)) for g in groups.values() if len(g) > 1)
        prompt = RATE_PROMPT.format(
            sheet=name, n=len(chunk), bursts=btext, language=language,
            task=RATE_TASK_FOR.format(criteria=criteria.replace('"', "'")) if criteria else RATE_TASK,
            keep_for=' for this request' if criteria else '',
            reason_for=', and whether it answers the request' if criteria else '',
            match='"match":true,' if criteria else '')
        answer = ask_model(provider, prompt, [os.path.join(work, name)], work,
                           req.get('model', ''), int(req.get('timeout', 600)))
        out = []
        for r in extract_json(answer).get('photos', []):
            n = int(r.get('n', 0))
            if 1 <= n <= len(chunk):
                out.append(dict(id=chunk[n - 1]['id'], score=max(1.0, min(10.0, float(r.get('score', 0)))),
                                keep=bool(r.get('keep', False)), match=r.get('match'),
                                delete=bool(r.get('delete', False)),
                                reason=' '.join(str(r.get('reason', '')).split())[:300]))
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
#
# A face is pasted only from a photo where that head is where it is in the base
# photo, the same size, turned the same way: the head of a person who moved
# pasted onto the base made a double image (the hair twice, a neighbour's head
# in the patch). The choice among those faces is the AI's when one is
# connected (eyes open, looking at the camera, the expression), else the
# measures here. The patch is the inner face, aligned on its landmarks, and
# it is left out when its edge does not match the base.

BESTTAKE_PROMPT = """The image {sheet} shows the faces of the people of a group photo taken several times in a burst: a row per person (the letter on the left), a column per photo (the number on top). An empty cell: that face cannot be used from that photo. The column framed in green is the photo the others are pasted into.
For every person choose the photo where the face is the best for the group photo: eyes open, looking at the camera, a natural smile or a good expression, sharp, not covered by hair, a hand or another person. Keep the face of the green photo when no other face is clearly better.
Answer with JSON only, no other text:
{{"faces":{{"A":1,"B":3}}}}"""


BT_EDGE = 0.34       # the most the edge of a pasted face may differ from the base


def _geometry(f):
    """yaw (nose off the middle of the eyes, in eye distances), roll (degrees)
    and the eye distance of a face"""
    lm = f['lm']
    iod = float(np.linalg.norm(lm[1] - lm[0])) or 1.0
    mid = (lm[0] + lm[1]) / 2.0
    yaw = float(lm[2][0] - mid[0]) / iod
    roll = math.degrees(math.atan2(float(lm[1][1] - lm[0][1]), float(lm[1][0] - lm[0][0])))
    return yaw, roll, iod


def _centre(f):
    return f['box'][0] + f['box'][2] / 2.0, f['box'][1] + f['box'][3] / 2.0


def _shift(a8, b8):
    """translation of b relative to a (phase correlation on small grayscale)"""
    ga = cv2.cvtColor(a8, cv2.COLOR_BGR2GRAY).astype(np.float32)
    gb = cv2.cvtColor(b8, cv2.COLOR_BGR2GRAY).astype(np.float32)
    (dx, dy), _ = cv2.phaseCorrelate(ga, gb)
    return dx, dy


def _same_person(bf, faces, shift):
    """index of the face of the same person as the base face bf (the closest
    one, after the global shift between the frames), or -1"""
    bx, by = _centre(bf)
    best, bd = -1, 1e9
    for k, f in enumerate(faces):
        fx, fy = _centre(f)
        d = math.hypot(fx - shift[0] - bx, fy - shift[1] - by)
        if d < bd:
            best, bd = k, d
    return best if bd < 0.8 * bf['box'][2] else -1


def _swappable(bf, f, shift):
    """None when the face f can be pasted over the base face bf, else why not"""
    bx, by = _centre(bf)
    fx, fy = _centre(f)
    # the patch is aligned on the landmarks: a head a little aside is fine,
    # the check of the edge of the patch says whether the rest matches
    if math.hypot(fx - shift[0] - bx, fy - shift[1] - by) > 0.6 * bf['box'][2]:
        return 'moved'
    ratio = f['box'][2] / max(bf['box'][2], 1.0)
    if not 0.8 < ratio < 1.25:
        return 'moved'
    byaw, broll, _ = _geometry(bf)
    fyaw, froll, _ = _geometry(f)
    if abs(byaw - fyaw) > 0.2 or abs(broll - froll) > 10:
        return 'turned'
    return None


def _local_score(f, top_sharp):
    yaw, _, _ = _geometry(f)
    frontal = max(0.0, 1.0 - abs(yaw) * 2.5)
    sharp = max(0.0, 1.0 + math.log(max(f['sharp'], 1e-6) / max(top_sharp, 1e-6)) / 2.5)
    return 0.5 * f['eyes'] + 0.3 * frontal + 0.2 * sharp


def _face_sheet(rows, small, base, out, cell=220):
    """rows: per person, per frame the face or None; the base column framed"""
    n = len(small)
    head, side = 44, 50
    sheet = np.full((head + len(rows) * cell, side + n * cell, 3), 24, np.uint8)
    for c in range(n):
        x = side + c * cell
        cv2.putText(sheet, str(c + 1), (x + cell // 2 - 12, 34), cv2.FONT_HERSHEY_DUPLEX, 1.1,
                    (0, 210, 255), 2, cv2.LINE_AA)
    for r, row in enumerate(rows):
        y = head + r * cell
        cv2.putText(sheet, chr(65 + r), (10, y + cell // 2 + 14), cv2.FONT_HERSHEY_DUPLEX, 1.2,
                    (0, 210, 255), 2, cv2.LINE_AA)
        for c, f in enumerate(row):
            if f is None:
                continue
            x0, y0, w, h = f['box']
            m = 0.35 * max(w, h)
            H, W = small[c].shape[:2]
            a, b = int(max(0, x0 - m)), int(max(0, y0 - m))
            crop = small[c][b:int(min(H, y0 + h + m)), a:int(min(W, x0 + w + m))]
            if crop.size == 0:
                continue
            crop = _even_light(crop)          # the eyes of a dark face seen
            ch, cw = crop.shape[:2]
            s = (cell - 12) / float(max(ch, cw))
            crop = cv2.resize(crop, (max(1, int(cw * s)), max(1, int(ch * s))), interpolation=cv2.INTER_AREA)
            ch, cw = crop.shape[:2]
            x = side + c * cell + (cell - cw) // 2
            yy = y + (cell - ch) // 2
            sheet[yy:yy + ch, x:x + cw] = crop
    x = side + base * cell
    cv2.rectangle(sheet, (x + 2, head + 2), (x + cell - 3, sheet.shape[0] - 3), (60, 200, 60), 4)
    imwrite(out, sheet, [cv2.IMWRITE_JPEG_QUALITY, 88])
    return out


def _transplant(base, donor, box, blm, dlm):
    """paste the inner face of donor over the face at box of base (full size
    coordinates, blm and dlm the landmarks of the face in each): '' when done,
    else why not"""
    H, W = base.shape[:2]
    x, y, w, h = box
    cx, cy = x + w / 2.0, y + h / 2.0
    x0, y0 = int(max(0, cx - 0.85 * w)), int(max(0, cy - 0.9 * h))
    x1, y1 = int(min(W, cx + 0.85 * w)), int(min(H, cy + 0.9 * h))
    if x1 - x0 < 16 or y1 - y0 < 16:
        return 'small'
    M, _ = cv2.estimateAffinePartial2D(dlm.astype(np.float32), blm.astype(np.float32), method=cv2.LMEDS)
    if M is None:
        return 'align'
    M = M.astype(np.float32)
    M[0, 2] -= x0
    M[1, 2] -= y0
    tgt = to_float(base[y0:y1, x0:x1])
    aligned = cv2.warpAffine(to_float(donor), M, (x1 - x0, y1 - y0), flags=cv2.INTER_LINEAR,
                             borderMode=cv2.BORDER_REFLECT)
    # the landmarks are found on a small copy: refine on the pixels
    s = min(1.0, 320.0 / max(tgt.shape[:2]))
    tg = cv2.cvtColor(to8(cv2.resize(tgt, None, fx=s, fy=s, interpolation=cv2.INTER_AREA)), cv2.COLOR_BGR2GRAY)
    ag = cv2.cvtColor(to8(cv2.resize(aligned, None, fx=s, fy=s, interpolation=cv2.INTER_AREA)), cv2.COLOR_BGR2GRAY)
    warp = np.eye(2, 3, dtype=np.float32)
    try:
        _, warp = cv2.findTransformECC(tg.astype(np.float32), ag.astype(np.float32), warp, cv2.MOTION_EUCLIDEAN,
                                       (cv2.TERM_CRITERIA_EPS | cv2.TERM_CRITERIA_COUNT, 60, 1e-5), None, 5)
        if abs(warp[0, 2]) < 0.1 * w * s and abs(warp[1, 2]) < 0.1 * h * s:
            warp[:, 2] /= s
            aligned = cv2.warpAffine(aligned, warp, (x1 - x0, y1 - y0),
                                     flags=cv2.INTER_LINEAR | cv2.WARP_INVERSE_MAP, borderMode=cv2.BORDER_REFLECT)
    except cv2.error:
        pass
    # the inner face: brows to chin, cheek to cheek, not the hair
    pts = blm - np.array([x0, y0], np.float32)
    ex = (pts[0] + pts[1]) / 2.0
    mouth = (pts[3] + pts[4]) / 2.0
    iod = float(np.linalg.norm(pts[1] - pts[0])) or 1.0
    centre = (ex + mouth) / 2.0
    angle = math.degrees(math.atan2(float(pts[1][1] - pts[0][1]), float(pts[1][0] - pts[0][0])))
    axes = (int(iod * 1.05), int(max(float(np.linalg.norm(mouth - ex)), iod * 0.6) * 1.35))
    mask = np.zeros((y1 - y0, x1 - x0), np.float32)
    cv2.ellipse(mask, (int(centre[0]), int(centre[1])), axes, angle, 0, 360, 1.0, -1)
    k = int(iod * 0.35) | 1
    mask = cv2.GaussianBlur(mask, (k, k), 0)
    # the colors of the base on a ring around the face, and the ring must match
    ring = (mask > 0.03) & (mask < 0.5)
    if ring.sum() < 50:
        return 'small'
    for c in range(3):
        a = aligned[:, :, c][ring].mean()
        b = tgt[:, :, c][ring].mean()
        if a > 1e-4:
            aligned[:, :, c] *= b / a
    # on a small copy: the noise of a full size photo does not count
    s2 = min(1.0, 200.0 / max(tgt.shape[:2]))
    small = lambda im: cv2.resize(im, None, fx=s2, fy=s2, interpolation=cv2.INTER_AREA)  # noqa: E731
    lum = lambda im: im[:, :, 0] * 0.114 + im[:, :, 1] * 0.587 + im[:, :, 2] * 0.299  # noqa: E731
    lt, la, ms = lum(small(tgt)), lum(small(aligned)), small(mask)
    ring_s = (ms > 0.03) & (ms < 0.5)
    diff = float(np.abs(lt - la)[ring_s].mean() / max(float(lt[ring_s].mean()), 0.02)) if ring_s.any() else 1.0
    log('best take: edge difference %.3f' % diff)
    if diff > BT_EDGE:
        return 'edge'
    out = tgt * (1 - mask[..., None]) + aligned * mask[..., None]
    base[y0:y1, x0:x1] = from_float(out, base.dtype)
    return ''


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
        progress(0.05 + 0.35 * (k + 1) / len(full), 'finding the faces %d/%d' % (k + 1, len(full)))
    if not any(faces):
        raise RuntimeError('no face found in these photos')

    shifts = [_shift(small[0], s) for s in small]           # relative to frame 0
    top = max((f['sharp'] for fl in faces for f in fl), default=1.0)
    # the base: the most faces, then the best ones
    base = max(range(len(full)), key=lambda k: (len(faces[k]), sum(_local_score(f, top) for f in faces[k])))
    bshift = shifts[base]

    # every face of the base, and its faces in the other photos that can be
    # pasted; a better face that cannot is said in the report
    rows, lost = [], []
    for bf in faces[base]:
        row, better = [], ''
        for k in range(len(full)):
            if k == base:
                row.append(bf)
                continue
            rel = (shifts[k][0] - bshift[0], shifts[k][1] - bshift[1])
            idx = _same_person(bf, faces[k], rel)
            no = _swappable(bf, faces[k][idx], rel) if idx >= 0 else 'missing'
            row.append(faces[k][idx] if not no else None)
            if no and no != 'missing' and _local_score(faces[k][idx], top) > _local_score(bf, top) + 0.05:
                better = no
        rows.append(row)
        lost.append(better)

    # the choice: the AI, else the measures
    choice, chooser = {}, 'local'
    provider = req.get('provider') or ''
    if provider and any(sum(f is not None for f in row) > 1 for row in rows):
        work = tempfile.mkdtemp(prefix='lsai_besttake_')
        try:
            progress(0.45, 'asking %s for the best faces' % provider)
            sheet = _face_sheet(rows, small, base, os.path.join(work, 'faces.jpg'))
            answer = ask_model(provider, BESTTAKE_PROMPT.format(sheet='faces.jpg'), [sheet], work,
                               req.get('model', ''), int(req.get('timeout', 300)))
            for key, v in (extract_json(answer).get('faces') or {}).items():
                r, c = ord(str(key)[:1].upper()) - 65, int(v) - 1
                if 0 <= r < len(rows) and 0 <= c < len(full) and rows[r][c] is not None:
                    choice[r] = c
            chooser = provider
        except Exception as e:     # noqa: BLE001
            log('best take: the AI did not choose (%s), the measures do' % e)
        finally:
            shutil.rmtree(work, ignore_errors=True)
    for r, row in enumerate(rows):
        if r in choice:
            continue
        cands = [c for c, f in enumerate(row) if f is not None]
        c = max(cands, key=lambda c: _local_score(row[c], top))
        choice[r] = c if _local_score(row[c], top) > _local_score(row[base], top) + 0.05 else base

    result = full[base].copy()
    report = []
    sc = scales[base]
    for r, row in enumerate(rows):
        c = choice[r]
        entry = dict(face=r, frame=frames[c]['id'], replaced=False, kept='')
        if c != base:
            box = [v / sc for v in row[base]['box']]
            blm = row[base]['lm'] / sc
            dlm = row[c]['lm'] / scales[c]
            no = _transplant(result, full[c], box, blm, dlm)
            entry['replaced'] = not no
            entry['kept'] = no
        elif lost[r]:
            entry['kept'] = lost[r]
        report.append(entry)
        progress(0.6 + 0.35 * (r + 1) / len(rows), 'face %d/%d' % (r + 1, len(rows)))
    out = req['output']
    imwrite(out, result)
    progress(1.0, 'done')
    return dict(output=out, base=frames[base]['id'], faces=report, chooser=chooser,
                replaced=sum(1 for e in report if e['replaced']))


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



# ---------------------------------------------------------------------------
# edit with words

AUTOEDIT_ONE = """You are a professional photo retoucher working with Lightroom sliders. The image {name} is a photo as it looks now.
Its current settings: {current}
The photographer asks: "{request}"
Choose Lightroom settings that do this and make it a finished, striking photo, as in the portfolio of a top photographer.
A RAW file starts flat, so a finished edit almost always needs real contrast and depth: rich blacks, clean bright whites, clear midtones, and a subject that stands out from its background (Contrast, Whites, Blacks, Clarity, Dehaze, Vibrance, Vignette).
Match the strength to the request: bold, dramatic, cinematic, moody or vivid asks for bold values; natural, soft or airy stays gentle.
Keep the subject well exposed and readable, keep skin tones natural and the white balance believable unless asked otherwise, and do not clip large bright areas.
Color: a warm or cool mood comes from the sky and the light, not from a cast over everything. Keep things that are neutral in reality (grey animals, stone, white clothes, snow) close to neutral, give color contrast (warm highlights against cooler shadows) rather than one color over the whole photo, and keep ColorGrade Midtones and Global low.
Settings you can use (values are absolute, not added to the current ones; only give the ones that change):
{vocabulary}
Answer with JSON only, no other text:
{{"settings": {{"Exposure": 0.2, "Temperature": 5800}}, "summary": "what you did, max 15 words"}}"""

AUTOEDIT_REFINE = """You edited a photo for the request "{request}". The image {name} shows the result.
The settings now: {current}
Look at the result critically, as a professional retoucher. Is it a finished, striking photo? Check contrast and depth first (flat grey blacks and dull midtones are the most common problem), whether the subject stands out, then exposure (a subject too dark to read), white balance and color casts (one color over the whole photo, too yellow, too red or too blue, neutral things that are not neutral), skin tones, clipped highlights, too much or too little of what was asked.
If it can be better, give the settings that change (absolute values, only the ones that change). If it is good, give an empty "settings".
Settings you can use:
{vocabulary}
Answer with JSON only, no other text:
{{"settings": {{}}, "summary": "what you changed or why it is good, max 15 words"}}"""

AUTOEDIT_SHEET = """The image {sheet} is a contact sheet of {n} photos, each marked with a yellow number (1 to {n}).
{reference}The photographer asks for these photos: "{request}"
Give every photo its own Lightroom settings to do this: the photos differ in light, so adapt exposure and white balance to each one, and keep the look consistent across the set. Make every photo look finished, with real contrast and depth (RAW files start flat). Keep skin tones natural and do not clip highlights.
Current settings of each photo:
{current}
Settings you can use (values are absolute, not added to the current ones; only give the ones that change):
{vocabulary}
Answer with JSON only, no other text:
{{"photos":[{{"n":1,"settings":{{"Exposure":0.3}},"summary":"max 10 words"}}]}}"""

MATCH_REQUEST = ("make each numbered photo match the look of the reference photo R (brightness, contrast, "
                 "color, white balance, mood). They already have its settings: give the corrections that "
                 "still make them look like one set")


def _settings_of(answer):
    d = extract_json(answer)
    return d.get('settings') or {}, str(d.get('summary', ''))[:200]


def cmd_autoedit(req):
    images = req['images']
    provider = req.get('provider', 'claude')
    model = req.get('model', '')
    timeout = int(req.get('timeout', 600))
    request = (req.get('instruction') or '').strip() or \
        'a finished professional edit with good contrast and depth that makes this photo look its best'
    work = tempfile.mkdtemp(prefix='lsai_edit_')
    results, errors = [], []

    if len(images) == 1 and not req.get('reference'):
        im = images[0]
        current = im.get('settings') or {}
        name = 'photo.jpg'
        shutil.copyfile(im['path'], os.path.join(work, name))
        prompt = (AUTOEDIT_REFINE if req.get('round', 1) > 1 else AUTOEDIT_ONE).format(
            name=name, current=json.dumps(lsedit.friendly(current)), request=request,
            vocabulary=lsedit.VOCABULARY)
        progress(0.1, 'asking %s' % provider)
        answer = ask_model(provider, prompt, [os.path.join(work, name)], work, model, timeout)
        settings, summary = _settings_of(answer)
        results.append(dict(id=im['id'], edit=lsedit.to_crs(settings, current), summary=summary))
    else:
        per = int(req.get('per_sheet', 12))
        ref = req.get('reference')
        # the reference takes the first cell of every sheet
        if ref:
            per = max(1, per - 1)
        sheets = [images[k:k + per] for k in range(0, len(images), per)]

        def one_sheet(k):
            chunk = sheets[k]
            name = 'sheet_%d.jpg' % (k + 1)
            paths = [im['path'] for im in chunk]
            labels = [str(i + 1) for i in range(len(chunk))]
            if ref:
                paths = [ref['path']] + paths
                labels = ['R'] + labels
            make_sheet(paths, os.path.join(work, name), labels=labels)
            current = '\n'.join('%d: %s' % (i + 1, lsedit.compact(lsedit.friendly(im.get('settings') or {})))
                                for i, im in enumerate(chunk))
            rtext = ('Photo R (first) is the reference photo, already edited the way the photographer wants; '
                     'do not give settings for it.\n') if ref else ''
            prompt = AUTOEDIT_SHEET.format(sheet=name, n=len(chunk), request=request, current=current,
                                           reference=rtext, vocabulary=lsedit.VOCABULARY)
            answer = ask_model(provider, prompt, [os.path.join(work, name)], work, model, timeout)
            out = []
            for r in extract_json(answer).get('photos', []):
                try:
                    n = int(r.get('n', 0))
                except (TypeError, ValueError):
                    continue
                if 1 <= n <= len(chunk):
                    im = chunk[n - 1]
                    out.append(dict(id=im['id'], edit=lsedit.to_crs(r.get('settings') or {}, im.get('settings')),
                                    summary=str(r.get('summary', ''))[:200]))
            return out

        progress(0.02, 'asking %s: %d sheet(s)' % (provider, len(sheets)))
        from concurrent.futures import ThreadPoolExecutor, as_completed
        done = 0
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
    if not results and errors:
        raise RuntimeError(errors[0])
    progress(1.0, 'done')
    return dict(images=results, errors=errors, provider=provider)


# ---------------------------------------------------------------------------
# Match Look (on this computer): the photos already have the settings of the
# reference; their exposure and white balance are adapted to their own
# light, as Lightroom's "Match Total Exposures" and a relative white balance

def look(path):
    """brightness of a rendered photo: log2 of the median linear luminance"""
    img = to_float(imread(path, cv2.IMREAD_COLOR))
    img, _ = downscale(img, 800)
    lin = np.where(img <= 0.04045, img / 12.92, ((img + 0.055) / 1.055) ** 2.4)
    y = 0.0722 * lin[:, :, 0] + 0.7152 * lin[:, :, 1] + 0.2126 * lin[:, :, 2]
    ok = (y > 0.002) & (y < 0.97)
    return float(np.median(np.log2(y[ok]))) if ok.sum() > 100 else float(np.log2(max(1e-4, y.mean())))


def captured(exif):
    """log2 of the light the camera captured (shutter x ISO / f-number^2), None if unknown"""
    try:
        t, n, iso = float(exif['exposure']), float(exif['aperture']), float(exif['iso'])
    except (KeyError, TypeError, ValueError):
        return None
    if t <= 0 or n <= 0 or iso <= 0:
        return None
    return math.log2(t * iso / (n * n))


def _mired(t):
    return 1e6 / max(1000.0, float(t))


def _as_shot(settings, t, tint):
    """white balance of the camera (5003 K, 0 for photos that are not raw)"""
    if settings.get('raw') is False:
        return 5003.0, 0.0
    return float(settings.get('AsShotTemperature') or t), float(settings.get('AsShotTint', tint))


EV_RESPONSE = 1.2      # the median brightness of a rendering moves 1.2 EV per EV of exposure


def cmd_match(req):
    ref = req['reference']
    rs = ref.get('settings') or {}
    ref_exposure = float(rs.get('Exposure2012', 0.0))
    ref_t = float(rs.get('Temperature', 5000.0))
    ref_tint = float(rs.get('Tint', 0.0))
    # what the reference got on top of its camera white balance (a photo that
    # is not raw is already balanced: 5003 K, tint 0 leave it as it is)
    shot_t, shot_tint = _as_shot(rs, ref_t, ref_tint)
    d_mired = _mired(ref_t) - _mired(shot_t)
    d_tint = ref_tint - shot_tint
    ref_light = captured(ref.get('exif') or {})
    ref_look = None
    out = []
    images = req['images']
    for k, im in enumerate(images):
        progress(k / max(1, len(images)), 'matching %d/%d' % (k + 1, len(images)))
        before = im.get('before') or {}
        edit, how = {}, ''
        light = captured(im.get('exif') or {})
        if ref_light is not None and light is not None and abs(ref_light - light) <= 2.5:
            ev = ref_exposure + (ref_light - light)
            how = 'camera settings'
        else:
            # no camera settings, or photos in very different light:
            # the median brightness of the renderings
            try:
                if ref_look is None:
                    ref_look = look(ref['path'])
                ev = ref_exposure + lsedit.clamp((ref_look - look(im['path'])) / EV_RESPONSE, -1.5, 1.5)
                how = 'brightness'
            except Exception as e:     # noqa: BLE001
                log('skip %s: %s' % (im.get('path'), e))
                ev = ref_exposure
        edit['Exposure2012'] = round(lsedit.clamp(ev, -5.0, 5.0), 2)
        if abs(d_mired) > 0.5 or abs(d_tint) > 0.5:
            base_t, base_tint = _as_shot(before, before.get('Temperature') or ref_t, before.get('Tint', 0.0))
            t = lsedit.clamp(1e6 / max(40.0, _mired(base_t) + d_mired), 2000, 25000)
            edit.update(lsedit.to_crs(dict(Temperature=t, Tint=float(base_tint) + d_tint), {}))
        else:
            # the reference keeps the white balance of its camera: every photo too
            edit['WhiteBalance'] = 'As Shot'
        out.append(dict(id=im['id'], edit=edit, how=how))
    progress(1.0, 'done')
    return dict(images=out)


# ---------------------------------------------------------------------------
# keywords, title and caption

KEYWORDS_PROMPT = """The image {sheet} is a contact sheet of {n} photos, each marked with a yellow number (1 to {n}).
For every photo write, in {language}:
- "keywords": 5 to 12 keywords for a photo library: the subject, people (count, not names), the kind of place, the activity, the mood, main colors, season or time of day, the style of the photo. Lower case, no hashtags, no duplicates.
{titles}Only describe what can be seen; do not guess names of people or exact places.
Answer with JSON only, no other text:
{{"photos":[{{"n":1,"keywords":["beach","sunset"]{example}}}]}}"""


def cmd_keywords(req):
    images = req['images']
    provider = req.get('provider', 'claude')
    per = int(req.get('per_sheet', 12))
    language = req.get('language') or 'English'
    titles = bool(req.get('titles', True))
    work = tempfile.mkdtemp(prefix='lsai_kw_')
    sheets = [images[k:k + per] for k in range(0, len(images), per)]

    def one_sheet(k):
        chunk = sheets[k]
        name = 'sheet_%d.jpg' % (k + 1)
        make_sheet([im['path'] for im in chunk], os.path.join(work, name))
        ttext = ('- "title": a short title, max 6 words\n'
                 '- "caption": one sentence that describes the photo, max 25 words\n') if titles else ''
        prompt = KEYWORDS_PROMPT.format(sheet=name, n=len(chunk), language=language, titles=ttext,
                                        example=',"title":"...","caption":"..."' if titles else '')
        answer = ask_model(provider, prompt, [os.path.join(work, name)], work,
                           req.get('model', ''), int(req.get('timeout', 600)))
        out = []
        for r in extract_json(answer).get('photos', []):
            try:
                n = int(r.get('n', 0))
            except (TypeError, ValueError):
                continue
            if 1 <= n <= len(chunk):
                kws, seen = [], set()
                for kw in r.get('keywords') or []:
                    kw = str(kw).strip().strip('#').replace('|', ' ')[:60]
                    if kw and kw.lower() not in seen:
                        seen.add(kw.lower())
                        kws.append(kw)
                item = dict(id=chunk[n - 1]['id'], keywords=kws[:15])
                if titles:
                    item['title'] = str(r.get('title', '')).strip()[:120]
                    item['caption'] = str(r.get('caption', '')).strip()[:400]
                out.append(item)
        return out

    results, errors, done = [], [], 0
    progress(0.02, 'asking %s: %d sheet(s)' % (provider, len(sheets)))
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
    if not results and errors:
        raise RuntimeError(errors[0])
    progress(1.0, 'done')
    return dict(images=results, errors=errors, provider=provider)


# ---------------------------------------------------------------------------
# crop suggestions

CROP_PROMPT = """You are a professional photo editor. {what}
Suggest the best crop for {each}: a stronger composition (rule of thirds or a clear center, balance, no distractions at the edges, room in front of a face or a moving subject), keeping what matters (do not cut heads, hands or feet awkwardly, keep the whole subject when it is small).
{aspect}{straighten}Give the crop as percentages of the width and height of the photo as it is shown: left, top, right, bottom (0 to 100, from the top left corner). A photo that is best as it is gets 0, 0, 100, 100.
Answer with JSON only, no other text:
{{"photos":[{{"n":1,"left":8,"top":0,"right":92,"bottom":96,{angle}"reason":"max 10 words"}}]}}"""


def cmd_crop(req):
    images = req['images']
    provider = req.get('provider', 'claude')
    aspect = req.get('aspect') or 'original'
    straighten = bool(req.get('straighten', True))
    work = tempfile.mkdtemp(prefix='lsai_crop_')
    if aspect == 'original':
        atext = 'Keep the aspect ratio of the photo.\n'
    elif aspect in lsedit.ASPECTS:
        atext = 'The crop must have the aspect ratio %s (width:height).\n' % aspect
    else:
        atext = 'Any aspect ratio is fine.\n'
    stext = ('If the horizon or lines that should be vertical are tilted, give "angle": the rotation in degrees '
             'that levels them (positive = rotate clockwise, usually less than 5), else 0.\n') if straighten else ''
    single = len(images) == 1
    per = 1 if single else int(req.get('per_sheet', 9))
    sheets = [images[k:k + per] for k in range(0, len(images), per)]

    def one_sheet(k):
        chunk = sheets[k]
        if single:
            name = 'photo.jpg'
            shutil.copyfile(chunk[0]['path'], os.path.join(work, name))
            what = 'The image %s is a photo (call it photo 1).' % name
            each = 'it'
        else:
            name = 'sheet_%d.jpg' % (k + 1)
            make_sheet([im['path'] for im in chunk], os.path.join(work, name), cols=3, cell=620)
            what = ('The image %s is a contact sheet of %d photos, each marked with a yellow number (1 to %d).'
                    % (name, len(chunk), len(chunk)))
            each = 'every photo (the percentages are of that photo, not of the sheet)'
        prompt = CROP_PROMPT.format(what=what, each=each, aspect=atext, straighten=stext,
                                    angle='"angle":0,' if straighten else '')
        answer = ask_model(provider, prompt, [os.path.join(work, name)], work,
                           req.get('model', ''), int(req.get('timeout', 600)))
        out = []
        for r in extract_json(answer).get('photos', []):
            try:
                n = int(r.get('n', 0))
                rect = [float(r.get(key, d)) / 100.0 for key, d in
                        (('left', 0), ('top', 0), ('right', 100), ('bottom', 100))]
            except (TypeError, ValueError):
                continue
            if 1 <= n <= len(chunk):
                im = chunk[n - 1]
                angle = None
                if straighten:
                    angle = lsedit.clamp(lsedit._num(r.get('angle')), -10.0, 10.0)
                    if abs(angle) < 0.2:
                        angle = 0.0
                edit = lsedit.crop_edit(im['width'], im['height'], rect, aspect, im.get('settings'), angle)
                out.append(dict(id=im['id'], edit=edit, rect=rect, angle=angle,
                                reason=str(r.get('reason', ''))[:200]))
        return out

    results, errors, done = [], [], 0
    progress(0.02, 'asking %s' % provider)
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
            progress(done / len(sheets), 'asking %s, %d/%d answered' % (provider, done, len(sheets)))
    shutil.rmtree(work, ignore_errors=True)
    if not results and errors:
        raise RuntimeError(errors[0])
    progress(1.0, 'done')
    return dict(images=results, errors=errors, provider=provider)


# ---------------------------------------------------------------------------
# chats: register the Tonelark MCP server in the AI tools

def _codex_auto_approve():
    """the Tonelark tools work on the user's own photos, with undo: Codex
    runs them without asking every time"""
    path = os.path.join(os.environ.get('CODEX_HOME') or os.path.join(os.path.expanduser('~'), '.codex'),
                        'config.toml')
    try:
        with open(path, encoding='utf-8') as f:
            lines = f.read().split('\n')
    except OSError:
        return
    out, section = [], False
    for line in lines:
        if line.strip().startswith('['):
            section = line.strip() == '[mcp_servers.tonelark]'
            out.append(line)
            if section:
                out.append('default_tools_approval_mode = "approve"')
            continue
        if section and line.strip().startswith('default_tools_approval_mode'):
            continue
        out.append(line)
    with open(path, 'w', encoding='utf-8', newline='') as f:
        f.write('\n'.join(out))


def cmd_mcp(req):
    python = sys.executable
    server = os.path.join(HERE, 'lsmcp.py')
    out = {}
    for tool in ('claude', 'codex', 'gemini'):
        exe = _which(tool)
        if not exe:
            out[tool] = 'not installed'
            continue
        progress(0.3, 'connecting %s' % tool)
        # (the first versions were called Lightspeed: that entry goes too)
        if tool == 'claude':
            removes = [[exe, 'mcp', 'remove', '-s', 'user', n] for n in ('lightspeed', 'tonelark')]
            add = [exe, 'mcp', 'add', '-s', 'user', 'tonelark', '--', python, server]
        elif tool == 'codex':
            removes = [[exe, 'mcp', 'remove', n] for n in ('lightspeed', 'tonelark')]
            add = [exe, 'mcp', 'add', 'tonelark', '--', python, server]
        else:
            removes = [[exe, 'mcp', 'remove', '-s', 'user', n] for n in ('lightspeed', 'tonelark')]
            add = [exe, 'mcp', 'add', '-s', 'user', 'tonelark', python, server]
        for remove in removes:
            _status(remove, 60)
        rc, text = _status(add, 60)
        if rc == 0 and tool == 'codex':
            _codex_auto_approve()
        out[tool] = 'connected' if rc == 0 else ('error: ' + text.strip()[-300:])
    progress(1.0, 'done')
    return dict(tools=out, python=python, server=server)


COMMANDS = dict(cull=cmd_cull, rate=cmd_rate, besttake=cmd_besttake, genedit=cmd_genedit,
                providers=cmd_providers, autoedit=cmd_autoedit, match=cmd_match, keywords=cmd_keywords,
                crop=cmd_crop, mcp=cmd_mcp, faces=cmd_faces)


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
