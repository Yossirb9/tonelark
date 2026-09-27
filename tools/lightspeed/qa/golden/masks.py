"""Mask tests: add masked module instances (drawn and parametric masks) to a
darktable XMP and render them with Lightspeed's darktable-cli.

    python masks.py
"""
import base64
import os
import re
import struct
import subprocess
import zlib

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = r'C:\lightspeed\install\bin\darktable-cli.exe'
CFG = 'C:/lightspeed/qa/cli-golden'
OUT = os.path.join(HERE, 'masks')

# darktable enums
ELLIPSE, GRADIENT, BRUSH, GROUP = 1 << 5, 1 << 4, 1 << 6, 1 << 2
USE, SHOW, INVERSE, UNION, INTERSECTION = 1, 2, 4, 8, 16
MASK_ENABLED, MASK_DRAWN, MASK_PARAMETRIC = 1, 2, 4
CS_RGB_SCENE, BLEND_NORMAL = 4, 0x18
GRAY_IN, HZ_IN = 0, 10
BLENDIF_ACTIVE = 1 << 31


def decode(s):
    if s.startswith('gz'):
        return zlib.decompress(base64.b64decode(s[4:]))
    return bytes.fromhex(s)


def f32(*v):
    return struct.pack('<%df' % len(v), *v)


def i32(*v):
    return struct.pack('<%di' % len(v), *v)


def exposure_params(ev):
    # v7: mode, black, exposure, percentile, target, compensate bias, compensate highlights
    return i32(0) + f32(0.0, ev, 50.0, -4.0) + i32(0, 0)


def colorbalance_params(**kw):
    names = ['shadows_Y', 'shadows_C', 'shadows_H', 'midtones_Y', 'midtones_C', 'midtones_H',
             'highlights_Y', 'highlights_C', 'highlights_H', 'global_Y', 'global_C', 'global_H',
             'shadows_weight', 'white_fulcrum', 'highlights_weight', 'chroma_shadows',
             'chroma_highlights', 'chroma_global', 'chroma_midtones', 'saturation_global',
             'saturation_highlights', 'saturation_midtones', 'saturation_shadows', 'hue_angle',
             'brilliance_global', 'brilliance_highlights', 'brilliance_midtones',
             'brilliance_shadows', 'mask_grey_fulcrum', 'vibrance', 'grey_fulcrum', 'contrast']
    v = dict.fromkeys(names, 0.0)
    v.update(shadows_weight=1.0, highlights_weight=1.0, mask_grey_fulcrum=0.1845,
             grey_fulcrum=0.1845)
    v.update(kw)
    return f32(*[v[n] for n in names]) + i32(1)   # saturation formula: darktable UCS


class Blend:
    """dt_develop_blend_params_t v14, starting from the module defaults"""

    def __init__(self, default):
        self.b = bytearray(default)
        assert len(self.b) == 420, len(self.b)

    def set(self, off, fmt, *v):
        struct.pack_into('<' + fmt, self.b, off, *v)
        return self

    def drawn(self, group_id, inverted=False):
        mode = struct.unpack_from('<I', self.b, 0)[0]
        self.set(0, 'I', mode | MASK_ENABLED | MASK_DRAWN)
        self.set(24, 'i', group_id)
        if inverted:
            self.set(20, 'I', struct.unpack_from('<I', self.b, 20)[0] | 1)
        return self

    def parametric(self, channel, lo0, lo1, hi0, hi1, boost=0.0):
        mode = struct.unpack_from('<I', self.b, 0)[0]
        self.set(0, 'I', mode | MASK_ENABLED | MASK_PARAMETRIC)
        blendif = struct.unpack_from('<I', self.b, 28)[0] | (1 << channel) | BLENDIF_ACTIVE
        self.set(28, 'I', blendif)
        self.set(68 + channel * 16, '4f', lo0, lo1, hi0, hi1)
        self.set(324 + channel * 4, 'f', boost)
        return self

    def feather(self, radius):
        # feathering guided by the output image, then blur
        return self.set(32, 'f', radius).set(36, 'I', 6)

    def blur(self, radius):
        return self.set(40, 'f', radius)

    def hex(self):
        self.set(4, 'i', CS_RGB_SCENE)
        self.set(8, 'I', BLEND_NORMAL)
        self.set(16, 'f', 100.0)
        return bytes(self.b).hex()


