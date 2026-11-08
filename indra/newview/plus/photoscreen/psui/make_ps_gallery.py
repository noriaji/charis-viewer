"""Writes the parts sample's XUI (floater_plus_ps_gallery.xml, en and ja; research/221, branch ps-style).

The places are those of the target picture (research/photoscreen-design-target-2026-10-30.webp)
scaled to a 1080 pixel high window (x 1.18): the rail, the left panel, the world with its chips,
histogram, read-out and buttons, the bottom strip, the right panel and the shutter strip. The ja
file holds the words only (the official way: names match the en file).

Run from anywhere: python -I make_ps_gallery.py
"""
import os
from xml.sax.saxutils import escape, quoteattr

HERE = os.path.dirname(os.path.abspath(__file__))
XUI = os.path.normpath(os.path.join(HERE, '..', '..', '..', 'skins', 'default', 'xui'))


def E(tag, name, rect=None, ja=None, children=(), **attrs):
    """An element: rect = (left, top, width, height) in its parent (layout topleft)."""
    a = {'name': name}
    if rect:
        a.update(layout='topleft', left=rect[0], top=rect[1], width=rect[2], height=rect[3])
    a.update(attrs)
    return {'tag': tag, 'attrs': a, 'ja': ja or {}, 'children': list(children)}


def T(en, ja):
    return en, ja


def words(el, key, value):
    """Puts a (en, ja) pair or a plain value into the element."""
    if isinstance(value, tuple):
        el['attrs'][key] = value[0]
        el['ja'][key] = value[1]
    else:
        el['attrs'][key] = value


def W(tag, name, rect, texts=None, **attrs):
    el = E(tag, name, rect, **attrs)
    for k, v in (texts or {}).items():
        words(el, k, v)
    return el


def S(name, text):
    """A <string name=...> of the floater: text is a (en, ja) pair."""
    el = E('string', name)
    words(el, '_text', text)
    return el


def item(name, label, value=None):
    el = E('plus_ps_combo.item', name)
    words(el, 'label', label)
    el['attrs']['value'] = value or name
    return el

# ---------------------------------------------------------------- the rail


RAIL_TABS = [
    ('scene', T('Scene', 'シーン')),
    ('avatar', T('Avatar', 'アバター')),
    ('environment', T('Environment', '環境')),
    ('lighting', T('Lighting', 'ライティング')),
    ('pose', T('Pose &\nAnimation', 'ポーズと\nアニメ')),
    ('camera', T('Camera', 'カメラ')),
    ('effects', T('Effects', 'エフェクト')),
    ('presets', T('Presets', 'プリセット')),
]


def rail():
    tabs = []
    for i, (icon, label) in enumerate(RAIL_TABS):
        extra = {'initial_value': 'true'} if i == 0 else {}
        tabs.append(W('plus_ps_rail_tab', 'tab_' + icon, (0, 8 + i * 84, 123, 83), {'label': label}, icon=icon, **extra))
    return E('plus_ps_box', 'rail', (0, 0, 123, 1076), kind='rail', corners='tl', children=tabs)

# ---------------------------------------------------------------- the left panel


def checks(prefix, top, rows):
    out = []
    for i, (label, on) in enumerate(rows):
        out.append(W('plus_ps_check', '%s_%d' % (prefix, i), (14, top + i * 30, 252, 30), {'label': label},
                     initial_value='true' if on else 'false'))
    return out


