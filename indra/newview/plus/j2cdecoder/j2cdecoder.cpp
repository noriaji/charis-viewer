/**
 * @file j2cdecoder.cpp
 * @brief Settings of slj2k, the Charis JPEG 2000 decoder tried before OpenJPEG.
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

#include "plus.h"

#include "llcontrol.h"
#include "llviewercontrol.h"

// In llimagej2coj/slj2k/plus_j2c.cpp (the library is linked in through llimage)
namespace plus_j2c
{
    void setConfig(S32 mode, U32 threads, U32 minArea);
}

namespace
{
    constexpr char SETTING_ENABLED[]  = "PlusJ2CDecoder";
    constexpr char SETTING_THREADS[]  = "PlusJ2CDecoderThreads";
    constexpr char SETTING_MIN_AREA[] = "PlusJ2CDecoderThreadsMinArea";

    void apply()
    {
        plus_j2c::setConfig(gSavedSettings.getBOOL(SETTING_ENABLED) ? 1 : 0, gSavedSettings.getU32(SETTING_THREADS),
                            gSavedSettings.getU32(SETTING_MIN_AREA));
    }

    void init()
    {
        for (const char* name : { SETTING_ENABLED, SETTING_THREADS, SETTING_MIN_AREA })
        {
            if (LLControlVariable* control = gSavedSettings.getControl(name).get())
            {
                control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { apply(); });
            }
        }
        apply();
    }

    plus::FeatureRegistration sRegistration({ "j2cdecoder", &init, {} });
}
