"""Makes the UI's values and the Charis skin from the one table tokens.txt (research/239 (12), branch ui-skin).

    python -I make_ui_tokens.py           writes the files below
    python -I make_ui_tokens.py --check   writes nothing; exit 0 when the files are what tokens.txt makes,
                                          1 when one differs or tokens.txt has a mistake (each one is
                                          printed), 2 when it cannot tell (numpy / Pillow / matplotlib missing)

Made (relative to indra/newview; nothing of it is edited by hand):
  <this folder>/pstokens.h, pstokens.cpp     the C++ values of plus::psui::style (psstyle.h includes the .h)
  skins/charis/colors.xml                    the tokens' colours as Ps<Name> and the official names they change
  skins/charis/textures/textures.xml         the official image names the skin draws again
  skins/charis/textures/charis/*.png         those images (9 sliced: the corners keep their size)

The skin is laid over skins/default (lldir.cpp setSkinFolder: the setting SkinCurrent, "charis" by default):
colors.xml and textures.xml are read from every skin folder in turn, a later one replacing names of an earlier
one, so the official files are not touched and their updates still come in.

The title bar's icons are Lucide's (https://lucide.dev, ISC license; skins/charis/ORIGIN.md), drawn with the
rasterizer of make_ps_images.py. Needs numpy, matplotlib and Pillow (as make_ps_images.py does).
"""
import io
import os
import re
import sys
import xml.etree.ElementTree as ET

try:
    import numpy as np
    from PIL import Image
    IMAGING_MISSING = None
except ImportError as missing:
    np = Image = None
    IMAGING_MISSING = str(missing)

HERE = os.path.dirname(os.path.abspath(__file__))


def find_newview():
    """indra/newview above this folder (psui may move: research/239 (8) ui-core)."""
    d = HERE
    while True:
        if os.path.isfile(os.path.join(d, 'skins', 'default', 'colors.xml')):
            return d
        up = os.path.dirname(d)
        if up == d:
            raise SystemExit('make_ui_tokens.py: indra/newview (skins/default/colors.xml) not found above ' + HERE)
        d = up


NEWVIEW = find_newview()
TOKENS = os.path.join(HERE, 'tokens.txt')
OFFICIAL_COLORS = os.path.join(NEWVIEW, 'skins', 'default', 'colors.xml')
SKIN = os.path.join(NEWVIEW, 'skins', 'charis')
IMAGE_DIR = 'charis'        # under skins/charis/textures/ (viewer_manifest.py copies one folder level of it)

GENERATED_CPP = ('// Made by make_ui_tokens.py from tokens.txt (research/239 (12)). Do not edit: change tokens.txt\n'
                 '// and run python -I make_ui_tokens.py (tools/predeliver.py (u) fails while this differs).\n')
GENERATED_XML = ('<!-- Made by indra/newview/%s/make_ui_tokens.py from tokens.txt (research/239 (12)).\n'
                 '     Do not edit: change tokens.txt and run python -I make_ui_tokens.py. -->\n')

FONT_SIZES = ('Huge', 'Large', 'Medium', 'Small')

# ---------------------------------------------------------------- tokens.txt


class TokenError(Exception):
    pass


class Token:
    def __init__(self, name, kind, raw, note, line):
        self.name, self.kind, self.raw, self.note, self.line = name, kind, raw, note, line
        self.value = None       # colour: (0xRRGGBB, alpha); f32 / s32: a number; font: (size, bold)

    @property
    def xui(self):
        return 'Ps' + ''.join(w.capitalize() for w in self.name.split('_'))


def split_note(text):
    m = re.search(r'\s#(?:\s|$)', text)      # a note; '#0c111a' is a value
    if not m:
        return text.strip(), ''
    return text[:m.start()].strip(), text[m.end():].strip()


def parse_tokens(path):
    sections = []           # [(title, [Token | '## note'])]
    tokens = {}
    official = []           # [(kind 'official' | 'palette', name, raw, note, line)]
    keep = []               # name patterns
    with open(path, encoding='utf-8') as f:
        lines = f.read().split('\n')
    for n, line in enumerate(lines, 1):
        s = line.strip()
        if not s:
            continue
        if s.startswith('##'):
            if not sections:
                raise TokenError('%d: a note before the first section' % n)
            sections[-1][1].append(s[2:].strip())
            continue
        if s.startswith('#'):
            continue
        if s.startswith('['):
            if not s.endswith(']'):
                raise TokenError('%d: a section is written [title]' % n)
            sections.append((s[1:-1].strip(), []))
            continue
        body, note = split_note(s)
        parts = body.split()
        if parts[0] in ('official', 'palette'):
            if len(parts) != 3:
                raise TokenError('%d: %s <official name> =TOKEN[/alpha]' % (n, parts[0]))
            official.append((parts[0], parts[1], parts[2], note, n))
            continue
        if parts[0] == 'keep':
            if len(parts) != 2:
                raise TokenError('%d: keep <official name or prefix*>' % n)
            keep.append(parts[1])
            continue
        if len(parts) < 3:
            raise TokenError('%d: NAME TYPE VALUE' % n)
        name, kind = parts[0], parts[1]
        if not re.fullmatch(r'[A-Z][A-Z0-9_]*', name):
            raise TokenError('%d: a token name is UPPER_CASE: %s' % (n, name))
        if kind not in ('color', 'f32', 's32', 'font'):
            raise TokenError('%d: the type is color, f32, s32 or font: %s' % (n, kind))
        if name in tokens:
            raise TokenError('%d: %s twice (line %d)' % (n, name, tokens[name].line))
        if not sections:
            raise TokenError('%d: a token before the first section' % n)
        t = Token(name, kind, ' '.join(parts[2:]), note, n)
        tokens[name] = t
        sections[-1][1].append(t)
    for t in tokens.values():
        resolve(t, tokens, [])
    xui = {}
    for t in tokens.values():
        if t.kind == 'color':
            if t.xui in xui:
                raise TokenError('%d: %s and %s are both %s in colors.xml' % (t.line, t.name, xui[t.xui], t.xui))
            xui[t.xui] = t.name
    return sections, tokens, official, keep


def parse_color_ref(raw, tokens, line, chain):
    """'#rrggbb[/a]' or '=NAME[/a]' to (0xRRGGBB, alpha); for official lines too."""
    m = re.fullmatch(r'#([0-9a-fA-F]{6})(?:/([0-9.]+))?', raw)
    if m:
        return int(m.group(1), 16), alpha_of(m.group(2), line)
    m = re.fullmatch(r'=([A-Z][A-Z0-9_]*)(?:/([0-9.]+))?', raw)
    if not m:
        raise TokenError('%d: a colour is #rrggbb[/alpha] or =TOKEN[/alpha]: %s' % (line, raw))
    other = tokens.get(m.group(1))
    if other is None or other.kind != 'color':
        raise TokenError('%d: no colour token %s' % (line, m.group(1)))
    hexv, alpha = resolve(other, tokens, chain)
    return hexv, (alpha if m.group(2) is None else alpha_of(m.group(2), line))


def alpha_of(text, line):
    if text is None:
        return 1.0
    a = float(text)
    if not 0.0 <= a <= 1.0:
        raise TokenError('%d: the opacity is 0 .. 1: %s' % (line, text))
    return a


def resolve(t, tokens, chain):
    if t.value is not None:
        return t.value
    if t.name in chain:
        raise TokenError('%d: the tokens refer to each other: %s' % (t.line, ' -> '.join(chain + [t.name])))
    chain = chain + [t.name]
    if t.kind == 'color':
        t.value = parse_color_ref(t.raw, tokens, t.line, chain)
    elif t.kind in ('f32', 's32'):
        if t.raw.startswith('='):
            other = tokens.get(t.raw[1:])
            if other is None or other.kind not in ('f32', 's32'):
                raise TokenError('%d: no size token %s' % (t.line, t.raw[1:]))
            v = resolve(other, tokens, chain)
        else:
            try:
                v = float(t.raw)
            except ValueError:
                raise TokenError('%d: a size is a number or =TOKEN: %s' % (t.line, t.raw))
        if t.kind == 's32' and v != int(v):
            raise TokenError('%d: %s is s32, %s is not whole' % (t.line, t.name, t.raw))
        t.value = int(v) if t.kind == 's32' else float(v)
    else:
        words = t.raw.split()
        if words[0] not in FONT_SIZES or words[1:] not in ([], ['bold']):
            raise TokenError('%d: a font is %s [bold]: %s' % (t.line, '|'.join(FONT_SIZES), t.raw))
        t.value = (words[0], len(words) > 1)
    return t.value


