"""Tonelark edit settings: the Lightroom words the AI models know, and the
Camera Raw (crs) names that Tonelark applies.

Friendly settings (what a model or a chat writes):

    {"Exposure": 0.3, "Contrast": 10, "Highlights": -40, "Shadows": 25,
     "Whites": 5, "Blacks": -5, "Texture": 0, "Clarity": 10, "Dehaze": 0,
     "Vibrance": 15, "Saturation": 0, "Temperature": 5600, "Tint": 5,
     "HSL": {"Orange": {"hue": 0, "sat": -10, "lum": 5}},
     "ColorGrade": {"Shadows": {"hue": 220, "sat": 10}, "Highlights": {"hue": 45, "sat": 15}},
     "Monochrome": false}

Only the given settings change, values are absolute (not added).
No third party modules: also used by the MCP server.
"""
import math

BASIC = [  # friendly name, Camera Raw name, min, max, digits
    ('Exposure', 'Exposure2012', -5.0, 5.0, 2),
    ('Contrast', 'Contrast2012', -100, 100, 0),
    ('Highlights', 'Highlights2012', -100, 100, 0),
    ('Shadows', 'Shadows2012', -100, 100, 0),
    ('Whites', 'Whites2012', -100, 100, 0),
    ('Blacks', 'Blacks2012', -100, 100, 0),
    ('Texture', 'Texture', -100, 100, 0),
    ('Clarity', 'Clarity2012', -100, 100, 0),
    ('Dehaze', 'Dehaze', -100, 100, 0),
    ('Vibrance', 'Vibrance', -100, 100, 0),
    ('Saturation', 'Saturation', -100, 100, 0),
]
HSL_COLORS = ['Red', 'Orange', 'Yellow', 'Green', 'Aqua', 'Blue', 'Purple', 'Magenta']
HSL_ALIASES = {'cyan': 'Aqua', 'teal': 'Aqua', 'violet': 'Purple', 'lavender': 'Purple', 'pink': 'Magenta'}
GRADE = {  # zone -> hue key, saturation key
    'Shadows': ('SplitToningShadowHue', 'SplitToningShadowSaturation'),
    'Midtones': ('ColorGradeMidtoneHue', 'ColorGradeMidtoneSat'),
    'Highlights': ('SplitToningHighlightHue', 'SplitToningHighlightSaturation'),
    'Global': ('ColorGradeGlobalHue', 'ColorGradeGlobalSat'),
}

VOCABULARY = """Exposure -5..5 (EV); Contrast, Highlights, Shadows, Whites, Blacks, Texture, Clarity, Dehaze, Vibrance, Saturation -100..100;
Temperature 2000..25000 (Kelvin of the light: higher makes the photo warmer/yellower, lower cooler/bluer); Tint -150..150 (positive = magenta, negative = green);
HSL: {"Orange": {"hue": -100..100, "sat": -100..100, "lum": -100..100}, ...} for Red, Orange, Yellow, Green, Aqua, Blue, Purple, Magenta (skin is mostly Orange);
ColorGrade: {"Shadows": {"hue": 0..360, "sat": 0..100}, "Midtones": {...}, "Highlights": {...}, "Global": {...}} (hue 0 red, 45 orange, 60 yellow, 120 green, 200 cyan-blue, 240 blue, 300 magenta; sat 5..25 is subtle, 25..50 strong);
Vignette -100..100 (negative darkens the corners and draws the eye to the subject, -15..-35 is usual);
Monochrome: true or false."""


def _num(v, default=0.0):
    try:
        f = float(v)
        return f if math.isfinite(f) else default
    except (TypeError, ValueError):
        return default


def clamp(v, lo, hi):
    return max(lo, min(hi, v))


