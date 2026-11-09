// Made by make_ui_tokens.py from tokens.txt (research/239 (12)). Do not edit: change tokens.txt
// and run python -I make_ui_tokens.py (tools/predeliver.py (u) fails while this differs).
//
// The values of the photo screen's look and of every Charis window (research/221, 239). The widgets
// read only these. The colours are references into detail::colors: its defaults are tokens.txt's,
// and applyColorTable() (at the start, psui's feature init) puts in what the colour table has under
// the Ps* names (skins/charis/colors.xml, made from the same table, and a user's colors.xml), so a
// later colour theme (research/239 (12) 2) changes the table and calls it again. Included by psstyle.h
// (after rgb() and FontSpec); not on its own.

#ifndef PLUS_PSTOKENS_H
#define PLUS_PSTOKENS_H

#ifndef PLUS_PSSTYLE_H
#error "include psstyle.h, not pstokens.h"
#endif

namespace plus::psui::style
{
    namespace detail
    {
        struct Colors
        {
            LLColor4 GROUND                  = rgb(0x0c111a);
            LLColor4 PANEL_TOP               = rgb(0x141b28, 0.96f);
            LLColor4 PANEL_BOTTOM            = rgb(0x09111c, 0.96f);
            LLColor4 PANEL_MID               = rgb(0x0f1622);
            LLColor4 PANEL_BORDER            = rgb(0x1c2533);
            LLColor4 DIVIDER_LEFT            = rgb(0x1b232e);
            LLColor4 DIVIDER_RIGHT           = rgb(0x323d4c);
            LLColor4 TEXT_WHITE              = rgb(0xf4f7fb);
            LLColor4 TEXT_LABEL              = rgb(0xb9c3cf);
            LLColor4 TEXT_DIM                = rgb(0x8e98a2);
            LLColor4 TEXT_SUB                = rgb(0xacb7c6);
            LLColor4 TEXT_HINT               = rgb(0x9eabbb);
            LLColor4 ICON_LIGHT              = rgb(0xd5dde8);
            LLColor4 ACCENT                  = rgb(0x3c91ef);
            LLColor4 ACCENT_LINE             = rgb(0x83cbff);
            LLColor4 FOCUS_RING              = rgb(0x3c91ef, 0.7f);
            LLColor4 RAIL                    = rgb(0x0b1421);
            LLColor4 RAIL_ICON_COLOR         = rgb(0xabb9d2);
            LLColor4 RAIL_TEXT               = rgb(0xb9c3cf);
            LLColor4 RAIL_SEL_LEFT           = rgb(0x174f9a);
            LLColor4 RAIL_SEL_RIGHT          = rgb(0x0e325d);
            LLColor4 RAIL_SEL_BAR            = rgb(0x83cbff);
            LLColor4 RAIL_SEL_TEXT           = rgb(0xe8fbff);
            LLColor4 RAIL_HOVER              = rgb(0xffffff, 0.05f);
            LLColor4 RAIL_PRESS              = rgb(0x000000, 0.1f);
            LLColor4 HEAD_ICON_COLOR         = rgb(0xd5dde8);
            LLColor4 HEAD_CHEVRON_COLOR      = rgb(0x8e98a2);
            LLColor4 LABEL                   = rgb(0xb9c3cf);
            LLColor4 CHECK_OFF               = rgb(0x111b27);
            LLColor4 CHECK_BORDER            = rgb(0x343b48);
            LLColor4 CHECK_BORDER_HOVER      = rgb(0x4a5566);
            LLColor4 CHECK_ON                = rgb(0x5aa0ee);
            LLColor4 CHECK_MARK_COLOR        = rgb(0xffffff);
            LLColor4 CHECK_TEXT              = rgb(0xc9d1dc);
            LLColor4 SLIDER_FILL_LEFT        = rgb(0x7d879e);
            LLColor4 SLIDER_FILL_RIGHT       = rgb(0xb3bccf);
            LLColor4 SLIDER_EMPTY            = rgb(0x363e4d);
            LLColor4 KNOB_TOP                = rgb(0xd0daf0);
            LLColor4 KNOB_BOTTOM             = rgb(0xaab6d0);
            LLColor4 SLIDER_TICK             = rgb(0x4a5566);
            LLColor4 VALUE_FILL              = rgb(0x0c121b);
            LLColor4 DEFAULT_BTN_COLOR       = rgb(0x8e98a2);
            LLColor4 DEFAULT_BTN_COLOR_HOVER = rgb(0xf4f7fb);
            LLColor4 DEFAULT_BTN_FILL_HOVER  = rgb(0xffffff, 0.05f);
            LLColor4 SWITCH_ON               = rgb(0x408fe6);
            LLColor4 SWITCH_OFF              = rgb(0x363e4d);
            LLColor4 SWITCH_KNOB_ON          = rgb(0xffffff);
            LLColor4 SWITCH_KNOB_OFF         = rgb(0x8e98a2);
            LLColor4 COMBO_FILL              = rgb(0x202a40, 0.8f);
            LLColor4 COMBO_BORDER            = rgb(0x3a4458);
            LLColor4 LIST_BG                 = rgb(0x141b28);
            LLColor4 LIST_SELECTED           = rgb(0x1a4172);
            LLColor4 CHIP_FILL               = rgb(0x0e1119, 0.7f);
            LLColor4 CHIP_ICON_COLOR         = rgb(0xd5dde8);
            LLColor4 HIST_FILL               = rgb(0x1a2132, 0.75f);
            LLColor4 HIST_BORDER             = rgb(0x404964, 0.6f);
            LLColor4 HIST_R                  = rgb(0xe2524a, 0.45f);
            LLColor4 HIST_G                  = rgb(0x46c06a, 0.45f);
            LLColor4 HIST_B                  = rgb(0x3d7ff0, 0.45f);
            LLColor4 HIST_LUMA               = rgb(0xd9dde6, 0.55f);
            LLColor4 HIST_BASE               = rgb(0x5a6378);
            LLColor4 READOUT_TEXT            = rgb(0xf8f8fa);
            LLColor4 READOUT_SHADE           = rgb(0x000000, 0.35f);
            LLColor4 ICONBTN_COLOR           = rgb(0xf2f7ff);
            LLColor4 ICONBTN_HOVER_COLOR     = rgb(0xffffff, 0.1f);
            LLColor4 SEG_BORDER              = rgb(0x376386);
            LLColor4 SEG_BORDER_DIM          = rgb(0x26354a);
            LLColor4 SEG_ON_LEFT             = rgb(0x2b5185);
            LLColor4 SEG_ON_RIGHT            = rgb(0x1e3858);
            LLColor4 SEG_OFF                 = rgb(0x1d2838);
            LLColor4 SHUTTER_RING            = rgb(0xedf4ff);
            LLColor4 SHUTTER_TOP             = rgb(0xf7f9fc);
            LLColor4 SHUTTER_BOTTOM          = rgb(0xe4e8f0);
            LLColor4 CARD_FILL               = rgb(0x0c1522);
            LLColor4 CARD_BORDER             = rgb(0x2a3342);
            LLColor4 PLACEHOLDER             = rgb(0x141b28);
            LLColor4 PLACEHOLDER_ICON        = rgb(0x5a6378);
            LLColor4 BUTTON_FILL             = rgb(0x0e1721);
            LLColor4 BUTTON_BORDER           = rgb(0x333a45);
            LLColor4 BUTTON_FILL_HOVER       = rgb(0x152030);
            LLColor4 BUTTON_BORDER_HOVER     = rgb(0x4a5566);
            LLColor4 BUTTON_FILL_PRESS       = rgb(0x0a111a);
            LLColor4 TAB_TEXT_ON             = rgb(0xedfeff);
            LLColor4 TAB_LINE_EDGE           = rgb(0x3c91ef);
            LLColor4 TAB_LINE_MID            = rgb(0xa9dafc);
            LLColor4 TAB_GLOW_COLOR          = rgb(0x3c91ef, 0.25f);
            LLColor4 PRESET_HOVER_BORDER     = rgb(0x83cbff);
            LLColor4 PRESET_SELECTED_BORDER  = rgb(0x3c91ef);
            LLColor4 PRESET_CLOSE_FILL       = rgb(0x000000, 0.6f);
            LLColor4 ADD_FILL                = rgb(0x0b121e);
            LLColor4 ADD_BORDER              = rgb(0x242b36);
            LLColor4 LIST_ROW_TEXT           = rgb(0xc9d1dc);
            LLColor4 LIST_STAR_ON            = rgb(0xf2c14e);
            LLColor4 LIST_STAR_OFF           = rgb(0x5a6378);
            LLColor4 BADGE_FILL              = rgb(0x3c91ef);
            LLColor4 BADGE_QUIET             = rgb(0x363e4d);
            LLColor4 BADGE_TEXT              = rgb(0xffffff);
            LLColor4 CONV_OWN_TEXT           = rgb(0xacb7c6);
            LLColor4 CONV_SYSTEM_TEXT        = rgb(0x8e98a2);
            LLColor4 PANEL_BG                = rgb(0x0f1622);
            LLColor4 FIELD_BG                = rgb(0x0c121b);
            LLColor4 FIELD_BORDER            = rgb(0x343b48);
            LLColor4 SELECTION_BG            = rgb(0x1a4172);
            LLColor4 HOVER_BG                = rgb(0xffffff, 0.05f);
            LLColor4 LINK_TEXT               = rgb(0x83cbff);
            LLColor4 DIVIDER_LINE            = rgb(0x323d4c);
            LLColor4 WINDOW_TOP              = rgb(0x141b28, 0.96f);
            LLColor4 WINDOW_BOTTOM           = rgb(0x09111c, 0.96f);
            LLColor4 WINDOW_BORDER           = rgb(0x1c2533);
            LLColor4 WINDOW_BORDER_FOCUS     = rgb(0x323d4c);
            LLColor4 TITLE_ICON              = rgb(0xd5dde8);
            LLColor4 TITLE_ICON_PRESS        = rgb(0x83cbff);
            LLColor4 TOOLTIP_FILL            = rgb(0x141b28);
            LLColor4 TOOLTIP_BORDER          = rgb(0x1c2533);
            LLColor4 BUTTON_ON_TOP           = rgb(0x2b5185);
            LLColor4 BUTTON_ON_BOTTOM        = rgb(0x1e3858);
            LLColor4 BUTTON_ON_BORDER        = rgb(0x376386);
            LLColor4 SCROLL_THUMB            = rgb(0x4a5566);
            LLColor4 SCROLL_TRACK            = rgb(0x0c111a, 0.35f);
            LLColor4 HIGHLIGHT_BG            = rgb(0x1a4172);
            LLColor4 ROW_HOVER_BG            = rgb(0x1d2838);
            LLColor4 SEARCH_HIT_FILL         = rgb(0x3c91ef, 0.18f);
            LLColor4 SEARCH_HIT_RING         = rgb(0x83cbff, 0.8f);
            LLColor4 SHORTCUT_FILL           = rgb(0x0e1119, 0.7f);
            LLColor4 SHORTCUT_ON_LEFT        = rgb(0x2b5185, 0.9f);
            LLColor4 SHORTCUT_ON_RIGHT       = rgb(0x1e3858, 0.9f);
        };
        inline Colors colors;
    }