def rgba(value):
    hexv, alpha = value
    return ((hexv >> 16) & 0xff) / 255.0, ((hexv >> 8) & 0xff) / 255.0, (hexv & 0xff) / 255.0, alpha

# ---------------------------------------------------------------- C++


def f32_text(v):
    if v == int(v):
        return '%d.f' % int(v)
    return repr(float(v)) + 'f'


def color_cpp(value):
    hexv, alpha = value
    if alpha == 1.0:
        return 'rgb(0x%06x)' % hexv
    return 'rgb(0x%06x, %s)' % (hexv, f32_text(alpha))


def heading(title, width=96):
    text = '// ---- %s ' % title
    return text + '-' * max(4, width - len(text))


def make_header(sections, tokens):
    colors = [t for t in tokens.values() if t.kind == 'color']
    width = max(len(t.name) for t in tokens.values())
    out = [GENERATED_CPP.rstrip('\n'), '//',
           '// The values of the photo screen\'s look and of every Charis window (research/221, 239). The widgets',
           '// read only these. The colours are references into detail::colors: its defaults are tokens.txt\'s,',
           '// and applyColorTable() (at the start, psui\'s feature init) puts in what the colour table has under',
           '// the Ps* names (skins/charis/colors.xml, made from the same table, and a user\'s colors.xml), so a',
           '// later colour theme (research/239 (12) 2) changes the table and calls it again. Included by psstyle.h',
           '// (after rgb() and FontSpec); not on its own.', '',
           '#ifndef PLUS_PSTOKENS_H', '#define PLUS_PSTOKENS_H', '',
           '#ifndef PLUS_PSSTYLE_H', '#error "include psstyle.h, not pstokens.h"', '#endif', '',
           'namespace plus::psui::style', '{',
           '    namespace detail', '    {',
           '        struct Colors', '        {']
    for t in colors:
        out.append('            LLColor4 %-*s = %s;' % (width, t.name, color_cpp(t.value)))
    out += ['        };', '        inline Colors colors;', '    }', '']
    for title, items in sections:
        out.append('    ' + heading(title, 92))
        for item in items:
            if isinstance(item, str):
                out.append('    // ' + item)
                continue
            t = item
            if t.kind == 'color':
                line = 'inline const LLColor4& %-*s = detail::colors.%s;' % (width, t.name, t.name)
            elif t.kind == 'f32':
                line = 'constexpr F32 %-*s = %s;' % (width + 10, t.name, f32_text(t.value))
            elif t.kind == 's32':
                line = 'constexpr S32 %-*s = %d;' % (width + 10, t.name, t.value)
            else:
                size, bold = t.value
                line = 'constexpr FontSpec %-*s = { %-9s %s };' % (width + 5, t.name, '"%s",' % size,
                                                                   'LLFontGL::BOLD' if bold else '0')
            if t.note:
                line += '   // ' + t.note
            out.append('    ' + line)
        out.append('')
    out += ['    // Puts the colour table\'s Ps* colours into detail::colors (those it has; the others keep',
            '    // tokens.txt\'s). Once at the start; again after the table changes.',
            '    void applyColorTable();', '}', '', '#endif // PLUS_PSTOKENS_H', '']
    return '\n'.join(out)


def make_cpp(tokens):
    colors = [t for t in tokens.values() if t.kind == 'color']
    width = max(len(t.xui) for t in colors) + 3
    out = [GENERATED_CPP.rstrip('\n'), '', '#include "llviewerprecompiledheaders.h"', '',
           '#include "psstyle.h"', '', '#include "lluicolortable.h"', '',
           'namespace plus::psui::style', '{', '    namespace', '    {',
           '        struct Entry', '        {', '            const char* name;',
           '            LLColor4 detail::Colors::* value;', '        };', '',
           '        const Entry ENTRIES[] = {']
    for t in colors:
        out.append('            { %-*s &detail::Colors::%s },' % (width, '"%s",' % t.xui, t.name))
    out += ['        };', '    }', '',
            '    void applyColorTable()', '    {',
            '        const LLUIColorTable& table = LLUIColorTable::instance();',
            '        for (const Entry& entry : ENTRIES)', '        {',
            '            if (table.colorExists(entry.name))', '            {',
            '                detail::colors.*entry.value = table.getColor(entry.name).get();',
            '            }', '        }', '    }', '}', '']
    return '\n'.join(out)

# ---------------------------------------------------------------- colors.xml


def num(v):
    s = '%.4f' % v
    s = s.rstrip('0').rstrip('.')
    return s if s not in ('', '-0') else '0'


def color_value(value):
    return ' '.join(num(c) for c in rgba(value))


def read_official_colors():
    root = ET.parse(OFFICIAL_COLORS).getroot()
    out = {}
    order = []
    for c in root.iter('color'):
        name = c.get('name')
        if name in out:
            continue
        out[name] = (c.get('value'), c.get('reference'))
        order.append(name)
    return out, order


def resolve_official(name, defs, seen=()):
    """An official colour's value as the default skin leaves it ('r g b a' text), or None."""
    if name not in defs or name in seen:
        return None
    value, ref = defs[name]
    if value is not None:
        return value
    return resolve_official(ref, defs, seen + (name,))


def matches(name, patterns):
    for p in patterns:
        if p.startswith('*') and name.endswith(p[1:]):
            return True
        if p.endswith('*') and name.startswith(p[:-1]):
            return True
        if name == p:
            return True
    return False


def make_colors_xml(tokens, official, keep):
    defs, order = read_official_colors()
    overridden = {}
    palette = set()
    for kind, name, raw, note, line in official:
        if name not in defs:
            raise TokenError('%d: the official colors.xml has no %s' % (line, name))
        if name in overridden:
            raise TokenError('%d: %s twice' % (line, name))
        overridden[name] = (raw, note, line)
        if kind == 'palette':
            palette.add(name)
    if any(t.xui in defs for t in tokens.values() if t.kind == 'color'):
        raise TokenError('a Ps* name is an official colour name: %s'
                         % ', '.join(t.xui for t in tokens.values() if t.kind == 'color' and t.xui in defs))

    def follows_palette(name, seen=()):
        """True when the official name's reference chain reaches a palette name before a value."""
        value, ref = defs[name]
        if value is not None or ref not in defs or ref in seen:
            return False
        return ref in palette or (ref not in overridden and follows_palette(ref, seen + (name,)))

    out = ['<?xml version="1.0" encoding="utf-8" standalone="yes" ?>',
           GENERATED_XML.rstrip('\n') % os.path.relpath(HERE, NEWVIEW).replace('\\', '/'), '<colors>', '',
           '  <!-- The tokens (pstokens.h has the same values): the XUI windows use these names -->']
    for t in tokens.values():
        if t.kind == 'color':
            out.append('  <color name="%s" value="%s" />' % (t.xui, color_value(t.value)))
    out += ['', '  <!-- Official names the Charis look changes -->']
    for kind, name, raw, note, line in official:
        m = re.fullmatch(r'=([A-Z][A-Z0-9_]*)', raw)
        if m and m.group(1) in tokens:
            out.append('  <color name="%s" reference="%s" />' % (name, tokens[m.group(1)].xui))
        else:
            value = parse_color_ref(raw, tokens, line, [])
            out.append('  <color name="%s" value="%s" />  <!-- %s -->' % (name, color_value(value), raw.lstrip('=')))
    follow = []
    kept = []
    for name in order:
        if name in overridden or not follows_palette(name):
            continue
        if matches(name, keep):
            value = resolve_official(name, defs)
            if value is not None:
                kept.append('  <color name="%s" value="%s" />' % (name, value))
        else:
            follow.append('  <color name="%s" reference="%s" />' % (name, defs[name][1]))
    if follow:
        out += ['', '  <!-- Official names that refer to a palette name above: read again here so they follow it -->']
        out += follow
    if kept:
        out += ['', '  <!-- Official names that would follow a palette name but stay as the official skin has them -->']
        out += kept
    out += ['', '</colors>', '']
    return '\n'.join(out)

# ---------------------------------------------------------------- images


