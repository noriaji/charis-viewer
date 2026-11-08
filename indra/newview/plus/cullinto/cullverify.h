/**
 * @file cullverify.h
 * @brief What the verifies of the main camera's cull walks share: a walk
 *        recorded on the calling thread, the groups' marks taken and put back,
 *        a record done in order (cull-par research/184 A1, cull-reuse
 *        research/188 in the sl-viewer repository).
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

#ifndef PLUS_CULLINTO_CULLVERIFY_H
#define PLUS_CULLINTO_CULLVERIFY_H

#include "cullrecord.h"
#include "llvieweroctree.h"     // LLOcclusionCullingGroup::CullMarks

#include <vector>

class LLCullResult;
class LLSpatialGroup;
class LLSpatialPartition;

namespace plus::cullinto
{
    // The calling thread's record while a walk runs (also on an exception).
    class RecordScope
    {
    public:
        explicit RecordScope(Record* record) : mSaved(tRecord) { tRecord = record; }
        ~RecordScope() { tRecord = mSaved; }
        RecordScope(const RecordScope&) = delete;
        RecordScope& operator=(const RecordScope&) = delete;
    private:
        Record* mSaved;
    };

    // What a walk writes on a group (getCullMarks and LLSpatialGroup's own).
    struct GroupMarks
    {
        LLSpatialGroup* group;
        LLOcclusionCullingGroup::CullMarks marks;
        F32 distance;
        F32 depth;
        F32 pixel_area;
        F32 radius;
        F32 view_angle[4];
        F32 last_view_angle[4];
    };

    // Every group of part's octree, in a fixed order, appended to out.
    void takeMarks(LLSpatialPartition* part, std::vector<GroupMarks>& out);
    // The groups left as marks took them.
    void putMarks(const std::vector<GroupMarks>& marks);
    // Bit for bit.
    bool sameMarks(const GroupMarks& a, const GroupMarks& b);

    // What a recorded walk would have written, done now in its order: the
    // pushes to result, markOccluder, markRebuild, the query names given back,
    // the counts.
    void applyRecord(const Record& record, LLCullResult& result);
}

#endif // PLUS_CULLINTO_CULLVERIFY_H