    // ---- 0. the ground ----------------------------------------------------------------------
    inline const LLColor4& GROUND                  = detail::colors.GROUND;   // between the panels, around the world
    inline const LLColor4& PANEL_TOP               = detail::colors.PANEL_TOP;
    inline const LLColor4& PANEL_BOTTOM            = detail::colors.PANEL_BOTTOM;
    inline const LLColor4& PANEL_MID               = detail::colors.PANEL_MID;   // what is behind a picture on a panel
    inline const LLColor4& PANEL_BORDER            = detail::colors.PANEL_BORDER;
    constexpr F32 PANEL_RADIUS                      = 12.f;
    constexpr S32 PANEL_GAP                         = 8;
    inline const LLColor4& DIVIDER_LEFT            = detail::colors.DIVIDER_LEFT;
    inline const LLColor4& DIVIDER_RIGHT           = detail::colors.DIVIDER_RIGHT;
    constexpr S32 DIVIDER_MARGIN                    = 14;
    inline const LLColor4& TEXT_WHITE              = detail::colors.TEXT_WHITE;
    inline const LLColor4& TEXT_LABEL              = detail::colors.TEXT_LABEL;
    inline const LLColor4& TEXT_DIM                = detail::colors.TEXT_DIM;   // tabs not chosen, the half not chosen
    inline const LLColor4& TEXT_SUB                = detail::colors.TEXT_SUB;   // second lines
    inline const LLColor4& TEXT_HINT               = detail::colors.TEXT_HINT;   // "F12 to leave"
    inline const LLColor4& ICON_LIGHT              = detail::colors.ICON_LIGHT;
    inline const LLColor4& ACCENT                  = detail::colors.ACCENT;
    inline const LLColor4& ACCENT_LINE             = detail::colors.ACCENT_LINE;
    inline const LLColor4& FOCUS_RING              = detail::colors.FOCUS_RING;   // keyboard focus (not in the picture)
    constexpr F32 WORLD_RADIUS                      = 10.f;

