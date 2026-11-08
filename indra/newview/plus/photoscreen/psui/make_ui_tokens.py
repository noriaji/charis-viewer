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


def round_rect(w, h, half_w, half_h, radius):
    """Coverage (0..1) of a rounded rectangle centred in a w x h image, anti-aliased by its distance."""
    ys, xs = np.mgrid[0:h, 0:w]
    r = max(0.0, min(radius, half_w, half_h))
    qx = np.abs(xs + 0.5 - w / 2.0) - (half_w - r)
    qy = np.abs(ys + 0.5 - h / 2.0) - (half_h - r)
    d = np.hypot(np.maximum(qx, 0.0), np.maximum(qy, 0.0)) + np.minimum(np.maximum(qx, qy), 0.0) - r
    return np.clip(0.5 - d, 0.0, 1.0)


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
    for name in ('Window_Background', 'Window_NoTitle_Background', 'Toast_Background', 'Inspector_Background'):
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
    return images, entries


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
    for name, file, scale in entries:
        line = '  <texture name="%s" file_name="%s/%s" preload="true"' % (name, IMAGE_DIR, file)
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
