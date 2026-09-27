"""Golden-hour edits with Lightroom-style settings (crs XMP), rendered by Lightspeed.

    python golden.py [name ...]      # default: all photos
Writes xmp/<name>.xmp, out/<name>.jpg (full size) and sheets/<name>.jpg (before | after).
"""
import os
import subprocess
import sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = r'C:\lightspeed\install\bin\darktable-cli.exe'
CFG = 'C:/lightspeed/qa/cli-golden'

# as shot white balance of each photo (Kelvin, tint), read by the importer
AS_SHOT = {'dng17': (5470, 23), 'dsc00936': (4997, 22), 'dsc06876': (3961, 0),
           'img7059': (3533, -23), 'paris': (5470, 23), 'ryn03000': (4716, 0)}

# Lightroom values; 'dtemp'/'dtint' are added to the as shot white balance
EDITS = {
    # portrait by a sunlit yellow wall: already warm, make the light glow
    'dng17': dict(dtemp=350, dtint=4, Exposure2012=0.05, Contrast2012=14, Highlights2012=-40,
                  Shadows2012=22, Whites2012=4, Blacks2012=-8, Texture=4, Clarity2012=6,
                  Dehaze=-4, Vibrance=16, Saturation=0,
                  grade=dict(highlights=(42, 18, 0), midtones=(35, 5, 0), shadows=(25, 8, 0)),
                  hsl=dict(Orange=(0, -8, 3), Yellow=(-8, 4, 0), Blue=(0, -20, 0)),
                  vignette=-12),
    # man under a waterfall: warm sunset light in the mist, cool shadows
    'dsc00936': dict(dtemp=450, dtint=2, Exposure2012=0.20, Contrast2012=20, Highlights2012=-45,
                     Shadows2012=28, Whites2012=6, Blacks2012=-14, Texture=6, Clarity2012=14,
                     Dehaze=10, Vibrance=18, Saturation=2,
                     grade=dict(highlights=(42, 48, 0), midtones=(38, 10, 0), shadows=(205, 32, 0)),
                     hsl=dict(Orange=(0, 8, 4), Blue=(0, -5, 0)),
                     vignette=-20),
    # deer in the forest: low sun through the trees
    'dsc06876': dict(dtemp=650, dtint=4, Exposure2012=0.30, Contrast2012=12, Highlights2012=-45,
                     Shadows2012=32, Whites2012=0, Blacks2012=-6, Texture=10, Clarity2012=12,
                     Dehaze=6, Vibrance=20, Saturation=0,
                     grade=dict(highlights=(45, 24, 0), midtones=(38, 7, 0), shadows=(200, 6, 0)),
                     hsl=dict(Green=(22, -18, 0), Yellow=(-10, 10, 4), Orange=(0, 10, 5)),
                     vignette=-20),
    # portrait on a bench in the shade (shot vertically)
    'img7059': dict(dtemp=700, dtint=6, Exposure2012=0.15, Contrast2012=8, Highlights2012=-30,
                    Shadows2012=20, Whites2012=4, Blacks2012=-6, Texture=0, Clarity2012=4,
                    Dehaze=0, Vibrance=14, Saturation=0,
                    grade=dict(highlights=(40, 26, 0), midtones=(35, 6, 0), shadows=(28, 8, 0)),
                    hsl=dict(Green=(25, -20, 0), Orange=(0, -4, 5), Blue=(0, -15, 0)),
                    vignette=-12, orientation=8),
    # hat and cherry blossoms
    'paris': dict(dtemp=400, dtint=4, Exposure2012=0.10, Contrast2012=10, Highlights2012=-32,
                  Shadows2012=16, Whites2012=4, Blacks2012=-6, Texture=2, Clarity2012=5,
                  Dehaze=0, Vibrance=12, Saturation=0,
                  grade=dict(highlights=(40, 22, 0), midtones=(32, 6, 0), shadows=(22, 6, 0)),
                  hsl=dict(Orange=(0, -4, 4), Yellow=(-6, 0, 0), Magenta=(0, 8, 0)),
                  vignette=-10),
    # sunset over the mountains, very underexposed
    'ryn03000': dict(dtemp=500, dtint=4, Exposure2012=1.50, Contrast2012=10, Highlights2012=-65,
                     Shadows2012=65, Whites2012=10, Blacks2012=6, Texture=8, Clarity2012=16,
                     Dehaze=12, Vibrance=25, Saturation=2,
                     grade=dict(highlights=(38, 28, 0), midtones=(34, 7, 0), shadows=(215, 14, 0)),
                     hsl=dict(Orange=(0, 12, 6), Yellow=(-8, 8, 0), Blue=(0, -10, -5)),
                     vignette=-15),
}