    // ---- states (not in the picture: fixed steps) -------------------------------------------
    constexpr F32 STATE_HOVER                       = 0.06f;   // hovered: the fill this much towards white
    constexpr F32 STATE_PRESS                       = 0.06f;   // pressed: this much towards black
    constexpr F32 STATE_DISABLED                    = 0.4f;   // disabled: the whole part at this opacity

    // ---- 1. the rail ------------------------------------------------------------------------
    inline const LLColor4& RAIL                    = detail::colors.RAIL;
    inline const LLColor4& RAIL_ICON_COLOR         = detail::colors.RAIL_ICON_COLOR;
    inline const LLColor4& RAIL_TEXT               = detail::colors.RAIL_TEXT;
    inline const LLColor4& RAIL_SEL_LEFT           = detail::colors.RAIL_SEL_LEFT;
    inline const LLColor4& RAIL_SEL_RIGHT          = detail::colors.RAIL_SEL_RIGHT;
    inline const LLColor4& RAIL_SEL_BAR            = detail::colors.RAIL_SEL_BAR;
    inline const LLColor4& RAIL_SEL_TEXT           = detail::colors.RAIL_SEL_TEXT;
    inline const LLColor4& RAIL_HOVER              = detail::colors.RAIL_HOVER;
    inline const LLColor4& RAIL_PRESS              = detail::colors.RAIL_PRESS;
    constexpr S32 RAIL_WIDTH                        = 123;
    constexpr S32 RAIL_TAB_HEIGHT                   = 83;
    constexpr S32 RAIL_ICON                         = 26;
    constexpr S32 RAIL_ICON_TOP                     = 15;   // from the tab's top to the icon's top
    constexpr S32 RAIL_TEXT_TOP                     = 49;   // from the tab's top to the first line's top
    constexpr F32 RAIL_BAR_WIDTH                    = 3.f;