def left_panel():
    c = [
        W('plus_ps_header', 'h_scene', (0, 0, 280, 52), {'label': T('Scene', 'シーン')}, icon='clapperboard'),
        W('plus_ps_label', 'l_location', (14, 62, 252, 24), {'label': T('Location', '場所')}),
        W('plus_ps_card', 'c_location', (14, 90, 252, 59),
          {'label': T('My Parcel', '自分の区画'), 'subtitle': T('Seaside Villa', '海辺の別荘')}, icon='scene'),
        W('plus_ps_label', 'l_time', (14, 160, 252, 24), {'label': T('Time of Day', '時刻')}),
        W('plus_ps_picture', 'p_time', (14, 188, 252, 52), placeholder='dusk_night'),
        W('plus_ps_slider', 's_time', (14, 246, 252, 30), min_value='0', max_value='3', increment='1',
          decimal_digits='0', label_width='0', ticks='4', initial_value='2'),
        W('plus_ps_label', 'l_sky', (14, 288, 252, 24), {'label': T('Windlight', '空の設定')}),
        W('plus_ps_card', 'c_sky', (14, 316, 252, 59), {'label': T('Golden Hour', 'ゴールデンアワー')}, icon='sun'),
        W('plus_ps_header', 'h_env', (0, 392, 280, 52), {'label': T('Environment', '環境')}, icon='sliders', divider='top'),
    ]
    c += checks('chk_env', 448, [
        (T('Show HUD', 'HUD を表示'), False),
        (T('Show Other Avatars', '他のアバターを表示'), False),
        (T('Show Attachments', '装着物を表示'), False),
        (T('Show Particles', 'パーティクルを表示'), True),
        (T('Show Shadows', '影を表示'), True),
        (T('Maximum LOD', '最高の細かさ（LOD）'), True),
    ])
    c += [
        W('plus_ps_header', 'h_avatar', (0, 632, 280, 52), {'label': T('Avatar', 'アバター')}, icon='user', divider='top'),
        W('plus_ps_card', 'c_avatar', (14, 690, 252, 69),
          {'label': T('Hinata', 'ひなた'), 'subtitle': T('Complexity 134,217', '複雑さ 134,217')}, kind='avatar', icon='avatar'),
    ]
    c += checks('chk_avatar', 770, [
        (T('Freeze Animation (AO)', 'AO を止める'), True),
        (T('Freeze Pose', 'ポーズを止める'), True),
        (T('Disable Head Tracking', '頭の向きを止める'), True),
        (T('Hide Attachments', '装着物を隠す'), False),
        (T('Render at Max Quality', '最高画質で描く'), True),
    ])
    c.append(W('plus_ps_button', 'b_reset', (14, 932, 252, 40), {'label': T('Reset Avatar', 'アバターを元に戻す')}))
    return E('plus_ps_box', 'left', (131, 8, 280, 1060), kind='panel', children=c)

# ---------------------------------------------------------------- the world


def world():
    wl, wt, ww, wh = 419, 8, 972, 808
    chips = [
        W('plus_ps_chip', 'chip_look', (0, 0, 150, 40), {'label': T('Cinematic', 'シネマ')}, icon='film', dropdown='true'),
        W('plus_ps_chip', 'chip_size', (160, 0, 90, 40), {'label': '4K'}, icon='resolution'),
        W('plus_ps_chip', 'chip_aspect', (260, 0, 100, 40), {'label': '16:9'}, icon='ratio'),
        W('plus_ps_chip', 'chip_grid', (370, 0, 110, 40), {'label': T('Grid', 'グリッド')}, icon='palette', dropdown='true'),
    ]
    icons = [
        W('plus_ps_icon_button', 'ib_shoot', (1, 0, 42, 40), {'tool_tip': T('Take a picture', '撮影')}, icon='camera'),
        W('plus_ps_icon_button', 'ib_focus', (44, 0, 42, 40), {'tool_tip': T('Focus', 'ピント')}, icon='focus'),
        W('plus_ps_icon_button', 'ib_frame', (87, 0, 42, 40), {'tool_tip': T('Focus frame', 'ピント枠')}, icon='frame', toggle='true'),
    ]
    return [
        W('plus_ps_picture', 'world', (wl, wt, ww, wh), placeholder='dusk_night', radius='0'),
        E('plus_ps_row', 'chips', (wl + 12, wt + 12, 700, 40), spacing='10', children=chips),
        W('plus_ps_histogram', 'histogram', (wl + ww - 12 - 195, wt + 12, 195, 101)),
        W('plus_ps_readout', 'readout', (wl, wt + wh - 83, ww, 83), {'label': 'F 1.8|1/125|ISO 100|50 mm'}),
        E('plus_ps_box', 'icon_box', (wl + ww - 12 - 40 - 14 - 130, wt + wh - 14 - 40, 130, 40), kind='glass', children=icons),
        E('plus_ps_box', 'full_box', (wl + ww - 12 - 40, wt + wh - 14 - 40, 40, 40), kind='glass', children=[
            W('plus_ps_icon_button', 'ib_full', (0, 0, 40, 40), {'tool_tip': T('Simple view', '簡易表示へ')}, icon='maximize'),
        ]),
        E('plus_ps_box', 'world_corners', (wl, wt, ww, wh), kind='world_corners', mouse_opaque='false'),
    ]

