/**
 * @file chatconsole.h
 * @brief Chat console: nearby chat at the bottom left of the screen (see chatconsole.cpp).
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

#ifndef PLUS_CHATCONSOLE_H
#define PLUS_CHATCONSOLE_H

namespace plus::chatconsole
{
    // True while the console shows the nearby chat (PlusChatConsoleOn): the official toasts of new chat
    // lines are left out then, or every line would be on the screen twice.
    //
    // Hook: one call in LLFloaterIMNearbyChatHandler::processChat() (llfloaterimnearbychathandler.cpp),
    // after the line is stored and posted to the "LLChat" event pump, before the toast is made.
    bool replacesToasts();

    // How long a nearby chat line is kept (PlusChatConsolePersistTime, seconds; at least FADE_SECONDS + 1)
    // and the last stretch of that it fades over. Shared with the detailed photo screen's own chat lines
    // (photodetail_hud.cpp), so both fade out over the same time.
    constexpr float FADE_SECONDS = 2.f;
    float persistTime();

    // 1 right after `added` (LLFrameTimer::getElapsedSeconds()), fading to 0 over the last FADE_SECONDS
    // before persistTime() is reached.
    float fadeAlpha(double added, double now);
}

#endif // PLUS_CHATCONSOLE_H