    // ---- 2. headings ------------------------------------------------------------------------
    constexpr S32 HEAD_PAD                          = 14;   // left and right
    constexpr S32 HEAD_ICON                         = 21;
    constexpr S32 HEAD_ICON_RIGHT                   = 19;   // the right panel's headings
    constexpr S32 HEAD_ICON_GAP                     = 10;
    constexpr S32 HEAD_CHEVRON                      = 14;
    inline const LLColor4& HEAD_ICON_COLOR         = detail::colors.HEAD_ICON_COLOR;
    inline const LLColor4& HEAD_CHEVRON_COLOR      = detail::colors.HEAD_CHEVRON_COLOR;

    // ---- 3. small labels --------------------------------------------------------------------
    inline const LLColor4& LABEL                   = detail::colors.LABEL;

    // ---- 4. check boxes ---------------------------------------------------------------------
    constexpr S32 CHECK_BOX                         = 21;
    constexpr F32 CHECK_RADIUS                      = 4.f;
    constexpr S32 CHECK_GAP                         = 10;
    constexpr S32 CHECK_MARK                        = 17;
    inline const LLColor4& CHECK_OFF               = detail::colors.CHECK_OFF;
    inline const LLColor4& CHECK_BORDER            = detail::colors.CHECK_BORDER;
    inline const LLColor4& CHECK_BORDER_HOVER      = detail::colors.CHECK_BORDER_HOVER;
    inline const LLColor4& CHECK_ON                = detail::colors.CHECK_ON;
    inline const LLColor4& CHECK_MARK_COLOR        = detail::colors.CHECK_MARK_COLOR;
    inline const LLColor4& CHECK_TEXT              = detail::colors.CHECK_TEXT;

    // ---- 5. sliders -------------------------------------------------------------------------
    constexpr S32 SLIDER_LABEL_WIDTH                = 124;
    constexpr S32 SLIDER_GAP                        = 12;   // label - track - value box
    constexpr F32 SLIDER_TRACK                      = 4.f;
    constexpr F32 SLIDER_KNOB                       = 16.f;
    constexpr F32 SLIDER_KNOB_PRESSED               = 18.f;
    inline const LLColor4& SLIDER_FILL_LEFT        = detail::colors.SLIDER_FILL_LEFT;
    inline const LLColor4& SLIDER_FILL_RIGHT       = detail::colors.SLIDER_FILL_RIGHT;
    inline const LLColor4& SLIDER_EMPTY            = detail::colors.SLIDER_EMPTY;
    inline const LLColor4& KNOB_TOP                = detail::colors.KNOB_TOP;
    inline const LLColor4& KNOB_BOTTOM             = detail::colors.KNOB_BOTTOM;
    inline const LLColor4& SLIDER_TICK             = detail::colors.SLIDER_TICK;
    constexpr S32 VALUE_WIDTH                       = 62;
    constexpr S32 VALUE_HEIGHT                      = 26;
    constexpr F32 VALUE_RADIUS                      = 4.f;
    constexpr S32 VALUE_PAD                         = 8;
    inline const LLColor4& VALUE_FILL              = detail::colors.VALUE_FILL;
    constexpr S32 DEFAULT_BTN_WIDTH                 = 22;   // the "D" button that resets to the shipped default, after the value box
    constexpr S32 DEFAULT_BTN_GAP                   = 8;   // value box - "D" (narrower than SLIDER_GAP: the track keeps more room)
    inline const LLColor4& DEFAULT_BTN_COLOR       = detail::colors.DEFAULT_BTN_COLOR;
    inline const LLColor4& DEFAULT_BTN_COLOR_HOVER = detail::colors.DEFAULT_BTN_COLOR_HOVER;
    inline const LLColor4& DEFAULT_BTN_FILL_HOVER  = detail::colors.DEFAULT_BTN_FILL_HOVER;