def need_imaging():
    """make_ps_images (its icon rasterizer; it draws nothing on import), or exit 2 without numpy / Pillow /
    matplotlib."""
    if IMAGING_MISSING is None:
        sys.path.insert(0, HERE)
        try:
            import make_ps_images
            return make_ps_images
        except ImportError as ex:
            missing = str(ex)
    else:
        missing = IMAGING_MISSING
    print('make_ui_tokens.py needs numpy, matplotlib and Pillow: %s' % missing)
    raise SystemExit(2)


def rect_cov(w, h, rect, radius):
    """Coverage (0..1) in a w x h image of the rounded rectangle rect = (x0, y0, x1, y1) (pixels, y down; it
    may reach outside the image), anti-aliased by its distance."""
    x0, y0, x1, y1 = rect
    half_w, half_h = (x1 - x0) / 2.0, (y1 - y0) / 2.0
    ys, xs = np.mgrid[0:h, 0:w]
    r = max(0.0, min(radius, half_w, half_h))
    qx = np.abs(xs + 0.5 - (x0 + x1) / 2.0) - (half_w - r)
    qy = np.abs(ys + 0.5 - (y0 + y1) / 2.0) - (half_h - r)
    d = np.hypot(np.maximum(qx, 0.0), np.maximum(qy, 0.0)) + np.minimum(np.maximum(qx, qy), 0.0) - r
    return np.clip(0.5 - d, 0.0, 1.0)


def round_rect(w, h, half_w, half_h, radius):
    """Coverage (0..1) of a rounded rectangle centred in a w x h image."""
    return rect_cov(w, h, (w / 2.0 - half_w, h / 2.0 - half_h, w / 2.0 + half_w, h / 2.0 + half_h), radius)


def coloured(coverage, color):
    """An RGBA image of color where coverage says (its opacity times the coverage)."""
    rgb = [np.full(coverage.shape, color[i]) for i in range(3)]
    return np.stack(rgb + [coverage * color[3]], axis=-1)


def towards(c, target, t):
    return (c[0] + (target - c[0]) * t, c[1] + (target - c[1]) * t, c[2] + (target - c[2]) * t, c[3])


def slices(size, fixed):
    """textures.xml's 9 slices of a size x size image with fixed pixels on each side: left, top, right, bottom
    (top and bottom counted from the bottom)."""
    return fixed, size - fixed, size - fixed, fixed


def panel_image(size, radius, top, bottom, border, fixed, opacity=1.0):
    """A rounded box with a 1 px border: top colour in the fixed rows above, bottom below, a gradient between
    (the stretched middle of the 9 slices, so it runs over the whole height drawn)."""
    half = size / 2.0
    outer = round_rect(size, size, half, half, radius)
    inner = round_rect(size, size, half - 1.0, half - 1.0, radius - 1.0)
    t = np.clip((np.arange(size) + 0.5 - fixed) / max(1.0, size - 2.0 * fixed), 0.0, 1.0)[:, None]
    fill = [top[i] + (bottom[i] - top[i]) * t for i in range(4)]
    a_fill = fill[3] * inner
    a_border = border[3] * (outer - inner)
    alpha = a_fill + a_border
    rgb = [(fill[i] * a_fill + border[i] * a_border) / np.maximum(alpha, 1e-6) for i in range(3)]
    return np.stack(rgb + [alpha * opacity], axis=-1)


def bar_image(w, h, thickness, radius, color, vertical):
    """A rounded bar thickness pixels wide (vertical) or high, centred, running the image's length."""
    half_w, half_h = (thickness / 2.0, h / 2.0) if vertical else (w / 2.0, thickness / 2.0)
    return coloured(round_rect(w, h, half_w, half_h, radius), color)


def icon_image(mpi, paths, size, color, width=2.0):
    """A Lucide icon (24 unit grid, round lines) drawn on size pixels in color."""
    ic = mpi.Icon(mpi.Raster(size, mpi.GRID))
    for p in paths:
        ic.stroke(mpi.P(p) if isinstance(p, str) else p, width)
    return coloured(ic.alpha(), color)


# The title bar's icons (Lucide: x, minus, copy, arrow-down-to-line, circle-help).
TITLE_ICONS = {
    'close':    ['M18 6 6 18', 'm6 6 12 12'],
    'minimize': ['M5 12h14'],
    'restore':  ['M10 8h10a2 2 0 0 1 2 2v10a2 2 0 0 1-2 2H10a2 2 0 0 1-2-2V10a2 2 0 0 1 2-2z',
                 'M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2'],
    'dock':     ['M12 17V3', 'm6 11 6 6 6-6', 'M19 21H5'],
    'help':     ['circle', 'M9.09 9a3 3 0 0 1 5.83 1c0 2-3 3-3 3', 'M12 17h.01'],
}


def make_images(tokens, mpi):
    """{file name: RGBA float array} and the textures.xml entries [(official name, file, slices or None)]."""

    def c(name):
        return rgba(tokens[name].value)

    def v(name):
        return tokens[name].value

    images = {}
    entries = []
    # windows, toasts, inspectors: the panel's gradient, round corners, 1 px border; no title band (the
    # official Window_* had a lighter title band; the panels' headings stand on the gradient)
    radius = v('WINDOW_RADIUS')
    fixed = int(radius) + 2
    size = 4 * fixed + 8
    images['window.png'] = panel_image(size, radius, c('WINDOW_TOP'), c('WINDOW_BOTTOM'), c('WINDOW_BORDER'), fixed)
    images['window_focus.png'] = panel_image(size, radius, c('WINDOW_TOP'), c('WINDOW_BOTTOM'),
                                             c('WINDOW_BORDER_FOCUS'), fixed)
    # Volume_Background: the box of the status bar's hover pull-downs (volume, nearby media, graphic and camera
    # presets); the official one was a grey rounded rectangle
    for name in ('Window_Background', 'Window_NoTitle_Background', 'Toast_Background', 'Inspector_Background',
                 'Volume_Background'):
        entries.append((name, 'window.png', slices(size, fixed)))
    for name in ('Window_Foreground', 'Window_NoTitle_Foreground', 'Toast_Over'):
        entries.append((name, 'window_focus.png', slices(size, fixed)))
    # tooltips
    radius = v('TOOLTIP_RADIUS')
    fixed = int(radius) + 2
    size = 4 * fixed
    images['tooltip.png'] = panel_image(size, radius, c('TOOLTIP_FILL'), c('TOOLTIP_FILL'), c('TOOLTIP_BORDER'), fixed)
    entries.append(('Tooltip', 'tooltip.png', slices(size, fixed)))
    # official buttons: psui's button (16.) off, the segments' blue (14.) on; the states of tokens' [states]
    radius = v('BUTTON_RADIUS')
    fixed = int(radius) + 1
    size = 4 * fixed
    off = (c('BUTTON_FILL'), c('BUTTON_FILL'), c('BUTTON_BORDER'))
    on = (c('BUTTON_ON_TOP'), c('BUTTON_ON_BOTTOM'), c('BUTTON_ON_BORDER'))
    press = v('STATE_PRESS')
    disabled = v('STATE_DISABLED')

    def button(colors, opacity=1.0, pressed=False):
        top, bottom, border = colors
        if pressed:
            top, bottom = towards(top, 0.0, press), towards(bottom, 0.0, press)
        return panel_image(size, radius, top, bottom, border, fixed, opacity)

    images['button_off.png'] = button(off)
    images['button_over.png'] = button((c('BUTTON_FILL_HOVER'), c('BUTTON_FILL_HOVER'), c('BUTTON_BORDER_HOVER')))
    images['button_disabled.png'] = button(off, disabled)
    images['button_on.png'] = button(on)
    images['button_on_press.png'] = button(on, pressed=True)
    images['button_on_disabled.png'] = button(on, disabled)
    for name, file in (('PushButton_Off', 'button_off.png'), ('PushButton_Over', 'button_over.png'),
                       ('PushButton_Disabled', 'button_disabled.png'), ('PushButton_Selected', 'button_on.png'),
                       ('PushButton_On', 'button_on.png'), ('PushButton_On_Selected', 'button_on_press.png'),
                       ('PushButton_Selected_Press', 'button_on_press.png'),
                       ('PushButton_Selected_Disabled', 'button_on_disabled.png')):
        entries.append((name, file, slices(size, fixed)))
    # scroll bars (the official bar is 15 px thick): a thin rounded thumb over a groove of the same width
    thick, sr = v('SCROLL_WIDTH'), v('SCROLL_RADIUS')
    across, along = 15, 32
    end = int(sr) + 2
    images['scroll_thumb_v.png'] = bar_image(across, along, thick, sr, c('SCROLL_THUMB'), True)
    images['scroll_track_v.png'] = bar_image(across, along, thick, sr, c('SCROLL_TRACK'), True)
    images['scroll_thumb_h.png'] = bar_image(along, across, thick, sr, c('SCROLL_THUMB'), False)
    images['scroll_track_h.png'] = bar_image(along, across, thick, sr, c('SCROLL_TRACK'), False)
    entries.append(('ScrollThumb_Vert', 'scroll_thumb_v.png', (0, along - end, across, end)))
    entries.append(('ScrollTrack_Vert', 'scroll_track_v.png', (0, along - end, across, end)))
    entries.append(('ScrollThumb_Horiz', 'scroll_thumb_h.png', (end, across, along - end, 0)))
    entries.append(('ScrollTrack_Horiz', 'scroll_track_h.png', (end, across, along - end, 0)))
    # the title bar's buttons: line icons, light; pressed in the accent's light line
    isize = v('TITLE_ICON_SIZE')
    official = {'close': 'Close', 'minimize': 'Minimize', 'restore': 'Restore', 'dock': 'Dock', 'help': 'Help'}
    for key, paths in TITLE_ICONS.items():
        paths = [mpi.circle(12, 12, 10) if p == 'circle' else p for p in paths]
        images['title_%s.png' % key] = icon_image(mpi, paths, isize, c('TITLE_ICON'))
        images['title_%s_press.png' % key] = icon_image(mpi, paths, isize, c('TITLE_ICON_PRESS'))
        entries.append(('Icon_%s_Foreground' % official[key], 'title_%s.png' % key, None))
        entries.append(('Icon_%s_Press' % official[key], 'title_%s_press.png' % key, None))
    old_parts(tokens, mpi, images, entries)
    check_stretched_bands(images, entries)
    return images, entries