# ---------------------------------------------------------------- the bottom strip


PRESETS = [
    (T('Portrait', 'ポートレート'), T('Close Up', '顔アップ')),
    (T('Fashion', 'ファッション'), T('Half Body', '上半身')),
    (T('Full Body', '全身'), T('Whole Avatar', 'アバター全体')),
    (T('Low Angle', 'ローアングル'), T('Dramatic', '迫力')),
    (T('High Angle', 'ハイアングル'), T('Looking Down', '見下ろし')),
    (T('Landscape', '風景'), T('Scenery', '景色')),
    (T('Marketplace', 'マーケット'), T('Product Shot', '商品写真')),
]


def bottom():
    tabs = [
        W('plus_ps_tab', 'tab_camera', (0, 0, 120, 36), {'label': T('Camera Presets', 'カメラ')}, initial_value='true'),
        W('plus_ps_tab', 'tab_pose', (130, 0, 110, 36), {'label': T('Pose Presets', 'ポーズ')}),
        W('plus_ps_tab', 'tab_scene', (250, 0, 110, 36), {'label': T('Scene Presets', 'シーン')}),
        W('plus_ps_tab', 'tab_mine', (370, 0, 100, 36), {'label': T('My Presets', 'マイ')}),
    ]
    c = [E('plus_ps_row', 'tabs', (0, 8, 972, 36), kind='tabs', pad='14', spacing='18', children=tabs)]
    for i, (title, sub) in enumerate(PRESETS):
        extra = {}
        if i == 0:
            extra['initial_value'] = 'true'
        if i >= 5:
            extra['deletable'] = 'true'
        c.append(W('plus_ps_preset_card', 'preset_%d' % i, (12 + i * 120, 56, 108, 140),
                   {'label': title, 'subtitle': sub}, icon='camera', **extra))
    c.append(W('plus_ps_add_card', 'preset_add', (12 + 7 * 120, 56, 108, 132), {'label': T('Save Current', '現在の設定を保存')}))
    return E('plus_ps_box', 'bottom', (419, 824, 972, 244), kind='panel', children=c)

# ---------------------------------------------------------------- the right panel


def slider(name, top, label, lo, hi, inc, dec, value, unit=None, sign=False, default=None):
    """default: the value its "D" button resets to (research/272); None: no button."""
    attrs = dict(min_value=str(lo), max_value=str(hi), increment=str(inc), decimal_digits=str(dec), initial_value=str(value))
    if default is not None:
        attrs['default_value'] = str(default)
    if unit:
        attrs['unit'] = unit
    if sign:
        attrs['show_sign'] = 'true'
    return W('plus_ps_slider', name, (21, top, 363, 31), {'label': label}, **attrs)


def combo_row(name, top, label, items):
    return [
        W('plus_ps_label', 'l_' + name, (21, top, 124, 33), {'label': label}),
        E('plus_ps_combo', 'c_' + name, (157, top, 227 if name != 'focus_mode' else 186, 33), children=items),
    ]