    // ---- 6. switches ------------------------------------------------------------------------
    constexpr F32 SWITCH_WIDTH                      = 31.f;
    constexpr F32 SWITCH_HEIGHT                     = 17.f;
    constexpr F32 SWITCH_KNOB                       = 12.f;
    constexpr F32 SWITCH_KNOB_INSET                 = 2.5f;
    constexpr F32 SWITCH_SECONDS                    = 0.12f;
    inline const LLColor4& SWITCH_ON               = detail::colors.SWITCH_ON;
    inline const LLColor4& SWITCH_OFF              = detail::colors.SWITCH_OFF;
    inline const LLColor4& SWITCH_KNOB_ON          = detail::colors.SWITCH_KNOB_ON;
    inline const LLColor4& SWITCH_KNOB_OFF         = detail::colors.SWITCH_KNOB_OFF;

    // ---- 7. drop-downs ----------------------------------------------------------------------
    constexpr F32 COMBO_RADIUS                      = 6.f;
    constexpr S32 COMBO_PAD                         = 10;
    constexpr S32 COMBO_CHEVRON                     = 14;
    inline const LLColor4& COMBO_FILL              = detail::colors.COMBO_FILL;
    inline const LLColor4& COMBO_BORDER            = detail::colors.COMBO_BORDER;
    inline const LLColor4& LIST_BG                 = detail::colors.LIST_BG;
    inline const LLColor4& LIST_SELECTED           = detail::colors.LIST_SELECTED;

    // ---- 8. chips (12. the buttons in the world use the same box) ---------------------------
    constexpr F32 CHIP_RADIUS                       = 7.f;
    constexpr S32 CHIP_HEIGHT                       = 40;
    constexpr S32 CHIP_PAD                          = 12;
    constexpr S32 CHIP_ICON                         = 19;
    constexpr S32 CHIP_GAP                          = 10;   // icon - text - chevron; and between chips
    inline const LLColor4& CHIP_FILL               = detail::colors.CHIP_FILL;
    inline const LLColor4& CHIP_ICON_COLOR         = detail::colors.CHIP_ICON_COLOR;

    // ---- 9. histogram -----------------------------------------------------------------------
    constexpr F32 HIST_RADIUS                       = 7.f;
    constexpr S32 HIST_PAD                          = 8;
    inline const LLColor4& HIST_FILL               = detail::colors.HIST_FILL;
    inline const LLColor4& HIST_BORDER             = detail::colors.HIST_BORDER;
    inline const LLColor4& HIST_R                  = detail::colors.HIST_R;
    inline const LLColor4& HIST_G                  = detail::colors.HIST_G;
    inline const LLColor4& HIST_B                  = detail::colors.HIST_B;
    inline const LLColor4& HIST_LUMA               = detail::colors.HIST_LUMA;
    inline const LLColor4& HIST_BASE               = detail::colors.HIST_BASE;

    // ---- 11. the exposure read-out ----------------------------------------------------------
    constexpr S32 READOUT_LEFT                      = 26;
    constexpr S32 READOUT_BOTTOM                    = 24;
    constexpr S32 READOUT_GAP                       = 24;
    inline const LLColor4& READOUT_TEXT            = detail::colors.READOUT_TEXT;
    inline const LLColor4& READOUT_SHADE           = detail::colors.READOUT_SHADE;

    // ---- 12. icon buttons in the world ------------------------------------------------------
    constexpr S32 ICONBTN_ICON                      = 21;
    constexpr F32 ICONBTN_HOVER                     = 31.f;
    inline const LLColor4& ICONBTN_COLOR           = detail::colors.ICONBTN_COLOR;
    inline const LLColor4& ICONBTN_HOVER_COLOR     = detail::colors.ICONBTN_HOVER_COLOR;

    // ---- 13. the right panel ----------------------------------------------------------------
    constexpr S32 ROW_MARGIN                        = 21;

    // ---- 14. the shutter strip --------------------------------------------------------------
    constexpr F32 SEG_RADIUS                        = 7.f;
    inline const LLColor4& SEG_BORDER              = detail::colors.SEG_BORDER;
    inline const LLColor4& SEG_BORDER_DIM          = detail::colors.SEG_BORDER_DIM;
    inline const LLColor4& SEG_ON_LEFT             = detail::colors.SEG_ON_LEFT;
    inline const LLColor4& SEG_ON_RIGHT            = detail::colors.SEG_ON_RIGHT;
    inline const LLColor4& SEG_OFF                 = detail::colors.SEG_OFF;
    constexpr F32 SEG_SEPARATOR_INSET               = 6.f;   // the line between two choices not chosen (three or more), off the top and bottom (not in the picture, in its style)
    inline const LLColor4& SHUTTER_RING            = detail::colors.SHUTTER_RING;
    inline const LLColor4& SHUTTER_TOP             = detail::colors.SHUTTER_TOP;
    inline const LLColor4& SHUTTER_BOTTOM          = detail::colors.SHUTTER_BOTTOM;
    constexpr F32 SHUTTER_RING_WIDTH                = 3.5f;
    constexpr F32 SHUTTER_GROOVE                    = 3.5f;
    constexpr F32 SHUTTER_PRESSED_SCALE             = 0.92f;
    constexpr F32 SHUTTER_BUSY_SECONDS              = 0.2f;
    constexpr S32 GEAR_ICON                         = 26;

