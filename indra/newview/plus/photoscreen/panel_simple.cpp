/**
 * @file panel_simple.cpp
 * @brief The simple view of the photo screen (research/218).
 *
 * What is here: panel_plus_photoscreen_simple.xml laid on photoscreen's overlay while the simple mode is on
 * (the world fills the window there), and the parts that are not widgets:
 *  - the rail of six tabs at the left and the see-through panel of the one that is open: Focus (plus/focuslock),
 *    Mood (the six looks of plus/photomode, the same settings as the photo tools' presets), Framing (composition.cpp),
 *    Sky and water (a list of the inventory's skies or waters, a search, arrows; photomode's applyEnvironmentItem),
 *    Camera spot (photomode's temporary view: keep, go back; the flycam on / off, plus/flycammouse), Talk (the IM and
 *    group chats: photoconv_panel.cpp, with the unread count on the tab);
 *  - the strip at the bottom middle ("Focus: ... Mood: ...", led by "Flycam on" while it is), with the thirds lines on / off (photoguide: PlusPhotoGuideOn
 *    and PlusPhotoGuideKind = thirds);
 *  - the focus frame: corners and a cross at the screen place of what is focused on (focuslock's focusScreenPos());
 *  - a line to type chat in at the bottom left (the lines of the nearby chat are those of plus/chatconsole, drawn in
 *    this mode only, over the line: chatLineTop(); the chat window stays too if it was open). A text control has the keyboard while it is in use, so
 *    the camera and photo keys (plus/photomode/photokeys.cpp, which looks at acceptsTextInput) do not act then.
 *
 * Everything the panels show is read from the settings the photo tools use (nothing is kept here but which tab
 * is open), so the detailed view and this one always agree. Choosing a mood writes the same values as the tool's
 * preset buttons; when the values were changed by hand since the last mood was chosen, a question comes first.
 *
 * Speed: the views are made at the first entry; the parts are placed when photoscreen lays out (not every frame). The
 * panels are refreshed four times a second, only the open page, and a widget is written only when its value changes. The
 * sky list is collected again every three seconds while it is open (only rewritten when it differs). The focus frame
 * costs one projection a frame. Nothing is allocated per frame.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "panel_simple.h"

#include "composition.h"
#include "photoconv.h"
#include "photoconv_panel.h"
#include "photoscreen.h"
#include "photoscreen_items.h"
#include "plus/flycammouse/flycammouse.h"
#include "plus/focuslock/focuslock.h"
#include "plus/photomode/photomode.h"
#include "plus/photomode/photosky.h"
#include "plus/photomode/phototools.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "llbutton.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llpanel.h"
#include "llrender2dutils.h"
#include "llscrolllistctrl.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llview.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"

namespace plus::photoscreen::simple
{
    namespace
    {
        namespace fl = plus::focuslock;
        namespace pm = plus::photomode;

        enum class Tab
        {
            None,
            Focus,
            Mood,
            Comp,
            Sky,
            Cam,
            Conv
        };

        struct TabInfo
        {
            Tab         tab;
            const char* button;
            const char* page;
        };
        constexpr TabInfo TABS[] = {
            { Tab::Focus, "tab_focus", "page_focus" },
            { Tab::Mood,  "tab_mood",  "page_mood" },
            { Tab::Comp,  "tab_comp",  "page_comp" },
            { Tab::Sky,   "tab_sky",   "page_sky" },
            { Tab::Cam,   "tab_cam",   "page_cam" },
            { Tab::Conv,  "tab_conv",  "page_conv" },
        };

        struct MoodInfo
        {
            const char* button;
            const char* id;     // photomode's preset ids
        };
        constexpr MoodInfo MOODS[] = {
            { "mood_portrait",  "portrait" },
            { "mood_backlight", "backlight" },
            { "mood_sunset",    "sunset" },
            { "mood_night",     "night" },
            { "mood_studio",    "studio" },
            { "mood_natural",   "natural" },
            { "mood_autumn",    "autumn" },
        };

        struct CompInfo
        {
            const char*             button;
            composition::Shot       shot;
        };
        constexpr CompInfo COMPS[] = {
            { "comp_full", composition::Shot::Full },
            { "comp_knee", composition::Shot::Knee },
            { "comp_bust", composition::Shot::Bust },
            { "comp_face", composition::Shot::Face },
            { "comp_low",  composition::Shot::Low },
            { "comp_high", composition::Shot::High },
        };

        // Fixed sizes, UI pixels (the XML has the same).
        constexpr S32 RAIL_W    = 92;
        constexpr S32 RAIL_H    = 248;
        constexpr S32 BODY_W    = 292;
        constexpr S32 BODY_MIN_H = 300;
        constexpr S32 BODY_MAX_H = 440;
        constexpr S32 EDGE      = 8;
        constexpr S32 STRIP_W   = 520;
        constexpr S32 CHAT_W    = 280;
        constexpr S32 CHAT_H    = 22;

        constexpr F32 REFRESH_SECONDS  = 0.25f;
        constexpr F64 SKY_LIST_SECONDS = 3.0;
        constexpr F64 BLUR_NOTICE_SECONDS = 8.0;
        constexpr size_t SKY_LIST_MAX  = 1000;
        constexpr S32 THIRDS_KIND      = 1;     // pm GuideKind::Thirds (photoguide.h)

        struct SkyRow
        {
            std::string name;
            std::string lower;
            LLUUID      item;
            LLUUID      asset;
        };

        // The widgets, found once (the views are made once and kept; the pointers are only used while the
        // root handle is alive).
        struct Widgets
        {
            LLView*           rail = nullptr;
            LLView*           body = nullptr;
            LLView*           strip = nullptr;
            LLLineEditor*     chat = nullptr;
            LLTextBox*        stripText = nullptr;
            LLButton*         thirds = nullptr;
            LLButton*         tabButtons[std::size(TABS)] = {};
            LLView*           pages[std::size(TABS)] = {};
            LLButton*         focusAuto = nullptr;
            LLButton*         focusSelf = nullptr;
            LLButton*         focusOther = nullptr;
            LLScrollListCtrl* nearby = nullptr;
            LLTextBox*        focusStatus = nullptr;
            LLButton*         blurOn = nullptr;
            LLButton*         moods[std::size(MOODS)] = {};
            LLButton*         comps[std::size(COMPS)] = {};
            LLButton*         compUndo = nullptr;
            LLTextBox*        compStatus = nullptr;
            LLButton*         skyKindSky = nullptr;
            LLButton*         skyKindWater = nullptr;
            LLLineEditor*     skySearch = nullptr;
            LLScrollListCtrl* skyList = nullptr;
            LLTextBox*        skyCurrent = nullptr;
            LLButton*         camGo = nullptr;
            LLTextBox*        camStatus = nullptr;
            LLButton*         flycam = nullptr;     // pressed while the flycam is on (photo-flycam)
            LLTextBox*        flycamKeys = nullptr;
            LLPanel*          conv = nullptr;       // the conversations (photoconv_panel.cpp), on page_conv
            LLView*           convBadge = nullptr;  // the unread count on tab_conv
        };

        struct State
        {
            LLHandle<LLPanel> root;
            Widgets           w;
            Tab               tab = Tab::None;
            bool              simple = false;

            // the focus page
            std::vector<fl::Nearby> nearby;     // reused
            std::string             nearbyKey;
            F64                     blurSwitchUntil = 0.0;

            // the sky page
            bool                 skyWater = false;
            std::vector<SkyRow>  skyRows;       // what the list shows
            std::vector<SkyRow>  skyScratch;    // reused for the next collecting
            std::vector<pm::SkyEntry> skyEntries; // reused: the inventory's, before the search
            std::string          skySearchLower;
            F64                  skyNextCollect = 0.0;
            LLUUID               skyShownCurrent;
            // the row put on last here (currentSkyRow()): its layer, the layer's count of changes and what the layer
            // showed just after it (the one before, while it comes); and the row shown as the one in use before it
            // (a row shown only because it was still coming does not count)
            LLUUID               skyLastItem;
            pm::EnvLayer         skyLastLayer = pm::EnvLayer::Sky;
            U32                  skyLastChanges = 0;
            pm::EnvSource        skyLastSource = pm::EnvSource::Other;
            LLUUID               skyLastSourceAsset;
            LLUUID               skyShownBefore;

            S32 railBottom = 0;                 // where place() put the rail and the panel (UI pixels from the bottom)
            S32 bodyBottom = 0;

            std::string compNote;               // the last framing's answer
            std::string lastStrip;
            std::string frameLabel;             // the focus frame's name and distance

            LLFrameTimer refresh;
            boost::signals2::scoped_connection convChanged;     // the tab's badge
        };
        State sS;

        // -------------------------------------------------------------------------------
        // small helpers: write a widget only when the value differs

        void setText(LLTextBox* box, const std::string& text)
        {
            if (box && box->getText() != text)
            {
                box->setText(text);
            }
        }

        void setToggle(LLButton* button, bool on)
        {
            if (button && button->getToggleState() != on)
            {
                button->setToggleState(on);
            }
        }

        void setEnabled(LLView* view, bool on)
        {
            if (view && view->getEnabled() != on)
            {
                view->setEnabled(on);
            }
        }

        void setVisible(LLView* view, bool on)
        {
            if (view && view->getVisible() != on)
            {
                view->setVisible(on);
            }
        }

        LLPanel* root()
        {
            return sS.root.get();
        }

        std::string str(const char* name, const LLStringUtil::format_map_t& args = LLStringUtil::format_map_t())
        {
            LLPanel* panel = root();
            return panel ? panel->getString(name, args) : std::string();
        }

        // -------------------------------------------------------------------------------
        // the pieces of text

        std::string whoOfFocus()
        {
            const std::string name = fl::focusName();
            return name.empty() ? str("focus_someone") : name;
        }

        std::string moodLabel()
        {
            const std::string current = pm::currentPreset();
            for (size_t i = 0; i < std::size(MOODS); ++i)
            {
                if (current == MOODS[i].id && sS.w.moods[i])
                {
                    return sS.w.moods[i]->getLabelUnselected();
                }
            }
            return std::string();
        }

        std::string focusSummary()
        {
            if (fl::state() == fl::State::Locked)
            {
                LLStringUtil::format_map_t args;
                switch (fl::getMode())
                {
                    case fl::MODE_CURSOR:
                        return str("focus_cursor");
                    case fl::MODE_CLICK:
                        return str("focus_click");
                    case fl::MODE_SELF:
                        args["[WHO]"] = str("focus_self");
                        break;
                    default:
                    {
                        LLStringUtil::format_map_t who;
                        who["[WHO]"]  = whoOfFocus();
                        args["[WHO]"] = str("focus_face_of", who);
                        break;
                    }
                }
                return str("focus_locked", args);
            }
            return str("focus_auto");
        }

        std::string stripText()
        {
            std::string text;
            static bool        flying = false;
            static std::string leave_key;   // read from the key bindings as the flycam goes on, not every refresh
            const bool         on     = plus::flycammouse::isOn();
            if (on && !flying)
            {
                leave_key = plus::flycammouse::leaveKey();
            }
            flying = on;
            if (on)
            {
                // first, so that it stays in sight when the strip cuts the end (photo-flycam)
                LLStringUtil::format_map_t args;
                args["[KEY]"] = leave_key;
                text = str("strip_flycam", args);
                text += "  \xE3\x83\xBB  ";
            }
            text += focusSummary();
            text += "  \xE3\x83\xBB  ";   // " ・ "
            const std::string mood = moodLabel();
            if (mood.empty())
            {
                text += str("strip_mood_none");
            }
            else
            {
                LLStringUtil::format_map_t args;
                args["[MOOD]"] = mood;
                text += str("strip_mood", args);
            }
            return text;
        }

        // -------------------------------------------------------------------------------
        // Focus

        void chooseFocusAuto()
        {
            fl::setMode(fl::MODE_OFF);
        }

        void chooseFocusSelf()
        {
            fl::setMode(fl::MODE_SELF);
        }

        void chooseFocusOther()
        {
            if (fl::getAvatar().isNull() && fl::listNearby(sS.nearby) > 0)
            {
                fl::setAvatar(sS.nearby.front().id);     // the nearest, until someone is chosen
            }
            fl::setMode(fl::MODE_OTHER);
        }

        void turnBlurOn()
        {
            if (const pm::Item* item = pm::findItem(items::ITEM_DEPTH_OF_FIELD))
            {
                pm::setItemValue(*item, 1.f);
            }
            else
            {
                gSavedSettings.setBOOL(items::ITEM_DEPTH_OF_FIELD, true);
            }
            sS.blurSwitchUntil = LLTimer::getTotalSeconds() + BLUR_NOTICE_SECONDS;
        }

        void refreshFocus()
        {
            Widgets& w = sS.w;
            const fl::Mode mode = fl::getMode();
            setToggle(w.focusAuto, mode == fl::MODE_OFF || mode == fl::MODE_CURSOR || mode == fl::MODE_CLICK);
            setToggle(w.focusSelf, mode == fl::MODE_SELF);
            setToggle(w.focusOther, mode == fl::MODE_OTHER);

            // the people: written again only when the list is another
            fl::listNearby(sS.nearby);
            std::string key;
            for (const fl::Nearby& person : sS.nearby)
            {
                key += person.id.asString();
                key += person.name;
                key += '|';
            }
            if (key != sS.nearbyKey)
            {
                sS.nearbyKey = key;
                w.nearby->clearRows();
                for (const fl::Nearby& person : sS.nearby)
                {
                    LLSD row;
                    row["value"]                 = person.id;
                    row["columns"][0]["column"]  = "name";
                    row["columns"][0]["value"]   = person.name;
                    w.nearby->addElement(row);
                }
            }
            const LLUUID target = fl::getAvatar();
            if (mode == fl::MODE_OTHER && target.notNull())
            {
                const LLScrollListItem* picked = w.nearby->getFirstSelected();
                if (!picked || picked->getValue().asUUID() != target)
                {
                    w.nearby->selectByID(target);
                }
            }
            else if (w.nearby->getFirstSelectedIndex() >= 0)
            {
                w.nearby->deselectAllItems();
            }

            const bool blur = pm::blurAvailable();
            std::string text;
            if (!blur)
            {
                text = LLTimer::getTotalSeconds() < sS.blurSwitchUntil ? str("blur_switching") : str("focus_blur_off");
            }
            else if (mode == fl::MODE_OTHER && target.isNull())
            {
                text = str("focus_nobody");
            }
            else if (mode == fl::MODE_CLICK && fl::state() == fl::State::Lost)
            {
                // no one to see here: what to do (no place yet: click, or the detail view's button), or the place
                // is behind the camera
                text = str(fl::hasClickPoint() ? "focus_click_behind" : fl::isPicking() ? "focus_click_wait" : "focus_click_none");
            }
            else if (mode != fl::MODE_OFF && fl::state() == fl::State::Lost)
            {
                LLStringUtil::format_map_t args;
                args["[WHO]"] = mode == fl::MODE_SELF ? str("focus_self") : whoOfFocus();
                text          = str("focus_lost", args);
            }
            else
            {
                text = focusSummary();
            }
            setText(w.focusStatus, text);
            setVisible(w.blurOn, !blur && LLTimer::getTotalSeconds() >= sS.blurSwitchUntil);
        }

        // -------------------------------------------------------------------------------
        // Mood

        // Whether the values of the mood chosen last were changed since (by hand, in the photo tools). A
        // value a mood leaves alone, or the sun when the sky was changed since (it is forgotten), does not count.
        bool moodChangedByHand()
        {
            const pm::Preset* preset = pm::findPreset(pm::currentPreset());
            if (!preset)
            {
                return false;
            }
            for (int i = 0; i < pm::PARAM_COUNT; ++i)
            {
                if (preset->values[i] <= pm::UNSET)
                {
                    continue;
                }
                const pm::Param param = static_cast<pm::Param>(i);
                const float     now   = pm::value(param);
                if (now <= pm::UNSET)
                {
                    continue;
                }
                const pm::ParamRange& range = pm::range(param);
                if (std::fabs(now - preset->values[i]) > (range.max - range.min) * 0.005f)
                {
                    return true;
                }
            }
            return pm::presetSettingsChanged(*preset); // the look and its effects of a preset that sets them
        }

        void chooseMood(const std::string& id)
        {
            if (!moodChangedByHand())
            {
                pm::applyPreset(id);
                return;
            }
            LLStringUtil::format_map_t args;
            args["[MOOD]"] = moodLabel();
            LLNotificationsUtil::add("GenericAlertYesCancel", LLSD().with("MESSAGE", str("mood_changed_title", args)), LLSD(),
                                     [id](const LLSD& notification, const LLSD& response) {
                                         if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                                         {
                                             pm::applyPreset(id);
                                         }
                                     });
        }

        void refreshMood()
        {
            const std::string current = pm::currentPreset();
            for (size_t i = 0; i < std::size(MOODS); ++i)
            {
                setToggle(sS.w.moods[i], current == MOODS[i].id);
            }
        }

        // -------------------------------------------------------------------------------
        // Framing

        void chooseComposition(composition::Shot shot)
        {
            LLStringUtil::format_map_t args;
            switch (composition::apply(shot))
            {
                case composition::Result::Ok:
                    args["[WHO]"] = fl::getMode() == fl::MODE_OTHER ? whoOfFocus() : str("who_self");
                    sS.compNote   = str("comp_ok", args);
                    break;
                case composition::Result::NoTarget:
                    sS.compNote = str("comp_no_target");
                    break;
                default:
                    sS.compNote = str("comp_no_camera");
                    break;
            }
            setText(sS.w.compStatus, sS.compNote);
        }

        void refreshComp()
        {
            const bool can = composition::canApply();
            for (LLButton* button : sS.w.comps)
            {
                setEnabled(button, can);
            }
            setEnabled(sS.w.compUndo, composition::hasPrevious());
            setText(sS.w.compStatus, sS.compNote.empty() ? str(can ? "comp_idle" : "comp_no_target") : sS.compNote);
        }

        // -------------------------------------------------------------------------------
        // Sky and water

        // The inventory's skies (or waters) that the search lets through, by name. Into sS.skyScratch.
        // The detailed view's list (pm::environmentItems): a library sky and its copy are one row.
        void collectSky()
        {
            sS.skyScratch.clear();
            const pm::EnvKind wanted = sS.skyWater ? pm::EnvKind::Water : pm::EnvKind::Sky;
            pm::environmentItems(wanted, sS.skyEntries, std::numeric_limits<size_t>::max());   // the search cuts after
            for (pm::SkyEntry& entry : sS.skyEntries)
            {
                SkyRow row;
                row.lower = entry.name;
                LLStringUtil::toLower(row.lower);
                if (!sS.skySearchLower.empty() && row.lower.find(sS.skySearchLower) == std::string::npos)
                {
                    continue;
                }
                row.name  = std::move(entry.name);
                row.item  = entry.item;
                row.asset = entry.asset;
                sS.skyScratch.push_back(std::move(row));
                if (sS.skyScratch.size() >= SKY_LIST_MAX)
                {
                    break;
                }
            }
        }

        bool sameRows(const std::vector<SkyRow>& a, const std::vector<SkyRow>& b)
        {
            if (a.size() != b.size())
            {
                return false;
            }
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (a[i].item != b[i].item || a[i].name != b[i].name)
                {
                    return false;
                }
            }
            return true;
        }

        enum class SkyRowFor { Shown, Step };

        // The row of the sky or water in use, or -1. The row put on last here counts while nothing else changed its
        // layer since and its sky is still coming (shown, and stepped from), could not be had while the layer still
        // shows what it showed when it was asked for (stepped from: the steps go past it; shown is the row shown
        // before it, not the first row of the same sky), or is the one in use (the row itself: another row of the
        // same sky, a copy of another name, is not it; the steps went round between the two before: combo-arrow).
        // Else the first row of the sky in use.
        int currentSkyRow(SkyRowFor use)
        {
            LLUUID asset;
            const pm::EnvSource source = pm::environmentSource(sS.skyWater, asset);
            const bool asset_in_use = source == pm::EnvSource::Asset;
            const pm::EnvLayer layer = sS.skyWater ? pm::EnvLayer::Water : pm::EnvLayer::Sky;
            if (sS.skyLastItem.notNull() && sS.skyLastLayer == layer && sS.skyLastChanges == pm::localEnvironmentChanges(layer))
            {
                const pm::EnvFetch fetch = pm::localEnvironmentFetch(layer);
                const bool failed_here = fetch == pm::EnvFetch::Failed && source == sS.skyLastSource
                                         && asset == sS.skyLastSourceAsset;
                // shown while it could not be had: the row shown before it, while that is still the one in use
                const bool shown_before = failed_here && use == SkyRowFor::Shown;
                const LLUUID& row_item = shown_before ? sS.skyShownBefore : sS.skyLastItem;
                for (size_t i = 0; i < sS.skyRows.size(); ++i)
                {
                    if (sS.skyRows[i].item != row_item)
                    {
                        continue;
                    }
                    const bool in_use = asset_in_use && sS.skyRows[i].asset == asset;
                    if (in_use || (!shown_before && (fetch == pm::EnvFetch::Pending || failed_here)))
                    {
                        return (int)i;
                    }
                    break;
                }
            }
            if (!asset_in_use)
            {
                return -1;
            }
            for (size_t i = 0; i < sS.skyRows.size(); ++i)
            {
                if (sS.skyRows[i].asset == asset)
                {
                    return (int)i;
                }
            }
            return -1;
        }

        void refreshSky(bool force)
        {
            Widgets&   w   = sS.w;
            const F64  now = LLTimer::getTotalSeconds();
            if (force || now >= sS.skyNextCollect)
            {
                sS.skyNextCollect = now + SKY_LIST_SECONDS;
                collectSky();
                if (force || !sameRows(sS.skyRows, sS.skyScratch))
                {
                    sS.skyRows.swap(sS.skyScratch);
                    w.skyList->clearRows();
                    for (const SkyRow& row : sS.skyRows)
                    {
                        LLSD element;
                        element["value"]                = row.item;
                        element["columns"][0]["column"] = "name";
                        element["columns"][0]["value"]  = row.name;
                        w.skyList->addElement(element);
                    }
                    sS.skyShownCurrent.setNull();   // select again
                }
            }
            setToggle(w.skyKindSky, !sS.skyWater);
            setToggle(w.skyKindWater, sS.skyWater);

            const int current = currentSkyRow(SkyRowFor::Shown);
            const LLUUID item = current >= 0 ? sS.skyRows[current].item : LLUUID::null;
            if (item != sS.skyShownCurrent)
            {
                sS.skyShownCurrent = item;
                if (item.notNull())
                {
                    w.skyList->selectByID(item);
                }
                else
                {
                    w.skyList->deselectAllItems();
                }
            }
            LLUUID asset;
            std::string text;
            if (current >= 0)
            {
                text = sS.skyRows[current].name;
            }
            else
            {
                text = str(pm::environmentSource(sS.skyWater, asset) == pm::EnvSource::Land ? "sky_land" : "sky_other");
            }
            if (sS.skyRows.empty() && sS.skySearchLower.empty())
            {
                text = str("sky_none");
            }
            setText(w.skyCurrent, text);
        }

        void putOnSky(const LLUUID& item)
        {
            if (item.isNull() || !pm::isEnvironmentItem(item))
            {
                return;
            }
            // The row shown now, to show while this one could not be had: only a row shown as the one in use. A row
            // shown because it is still coming is not (it may never come): the one shown before it stays.
            const int          shown        = currentSkyRow(SkyRowFor::Shown);
            const pm::EnvLayer shown_layer  = sS.skyWater ? pm::EnvLayer::Water : pm::EnvLayer::Sky;
            LLUUID             in_use_asset;
            const bool         shown_in_use = shown >= 0
                                              && pm::environmentSource(sS.skyWater, in_use_asset) == pm::EnvSource::Asset
                                              && sS.skyRows[shown].asset == in_use_asset;
            const bool         shown_coming = shown >= 0 && !shown_in_use && sS.skyRows[shown].item == sS.skyLastItem
                                              && sS.skyLastLayer == shown_layer
                                              && sS.skyLastChanges == pm::localEnvironmentChanges(shown_layer)
                                              && pm::localEnvironmentFetch(shown_layer) == pm::EnvFetch::Pending;
            const LLUUID shown_before = shown_coming ? sS.skyShownBefore
                                                     : shown >= 0 ? sS.skyRows[shown].item : LLUUID::null;
            if (pm::applyEnvironmentItem(item))
            {
                pm::noteApplied(item);
                pm::EnvKind kind = pm::EnvKind::Sky;
                pm::environmentKind(item, kind);
                sS.skyLastItem    = item;
                sS.skyLastLayer   = kind == pm::EnvKind::Water ? pm::EnvLayer::Water : pm::EnvLayer::Sky;
                sS.skyLastChanges = pm::localEnvironmentChanges(sS.skyLastLayer);   // after it: the count it made
                sS.skyLastSource  = pm::environmentSource(kind == pm::EnvKind::Water, sS.skyLastSourceAsset);
                sS.skyShownBefore = shown_before;
            }
            refreshSky(false);
        }

        // The one before / after the one in use in the list (or the one put on last, still coming or not had), round;
        // with none in use, the first or the last.
        void stepSky(int direction)
        {
            if (sS.skyRows.empty())
            {
                return;
            }
            const int count   = (int)sS.skyRows.size();
            const int current = currentSkyRow(SkyRowFor::Step);
            int       next    = current < 0 ? (direction > 0 ? 0 : count - 1) : (current + direction + count) % count;
            putOnSky(sS.skyRows[next].item);
        }

        void restoreSky()
        {
            if (sS.skyWater)
            {
                pm::restoreLandWater();
            }
            else
            {
                pm::restoreLandSky();
            }
            refreshSky(false);
        }

        // -------------------------------------------------------------------------------
        // Camera spot

        void refreshCam()
        {
            const bool has = pm::hasTempView();
            setEnabled(sS.w.camGo, has);
            setText(sS.w.camStatus, str(has ? "cam_kept" : "cam_none"));
            // the keys for the way the mouse looks in the flycam
            static LLCachedControl<S32> look(gSavedSettings, "PlusFlycamMouseMode", 1);
            static const char* const KEYS[] = { "flycam_keys_0", "flycam_keys_1", "flycam_keys_2" };
            setText(sS.w.flycamKeys, str(KEYS[llclamp((S32)look, 0, 2)]));
        }

        // -------------------------------------------------------------------------------
        // the frame of what is focused on

        class FrameView : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit FrameView(const Params& p) : LLView(p) {}
            friend class LLUICtrlFactory;

        public:
            void draw() override;
        };

        void refreshAll();     // below

        void FrameView::draw()
        {
            if (sS.refresh.hasExpired())
            {
                sS.refresh.setTimerExpirySec(REFRESH_SECONDS);
                refreshAll();
            }
            LLVector2 at;
            if (!fl::focusScreenPos(at))
            {
                return;
            }
            const bool    cursor = fl::getMode() == fl::MODE_CURSOR;
            const LLColor4 color = cursor ? LLColor4(1.f, 0.9f, 0.4f, 0.9f) : LLColor4(1.f, 1.f, 1.f, 0.9f);
            const S32     x      = ll_round(at.mV[VX]);
            const S32     y      = ll_round(at.mV[VY]);
            constexpr S32 HALF   = 36;   // the frame is 72 across
            constexpr S32 ARM    = 16;
            constexpr S32 CROSS  = 6;
            // four corners
            for (const S32 sx : { -1, 1 })
            {
                for (const S32 sy : { -1, 1 })
                {
                    const S32 cx = x + sx * HALF;
                    const S32 cy = y + sy * HALF;
                    gl_line_2d(cx, cy, cx - sx * ARM, cy, color);
                    gl_line_2d(cx, cy, cx, cy - sy * ARM, color);
                }
            }
            // the cross at the middle
            gl_line_2d(x - CROSS, y, x + CROSS, y, color);
            gl_line_2d(x, y - CROSS, x, y + CROSS, color);
            if (!sS.frameLabel.empty())
            {
                const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
                const S32       text_w = font->getWidth(sS.frameLabel);
                font->renderUTF8(sS.frameLabel, 0, x - text_w / 2, y - HALF - 4 - (S32)font->getLineHeight(), color);
            }
        }

        // -------------------------------------------------------------------------------
        // the refresh (four times a second, from the frame view's draw)

        void refreshAll()
        {
            LLPanel* panel = root();
            if (!panel || !panel->getVisible())
            {
                return;
            }
            Widgets& w = sS.w;

            // the strip and the frame's label, always
            const std::string strip = stripText();
            if (strip != sS.lastStrip)
            {
                sS.lastStrip = strip;
                setText(w.stripText, strip);
            }
            std::string label;
            const fl::Mode mode = fl::getMode();
            if (mode == fl::MODE_SELF || mode == fl::MODE_OTHER || mode == fl::MODE_CLICK)
            {
                label = mode == fl::MODE_CLICK ? str("frame_click") : fl::focusName();
                F32 meters = 0.f;
                if (fl::focusDistance(meters))
                {
                    label += llformat("  \xE3\x83\xBB  %.1f m", meters);
                }
            }
            sS.frameLabel = label;

            static LLCachedControl<bool> guide_on(gSavedSettings, items::ITEM_GUIDE_ON, false);
            static LLCachedControl<S32>  guide_kind(gSavedSettings, items::ITEM_GUIDE_KIND, THIRDS_KIND);
            setToggle(w.thirds, guide_on && guide_kind == THIRDS_KIND);
            setToggle(w.flycam, plus::flycammouse::isOn());

            switch (sS.tab)
            {
                case Tab::Focus: refreshFocus(); break;
                case Tab::Mood:  refreshMood(); break;
                case Tab::Comp:  refreshComp(); break;
                case Tab::Sky:   refreshSky(false); break;
                case Tab::Cam:   refreshCam(); break;
                default: break;
            }
        }

        // -------------------------------------------------------------------------------
        // building

        void setTab(Tab tab)
        {
            Widgets& w = sS.w;
            sS.tab     = tab;
            for (size_t i = 0; i < std::size(TABS); ++i)
            {
                setToggle(w.tabButtons[i], TABS[i].tab == tab);
                setVisible(w.pages[i], TABS[i].tab == tab);
            }
            setVisible(w.body, tab != Tab::None);
            sS.compNote.clear();
            sS.nearbyKey.clear();
            if (tab == Tab::Sky)
            {
                refreshSky(true);
            }
            else if (tab != Tab::None)
            {
                sS.refresh.setTimerExpirySec(0.f);  // the page is written at once, on the next frame
            }
            if (tab == Tab::Focus || tab == Tab::Mood || tab == Tab::Comp || tab == Tab::Cam)
            {
                refreshAll();
            }
            if (tab == Tab::Conv)
            {
                conv::openPanel(w.conv);
            }
        }

        bool findWidgets(LLPanel* panel)
        {
            Widgets& w = sS.w;
            w.rail       = panel->findChildView("rail");
            w.body       = panel->findChildView("body");
            w.strip      = panel->findChildView("strip");
            w.chat       = panel->findChild<LLLineEditor>("ed_chat");
            w.stripText  = panel->findChild<LLTextBox>("strip_text");
            w.thirds     = panel->findChild<LLButton>("btn_thirds");
            for (size_t i = 0; i < std::size(TABS); ++i)
            {
                w.tabButtons[i] = panel->findChild<LLButton>(TABS[i].button);
                w.pages[i]      = panel->findChildView(TABS[i].page);
            }
            w.focusAuto   = panel->findChild<LLButton>("btn_focus_auto");
            w.focusSelf   = panel->findChild<LLButton>("btn_focus_self");
            w.focusOther  = panel->findChild<LLButton>("btn_focus_other");
            w.nearby      = panel->findChild<LLScrollListCtrl>("list_nearby");
            w.focusStatus = panel->findChild<LLTextBox>("focus_status");
            w.blurOn      = panel->findChild<LLButton>("btn_blur_on");
            for (size_t i = 0; i < std::size(MOODS); ++i)
            {
                w.moods[i] = panel->findChild<LLButton>(MOODS[i].button);
            }
            for (size_t i = 0; i < std::size(COMPS); ++i)
            {
                w.comps[i] = panel->findChild<LLButton>(COMPS[i].button);
            }
            w.compUndo     = panel->findChild<LLButton>("comp_undo");
            w.compStatus   = panel->findChild<LLTextBox>("comp_status");
            w.skyKindSky   = panel->findChild<LLButton>("btn_sky_kind_sky");
            w.skyKindWater = panel->findChild<LLButton>("btn_sky_kind_water");
            w.skySearch    = panel->findChild<LLLineEditor>("ed_sky_search");
            w.skyList      = panel->findChild<LLScrollListCtrl>("list_sky");
            w.skyCurrent   = panel->findChild<LLTextBox>("sky_current");
            w.camGo        = panel->findChild<LLButton>("btn_cam_go");
            w.camStatus    = panel->findChild<LLTextBox>("cam_status");
            w.flycam       = panel->findChild<LLButton>("btn_flycam");
            w.flycamKeys   = panel->findChild<LLTextBox>("flycam_keys");

            bool ok = w.rail && w.body && w.strip && w.chat && w.stripText && w.thirds && w.focusAuto && w.focusSelf &&
                      w.focusOther && w.nearby && w.focusStatus && w.blurOn && w.compUndo && w.compStatus && w.skyKindSky &&
                      w.skyKindWater && w.skySearch && w.skyList && w.skyCurrent && w.camGo && w.camStatus && w.flycam &&
                      w.flycamKeys;
            for (LLButton* b : w.tabButtons) { ok = ok && b; }
            for (LLView* v : w.pages)        { ok = ok && v; }
            for (LLButton* b : w.moods)      { ok = ok && b; }
            for (LLButton* b : w.comps)      { ok = ok && b; }
            return ok;
        }

        void wire()
        {
            Widgets& w = sS.w;
            for (size_t i = 0; i < std::size(TABS); ++i)
            {
                const Tab tab = TABS[i].tab;
                w.tabButtons[i]->setCommitCallback([tab](LLUICtrl*, const LLSD&) { setTab(sS.tab == tab ? Tab::None : tab); });
            }
            w.focusAuto->setCommitCallback([](LLUICtrl*, const LLSD&) { chooseFocusAuto(); refreshAll(); });
            w.focusSelf->setCommitCallback([](LLUICtrl*, const LLSD&) { chooseFocusSelf(); refreshAll(); });
            w.focusOther->setCommitCallback([](LLUICtrl*, const LLSD&) { chooseFocusOther(); refreshAll(); });
            w.nearby->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) {
                const LLScrollListItem* picked = static_cast<LLScrollListCtrl*>(ctrl)->getFirstSelected();
                if (picked && picked->getValue().asUUID().notNull())
                {
                    fl::setAvatar(picked->getValue().asUUID());
                    fl::setMode(fl::MODE_OTHER);    // choosing someone means to follow them
                    refreshAll();
                }
            });
            w.blurOn->setCommitCallback([](LLUICtrl*, const LLSD&) { turnBlurOn(); refreshAll(); });
            for (size_t i = 0; i < std::size(MOODS); ++i)
            {
                const std::string id = MOODS[i].id;
                w.moods[i]->setCommitCallback([id](LLUICtrl*, const LLSD&) { chooseMood(id); refreshAll(); });
            }
            for (size_t i = 0; i < std::size(COMPS); ++i)
            {
                const composition::Shot shot = COMPS[i].shot;
                w.comps[i]->setCommitCallback([shot](LLUICtrl*, const LLSD&) { chooseComposition(shot); });
            }
            w.compUndo->setCommitCallback([](LLUICtrl*, const LLSD&) {
                composition::restorePrevious();
                sS.compNote.clear();
                refreshAll();
            });
            w.skyKindSky->setCommitCallback([](LLUICtrl*, const LLSD&) { sS.skyWater = false; refreshSky(true); });
            w.skyKindWater->setCommitCallback([](LLUICtrl*, const LLSD&) { sS.skyWater = true; refreshSky(true); });
            w.skySearch->setKeystrokeCallback(
                [](LLLineEditor* editor, void*) {
                    sS.skySearchLower = editor->getText();
                    LLStringUtil::trim(sS.skySearchLower);
                    LLStringUtil::toLower(sS.skySearchLower);
                    refreshSky(true);
                },
                nullptr);
            w.skyList->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) {
                const LLScrollListItem* picked = static_cast<LLScrollListCtrl*>(ctrl)->getFirstSelected();
                if (picked)
                {
                    putOnSky(picked->getValue().asUUID());
                }
            });
            LLPanel* panel = root();
            panel->getChild<LLButton>("btn_sky_prev")->setCommitCallback([](LLUICtrl*, const LLSD&) { stepSky(-1); });
            panel->getChild<LLButton>("btn_sky_next")->setCommitCallback([](LLUICtrl*, const LLSD&) { stepSky(1); });
            panel->getChild<LLButton>("btn_sky_land")->setCommitCallback([](LLUICtrl*, const LLSD&) { restoreSky(); });
            panel->getChild<LLButton>("btn_cam_save")->setCommitCallback([](LLUICtrl*, const LLSD&) {
                pm::saveTempView();
                refreshCam();
            });
            w.camGo->setCommitCallback([](LLUICtrl*, const LLSD&) {
                pm::goToTempView(true);
                refreshCam();
            });
            // the flycam on or off (the button follows in refreshAll(), however it was switched)
            w.flycam->setCommitCallback([](LLUICtrl*, const LLSD&) {
                plus::flycammouse::toggleFromUi();
                refreshAll();
            });
            {
                LLStringUtil::format_map_t args;
                args["[KEY]"] = plus::flycammouse::switchKey();     // as bound when the view is made
                w.flycam->setToolTip(str("flycam_tip", args));
            }
            w.thirds->setCommitCallback([](LLUICtrl*, const LLSD&) {
                if (gSavedSettings.getBOOL(items::ITEM_GUIDE_ON) && gSavedSettings.getS32(items::ITEM_GUIDE_KIND) == THIRDS_KIND)
                {
                    gSavedSettings.setBOOL(items::ITEM_GUIDE_ON, false);
                }
                else
                {
                    gSavedSettings.setS32(items::ITEM_GUIDE_KIND, THIRDS_KIND);
                    gSavedSettings.setBOOL(items::ITEM_GUIDE_ON, true);
                }
                refreshAll();
            });
            w.chat->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) { sendChatLine(static_cast<LLLineEditor*>(ctrl)); });
        }

        bool build(LLView* overlay)
        {
            LLPanel* panel = LLUICtrlFactory::getInstance()->createFromFile<LLPanel>(
                "panel_plus_photoscreen_simple.xml", overlay, LLPanel::child_registry_t::instance());
            if (!panel)
            {
                return false;
            }
            if (!findWidgets(panel))
            {
                LL_WARNS() << "photo screen: the simple view's XML lacks a widget" << LL_ENDL;
                panel->setVisible(false);
                return false;
            }
            sS.root = panel->getHandle();

            FrameView::Params params;
            params.name          = "focus_frame";
            params.rect          = panel->getLocalRect();
            params.follows.flags = FOLLOWS_ALL;
            params.mouse_opaque  = false;
            if (FrameView* frame = LLUICtrlFactory::create<FrameView>(params))
            {
                panel->addChild(frame);
                panel->sendChildToBack(frame);   // under the panels
            }
            wire();
            if (LLView* page = panel->findChildView("page_conv"))
            {
                sS.w.conv = conv::createPanel(page);
                if (sS.w.conv)
                {
                    sS.w.conv->setVisible(true);    // shown with its page
                }
            }
            sS.w.convBadge = conv::addBadge(panel->findChild<LLButton>("tab_conv"));
            conv::setBadge(sS.w.convBadge, conv::badgeCount());
            sS.convChanged = conv::onChanged([]() {
                if (root())     // the widgets' pointers are good while the view is there
                {
                    conv::setBadge(sS.w.convBadge, conv::badgeCount());
                }
            });
            setTab(Tab::None);
            return true;
        }

        // The rail, the open panel, the strip and the chat line in a window of w x h.
        void place(S32 w, S32 h)
        {
            Widgets& ui = sS.w;
            const S32 body_h = llclamp(h - 140, BODY_MIN_H, BODY_MAX_H);
            // the rail and the panel are centred on the height, with the top never over the window's top and the bottom
            // above the chat line
            S32 top = std::max(h / 2 + body_h / 2, CHAT_H + 3 * EDGE + body_h);
            top     = std::min(top, h - 2 * EDGE);
            ui.rail->setOrigin(EDGE, top - RAIL_H);
            ui.body->reshape(BODY_W, body_h);
            ui.body->setOrigin(EDGE + RAIL_W + EDGE, top - body_h);
            sS.railBottom = top - RAIL_H;
            sS.bodyBottom = top - body_h;
            ui.strip->setOrigin(std::max((w - STRIP_W) / 2, EDGE + CHAT_W + EDGE), EDGE);
            ui.chat->reshape(CHAT_W, CHAT_H);
            ui.chat->setOrigin(EDGE, EDGE);
        }
    }

    void onLayout(LLView* overlay, S32 w, S32 h, bool simple)
    {
        if (!overlay)
        {
            return;
        }
        if (!root() && !build(overlay))
        {
            return;
        }
        LLPanel* panel = root();
        panel->setShape(LLRect(0, h, w, 0));
        panel->setVisible(simple);
        sS.simple = simple;
        if (simple)
        {
            place(w, h);
            sS.refresh.setTimerExpirySec(0.f);
        }
    }

    namespace
    {
        // The simple view is on the screen (the chat console's room is measured from its parts).
        bool simpleShown()
        {
            LLPanel* panel = root();
            return panel && sS.simple && panel->isInVisibleChain();
        }
    }

    S32 chatLineTop()
    {
        if (!simpleShown() || !sS.w.chat)
        {
            return 0;
        }
        return sS.w.chat->getRect().mTop + EDGE;
    }

    S32 chatConsoleTop()
    {
        if (!simpleShown())
        {
            return 0;
        }
        const S32 under = sS.tab != Tab::None ? llmin(sS.railBottom, sS.bodyBottom) : sS.railBottom;
        return llmax(1, under - EDGE);
    }
}