def right_panel():
    c = [
        W('plus_ps_header', 'h_camera', (0, 0, 405, 40), {'label': T('Camera', 'カメラ')}, icon='aperture', size='right'),
        W('plus_ps_header', 'sh_lens', (21, 46, 363, 28), {'label': T('Lens', 'レンズ')}, size='sub'),
        slider('s_focal', 78, T('Focal Length', '焦点距離'), 10, 200, 1, 0, 50, unit=' mm'),
        slider('s_fov', 109, T('Field of View', '画角'), 5, 120, 0.1, 1, 39.6, unit='°'),
        slider('s_aperture', 140, T('Aperture (F)', '絞り（F 値）'), 1, 22, 0.1, 1, 1.8, default=22),
    ]
    c += combo_row('focus_mode', 172, T('Focus Mode', 'ピントの合わせ方'), [
        item('auto', T('Auto (Avatar)', '自動（アバター）')),
        item('cursor', T('Cursor', 'カーソル')),
        item('self', T('My Face', '自分の顔')),
        item('other', T('Their Face', '相手の顔')),
    ])
    c += [
        W('plus_ps_icon_button', 'ib_focus_frame', (349, 172, 35, 33), {'tool_tip': T('Show the focus frame', 'ピント枠を表示')},
          icon='scan_face', toggle='true'),
        slider('s_focus_distance', 211, T('Focus Distance', 'ピント距離'), 0.1, 50, 0.1, 1, 2.3, unit=' m'),
        W('plus_ps_divider', 'div_dof', (0, 246, 405, 9), kind='right'),
        W('plus_ps_header', 'sh_dof', (21, 256, 300, 31), {'label': T('Depth of Field', '被写界深度')}, size='sub'),
        W('plus_ps_switch', 'sw_dof', (353, 256, 31, 31), initial_value='true'),
    ]
    c += combo_row('bokeh', 291, T('Bokeh Shape', 'ボケの形'), [
        item('circle', T('Circular', '円形')),
        item('hexagon', T('Hexagon', '六角形')),
    ])
    c += [
        slider('s_bokeh', 330, T('Bokeh Intensity', 'ボケの強さ'), 0, 1, 0.1, 1, 0.6),
        W('plus_ps_header', 'h_exposure', (0, 368, 405, 40), {'label': T('Exposure & Tone', '露出と階調')}, icon='sun',
          size='right', divider='top'),
    ]
    for i, (name, label, value) in enumerate([
            ('s_exposure', T('Exposure', '露出'), 0.2), ('s_contrast', T('Contrast', 'コントラスト'), 0),
            ('s_highlights', T('Highlights', 'ハイライト'), -0.3), ('s_shadows', T('Shadows', 'シャドウ'), 0.4),
            ('s_whites', T('Whites', '白'), 0), ('s_blacks', T('Blacks', '黒'), -0.1)]):
        c.append(slider(name, 412 + i * 31, label, -1, 1, 0.1, 1, value, sign=True, default=0))   # 0: "D" dim
    c += [
        W('plus_ps_header', 'h_grading', (0, 604, 405, 40), {'label': T('Color Grading', 'カラーグレーディング')}, icon='palette',
          size='right', divider='top', collapsible='false'),
        W('plus_ps_switch', 'sw_grading', (353, 604, 31, 40), initial_value='true'),
    ]
    c += combo_row('lut', 648, T('LUT', 'ルック（LUT）'), [
        item('cinematic', T('Cinematic 01', 'シネマ 01')),
        item('warm', T('Warm', '暖色')),
        item('none', T('None', 'なし')),
    ])
    for i, (name, label, lo, hi, inc, dec, value) in enumerate([
            ('s_temperature', T('Temperature', '色温度'), -100, 100, 1, 0, 5), ('s_tint', T('Tint', '色かぶり'), -100, 100, 1, 0, 0),
            ('s_vibrance', T('Vibrance', '自然な彩度'), -1, 1, 0.1, 1, 0.2), ('s_saturation', T('Saturation', '彩度'), -1, 1, 0.1, 1, 0)]):
        c.append(slider(name, 688 + i * 31, label, lo, hi, inc, dec, value, sign=True))
    c += [
        W('plus_ps_header', 'h_effects', (0, 818, 405, 40), {'label': T('Effects', 'エフェクト')}, icon='effects',
          size='right', divider='top', initial_value='false'),
        W('plus_ps_header', 'h_output', (0, 858, 405, 40), {'label': T('Output', '出力')}, icon='output',
          size='right', divider='top', initial_value='false'),
    ]
    return E('plus_ps_box', 'right', (1399, 8, 405, 932), kind='panel', children=c)


