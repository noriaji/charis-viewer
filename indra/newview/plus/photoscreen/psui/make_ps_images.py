"""Makes the photo screen's masks and icons (research/221, branch ps-style).

Writes into skins/default/textures/plus/ (one folder level only: viewer_manifest.py copies
textures/*/*.png, not deeper folders):
  ps_round.png   64x64  a white disc; drawRound() draws its quarters as the corners of any radius
  ps_circle.png 128x128 the same disc, finer edge: circles of 20 px radius and more
  ps_cut.png     64x64  the outside of the disc: covers the corners of an image (rounded pictures)
  ps_shadow.png  64x64  a blurred disc: the shadow under the knobs and the shutter button
  ps_icon_<name>.png  40x40 white line icons (shown at about 20 px, coloured when drawn)
  <name>_18.png  18x18 white filled toolbar buttons (photoscreen_18.png: the photo screen's button)

The icons are drawn from the SVG paths below, most of them those of Lucide (https://lucide.dev,
ISC license, see ORIGIN.md next to this file); "pose", "environment" and "lighting" are put
together here in the same style. Lucide draws on a 24 unit grid with 2 unit round lines.

Run from anywhere: python -I make_ps_images.py   (needs numpy, matplotlib and Pillow)
"""
import math
import os
import re

import numpy as np
from matplotlib.path import Path
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '..', '..', '..', 'skins', 'default', 'textures', 'plus'))

ICON = 40          # pixels of an icon
SS = 4             # supersampling per axis
GRID = 24.0        # Lucide's grid

# ---------------------------------------------------------------- the masks


def disc(size, ramp_texels, inverse=False):
    """A disc filling the image; its edge goes from 1 to 0 over ramp_texels, half covered at the
    image's border, so a shape drawn with it ends at its rectangle (square edges do too)."""
    c = (size - 1) / 2.0
    y, x = np.mgrid[0:size, 0:size]
    d = np.sqrt((x - c) ** 2 + (y - c) ** 2)
    r = size / 2.0
    a = np.clip((r - d) / ramp_texels + 0.5, 0.0, 1.0)
    if inverse:
        a = 1.0 - a
    return a


def shadow(size):
    """A soft disc: the shadow under a round thing (the disc is 70 % of the image)."""
    c = (size - 1) / 2.0
    y, x = np.mgrid[0:size, 0:size]
    d = np.sqrt((x - c) ** 2 + (y - c) ** 2) / (size / 2.0)
    return np.clip(1.0 - np.clip((d - 0.55) / 0.45, 0.0, 1.0), 0.0, 1.0) ** 2


def save_alpha(name, a):
    rgba = np.zeros(a.shape + (4,), np.uint8)
    rgba[..., 0:3] = 255
    rgba[..., 3] = np.round(np.clip(a, 0.0, 1.0) * 255.0).astype(np.uint8)
    Image.fromarray(rgba).save(os.path.join(OUT, name), optimize=True)

# ---------------------------------------------------------------- SVG paths


TOKEN = re.compile(r'[MmLlHhVvCcSsQqTtAaZz]|-?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?')


