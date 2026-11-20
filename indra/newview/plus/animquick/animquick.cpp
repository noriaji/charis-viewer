/**
 * @file animquick.cpp
 * @brief Quick animation preview (research/332).
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

#include "animquick.h"

#include <set>

#include "llfloaterreg.h"
#include "llpreviewanim.h"
#include "llvoavatarself.h"
#include "llviewercontrol.h"

namespace plus::animquick
{
    namespace
    {
        const char* const SETTING_ON = "PlusAnimQuickPreview";
        const char* const SETTING_INWORLD = "PlusAnimQuickInworld";
        const char* const FLOATER_NAME = "preview_anim";
        // LLFloaterReg::getFloaterList() is keyed by the group the floater was registered in (llviewerfloaterreg.cpp),
        // not by its own name; findInstance() goes by the name.
        const char* const FLOATER_GROUP = "preview";

        LLSD playMode()
        {
            return LLSD(gSavedSettings.getBOOL(SETTING_INWORLD) ? "Inworld" : "Locally");
        }

        std::set<LLUUID> sLocalPlays;

        struct Place
        {
            bool mValid = false;
            S32 mLeft = 0;
            S32 mTop = 0;
        };

        // Close the disposable windows (shown, not tabbed into a multi-preview, not pinned) except 'keep'; the
        // top left corner of the first one is returned, to put the next window there. The windows are collected as
        // handles first, because closing one takes it out of the list being walked. onClose stops that window's own
        // animation and nothing else.
        Place closeDisposable(const LLFloater* keep)
        {
            std::vector<LLHandle<LLFloater>> disposable;
            for (LLFloater* floaterp : LLFloaterReg::getFloaterList(FLOATER_GROUP))
            {
                LLPreviewAnim* preview = dynamic_cast<LLPreviewAnim*>(floaterp);
                if (preview && preview != keep && !preview->isDead() && preview->getVisible() && !preview->getHost()
                    && !preview->isPinned())
                {
                    disposable.push_back(preview->getHandle());
                }
            }

            Place place;
            for (LLHandle<LLFloater>& handle : disposable)
            {
                LLFloater* floaterp = handle.get();
                if (!floaterp)
                {
                    continue;
                }
                if (!place.mValid)
                {
                    place.mValid = true;
                    place.mLeft = floaterp->getRect().mLeft;
                    place.mTop = floaterp->getRect().mTop;
                }
                floaterp->closeFloater();
            }
            return place;
        }
    }

    bool enabled()
    {
        return gSavedSettings.getBOOL(SETTING_ON);
    }

    void noteLocalPlay(const LLUUID& asset)
    {
        sLocalPlays.insert(asset);
    }

    void forgetLocalPlay(const LLUUID& asset)
    {
        sLocalPlays.erase(asset);
    }

    void stopLocalPlays()
    {
        // taken out first: cleanup() of a window calls forgetLocalPlay, which edits the set
        std::set<LLUUID> assets;
        assets.swap(sLocalPlays);
        if (assets.empty())
        {
            return;
        }
        if (isAgentAvatarValid())
        {
            for (const LLUUID& asset : assets)
            {
                gAgentAvatarp->stopMotion(asset, true);
            }
        }
        // A motion that was still loading is only marked stopped, and the window never sees it start, so its buttons
        // would stay down: put back the buttons of every window that played one of these (tabbed ones included).
        std::vector<LLHandle<LLFloater>> windows;
        for (LLFloater* floaterp : LLFloaterReg::getFloaterList(FLOATER_GROUP))
        {
            LLPreviewAnim* preview = dynamic_cast<LLPreviewAnim*>(floaterp);
            if (preview && assets.count(preview->getPlayingAsset()))
            {
                windows.push_back(preview->getHandle());
            }
        }
        for (LLHandle<LLFloater>& handle : windows)
        {
            if (LLPreviewAnim* preview = dynamic_cast<LLPreviewAnim*>(handle.get()))
            {
                preview->cleanup();
            }
        }
    }

    void rememberMode(bool inworld)
    {
        gSavedSettings.setBOOL(SETTING_INWORLD, inworld);
    }

    bool open(const LLUUID& item_id)
    {
        if (!enabled() || !isAgentAvatarValid())
        {
            return false;
        }

        const LLSD key(item_id);
        if (LLFloater* existing = LLFloaterReg::findInstance(FLOATER_NAME, key))
        {
            // This item already has a window. A pinned one, or one that is playing, is only brought to the front by
            // the official code. A disposable one that is not playing is played again (going back to an animation
            // that has ended is part of looking through them), and the other disposable windows go, as for a new one.
            LLPreviewAnim* again = dynamic_cast<LLPreviewAnim*>(existing);
            if (!again || again->isPinned() || again->isPlaying())
            {
                return false;
            }
            closeDisposable(again);
            LLFloaterReg::showInstance(FLOATER_NAME, key, TAKE_FOCUS_YES);
            again->cleanup(); // the Play buttons back up (play() toggles them), whatever state a finished play left
            again->play(playMode());
            return true;
        }

        const Place place = closeDisposable(nullptr);

        LLPreviewAnim* preview = LLFloaterReg::showTypedInstance<LLPreviewAnim>(FLOATER_NAME, key, TAKE_FOCUS_YES);
        if (!preview)
        {
            return false;
        }
        preview->setPinned(false);
        if (place.mValid)
        {
            preview->setOrigin(place.mLeft, place.mTop - preview->getRect().getHeight());
        }
        preview->play(playMode());
        return true;
    }
}
