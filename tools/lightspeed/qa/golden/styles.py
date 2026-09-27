"""Ten styles on one photo (dng17: over the shoulder by a yellow wall), as Lightroom settings.

    python styles.py [number ...]
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from golden import AS_SHOT, GRADE_KEYS, HERE, render  # noqa: E402

NAME = 'dng17'
SRC = os.path.join(HERE, 'src', NAME + '.dng')
OUT = os.path.join(HERE, 'styles')

FADE = [(0, 26), (64, 76), (128, 134), (192, 194), (255, 246)]
S_CURVE = [(0, 0), (64, 56), (128, 128), (192, 202), (255, 255)]

STYLES = [
    ('Natural Clean', dict(dtemp=-250, dtint=-4, Exposure2012=0.10, Contrast2012=8, Highlights2012=-30,
        Shadows2012=15, Whites2012=5, Blacks2012=-5, Clarity2012=5, Vibrance=10,
        hsl=dict(Yellow=(0, -18, 0), Orange=(0, -6, 2)))),
    ('Golden Hour', dict(dtemp=350, dtint=4, Exposure2012=0.05, Contrast2012=14, Highlights2012=-40,
        Shadows2012=22, Whites2012=4, Blacks2012=-8, Texture=4, Clarity2012=6, Dehaze=-4, Vibrance=16,
        grade=dict(highlights=(42, 18, 0), midtones=(35, 5, 0), shadows=(25, 8, 0)),
        hsl=dict(Orange=(0, -8, 3), Yellow=(-8, 4, 0), Blue=(0, -20, 0)), vignette=-12)),
    ('Film Portra', dict(dtemp=50, dtint=4, Exposure2012=0.15, Contrast2012=-15, Highlights2012=-45,
        Shadows2012=30, Whites2012=-15, Blacks2012=12, Saturation=-10,
        curve=[(0, 34), (64, 80), (128, 134), (192, 192), (255, 240)],
        grade=dict(highlights=(48, 14, 0), shadows=(185, 16, 0)),
        hsl=dict(Green=(20, -30, 0), Yellow=(-6, -20, 4), Orange=(0, -4, 6), Blue=(0, -15, 0)),
        grain=(28, 35), vignette=-8)),
    ('Bright & Airy', dict(dtemp=-450, dtint=6, Exposure2012=0.75, Contrast2012=-20, Highlights2012=-55,
        Shadows2012=50, Whites2012=10, Blacks2012=20, Clarity2012=-10, Saturation=-15,
        grade=dict(highlights=(330, 5, 0)),
        hsl=dict(Yellow=(0, -50, 20), Orange=(0, -20, 12)))),
    ('Moody Dark', dict(dtemp=-350, dtint=0, Exposure2012=-0.60, Contrast2012=25, Highlights2012=-55,
        Shadows2012=-25, Whites2012=-20, Blacks2012=-35, Clarity2012=18, Dehaze=10, Saturation=-30,
        grade=dict(highlights=(35, 8, 0), shadows=(215, 22, 0)),
        hsl=dict(Yellow=(-10, -55, -20), Orange=(0, -25, -5)), vignette=-40)),
    ('Cinematic Teal & Orange', dict(dtemp=-550, dtint=-4, Contrast2012=18, Highlights2012=-35,
        Shadows2012=12, Blacks2012=-12, Clarity2012=8, curve=S_CURVE,
        grade=dict(highlights=(35, 22, 0), midtones=(205, 8, 0), shadows=(200, 45, 0)),
        hsl=dict(Yellow=(-20, -35, 0), Orange=(0, 0, 5)), vignette=-18)),
    ('Vintage Faded', dict(dtemp=300, dtint=10, Contrast2012=-20, Highlights2012=-30, Shadows2012=20,
        Blacks2012=20, Saturation=-25, curve=[(0, 48), (80, 92), (170, 178), (255, 232)],
        grade=dict(highlights=(50, 16, 0), shadows=(285, 12, 0)), grain=(35, 45), vignette=-20)),
    ('Classic B&W', dict(bw=True, Exposure2012=0.05, Contrast2012=30, Highlights2012=-25, Shadows2012=10,
        Whites2012=15, Blacks2012=-20, Clarity2012=20, curve=S_CURVE, grain=(15, 25), vignette=-15)),
    ('Sepia Matte', dict(bw=True, Exposure2012=0.10, Contrast2012=-10, Highlights2012=-20, Shadows2012=15,
        curve=FADE, grade=dict(highlights=(42, 22, 0), shadows=(30, 18, 0)), grain=(25, 35),
        vignette=-12)),
    ('Cool Editorial', dict(dtemp=-1300, dtint=-8, Exposure2012=0.15, Contrast2012=10, Highlights2012=-30,
        Shadows2012=12, Clarity2012=6, Saturation=-20,
        grade=dict(highlights=(200, 10, 0), shadows=(220, 16, 0)),
        hsl=dict(Yellow=(10, -60, -8), Orange=(0, -25, 0)))),
]


def xmp_for(e):
    e = dict(e)
    t0, i0 = AS_SHOT[NAME]
    attrs = {'ProcessVersion': '11.0'}
    if 'dtemp' in e:
        attrs.update(WhiteBalance='Custom', Temperature='%d' % (t0 + e.pop('dtemp')),
                     Tint='%+d' % (i0 + e.pop('dtint', 0)))
    else:
        attrs['WhiteBalance'] = 'As Shot'
    if e.pop('bw', False):
        attrs['ConvertToGrayscale'] = 'True'
    for side, (h, s, l) in e.pop('grade', {}).items():
        kh, ks, kl = GRADE_KEYS[side]
        attrs[kh], attrs[ks], attrs[kl] = str(h), str(s), str(l)
    for color, (h, s, l) in e.pop('hsl', {}).items():
        attrs['HueAdjustment' + color] = '%+d' % h
        attrs['SaturationAdjustment' + color] = '%+d' % s
        attrs['LuminanceAdjustment' + color] = '%+d' % l
    vig = e.pop('vignette', 0)
    if vig:
        attrs.update(PostCropVignetteAmount='%+d' % vig, PostCropVignetteMidpoint='50',
                     PostCropVignetteFeather='60', PostCropVignetteRoundness='0',
                     PostCropVignetteStyle='1')
    grain = e.pop('grain', None)
    if grain:
        attrs.update(GrainAmount=str(grain[0]), GrainSize=str(grain[1]), GrainFrequency='50')
    curve = e.pop('curve', None)
    if curve:
        attrs['ToneCurveName2012'] = 'Custom'
    for k, v in e.items():
        attrs[k] = ('%+.2f' % v) if k == 'Exposure2012' else ('%+d' % v)
    lines = '\n'.join('    crs:%s="%s"' % kv for kv in attrs.items())
    seq = ''
    if curve:
        pts = '\n'.join('      <rdf:li>%d, %d</rdf:li>' % p for p in curve)
        seq = f'''
   <crs:ToneCurvePV2012>
    <rdf:Seq>
{pts}
    </rdf:Seq>
   </crs:ToneCurvePV2012>
  '''
    return f'''<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="Adobe XMP Core 7.0">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
    xmlns:xmp="http://ns.adobe.com/xap/1.0/"
    xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"
    xmp:CreatorTool="Adobe Photoshop Lightroom Classic 13.0 (Windows)"
{lines}>{seq}</rdf:Description>
 </rdf:RDF>
</x:xmpmeta>
'''


def contact_sheet(files, out):
    f = ImageFont.truetype('segoeui.ttf', 26)
    ims = [Image.open(p) for p, _ in files]
    w = 460
    ims = [im.resize((w, int(im.height * w / im.width)), Image.LANCZOS) for im in ims]
    h = max(im.height for im in ims)
    cols = 5
    rows = (len(ims) + cols - 1) // cols
    s = Image.new('RGB', (cols * (w + 14) + 14, rows * (h + 54) + 14), (22, 22, 22))
    d = ImageDraw.Draw(s)
    for k, (im, (_, label)) in enumerate(zip(ims, files)):
        x = 14 + (k % cols) * (w + 14)
        y = 14 + (k // cols) * (h + 54)
        d.text((x, y), label, fill=(235, 235, 235), font=f)
        s.paste(im, (x, y + 40))
    s.save(out, quality=90)


def main():
    os.makedirs(OUT, exist_ok=True)
    pick = [int(a) for a in sys.argv[1:]] or range(1, len(STYLES) + 1)
    for n in pick:
        label, e = STYLES[n - 1]
        xmp = os.path.join(OUT, '%02d.xmp' % n)
        with open(xmp, 'w', encoding='utf-8') as f:
            f.write(xmp_for(e))
        render(label, SRC, xmp, os.path.join(OUT, '%02d.jpg' % n), 1400)
        print(n, label)
    files = [(os.path.join(OUT, '%02d.jpg' % n), '%d. %s' % (n, STYLES[n - 1][0]))
             for n in range(1, len(STYLES) + 1) if os.path.exists(os.path.join(OUT, '%02d.jpg' % n))]
    contact_sheet(files, os.path.join(OUT, 'styles_sheet.jpg'))


if __name__ == '__main__':
    main()