# 9 sliced images whose stretched band holds a drawing on purpose: the official image has it at the same place,
# and the part is mostly drawn at about the image's own size along that direction: the arrow of a drop-down you
# type in (as wide as the image), the scroll bar's arrow buttons (15 px square), the arrow of a drop-down and of
# the "Received items" bar (in the fixed right / left columns, the stretched rows: the official arrow too; most
# drop-downs are 22-23 high, the bar 35 as its image), and the badge's back (the official image's own outline)
STRETCH_DRAWN = {
    'combo_arrow_off.png': 'both', 'combo_arrow_open.png': 'both', 'combo_arrow_disabled.png': 'both',
    'combo_up_off.png': 'both', 'combo_up_press.png': 'both', 'combo_on.png': 'both',
    'scroll_arrow_up.png': 'both', 'scroll_arrow_down.png': 'both', 'scroll_arrow_left.png': 'both',
    'scroll_arrow_right.png': 'both',
    'dropdown_off.png': 'rows', 'dropdown_open.png': 'rows', 'dropdown_press.png': 'rows',
    'dropdown_disabled.png': 'rows', 'marketplace_closed.png': 'rows', 'marketplace_open.png': 'rows',
    'shape_badge_background.png': 'columns',
}
# The most two neighbouring lines of a stretched band (and a band's edge and the fixed line next to it) may
# differ, premultiplied: the gradients here change at most about 0.011 a line (button_on.png); the faintest
# line drawn is 0.028 (a disabled box's border at 40%; SEG_OFF beside SEG_BORDER_DIM at full is 0.07)
STRETCH_STEP = 0.02


def check_stretched_bands(images, entries):
    """Every 9 sliced image: the columns and rows its slices stretch must change smoothly (a gradient may run
    through them; a border or a mark there would be drawn as a wide blur), and so must a band's edge and the
    fixed line next to it (the drawing filters across that seam, so a border right at the band is smeared
    over a few pixels as the band stretches: keep a line of the fill on the fixed side). Bands as wide as the
    image (drawn at the image's size) are not looked at; STRETCH_DRAWN names the drawings meant to be there and
    the direction they are allowed in."""
    bad = []
    for entry in entries:
        name, file, scale = entry[:3]
        if not scale or file is None or file not in images:
            continue
        allowed = STRETCH_DRAWN.get(file, '')
        arr = images[file]
        h, w = arr.shape[:2]
        pre = np.concatenate([arr[..., :3] * arr[..., 3:], arr[..., 3:]], axis=-1)    # premultiplied
        left, top, right, bottom = scale
        x0, x1 = left, right                     # the stretched columns [x0, x1)
        y0, y1 = h - top, h - bottom             # the stretched rows (top and bottom count from the bottom)
        cols_ok = 0 < x1 - x0 < w
        rows_ok = 0 < y1 - y0 < h
        # the seams are looked at along the other band only: the fixed corner cells (where the corners are) are
        # not looked at
        seam_rows = slice(y0, y1) if rows_ok else slice(0, h)
        seam_cols = slice(x0, x1) if cols_ok else slice(0, w)

        def step(a, axis):
            return np.abs(np.diff(a, axis=axis)).max(initial=0.0)

        if allowed not in ('both', 'columns') and cols_ok:
            inside = step(pre[:, x0:x1], 1)
            seams = max(step(pre[seam_rows, max(0, x0 - 1):x0 + 1], 1), step(pre[seam_rows, x1 - 1:min(w, x1 + 1)], 1))
            if max(inside, seams) > STRETCH_STEP:
                bad.append('%s (%s): a line %s the stretched columns %d..%d'
                           % (name, file, 'in' if inside > STRETCH_STEP else 'at the edge of', x0, x1 - 1))
        if allowed not in ('both', 'rows') and rows_ok:
            inside = step(pre[y0:y1], 0)
            seams = max(step(pre[max(0, y0 - 1):y0 + 1, seam_cols], 0), step(pre[y1 - 1:min(h, y1 + 1), seam_cols], 0))
            if max(inside, seams) > STRETCH_STEP:
                bad.append('%s (%s): a line %s the stretched rows %d..%d'
                           % (name, file, 'in' if inside > STRETCH_STEP else 'at the edge of', y0, y1 - 1))
    if bad:
        raise TokenError('9 slices: ' + '; '.join(bad))

# ---------------------------------------------------------------- the official small parts


OFFICIAL_TEXTURES = os.path.join(NEWVIEW, 'skins', 'default', 'textures')
OLD_LINE = 1.5      # px: the chevrons' and the check mark's line on the official small parts (their size)


OFFICIAL_CHANGED = ('the official skins/default/textures/textures.xml (or its image) changed: look at old_parts in '
                    'make_ui_tokens.py again (%s)')

# The official image families old_parts draws again: every official name with one of these prefixes must be drawn
# again or named in OLD_PARTS_LEFT, so a state the official skin adds later (DropDown_Over ...) does not stay grey
# without a word (python -I make_ui_tokens.py --check fails on it).
OLD_PARTS_FAMILIES = ('DropDown_', 'ComboButton_', 'TextField_', 'Checkbox_', 'RadioButton_', 'SliderThumb_',
                      'SliderTrack_', 'Stepper_', 'ScrollArrow_', 'TabTop_', 'TabLeft_', 'ListItem_', 'Accordion_',
                      'MarketplaceBtn_', 'SegmentedBtn_', 'Toolbar_', 'Flyout_', 'Parcel_', 'Pathfinding_')
OLD_PARTS_LEFT_PATTERNS = (
    (r'Parcel_\w+_Dark', 'drawn for a dark ground already'),
)
OLD_PARTS_LEFT = {
    'ComboButton_Transparent': 'empty on purpose (the login screen)',
    'TextField_Transparent': 'empty on purpose (the login screen)',
    'Checkbox_Slim_Off': 'the login screen: a white outline on its own dark back',
    'Checkbox_Slim_On': 'the login screen: a white outline on its own dark back',
    'TabTop_Left_Off': 'not used by the viewer (the tabs use the _Flat_ ones)',
    'TabTop_Left_Selected': 'not used by the viewer',
    'TabTop_Middle_Off': 'not used by the viewer',
    'TabTop_Middle_Selected': 'not used by the viewer',
    'TabTop_Right_Off': 'not used by the viewer',
    'TabTop_Right_Selected': 'not used by the viewer',
    'Parcel_PG_Light': 'the coloured maturity badge (blue): seen on the dark field as before (about 3:1)',
    'Parcel_M_Light': 'the coloured maturity badge: seen on the dark field as before (about 3:1)',
    'Parcel_R_Light': 'the coloured maturity badge (red): seen on the dark field as before',
    'Parcel_Exp_Color': 'a coloured experience badge, not for a light or a dark ground',
    'Parcel_ScriptsNo_Light': 'the official skin points it at Parcel_ScriptsNo_Dark.png already',
    'Parcel_SeeAVsOn_Light': 'not used by the viewer',
    'Parcel_Voice_Light': 'not used by the viewer',
}

