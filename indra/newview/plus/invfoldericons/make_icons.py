# Makes the folder icons of plus/invfoldericons (Charis' own drawing, no image of
# another viewer is used): one colour and one emblem per system folder type, a
# closed and an open folder each, 16 x 16 like the official Inv_SysClosed.png.
#
#   python -I make_icons.py <out dir> [--preview <preview.png>]
#
# The out dir is skins/default/textures/plus/ (the PNGs go straight in it: the
# viewer manifest packs skins/*/textures/*/*.png, one folder deep). The names
# are inv_folder_<key>_closed.png / _open.png; invfoldericons.cpp declares them.
#
# Drawn pixel by pixel (no scaling down): a dark outline one pixel wide around
# the folder and around the emblem, so the shapes stay sharp at 16 px on the
# dark skin and on a light one.

import sys
from PIL import Image

W = H = 16

# ---- emblems: 7 x 7 (or smaller, centred); '.' = folder shows through -------
# '#' white, 'g' light grey, 'm' mid grey, 'k' outline colour (dark), 'y' yellow, 'r' red
EMBLEMS = {
    "star": [
        "...#...",
        "..###..",
        "#######",
        ".#####.",
        "..###..",
        ".##.##.",
        "##...##",
    ],
    "hanger": [
        "...##..",
        "..#..#.",
        ".....#.",
        "...##..",
        "..#..#.",
        ".#....#",
        "#######",
    ],
    "check": [
        "......#",
        ".....##",
        "#...##.",
        "##.##..",
        ".###...",
        "..#....",
        ".......",
    ],
    "play": [
        "##.....",
        "####...",
        "######.",
        "#######",
        "######.",
        "####...",
        "##.....",
    ],
    "hand": [
        ".#.#.#.",
        ".#.#.#.",
        ".#.#.##",
        ".######",
        "#######",
        ".#####.",
        "..###..",
    ],
    "cube": [
        "..#####",
        ".#####m",
        "gggggmm",
        "gggggmm",
        "gggggmm",
        "gggggm.",
        "ggggg..",
    ],
    "braces": [
        ".##.##.",
        ".#...#.",
        ".#...#.",
        "#.....#",
        ".#...#.",
        ".#...#.",
        ".##.##.",
    ],
    "picture": [
        "#######",
        "#....y#",
        "#.....#",
        "#..#..#",
        "#.###.#",
        "#######",
    ],
    "page": [
        "#####..",
        "#####g.",
        "#kkk###",
        "#######",
        "#kkkk##",
        "#######",
        "#kkkk##",
    ],
    "pin": [
        ".###.",
        "#####",
        "##r##",
        "#####",
        ".###.",
        "..#..",
        "..#..",
    ],
    "note": [
        "..#####",
        "..#####",
        "..#...#",
        "..#...#",
        ".##..##",
        "###.###",
        ".#...#.",
    ],
    "shirt": [
        ".##.##.",
        "#######",
        "#######",
        ".#####.",
        ".#####.",
        ".#####.",
        ".#####.",
    ],
    "person": [
        "..###..",
        "..###..",
        "...#...",
        "#######",
        "..###..",
        "..#.#..",
        ".##.##.",
    ],
    "trash": [
        "..###..",
        "#######",
        ".#####.",
        ".#k#k#.",
        ".#k#k#.",
        ".#k#k#.",
        ".#####.",
    ],
    "question": [
        ".####.",
        "##..##",
        "....##",
        "...##.",
        "..##..",
        "......",
        "..##..",
    ],
    "card": [
        "#######",
        "#kk#kk#",
        "#kk####",
        "#######",
        "#kkkkk#",
        "#######",
    ],
    "camera": [
        "..##...",
        "#######",
        "###k###",
        "##k#k##",
        "###k###",
        "#######",
    ],
    "triangle": [
        "...#...",
        "..#.#..",
        "..#.#..",
        ".#...#.",
        ".#...#.",
        "#.....#",
        "#######",
    ],
    "sun": [
        "y..y..y",
        ".yyyyy.",
        ".yyyyy.",
        "yyyyyyy",
        ".yyyyy.",
        ".yyyyy.",
        "y..y..y",
    ],
    "sphere": [
        "..###..",
        ".##ggg.",
        "##gggg#",
        "#gggggg",
        "#gggggg",
        ".gggggg",
        "..ggg..",
    ],
    "house": [
        "...#...",
        "..###..",
        ".#####.",
        "#######",
        ".##k##.",
        ".##k##.",
        ".##k##.",
    ],
    "arrow_down": [
        "..###..",
        "..###..",
        "#######",
        ".#####.",
        "..###..",
        "...#...",
        "#######",
    ],
    "letter_c": [
        "..####.",
        ".##..##",
        "##.....",
        "##.....",
        "##.....",
        ".##..##",
        "..####.",
    ],
    "arrow_up": [
        "...#...",
        "..###..",
        ".#####.",
        "#######",
        "..###..",
        "..###..",
        "#######",
    ],
}

