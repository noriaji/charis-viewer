// Made by make_ui_tokens.py from tokens.txt (research/239 (12)). Do not edit: change tokens.txt
// and run python -I make_ui_tokens.py (tools/predeliver.py (u) fails while this differs).

#include "llviewerprecompiledheaders.h"

#include "psstyle.h"

#include "lluicolortable.h"

namespace plus::psui::style
{
    namespace
    {
        struct Entry
        {
            const char* name;
            LLColor4 detail::Colors::* value;
        };

        const Entry ENTRIES[] = {
            { "PsGround",               &detail::Colors::GROUND },
            { "PsPanelTop",             &detail::Colors::PANEL_TOP },
            { "PsPanelBottom",          &detail::Colors::PANEL_BOTTOM },
            { "PsPanelMid",             &detail::Colors::PANEL_MID },
            { "PsPanelBorder",          &detail::Colors::PANEL_BORDER },
            { "PsDividerLeft",          &detail::Colors::DIVIDER_LEFT },
            { "PsDividerRight",         &detail::Colors::DIVIDER_RIGHT },
            { "PsTextWhite",            &detail::Colors::TEXT_WHITE },
            { "PsTextLabel",            &detail::Colors::TEXT_LABEL },
            { "PsTextDim",              &detail::Colors::TEXT_DIM },
            { "PsTextSub",              &detail::Colors::TEXT_SUB },
            { "PsTextHint",             &detail::Colors::TEXT_HINT },
            { "PsIconLight",            &detail::Colors::ICON_LIGHT },
            { "PsAccent",               &detail::Colors::ACCENT },
            { "PsAccentLine",           &detail::Colors::ACCENT_LINE },
            { "PsFocusRing",            &detail::Colors::FOCUS_RING },
            { "PsRail",                 &detail::Colors::RAIL },
            { "PsRailIconColor",        &detail::Colors::RAIL_ICON_COLOR },
            { "PsRailText",             &detail::Colors::RAIL_TEXT },
            { "PsRailSelLeft",          &detail::Colors::RAIL_SEL_LEFT },
            { "PsRailSelRight",         &detail::Colors::RAIL_SEL_RIGHT },
            { "PsRailSelBar",           &detail::Colors::RAIL_SEL_BAR },
            { "PsRailSelText",          &detail::Colors::RAIL_SEL_TEXT },
            { "PsRailHover",            &detail::Colors::RAIL_HOVER },
            { "PsRailPress",            &detail::Colors::RAIL_PRESS },
            { "PsHeadIconColor",        &detail::Colors::HEAD_ICON_COLOR },
            { "PsHeadChevronColor",     &detail::Colors::HEAD_CHEVRON_COLOR },
            { "PsLabel",                &detail::Colors::LABEL },
            { "PsCheckOff",             &detail::Colors::CHECK_OFF },
            { "PsCheckBorder",          &detail::Colors::CHECK_BORDER },
            { "PsCheckBorderHover",     &detail::Colors::CHECK_BORDER_HOVER },
            { "PsCheckOn",              &detail::Colors::CHECK_ON },
            { "PsCheckMarkColor",       &detail::Colors::CHECK_MARK_COLOR },
            { "PsCheckText",            &detail::Colors::CHECK_TEXT },
            { "PsSliderFillLeft",       &detail::Colors::SLIDER_FILL_LEFT },
            { "PsSliderFillRight",      &detail::Colors::SLIDER_FILL_RIGHT },
            { "PsSliderEmpty",          &detail::Colors::SLIDER_EMPTY },
            { "PsKnobTop",              &detail::Colors::KNOB_TOP },
            { "PsKnobBottom",           &detail::Colors::KNOB_BOTTOM },
            { "PsSliderTick",           &detail::Colors::SLIDER_TICK },
            { "PsValueFill",            &detail::Colors::VALUE_FILL },
            { "PsDefaultBtnColor",      &detail::Colors::DEFAULT_BTN_COLOR },
            { "PsDefaultBtnColorHover", &detail::Colors::DEFAULT_BTN_COLOR_HOVER },
            { "PsDefaultBtnFillHover",  &detail::Colors::DEFAULT_BTN_FILL_HOVER },
            { "PsSwitchOn",             &detail::Colors::SWITCH_ON },
            { "PsSwitchOff",            &detail::Colors::SWITCH_OFF },
            { "PsSwitchKnobOn",         &detail::Colors::SWITCH_KNOB_ON },
            { "PsSwitchKnobOff",        &detail::Colors::SWITCH_KNOB_OFF },
            { "PsComboFill",            &detail::Colors::COMBO_FILL },
            { "PsComboBorder",          &detail::Colors::COMBO_BORDER },
            { "PsListBg",               &detail::Colors::LIST_BG },
            { "PsListSelected",         &detail::Colors::LIST_SELECTED },
            { "PsChipFill",             &detail::Colors::CHIP_FILL },
            { "PsChipIconColor",        &detail::Colors::CHIP_ICON_COLOR },
            { "PsHistFill",             &detail::Colors::HIST_FILL },
            { "PsHistBorder",           &detail::Colors::HIST_BORDER },
            { "PsHistR",                &detail::Colors::HIST_R },
            { "PsHistG",                &detail::Colors::HIST_G },
            { "PsHistB",                &detail::Colors::HIST_B },
            { "PsHistLuma",             &detail::Colors::HIST_LUMA },
            { "PsHistBase",             &detail::Colors::HIST_BASE },
            { "PsReadoutText",          &detail::Colors::READOUT_TEXT },
            { "PsReadoutShade",         &detail::Colors::READOUT_SHADE },
            { "PsIconbtnColor",         &detail::Colors::ICONBTN_COLOR },
            { "PsIconbtnHoverColor",    &detail::Colors::ICONBTN_HOVER_COLOR },
            { "PsSegBorder",            &detail::Colors::SEG_BORDER },
            { "PsSegBorderDim",         &detail::Colors::SEG_BORDER_DIM },
            { "PsSegOnLeft",            &detail::Colors::SEG_ON_LEFT },
            { "PsSegOnRight",           &detail::Colors::SEG_ON_RIGHT },
            { "PsSegOff",               &detail::Colors::SEG_OFF },
            { "PsShutterRing",          &detail::Colors::SHUTTER_RING },
            { "PsShutterTop",           &detail::Colors::SHUTTER_TOP },
            { "PsShutterBottom",        &detail::Colors::SHUTTER_BOTTOM },
            { "PsCardFill",             &detail::Colors::CARD_FILL },
            { "PsCardBorder",           &detail::Colors::CARD_BORDER },
            { "PsPlaceholder",          &detail::Colors::PLACEHOLDER },
            { "PsPlaceholderIcon",      &detail::Colors::PLACEHOLDER_ICON },
            { "PsButtonFill",           &detail::Colors::BUTTON_FILL },
            { "PsButtonBorder",         &detail::Colors::BUTTON_BORDER },
            { "PsButtonFillHover",      &detail::Colors::BUTTON_FILL_HOVER },
            { "PsButtonBorderHover",    &detail::Colors::BUTTON_BORDER_HOVER },
            { "PsButtonFillPress",      &detail::Colors::BUTTON_FILL_PRESS },
            { "PsTabTextOn",            &detail::Colors::TAB_TEXT_ON },
            { "PsTabLineEdge",          &detail::Colors::TAB_LINE_EDGE },
            { "PsTabLineMid",           &detail::Colors::TAB_LINE_MID },
            { "PsTabGlowColor",         &detail::Colors::TAB_GLOW_COLOR },
            { "PsPresetHoverBorder",    &detail::Colors::PRESET_HOVER_BORDER },
            { "PsPresetSelectedBorder", &detail::Colors::PRESET_SELECTED_BORDER },
            { "PsPresetCloseFill",      &detail::Colors::PRESET_CLOSE_FILL },
            { "PsAddFill",              &detail::Colors::ADD_FILL },
            { "PsAddBorder",            &detail::Colors::ADD_BORDER },
            { "PsListRowText",          &detail::Colors::LIST_ROW_TEXT },
            { "PsListStarOn",           &detail::Colors::LIST_STAR_ON },
            { "PsListStarOff",          &detail::Colors::LIST_STAR_OFF },
            { "PsBadgeFill",            &detail::Colors::BADGE_FILL },
            { "PsBadgeQuiet",           &detail::Colors::BADGE_QUIET },
            { "PsBadgeText",            &detail::Colors::BADGE_TEXT },
            { "PsConvOwnText",          &detail::Colors::CONV_OWN_TEXT },
            { "PsConvSystemText",       &detail::Colors::CONV_SYSTEM_TEXT },
            { "PsPanelBg",              &detail::Colors::PANEL_BG },
            { "PsFieldBg",              &detail::Colors::FIELD_BG },
            { "PsFieldBorder",          &detail::Colors::FIELD_BORDER },
            { "PsSelectionBg",          &detail::Colors::SELECTION_BG },
            { "PsHoverBg",              &detail::Colors::HOVER_BG },
            { "PsLinkText",             &detail::Colors::LINK_TEXT },
            { "PsDividerLine",          &detail::Colors::DIVIDER_LINE },
            { "PsWindowTop",            &detail::Colors::WINDOW_TOP },
            { "PsWindowBottom",         &detail::Colors::WINDOW_BOTTOM },
            { "PsWindowBorder",         &detail::Colors::WINDOW_BORDER },
            { "PsWindowBorderFocus",    &detail::Colors::WINDOW_BORDER_FOCUS },
            { "PsTitleIcon",            &detail::Colors::TITLE_ICON },
            { "PsTitleIconPress",       &detail::Colors::TITLE_ICON_PRESS },
            { "PsTooltipFill",          &detail::Colors::TOOLTIP_FILL },
            { "PsTooltipBorder",        &detail::Colors::TOOLTIP_BORDER },
            { "PsButtonOnTop",          &detail::Colors::BUTTON_ON_TOP },
            { "PsButtonOnBottom",       &detail::Colors::BUTTON_ON_BOTTOM },
            { "PsButtonOnBorder",       &detail::Colors::BUTTON_ON_BORDER },
            { "PsScrollThumb",          &detail::Colors::SCROLL_THUMB },
            { "PsScrollTrack",          &detail::Colors::SCROLL_TRACK },
            { "PsHighlightBg",          &detail::Colors::HIGHLIGHT_BG },
            { "PsRowHoverBg",           &detail::Colors::ROW_HOVER_BG },
            { "PsSearchHitFill",        &detail::Colors::SEARCH_HIT_FILL },
            { "PsSearchHitRing",        &detail::Colors::SEARCH_HIT_RING },
        };
    }

    void applyColorTable()
    {
        const LLUIColorTable& table = LLUIColorTable::instance();
        for (const Entry& entry : ENTRIES)
        {
            if (table.colorExists(entry.name))
            {
                detail::colors.*entry.value = table.getColor(entry.name).get();
            }
        }
    }
}
