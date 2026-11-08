/**
 * @file gpudgc.cpp
 * @brief DGC (VK_EXT_device_generated_commands, the GPU writing the probe
 *        faces' draw commands) by the setting PlusGpuDrivenDGC: auto (NVIDIA's
 *        GPUs only), on or off, from the next start (dgc-nvidia-only;
 *        research/124 V5 in the sl-viewer repository).
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

// DGC was checked on the RTX 2080 only, and drivers differ a lot in it, so in
// the alpha it is on by default on NVIDIA's GPUs only. The Vulkan device asks
// for the extension once, when it is made (LLVKDevice::createDevice, with the
// vendor of the GPU it renders on). That is inside LLAppViewer::initWindow(),
// after the plus features' init, where gSavedSettings already holds the
// user's value: init() hands LLRHI the choice there. A change in the menu
// takes effect at the next start. LLRHI_GPU_DRIVEN_DGC (development) wins
// over the setting; the device's log line "DGC: on/off (why)" tells which.
//
// When the device is lost with DGC in use in two starts in a row, the setting
// becomes off for the next start (onDeviceLost(), from plus/vkcheck's exit;
// fix-round9b 4, fix-round10 3). One loss is not enough: a GPU that is too hot
// (a fan that fails), a driver update or a GPU overloaded by something else
// end a start alike, and DGC is not to blame for those. PlusGpuDrivenDGCLostCount
// counts those starts in a row. A start where DGC runs breaks the row unless it
// ends with the device lost: the count is 0 in this session from when DGC first
// runs (saved when the settings are: a normal end, or plus/vkcheck's exit for a
// failed submit or a lost window), and a loss counts on what it was at the start
// (sLossesBefore). DGC that ran HEALTHY_SECONDS before the loss breaks the row
// too. Also 0 when the user picks a choice in the menu and when DGC goes off.
// A failed submit (most often out of memory) is not counted. A start where DGC
// did not run (it ended at the login screen, or with a loss before DGC ran)
// leaves the count as it was. A crash of another kind saves nothing: the count
// stays as it was at the start.

#include "llviewerprecompiledheaders.h"

#include "gpudgc.h"

#include "llrhi.h"
#include "llcallbacklist.h"
#include "llcontrol.h"
#include "lltimer.h"
#include "lluictrl.h"
#include "llviewercontrol.h"

#include "plus.h"

namespace
{
    constexpr char SETTING[] = "PlusGpuDrivenDGC";   // 0 auto (NVIDIA only), 1 on, 2 off
    constexpr char LOST_COUNT[] = "PlusGpuDrivenDGCLostCount";
    constexpr S32 LOSSES_TO_TURN_OFF = 2;           // starts in a row ended by a lost device with DGC in use
    constexpr F32 HEALTHY_SECONDS = 600.f;          // DGC ran this long in a start: the losses before are forgotten
    constexpr F32 HEALTHY_CHECK_SECONDS = 5.f;

    enum Choice : S32
    {
        AUTO = 0,
        ON = 1,
        OFF = 2,
    };

    // The setting's value; anything else (a hand-edited settings.xml) is auto.
    S32 setting()
    {
        LLControlVariable* control = gSavedSettings.getControl(SETTING);
        const S32 value = control ? (S32)control->getValue().asInteger() : AUTO;
        return value == ON || value == OFF ? value : AUTO;
    }

    bool fromEnvironment()
    {
        return LLRHI::sGPUDrivenDGCChoice == LLRHI::DGCChoice::EnvOn ||
               LLRHI::sGPUDrivenDGCChoice == LLRHI::DGCChoice::EnvOff;
    }

    S32 lostCount()
    {
        LLControlVariable* control = gSavedSettings.getControl(LOST_COUNT);
        return control ? llmax((S32)control->getValue().asInteger(), 0) : 0;
    }

    void setLostCount(S32 count)
    {
        if (gSavedSettings.getControl(LOST_COUNT) && lostCount() != count)
        {
            gSavedSettings.setS32(LOST_COUNT, count);
        }
    }

    // The starts in a row before this one that ended with the device lost while DGC was in use:
    // what a loss in this start counts on (onDeviceLost()).
    S32 sLossesBefore = 0;

    // Every HEALTHY_CHECK_SECONDS while losses are counted from before. Once DGC runs in this
    // start, the count is 0 for the settings (saved when this start ends any other way than with
    // a lost device: the row is broken); once it has run HEALTHY_SECONDS, the GPU and its driver
    // take it, and a loss after that is the first again. True: done.
    bool forgetLossesWhenHealthy()
    {
        static LLTimer since;
        static bool running = false;
        if (sLossesBefore == 0)
        {
            return true;    // forgotten already (a choice in the menu)
        }
        if (!LLRHI::sGPUDrivenDGCUsed.load(std::memory_order_relaxed))
        {
            return false;   // DGC has not run yet in this start
        }
        if (!running)
        {
            since.reset();
            running = true;
            setLostCount(0);
            return false;
        }
        if (since.getElapsedTimeF32() < HEALTHY_SECONDS)
        {
            return false;
        }
        LL_INFOS("Plus") << "DGC ran " << (S32)HEALTHY_SECONDS << " s without a lost device: the "
                         << sLossesBefore << " lost before are forgotten" << LL_ENDL;
        sLossesBefore = 0;
        return true;
    }

    void init()
    {
        // Preferences > Charis: skins/default/xui/<lang>/panel_preferences_charis.xml (cb_dgc; plus/prefscharis
        // reads Is and Choosable).
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.GpuDrivenDGC.Is",
            [](LLUICtrl*, const LLSD& param) { return setting() == param.asInteger(); });
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.GpuDrivenDGC.Set",
            [](LLUICtrl*, const LLSD& param)
            {
                const S32 value = (S32)param.asInteger();
                gSavedSettings.setS32(SETTING, value == ON || value == OFF ? value : AUTO);
                setLostCount(0);    // the user's choice: the losses are counted again from there
                sLossesBefore = 0;
            });
        // The choices can be changed unless the environment decides.
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.GpuDrivenDGC.Choosable",
            [](LLUICtrl*, const LLSD&) { return !fromEnvironment(); });
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.GpuDrivenDGC.Never",
            [](LLUICtrl*, const LLSD&) { return false; });

        if (fromEnvironment())
        {
            LL_INFOS("Plus") << SETTING << " not used: LLRHI_GPU_DRIVEN_DGC is set" << LL_ENDL;
            return;
        }
        const S32 value = setting();
        LLRHI::sGPUDrivenDGCChoice = value == ON ? LLRHI::DGCChoice::On : value == OFF ? LLRHI::DGCChoice::Off : LLRHI::DGCChoice::Auto;
        if (value == OFF)
        {
            setLostCount(0);
        }
        else if (lostCount() > 0)
        {
            sLossesBefore = lostCount();
            LL_INFOS("Plus") << LOST_COUNT << " " << sLossesBefore << ": DGC goes off when the device is lost with it in use in "
                             << LOSSES_TO_TURN_OFF << " starts in a row" << LL_ENDL;
            doPeriodically(&forgetLossesWhenHealthy, HEALTHY_CHECK_SECONDS);
        }
    }

    plus::FeatureRegistration sRegistration({ "gpudgc", &init });
}

plus::gpudgc::Loss plus::gpudgc::onDeviceLost()
{
    if (!LLRHI::sGPUDrivenDGCUsed.load(std::memory_order_relaxed) || fromEnvironment() || setting() == OFF)
    {
        return Loss::NotCounted;
    }
    const S32 count = sLossesBefore + 1;
    if (count < LOSSES_TO_TURN_OFF)
    {
        LL_WARNS("Plus") << "The device was lost with DGC in use (" << count << " start of " << LOSSES_TO_TURN_OFF
                         << " in a row before DGC goes off)" << LL_ENDL;
        setLostCount(count);
        return Loss::Counted;
    }
    LL_WARNS("Plus") << "The device was lost with DGC in use in " << count << " starts in a row: " << SETTING
                     << " set off (" << OFF << ") for the next start" << LL_ENDL;
    gSavedSettings.setS32(SETTING, OFF);
    setLostCount(0);
    return Loss::TurnedOff;
}
