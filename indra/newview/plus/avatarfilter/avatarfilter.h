/**
 * @file avatarfilter.h
 * @brief One test for "this avatar is hidden by the avatar filter": the filter hides every avatar
 *        that is not a friend (RenderAvatarFriendsOnly, the official setting; Firestorm's "Show
 *        Friends only") or every avatar but yourself (PlusHideOtherAvatars). Used by the avatar
 *        draw pool and by LLVOAvatar (idle update, visibility, too-slow) so they cannot disagree.
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

#ifndef PLUS_AVATARFILTER_H
#define PLUS_AVATARFILTER_H

#include "llviewercontrol.h"
#include "llvoavatar.h"

namespace plus
{

// True when the avatar must not be drawn or updated. Never true for yourself, for the UI avatar and for
// someone set to "Always render fully";
// animesh follows its wearer (none: never). With both settings off it is two cached flag reads (no lookup by
// name), so it is cheap enough for the per-avatar, per-frame paths.
inline bool isHiddenByAvatarFilter(const LLVOAvatar* avatar)
{
    static LLCachedControl<bool> friends_only(gSavedSettings, "RenderAvatarFriendsOnly", false);
    static LLCachedControl<bool> hide_others(gSavedSettings, "PlusHideOtherAvatars", false);
    if (!friends_only() && !hide_others())
    {
        return false;
    }
    if (avatar->isUIAvatar() || avatar->isSelf())
    {
        return false;
    }
    // Someone set to "Always render fully" is never hidden: the user asked for that one to be seen. (An animesh
    // follows its wearer below, so a worn pet stays too.) The avatar rendering mode ("Always show friends") plays no
    // part: with "hide others" the friends are hidden too.
    if (avatar->isAlwaysRenderFully())
    {
        return false;
    }
    if (avatar->isControlAvatar())
    {
        // Animesh worn by someone (a pet, a worn creature) follows the avatar wearing it; animesh
        // placed in the world (no avatar wearing it) is never hidden. getAttachedAvatar() is
        // never a control avatar, so this recurses once.
        const LLVOAvatar* wearer = avatar->getAttachedAvatar();
        return wearer && isHiddenByAvatarFilter(wearer);
    }
    return hide_others() || !avatar->isBuddy();
}

} // namespace plus

#endif // PLUS_AVATARFILTER_H