# The official images whose size the drawing below takes as given (the check mark's box, the radio's circle ...):
# when the official skin changes one, the drawing has to be looked at again.
OLD_PARTS_SIZES = {
    'DropDown_Off': (122, 23), 'ComboButton_Off': (22, 23), 'ComboButton_Arrow': (7, 6), 'TextField_Off': (256, 23),
    'Search': (13, 13), 'Checkbox_Off': (14, 15), 'RadioButton_Off': (14, 14), 'SliderThumb_Off': (14, 14),
    'Stepper_Up_Off': (16, 11), 'ScrollArrow_Up': (15, 15), 'MarketplaceBtn_Off': (40, 35),
    'Accordion_ArrowClosed_Off': (10, 10),
}


def read_official_textures():
    """{official image name: (file, scale (left, top, right, bottom) or None, preload)} of skins/default."""
    out = {}
    try:
        for t in ET.parse(os.path.join(OFFICIAL_TEXTURES, 'textures.xml')).getroot().iter('texture'):
            keys = ('scale.left', 'scale.top', 'scale.right', 'scale.bottom')
            scale = tuple(int(t.get(k)) for k in keys) if all(t.get(k) is not None for k in keys) else None
            out[t.get('name')] = (t.get('file_name'), scale, t.get('preload', 'false') == 'true')
    except (OSError, ET.ParseError, ValueError) as ex:
        raise TokenError(OFFICIAL_CHANGED % ex)
    return out


def fixed_sides(w, h, scale):
    """The pixels the 9 slices keep at their size: left, top, right, bottom (none without slices)."""
    if scale is None:
        return 0, 0, 0, 0
    left, top, right, bottom = scale
    return left, h - top, w - right, bottom


def box(w, h, rect, radius, fill, border=None, opacity=1.0, across=False):
    """A rounded box in a w x h image: fill is a colour or (first, last), a gradient down rect (across: from
    its left to its right); border a 1 px line inside its edge, or None."""
    x0, y0, x1, y1 = rect
    outer = rect_cov(w, h, rect, radius)
    inner = rect_cov(w, h, (x0 + 1, y0 + 1, x1 - 1, y1 - 1), radius - 1.0) if border else outer
    first, last = fill if isinstance(fill[0], tuple) else (fill, fill)
    if across:
        t = np.clip((np.arange(w) + 0.5 - x0) / max(1.0, x1 - x0), 0.0, 1.0)[None, :]
    else:
        t = np.clip((np.arange(h) + 0.5 - y0) / max(1.0, y1 - y0), 0.0, 1.0)[:, None]
    color = [np.broadcast_to(first[i] + (last[i] - first[i]) * t, (h, w)) for i in range(4)]
    a_fill = color[3] * inner
    a_border = border[3] * (outer - inner) if border else np.zeros((h, w))
    alpha = a_fill + a_border
    edge = border or (0.0, 0.0, 0.0, 0.0)
    rgb = [(color[i] * a_fill + edge[i] * a_border) / np.maximum(alpha, 1e-6) for i in range(3)]
    return np.stack(rgb + [alpha * opacity], axis=-1)


def over(under, top):
    """top drawn over under (both RGBA, not premultiplied)."""
    a = top[..., 3:] + under[..., 3:] * (1.0 - top[..., 3:])
    rgb = (top[..., :3] * top[..., 3:] + under[..., :3] * under[..., 3:] * (1.0 - top[..., 3:])) / np.maximum(a, 1e-6)
    return np.concatenate([rgb, a], axis=-1)


def faded(arr, opacity):
    out = arr.copy()
    out[..., 3] *= opacity
    return out


def mixed(under, top):
    """The colour of top (with its opacity) over the opaque colour under."""
    a = top[3]
    return tuple(top[i] * a + under[i] * (1.0 - a) for i in range(3)) + (under[3],)


def lines(mpi, w, h, subs, color, width=OLD_LINE):
    """Round lines (pixels, y down) drawn in a w x h image."""
    n = max(w, h)
    ic = mpi.Icon(mpi.Raster(n, float(n)))
    ic.stroke(subs, width)
    return coloured(ic.alpha()[:h, :w], color)


def chevron(cx, cy, width, depth, pointing):
    """A chevron centred on (cx, cy): width across, depth along where it points (down, up, left, right)."""
    a, b = width / 2.0, depth / 2.0
    pts = {'down': [(cx - a, cy - b), (cx, cy + b), (cx + a, cy - b)],
           'up': [(cx - a, cy + b), (cx, cy - b), (cx + a, cy + b)],
           'left': [(cx + b, cy - a), (cx - b, cy), (cx + b, cy + a)],
           'right': [(cx - b, cy - a), (cx + b, cy), (cx - b, cy + a)]}[pointing]
    return [(pts, False)]