GRADE_KEYS = {'highlights': ('SplitToningHighlightHue', 'SplitToningHighlightSaturation',
                             'ColorGradeHighlightLum'),
              'shadows': ('SplitToningShadowHue', 'SplitToningShadowSaturation',
                          'ColorGradeShadowLum'),
              'midtones': ('ColorGradeMidtoneHue', 'ColorGradeMidtoneSat', 'ColorGradeMidtoneLum')}


def xmp_for(name):
    e = dict(EDITS[name])
    t0, i0 = AS_SHOT[name]
    attrs = {'ProcessVersion': '11.0', 'WhiteBalance': 'Custom',
             'Temperature': '%d' % (t0 + e.pop('dtemp')), 'Tint': '%+d' % (i0 + e.pop('dtint'))}
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
    orientation = e.pop('orientation', None)
    for k, v in e.items():
        attrs[k] = ('%+.2f' % v) if k == 'Exposure2012' else ('%+d' % v)
    lines = '\n'.join('    crs:%s="%s"' % kv for kv in attrs.items())
    tiff = '\n    tiff:Orientation="%d"' % orientation if orientation else ''
    return f'''<x:xmpmeta xmlns:x="adobe:ns:meta/" x:xmptk="Adobe XMP Core 7.0">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about=""
    xmlns:xmp="http://ns.adobe.com/xap/1.0/"
    xmlns:tiff="http://ns.adobe.com/tiff/1.0/"
    xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"
    xmp:CreatorTool="Adobe Photoshop Lightroom Classic 13.0 (Windows)"{tiff}
{lines}/>
 </rdf:RDF>
</x:xmpmeta>
'''


def render(name, src, xmp, out, size):
    fwd = lambda p: p.replace(os.sep, '/')
    args = [BIN, fwd(src)] + ([fwd(xmp)] if xmp else []) + [fwd(out)]
    if size:
        args += ['--width', str(size), '--height', str(size)]
    args += ['--core', '--configdir', CFG]
    if os.path.exists(out):
        os.remove(out)
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0 or not os.path.exists(out):
        print(name, 'failed', r.stdout[-400:], r.stderr[-400:])
        return False
    return True


def sheet(name, before, after, out):
    a, b = Image.open(before), Image.open(after)
    h = 900
    a = a.resize((int(a.width * h / a.height), h))
    b = b.resize((int(b.width * h / b.height), h))
    s = Image.new('RGB', (a.width + b.width + 30, h + 50), (24, 24, 24))
    s.paste(a, (10, 40))
    s.paste(b, (a.width + 20, 40))
    d = ImageDraw.Draw(s)
    try:
        f = ImageFont.truetype('segoeui.ttf', 24)
    except OSError:
        f = ImageFont.load_default()
    d.text((14, 6), 'before (default)', fill=(200, 200, 200), font=f)
    d.text((a.width + 24, 6), 'after (golden hour, Lightspeed)', fill=(255, 205, 130), font=f)
    s.save(out, quality=90)


def main():
    names = sys.argv[1:] or list(EDITS)
    for d in ('xmp', 'out', 'preview', 'sheets', 'default'):
        os.makedirs(os.path.join(HERE, d), exist_ok=True)
    srcs = {os.path.splitext(f)[0]: os.path.join(HERE, 'src', f) for f in os.listdir(os.path.join(HERE, 'src'))}
    for name in names:
        xmp = os.path.join(HERE, 'xmp', name + '.xmp')
        with open(xmp, 'w', encoding='utf-8') as f:
            f.write(xmp_for(name))
        before = os.path.join(HERE, 'default', name + '.jpg')
        if not os.path.exists(before):
            render(name, srcs[name], None, before, 1400)
        prev = os.path.join(HERE, 'preview', name + '.jpg')
        if render(name, srcs[name], xmp, prev, 1400):
            sheet(name, before, prev, os.path.join(HERE, 'sheets', name + '.jpg'))
            print(name, 'ok')
        if '--full' in os.environ.get('GOLDEN', ''):
            render(name, srcs[name], xmp, os.path.join(HERE, 'out', name + '.jpg'), 0)


if __name__ == '__main__':
    main()