    // ---- 15. cards with a picture -----------------------------------------------------------
    constexpr F32 CARD_RADIUS                       = 7.f;
    constexpr S32 CARD_PICTURE_WIDTH                = 65;
    constexpr S32 CARD_TEXT_GAP                     = 12;
    constexpr S32 CARD_MORE_ICON                    = 19;
    inline const LLColor4& CARD_FILL               = detail::colors.CARD_FILL;
    inline const LLColor4& CARD_BORDER             = detail::colors.CARD_BORDER;
    inline const LLColor4& PLACEHOLDER             = detail::colors.PLACEHOLDER;
    inline const LLColor4& PLACEHOLDER_ICON        = detail::colors.PLACEHOLDER_ICON;

    // ---- 16. buttons ------------------------------------------------------------------------
    constexpr F32 BUTTON_RADIUS                     = 7.f;
    inline const LLColor4& BUTTON_FILL             = detail::colors.BUTTON_FILL;
    inline const LLColor4& BUTTON_BORDER           = detail::colors.BUTTON_BORDER;
    inline const LLColor4& BUTTON_FILL_HOVER       = detail::colors.BUTTON_FILL_HOVER;
    inline const LLColor4& BUTTON_BORDER_HOVER     = detail::colors.BUTTON_BORDER_HOVER;
    inline const LLColor4& BUTTON_FILL_PRESS       = detail::colors.BUTTON_FILL_PRESS;

    // ---- 17. the bottom strip's tabs --------------------------------------------------------
    constexpr S32 TAB_PAD                           = 14;   // left and right of the text
    constexpr F32 TAB_LINE                          = 2.f;
    constexpr F32 TAB_LINE_INSET                    = 4.f;
    constexpr F32 TAB_GLOW                          = 6.f;
    inline const LLColor4& TAB_TEXT_ON             = detail::colors.TAB_TEXT_ON;
    inline const LLColor4& TAB_LINE_EDGE           = detail::colors.TAB_LINE_EDGE;
    inline const LLColor4& TAB_LINE_MID            = detail::colors.TAB_LINE_MID;
    inline const LLColor4& TAB_GLOW_COLOR          = detail::colors.TAB_GLOW_COLOR;

    // ---- 18. preset cards -------------------------------------------------------------------
    constexpr S32 PRESET_PICTURE_HEIGHT             = 90;
    constexpr S32 PRESET_TEXT_GAP                   = 8;
    constexpr S32 PRESET_GAP                        = 12;
    constexpr S32 PRESET_CLOSE                      = 16;
    inline const LLColor4& PRESET_HOVER_BORDER     = detail::colors.PRESET_HOVER_BORDER;
    inline const LLColor4& PRESET_SELECTED_BORDER  = detail::colors.PRESET_SELECTED_BORDER;
    inline const LLColor4& PRESET_CLOSE_FILL       = detail::colors.PRESET_CLOSE_FILL;
    inline const LLColor4& ADD_FILL                = detail::colors.ADD_FILL;
    inline const LLColor4& ADD_BORDER              = detail::colors.ADD_BORDER;
    constexpr S32 ADD_ICON                          = 31;

    // ---- 19. list rows (the sky and water lists of the scene tab; not in the picture, in its style) ----
    constexpr S32 LIST_ROW_HEIGHT                   = 30;
    constexpr S32 LIST_ROW_PAD                      = 10;   // left of the text, right of the star
    constexpr F32 LIST_ROW_RADIUS                   = 6.f;
    constexpr F32 LIST_ROW_BAR                      = 3.f;   // the chosen row's bar at the left
    constexpr S32 LIST_STAR                         = 16;
    constexpr S32 LIST_STAR_HIT                     = 28;   // the star's clickable width
    inline const LLColor4& LIST_ROW_TEXT           = detail::colors.LIST_ROW_TEXT;
    inline const LLColor4& LIST_STAR_ON            = detail::colors.LIST_STAR_ON;
    inline const LLColor4& LIST_STAR_OFF           = detail::colors.LIST_STAR_OFF;

    // ---- 20. badges (the conversations tab's unread count; not in the picture, in its style) ----
    constexpr S32 BADGE_HEIGHT                      = 18;
    constexpr S32 BADGE_PAD                         = 5;   // left and right of the number (one digit: a circle)
    constexpr S32 BADGE_MAX                         = 9;   // more is shown as "9+"
    inline const LLColor4& BADGE_FILL              = detail::colors.BADGE_FILL;   // unread the official settings tell of
    inline const LLColor4& BADGE_QUIET             = detail::colors.BADGE_QUIET;   // unread they leave quiet ("no action", muted)
    inline const LLColor4& BADGE_TEXT              = detail::colors.BADGE_TEXT;