def friendly(crs):
    """current settings (Camera Raw names, from Tonelark) -> friendly"""
    out = {}
    for name, key, lo, hi, digits in BASIC:
        out[name] = round(_num(crs.get(key)), digits)
        if digits == 0:
            out[name] = int(out[name])
    out['Temperature'] = int(round(_num(crs.get('Temperature'), 5000)))
    out['Tint'] = int(round(_num(crs.get('Tint'))))
    hsl = {}
    for c in HSL_COLORS:
        h = _num(crs.get('HueAdjustment' + c))
        s = _num(crs.get('SaturationAdjustment' + c))
        lum = _num(crs.get('LuminanceAdjustment' + c))
        if abs(h) >= 0.5 or abs(s) >= 0.5 or abs(lum) >= 0.5:
            hsl[c] = dict(hue=int(round(h)), sat=int(round(s)), lum=int(round(lum)))
    if hsl:
        out['HSL'] = hsl
    grade = {}
    for zone, (hk, sk) in GRADE.items():
        s = _num(crs.get(sk))
        if s >= 0.5:
            grade[zone] = dict(hue=int(round(_num(crs.get(hk)))), sat=int(round(s)))
    if grade:
        out['ColorGrade'] = grade
    vignette = int(round(_num(crs.get('PostCropVignetteAmount'))))
    if vignette:
        out['Vignette'] = vignette
    if str(crs.get('ConvertToGrayscale')) in ('True', 'true', '1'):
        out['Monochrome'] = True
    return out


def compact(settings):
    """short text of the settings that are not neutral"""
    parts = []
    for k, v in settings.items():
        if k in ('HSL', 'ColorGrade'):
            parts.append('%s %s' % (k, v))
        elif k == 'Temperature' or k == 'Tint' or (isinstance(v, (int, float)) and v) or v is True:
            parts.append('%s %s' % (k, v))
    return ', '.join(parts)


def _color(name):
    n = str(name).strip()
    for c in HSL_COLORS:
        if c.lower() == n.lower():
            return c
    return HSL_ALIASES.get(n.lower())


def _zone(name):
    n = str(name).strip().lower()
    for z in GRADE:
        if z.lower() == n:
            return z
    return None


def to_crs(settings, current=None):
    """friendly settings (only the given ones) -> Camera Raw names and values
    for Tonelark; current: the current settings (Camera Raw names)"""
    current = current or {}
    if not isinstance(settings, dict):
        return {}
    lower = {str(k).lower(): v for k, v in settings.items()}
    crs = {}
    for name, key, lo, hi, digits in BASIC:
        v = lower.get(name.lower(), lower.get(key.lower()))
        if v is not None:
            crs[key] = round(clamp(_num(v), lo, hi), digits)
    if 'temperature' in lower or 'tint' in lower or 'temp' in lower:
        t = clamp(_num(lower.get('temperature', lower.get('temp')), _num(current.get('Temperature'), 5000)),
                  2000, 25000)
        ti = clamp(_num(lower.get('tint'), _num(current.get('Tint'))), -150, 150)
        crs['WhiteBalance'] = 'Custom'
        crs['Temperature'] = round(t)
        crs['Tint'] = round(ti)
        # images that are not raw: Tonelark reads these
        crs['IncrementalTemperature'] = round(100.0 * math.log2(t / 5003.0), 2)
        crs['IncrementalTint'] = round(ti)
    hsl = lower.get('hsl')
    if isinstance(hsl, dict):
        for name, v in hsl.items():
            c = _color(name)
            if not c or not isinstance(v, dict):
                continue
            lv = {str(k).lower(): x for k, x in v.items()}
            for field, prefix in (('hue', 'Hue'), ('sat', 'Saturation'), ('saturation', 'Saturation'),
                                  ('lum', 'Luminance'), ('luminance', 'Luminance')):
                if field in lv:
                    crs['%sAdjustment%s' % (prefix, c)] = round(clamp(_num(lv[field]), -100, 100))
    grade = lower.get('colorgrade', lower.get('color_grade', lower.get('colorgrading')))
    if isinstance(grade, dict):
        for name, v in grade.items():
            z = _zone(name)
            if not z or not isinstance(v, dict):
                continue
            lv = {str(k).lower(): x for k, x in v.items()}
            hk, sk = GRADE[z]
            if 'hue' in lv:
                crs[hk] = round(_num(lv['hue']) % 360)
            if 'sat' in lv or 'saturation' in lv:
                crs[sk] = round(clamp(_num(lv.get('sat', lv.get('saturation'))), 0, 100))
    mono = lower.get('monochrome', lower.get('blackandwhite', lower.get('bw')))
    if mono is not None:
        crs['ConvertToGrayscale'] = 'True' if str(mono).lower() in ('true', '1', 'yes') else 'False'
    vignette = lower.get('vignette', lower.get('postcropvignetteamount'))
    if vignette is not None:
        crs.update(PostCropVignetteAmount=round(clamp(_num(vignette), -100, 100)), PostCropVignetteMidpoint=50,
                   PostCropVignetteFeather=50, PostCropVignetteRoundness=0, PostCropVignetteStyle=1)
    return crs