def old_parts(tokens, mpi, images, entries):
    """The official small parts in the colours of psui's parts (research/239, branch ui-old-recolor): drop-downs,
    text and search fields, check boxes, radio buttons, sliders, spinner and scroll bar arrows, tabs, list rows,
    accordion headers and arrows, the inventory's "Received items" bar, segmented buttons, the floaters' bottom-bar
    buttons and bottom tabs, a docked floater's tongue, an inspector's back, the badges, and the location bar's
    parcel icons. Only the colours change: every image is as big as the official one and keeps its 9 slices
    (scale.*) and preload, read from skins/default/textures/textures.xml, so the official windows keep their
    layout; two kinds keep wider fixed sides than the official (Toolbar_*, ListItem_Select: a border or a bar
    alone in a 1 or 2 px fixed side is smeared into the band; the image and where it is drawn do not change).
    Their words are in the official colour names of tokens.txt (TextFgColor ...)."""
    official = read_official_textures()
    done = set()

    def c(name):
        return rgba(tokens[name].value)

    def v(name):
        return tokens[name].value

    disabled = v('STATE_DISABLED')
    press = v('STATE_PRESS')

    def info(name):
        if name not in official or not official[name][0]:
            raise TokenError(OFFICIAL_CHANGED % ('no image %s' % name))
        return official[name]

    def size_of(name):
        try:
            with Image.open(os.path.join(OFFICIAL_TEXTURES, info(name)[0])) as im:
                size = im.size
        except OSError as ex:
            raise TokenError(OFFICIAL_CHANGED % ex)
        if name in OLD_PARTS_SIZES and size != OLD_PARTS_SIZES[name]:
            raise TokenError(OFFICIAL_CHANGED % ('%s is %dx%d, the drawing takes %dx%d'
                                                 % ((name,) + size + OLD_PARTS_SIZES[name])))
        return size

    def official_pixels(name):
        try:
            with Image.open(os.path.join(OFFICIAL_TEXTURES, info(name)[0])) as im:
                return np.asarray(im.convert('RGBA')).astype(float) / 255.0
        except OSError as ex:
            raise TokenError(OFFICIAL_CHANGED % ex)

    def radius_for(name, radius):
        """radius, but no more than a fixed side less 1 px, so a corner stays in what the 9 slices keep fixed
        (a box stands up to 1 px in from the image's edge; on such a side the arc's last column or row, about
        0.05 off the fill, comes next to the band: check_stretched_bands does not look at the corner cells)."""
        w, h = size_of(name)
        sides = [s - 1 for s in fixed_sides(w, h, info(name)[1]) if s > 0]
        return max(0, min([radius] + sides))

    def add(file, arr, *names, scale_of=None):
        """The image file for these official names (each as big as the official image). scale_of(name, scale)
        may change the official 9 slices (only where the official ones put a border at the band's edge)."""
        for name in names:
            w, h = size_of(name)
            if arr.shape[:2] != (h, w):
                raise TokenError('%s is %dx%d, the official %s is %dx%d'
                                 % (file, arr.shape[1], arr.shape[0], name, w, h))
            _, scale, preload = info(name)
            if scale_of:
                scale = scale_of(name, scale)
            entries.append((name, file, scale, preload))
            done.add(name)
        images[file] = arr

    # drop-downs (the closed box of a combo_box: psui's combo, 7.) and the arrow of one you type in
    w, h = size_of('DropDown_Off')
    r = radius_for('DropDown_Off', v('VALUE_RADIUS'))
    rect = (1, 0, w - 1, h - 1)             # the official box leaves a line at the sides and the bottom
    arrow = chevron(w - 12.0, (h - 1) / 2.0, 8.0, 4.0, 'down')
    fill, border = c('COMBO_FILL'), c('COMBO_BORDER')

    def drop(fill, border, arrow_color, w, h, rect, r, arrow):
        return over(box(w, h, rect, r, fill, border), lines(mpi, w, h, arrow, arrow_color))

    off = drop(fill, border, c('ICON_LIGHT'), w, h, rect, r, arrow)
    add('dropdown_off.png', off, 'DropDown_Off')
    add('dropdown_open.png', drop(fill, c('ACCENT'), c('ACCENT_LINE'), w, h, rect, r, arrow), 'DropDown_On')
    add('dropdown_press.png', drop(towards(fill, 0.0, press), border, c('ICON_LIGHT'), w, h, rect, r, arrow),
        'DropDown_Press')
    add('dropdown_disabled.png', faded(off, disabled), 'DropDown_Disabled')

    w, h = size_of('ComboButton_Off')
    r = radius_for('ComboButton_Off', v('VALUE_RADIUS'))
    rect = (0, 0, w, h - 1)
    down = chevron(w / 2.0, (h - 1) / 2.0, 8.0, 4.0, 'down')
    up = chevron(w / 2.0, (h - 1) / 2.0, 8.0, 4.0, 'up')
    off = drop(fill, border, c('ICON_LIGHT'), w, h, rect, r, down)
    add('combo_arrow_off.png', off, 'ComboButton_Off')
    add('combo_arrow_open.png', drop(fill, c('ACCENT'), c('ACCENT_LINE'), w, h, rect, r, down), 'ComboButton_Selected')
    add('combo_arrow_disabled.png', faded(off, disabled), 'ComboButton_Disabled')
    # the same box with the arrow up (the chat bar's and the places' "more" buttons); on: blue as a button on
    add('combo_up_off.png', drop(fill, border, c('ICON_LIGHT'), w, h, rect, r, up), 'ComboButton_UpOff')
    add('combo_up_press.png', drop(towards(fill, 0.0, press), border, c('ICON_LIGHT'), w, h, rect, r, up),
        'ComboButton_UpSelected')
    add('combo_on.png', drop((c('BUTTON_ON_TOP'), c('BUTTON_ON_BOTTOM')), c('BUTTON_ON_BORDER'), c('TEXT_WHITE'),
                             w, h, rect, r, down), 'ComboButton_On')
    # the bare arrow (the login screen's boxes)
    w, h = size_of('ComboButton_Arrow')
    add('combo_bare_arrow.png', lines(mpi, w, h, chevron(w / 2.0, h / 2.0, 5.0, 2.5, 'down'), c('ICON_LIGHT')),
        'ComboButton_Arrow')

    # text fields (line_editor, and the search boxes): psui's box to type in; the accent while typed in
    w, h = size_of('TextField_Off')
    r = radius_for('TextField_Off', v('VALUE_RADIUS'))
    rect = (0, 0, w, h - 1)
    field = box(w, h, rect, r, c('FIELD_BG'), c('FIELD_BORDER'))
    add('field_off.png', field, 'TextField_Off', 'TextField_Search_Off')
    add('field_focus.png', box(w, h, rect, r, c('FIELD_BG'), c('ACCENT')), 'TextField_Active', 'TextField_Search_Active')
    add('field_disabled.png', faded(field, disabled), 'TextField_Disabled', 'TextField_Search_Disabled')
    add('field_found.png', box(w, h, rect, r, mixed(c('FIELD_BG'), c('SEARCH_HIT_FILL')), c('SEARCH_HIT_RING')),
        'TextField_Highlight', 'TextField_Search_Highlight')
    # the magnifier in the search boxes (Lucide "search"): light on the dark box
    w, h = size_of('Search')
    add('search.png', icon_image(mpi, [mpi.circle(11, 11, 8), 'm21 21-4.3-4.3'], w, c('TEXT_LABEL'), 2.4), 'Search')

    # check boxes (psui's, 4., at the official size: the box 14 px, the mark as big in it as psui's)
    w, h = size_of('Checkbox_Off')
    side = 14.0
    k = side / v('CHECK_BOX')
    rect = (0, 0, side, side)
    r = v('CHECK_RADIUS') * k
    m = v('CHECK_MARK') * k / 24.0                  # Lucide "check" (24 unit grid) to pixels
    mark = [([(side / 2.0 + (x - 12.0) * m, side / 2.0 + (y - 11.5) * m) for x, y in ((20, 6), (9, 17), (4, 12))],
             False)]

    def check(fill, border=None, marked=False):
        out = box(w, h, rect, r, fill, border)
        return over(out, lines(mpi, w, h, mark, c('CHECK_MARK_COLOR'))) if marked else out

    off = check(c('CHECK_OFF'), c('CHECK_BORDER'))
    on = check(c('CHECK_ON'), marked=True)
    add('check_off.png', off, 'Checkbox_Off')
    add('check_off_press.png', check(c('CHECK_OFF'), c('CHECK_BORDER_HOVER')), 'Checkbox_Press')
    add('check_on.png', on, 'Checkbox_On')
    add('check_on_press.png', check(towards(c('CHECK_ON'), 0.0, press), marked=True), 'Checkbox_On_Press')
    add('check_off_disabled.png', faded(off, disabled), 'Checkbox_Off_Disabled')
    add('check_on_disabled.png', faded(on, disabled), 'Checkbox_On_Disabled')

    # radio buttons (pscompat's radio: the check box's colours, a white dot when chosen)
    w, h = size_of('RadioButton_Off')
    rect = (0.5, 0, 13.5, 13)                       # the official circle (13 px)
    half = 6.5
    dot_r = v('RADIO_DOT') / v('RADIO_SIZE') * half

    def radio(fill, border=None, dot=False):
        out = box(w, h, rect, half, fill, border)
        if dot:
            out = over(out, box(w, h, (7 - dot_r, 6.5 - dot_r, 7 + dot_r, 6.5 + dot_r), dot_r, c('CHECK_MARK_COLOR')))
        return out

    off = radio(c('CHECK_OFF'), c('CHECK_BORDER'))
    on = radio(c('CHECK_ON'), dot=True)
    add('radio_off.png', off, 'RadioButton_Off')
    add('radio_off_press.png', radio(c('CHECK_OFF'), c('CHECK_BORDER_HOVER')), 'RadioButton_Press')
    add('radio_on.png', on, 'RadioButton_On')
    add('radio_on_press.png', radio(towards(c('CHECK_ON'), 0.0, press), dot=True), 'RadioButton_On_Press')
    add('radio_off_disabled.png', faded(off, disabled), 'RadioButton_Disabled')
    add('radio_on_disabled.png', faded(on, disabled), 'RadioButton_On_Disabled')

    # sliders (psui's, 5.): a light knob over a dark groove (the official one fills nothing left of the knob)
    w, h = size_of('SliderThumb_Off')
    knob_rect = (0.5, 0, 13.5, 13)                  # the official knob (13 px), as the radio's circle
    knob_r = 6.5
    knob = (c('KNOB_TOP'), c('KNOB_BOTTOM'))
    off = box(w, h, knob_rect, knob_r, knob)
    add('slider_knob.png', off, 'SliderThumb_Off')
    add('slider_knob_press.png', box(w, h, knob_rect, knob_r, tuple(towards(k_, 0.0, press) for k_ in knob)),
        'SliderThumb_Press')
    add('slider_knob_disabled.png', faded(off, disabled), 'SliderThumb_Disabled')
    t2 = v('SLIDER_TRACK') / 2.0
    w, h = size_of('SliderTrack_Horiz')
    add('slider_track_h.png', box(w, h, (1, h / 2.0 - t2, w - 1, h / 2.0 + t2), t2, c('SLIDER_EMPTY')),
        'SliderTrack_Horiz')
    w, h = size_of('SliderTrack_Vert')
    add('slider_track_v.png', box(w, h, (w / 2.0 - t2, 1, w / 2.0 + t2, h - 1), t2, c('SLIDER_EMPTY')),
        'SliderTrack_Vert')

    # spinners' arrows (pscompat's spinner: bare chevrons, the accent's light line while pressed)
    w, h = size_of('Stepper_Up_Off')
    for pointing in ('up', 'down'):
        art = chevron(w / 2.0, h / 2.0, 8.0, 4.0, pointing)
        name = pointing.capitalize()
        add('spin_%s.png' % pointing, lines(mpi, w, h, art, c('TEXT_LABEL')), 'Stepper_%s_Off' % name)
        add('spin_%s_press.png' % pointing, lines(mpi, w, h, art, c('ACCENT_LINE')), 'Stepper_%s_Press' % name)

    # scroll bars' arrows: bare chevrons, quieter than the thumb's words
    w, h = size_of('ScrollArrow_Up')
    for pointing in ('up', 'down', 'left', 'right'):
        add('scroll_arrow_%s.png' % pointing, lines(mpi, w, h, chevron(w / 2.0, h / 2.0, 7.0, 3.5, pointing),
                                                    c('TEXT_DIM')), 'ScrollArrow_%s' % pointing.capitalize())

    # tabs on top: the segments' colours (14.), the chosen one blue with the accent under it (17.); round at
    # the top only (the box goes on below the image)
    w, h = size_of('TabTop_Middle_Flat_Off')
    r = radius_for('TabTop_Middle_Flat_Off', v('VALUE_RADIUS'))
    rect = (0, 0, w - 1, h + r)                     # a line between two tabs, as the official
    line = box(w, h, (0, h - 3, w - 1, h), 0.0, c('ACCENT'))
    add('tab_top_off.png', box(w, h, rect, r, c('SEG_OFF')),
        'TabTop_First_Flat_Off', 'TabTop_Middle_Flat_Off', 'TabTop_Last_Flat_Off')
    add('tab_top_on.png', over(box(w, h, rect, r, (c('SEG_ON_LEFT'), c('SEG_ON_RIGHT'))), line),
        'TabTop_First_Flat_Selected', 'TabTop_Middle_Flat_Selected', 'TabTop_Last_Flat_Selected')
    # a flashing tab (the model upload's log): the accent (the official one is white)
    add('tab_top_flash.png', box(w, h, rect, r, c('ACCENT')), 'TabTop_Right_Flashing')
    # tabs at the left: the rail's (1.): dark, the chosen one blue across with the light bar at its left
    w, h = size_of('TabLeft_Flat_Off')
    rect = (0, 1, w, h - 1)
    bar = box(w, h, (0, 1, v('RAIL_BAR_WIDTH'), h - 1), 0.0, c('RAIL_SEL_BAR'))
    add('tab_left_off.png', box(w, h, rect, 0.0, c('RAIL')), 'TabLeft_Flat_Off')
    add('tab_left_on.png', over(box(w, h, rect, 0.0, (c('RAIL_SEL_LEFT'), c('RAIL_SEL_RIGHT')), across=True), bar),
        'TabLeft_Flat_Selected')

    # list rows (the people, places and wearables lists): a list's chosen row with the accent's bar (19.), and
    # the official list's row under the mouse. The bar is the 2 px the official slices keep at the left; they
    # keep one more here (a line of the fill beside the bar, as the Toolbar_* buttons: else the bar is smeared
    # into the stretched band)
    w, h = size_of('ListItem_Select')
    bar_w = fixed_sides(w, h, info('ListItem_Select')[1])[0]
    add('list_row_on.png', over(box(w, h, (0, 0, w, h), 0.0, c('SELECTION_BG')),
                                box(w, h, (0, 0, bar_w, h), 0.0, c('ACCENT_LINE'))), 'ListItem_Select',
        scale_of=lambda name, scale: (bar_w + 1,) + scale[1:])
    add('list_row_hover.png', box(w, h, (0, 0, w, h), 0.0, c('ROW_HOVER_BG')), 'ListItem_Over')

    # accordion headers (drawn whole, no 9 slices, over header_bg_color DkGray2 = the panel's top): a divider
    # under them; the mouse over it as an official list's row, pressed, and the focused one as a list's chosen row
    w, h = size_of('Accordion_Off')
    divider = box(w, h, (0, h - 1, w, h), 0.0, c('DIVIDER_RIGHT'))
    add('accordion_off.png', divider, 'Accordion_Off')
    add('accordion_over.png', over(box(w, h, (0, 0, w, h), 0.0, c('ROW_HOVER_BG')), divider), 'Accordion_Over')
    add('accordion_press.png', over(box(w, h, (0, 0, w, h), 0.0, c('RAIL_PRESS')), divider), 'Accordion_Press')
    add('accordion_focus.png', over(box(w, h, (0, 0, w, h), 0.0, c('SELECTION_BG')), divider), 'Accordion_Selected')
    w, h = size_of('Accordion_ArrowClosed_Off')
    for state, official_state, color in (('', 'Off', 'ICON_LIGHT'), ('_press', 'Press', 'ACCENT_LINE')):
        add('accordion_closed%s.png' % state, lines(mpi, w, h, chevron(w / 2.0, h / 2.0, 6.0, 3.0, 'right'), c(color)),
            'Accordion_ArrowClosed_%s' % official_state)
        add('accordion_open%s.png' % state, lines(mpi, w, h, chevron(w / 2.0, h / 2.0, 6.0, 3.0, 'down'), c(color)),
            'Accordion_ArrowOpened_%s' % official_state)

    # the inventory's "Received items" bar and the like (a toggle with the arrow in its fixed left part): the
    # segments' box (14.); open: the arrow down
    w, h = size_of('MarketplaceBtn_Off')
    r = radius_for('MarketplaceBtn_Off', v('VALUE_RADIUS'))
    rect = (0, 0, w, h - 1)
    arrow_x = 21.0                                  # where the official arrow is, in the fixed left 30 px
    for file, name, pointing in (('marketplace_closed.png', 'MarketplaceBtn_Off', 'right'),
                                 ('marketplace_open.png', 'MarketplaceBtn_Selected', 'down')):
        add(file, drop(c('SEG_OFF'), c('SEG_BORDER_DIM'), c('ICON_LIGHT'), w, h, rect, r,
                       chevron(arrow_x, (h - 1) / 2.0, 9.0, 4.5, pointing)), name)

    # segmented buttons (left, middle, right): the segments (14.), rounded only at the outer ends
    w, h = size_of('SegmentedBtn_Left_Off')
    r = radius_for('SegmentedBtn_Left_Off', v('SEG_RADIUS'))
    hover = v('STATE_HOVER')
    seg_off = (c('SEG_OFF'), c('SEG_BORDER_DIM'))
    seg_on = ((c('SEG_ON_LEFT'), c('SEG_ON_RIGHT')), c('SEG_BORDER'))

    def shade(colors, target, amount):
        fill, border = colors
        fill = tuple(towards(f, target, amount) for f in fill) if isinstance(fill[0], tuple) else towards(fill, target, amount)
        return fill, border

    states = {'Off': (seg_off, 1.0), 'Over': (shade(seg_off, 1.0, hover), 1.0), 'Press': (shade(seg_off, 0.0, press), 1.0),
              'Disabled': (seg_off, disabled), 'Selected': (seg_on, 1.0), 'Selected_Over': (shade(seg_on, 1.0, hover), 1.0),
              'Selected_Press': (shade(seg_on, 0.0, press), 1.0), 'Selected_Disabled': (seg_on, disabled)}
    for part, rect in (('Left', (0, 0, w + r, h - 1)), ('Middle', (-r, 0, w + r, h - 1)), ('Right', (-r, 0, w, h - 1))):
        for state, ((fill, border), opacity) in states.items():
            name = 'SegmentedBtn_%s_%s' % (part, state)
            if name in official:
                add('segment_%s_%s.png' % (part.lower(), state.lower()),
                    box(w, h, rect, r, fill, border, opacity, across=True), name)

    # the small buttons of the floaters' bottom bars and the bottom tabs (Toolbar_*): the segments' quiet box (14.)
    # at rest, so a button of words only (the poser's "Start Posing", "Mirror" ...) still looks like a button;
    # under the mouse its border lights as the chosen segment's, the segments' blue when chosen, the accent when
    # flashing. Square, the border on the image's edge. Their official 9 slices keep only 1 px at some sides (the
    # right of Left and Middle, the left of Middle and Right, the top): that is the border alone, and the drawing
    # filters across the seam, so on a wide button the border was smeared 2-3 px into the stretched band. These
    # keep at least 2 px at every side (the border and a line of the fill; Left keeps the official 5 at the left
    # and 4 at the bottom, Right 5 at the right, Middle_Flash 5,23,29,4): with ListItem_Select, the only 9 slices
    # not copied from the official skin (the image and where it is drawn do not change; ORIGIN.md)
    w, h = size_of('Toolbar_Middle_Off')
    rect = (0, 0, w, h)

    def two_fixed(name, scale):
        left, top, right, bottom = scale
        return max(left, 2), min(top, h - 2), min(right, w - 2), max(bottom, 2)

    for part in ('Left', 'Middle', 'Right'):
        for state, arr in (('Off', box(w, h, rect, 0.0, c('SEG_OFF'), c('SEG_BORDER_DIM'))),
                           ('Over', box(w, h, rect, 0.0, c('SEG_OFF'), c('SEG_BORDER'))),
                           ('Selected', box(w, h, rect, 0.0, (c('SEG_ON_LEFT'), c('SEG_ON_RIGHT')), c('SEG_BORDER'))),
                           ('Flash', box(w, h, rect, 0.0, c('ACCENT')))):
            name = 'Toolbar_%s_%s' % (part, state)
            if name in official:
                add('bar_button_%s.png' % state.lower(), arr, name, scale_of=two_fixed)

    # shapes the official images give (a docked floater's tongue, an inspector's opaque back): their outline (the
    # official image's opacity), in the window's colour. A badge's back is tinted by BadgeImageColor (tokens.txt):
    # white, so the tint is the colour itself
    for name, color in (('Flyout_Left', 'WINDOW_BOTTOM'), ('Flyout_Right', 'WINDOW_BOTTOM'),
                        ('Flyout_Pointer', 'WINDOW_BOTTOM'), ('Inspector_Hover', 'PANEL_MID'),
                        ('Badge_Background', 'CHECK_MARK_COLOR')):
        add('shape_%s.png' % name.lower(), coloured(official_pixels(name)[..., 3], c(color)), name)

    # the location bar's parcel icons were drawn for the official light field: the official skin has the same
    # icons for dark grounds (Parcel_*_Dark), so these names get those files (as Parcel_ScriptsNo_Light already
    # does in the official skin). The ones with no dark twin get their greys turned over (dark to light; the
    # coloured marks stay), from the official image
    for kind in ('VoiceNo', 'FlyNo', 'PushNo', 'BuildNo', 'SeeAVsOff'):
        light, dark = 'Parcel_%s_Light' % kind, 'Parcel_%s_Dark' % kind
        if size_of(light) != size_of(dark):
            raise TokenError(OFFICIAL_CHANGED % ('%s and %s differ in size' % (light, dark)))
        _, scale, preload = info(light)
        entries.append((light, None, scale, preload, info(dark)[0]))
        done.add(light)
    for name in ('Parcel_ForSale_Light', 'Pathfinding_Dirty', 'Pathfinding_Disabled'):
        px = official_pixels(name)
        grey = (px[..., :3].max(axis=-1) - px[..., :3].min(axis=-1) < 0.2)[..., None]
        px[..., :3] = np.where(grey, 1.0 - px[..., :3], px[..., :3])
        add('icon_%s.png' % name.lower(), px, name)

    left = sorted(n for n in official if n.startswith(OLD_PARTS_FAMILIES) and n not in done and n not in OLD_PARTS_LEFT
                  and not any(re.fullmatch(p, n) for p, _ in OLD_PARTS_LEFT_PATTERNS))
    if left:
        raise TokenError(OFFICIAL_CHANGED % ('official images of the families drawn again that old_parts does not '
                                             'draw (draw them, or name them in OLD_PARTS_LEFT with the reason): '
                                             + ', '.join(left)))