    // ---- 21. the conversations tab's history (not in the picture, in its style) -------------
    inline const LLColor4& CONV_OWN_TEXT           = detail::colors.CONV_OWN_TEXT;   // what you sent
    inline const LLColor4& CONV_SYSTEM_TEXT        = detail::colors.CONV_SYSTEM_TEXT;   // the viewer's own notes

    // ---- 22. the names the XUI windows use (colors.xml Ps*; besides PsGround, PsTextWhite ... PsAccent of 0.) ----
    inline const LLColor4& PANEL_BG                = detail::colors.PANEL_BG;   // a flat panel inside a window (the window draws the gradient itself)
    inline const LLColor4& FIELD_BG                = detail::colors.FIELD_BG;   // a box to type in
    inline const LLColor4& FIELD_BORDER            = detail::colors.FIELD_BORDER;
    inline const LLColor4& SELECTION_BG            = detail::colors.SELECTION_BG;   // the chosen row of a list (a highlighted menu item is HIGHLIGHT_BG)
    inline const LLColor4& HOVER_BG                = detail::colors.HOVER_BG;   // a row under the mouse
    inline const LLColor4& LINK_TEXT               = detail::colors.LINK_TEXT;   // links and emphasised words
    inline const LLColor4& DIVIDER_LINE            = detail::colors.DIVIDER_LINE;   // a line between parts

    // ---- 23. the skin (skins/charis: the frame of every window and the official parts not replaced yet) ----
    inline const LLColor4& WINDOW_TOP              = detail::colors.WINDOW_TOP;   // a window's gradient (also toasts and inspectors)
    inline const LLColor4& WINDOW_BOTTOM           = detail::colors.WINDOW_BOTTOM;
    inline const LLColor4& WINDOW_BORDER           = detail::colors.WINDOW_BORDER;   // a window behind others
    inline const LLColor4& WINDOW_BORDER_FOCUS     = detail::colors.WINDOW_BORDER_FOCUS;   // the window in front
    constexpr F32 WINDOW_RADIUS                     = 12.f;
    inline const LLColor4& TITLE_ICON              = detail::colors.TITLE_ICON;   // close, minimise, dock, help in the title bar
    inline const LLColor4& TITLE_ICON_PRESS        = detail::colors.TITLE_ICON_PRESS;
    constexpr S32 TITLE_ICON_SIZE                   = 16;   // the official UIFloaterCloseBoxSize
    inline const LLColor4& TOOLTIP_FILL            = detail::colors.TOOLTIP_FILL;
    inline const LLColor4& TOOLTIP_BORDER          = detail::colors.TOOLTIP_BORDER;
    constexpr F32 TOOLTIP_RADIUS                    = 6.f;
    inline const LLColor4& BUTTON_ON_TOP           = detail::colors.BUTTON_ON_TOP;   // an official button that is on (or pressed): blue, as the segments
    inline const LLColor4& BUTTON_ON_BOTTOM        = detail::colors.BUTTON_ON_BOTTOM;
    inline const LLColor4& BUTTON_ON_BORDER        = detail::colors.BUTTON_ON_BORDER;
    inline const LLColor4& SCROLL_THUMB            = detail::colors.SCROLL_THUMB;
    inline const LLColor4& SCROLL_TRACK            = detail::colors.SCROLL_TRACK;   // only its opacity counts: the official track is drawn in ScrollbarTrackColor
    constexpr F32 SCROLL_WIDTH                      = 6.f;   // the thumb's width in the official 15 px bar
    constexpr F32 SCROLL_RADIUS                     = 3.f;
    inline const LLColor4& HIGHLIGHT_BG            = detail::colors.HIGHLIGHT_BG;   // a highlighted menu item, the inventory's chosen row: as a list's chosen row everywhere (2026-11-06, research/275 L181; was ACCENT/0.75, white text 4.8:1 and the link blue 2.8:1 on it, 10:1 and 5.9:1 on this). The gallery's chosen thumbnail is framed in PRESET_SELECTED_BORDER
    inline const LLColor4& ROW_HOVER_BG            = detail::colors.ROW_HOVER_BG;   // an official list row under the mouse, a notification's box: opaque, it must stand out from the ground

    // ---- 24. the shader reload notice (research/250, plus/shaderreload: a chip with a track under its text; not in the picture, in its style) ----
    constexpr S32 RELOAD_PAD_Y                      = 9;   // the box's top and bottom to what is in it
    constexpr S32 RELOAD_TEXT_LINE                  = 16;   // the body text's line (FONT_BODY)
    constexpr S32 RELOAD_BAR_GAP                    = 8;   // the text to the track (SLIDER_TRACK high, SLIDER_EMPTY and ACCENT)
    constexpr S32 RELOAD_MARGIN                     = 24;   // the bottom of the world between the toolbars to the box

