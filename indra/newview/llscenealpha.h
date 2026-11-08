/**
 * @file llscenealpha.h
 * @brief The alpha pool's batches on the retained scene's way (research/205, stage 3a): census and verify.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Charis Viewer Source Code
 * Copyright (C) 2026, Charis Viewer contributors
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

#ifndef LL_LLSCENEALPHA_H
#define LL_LLSCENEALPHA_H

#include "stdtypes.h"

#include <unordered_map>
#include <vector>

class LLDrawInfo;
class LLGLSLShader;
class LLSpatialGroup;

// research/205 (stage 3a): what the alpha pool's batches would be on the
// retained scene's way, measured and checked while the pool draws as before.
//
// Census (LLRHI_SCENE_ALPHA_CENSUS=1, any LLRHI_SCENE; 3a-0): the main
// view's alpha colour passes, by kind of pass (OIT all, the hybrid
// session's avatar and world halves, without OIT, the pre-water pool): the
// batches by what keeps them off the scene's first alpha step (rigged, GLTF,
// a partition other than volumes and bridges, a texture list, a legacy
// material, a custom blend) and the rest (fullbright, plain: research/205
// 3a-3's scope), and of those the spans (in a group, the batches next to
// each other in its draw order with the same program, vertex format and
// glow; what one indirect draw of the sorted way could take) and the buckets
// (the distinct keys of a pass: what the OIT session's way would draw, one
// indirect draw each per pool chunk). A line per kind every 10 s
// ("SceneAlpha").
//
// Verify (LLRHI_SCENE_ALPHA=verify, auto or on, which needs LLRHI_SCENE not off):
// an alpha record per non-rigged PASS_ALPHA batch, made from the same
// events as the scene's records (LLSceneRecords' entries call these first)
// and written at the next alpha pass (the batch is whole by then): its
// group, its place in the group's draw map (the low bits of its OIT
// priority), its program and a hash of what a GPU record would be written
// from. Every 8th frame, at each batch the main view's above-water colour
// passes draw: the batch has a record, in that group and place, with that
// content and program, and (OIT session) the priority the pool set is the
// group's priority with the record's place; and (passes of every group,
// GROUPS_ALL) every record of a drawn group was drawn. Differences are
// counted, the first 8 logged; research/218 vk-s2f: each verified pass and
// each difference go to the alpha's auto (LLSceneRecords::alphaChecked), and
// none is verified after it went off. It never draws: the scene's alpha records
// (llscenerecords.h, research/205 section 9) do; the batches the scene drew
// are noted by the loops with the program it drew them with.
//
// Main thread only. With both off every hook returns at once (sActive).
class LLSceneAlpha
{
public:
    static constexpr U32 VERIFY_EVERY = 8;      // frames between verified frames
    static constexpr F64 REPORT_SECONDS = 10.0;
    static constexpr U32 LOG_DIFFERENCES = 8;   // differences written to the log since the start

    // What keeps a batch off research/205 3a-3's scope (the first that fits), or its program there.
    enum Category : U8
    {
        CAT_RIGGED,
        CAT_GLTF,           // a GLTF material (blended: the PBR program; else not a record of the scope)
        CAT_PARTITION,      // not a volume or bridge group (particles, grass, HUD), or no indices
        CAT_TEXTURE_LIST,   // several diffuse textures (205 H5)
        CAT_MATERIAL,       // a legacy material: gDeferredMaterialProgram[mask] (205 H4, 3a-4)
        CAT_CUSTOM_BLEND,   // not SRC_ALPHA, ONE_MINUS_SRC_ALPHA (the minimum alpha differs)
        CAT_FULLBRIGHT,     // in the scope: the fullbright alpha program
        CAT_PLAIN,          // in the scope: the deferred alpha program
        CAT_COUNT,
    };

    enum PassKind : U8
    {
        KIND_OIT_ALL,
        KIND_HYBRID_AVATAR,
        KIND_HYBRID_WORLD,
        KIND_NO_OIT,
        KIND_PRE_WATER,
        KIND_COUNT,
    };

    enum Difference : U8
    {
        DIFF_MISSING,       // a drawn batch without a record (a missed onCreated)
        DIFF_GROUP,         // its record's group is another
        DIFF_PLACE,         // its record's place in the draw map is another
        DIFF_CONTENT,       // what it would be written from changed since
        DIFF_PROGRAM,       // the pool drew it with another program than the record says
        DIFF_PRIORITY,      // the pool's OIT priority is not the group's with the record's place
        DIFF_STALE,         // a record of a drawn group that was not drawn (a missed onDestroyed / clear)
        DIFF_COUNT,
    };

    // The switches, read once at startup (LLRHI_SCENE_ALPHA needs LLRHI_SCENE).
    static const bool sCensus;
    static const bool sVerify;
    static const bool sActive;  // census or verify: the hooks do anything

    // --- events (LLSceneRecords::onCreated and friends, first) ----------------
    void onCreated(LLDrawInfo& info, LLSpatialGroup* group, U32 pass);
    void onDestroyed(const LLDrawInfo& info);
    void onGroupCleared(LLSpatialGroup* group);

    // --- the alpha pool -----------------------------------------------------
    // Around LLDrawPoolAlpha::renderAlpha (renderAlphaList included): a pass.
    class PassScope
    {
    public:
        PassScope(bool post_water, bool depth_only, bool rigged, U32 groups, bool oit);
        ~PassScope();
        PassScope(const PassScope&) = delete;
        PassScope& operator=(const PassScope&) = delete;
    private:
        bool mOpen = false;
    };
    // A group the loop goes on to draw (after its water test).
    void noteGroup(LLSpatialGroup* group);
    // A batch the loop draws now, with its program bound: its place in the
    // group's draw map and the OIT priority the pool set for it.
    void noteBatch(LLSpatialGroup* group, const LLDrawInfo& params, size_t place, const LLGLSLShader* shader, U32 priority);

private:
    struct Record
    {
        LLSpatialGroup* group = nullptr;
        U32 place = 0;
        bool pending = true;        // made, not written yet
        U8 category = CAT_COUNT;
        U8 program = 0;             // ProgramKind (llscenealpha.cpp)
        U32 shader_mask = 0;
        U64 hash = 0;
        U32 seen = 0;               // the verified pass that drew it
    };

    struct PassCounts
    {
        U64 passes = 0;
        U64 batches = 0;
        U64 categories[CAT_COUNT] = {};
        U64 glow = 0;               // of the scope: an emissive buffer (the group's glow pass)
        U64 animated = 0;           // of the scope: a texture matrix
        U64 spans = 0;
        U64 longest = 0;
        U64 buckets = 0;            // summed over passes
    };

    void beginPass(bool post_water, bool depth_only, bool rigged, U32 groups, bool oit);
    void endPass();
    void writePending();
    void endGroup();
    void forget(const LLDrawInfo* info, Record& record);
    void difference(Difference what, const LLDrawInfo* info, const char* detail);
    void report();

    std::unordered_map<const LLDrawInfo*, Record> mRecords;
    std::unordered_map<const LLSpatialGroup*, std::vector<const LLDrawInfo*>> mGroupRecords;
    std::vector<const LLDrawInfo*> mPending;

    // The pass open now.
    bool mPass = false;
    bool mCensusPass = false;
    bool mVerifyPass = false;
    bool mCheckPriority = false;
    bool mCheckStale = false;
    U8 mKind = KIND_NO_OIT;
    U32 mPassSerial = 0;
    LLSpatialGroup* mGroup = nullptr;
    // Census: the open span and the pass's bucket keys.
    U64 mSpanKey = 0;               // 0: none open
    U64 mSpanLength = 0;
    std::vector<U64> mBuckets;

    // Verify.
    U32 mFrame = 0;
    bool mFrameKnown = false;
    bool mVerifyFrame = false;
    U32 mLogged = 0;
    bool mStarted = false;          // the first pass logged the switches

    // Since the last report.
    F64 mReportAt = 0.0;
    U32 mReportFrames = 0;
    PassCounts mCounts[KIND_COUNT];
    U64 mChecked = 0, mPendingSeen = 0, mVerifiedPasses = 0;
    U64 mCheckedBefore = 0;         // research/218 vk-s2f: mChecked at the verified pass's start
    U64 mDifferences[DIFF_COUNT] = {};
    U64 mCreated = 0, mWritten = 0, mDropped = 0;
};

extern LLSceneAlpha& gSceneAlpha;

#endif // LL_LLSCENEALPHA_H
