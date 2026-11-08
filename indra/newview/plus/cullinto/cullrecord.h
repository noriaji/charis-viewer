/**
 * @file cullrecord.h
 * @brief What a main camera cull walk on a cull thread leaves for the main
 *        thread (cull-par, research/184 A1 in the sl-viewer repository).
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

#ifndef PLUS_CULLINTO_CULLRECORD_H
#define PLUS_CULLINTO_CULLRECORD_H

#include "stdtypes.h"

#include <vector>

class LLViewerOctreeGroup;

// Small on purpose: included by the official files whose walk code records
// (pipeline.cpp markNotCulled / markOccluder, llspatialpartition.cpp
// calcDistance, llvieweroctree.cpp the occlusion check). The driver is
// plus/cullinto/cullinto.h.
//
// While a thread's tRecord is set (a cull thread's job, or the main thread's
// own part of the split walk), the walk writes only the groups of its own
// subtree. What it would write elsewhere is recorded instead, in the walk's
// order, and the main thread does it after the join in the serial walk's
// order (plus::cullinto::run):
//  - the pushes to LLPipeline::sCull (visible and drawable groups),
//  - LLPipeline::markOccluder (it writes the parent's ACTIVE_OCCLUSION and
//    pushes occlusion groups; a sibling's call reads the parent's state),
//  - LLPipeline::markRebuild of a group (gPipeline's build queue),
//  - query names given back (the free names are one FIFO for every group),
//  - the occluded / unoccluded counts (LLTrace) and mNumVisibleNodes.
// Nothing the walk reads is among those, so recording changes no result.
namespace plus::cullinto
{
    struct Op
    {
        enum Kind : U32
        {
            VISIBLE_GROUP,      // sCull->pushVisibleGroup
            DRAWABLE_GROUP,     // sCull->pushDrawableGroup
            MARK_OCCLUDER,      // gPipeline.markOccluder
            MARK_REBUILD,       // gPipeline.markRebuild (the group)
            RELEASE_QUERY,      // LLOcclusionCullingGroup::releaseOcclusionQueryObjectName(name)
        };
        U32 kind;
        U32 name;                       // RELEASE_QUERY
        LLViewerOctreeGroup* group;     // the others (an LLSpatialGroup)

        bool operator==(const Op& other) const
        {
            return kind == other.kind && name == other.name && group == other.group;
        }
    };

    // Occlusion answers seen by a walk, kept for the verify's second walk
    // (the GPU may finish a query between the two: both must read the same).
    struct Answer
    {
        U64 key;        // name << 1 | (GL_QUERY_RESULT ? 1 : 0)
        U32 value;
        bool operator<(const Answer& other) const { return key < other.key; }
    };

    struct Record
    {
        enum Answers : U32
        {
            ANSWERS_PASS,       // as always
            ANSWERS_LOG,        // as always, kept in answers
            ANSWERS_REPLAY,     // from *replay (sorted), the usual way for any not there
        };

        std::vector<Op> ops;
        std::vector<Answer> answers;
        const std::vector<Answer>* replay = nullptr;
        U32 answers_mode = ANSWERS_PASS;
        U32 answer_misses = 0;
        U32 occluded = 0;
        U32 unoccluded = 0;
        U32 visible_nodes = 0;
        bool failed = false;

        void clear()
        {
            ops.clear();
            answers.clear();
            replay = nullptr;
            answers_mode = ANSWERS_PASS;
            answer_misses = 0;
            occluded = unoccluded = visible_nodes = 0;
            failed = false;
        }

        void push(U32 kind, LLViewerOctreeGroup* group) { ops.push_back({ kind, 0, group }); }
        void pushName(U32 name) { ops.push_back({ Op::RELEASE_QUERY, name, nullptr }); }
    };

    // The calling thread's record while its walk records, else null.
    extern thread_local Record* tRecord;

    // LLRHI::Occlusion::getQueryObjectuiv, or the verify's kept answer.
    void queryObject(U32 name, U32 pname, U32* params);
}

#endif // PLUS_CULLINTO_CULLRECORD_H
