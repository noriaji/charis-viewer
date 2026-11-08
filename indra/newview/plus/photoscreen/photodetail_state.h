/**
 * @file photodetail_state.h
 * @brief The state the files of the detailed photo screen share (research/221 "ps-split"): the tabs, the
 *        rows shown, the cards, the handles of the panels, and the small helpers every area uses. Internal to
 *        plus/photoscreen/photodetail*.cpp; the frame calls only photodetail.h.
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

#ifndef PLUS_PHOTOSCREEN_PHOTODETAIL_STATE_H
#define PLUS_PHOTOSCREEN_PHOTODETAIL_STATE_H

#include "plus/focuslock/focuslock.h"
#include "plus/photomode/phototools.h"
#include "plus/photomode/phototoolsfloater.h"
#include "plus/photoscreen/photoscreen_items.h"

#include <functional>
#include <string>
#include <vector>

#include "llframetimer.h"
#include "llpanel.h"
#include "llrect.h"
#include "llsd.h"
#include "llstring.h"
#include "llview.h"
#include "stdtypes.h"

class LLButton;
class LLComboBox;
class LLControlVariable;
class LLTextBox;
class LLUICtrl;

namespace plus::photoscreen::detail
{
    namespace pm = plus::photomode;
    namespace fl = plus::focuslock;

    enum Tab
    {
        TabFocus,
        TabCamera,
        TabColor,
        TabSky,
        TabAvatar,
        TabSave,
        TabCount
    };
    const char* const TAB_IDS[TabCount] = { "focus", "camera", "color", "sky", "avatar", "save" };

    enum PresetTab
    {
        PtCamera,
        PtPose,
        PtScene,
        PtMy,
        PtCount
    };
    const char* const PTAB_IDS[PtCount] = { "camera", "pose", "scene", "my" };

    enum Section
    {
        SecCamera,
        SecExposure,
        SecColor,
        SecEffects,
        SecOutput,
        SecCount
    };
    const char* const SEC_IDS[SecCount] = { "camera", "exposure", "color", "effects", "output" };

    constexpr char SETTING_FRAME[]  = "PlusPhotoScreenFocusFrame";
    constexpr char SETTING_SIZE[]   = "PlusPhotoShotSize";
    constexpr char SETTING_ASPECT[] = "PlusPhotoShotAspect";
    // the guide's two settings are named once, in the placement table
    constexpr const char* SETTING_GUIDE_ON   = items::ITEM_GUIDE_ON;
    constexpr const char* SETTING_GUIDE_KIND = items::ITEM_GUIDE_KIND;
    constexpr const char* TOOLS_FLOATER = plus::photomode::PHOTO_TOOLS_FLOATER;

    constexpr F32 TICK_SECONDS = 0.25f;
    constexpr S32 GRID_KINDS   = 7;     // the guide's kinds 1..7 (photoguide.h)

    struct RowRef
    {
        LLUICtrl*  ctl = nullptr;
        pm::Kind   kind = pm::Kind::Slider;
        float      scale = 1.f;
        std::vector<pm::Option> options;
        std::function<float()> get;
        LLControlVariable* needs = nullptr;
        int  needs_value = -1;
        bool blur = false;
        pm::FeatureGate feature;    // and the feature it needs (catalog "needs_feature")
        // what the widget shows
        bool  shown_set = false;
        float shown     = 0.f;
        int   enabled   = -1;
    };

    struct Card
    {
        std::string label, tip, thumb;  // thumb: the key of the small picture ("" none)
        bool        current   = false;
        bool        deletable = false;
        std::function<void()> click, remove;
    };

    struct State
    {
        LLHandle<LLView>  overlay;
        LLHandle<LLPanel> left, right, top, bottom, hud;
        LLHandle<LLView>  hudDraw;
        Tab        tab  = TabFocus;
        PresetTab  ptab = PtScene;
        S32        page = 0;
        bool       flyout = false;      // the folded left panel is open over the world
        bool       shown  = false;
        bool       wide   = true;
        bool       sectionOpen[SecCount] = { true, true, false, false, false };
        std::vector<RowRef> leftRows, rightRows;
        std::vector<Card>   cards;
        std::string         cardsSig;
        std::string         focusPeopleKey;
        std::vector<fl::Nearby> nearby;
        std::vector<std::string> lookIds;
        LLRect world;
        S32    w = 0, h = 0;
        LLFrameTimer timer;
        bool   tickRegistered = false;
        std::string readoutShown, labelShown;
    };
    extern State sS;    // photodetail.cpp

    // --- helpers (photodetail.cpp) ------------------------------------------------------

    std::string str(const char* key);
    std::string str(const std::string& key);
    std::string str(const char* key, const LLStringUtil::format_map_t& args);
    LLPanel* loadPanel(const char* file, LLView* parent = nullptr);
    void later(std::function<void()> work);
    void setTextIfChanged(LLTextBox* box, const std::string& text);
    void setComboIfChanged(LLComboBox* combo, const LLSD& value);
    void setToggleIfChanged(LLButton* button, bool on);

    // The panels laid out again from the last place() (nothing while they are hidden).
    void relayout();
}

#endif // PLUS_PHOTOSCREEN_PHOTODETAIL_STATE_H