# ---- folder types: key -> (body colour, emblem or None) ---------------------
# Hues spread round the colour wheel; plain folders keep the manila colour and
# no emblem, so every system folder stands out from them by colour and emblem.
FOLDERS = {
    "plain":       ((0xE8, 0xC4, 0x6A), None),
    "root":        ((0x7F, 0x8C, 0x9A), "house"),
    "favorites":   ((0xD8, 0x40, 0x50), "star"),
    "outfits":     ((0x8B, 0x5C, 0xD0), "hanger"),
    "currentlook": ((0x8B, 0x5C, 0xD0), "check"),
    "clothing":    ((0xB2, 0x60, 0xC8), "shirt"),
    "bodyparts":   ((0xD9, 0x8C, 0x72), "person"),
    "animations":  ((0xEE, 0x90, 0x2E), "play"),
    "gestures":    ((0x22, 0xA3, 0x96), "hand"),
    "objects":     ((0x3C, 0x78, 0xD8), "cube"),
    "scripts":     ((0x3E, 0x9E, 0x48), "braces"),
    "textures":    ((0xD8, 0x55, 0x9E), "picture"),
    "snapshots":   ((0x5A, 0x63, 0xC8), "camera"),
    "notecards":   ((0xA0, 0xA8, 0xB2), "page"),
    "landmarks":   ((0x8D, 0xBF, 0x3A), "pin"),
    "sounds":      ((0x1E, 0xA7, 0xC8), "note"),
    "callingcards":((0x8D, 0x6E, 0x52), "card"),
    "meshes":      ((0x6F, 0x8F, 0xA6), "triangle"),
    "settings":    ((0x5A, 0xA8, 0xE6), "sun"),
    "materials":   ((0x7A, 0x7F, 0x9A), "sphere"),
    "trash":       ((0x6B, 0x70, 0x78), "trash"),
    "lostandfound":((0xE0, 0x60, 0x3A), "question"),
    "inbox":       ((0x4E, 0x8A, 0x6A), "arrow_down"),
    "outbox":      ((0x4E, 0x8A, 0x6A), "arrow_up"),
    # the #Charis folder of this viewer (a plain folder by type, picked by name)
    "charis":      ((0x2B, 0x3F, 0x9E), "letter_c"),
}


def scale(c, f):
    return tuple(max(0, min(255, int(round(v * f)))) for v in c)


def lighten(c, f):
    return tuple(int(round(v + (255 - v) * f)) for v in c)


def outline_of(mask):
    """Pixels of the mask with a 4-neighbour outside it."""
    out = set()
    for (x, y) in mask:
        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            if (x + dx, y + dy) not in mask:
                out.add((x, y))
                break
    return out


def closed_shape():
    """Tab + body. Returns (back mask, front mask, front top row)."""
    tab = {(x, y) for y in range(2, 4) for x in range(1, 7) if not (y == 2 and x in (1, 6))}
    body = {(x, y) for y in range(4, 15) for x in range(0, 16)}
    # round the four corners of the body by one pixel
    body -= {(0, 4), (15, 4), (0, 14), (15, 14)}
    return tab | body, set(), 4


def open_shape():
    """Back panel with tab, and the front flap tilted forward (a parallelogram)."""
    back, _, _ = closed_shape()
    back = {(x, y) for (x, y) in back if x <= 13}
    front = set()
    for y in range(6, 15):
        shift = (14 - y) * 3 // 8        # 3 px at the top, 0 at the bottom
        for x in range(0 + shift, 13 + shift):
            if 0 <= x < W:
                front.add((x, y))
    front -= {(3, 6), (15, 6)}
    return back, front, 6


