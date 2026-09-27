"""Lightspeed logo: a lens aperture whose six blades glow in golden-hour
colours around a point of light, on a night-blue rounded square.

Writes the master SVGs and renders every PNG/ICO the application and the
installer use. Needs rsvg-convert (MSYS2 librsvg) and Pillow.

    python data/pixmaps/lightspeed/make_logo.py
"""
import math
import os
import shutil
import subprocess
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
PIXMAPS = os.path.dirname(HERE)
RSVG = shutil.which('rsvg-convert') or r'C:\msys64\ucrt64\bin\rsvg-convert.exe'

# golden hour, from the sun's core to the afterglow
BLADES = [('#fff0c8', '#ffb23e'),   # gold
          ('#ffe6b8', '#ff9135'),   # amber
          ('#ffdcb0', '#ff6f3c'),   # orange
          ('#ffd6b8', '#f2566e'),   # coral rose
          ('#ffe0b8', '#ff7f45'),   # orange
          ('#fff0c8', '#ffa23a')]   # warm gold


def _darker(c, f=0.82):
    r, g, b = (int(c[i:i + 2], 16) for i in (1, 3, 5))
    return '#%02x%02x%02x' % (int(r * f), int(g * f), int(b * f))


def _pt(p):
    return '%.2f,%.2f' % p


def aperture(cx, cy, R, r, gap, rot=-8.0):
    """six blades between a hexagon of radius r and a circle of radius R"""
    def hexv(k):
        a = math.radians(60 * k + rot)
        return (cx + r * math.cos(a), cy + r * math.sin(a))

    def exit_point(p, q):
        # from p through q, where the line leaves the circle R
        dx, dy = q[0] - p[0], q[1] - p[1]
        fx, fy = p[0] - cx, p[1] - cy
        a = dx * dx + dy * dy
        b = 2 * (fx * dx + fy * dy)
        c = fx * fx + fy * fy - R * R
        t = (-b + math.sqrt(b * b - 4 * a * c)) / (2 * a)
        return (p[0] + t * dx, p[1] + t * dy)

    blades = []
    for k in range(6):
        v1, v2, v3 = hexv(k), hexv(k + 1), hexv(k + 2)
        pk = exit_point(v1, v2)        # line through V_k, V_k+1
        pk1 = exit_point(v2, v3)       # line through V_k+1, V_k+2
        d = ('M %s L %s A %.2f %.2f 0 0 1 %s L %s Z'
             % (_pt(v2), _pt(pk), R, R, _pt(pk1), _pt(v3)))
        blades.append(d)
    return blades


def icon_svg(size=1024, small=False):
    s = size / 1024.0
    cx = cy = 512
    R, r = 330, (150 if small else 128)
    gap = 26 if small else 14
    paths = aperture(cx, cy, R, r, gap)
    defs, body = [], []
    for k, d in enumerate(paths):
        c1, c2 = BLADES[k]
        # light from the centre through the blade
        defs.append('<radialGradient id="b%d" gradientUnits="userSpaceOnUse" '
                    'cx="512" cy="512" r="%d">'
                    '<stop offset="0.3" stop-color="%s"/><stop offset="0.62" stop-color="%s"/>'
                    '<stop offset="1" stop-color="%s"/>'
                    '</radialGradient>' % (k, R, c1, c2, _darker(c2)))
        body.append('<path d="%s" fill="url(#b%d)" stroke="#0d1230" stroke-width="%d" '
                    'stroke-linejoin="round"/>' % (d, k, gap))
    glow = '' if small else (
        '<circle cx="512" cy="512" r="420" fill="url(#halo)"/>')
    core = ('<circle cx="512" cy="512" r="%d" fill="url(#core)"/>'
            '<circle cx="512" cy="512" r="%d" fill="#fffaf0"/>'
            % ((118, 52) if small else (112, 40)))
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 1024 1024">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0.35" y2="1">
      <stop offset="0" stop-color="#26306b"/>
      <stop offset="0.55" stop-color="#141a42"/>
      <stop offset="1" stop-color="#0a0d24"/>
    </linearGradient>
    <radialGradient id="halo" cx="0.5" cy="0.5" r="0.5">
      <stop offset="0.55" stop-color="#ff9a3d" stop-opacity="0.28"/>
      <stop offset="1" stop-color="#ff9a3d" stop-opacity="0"/>
    </radialGradient>
    <radialGradient id="core" cx="0.5" cy="0.5" r="0.5">
      <stop offset="0" stop-color="#fff4d6"/>
      <stop offset="0.45" stop-color="#ffd27a"/>
      <stop offset="1" stop-color="#1a1440"/>
    </radialGradient>
    <linearGradient id="rim" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#ffffff" stop-opacity="0.22"/>
      <stop offset="1" stop-color="#ffffff" stop-opacity="0"/>
    </linearGradient>
    {''.join(defs)}
  </defs>
  <rect x="24" y="24" width="976" height="976" rx="220" fill="url(#bg)"/>
  <rect x="26" y="26" width="972" height="972" rx="218" fill="none" stroke="url(#rim)" stroke-width="4"/>
  {glow}
  <circle cx="512" cy="512" r="{R + 22}" fill="#0d1230"/>
  {''.join(body)}
  <circle cx="512" cy="512" r="{R + 22}" fill="none" stroke="#ffcf8a" stroke-opacity="0.35" stroke-width="6"/>
  {core}