def shutter_strip():
    c = [
        W('plus_ps_segment', 'seg_mode', (10, 40, 137, 40), {'left_label': T('Photo', '写真'), 'right_label': '360°'}),
        W('plus_ps_shutter', 'shutter_button', (158, 4, 75, 75), {'tool_tip': T('Take a picture', '撮影')}),
        W('plus_ps_icon_button', 'ib_settings', (330, 28, 44, 44), {'tool_tip': T('Photo tools', '撮影ツール')},
          icon='settings', icon_size='26'),
        W('plus_ps_label', 'l_exit', (110, 90, 285, 22), {'label': T('Press F12 to exit Photo Mode', 'F12 で撮影モードを抜ける')},
          kind='hint', align='right'),
    ]
    return E('plus_ps_box', 'shutter', (1399, 948, 405, 120), kind='panel', children=c)

# ---------------------------------------------------------------- the parts for the other windows (research/265)
# Under the picture's parts: the choice of three or more, the star, the column made by psstack.h with its
# labels beside the controls and the marks of the photo tools (filled by psgallery.cpp), and the sub-tabs.

PARTS_TOP = 1084        # under the picture's parts (1076) and a gap
PARTS_H = 430


def more_choices():
    c = [
        W('plus_ps_header', 'h_more', (0, 0, 600, 40), {'label': T('More Parts', 'ほかの窓の部品')}, icon='sliders',
          size='right'),
        W('plus_ps_label', 'l_seg3', (21, 52, 558, 22), {'label': T('A choice of three', '3 択')}),
        W('plus_ps_segment', 'seg_three', (21, 78, 558, 33),
          {'labels': T('Off|Me|Everyone', 'しない|自分|みんな')}, initial_value='1'),
        W('plus_ps_label', 'l_seg7', (21, 122, 558, 22), {'label': T('The quality levels', '画質の段')}),
        W('plus_ps_segment', 'seg_levels', (21, 148, 558, 33),
          {'labels': T('Low|Low-Mid|Mid|Mid-High|High|High-Ultra|Ultra', '低|低〜中|中|中〜高|高|高〜超高|超高')}, initial_value='3'),
        W('plus_ps_label', 'l_star', (21, 192, 558, 22), {'label': T('Show on Home (star)', 'ホームに出す（星）')}),
        W('plus_ps_icon_button', 'ib_star_off', (21, 218, 24, 24), {'tool_tip': T('Show this on the Home tab', 'ホームに出す')},
          star='true', toggle='true'),
        W('plus_ps_icon_button', 'ib_star_on', (49, 218, 24, 24), {'tool_tip': T('Show this on the Home tab', 'ホームに出す')},
          star='true', toggle='true', initial_value='true'),
    ]
    return E('plus_ps_box', 'more_choices', (8, PARTS_TOP, 600, PARTS_H), kind='panel', children=c)


def stack_sample():
    scroll = E('scroll_container', 'stack_scroll', (0, 8, 620, PARTS_H - 16), border_visible='false', opaque='false',
               reserve_scroll_corner='false', follows='left|top',
               children=[E('panel', 'stack_content', (0, 0, 610, PARTS_H - 16), follows='left|top',
                           background_visible='false', mouse_opaque='false')])
    return E('plus_ps_box', 'stack_box', (616, PARTS_TOP, 620, PARTS_H), kind='panel', children=[scroll])


