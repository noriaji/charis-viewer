/**
 * @file idleskip.cpp
 * @brief Texture animations and spins (llTargetOmega) of objects nobody sees
 *        are not stepped every frame (research/178 D1, D2).
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

// On by default; LLRHI_IDLE_SKIP_TEXANIM=0 / LLRHI_IDLE_SKIP_OMEGA=0 (cue
// idleskiptexanim.toggle / idleskipomega.toggle) step every one every frame as
// upstream. LLRHI_IDLE_SKIP_STATS=1 (idleskipstats.toggle): a line every 5 s
// (tag IdleSkip).
//
// What upstream does: LLViewerTextureAnim::updateClass steps every texture
// animation in the world each frame (2,407 in heavy-extreme), seen or not,
// and every spinning object (839) turns in LLViewerObject::idleUpdate, is
// marked moved and goes through LLPipeline::updateMove (research/178 D).
//
// Here an object no camera took in the last SEEN_FRAMES frames is stepped one
// frame in HIDDEN_PERIOD (spread over the frames by the object), so a hidden
// one is at most HIDDEN_PERIOD - 1 frames behind. Both are functions of time:
// the texture animation of its own timer, the spin of the time since its last
// update (applyAngularVelocity(dt) with dt from mLastInterpUpdateSecs, about
// a fixed axis), so the step after a gap lands where the frame by frame steps
// would have. The one picture that differs from upstream: the frame an object
// comes into view (its cull runs after the idle update) shows it up to
// HIDDEN_PERIOD - 1 frames behind; the next frame it is where upstream has
// it. The counters' "shown behind" counts those (object, frame) pairs.
//
// Seen memo (research/180 9 1, on unless LLRHI_IDLE_SKIP_MEMO=0, cue
// idleskipmemo.toggle): "seen" reads up to a dozen frame numbers through
// four or five pointers (the drawable's entry, its group, the root's, the
// bridge's) for each of some 3,000 animations and spins a frame, mostly cache
// misses (seen_one, 2.4 % of the main thread in heavy-leeaf-2). The culls
// only ever move those numbers up, to the frame they run in, so a drawable
// whose latest number is f stays seen through frame f + SEEN_FRAMES - 1
// whatever the culls do: the object keeps that frame (mPlusSeenUntil) and
// the next two frames answer from it, without reading the drawable. A
// linkset root keeps the answer of its own part (the root and its bridge,
// the same for every child) the same way (mPlusLinkSeenUntil) and, when it
// was not seen, the frame it found that (mPlusLinkHiddenAt: the culls do not
// run during the idle update, so the other children of that frame take it,
// as long as the root's part is read from the same groups and bridge,
// mPlusLinkHiddenFrom: the idle update moves attachments and makes bridges,
// bugloop round 1). The one case it can differ: a drawable that moves to
// another octree group, or a prim linked to another root, within those two
// frames, is taken as seen (stepped as upstream) up to two frames longer.
// LLRHI_IDLE_SKIP_MEMO_VERIFY=1 (idleskipmemoverify.toggle) answers the full
// way, compares with the memo and counts the differences (both directions;
// "hidden but seen" would skip an object upstream steps and must stay 0).
//
// Asked only when it decides (research/180 10.18, seenby; on unless
// LLRHI_SEEN_BY_MEMO=0, cue seenbymemo.toggle). After the memo, seen_by was
// still 2.5 % of the main thread (0.35 ms, heavy-leeaf-2): 93 % of the asks
// are hidden objects, read in full each frame. Two asks whose answer does not
// change what happens are left out:
// - The turn frame: a hidden object steps on its turn, a seen one steps
//   anyway, so on the object's turn (hidden_turn, one frame in HIDDEN_PERIOD)
//   it steps without asking (skipSpin: nor looking at its children's
//   effects). With LLRHI_IDLE_SKIP_STATS=1 one skipped the frame before is
//   still asked, for the "shown behind" count.
// - The root first: a child of a linkset is seen when its root's part is
//   (seen_by reads the child, then the root and its bridge). The root keeps
//   that part's frame (mPlusLinkSeenUntil) and its own (mPlusSeenUntil, the
//   same part for a root); either still running means the child is seen
//   through that frame, without reading the child's drawable, its entry, its
//   group or the root's drawable. The root is found by the objects
//   (getRootEdit: the child's object is at hand). It answers only "seen":
//   where the objects' root differs from the drawables' (an attachment's
//   child: the drawables' root is the avatar), it steps an object upstream
//   steps too, at worst one the full way would have left (counted by the
//   verify as "root first seen but hidden"). A hidden object is read in full
//   on every frame it could be left, as before.
// The decisions (step or leave) are the same as without these on every
// frame; only the asks are fewer. The IdleSkip line counts both ("turn not
// asked", "root first hits").

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "idleskip.h"

#include "llappviewer.h"
#include "lldrawable.h"
#include "llmeshrepository.h"
#include "llrhi.h"
#include "llspatialpartition.h"
#include "llstartup.h"
#include "lltexturefetch.h"
#include "llviewercamera.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewertexturelist.h"
#include "llvovolume.h"
#include "pipeline.h"

namespace
{
    // A drawable a camera took this many frames back or fewer counts as seen
    // (the frame counter moves once a frame, before the main view's cull).
    constexpr S32 SEEN_FRAMES = 3;
    // A hidden object still steps one frame in this many.
    constexpr U32 HIDDEN_PERIOD = 4;
    constexpr F32 SPIN_MIN_OMEGA_SQUARED = 0.00001f;   // as applyAngularVelocity

    struct Stats
    {
        U64 frames = 0;
        U64 anim_steps = 0;         // animateTextures calls (one per animation a frame)
        U64 anim_skipped = 0;
        U64 anim_behind = 0;        // stepped while seen right after a skip
        U64 spin_candidates = 0;    // pure spins looked at
        U64 spin_skipped = 0;
        U64 spin_behind = 0;
        U64 seen_asked = 0;         // seenRecently calls with a drawable
        U64 memo_hits = 0;          // answered by the object's memo
        U64 link_hits = 0;          // the root's part answered by the root's memo
        U64 verify_checked = 0;     // LLRHI::sIdleSkipMemoVerify: compared
        U64 verify_seen_hidden = 0; // the memo said seen, the full way hidden (taken as seen longer)
        U64 verify_hidden_seen = 0; // the memo said hidden, the full way seen (must stay 0)
        U64 turn_not_asked = 0;     // LLRHI::sSeenByMemo: turn frames stepped without asking
        U64 root_first_hits = 0;    // answered seen by the root's memo before reading the drawable
        U64 verify_root_checked = 0;        // verify: the root's memo said seen, compared with the full way
        U64 verify_root_seen_hidden = 0;    // ... and the full way said hidden (stepped as upstream; small)
        U64 matrices_built = 0;     // texture animation matrices built (noteTexAnimMatrices)
        U64 matrices_copied = 0;    // copied from the face before
        U64 matrices_differed = 0;  // LLRHI::sTexAnimMemoVerify: a copy unlike a fresh build (must stay 0)
    };
    Stats sStats;

    S32 now_frame()
    {
        return (S32)LLDrawable::getCurrentFrame();
    }

    bool recent(S32 frame)
    {
        return now_frame() - frame < SEEN_FRAMES;
    }

    // The last frame a "recent" frame number keeps its drawable seen.
    S32 seen_until(S32 latest)
    {
        return latest + SEEN_FRAMES - 1;
    }

    // The latest frame any camera took the drawable or its octree group, as
    // far as needed to tell whether it is recent (a recent one of the
    // drawable is enough: the group is not read).
    S32 latest_seen(LLDrawable* drawable)
    {
        S32 latest = (S32)drawable->getVisible();
        if (recent(latest))
        {
            return latest;
        }
        if (const LLSpatialGroup* group = drawable->getSpatialGroup())
        {
            latest = llmax(latest, group->getAnyVisible(), group->getVisible(LLViewerCamera::CAMERA_WATER0),
                           group->getVisible(LLViewerCamera::CAMERA_WATER1));
        }
        return latest;
    }

    // What the root's part was read from: the root's octree group, its bridge
    // and the bridge's group. The idle update does change these between two
    // asks of a frame (bugloop round 1, "memo hidden but seen" 1): an avatar's
    // update moves its attachments at once (updateMoveNormalAsync), and a root
    // made active gets a new bridge (LLDrawable::getSpatialPartition), which
    // starts as seen two frames back (LLDrawable::init: initVisible). So the
    // root's "not seen this frame" holds only while these are the same.
    // Compared only, never followed (a freed one is not reused during the
    // idle update: dead bridges and groups go after it).
    struct LinkFrom
    {
        const void* parts[3];
    };

    LinkFrom link_from(LLDrawable* root)
    {
        const LLSpatialBridge* bridge = root->getSpatialBridge();
        return { { root->getSpatialGroup(), bridge, bridge ? bridge->getSpatialGroup() : nullptr } };
    }

    bool same_from(const LLViewerObject* root_obj, const LinkFrom& from)
    {
        return root_obj->mPlusLinkHiddenFrom[0] == from.parts[0] && root_obj->mPlusLinkHiddenFrom[1] == from.parts[1]
            && root_obj->mPlusLinkHiddenFrom[2] == from.parts[2];
    }

    // LLRHI::sIdleSkipMemoVerify: a line for the first few cases, to tell why.
    void log_link(const char* what, const LLViewerObject* obj, LLDrawable* root, const LLViewerObject* root_obj,
                  const LinkFrom& from)
    {
        constexpr U32 LOG_MAX = 20;
        static U32 logged = 0;
        if (logged >= LOG_MAX)
        {
            return;
        }
        ++logged;
        LL_WARNS("IdleSkip") << llformat("seen memo: %s: object %u (root %u, %s%s), root %s, bridge %s; read from "
                                         "group %s, bridge %s, bridge group %s (frame %d)",
                                         what, obj ? obj->getLocalID() : 0u, root_obj->getLocalID(),
                                         root_obj->isAvatar() ? "avatar" : "object",
                                         root_obj->isAttachment() ? ", attachment" : "",
                                         root->isActive() ? "active" : "static",
                                         from.parts[1] ? "yes" : "no",
                                         root_obj->mPlusLinkHiddenFrom[0] == from.parts[0] ? "same" : "other",
                                         root_obj->mPlusLinkHiddenFrom[1] == from.parts[1] ? "same" : "other",
                                         root_obj->mPlusLinkHiddenFrom[2] == from.parts[2] ? "same" : "other",
                                         now_frame())
                             << LL_ENDL;
    }

    // The root's part (the root, unless it is the drawable already read, and
    // the root's bridge): its latest frame, as far as latest_seen reads.
    S32 latest_of_link(LLDrawable* root, bool root_read, S32 root_latest)
    {
        S32 latest = root_read ? root_latest : latest_seen(root);
        if (!recent(latest))
        {
            if (LLSpatialBridge* bridge = root->getSpatialBridge())
            {
                latest = llmax(latest, latest_seen(bridge));
            }
        }
        return latest;
    }

    // The last frame the root's memos keep a child of the object's linkset
    // seen, or -1 when the object is a root (or has no object root but itself).
    // Reads the objects only (the child's is at hand, the root's is what the
    // full way reads last).
    S32 root_seen_until(const LLViewerObject* obj)
    {
        if (!obj->getParent())
        {
            return -1;
        }
        const LLViewerObject* root = obj->getRootEdit();
        if (root == obj)
        {
            return -1;      // an attachment's root: its parent is the avatar
        }
        return llmax(root->mPlusLinkSeenUntil, root->mPlusSeenUntil);
    }

    // seenRecently's answer. obj: the drawable's object (its memo), or null.
    // memo: read and write the memos; verify: write them, but answer the
    // full way and compare.
    bool seen_by(LLViewerObject* obj, LLDrawable* drawable, bool memo, bool verify)
    {
        const S32 now = now_frame();
        ++sStats.seen_asked;
        const bool memo_seen = memo && obj && now <= obj->mPlusSeenUntil;
        if (memo_seen && !verify)
        {
            ++sStats.memo_hits;
            return true;
        }
        // The root first (research/180 10.18): its part seen, the child is.
        bool root_seen = false;
        if (!memo_seen && memo && obj && LLRHI::sSeenByMemo)
        {
            const S32 until = root_seen_until(obj);
            if (now <= until)
            {
                root_seen = true;
                if (!verify)
                {
                    ++sStats.root_first_hits;
                    obj->mPlusSeenUntil = until;
                    return true;
                }
            }
        }

        S32 latest = latest_seen(drawable);
        if (!recent(latest))
        {
            // A child of a linkset in a bridge: the bridge (the root's) too.
            LLDrawable* root = drawable;
            while (root->getParent())
            {
                root = root->getParent();
            }
            LLViewerObject* root_obj = memo ? root->getVObj().get() : nullptr;
            // The root's memo: 1 seen, 0 not seen this frame, -1 nothing kept.
            S32 link_memo = -1;
            S32 link = latest;
            if (root_obj)
            {
                if (now <= root_obj->mPlusLinkSeenUntil)
                {
                    link_memo = 1;
                    link = root_obj->mPlusLinkSeenUntil - (SEEN_FRAMES - 1);
                }
                else if (root_obj->mPlusLinkHiddenAt == now)
                {
                    const LinkFrom from = link_from(root);
                    if (same_from(root_obj, from))
                    {
                        link_memo = 0;  // not recent: latest stays as it is
                    }
                    else if (verify)
                    {
                        log_link("not seen, read from others since (read again)", obj, root, root_obj, from);
                    }
                }
            }
            if (link_memo >= 0 && !verify)
            {
                ++sStats.link_hits;
            }
            else
            {
                link = latest_of_link(root, root == drawable, latest);
                const bool link_seen = recent(link);
                if (verify && link_memo >= 0 && link_seen != (link_memo == 1))
                {
                    ++(link_seen ? sStats.verify_hidden_seen : sStats.verify_seen_hidden);
                    if (link_seen)
                    {
                        log_link("memo hidden but seen", obj, root, root_obj, link_from(root));
                    }
                }
                if (root_obj)
                {
                    if (link_seen)
                    {
                        root_obj->mPlusLinkSeenUntil = seen_until(link);
                    }
                    else
                    {
                        root_obj->mPlusLinkHiddenAt = now;
                        const LinkFrom from = link_from(root);
                        for (int i = 0; i < 3; ++i)
                        {
                            root_obj->mPlusLinkHiddenFrom[i] = from.parts[i];
                        }
                    }
                }
            }
            latest = llmax(latest, link);
        }

        const bool seen = recent(latest);
        if (obj && memo && seen)
        {
            obj->mPlusSeenUntil = seen_until(latest);
        }
        if (verify)
        {
            ++sStats.verify_checked;
            if (memo_seen && !seen)
            {
                ++sStats.verify_seen_hidden;
            }
            if (root_seen)
            {
                ++sStats.verify_root_checked;
                if (!seen)
                {
                    ++sStats.verify_root_seen_hidden;
                }
            }
        }
        return seen;
    }

    // seenRecently for a drawable and its object (the callers have the object
    // at hand: its memo is read without touching the drawable).
    bool seen_recently(LLViewerObject* obj, LLDrawable* drawable)
    {
        const bool verify = LLRHI::sIdleSkipMemoVerify;
        const bool memo = LLRHI::sIdleSkipMemo || verify;
        return seen_by(obj, drawable, memo, verify);
    }

    // Spread the hidden objects' frames.
    bool hidden_turn(const void* key)
    {
        return ((gFrameCount + (U32)(reinterpret_cast<uintptr_t>(key) >> 6)) % HIDDEN_PERIOD) == 0;
    }

    // research/180 10.18: the object's turn frame, on which it steps seen or
    // not, so the caller steps it without asking. Not while the counters run
    // and it was left the frame before: the full way then counts "shown
    // behind" as before (it steps there too).
    bool turn_without_asking(const LLViewerObject* obj, bool skipped_before)
    {
        if (!LLRHI::sSeenByMemo || !hidden_turn(obj) || (skipped_before && LLRHI::sIdleSkipStats))
        {
            return false;
        }
        ++sStats.turn_not_asked;
        return true;
    }

    // Whether the object or a child has something upstream would move with it
    // while hidden that others can see or hear: a particle source, a sound,
    // a light, or an avatar sitting on it.
    bool carries_effects(LLViewerObject* objectp)
    {
        if (objectp->isParticleSource() || objectp->isAudioSource()
            || static_cast<LLVOVolume*>(objectp)->getIsLightFast())
        {
            return true;
        }
        for (LLViewerObject* child : objectp->getChildren())
        {
            if (!child || child->isAvatar() || child->isParticleSource() || child->isAudioSource()
                || (child->getPCode() == LL_PCODE_VOLUME && static_cast<LLVOVolume*>(child)->getIsLightFast()))
            {
                return true;
            }
        }
        return false;
    }

    // The texture visit pace waits this long after the last loading frame.
    // Objects arriving 15 s on end with the camera still left paced textures
    // up to 3 rounds late (research/178 s8.2, late 2,022 in 5 s).
    constexpr F32 SETTLE_SECONDS = 3.f;

    struct LoadWatch
    {
        F32 quiet_since = 0.f;  // the last loading frame (or the restart)
        U32 last_frame = 0;     // of the last call
        bool loading = true;    // the last call's frame
    };
    LoadWatch sLoadWatch;

    // Cheapest first; one lock (the fetch's request map) when all else is quiet.
    bool loading_this_frame()
    {
        if (LLStartUp::getStartupState() < STATE_STARTED || gObjectList.mNumNewObjects > 0
            || LLViewerTextureList::sCreatePending > 0
            || gPipeline.mPriorityWaiting > 0 || gPipeline.mLoadWaitingInside > 0
            || LLMeshRepoThread::sActiveHeaderRequests > 0 || LLMeshRepoThread::sActiveLODRequests > 0
            || !gMeshRepo.mPendingRequests.empty())
        {
            return true;
        }
        LLTextureFetch* fetch = LLAppViewer::getTextureFetch();
        return fetch && fetch->getNumRequests() > 0;
    }
}

namespace plus::idleskip
{
    bool loadSettled()
    {
        LoadWatch& w = sLoadWatch;
        const F32 now = gFrameTimeSeconds;
        w.loading = loading_this_frame();
        if (w.loading || gFrameCount - w.last_frame > 1)
        {
            w.quiet_since = now;
        }
        w.last_frame = gFrameCount;
        return now - w.quiet_since >= SETTLE_SECONDS;
    }

    bool loadingNow()
    {
        return sLoadWatch.loading;
    }

    bool seenRecently(LLDrawable* drawable)
    {
        if (!drawable)
        {
            return true;    // nothing to tell by: as upstream
        }
        return seen_recently(drawable->getVObj().get(), drawable);
    }

    bool skipTexAnim(LLVOVolume* vobj, bool animating)
    {
        if (!LLRHI::sIdleSkipTexAnim)
        {
            return false;
        }
        ++sStats.anim_steps;
        if (!animating || vobj->isSelected() || vobj->isHUDAttachment())
        {
            return false;
        }
        LLDrawable* drawable = vobj->mDrawable;
        if (drawable && turn_without_asking(vobj, vobj->mPlusTexAnimSkipped))
        {
            vobj->mPlusTexAnimSkipped = false;
            return false;
        }
        if (!drawable || seen_recently(vobj, drawable))
        {
            if (vobj->mPlusTexAnimSkipped && drawable)
            {
                ++sStats.anim_behind;
            }
            vobj->mPlusTexAnimSkipped = false;
            return false;
        }
        if (hidden_turn(vobj))
        {
            vobj->mPlusTexAnimSkipped = false;
            return false;
        }
        vobj->mPlusTexAnimSkipped = true;
        ++sStats.anim_skipped;
        return true;
    }

    bool skipSpin(LLViewerObject* objectp)
    {
        if (!LLRHI::sIdleSkipOmega)
        {
            return false;
        }
        if (objectp->isDead() || objectp->isAvatar() || objectp->getPCode() != LL_PCODE_VOLUME
            || objectp->isAttachment() || objectp->isSelected()
            || objectp->getAngularVelocity().magVecSquared() <= SPIN_MIN_OMEGA_SQUARED
            || !objectp->getVelocity().isExactlyZero() || !objectp->getAcceleration().isExactlyZero()
            || objectp->isChanged(LLXform::MOVED | LLXform::SHIFTED))
        {
            objectp->mPlusSpinSkipped = false;
            return false;
        }
        ++sStats.spin_candidates;
        LLDrawable* drawable = objectp->mDrawable;
        if (drawable && turn_without_asking(objectp, objectp->mPlusSpinSkipped))
        {
            objectp->mPlusSpinSkipped = false;
            return false;
        }
        if (!drawable || seen_recently(objectp, drawable) || carries_effects(objectp))
        {
            if (objectp->mPlusSpinSkipped && drawable)
            {
                ++sStats.spin_behind;
            }
            objectp->mPlusSpinSkipped = false;
            return false;
        }
        if (hidden_turn(objectp))
        {
            objectp->mPlusSpinSkipped = false;
            return false;
        }
        objectp->mPlusSpinSkipped = true;
        ++sStats.spin_skipped;
        return true;
    }

    TexAnimMatrices::~TexAnimMatrices()
    {
        sStats.matrices_built += mBuilt;
        sStats.matrices_copied += mCopied;
        sStats.matrices_differed += mDiffered;
    }

    bool TexAnimMatrices::copy(F32 off_s, F32 off_t, F32 scale_s, F32 scale_t, F32 rot, LLMatrix4& out)
    {
        mValues[0] = off_s;
        mValues[1] = off_t;
        mValues[2] = scale_s;
        mValues[3] = scale_t;
        mValues[4] = rot;
        // Bit for bit (memcmp): the build is a function of these values alone.
        mCandidate = mHave && (LLRHI::sTexAnimMemo || LLRHI::sTexAnimMemoVerify) && !memcmp(mValues, mKept, sizeof(mValues));
        if (mCandidate && !LLRHI::sTexAnimMemoVerify)
        {
            out = mMatrix;
            ++mCopied;
            return true;
        }
        return false;
    }

    void TexAnimMatrices::built(const LLMatrix4& matrix)
    {
        ++mBuilt;
        if (mCandidate && memcmp(&matrix, &mMatrix, sizeof(LLMatrix4)))
        {
            ++mDiffered;    // verify only: copy() hands over only when not verifying
        }
        mMatrix = matrix;
        memcpy(mKept, mValues, sizeof(mKept));
        mHave = true;
        mCandidate = false;
    }

    void endFrame()
    {
        static F32 logged_at = 0.f;
        if (!LLRHI::sIdleSkipStats && !LLRHI::sIdleSkipMemoVerify && !LLRHI::sTexAnimMemoVerify)
        {
            sStats = Stats();   // counted while off too (a few adds): dropped each frame
            logged_at = gFrameTimeSeconds;
            return;
        }
        ++sStats.frames;
        if (gFrameTimeSeconds - logged_at < 5.f)
        {
            return;
        }
        logged_at = gFrameTimeSeconds;
        const Stats& s = sStats;
        const F64 f = (F64)llmax(s.frames, (U64)1);
        LL_INFOS("IdleSkip") << llformat("idle skip over %llu frames, per frame: texture animations [%s] %.0f, skipped %.0f, "
                                         "shown behind %.2f; spins [%s] %.0f, skipped %.0f, shown behind %.2f; "
                                         "seen asked %.0f, memo [%s] hits %.0f, root hits %.0f; seen by [%s] turn not asked %.0f, "
                                         "root first hits %.0f",
                                         (unsigned long long)s.frames,
                                         LLRHI::sIdleSkipTexAnim ? "on" : "off", s.anim_steps / f, s.anim_skipped / f,
                                         s.anim_behind / f,
                                         LLRHI::sIdleSkipOmega ? "on" : "off", s.spin_candidates / f, s.spin_skipped / f,
                                         s.spin_behind / f,
                                         s.seen_asked / f,
                                         LLRHI::sIdleSkipMemoVerify ? "verify" : LLRHI::sIdleSkipMemo ? "on" : "off",
                                         s.memo_hits / f, s.link_hits / f,
                                         LLRHI::sSeenByMemo ? "on" : "off", s.turn_not_asked / f, s.root_first_hits / f)
                             << (LLRHI::sIdleSkipMemoVerify
                                     ? llformat(" (verify checked %llu, memo seen but hidden %llu, memo hidden but seen %llu, "
                                                "root first checked %llu, root first seen but hidden %llu)",
                                                (unsigned long long)s.verify_checked, (unsigned long long)s.verify_seen_hidden,
                                                (unsigned long long)s.verify_hidden_seen, (unsigned long long)s.verify_root_checked,
                                                (unsigned long long)s.verify_root_seen_hidden)
                                     : std::string())
                             << llformat("; texture animation matrices [%s] built %.1f, copied from the face before %.1f",
                                         LLRHI::sTexAnimMemoVerify ? "verify" : LLRHI::sTexAnimMemo ? "on" : "off",
                                         s.matrices_built / f, s.matrices_copied / f)
                             << (LLRHI::sTexAnimMemoVerify
                                     ? llformat(" (verify: copies unlike a fresh build %llu)", (unsigned long long)s.matrices_differed)
                                     : std::string())
                             << LL_ENDL;
        sStats = Stats();
    }
}
