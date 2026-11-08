/**
 * @file nudge.h
 * @brief Fine adjustment of the avatar's facing and position, for lining up
 *        with others. Everyone sees it: the facing goes out in AgentUpdate's
 *        BodyRotation, the position steps are the simulator's own nudges.
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

#ifndef PLUS_NUDGE_H
#define PLUS_NUDGE_H

#include "llkeyboard.h"

#include <functional>

class LLFloater;

namespace plus::nudge
{
    // Keyboard actions (registered in llviewerinput.cpp as "nudge_*").
    // dir: +1 = left / forward, -1 = right / back.
    bool keyTurn(EKeystate state, S32 dir);
    bool keyStepAt(EKeystate state, S32 dir);
    bool keyStepLeft(EKeystate state, S32 dir);

    // Binds the default keys (Ctrl+Left/Right, Ctrl+Shift+arrows) through bind,
    // LLViewerInput::bindKey. Called at the end of LLViewerInput::loadBindingsXML(),
    // so a user's own key_bindings.xml gets them too. A key and mask that file gives to
    // another command stays that command's (bind leaves it and logs it).
    using BindKey = std::function<bool(S32 mode, KEY key, MASK mask, const char* command)>;
    void bindKeys(const BindKey& bind);

    // Connects the buttons of the Move floater (panel_nudge in floater_moveview.xml).
    void setupFloater(LLFloater* floater);
}

#endif // PLUS_NUDGE_H