# The words of the column psgallery.cpp makes (the sample's own: no setting is behind them).
STACK_WORDS = [
    S('StackHead', T('Rows with the label beside', 'ラベルが横に並ぶ行')),
    S('StackExposure', T('Exposure', '露出')),
    S('StackSoftness', T('Shadow Softness', '影の柔らかさ')),
    S('StackParticles', T('Show Particles', 'パーティクルを表示')),
    S('StackShadows', T('Shadows', '影')),
    S('StackShadows0', T('None', 'なし')),
    S('StackShadows1', T('Sun and Moon', '太陽と月')),
    S('StackShadows2', T('Sun, Moon and Lights', '太陽・月・光源')),
    S('StackGaze', T('Gaze', '視線')),
    S('StackGazeChoices', T('Off|Camera|Everyone', 'しない|カメラ|みんな')),
    S('StackTime', T('Real Sky Time', '実際の空の時刻')),
    S('StackTimeHint', T('e.g. 2026-12-14 22:00 (empty: now)', '例: 2026-12-14 22:00（空なら今）')),
    S('StackLook', T('Look', 'ルック')),
    S('StackLookPrev', T('The look before', '前のルック')),
    S('StackLookNext', T('The next look', '次のルック')),
    S('StackLook0', T('None', 'なし')),
    S('StackLook1', T('Cinematic', 'シネマ')),
    S('StackLook2', T('Warm', '暖色')),
    S('StackDetails', T('Details (2)', '詳細 (2)')),
    S('StackFog', T('Ground Fog', '地面の霧')),
    S('StackGlow', T('Fog Glow', '霧の光')),
    S('StackStarTip', T('Show this on the Home tab', 'ホームに出す')),
    S('StackChangedTip', T('Changed', '変更あり')),
    S('StackRebuildTip', T('The picture stops for a few seconds when this changes', '変えると数秒止まります')),
]

TAB_PAGES = [
    ('display', T('Display', '表示'), T('The page of the first tab.', '1 枚目のタブのページ。')),
    ('graphics', T('Graphics', 'グラフィック'), T('The page of the second tab.', '2 枚目のタブのページ。')),
    ('other', T('Other', 'その他'), T('The page of the third tab.', '3 枚目のタブのページ。')),
]


def tab_pages():
    tabs = []
    pages = []
    for i, (name, label, text) in enumerate(TAB_PAGES):
        extra = {'initial_value': 'true'} if i == 0 else {}
        tabs.append(W('plus_ps_tab', 'tab_' + name, (i * 120, 0, 110, 36), {'label': label}, **extra))
        pages.append(E('panel', 'page_' + name, (0, 92, 560, PARTS_H - 100), background_visible='false',
                       mouse_opaque='false', visible='true' if i == 0 else 'false', children=[
                           W('plus_ps_label', 'l_page_' + name, (21, 12, 518, 22), {'label': text}),
                       ]))
    c = [W('plus_ps_header', 'h_tabs', (0, 0, 560, 40), {'label': T('Sub-tabs', 'サブタブ')}, icon='sliders', size='right'),
         E('plus_ps_row', 'sub_tabs', (0, 48, 560, 36), kind='tabs', pad='14', spacing='18', children=tabs)] + pages
    return E('plus_ps_box', 'tabs_box', (1244, PARTS_TOP, 560, PARTS_H), kind='panel', children=c)

# ---------------------------------------------------------------- the compatible parts (research/266 2.1, pscompat.h)
# The official widgets in this look, for the windows whose C++ looks them up by type (the preferences): the
# rows at the preferences' width (529), the official attributes (min_val, is_toggle, radio_item ...).

COMPAT_TOP = PARTS_TOP + PARTS_H + 8
COMPAT_H = 300
COMPAT_ROW = 529        # a row of the preferences (research/266 3.)


