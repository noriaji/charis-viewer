/**
 * @file minimapchat.cpp
 * @brief Draws the ranges text chat carries (whisper, say, shout) on the minimap.
 *
 *        waiting for the simulator to drop the region.
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

#include "minimapchat.h"

#include "indra_constants.h"
#include "v4color.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llviewercontrol.h"

namespace plus::minimapchat
{
    namespace
    {
        constexpr S32 STEPS = 72;

        struct Ring
        {
            F32 meters;
            LLColor4 line;
            LLColor4 fill;
        };

        // Whisper, say, shout: half, once and five times the normal chat radius.
        const Ring RINGS[] = {
            { CHAT_NORMAL_RADIUS * 5.f, LLColor4(1.f, 0.4f, 0.3f, 0.75f), LLColor4(1.f, 0.4f, 0.3f, 0.04f) },
            { CHAT_NORMAL_RADIUS,       LLColor4(0.3f, 1.f, 0.4f, 0.75f), LLColor4(0.3f, 1.f, 0.4f, 0.06f) },
            { CHAT_NORMAL_RADIUS * 0.5f, LLColor4(0.4f, 0.7f, 1.f, 0.75f), LLColor4(0.4f, 0.7f, 1.f, 0.08f) },
        };
    }

    void drawRanges(float x, float y, float pixels_per_meter)
    {
        static LLCachedControl<bool> show(gSavedSettings, "MiniMapShowChatRange", true);
        if (!show)
        {
            return;
        }

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.pushMatrix();
        gGL.translatef(x, y, 0.f);
        for (const Ring& ring : RINGS)
        {
            const F32 radius = ring.meters * pixels_per_meter;
            gGL.color4fv(ring.fill.mV);
            gl_circle_2d(0.f, 0.f, radius, STEPS, true);
            gGL.color4fv(ring.line.mV);
            gl_circle_2d(0.f, 0.f, radius, STEPS, false);
        }
        gGL.popMatrix();
    }
}