def paint(img, mask, fill, line, top_row=None, hi=None):
    edge = outline_of(mask)
    for (x, y) in mask:
        if (x, y) in edge:
            img.putpixel((x, y), line + (255,))
        elif hi is not None and top_row is not None and y == top_row + 1:
            img.putpixel((x, y), hi + (255,))
        else:
            img.putpixel((x, y), fill + (255,))


def draw_emblem(img, name, line, cx, top):
    rows = EMBLEMS[name]
    h = len(rows)
    w = max(len(r) for r in rows)
    x0 = cx - w // 2
    y0 = top + (7 - h + 1) // 2
    glyph = {}
    for j, r in enumerate(rows):
        for i, ch in enumerate(r):
            if ch != ".":
                glyph[(x0 + i, y0 + j)] = ch
    colours = {
        "#": (0xFF, 0xFF, 0xFF),
        "g": (0xD2, 0xD6, 0xDC),
        "m": (0x96, 0x9E, 0xAA),
        "k": line,
        "y": (0xFF, 0xD2, 0x3A),
        "r": (0xE5, 0x39, 0x35),
    }
    # dark ring (8 neighbours) so the white shape keeps off the folder colour
    ring = set()
    for (x, y) in glyph:
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                p = (x + dx, y + dy)
                if p not in glyph and 0 <= p[0] < W and 0 <= p[1] < H:
                    ring.add(p)
    for p in ring:
        img.putpixel(p, line + (255,))
    for p, ch in glyph.items():
        img.putpixel(p, colours[ch] + (255,))


def make(key, is_open):
    body, emblem = FOLDERS[key]
    line = scale(body, 0.38)
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    if not is_open:
        mask, _, top = closed_shape()
        paint(img, mask, body, line, top, lighten(body, 0.30))
        if emblem:
            draw_emblem(img, emblem, line, 7, 6)
    else:
        back, front, top = open_shape()
        paint(img, back, scale(body, 0.72), line)
        paint(img, front, lighten(body, 0.12), line, top, lighten(body, 0.40))
        if emblem:
            draw_emblem(img, emblem, line, 7, 7)
    return img


def main(argv):
    if len(argv) < 2:
        print(__doc__ or "usage: make_icons.py <out dir> [--preview <png>]")
        return 2
    out_dir = argv[1].rstrip("/\\")
    preview = argv[argv.index("--preview") + 1] if "--preview" in argv else None
    made = []
    for key in FOLDERS:
        for is_open in (False, True):
            img = make(key, is_open)
            name = "inv_folder_%s_%s.png" % (key, "open" if is_open else "closed")
            img.save(out_dir + "/" + name)
            made.append((key, is_open, img))
    print("made %d icons in %s" % (len(made), out_dir))

    if preview:
        # each icon at 1x and 4x, on the dark skin colour and on a light one
        cols = 4
        cell = 4 * W + 8 + W + 8
        rows_n = (len(FOLDERS) + cols - 1) // cols
        sheet = Image.new("RGBA", (cols * cell * 2, rows_n * 2 * (4 * H + 8)), (0, 0, 0, 255))
        for idx, key in enumerate(FOLDERS):
            r, c = divmod(idx, cols)
            for bgi, bg in enumerate(((0x24, 0x26, 0x29), (0xE8, 0xE8, 0xE8))):
                ox = (c * 2 + bgi) * cell
                oy = r * 2 * (4 * H + 8)
                tile = Image.new("RGBA", (cell, 2 * (4 * H + 8)), bg + (255,))
                sheet.paste(tile, (ox, oy))
                for k, is_open in enumerate((False, True)):
                    img = make(key, is_open)
                    big = img.resize((4 * W, 4 * H), Image.NEAREST)
                    y = oy + 4 + k * (4 * H + 8)
                    sheet.alpha_composite(big, (ox + 4, y))
                    sheet.alpha_composite(img, (ox + 4 * W + 12, y + 24))
        sheet.save(preview)
        print("preview:", preview)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