def radio_item(name, label, rect):
    el = E('radio_item', name, rect)
    words(el, 'label', label)
    el['attrs']['value'] = name
    return el


def compat_parts():
    a, b, c = 21, 21 + COMPAT_ROW + 21, 21 + 2 * (COMPAT_ROW + 21)
    w = COMPAT_ROW
    kids = [
        W('plus_ps_header', 'h_compat', (0, 0, 1796, 40),
          {'label': T('Compatible parts (the official widgets: the preferences)', '互換の部品（公式の部品: 環境設定）')},
          icon='settings', size='right'),
        # the first column: settings rows
        W('plus_ps_check_compat', 'cc_on', (a, 52, w, 30), {'label': T('Show the tips', 'ヒントを表示')}, initial_value='true'),
        W('plus_ps_check_compat', 'cc_off', (a, 82, w, 30),
          {'label': T('Check for updates when starting', '起動時に更新を確かめる')}),
        W('plus_ps_slider_compat', 'sc_distance', (a, 112, w, 31), {'label': T('Draw Distance', '描画距離')},
          min_val='64', max_val='512', increment='32', decimal_digits='0', initial_value='256'),
        W('plus_ps_slider_compat', 'sc_scale', (a, 143, w, 31), {'label': T('UI Size', 'UI の大きさ')},
          min_val='0.75', max_val='1.5', increment='0.025', decimal_digits='3', initial_value='1', ticks='4'),
        W('plus_ps_spinner', 'sp_cache', (a, 174, w, 30), {'label': T('Cache Size (MB)', 'キャッシュ（MB）')},
          min_val='64', max_val='9984', increment='64', decimal_digits='0', initial_value='4096'),
        W('plus_ps_text', 'tx_note', (a, 210, w, 48),
          {'value': T('Text as the official text box: several lines, links such as https://secondlife.com and arguments.',
                      '公式の文字欄と同じ文字: 複数行、https://secondlife.com のようなリンク、引数。')},
          kind='hint', wrap='true'),
        # the second column: buttons and text
        W('plus_ps_button_compat', 'bc_clear', (b, 52, 254, 40), {'label': T('Clear Cache', 'キャッシュを消す')}),
        W('plus_ps_button_compat', 'bc_toggle', (b + 275, 52, 254, 40), {'label': T('Toggle (on)', '切り替え（オン）')},
          is_toggle='true', initial_value='true'),
        W('plus_ps_button_compat', 'bc_icon', (b, 100, 254, 40), {'label': T('Settings', '設定')}, icon='settings'),
        W('plus_ps_button_compat', 'bc_icon_only', (b + 275, 100, 40, 40), {'tool_tip': T('Settings', '設定')},
          icon='settings'),
    ]
    for i, (kind, text) in enumerate([
            ('label', T('A label (kind label)', 'ラベル（label）')),
            ('white', T('A value (kind white)', '値（white）')),
            ('dim', T('Not chosen (kind dim)', '選ばれていない（dim）')),
            ('sub', T('A second line (kind sub)', '2 行目（sub）')),
            ('hint', T('A hint (kind hint)', 'ヒント（hint）')),
            ('title', T('A title (kind title)', '題（title）'))]):
        kids.append(W('plus_ps_text', 'tx_' + kind, (b, 152 + i * 24, w, 24), {'value': text}, kind=kind))
    kids += [
        # the third column: choices
        W('plus_ps_label', 'l_radio_list', (c, 52, w, 22), {'label': T('A choice in a list (kind list)', '一覧の択一（list）')}),
        E('plus_ps_radio', 'rc_list', (c, 78, w, 90), initial_value='normal', children=[
            radio_item('low', T('Low', '低'), (0, 0, w, 30)),
            radio_item('normal', T('Normal', '普通'), (0, 30, w, 30)),
            radio_item('high', T('High', '高'), (0, 60, w, 30)),
        ]),
        W('plus_ps_label', 'l_radio_bar', (c, 180, w, 22), {'label': T('A choice on a bar (kind segment)', '1 本の択一（segment）')}),
        E('plus_ps_radio', 'rc_bar', (c, 206, w, 33), kind='segment', initial_value='me', children=[
            radio_item('nobody', T('Nobody', 'しない'), (0, 0, 176, 33)),
            radio_item('me', T('Me', '自分'), (176, 0, 176, 33)),
            radio_item('everyone', T('Everyone', 'みんな'), (352, 0, 177, 33)),
        ]),
    ]
    return E('plus_ps_box', 'compat_box', (8, COMPAT_TOP, 1796, COMPAT_H), kind='panel', children=kids)

