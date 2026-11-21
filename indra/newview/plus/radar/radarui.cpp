/**
 * @file radarui.cpp
 * @brief What the radar window shows and does, apart from how it is shown (research/386).
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

// See radarui.h. The code is what radarfloater.cpp's window class held, taken out so that the XUI window and the RmlUi
// window (radarrml.cpp, research/386) use the same text, styles and actions.

#include "llviewerprecompiledheaders.h"

#include "radarui.h"
#include "plus/camerashot/camerashot.h"
#include "plus/contactsethooks/cshooks.h"
#include "plus/rlv/rlvhooks.h"

#include <cstdlib>

#include "llagent.h"
#include "llagentcamera.h"
#include "llavataractions.h"
#include "llcallingcard.h"
#include "llclipboard.h"
#include "llfontgl.h"
#include "lluicolortable.h"
#include "llviewercontrol.h"
#include "llviewerjoystick.h"
#include "llviewermenu.h"
#include "llviewerobjectlist.h"
#include "llvoavatar.h"

namespace plus::radar::ui
{
    namespace
    {
        // How near the camera comes to an avatar that is zoomed in on, in meters.
        constexpr F32 ZOOM_DISTANCE = 2.8f;
        // The avatar's position is its pelvis: a little above it is where one looks.
        constexpr F64 ZOOM_LOOK_HEIGHT = 0.3;

        // The avatar's object when it is drawn (the camera needs it).
        LLVOAvatar* drawnAvatar(const LLUUID& id)
        {
            LLVOAvatar* avatar = dynamic_cast<LLVOAvatar*>(gObjectList.findObject(id));
            return avatar && !avatar->isDead() ? avatar : nullptr;
        }

        // The camera in front of an avatar, looking at it (Firestorm's LLAvatarActions::zoomIn()).
        void zoomTo(LLVOAvatar* avatar)
        {
            // A camera shot running would move the camera on from here the next frame.
            plus::camerashot::stop();
            // The flycam would override the move.
            if (LLViewerJoystick::getInstance()->getOverrideCamera())
            {
                gAgentCamera.toggleFlycam(); // off (the flycam of the 3D mouse and of game control, official 26.5)
            }
            LLViewerJoystick::getInstance()->setCameraNeedsUpdate(true);

            gAgentCamera.setFocusOnAvatar(false, ANIMATE);

            LLVector3d target = avatar->getPositionGlobal();
            target.mdV[VZ] += ZOOM_LOOK_HEIGHT;
            LLVector3d camera_dir = gAgentCamera.getCameraPositionGlobal() - target;
            if (camera_dir.normalize() == 0.0)
            {
                camera_dir = LLVector3d::x_axis;
            }
            gAgentCamera.setCameraPosAndFocusGlobal(target + camera_dir * ZOOM_DISTANCE, target, avatar->getID());
        }
    }

    // The first number in a cell's text ("12.5", ">162"); the sort puts ">" (not known) last.
    F64 sortNumber(const std::string& text)
    {
        if (!text.empty() && text[0] == '>')
        {
            return 1e12;
        }
        const size_t start = text.find_first_of("0123456789");
        return start == std::string::npos ? -1.0 : std::strtod(text.c_str() + start, nullptr);
    }

    // "h:mm:ss" in seconds.
    S32 sortSeconds(const std::string& text)
    {
        S32 h = 0, m = 0, s = 0;
        if (sscanf(text.c_str(), "%d:%d:%d", &h, &m, &s) == 3)
        {
            return h * 3600 + m * 60 + s;
        }
        return 0;
    }

    // RLVa @shownames: the avatar's name may not be shown (plus/rlv, research/248 3.4).
    bool nameHidden(const LLUUID& id)
    {
        return plus::rlv::enabled() && !plus::rlv::canShowMenuName(id);
    }

    std::string cellText(const Entry& entry, const std::string& column, const Marks& marks)
    {
        for (size_t i = 0; i < COLUMN_COUNT; ++i)
        {
            if (column == COLUMNS[i].name)
            {
                return cellText(entry, i, marks);
            }
        }
        return std::string();
    }

    std::string cellText(const Entry& entry, size_t column, const Marks& marks)
    {
        static LLCachedControl<F32> far_clip(gSavedSettings, "RenderFarClip");
        switch (column)
        {
        case COL_NAME:
            return entry.name;
        case COL_REGION:
            return entry.onParcel ? marks.parcel : (entry.inRegion ? marks.region : std::string());
        case COL_SITTING:
            return entry.sitting ? marks.sitting : std::string();
        case COL_AGE:
            if (nameHidden(entry.id))
            {
                return std::string();   // RLVa @shownames: no age either (FSRadar::updateRadarList())
            }
            if (entry.age >= 0)
            {
                return llformat("%d", entry.age);
            }
            return entry.age == -2 ? marks.age_hidden : (entry.age == -3 ? marks.age_unknown : std::string());
        case COL_PAYMENT:
            return (entry.flags & AVATAR_TRANSACTED) ? "$$" : ((entry.flags & AVATAR_IDENTIFIED) ? "$" : "");
        case COL_SEEN:
        {
            const S32 seconds = (S32)difftime(time(nullptr), entry.firstSeen);
            return llformat("%d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60);
        }
        case COL_RANGE:
            // Not known: farther than the draw distance.
            return entry.range > UNKNOWN_RANGE ? llformat("%.1f", entry.range) : llformat(">%d", (S32)(F32)far_clip);
        default:
            return std::string();
        }
    }

    Style nameStyle(const Entry& entry)
    {
        Style style;
        // RLVa @shownames: a friend is not told by the bold either (FSRadar::updateRadarList()).
        style.bold = entry.friendship && !nameHidden(entry.id);
        style.italic = entry.muted;
        // The contact set colour (FSContactSetsColorizeRadar; not under @shownames, cshooks).
        style.coloured = plus::cshooks::colorFor(entry.id, ContactSetType::RADAR, style.colour);
        return style;
    }

    Style rangeStyle(const Entry& entry)
    {
        static const LLColor4 say_color(1.f, 0.4f, 0.4f, 1.f);
        static const LLColor4 shout_color(1.f, 0.85f, 0.45f, 1.f);
        Style style;
        const bool known = entry.range > UNKNOWN_RANGE;
        style.coloured = known && entry.range <= CHAT_RANGE_SHOUT;
        style.colour = known && entry.range <= CHAT_RANGE_SAY ? say_color : shout_color;
        // In bold when the avatar is drawn.
        style.bold = entry.inDraw;
        return style;
    }

    LLColor4 listTextColour()
    {
        return LLUIColorTable::instance().getColor("DefaultListText", LLColor4::white).get();
    }

    bool matchesFilter(const Entry& entry, const std::string& lower_filter)
    {
        if (lower_filter.empty())
        {
            return true;
        }
        std::string name = entry.name;
        LLStringUtil::toLower(name);
        return name.find(lower_filter) != std::string::npos;
    }

    void counterArgs(const Stats& stats, LLStringUtil::format_map_t& args)
    {
        args["[TOTAL]"]  = llformat("%d", stats.total);
        args["[REGION]"] = llformat("%d", stats.region);
        args["[CHAT]"]   = llformat("%d", stats.chatRange);
    }

    Notice zoomToId(const LLUUID& id)
    {
        if (LLVOAvatar* avatar = drawnAvatar(id))
        {
            zoomTo(avatar);
            return Notice::None;
        }
        return Notice::NoFocus;
    }

    bool menuEnabled(const std::string& what, const uuid_vec_t& ids)
    {
        if (ids.empty())
        {
            return false;
        }
        if (what == "any")
        {
            return true;
        }
        if (what == "offer_teleport")
        {
            return LLAvatarActions::canOfferTeleport(ids);
        }
        if (what == "call")
        {
            return LLAvatarActions::canCall();     // one: a call; several: a conference call
        }
        if (ids.size() != 1)
        {
            return false;
        }
        const LLUUID& id   = ids.front();
        const Entry* entry = Engine::getInstance()->find(id);
        if (what == "one")
        {
            return true;
        }
        if (what == "add_friend")
        {
            return !LLAvatarActions::isFriend(id);
        }
        if (what == "remove_friend")
        {
            return LLAvatarActions::isFriend(id);
        }
        if (what == "map")
        {
            return is_agent_mappable(id) || gAgent.isGodlike();
        }
        if (what == "position")
        {
            return entry && entry->range > UNKNOWN_RANGE;
        }
        if (what == "zoom")
        {
            return entry && entry->inDraw;
        }
        if (what == "block")
        {
            return LLAvatarActions::canBlock(id);
        }
        return false;
    }

    void copyToClipboard(const uuid_vec_t& ids, bool as_ids)
    {
        std::string text;
        for (const LLUUID& id : ids)
        {
            if (!text.empty())
            {
                text += "\n";
            }
            const Entry* entry = Engine::getInstance()->find(id);
            text += as_ids ? id.asString() : (entry ? entry->name : id.asString());
        }
        if (text.empty())
        {
            return;
        }
        const LLWString wide = utf8str_to_wstring(text);
        LLClipboard::instance().copyToClipboard(wide, 0, static_cast<S32>(wide.length()));
    }

    Notice runAction(const std::string& action, const uuid_vec_t& ids)
    {
        if (ids.empty())
        {
            return Notice::None;
        }
        const LLUUID& id = ids.front();
        const bool one   = ids.size() == 1;
        if (action == "profile")
        {
            LLAvatarActions::showProfile(id);
        }
        else if (action == "add_friend")
        {
            LLAvatarActions::requestFriendshipDialog(id);
        }
        else if (action == "remove_friend")
        {
            LLAvatarActions::removeFriendDialog(id);
        }
        else if (action == "im")
        {
            one ? LLAvatarActions::startIM(id) : LLAvatarActions::startConference(ids);
        }
        else if (action == "call")
        {
            one ? LLAvatarActions::startCall(id) : LLAvatarActions::startAdhocCall(ids);
        }
        else if (action == "map")
        {
            LLAvatarActions::showOnMap(id);
        }
        else if (action == "track")
        {
            Engine::getInstance()->startTracking(id);
        }
        else if (action == "teleport_to")
        {
            const Entry* entry = Engine::getInstance()->find(id);
            if (entry && entry->range > UNKNOWN_RANGE)
            {
                gAgent.teleportViaLocation(entry->position);
            }
            else
            {
                return Notice::NoPosition;
            }
        }
        else if (action == "zoom")
        {
            return zoomToId(id);
        }
        else if (action == "offer_teleport")
        {
            LLAvatarActions::offerTeleport(ids);
        }
        else if (action == "request_teleport")
        {
            LLAvatarActions::teleportRequest(id);
        }
        else if (action == "share")
        {
            LLAvatarActions::share(id);
        }
        else if (action == "pay")
        {
            LLAvatarActions::pay(id);
        }
        else if (action == "invite_group")
        {
            LLAvatarActions::inviteToGroup(id);
        }
        else if (action == "block")
        {
            LLAvatarActions::toggleBlock(id);
        }
        else if (action == "copy_name")
        {
            copyToClipboard(ids, false);
        }
        else if (action == "copy_id")
        {
            copyToClipboard(ids, true);
        }
        return Notice::None;
    }
}