def arc_points(x1, y1, rx, ry, phi, fa, fs, x2, y2, steps=24):
    """SVG elliptical arc (endpoint form) to points, without the first one."""
    if rx == 0 or ry == 0:
        return [(x2, y2)]
    rx, ry = abs(rx), abs(ry)
    cp, sp = math.cos(phi), math.sin(phi)
    dx, dy = (x1 - x2) / 2.0, (y1 - y2) / 2.0
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    lam = (x1p ** 2) / (rx ** 2) + (y1p ** 2) / (ry ** 2)
    if lam > 1:
        s = math.sqrt(lam)
        rx, ry = rx * s, ry * s
    num = rx ** 2 * ry ** 2 - rx ** 2 * y1p ** 2 - ry ** 2 * x1p ** 2
    den = rx ** 2 * y1p ** 2 + ry ** 2 * x1p ** 2
    co = math.sqrt(max(0.0, num / den)) if den else 0.0
    if fa == fs:
        co = -co
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x1 + x2) / 2.0
    cy = sp * cxp + cp * cyp + (y1 + y2) / 2.0

    def ang(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = ang(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = ang((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not fs and dt > 0:
        dt -= 2 * math.pi
    elif fs and dt < 0:
        dt += 2 * math.pi
    n = max(4, int(abs(dt) / (2 * math.pi) * steps * 2))
    pts = []
    for i in range(1, n + 1):
        t = t1 + dt * i / n
        x, y = rx * math.cos(t), ry * math.sin(t)
        pts.append((cp * x - sp * y + cx, sp * x + cp * y + cy))
    return pts


def bezier(p0, p1, p2, p3, n=16):
    out = []
    for i in range(1, n + 1):
        t = i / n
        u = 1 - t
        out.append((u ** 3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t ** 3 * p3[0],
                    u ** 3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t ** 3 * p3[1]))
    return out


def parse_path(d):
    """SVG path data to a list of (points, closed) subpaths."""
    toks = TOKEN.findall(d)
    i = 0
    subs = []
    pts = []
    closed = False
    cx = cy = 0.0
    sx = sy = 0.0
    last_ctrl = None
    cmd = None

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    def flag():
        # arc flags may be written without separators ("a2 2 0 0 0 ..."): one digit each
        nonlocal i
        t = toks[i]
        if len(t) > 1 and t[0] in '01' and not t.startswith(('0.', '1.')):
            toks[i] = t[1:]
            return float(t[0])
        i += 1
        return float(t)

    while i < len(toks):
        if re.match(r'[A-Za-z]', toks[i]):
            cmd = toks[i]
            i += 1
            if cmd in 'Zz':
                closed = True
                cx, cy = sx, sy
                if pts:
                    subs.append((pts, True))
                pts = []
                closed = False
                continue
        rel = cmd.islower()
        c = cmd.upper()
        if not pts and c != 'M':
            pts = [(cx, cy)]     # drawing on after a Z starts where the closed subpath began
        if c == 'M':
            if pts:
                subs.append((pts, closed))
            x, y = num(), num()
            if rel:
                x, y = cx + x, cy + y
            cx, cy = sx, sy = x, y
            pts = [(x, y)]
            cmd = 'l' if rel else 'L'
            last_ctrl = None
        elif c == 'L':
            x, y = num(), num()
            if rel:
                x, y = cx + x, cy + y
            cx, cy = x, y
            pts.append((x, y))
            last_ctrl = None
        elif c == 'H':
            x = num()
            cx = cx + x if rel else x
            pts.append((cx, cy))
            last_ctrl = None
        elif c == 'V':
            y = num()
            cy = cy + y if rel else y
            pts.append((cx, cy))
            last_ctrl = None
        elif c in 'CS':
            if c == 'C':
                x1, y1 = num(), num()
                if rel:
                    x1, y1 = cx + x1, cy + y1
            else:
                x1, y1 = (2 * cx - last_ctrl[0], 2 * cy - last_ctrl[1]) if last_ctrl else (cx, cy)
            x2, y2, x, y = num(), num(), num(), num()
            if rel:
                x2, y2, x, y = cx + x2, cy + y2, cx + x, cy + y
            pts.extend(bezier((cx, cy), (x1, y1), (x2, y2), (x, y)))
            last_ctrl = (x2, y2)
            cx, cy = x, y
        elif c == 'Q':
            x1, y1, x, y = num(), num(), num(), num()
            if rel:
                x1, y1, x, y = cx + x1, cy + y1, cx + x, cy + y
            c1 = (cx + 2 / 3 * (x1 - cx), cy + 2 / 3 * (y1 - cy))
            c2 = (x + 2 / 3 * (x1 - x), y + 2 / 3 * (y1 - y))
            pts.extend(bezier((cx, cy), c1, c2, (x, y)))
            cx, cy = x, y
            last_ctrl = None
        elif c == 'A':
            rx, ry, rot = num(), num(), num()
            fa, fs = flag(), flag()
            x, y = num(), num()
            if rel:
                x, y = cx + x, cy + y
            pts.extend(arc_points(cx, cy, rx, ry, math.radians(rot), fa, fs, x, y))
            cx, cy = x, y
            last_ctrl = None
        else:
            raise ValueError('path command ' + cmd)
    if pts:
        subs.append((pts, closed))
    return subs


def circle(cx, cy, r):
    n = 48
    return [([(cx + r * math.cos(2 * math.pi * k / n), cy + r * math.sin(2 * math.pi * k / n)) for k in range(n)], True)]


def rect(x, y, w, h, rx=0.0):
    if rx <= 0:
        return [([(x, y), (x + w, y), (x + w, y + h), (x, y + h)], True)]
    d = ('M{a} {y}H{b}A{r} {r} 0 0 1 {xr} {c}V{dd}A{r} {r} 0 0 1 {b} {yb}H{a}A{r} {r} 0 0 1 {x} {dd}V{c}A{r} {r} 0 0 1 {a} {y}Z'
         .format(a=x + rx, b=x + w - rx, c=y + rx, dd=y + h - rx, r=rx, x=x, y=y, xr=x + w, yb=y + h))
    return parse_path(d)

# ---------------------------------------------------------------- the rasterizer


class Raster:
    """The sample points of an image of `pixels` x `pixels`, the shapes given on a `grid` x `grid` grid."""

    def __init__(self, pixels, grid):
        self.pixels = pixels
        self.n = pixels * SS
        ys, xs = np.mgrid[0:self.n, 0:self.n]
        self.px = ((xs + 0.5) / self.n * grid).ravel()
        self.py = ((ys + 0.5) / self.n * grid).ravel()
        self.pts = np.stack([self.px, self.py], axis=1)


R40 = Raster(ICON, GRID)    # the photo screen's icons
R18 = Raster(18, 18.0)      # the toolbar buttons: drawn on their own pixels (1 unit = 1 px), so lines are sharp


def transform(subs, s=1.0, tx=0.0, ty=0.0):
    return [([(x * s + tx, y * s + ty) for x, y in p], c) for p, c in subs]


def stroke_cov(subs, width, r=R40):
    N, PX, PY = r.n, r.px, r.py
    cov = np.zeros(N * N, bool)
    h = width / 2.0
    for pts, closed in subs:
        seq = list(pts) + ([pts[0]] if closed else [])
        if len(seq) == 1:
            seq = seq * 2
        a = np.array(seq[:-1])
        b = np.array(seq[1:])
        for (ax, ay), (bx, by) in zip(a, b):
            dx, dy = bx - ax, by - ay
            ll = dx * dx + dy * dy
            if ll == 0:
                t = np.zeros(N * N)
            else:
                t = np.clip(((PX - ax) * dx + (PY - ay) * dy) / ll, 0.0, 1.0)
            qx, qy = ax + t * dx - PX, ay + t * dy - PY
            cov |= (qx * qx + qy * qy) <= h * h
    return cov


def fill_cov(subs, r=R40):
    verts = []
    codes = []
    for pts, _ in subs:
        verts.extend(pts)
        verts.append(pts[0])
        codes.extend([Path.MOVETO] + [Path.LINETO] * (len(pts) - 1) + [Path.CLOSEPOLY])
    # nonzero winding (matplotlib's default for compound paths is even-odd per contains_points:
    # the shapes here do not overlap themselves, so both give the same)
    return Path(np.array(verts), codes).contains_points(r.pts)


class Icon:
    def __init__(self, r=R40):
        self.r = r
        self.cov = np.zeros(r.n * r.n, bool)

    def stroke(self, subs, width=2.0):
        self.cov |= stroke_cov(subs, width, self.r)
        return self

    def fill(self, subs):
        self.cov |= fill_cov(subs, self.r)
        return self

    def erase(self, subs, grow=0.0):
        m = fill_cov(subs, self.r)
        if grow > 0:
            m |= stroke_cov(subs, grow * 2, self.r)
        self.cov &= ~m
        return self

    def alpha(self):
        p = self.r.pixels
        a = self.cov.reshape(p, SS, p, SS).mean(axis=(1, 3))
        return a


def P(d):
    return parse_path(d)


def line_icon(*ds, width=2.0):
    ic = Icon()
    for d in ds:
        ic.stroke(P(d) if isinstance(d, str) else d, width)
    return ic


CORNERS = ['M3 7V5a2 2 0 0 1 2-2h2', 'M17 3h2a2 2 0 0 1 2 2v2', 'M21 17v2a2 2 0 0 1-2 2h-2', 'M7 21H5a2 2 0 0 1-2-2v-2']
GEAR = ('M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73'
        'l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73'
        'l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44'
        'a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39'
        'a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73'
        'l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z')
CAMERA = 'M14.5 4h-5L7 7H4a2 2 0 0 0-2 2v9a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V9a2 2 0 0 0-2-2h-3l-2.5-3z'
DROPLET = ('M12 22a7 7 0 0 0 7-7c0-2-1-3.9-3-5.5s-3.5-4-4-6.5c-.5 2.5-2 4.9-4 6.5C6 11.1 5 13 5 15'
           'a7 7 0 0 0 7 7z')
STAR = 'M12 2l3.09 6.26L22 9.27l-5 4.87 1.18 6.88L12 17.77l-6.18 3.25L7 14.14 2 9.27l6.91-1.01Z'
LEAF ='M11 20A7 7 0 0 1 9.8 6.1C15.5 5 17 4.48 19 2c1 2 2 4.18 2 8 0 5.5-4.78 10-10 10Z'
LEAF_STEM = 'M2 21c0-3 1.85-5.36 5.08-6C9.5 14.52 12 13 13 12'
BULB = 'M15 14c.2-1 .7-1.7 1.5-2.5 1-.9 1.5-2.2 1.5-3.5A6 6 0 0 0 6 8c0 1 .2 2.2 1.5 3.5.7.7 1.3 1.5 1.5 2.5'
PALETTE = ('M12 2C6.5 2 2 6.5 2 12s4.5 10 10 10c.926 0 1.648-.746 1.648-1.688 0-.437-.18-.835-.437-1.125'
           '-.29-.289-.438-.652-.438-1.125a1.64 1.64 0 0 1 1.668-1.668h1.996c3.051 0 5.555-2.503 5.555-5.554'
           'C21.965 6.012 17.461 2 12 2z')
SPARKLE = ('M9.937 15.5A2 2 0 0 0 8.5 14.063l-6.135-1.582a.5.5 0 0 1 0-.962L8.5 9.936A2 2 0 0 0 9.937 8.5'
           'l1.582-6.135a.5.5 0 0 1 .963 0L14.063 8.5A2 2 0 0 0 15.5 9.937l6.135 1.581a.5.5 0 0 1 0 .964'
           'L15.5 14.063a2 2 0 0 0-1.437 1.437l-1.582 6.135a.5.5 0 0 1-.963 0z')


def icons():
    out = {}
    # --- the rail (1. of research/221) -------------------------------------------------
    # scene: a picture with a mountain and the sun (Lucide "image")
    out['scene'] = line_icon(rect(3, 3, 18, 18, 2), circle(9, 9, 2), 'm21 15-3.086-3.086a2 2 0 0 0-2.828 0L6 21')
    # avatar: a person, filled (Lucide "user", the shapes filled)
    ic = Icon().fill(circle(12, 7, 4.6)).fill(P('M3.5 21.5v-1.5a5 5 0 0 1 5-5h7a5 5 0 0 1 5 5v1.5Z'))
    out['avatar'] = ic
    # environment: a drop and a leaf (Lucide "droplet" and "leaf", made smaller and put together)
    ic = Icon().stroke(transform(P(DROPLET), 0.62, -0.6, 8.6), 1.9)
    ic.stroke(transform(P(LEAF), 0.6, 9.4, 0.2), 1.9).stroke(transform(P(LEAF_STEM), 0.6, 9.4, 0.2), 1.9)
    out['environment'] = ic
    # lighting: a bulb with rays (Lucide "lightbulb", smaller, and the rays of "sun")
    ic = Icon()
    for d in (BULB, 'M9 18h6', 'M10 22h4'):
        ic.stroke(transform(P(d), 0.72, 3.36, 5.4), 2.0)
    for d in ('M12 1.2v1.6', 'M4.2 4.2l1.1 1.1', 'M19.8 4.2l-1.1 1.1', 'M1.5 10.6h1.6', 'M20.9 10.6h1.6'):
        ic.stroke(P(d), 1.9)
    out['lighting'] = ic
    # pose & animation: a running person (in Lucide's style; Lucide has no runner)
    ic = Icon().fill(circle(14.5, 4, 2.3))
    for d in ('M6 10.5 9.5 7.5l4 1-2 5 3.5 3v5', 'M11.5 13.5l-2.5 3.5H4.5', 'M13.5 8.5l2.5 3 3.5 0.5'):
        ic.stroke(P(d), 2.0)
    out['pose'] = ic
    # camera, filled (Lucide "camera": the body filled, the lens cut out and filled again)
    ic = Icon().fill(P(CAMERA)).erase(circle(12, 13, 4.4)).fill(circle(12, 13, 2.7))
    out['camera'] = ic
    # effects: sparkles (Lucide "sparkles")
    out['effects'] = line_icon(SPARKLE, 'M20 3v4', 'M22 5h-4', 'M4 17v2', 'M5 18H3')
    # presets: a page with lines and a folded corner (Lucide "file-text")
    out['presets'] = line_icon('M15 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V7Z',
                               'M14 2v4a2 2 0 0 0 2 2h4', 'M10 9H8', 'M16 13H8', 'M16 17H8')
    # quality: a gauge (Lucide "gauge"; ps-quality: the rail's first tab)
    out['quality'] = line_icon('m12 14 4-4', 'M3.34 19a10 10 0 1 1 17.32 0')
    # --- headings of the panels (2.) ----------------------------------------------------
    out['clapperboard'] = line_icon('M20.2 6 3 11l-.9-2.4c-.3-1.1.3-2.2 1.3-2.5l13.5-4c1.1-.3 2.2.3 2.5 1.3Z',
                                    'm6.2 5.3 3.1 3.9', 'm12.4 3.4 3.1 4', 'M3 11h18v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2Z')
    out['sliders'] = line_icon('M21 4h-7', 'M10 4H3', 'M21 12h-9', 'M8 12H3', 'M21 20h-5', 'M12 20H3',
                               'M14 2v4', 'M8 10v4', 'M16 18v4')
    out['user'] = line_icon('M19 21v-2a4 4 0 0 0-4-4H9a4 4 0 0 0-4 4v2', circle(12, 7, 4))
    out['aperture'] = line_icon(circle(12, 12, 10), 'm14.31 8 5.74 9.94', 'M9.69 8h11.48', 'm7.38 12 5.74-9.94',
                                'M9.69 16 3.95 6.06', 'M14.31 16H2.83', 'm16.62 12-5.74 9.94')
    out['sun'] = line_icon(circle(12, 12, 4), 'M12 2v2', 'M12 20v2', 'm4.93 4.93 1.41 1.41', 'm17.66 17.66 1.41 1.41',
                           'M2 12h2', 'M20 12h2', 'm6.34 17.66-1.41 1.41', 'm19.07 4.93-1.41 1.41')
    ic = line_icon(PALETTE)
    for cx, cy in ((13.5, 6.5), (17.5, 10.5), (8.5, 7.5), (6.5, 12.5)):
        ic.fill(circle(cx, cy, 1.3))
    out['palette'] = ic
    out['output'] = line_icon('M4 12v8a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-8', 'M16 6 12 2 8 6', 'M12 2v13')
    # --- chips (8.) ---------------------------------------------------------------------
    out['film'] = line_icon(rect(3, 3, 18, 18, 2), 'M7 3v18', 'M3 7.5h4', 'M3 12h18', 'M3 16.5h4', 'M17 3v18',
                            'M17 7.5h4', 'M17 16.5h4')
    out['resolution'] = line_icon(*CORNERS, rect(7, 7, 10, 10, 1.5))
    out['ratio'] = line_icon(rect(6, 2, 12, 20, 2), rect(2, 6, 20, 12, 2))
    out['grid'] = line_icon(rect(3, 3, 18, 18, 2), 'M3 9h18', 'M3 15h18', 'M9 3v18', 'M15 3v18')
    # --- the buttons in the world (12.) -------------------------------------------------
    out['focus'] = line_icon(*CORNERS, circle(12, 12, 3))
    out['frame'] = line_icon(*CORNERS).fill(circle(12, 12, 1.6))
    out['maximize'] = line_icon('M15 3h6v6', 'M9 21H3v-6', 'M21 3l-7 7', 'M3 21l7-7')
    out['scan_face'] = line_icon(*CORNERS, 'M8 14s1.5 2 4 2 4-2 4-2', 'M9 9h.01', 'M15 9h.01')
    # flycam: the camera flies free in three axes (Lucide "move-3d"; photo-flycam: the flycam on / off in the world)
    out['flycam'] = line_icon('M5 3v16h16', 'm5 19 6-6', 'm2 6 3-3 3 3', 'm18 16 3 3-3 3')
    # --- small ones ---------------------------------------------------------------------
    out['chevron_down'] = line_icon('m6 9 6 6 6-6', width=1.6)
    out['chevron_up'] = line_icon('m18 15-6-6-6 6', width=1.6)
    ic = Icon()
    for cx in (5, 12, 19):
        ic.fill(circle(cx, 12, 1.7))
    out['more'] = ic
    out['plus'] = line_icon('M5 12h14', 'M12 5v14', width=1.5)
    out['settings'] = Icon().fill(P(GEAR)).erase(circle(12, 12, 3.2))
    out['check'] = line_icon('M20 6 9 17l-5-5', width=3.0)
    out['close'] = line_icon('M18 6 6 18', 'm6 6 12 12', width=2.4)
    # --- the sky and water lists (ps-sky-water) -----------------------------------------
    # star: Lucide "star" (the outline: a favourite that is not one; filled: a favourite)
    out['star'] = line_icon(STAR, width=1.8)
    out['star_filled'] = Icon().fill(P(STAR)).stroke(P(STAR), 1.8)
    # water: Lucide "droplet"
    out['water'] = line_icon(DROPLET)
    # --- the conversations tab (ps-im) --------------------------------------------------
    # message: Lucide "message-circle" (the rail's conversations tab)
    out['message'] = line_icon('M7.9 20A9 9 0 1 0 4 16.1L2 22Z')
    # send: Lucide "send" (the button beside the line to type in)
    out['send'] = line_icon('M14.536 21.686a.5.5 0 0 0 .937-.024l6.5-19a.496.496 0 0 0-.635-.635l-19 6.5'
                            'a.5.5 0 0 0-.024.937l7.93 3.18a2 2 0 0 1 1.112 1.11z', 'm21.854 2.147-10.94 10.939')
    # --- the camera controls of the camera tab (photo-camera, research/260) ---------------
    # the pads' arrows: Lucide "chevron-left" / "chevron-right" (as thin as chevron_up / down)
    out['chevron_left'] = line_icon('m15 18-6-6 6-6', width=1.6)
    out['chevron_right'] = line_icon('m9 18 6-6-6-6', width=1.6)
    # the tilt buttons: Lucide "rotate-ccw" / "rotate-cw"
    out['rotate_ccw'] = line_icon('M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8', 'M3 3v5h5')
    out['rotate_cw'] = line_icon('M21 12a9 9 0 1 1-9-9c2.52 0 4.93 1 6.74 2.74L21 8', 'M21 3v5h-5')
    # zoom out: Lucide "minus" (as thin as plus)
    out['minus'] = line_icon('M5 12h14', width=1.5)
    return out


def toolbar_icons():
    """The 18 px toolbar buttons, white and filled like phototools_18.png (the photo tools: a camera with a
    sparkle), on an 18 unit grid so the edges fall on pixels."""
    out = {}
    # photo screen: a small filled camera inside the four corners of a frame (Lucide "scan" and "camera"):
    # "the whole window for pictures". The corners make it differ from the photo tools by shape alone.
    ic = Icon(R18)
    for d in ('M1 6V3a2 2 0 0 1 2-2h3', 'M12 1h3a2 2 0 0 1 2 2v3',
              'M17 12v3a2 2 0 0 1-2 2h-3', 'M6 17H3a2 2 0 0 1-2-2v-3'):
        ic.stroke(P(d), 2.0)
    ic.fill(rect(4, 6, 10, 8, 1)).fill(P('M6.5 6.5 7.75 4h2.5l1.25 2.5Z'))
    ic.erase(circle(9, 10, 2.3)).fill(circle(9, 10, 1.1))
    out['photoscreen'] = ic
    return out


def main():
    os.makedirs(OUT, exist_ok=True)
    save_alpha('ps_round.png', disc(64, 3.5))
    save_alpha('ps_circle.png', disc(128, 2.0))
    save_alpha('ps_cut.png', disc(64, 3.5, inverse=True))
    save_alpha('ps_shadow.png', shadow(64))
    made = icons()
    for name, ic in made.items():
        save_alpha('ps_icon_%s.png' % name, ic.alpha())
    bar = toolbar_icons()
    for name, ic in bar.items():
        save_alpha('%s_18.png' % name, ic.alpha())
    print('%d icons, %d toolbar buttons and 4 masks in %s' % (len(made), len(bar), OUT))


if __name__ == '__main__':
    main()
