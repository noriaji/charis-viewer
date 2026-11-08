/**
 * @file shadowfar.cpp
 * @brief The sun shadow cascades leave out small moving casters far from the
 *        camera (PlusShadowSkipFarSmall, research/178 C2 in the sl-viewer
 *        repository).
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

// Why the sort is the place (research/178 C):
//  - renderSunShadowCascades sorts the groups the cascades' culls found. The
//    groups of spatial bridges (active objects) always go to the casters drawn
//    every frame (the "moving" lists, one per cascade); the static casters go
//    to the cached depth. Leaving a bridge group out there takes it out of
//    every cascade's moving list and touches nothing the cache's fingerprints
//    read (bridge groups are never hashed).
//  - The sort already looks at every group once (no walk is added). The test
//    is a few loads and one squared distance per bridge group.
//  - Size and distance are the bridge's own: its world extents
//    (LLSpatialBridge::updateSpatialExtents), so a linkset is judged whole.
//  - Avatars, attachments (worn or HUD) and animated objects (a control
//    avatar) are never left out: people's shadows always stay. Rigged meshes
//    are attachments or animated objects, so they stay too.
//  - Nothing is kept from one frame to the next.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "shadowfar.h"

#include "llcontrol.h"
#include "lldrawable.h"
#include "llframetimer.h"
#include "llrhi.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"

#include <atomic>

extern bool gSnapshot;

namespace plus::shadowfar
{
namespace detail
{
    bool sActive = false;
}

namespace
{
    constexpr char SETTING_ON[] = "PlusShadowSkipFarSmall";
    constexpr char SETTING_DISTANCE[] = "PlusShadowSkipFarSmallDistance";
    constexpr char SETTING_RADIUS[] = "PlusShadowSkipFarSmallRadius";
    constexpr char SWITCH_ENV[] = "LLRHI_SHADOW_FAR_SMALL";    // "0": never (the official way)
    constexpr char SWITCH_CUE[] = "shadowfarsmall.toggle";     // LLRHI_CUE_DIR: the setting flipped, not saved
    constexpr F64 CUE_POLL_SECONDS = 1.0;
    constexpr F64 REPORT_SECONDS = 5.0;
    // settings_plus.xml's defaults (the standard preset's values)
    constexpr F32 DEFAULT_DISTANCE = 48.f;
    constexpr F32 DEFAULT_RADIUS = 0.25f;
    constexpr F32 MIN_DISTANCE = 8.f;
    constexpr F32 MAX_DISTANCE = 1024.f;
    constexpr F32 MAX_RADIUS = 4.f;

    // Set on the main thread in beginSort, read by the sort (any thread) after it.
    struct Params
    {
        LLVector4a eye;
        F32 distance = DEFAULT_DISTANCE;
        F32 radius = DEFAULT_RADIUS;
    };
    Params sParams;

    struct Totals
    {
        std::atomic<U64> tested{ 0 };
        std::atomic<U64> left_out{ 0 };
        std::atomic<U64> left_out_cascades{ 0 };
    };
    Totals sTotals;
    U64 sFrames = 0;
    F64 sNextReport = 0.0;

    bool switchAllowed()
    {
        static const bool allowed = LLRHI::envDefaultOn(SWITCH_ENV);
        return allowed;
    }

    // The cue's flip; its own function so the log line reads "...::poll : ..."
    // (what tools/native-ab.ps1 takes as the toggle's answer).
    void poll()
    {
        LLControlVariable* control = gSavedSettings.getControl(SETTING_ON);
        if (!control)
        {
            LL_INFOS("RHIProfile") << "shadow far small (research/178 C2): no " << SETTING_ON << " setting, unchanged" << LL_ENDL;
            return;
        }
        const bool on = !control->getValue().asBoolean();
        control->setValue(LLSD(on), false);
        LL_INFOS("RHIProfile") << "shadow far small (research/178 C2) " << (on ? "on" : "off")
                               << (switchAllowed() ? "" : " (held off by LLRHI_SHADOW_FAR_SMALL=0)") << LL_ENDL;
    }

    // At most once a second, with LLRHI_CUE_DIR only.
    void pollCue()
    {
        static LLRHI::CuePoll cue_poll(CUE_POLL_SECONDS);
        if (cue_poll.due() && LLRHI::takeCue(SWITCH_CUE))
        {
            poll();
        }
    }

    // Every REPORT_SECONDS while frames were counted: one VKPerf line, then
    // the counts start again.
    void report()
    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (now < sNextReport)
        {
            return;
        }
        sNextReport = now + REPORT_SECONDS;
        if (!sFrames)
        {
            return;
        }
        const F64 per = 1.0 / (F64)sFrames;
        const U64 tested = sTotals.tested.exchange(0, std::memory_order_relaxed);
        const U64 left_out = sTotals.left_out.exchange(0, std::memory_order_relaxed);
        const U64 left_out_cascades = sTotals.left_out_cascades.exchange(0, std::memory_order_relaxed);
        LL_INFOS("VKPerf") << "ShadowFarSmall (research/178 C2): " << sFrames << " frames; per frame bridge groups "
                           << (U64)(tested * per + 0.5) << ", left out " << (U64)(left_out * per + 0.5)
                           << " (from " << (U64)(left_out_cascades * per + 0.5) << " cascades); beyond "
                           << sParams.distance << " m, radius up to " << sParams.radius << " m" << LL_ENDL;
        sFrames = 0;
    }
}

namespace detail
{
    bool leaveOutSlow(LLSpatialGroup* group, U32 mask, Count& count)
    {
        LLSpatialBridge* bridge = group->getSpatialPartition()->asBridge();
        if (!bridge || bridge->isDead())
        {
            return false;
        }
        ++count.tested;
        LLDrawable* root = bridge->mDrawable;
        if (!root || root->isDead() || root->isAvatar())
        {
            return false;
        }
        const LLViewerObject* object = root->getVObj().get();  // no reference taken (any thread)
        if (!object || object->isDead() || object->isAttachment() || object->getControlAvatar())
        {
            return false;
        }
        const LLVector4a* ext = bridge->getSpatialExtents();
        LLVector4a centre, half;
        centre.setAdd(ext[0], ext[1]);
        centre.mul(0.5f);
        half.setSub(ext[1], ext[0]);
        half.mul(0.5f);
        const F32 radius = half.getLength3().getF32();
        // Not known yet (no extents), not finite, or not small: kept.
        if (!(radius > 0.f) || !(radius <= sParams.radius))
        {
            return false;
        }
        LLVector4a to;
        to.setSub(centre, sParams.eye);
        const F32 dist2 = to.dot3(to).getF32();
        const F32 reach = sParams.distance + radius;    // its nearest point beyond the distance
        if (!(dist2 > reach * reach))
        {
            return false;
        }
        ++count.left_out;
        for (U32 m = mask; m; m &= m - 1)
        {
            ++count.left_out_cascades;
        }
        return true;
    }
}

void beginSort(bool main_view)
{
    static LLCachedControl<bool> on(gSavedSettings, SETTING_ON, true);
    static LLCachedControl<F32> distance(gSavedSettings, SETTING_DISTANCE, DEFAULT_DISTANCE);
    static LLCachedControl<F32> radius(gSavedSettings, SETTING_RADIUS, DEFAULT_RADIUS);
    detail::sActive = false;
    if (!main_view)
    {
        return;
    }
    pollCue();
    report();
    if (!on || !switchAllowed() || gSnapshot)
    {
        return;
    }
    const F32 d = (F32)distance;
    const F32 r = (F32)radius;
    sParams.distance = llfinite(d) ? llclamp(d, MIN_DISTANCE, MAX_DISTANCE) : DEFAULT_DISTANCE;
    sParams.radius = llfinite(r) ? llclamp(r, 0.f, MAX_RADIUS) : DEFAULT_RADIUS;
    if (sParams.radius <= 0.f)
    {
        return;     // nothing is that small
    }
    sParams.eye.load3(LLViewerCamera::getInstance()->getOrigin().mV);
    detail::sActive = true;
    ++sFrames;
}

void addCount(const Count& count)
{
    if (!count.tested)
    {
        return;
    }
    sTotals.tested.fetch_add(count.tested, std::memory_order_relaxed);
    sTotals.left_out.fetch_add(count.left_out, std::memory_order_relaxed);
    sTotals.left_out_cascades.fetch_add(count.left_out_cascades, std::memory_order_relaxed);
}
}
