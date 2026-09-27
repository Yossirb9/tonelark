import os, sys
sys.path.insert(0, r'C:\lightspeed\qa\golden')
from masks import *
from PIL import Image, ImageDraw, ImageFont

GOLD = 64.4   # Lightroom hue 40 in color balance rgb

# sunset: sun glow (radial), face dodge (brush), foreground lift (luminance range)
x = Xmp(os.path.join(HERE, 'dtxmp', 'ryn03000.xmp'))
e = x.form(ELLIPSE, 'sun glow', ellipse(0.60, 0.28, 0.26, 0.14, 0.0, 1.6))
g = x.group('grp sun glow', [(e, USE | SHOW | UNION)])
x.add('colorbalancergb', 5, colorbalance_params(highlights_C=0.10, highlights_H=GOLD, highlights_Y=0.06),
      Blend(x.default_blend('colorbalancergb')).drawn(g), 'sun glow')
b = x.form(BRUSH, 'face', brush([(0.53, 0.31), (0.54, 0.33), (0.545, 0.35), (0.55, 0.37)], 0.03, 0.2), 4)
g2 = x.group('grp face', [(b, USE | SHOW | UNION)])
x.add('exposure', 7, exposure_params(0.5), Blend(x.default_blend('exposure')).drawn(g2).blur(8.0), 'face dodge')
x.add('exposure', 7, exposure_params(0.40),
      Blend(x.default_blend('exposure')).parametric(GRAY_IN, 0.0, 0.0, 0.02, 0.07), 'shadow lift')
p = os.path.join(OUT, 'ryn_final.xmp'); x.write(p)
render(os.path.join(HERE, 'src', 'ryn03000.dng'), p, os.path.join(OUT, 'ryn_final.jpg'), 1400)

# cherry blossoms: more vivid pink (color range), the woman excluded (inverted ellipse)
x = Xmp(os.path.join(HERE, 'dtxmp', 'paris.xmp'))
e = x.form(ELLIPSE, 'woman', ellipse(0.47, 0.55, 0.22, 0.42, 0.0, 0.15))
g = x.group('grp not the woman', [(e, USE | SHOW | UNION | INVERSE)])
bl = Blend(x.default_blend('colorbalancergb')).drawn(g).parametric(HZ_IN, 0.04, 0.07, 0.13, 0.155)
bl.set(0, 'I', MASK_ENABLED | MASK_DRAWN | MASK_PARAMETRIC)
x.add('colorbalancergb', 5, colorbalance_params(chroma_global=0.35, saturation_global=0.15, hue_angle=6.0),
      bl, 'blossoms')
p = os.path.join(OUT, 'paris_final.xmp'); x.write(p)
render(os.path.join(HERE, 'src', 'paris.dng'), p, os.path.join(OUT, 'paris_final.jpg'), 1400)
print('done')