# ---------------------------------------------------------------- the floater


def floater():
    height = COMPAT_TOP + COMPAT_H + 8
    sample = E('panel', 'sample', (0, 0, 1812, height), follows='left|top', background_visible='false',
               children=[E('plus_ps_box', 'ground', (0, 0, 1812, height), kind='ground'),
                         rail(), left_panel()] + world() + [bottom(), right_panel(), shutter_strip()]
               + [more_choices(), stack_sample(), tab_pages(), compat_parts()])
    scroll = E('scroll_container', 'scroll', (0, 64, 1280, 756), follows='all', border_visible='false', opaque='false',
               reserve_scroll_corner='false', children=[sample])
    top = [
        W('plus_ps_check', 'show_disabled', (12, 28, 260, 30), {'label': T('Show the disabled look', '無効の見た目を表示')}),
        W('plus_ps_label', 'hint', (290, 28, 970, 30),
          {'label': T('Sizes of the target picture at 1080 high (research/221). Hover, click and Tab to see the states.',
                      '完成イメージを 1080 の高さにした寸法（research/221）。マウスを乗せる・押す・Tab で状態を確かめる')},
          kind='hint'),
    ]
    el = E('floater', 'plus_ps_gallery', None, children=STACK_WORDS + top + [scroll])
    el['attrs'].update(positioning='cascading', legacy_header_height='18', save_rect='true', single_instance='true',
                       reuse_instance='true', can_resize='true', can_minimize='true', can_close='true',
                       height='820', width='1280', min_height='420', min_width='640', layout='topleft')
    words(el, 'title', T('Photo Screen Parts Sample (Development)', '撮影モードの部品見本（開発用）'))
    return el

# ---------------------------------------------------------------- writing


def render(el, ja, depth=0):
    pad = '  ' * depth
    attrs = el['ja'] if ja else el['attrs']
    if ja:
        attrs = dict(attrs)
        attrs['name'] = el['attrs']['name']
    kids = [k for k in el['children'] if not ja or has_ja(k)]
    if '_text' in attrs:
        return '%s<string name=%s>%s</string>\n' % (pad, quoteattr(attrs['name']), escape(attrs['_text']))
    order = ['name'] + sorted(k for k in attrs if k != 'name')
    text = pad + '<' + el['tag']
    for k in order:
        text += '\n' + pad + '  ' + k + '=' + quoteattr(str(attrs[k]), {'\n': '&#10;'})
    if not kids:
        return text + ' />\n'
    text += '>\n'
    for k in kids:
        text += render(k, ja, depth + 1)
    return text + pad + '</' + el['tag'] + '>\n'


def has_ja(el):
    return bool(el['ja']) or any(has_ja(k) for k in el['children'])


HEADER = ('<?xml version="1.0" encoding="utf-8" standalone="yes" ?>\n'
          '<!-- The photo screen\'s parts sample (plus/photoscreen/psui/psgallery.cpp, research/221).\n'
          '     Written by plus/photoscreen/psui/make_ps_gallery.py: change the script and run it again. -->\n')


def main():
    root = floater()
    for lang, ja in (('en', False), ('ja', True)):
        path = os.path.join(XUI, lang, 'floater_plus_ps_gallery.xml')
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(HEADER + render(root, ja))
        print(path)


if __name__ == '__main__':
    main()