def to_bytes(arr):
    """A float RGBA image (0..1) as 8 bit values."""
    return np.round(np.clip(arr, 0.0, 1.0) * 255.0).astype(np.uint8)


def to_png_bytes(arr):
    buf = io.BytesIO()
    Image.fromarray(to_bytes(arr)).save(buf, format='PNG', optimize=True)
    return buf.getvalue()


def make_textures_xml(entries):
    out = ['<?xml version="1.0" encoding="utf-8" standalone="yes" ?>',
           GENERATED_XML.rstrip('\n') % os.path.relpath(HERE, NEWVIEW).replace('\\', '/'),
           '<!-- Laid over skins/default/textures/textures.xml: these names get the images of %s/ -->' % IMAGE_DIR,
           '<textures version="101">']
    for entry in entries:
        name, file, scale = entry[:3]
        preload = entry[3] if len(entry) > 3 else True      # the official small parts keep the official one
        # a fifth: an official image file (skins/default/textures/...; the image list looks in every skin)
        path = entry[4] if len(entry) > 4 else '%s/%s' % (IMAGE_DIR, file)
        line = '  <texture name="%s" file_name="%s" preload="%s"' % (name, path, 'true' if preload else 'false')
        if scale:
            line += ' scale.left="%d" scale.top="%d" scale.right="%d" scale.bottom="%d"' % scale
        out.append(line + ' />')
    out += ['</textures>', '']
    return '\n'.join(out)