</svg>
'''


def wordmark_svg():
    # "Light" regular, "speed" bold, as in the header
    return '''<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="112" height="20" viewBox="0 0 112 20">
  <text x="0" y="16" font-family="Segoe UI Light, Segoe UI, Roboto Light, sans-serif" font-weight="300"
        font-size="18" fill="#d9d9d9" letter-spacing="0.3">Light<tspan font-weight="600" fill="#ffffff">speed</tspan></text>
</svg>
'''


def render(svg_text, size, out):
    with tempfile.NamedTemporaryFile('w', suffix='.svg', delete=False, encoding='utf-8') as f:
        f.write(svg_text)
        tmp = f.name
    try:
        subprocess.check_call([RSVG, '-w', str(size), '-h', str(size), '-o', out, tmp])
    finally:
        os.unlink(tmp)


def main():
    big = icon_svg()
    small = icon_svg(small=True)
    with open(os.path.join(HERE, 'lightspeed.svg'), 'w', encoding='utf-8') as f:
        f.write(big)
    with open(os.path.join(HERE, 'lightspeed-small.svg'), 'w', encoding='utf-8') as f:
        f.write(small)

    # the header button and the splash screen, seasonal variants too
    for name in ('idbutton', 'idbutton-1', 'idbutton-2', 'idbutton-3'):
        with open(os.path.join(PIXMAPS, name + '.svg'), 'w', encoding='utf-8') as f:
            f.write(icon_svg(size=40))
        render(big, 128, os.path.join(PIXMAPS, name + '.png'))
    for name in ('darktable', 'darktable-1', 'darktable-2', 'darktable-3'):
        with open(os.path.join(PIXMAPS, 'scalable', name + '.svg'), 'w', encoding='utf-8') as f:
            f.write(big)

    # application icons, small sizes use the simplified drawing
    for size in (16, 22, 24, 32, 48, 64, 256):
        render(small if size <= 32 else big, size,
               os.path.join(PIXMAPS, '%dx%d' % (size, size), 'darktable.png'))
    render(big, 128, os.path.join(PIXMAPS, 'dt_logo_128x128.png'))

    # Windows icon with every size, rendered separately for sharpness
    frames = []
    for size in (16, 24, 32, 48, 64, 128, 256):
        out = os.path.join(tempfile.gettempdir(), 'ls_ico_%d.png' % size)
        render(small if size <= 32 else big, size, out)
        frames.append(Image.open(out).convert('RGBA'))
    frames[-1].save(os.path.join(PIXMAPS, 'dt_logo_128x128.ico'), format='ICO',
                    sizes=[(f.width, f.height) for f in frames], append_images=frames[:-1])

    # a large preview
    render(big, 1024, os.path.join(HERE, 'lightspeed-1024.png'))
    print('logo written')


if __name__ == '__main__':
    main()