class Xmp:
    def __init__(self, path):
        self.text = open(path, encoding='utf-8').read()
        self.entries = []
        self.forms = []          # (id, type, name, points bytes, nb)
        self.next_id = 1700000000

    def default_blend(self, op):
        m = re.search(r'darktable:operation="%s".*?darktable:blendop_params="([^"]+)"' % op,
                      self.text, re.S)
        return decode(m.group(1))

    def next_priority(self, op):
        pr = [int(p) for p in re.findall(r'%s,(\d+)' % op, self.text)]
        pr += [e['priority'] for e in self.entries if e['op'] == op]
        return max(pr) + 1

    def form(self, ftype, name, points, nb=1):
        fid = self.next_id
        self.next_id += 1
        self.forms.append((fid, ftype, name, points, nb))
        return fid

    def group(self, name, members):
        # members: [(form id, state)]
        gid = self.next_id
        self.next_id += 1
        pts = b''.join(i32(fid) + i32(gid) + i32(state) + f32(1.0) for fid, state in members)
        self.forms.append((gid, GROUP, name, pts, len(members)))
        return gid

    def add(self, op, version, params, blend, name):
        self.entries.append(dict(op=op, version=version, params=params.hex(), blend=blend.hex(),
                                 name=name, priority=self.next_priority(op)))

    def write(self, path):
        t = self.text
        n0 = len(re.findall(r'<rdf:li\s+darktable:num=', t))
        lis = []
        for k, e in enumerate(self.entries):
            lis.append(f'''     <rdf:li
      darktable:num="{n0 + k}"
      darktable:operation="{e['op']}"
      darktable:enabled="1"
      darktable:modversion="{e['version']}"
      darktable:params="{e['params']}"
      darktable:multi_name="{e['name']}"
      darktable:multi_name_hand_edited="1"
      darktable:multi_priority="{e['priority']}"
      darktable:blendop_version="14"
      darktable:blendop_params="{e['blend']}"/>''')
            # new instance in the pipe order, right after the previous instance
            t = re.sub(r'(darktable:iop_order_list="[^"]*?%s,%d)' % (e['op'], e['priority'] - 1),
                       r'\1,%s,%d' % (e['op'], e['priority']), t, count=1)
        hist = t.index('</rdf:Seq>', t.index('<darktable:history>'))
        t = t[:hist] + '\n'.join(lis) + '\n    ' + t[hist:]
        t = re.sub(r'darktable:history_end="\d+"',
                   'darktable:history_end="%d"' % (n0 + len(self.entries)), t)
        # all the forms, attached to the last history item
        last = n0 + len(self.entries) - 1
        mlis = []
        for fid, ftype, name, pts, nb in self.forms:
            mlis.append(f'''     <rdf:li
      darktable:mask_num="{last}"
      darktable:mask_id="{fid}"
      darktable:mask_type="{ftype}"
      darktable:mask_name="{name}"
      darktable:mask_version="6"
      darktable:mask_points="{pts.hex()}"
      darktable:mask_nb="{nb}"
      darktable:mask_src="0000000000000000"/>''')
        t = t.replace('<darktable:masks_history>\n    <rdf:Seq/>',
                      '<darktable:masks_history>\n    <rdf:Seq>\n' + '\n'.join(mlis) + '\n    </rdf:Seq>', 1)
        open(path, 'w', encoding='utf-8').write(t)


def ellipse(cx, cy, rx, ry, rotation=0.0, border=0.2):
    return f32(cx, cy, rx, ry, rotation, border) + i32(1)   # proportional feather


def gradient(ax, ay, rotation, compression=0.5, steepness=0.0, curvature=0.0):
    return f32(ax, ay, rotation, compression, steepness, curvature) + i32(2)   # sigmoidal


def brush(points, size=0.03, hardness=0.5):
    out = b''
    for x, y in points:
        out += f32(x, y, x, y, x, y, size, size, 1.0, hardness) + i32(1)
    return out


def render(src, xmp, out, size=1400):
    if os.path.exists(out):
        os.remove(out)
    fwd = lambda p: p.replace(os.sep, '/')
    r = subprocess.run([BIN, fwd(src), fwd(xmp), fwd(out), '--width', str(size), '--height',
                        str(size), '--core', '--configdir', CFG], capture_output=True, text=True)
    if not os.path.exists(out):
        print('render failed', r.stdout[-500:], r.stderr[-500:])
    return out


RED = dict(global_C=0.55, global_H=29.3)      # mask overlay, like Lightroom's red


def overlay_tests():
    """one red overlay per mask type, to check what each one selects"""
    os.makedirs(OUT, exist_ok=True)
    src = os.path.join(HERE, 'src', 'ryn03000.dng')
    base = os.path.join(HERE, 'dtxmp', 'ryn03000.xmp')
    tests = []

    def drawn(name, ftype, pts, inverted=False, nb=1):
        x = Xmp(base)
        fid = x.form(ftype, name, pts, nb)
        gid = x.group('grp ' + name, [(fid, USE | SHOW | UNION | (INVERSE if inverted else 0))])
        x.add('colorbalancergb', 5, colorbalance_params(**RED),
              Blend(x.default_blend('colorbalancergb')).drawn(gid), name)
        return x

    tests.append(('linear gradient', drawn('gradient', GRADIENT, gradient(0.5, 0.35, 0.0))))
    tests.append(('radial (ellipse)', drawn('ellipse', ELLIPSE, ellipse(0.62, 0.28, 0.22, 0.12, 0.0, 0.5))))
    face = [(0.53, 0.30), (0.54, 0.32), (0.545, 0.34), (0.55, 0.36)]
    tests.append(('brush', drawn('brush', BRUSH, brush(face, 0.02, 0.7), nb=len(face))))

    x = Xmp(base)
    x.add('colorbalancergb', 5, colorbalance_params(**RED),
          Blend(x.default_blend('colorbalancergb')).parametric(GRAY_IN, 0.0, 0.0, 0.02, 0.06), 'shadows')
    tests.append(('luminance range (dark tones)', x))

    x = Xmp(base)
    x.add('colorbalancergb', 5, colorbalance_params(global_C=0.55, global_H=260.0),
          Blend(x.default_blend('colorbalancergb')).parametric(HZ_IN, 0.10, 0.15, 0.25, 0.30), 'jacket')
    tests.append(('color range (hue)', x))

    for k, (label, x) in enumerate(tests):
        xp = os.path.join(OUT, 'test%d.xmp' % k)
        x.write(xp)
        render(src, xp, os.path.join(OUT, 'test%d.jpg' % k), 700)
        print(label, 'rendered')
    return tests


if __name__ == '__main__':
    overlay_tests()