    // ---- 25. the focus hint (research/264, plus/focuslock/focushint.cpp: a chip at the top of the world while the focus is picked by clicking; not in the picture, in its style) ----
    constexpr S32 FOCUS_HINT_MARGIN                 = 24;   // the top of the world (between the toolbars) to the box
    constexpr S32 FOCUS_HINT_GAP                    = 8;   // in the photo screen: its parts at the top of the world (the chips, the histogram) to the box

    // ---- 26. the compatible widgets (research/266 2.1, pscompat.h: the official widgets drawn in this look; not in the picture, in its style) ----
    constexpr S32 BUTTON_ICON                       = 19;   // an icon on a button (before its words, or alone in the middle)
    constexpr S32 BUTTON_PAD                        = 12;   // the button's edge to its icon
    constexpr S32 RADIO_SIZE                        = 18;   // the round mark of a choice in a list (the check box's colours)
    constexpr F32 RADIO_DOT                         = 8.f;   // the dot in the chosen one
    constexpr S32 SPIN_ARROW_WIDTH                  = 18;   // the up and down arrows at the right of a spinner's value box
    constexpr S32 SPIN_ARROW                        = 10;   // their chevrons
    inline const LLColor4& SEARCH_HIT_FILL         = detail::colors.SEARCH_HIT_FILL;   // a part the preferences' search found: the ground under it
    inline const LLColor4& SEARCH_HIT_RING         = detail::colors.SEARCH_HIT_RING;   // and the line around it
    constexpr F32 SEARCH_HIT_RADIUS                 = 6.f;

    // ---- 27. tab containers (plus_ps_tabs, research/266 prefs-frame; not in the picture, in its style) ----
    constexpr S32 TABS_RAIL_WIDTH                   = 150;   // tab_position left: the rail's width, where the pages start (ja's longest name: 9 x 13 + 28)
    constexpr S32 TABS_ROW_HEIGHT                   = 32;   // a row of the rail while there is room
    constexpr S32 TABS_ROW_MIN                      = 24;   // the least: a short window's rows (below it the rail scrolls)
    constexpr S32 TABS_ROW_GAP                      = 8;   // the rail's rows end this far from the pages
    constexpr S32 TABS_TEXT_PAD                     = 14;   // a row's text from its left
    constexpr S32 TABS_TOP_HEIGHT                   = 36;   // tab_position top: the strip of tabs over the pages
    constexpr S32 TABS_TOP_PAD                      = 14;   // before the first tab (plus_ps_row's pad)
    constexpr S32 TABS_TOP_GAP                      = 10;   // between two tabs

    // ---- 28. the shortcut bar (plus/shortcutbar: the text buttons of the row under the navigation bar, over the world; not in the picture, in its style) ----
    constexpr S32 SHORTCUT_BTN_HEIGHT               = 22;   // centred in the row (main_view.xml shortcut_bar_container, 24 high)
    constexpr F32 SHORTCUT_BTN_RADIUS               = 6.f;
    constexpr S32 SHORTCUT_BTN_PAD                  = 10;   // the text to the button's left and right edge
    constexpr S32 SHORTCUT_BTN_GAP                  = 6;   // between two buttons
    constexpr S32 SHORTCUT_EDGE                     = 6;   // the window's left edge to the first button
    inline const LLColor4& SHORTCUT_FILL           = detail::colors.SHORTCUT_FILL;   // a button over the world: the world button's translucent navy
    inline const LLColor4& SHORTCUT_ON_LEFT        = detail::colors.SHORTCUT_ON_LEFT;   // a menu item that is on (a check mark in the menu): blue, as the chosen segment
    inline const LLColor4& SHORTCUT_ON_RIGHT       = detail::colors.SHORTCUT_ON_RIGHT;

    // ---- fonts (the official sizes nearest to the picture's: research/221 "文字") -------------
    constexpr FontSpec FONT_TITLE                   = { "Huge",   0 };   // headings of the left panel (20 px)
    constexpr FontSpec FONT_HEAD                    = { "Large",  0 };   // headings of the right panel, sub-headings (17-18 px)
    constexpr FontSpec FONT_BODY                    = { "Medium", 0 };   // labels, rows, values (15 px)
    constexpr FontSpec FONT_BUTTON                  = { "Large",  0 };   // buttons, chips, card titles (17 px)
    constexpr FontSpec FONT_SMALL                   = { "Small",  0 };   // second lines, the hint (13-14 px)
    constexpr FontSpec FONT_READOUT                 = { "Large",  0 };   // the exposure read-out (18 px)
    // Font sizes are in points (fonts.xml: Small 9, Medium 10, Large 11, Huge 16); the picture's pixel heights fall
    // between them, so these are compared in the parts sample and moved here.

    // Puts the colour table's Ps* colours into detail::colors (those it has; the others keep
    // tokens.txt's). Once at the start; again after the table changes.
    void applyColorTable();
}

#endif // PLUS_PSTOKENS_H