# ---------------------------------------------------------------------------
# crop

ASPECTS = {'1:1': 1.0, '4:5': 0.8, '5:4': 1.25, '2:3': 2 / 3, '3:2': 1.5, '3:4': 0.75, '4:3': 4 / 3,
           '16:9': 16 / 9, '9:16': 9 / 16, '5:7': 5 / 7, '7:5': 1.4}


def inner_scale(width, height, angle):
    """size of the largest rectangle of the original format inside the image
    rotated by angle (degrees), relative to the image"""
    a = math.radians(abs(angle))
    c, s = math.cos(a), math.sin(a)
    W, H = float(width), float(height)
    return min(W / (W * c + H * s), H / (W * s + H * c))


def crop_edit(width, height, rect, aspect='original', current=None, angle=None):
    """crop settings for Tonelark.

    rect: left, top, right, bottom in 0..1 of the photo as shown now (which
    may be cropped and straightened already: current = dict(CropLeft, CropTop,
    CropRight, CropBottom, Straighten)). angle: new rotation in degrees to add
    (positive = clockwise), None to keep. The rectangle is fitted to the
    aspect ratio ('original', 'free' or 'W:H') and kept inside the image."""
    current = current or {}
    l0, t0 = _num(current.get('CropLeft'), 0.0), _num(current.get('CropTop'), 0.0)
    r0, b0 = _num(current.get('CropRight'), 1.0), _num(current.get('CropBottom'), 1.0)
    a0 = _num(current.get('Straighten'), 0.0)
    l, t, r, b = [clamp(_num(v), 0.0, 1.0) for v in rect]
    if r < l:
        l, r = r, l
    if b < t:
        t, b = b, t
    # in the frame of the current straighten, before the current crop
    L, T = l0 + l * (r0 - l0), t0 + t * (b0 - t0)
    R, B = l0 + r * (r0 - l0), t0 + b * (b0 - t0)
    a1 = a0
    # Tonelark's rotation is counterclockwise for positive values
    if angle is not None:
        a1 = clamp(a0 - _num(angle), -15.0, 15.0)
    if abs(a1 - a0) > 1e-3:
        k = inner_scale(width, height, a0) / inner_scale(width, height, a1)
        L, R = 0.5 + (L - 0.5) * k, 0.5 + (R - 0.5) * k
        T, B = 0.5 + (T - 0.5) * k, 0.5 + (B - 0.5) * k

    # aspect ratio, in pixels of the (straightened) image
    W, H = float(width), float(height)
    target = None
    if aspect == 'original':
        target = W / H
    elif aspect in ASPECTS:
        target = ASPECTS[aspect]
    cx, cy = (L + R) / 2, (T + B) / 2
    w, h = max(0.1, R - L) * W, max(0.1, B - T) * H
    if target:
        if w / h > target:
            w = h * target
        else:
            h = w / target
        # not larger than the image
        s = min(1.0, W / w, H / h)
        w, h = w * s, h * s
    w, h = w / W, h / H
    w, h = max(w, 0.2), max(h, 0.2)
    cx = clamp(cx, w / 2, 1 - w / 2)
    cy = clamp(cy, h / 2, 1 - h / 2)
    out = dict(CropLeft=round(cx - w / 2, 4), CropTop=round(cy - h / 2, 4),
               CropRight=round(cx + w / 2, 4), CropBottom=round(cy + h / 2, 4))
    if angle is not None:
        out['Straighten'] = round(a1, 2)
    return out
