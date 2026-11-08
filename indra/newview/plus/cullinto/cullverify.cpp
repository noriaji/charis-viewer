/**
 * @file cullverify.cpp
 * @brief What the verifies of the main camera's cull walks share (cull-par
 *        research/184 A1, cull-reuse research/188 in the sl-viewer repository).
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

#include "cullverify.h"

#include "llspatialpartition.h"
#include "pipeline.h"

#include <cstring>

namespace plus::cullinto
{
namespace
{
    // Every group of an octree, in a fixed order.
    class MarksTaker : public OctreeTraveler
    {
    public:
        explicit MarksTaker(std::vector<GroupMarks>& out) : mOut(out) { }

        virtual void visit(const OctreeNode* node)
        {
            LLSpatialGroup* group = (LLSpatialGroup*)node->getListener(0);
            if (!group)
            {
                return;
            }
            GroupMarks m;
            m.group = group;
            group->getCullMarks(m.marks);
            m.distance = group->mDistance;
            m.depth = group->mDepth;
            m.pixel_area = group->mPixelArea;
            m.radius = group->mRadius;
            memcpy(m.view_angle, group->mViewAngle.getF32ptr(), sizeof(m.view_angle));
            memcpy(m.last_view_angle, group->mLastUpdateViewAngle.getF32ptr(), sizeof(m.last_view_angle));
            mOut.push_back(m);
        }

    private:
        std::vector<GroupMarks>& mOut;
    };
}

void takeMarks(LLSpatialPartition* part, std::vector<GroupMarks>& out)
{
    MarksTaker taker(out);
    taker.traverse(part->mOctree);
}

void putMarks(const std::vector<GroupMarks>& marks)
{
    for (const GroupMarks& m : marks)
    {
        LLSpatialGroup* group = m.group;
        group->setCullMarks(m.marks);
        group->mDistance = m.distance;
        group->mDepth = m.depth;
        group->mPixelArea = m.pixel_area;
        group->mRadius = m.radius;
        group->mViewAngle.loadua(m.view_angle);
        group->mLastUpdateViewAngle.loadua(m.last_view_angle);
    }
}

bool sameMarks(const GroupMarks& a, const GroupMarks& b)
{
    return a.group == b.group && a.marks.group_state == b.marks.group_state &&
           a.marks.occlusion_state == b.marks.occlusion_state && a.marks.query == b.marks.query &&
           a.marks.check_count == b.marks.check_count && a.marks.visible == b.marks.visible &&
           a.marks.any_visible == b.marks.any_visible && !memcmp(&a.distance, &b.distance, sizeof(F32)) &&
           !memcmp(&a.depth, &b.depth, sizeof(F32)) && !memcmp(&a.pixel_area, &b.pixel_area, sizeof(F32)) &&
           !memcmp(&a.radius, &b.radius, sizeof(F32)) && !memcmp(a.view_angle, b.view_angle, sizeof(a.view_angle)) &&
           !memcmp(a.last_view_angle, b.last_view_angle, sizeof(a.last_view_angle));
}

void applyRecord(const Record& record, LLCullResult& result)
{
    for (const Op& op : record.ops)
    {
        LLSpatialGroup* group = static_cast<LLSpatialGroup*>(op.group);
        switch (op.kind)
        {
        case Op::VISIBLE_GROUP:
            result.pushVisibleGroup(group);
            break;
        case Op::DRAWABLE_GROUP:
            result.pushDrawableGroup(group);
            break;
        case Op::MARK_OCCLUDER:
            gPipeline.markOccluder(group);
            break;
        case Op::MARK_REBUILD:
            gPipeline.markRebuild(group);
            break;
        case Op::RELEASE_QUERY:
            LLOcclusionCullingGroup::releaseOcclusionQueryObjectName(op.name);
            break;
        default:
            break;
        }
    }
    LLOcclusionCullingGroup::addOcclusionCounts(record.occluded, record.unoccluded);
    gPipeline.mNumVisibleNodes += (S32)record.visible_nodes;
}
}