# ---------------------------------------------------------------- write / check


def outputs():
    sections, tokens, official, keep = parse_tokens(TOKENS)
    text = {
        os.path.join(HERE, 'pstokens.h'): make_header(sections, tokens),
        os.path.join(HERE, 'pstokens.cpp'): make_cpp(tokens),
        os.path.join(SKIN, 'colors.xml'): make_colors_xml(tokens, official, keep),
    }
    images, entries = make_images(tokens, need_imaging())
    text[os.path.join(SKIN, 'textures', 'textures.xml')] = make_textures_xml(entries)
    pngs = {os.path.join(SKIN, 'textures', IMAGE_DIR, n): a for n, a in images.items()}
    return text, pngs


def rel(path):
    return os.path.relpath(path, NEWVIEW).replace('\\', '/')


def check(text, pngs):
    bad = []
    for path, want in text.items():
        try:
            with open(path, encoding='utf-8', newline='') as f:
                have = f.read()
        except OSError:
            bad.append('%s: missing' % rel(path))
            continue
        if have.replace('\r\n', '\n') != want:
            bad.append('%s: differs from what tokens.txt makes' % rel(path))
    for path, arr in pngs.items():
        try:
            have = np.asarray(Image.open(path).convert('RGBA')).astype(np.int16)
        except OSError:
            bad.append('%s: missing' % rel(path))
            continue
        want = to_bytes(arr).astype(np.int16)
        # one step of 255 allowed: numpy versions may round the last bit differently
        if have.shape != want.shape or np.abs(have - want).max() > 1:
            bad.append('%s: differs from what tokens.txt makes' % rel(path))
    folder = os.path.join(SKIN, 'textures', IMAGE_DIR)
    if os.path.isdir(folder):
        for n in sorted(os.listdir(folder)):
            p = os.path.join(folder, n)
            if n.lower().endswith('.png') and p not in pngs:
                bad.append('%s: not made from tokens.txt (left over: remove it)' % rel(p))
    return bad


def write(text, pngs):
    for path, data in text.items():
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(data)
    folder = os.path.join(SKIN, 'textures', IMAGE_DIR)
    os.makedirs(folder, exist_ok=True)
    for n in os.listdir(folder):
        p = os.path.join(folder, n)
        if n.lower().endswith('.png') and p not in pngs:
            os.remove(p)
    for path, arr in pngs.items():
        with open(path, 'wb') as f:
            f.write(to_png_bytes(arr))
    print('%d files and %d images in %s' % (len(text), len(pngs), NEWVIEW))


def main():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    try:
        text, pngs = outputs()
    except TokenError as ex:
        print('tokens.txt:' + str(ex))
        return 1
    if '--check' in sys.argv[1:]:
        bad = check(text, pngs)
        for b in bad:
            print(b)
        if not bad:
            print('%d files and %d images are what tokens.txt makes' % (len(text), len(pngs)))
        return 1 if bad else 0
    write(text, pngs)
    return 0


if __name__ == '__main__':
    sys.exit(main())
