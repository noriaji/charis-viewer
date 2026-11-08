/**
 * @file drawlistreport.cpp
 * @brief The draw lists' viewer-side counts on the VKPerf line (research/137
 *        N7 in the sl-viewer repository).
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

// The opaque pools' lists (plus/drawlist, research/137 N2) and the alpha
// pool's (plus/alphalist, N3) count what they did on the main thread
// (takeStats). The Vulkan backend writes the VKPerf line every 5 seconds,
// inside the present: it asks LLRHI::DrawList::report, which asks the
// reports handed in here. The legacy material and bump lists
// (plus/materiallist, N4) have no counts of their own: the backend's part of
// the line counts every list by its name; their report is the threaded
// build's (research/137 N11). Nothing here runs per frame.

#include "llviewerprecompiledheaders.h"

#include "llrhidrawlist.h"

#include "plus.h"
#include "plus/alphalist/alphalist.h"
#include "plus/drawlist/drawlistbatch.h"
#include "plus/materiallist/materiallist.h"

#include <iomanip>
#include <sstream>

namespace
{
    std::string opaqueReport(U32 frames)
    {
        plus::drawlist::Stats s;
        plus::drawlist::takeStats(s);
        if (!s.loops && !s.draws)
        {
            return std::string();
        }
        const F64 per = 1.0 / llmax(frames, 1u);
        std::ostringstream out;
        out << std::fixed << std::setprecision(2);
        out << "loops " << s.loops * per << ", batches listed " << s.draws * per << ", old path: mapped " << s.old_mapped * per
            << " marked " << s.old_marked * per << " texture " << s.old_texture * per << " refused " << s.old_refused * per
            << ", palettes by row " << s.palette_rows * per << " as patches " << s.palette_patches * per << ", uniform patches "
            << s.uniform_patches * per << ", marked active " << s.marked_active * per;
        return out.str();
    }

    std::string alphaReport(U32 frames)
    {
        plus::alphalist::Stats s;
        plus::alphalist::takeStats(s);
        if (!s.passes && !s.listed && !s.old_path)
        {
            return std::string();
        }
        const F64 per = 1.0 / llmax(frames, 1u);
        std::ostringstream out;
        out << std::fixed << std::setprecision(2);
        out << "passes " << s.passes * per << ", batches listed " << s.listed * per << ", old path " << s.old_path * per << ", cuts [";
        bool first = true;
        for (U32 i = 0; i < plus::alphalist::CUT_COUNT; ++i)
        {
            if (s.cuts[i])
            {
                out << (first ? "" : ", ") << plus::alphalist::cutName(i) << " " << s.cuts[i] * per;
                first = false;
            }
        }
        out << "]";
        return out.str();
    }

    // research/137 N11: the material loops built on a cull thread.
    std::string materialReport(U32 frames)
    {
        plus::materiallist::BuildStats s;
        plus::materiallist::takeBuildStats(s);
        if (!s.loops && !s.no_thread)
        {
            return std::string();
        }
        const F64 per = 1.0 / llmax(frames, 1u);
        std::ostringstream out;
        out << std::fixed << std::setprecision(2);
        out << "built on a cull thread: loops " << s.loops * per << " (not: no cull thread " << s.no_thread * per << "), chunks "
            << s.chunks * per << ", draws " << s.draws * per << ", old path " << s.old_path * per << ", loops refused "
            << s.refused * per << ", main thread waited " << s.wait_ns * 1e-6 * per << " ms, cull thread built " << s.build_ns * 1e-6 * per
            << " ms";
        return out.str();
    }

    void init()
    {
        LLRHI::DrawList::setOwnerReport(LLRHI::DrawList::KIND_OPAQUE, &opaqueReport);
        LLRHI::DrawList::setOwnerReport(LLRHI::DrawList::KIND_ALPHA, &alphaReport);
        LLRHI::DrawList::setOwnerReport(LLRHI::DrawList::KIND_MATERIAL, &materialReport);
    }

    plus::FeatureRegistration sRegistration({ "drawlistreport", &init });
}
