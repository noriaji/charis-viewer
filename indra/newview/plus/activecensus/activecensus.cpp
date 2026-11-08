/**
 * @file activecensus.cpp
 * @brief Why drawables became active (the census, R0), and the orphan reunion
 *        that leaves a still static parent static (R1), research/180 10.12.
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

#include "activecensus.h"

#include "llappviewer.h"        // gFrameTimeSeconds
#include "lldrawable.h"
#include "llframetimer.h"
#include "llprimitive.h"        // LL_PCODE_VOLUME
#include "llrhihash.h"
#include "llspatialpartition.h" // LLSpatialBridge, LLSpatialGroup
#include "llthread.h"           // on_main_thread
#include "llviewercamera.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "pipeline.h"           // LLPipeline::sShadowRender

#include <chrono>
#include <sstream>
#include <vector>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn

namespace plus::activecensus
{
    bool sVerifyPending = false;

    namespace
    {
        constexpr F32 REPORT_SECONDS = 5.f;
        constexpr U32 VERIFY_AFTER_FRAMES = 30;
        constexpr size_t VERIFY_MAX = 4096;     // pending checks kept (more reunions in 30 frames go unchecked, counted)
        constexpr U8 STILL_REPORTS = 2;         // 2 reports in a row unchanged: still for 10 s or more
        constexpr F32 POSITION_EPSILON_SQUARED = 1.e-6f;    // 1 mm
        constexpr F32 ROTATION_DOT_MIN = 0.99999f;
        constexpr F32 ROUND_ROTATION = 1.e5f;   // the census's still test: matrix rows rounded to 1e-5 ...
        constexpr F32 ROUND_POSITION = 1.e4f;   // ... and the position to 0.1 mm (the low bits may wobble)
        constexpr U32 SAMPLES_MAX = 3;          // moving linksets named per line
        constexpr U32 SRC_KEYS = SRC_COUNT * 2; // a source, and the same put on the list by a child
        constexpr U8 SRC_IN_MOVE = 0x20;        // sMoveNow, mPlusActiveSource: in a move (any source)
        static_assert(SRC_COUNT <= SRC_IN_MOVE, "the sources fit below the flags");

        const char* const WHY_NAMES[WHY_COUNT] =
        {
            "before", "orphan", "move", "anim", "physics", "omega", "edit",
            "attachment", "flex", "animesh", "volume", "link", "other"
        };

        const char* const SRC_NAMES[SRC_COUNT] =
        {
            "before", "full", "terse", "compressed", "cached", "parent of an update", "idle",
            "idle spin", "idle motion", "idle other", "link", "orphan", "edit", "attachment", "other"
        };

        // Why a reunion went upstream's way (census only).
        enum Refusal : U8
        {
            REFUSE_PARENT_ACTIVE = 0,   // already active: its move changes nothing
            REFUSE_PARENT_LINKED,       // not a root (or an attachment)
            REFUSE_ON_MOVE_LIST,        // moving anyway this frame
            REFUSE_KIND,                // not a plain volume, attachment, flex, animesh, selected
            REFUSE_MOTION,              // velocity, acceleration, spin, physics, region shift
            REFUSE_CHILD,               // the child active, an avatar, flex or spinning
            REFUSE_MOVED,               // its drawable not where the object is: the move does something
            REFUSE_COUNT
        };
        const char* const REFUSAL_NAMES[REFUSE_COUNT] =
        {
            "parent active", "parent linked", "on the move list", "kind", "motion", "child", "moved"
        };

        // Main thread only.
        U32 sDepth = 0;                     // nested makeActive calls
        U8 sCurrent = WHY_OTHER;         // the outermost one's reason
        Reason sScope = WHY_BEFORE;      // ReasonScope's, WHY_BEFORE: none
        MoveSource sMoveSource = SRC_BEFORE;        // MoveScope's, SRC_BEFORE: none
        const LLViewerObject* sMoveObject = nullptr;  // MoveScope's object (for the length of the scope only)
        U32 sViaChild = 0;                          // markMoved's recursions to the parent
        U8 sMoveNow = 0;                            // the move updateMove makes: source, SRC_IN_MOVE, SRC_SAME

        struct Activated
        {
            U32 roots[WHY_COUNT] = {};
            U32 prims[WHY_COUNT] = {};
        };
        Activated sActivated;               // since the last report

        // Moves since the last report (census): the first updateMove after a
        // drawable was put on the list, by source.
        struct Moves
        {
            U32 moves[SRC_KEYS] = {};
            U32 same[SRC_KEYS] = {};        // position, rotation and scale already the object's
            U32 position[SRC_KEYS] = {};    // of the others, what changed
            U32 rotation[SRC_KEYS] = {};
            U32 scale[SRC_KEYS] = {};
            U32 static_moves[SRC_KEYS] = {};    // of the moves, of a static drawable (upstream's move makes it active)
            U32 static_same[SRC_KEYS] = {};     // ... and changing nothing
            U32 act_roots[SRC_KEYS] = {};   // made active in a move (with the parents and children made active along)
            U32 act_prims[SRC_KEYS] = {};
            U32 act_same_prims[SRC_KEYS] = {};  // ... in a move changing nothing
        };
        Moves sMoves;

        // The move same (LLRHI::sMoveSame), since the last report.
        struct Same
        {
            U32 kept = 0;                   // static drawables left static
            U32 refused_source = 0;         // changing nothing, but a link change, orphan, edit, attachment, other
            U32 refused_kind = 0;           // ... not a plain still volume
            U32 refused_parent = 0;         // ... its parent active, or not its xform's parent
            U32 checked = 0;                // the verify right after the move
            U32 ok = 0;
            U32 mismatch = 0;               // its world position or rotation not the object's
            U32 active_after = 0;           // made active in the rest of the move anyway
        };
        Same sSame;

        struct Reunions
        {
            U32 reunions = 0;
            U32 eligible = 0;               // would stay static (counted with R1 off too)
            U32 kept = 0;                   // left static (R1 on)
            U32 refused[REFUSE_COUNT] = {};
        };
        Reunions sReunions;

        enum VerifyKind : U8
        {
            VERIFY_ORPHAN = 0,              // a reunion kept static (R1)
            VERIFY_SAME,                    // a move same kept static
            VERIFY_KINDS
        };
        const char* const VERIFY_NAMES[VERIFY_KINDS] = { "verify", "move same verify 30 frames on" };

        struct Pending
        {
            LLUUID id;                      // the linkset's root
            U32 due;                        // LLFrameTimer frame
            VerifyKind kind;
        };
        std::vector<Pending> sPending;

        struct Verify
        {
            U32 checked = 0;
            U32 ok = 0;
            U32 root_mismatch = 0;
            U32 child_mismatch = 0;         // linksets with a child off its place, or active under a static root
            U32 active_again = 0;           // made active since by another way (upstream's path from then on)
            U32 gone = 0;
            U32 children = 0;
            U32 children_unbuilt = 0;       // a child's rebuild still queued (the rebuild budget; not a mismatch)
            U32 dropped = 0;                // over VERIFY_MAX
        };
        Verify sVerify[VERIFY_KINDS];

        // --- research/186 step 1: the spin census ---
        constexpr F32 SPIN_RECENT_SECONDS = 10.f;   // a held root's last own move: recent ...
        constexpr F32 SPIN_MINUTE_SECONDS = 60.f;   // ... within a minute, or older
        constexpr F64 SPIN_GATE_US = 300.0;         // research/186 5: under 0.3 ms a frame, no step 2

        const char* const COST_NAMES[COST_KINDS] =
        {
            "move", "root moves for the child", "root moves of its own (aside)", "bridge moves",
            "bridge state sort (main view)", "cull", "bridge culls main view", "bridge culls shadows",
            "bridge culls other views"
        };

        // Costs since the last line, of the linksets flagged at the line before it.
        struct SpinCost
        {
            U64 ns[COST_KINDS] = {};
            U32 calls[COST_KINDS] = {};
            U32 lod_updates = 0;            // updateDistance in stateSort(bridge)
            U32 lod_prims = 0;              // ... the prims it walks (the root and its children)
            U32 seen_infos = 0;             // the main view's sorted bridges' draw infos (each its object's matrix)
            U32 shadow_groups = 0;          // the main view's sun cascades: the bridges' groups (moving casters)
            U32 shadow_infos = 0;           // ... their draw infos times the cascades that saw them
        };
        SpinCost sSpinCost;
        U32 sSpinFlagged = 0;               // the linksets flagged at the last line (what sSpinCost is of)
        U32 sSpinFlaggedPrims = 0;
        U32 sCostDepth = 0;                 // CostScopes timing (main thread)

        // The held linksets now (census, every 5 s).
        struct SpinNow
        {
            U32 roots = 0, prims = 0, spinning = 0, infos = 0;
            U32 never = 0;                  // the root never moved for its own reason (changing something)
            U32 once = 0;                   // once: as a rule, its placing when it was loaded
            U32 recent = 0;                 // more than once, the last within SPIN_RECENT_SECONDS
            U32 minute = 0;                 // ... within SPIN_MINUTE_SECONDS
            U32 older = 0;                  // ... longer ago
            U32 last_source[SRC_COUNT] = {};    // of those moved more than once: the last move's source
        };
        SpinNow sSpinNow;

        S64 nowNanoseconds()
        {
            return (S64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // The root of a linkset flagged at the last line, for its root's or
        // its bridge's drawable; null for anything else.
        LLDrawable* flaggedRoot(LLDrawable* drawable)
        {
            LLDrawable* root = drawable->isSpatialBridge() ? ((LLSpatialBridge*)drawable)->mDrawable : drawable;
            if (!root || root->isDead() || !root->mPlusSpinHeld || !root->isActive() || !root->isRoot())
            {
                return nullptr;
            }
            return root;
        }

        U32 drawInfos(const OctreeNode* node)
        {
            U32 infos = 0;
            const LLSpatialGroup* group = (const LLSpatialGroup*)node->getListener(0);
            if (group)
            {
                for (const auto& type : group->mDrawMap)
                {
                    infos += (U32)type.second.size();
                }
            }
            for (U32 i = 0; i < node->getChildCount(); ++i)
            {
                infos += drawInfos(node->getChild(i));
            }
            return infos;
        }

        // A held root at the census: its bridge's draw infos, when it last
        // moved for its own reason.
        void spinHeld(LLDrawable* drawable, U32 prims, U32 spinning, F32 now)
        {
            SpinNow& s = sSpinNow;
            ++s.roots;
            s.prims += prims;
            s.spinning += spinning;
            LLSpatialBridge* bridge = drawable->getSpatialBridge();
            const U32 infos = (bridge && bridge->mOctree) ? drawInfos(bridge->mOctree) : 0;
            drawable->mPlusSpinInfos = (U16)llmin(infos, 65535u);
            s.infos += infos;
            if (drawable->mPlusOwnMoves == 0)
            {
                ++s.never;
            }
            else if (drawable->mPlusOwnMoves == 1)
            {
                ++s.once;
            }
            else
            {
                const F32 age = now - drawable->mPlusOwnMoveAt;
                (age <= SPIN_RECENT_SECONDS ? s.recent : age <= SPIN_MINUTE_SECONDS ? s.minute : s.older) += 1;
                ++s.last_source[llmin((U32)drawable->mPlusOwnMoveSource, (U32)SRC_COUNT - 1)];
            }
        }

        bool zero(const LLVector3& v)
        {
            return v.isExactlyZero();
        }

        bool selected(const LLViewerObject* vobj)
        {
            const LLViewerObject* parent = (const LLViewerObject*)vobj->getParent();
            return vobj->isSelected() || (parent && parent->isSelected());
        }

        U8 classify(LLDrawable* drawable)
        {
            if (drawable->mPlusMoveReason)
            {
                const U8 reason = drawable->mPlusMoveReason;
                drawable->mPlusMoveReason = 0;
                return reason;
            }
            if (sScope != WHY_BEFORE)
            {
                return sScope;
            }
            const LLViewerObject* vobj = drawable->getVObj();
            if (!vobj)
            {
                return WHY_OTHER;
            }
            if (vobj->isAttachment())
            {
                return WHY_ATTACHMENT;
            }
            if (vobj->isAnimatedObject())
            {
                return WHY_ANIMESH;
            }
            if (vobj->isFlexible())
            {
                return WHY_FLEX;
            }
            if (selected(vobj))
            {
                return WHY_EDIT;
            }
            if (!zero(vobj->getAngularVelocity()))
            {
                return WHY_OMEGA;
            }
            if (!zero(vobj->getVelocity()) || !zero(vobj->getAcceleration()) || vobj->flagUsePhysics())
            {
                return WHY_PHYSICS;
            }
            if (drawable->isState(LLDrawable::ON_MOVE_LIST))
            {
                return drawable->isRoot() ? WHY_MOVE : WHY_ANIM;
            }
            for (const LLPointer<LLViewerObject>& child : vobj->getChildren())
            {
                if (child && child->mDrawable.notNull() && child->mDrawable->isActive())
                {
                    return WHY_LINK;
                }
            }
            return WHY_OTHER;
        }

        // The matrices' hashes: FNV-1a (llrhihash.h), kept in the drawable's 32-bit fields folded
        // (kept()). They are only compared with the same linkset's at the last report.

        // A matrix rounded (rows to 1e-5, the position to 0.1 mm; -0 and 0 alike).
        U64 hashRounded(U64 hash, const LLMatrix4& matrix)
        {
            for (U32 row = 0; row < 4; ++row)
            {
                const F32 scale = row == 3 ? ROUND_POSITION : ROUND_ROTATION;
                for (U32 column = 0; column < 4; ++column)
                {
                    const S32 value = ll_round(matrix.mMatrix[row][column] * scale);
                    hash = LLRHI::fnv1a(hash, &value, sizeof(value));
                }
            }
            return hash;
        }

        // A hash as the drawable keeps it: 32 bits, never 0 (0: no hash yet).
        U32 kept(U64 hash)
        {
            const U32 folded = (U32)(hash ^ (hash >> 32));
            return folded ? folded : 1;
        }

        // A root linkset's world matrices: the root's and the children's that
        // do not spin (rounded; and the both unrounded), and the spinning
        // children's apart (they turn by design: a spin keeps the linkset
        // active, it is not what the still test asks).
        struct LinksetHashes
        {
            U32 root = 0;                   // kept()
            U32 children = 0;
            U32 raw = 0;
            U32 spin = 0;
            U32 prims = 1;
            U32 spinning = 0;               // spinning children
        };

        void linksetHashes(LLViewerObject* root, LinksetHashes& out)
        {
            U64 children = LLRHI::FNV1A_BASIS;
            U64 raw = LLRHI::FNV1A_BASIS;
            U64 spin = LLRHI::FNV1A_BASIS;
            const LLMatrix4& matrix = root->mDrawable->mXform.getWorldMatrix();
            out.root = kept(hashRounded(LLRHI::FNV1A_BASIS, matrix));
            raw = LLRHI::fnv1a(raw, &matrix, sizeof(LLMatrix4));
            for (const LLPointer<LLViewerObject>& child : root->getChildren())
            {
                if (!child || child->isAvatar() || child->mDrawable.isNull())
                {
                    continue;
                }
                const LLMatrix4& child_matrix = child->mDrawable->mXform.getWorldMatrix();
                ++out.prims;
                if (!child->getAngularVelocity().isExactlyZero())
                {
                    ++out.spinning;
                    spin = LLRHI::fnv1a(spin, &child_matrix, sizeof(LLMatrix4));
                }
                else
                {
                    children = hashRounded(children, child_matrix);
                    raw = LLRHI::fnv1a(raw, &child_matrix, sizeof(LLMatrix4));
                }
            }
            out.children = kept(children);
            out.raw = kept(raw);
            out.spin = kept(spin);
        }

        // research/180 10.12 c4-c7: what a static linkset could not be.
        // ignore_spin: as if its spinning children did not spin.
        bool returnable(LLViewerObject* root, bool ignore_spin)
        {
            if (root->isAttachment() || root->isAnimatedObject() || root->isFlexible() || selected(root) ||
                !zero(root->getAngularVelocity()) || !zero(root->getVelocity()) || !zero(root->getAcceleration()) ||
                root->flagUsePhysics())
            {
                return false;
            }
            for (const LLPointer<LLViewerObject>& child : root->getChildren())
            {
                if (!child)
                {
                    continue;
                }
                const bool spins = !zero(child->getAngularVelocity());
                if (child->isAvatar() || child->isFlexible() || child->isSelected() || (spins && !ignore_spin) ||
                    (!spins && child->mDrawable.notNull() && child->mDrawable->isState(LLDrawable::ANIMATED_CHILD)))
                {
                    return false;
                }
            }
            return true;
        }

        Refusal refusal(LLViewerObject* parent, LLViewerObject* child, LLDrawable* drawable, bool& eligible)
        {
            eligible = false;
            if (drawable->isActive())
            {
                return REFUSE_PARENT_ACTIVE;
            }
            if (!drawable->isRoot() || parent->getParent())
            {
                return REFUSE_PARENT_LINKED;
            }
            if (drawable->isState(LLDrawable::ON_MOVE_LIST))
            {
                return REFUSE_ON_MOVE_LIST;
            }
            if (parent->getPCode() != LL_PCODE_VOLUME || parent->isAvatar() || parent->isAttachment() ||
                parent->isHUDAttachment() || parent->isAnimatedObject() || parent->isFlexible() || parent->isSelected())
            {
                return REFUSE_KIND;
            }
            if (!zero(parent->getVelocity()) || !zero(parent->getAcceleration()) || !zero(parent->getAngularVelocity()) ||
                parent->flagUsePhysics() || parent->isChanged(LLXform::SHIFTED))
            {
                return REFUSE_MOTION;
            }
            if (child->isAvatar() || child->isFlexible() || !zero(child->getAngularVelocity()) ||
                (child->mDrawable.notNull() && child->mDrawable->isActive()))
            {
                return REFUSE_CHILD;
            }
            // What upstream's move (updateMoveUndamped: updateXform(true)) would set for a root.
            if (drawable->mXform.getPosition() != parent->getPositionAgent() ||
                drawable->mXform.getRotation() != parent->getRotation() ||
                drawable->getScale() != parent->getScale())
            {
                return REFUSE_MOVED;
            }
            eligible = true;
            return REFUSE_COUNT;
        }

        bool samePlace(const LLVector3& position, const LLQuaternion& rotation,
                       const LLVector3& want_position, const LLQuaternion& want_rotation)
        {
            return dist_vec_squared(position, want_position) <= POSITION_EPSILON_SQUARED &&
                   fabsf(dot(rotation, want_rotation)) >= ROTATION_DOT_MIN;
        }

        // A linkset left static (a reunion, a move same), 30 frames on: the
        // root and its children where the objects are, as upstream's move
        // would have put them.
        void verify(const LLUUID& id, VerifyKind kind)
        {
            Verify& v = sVerify[kind];
            ++v.checked;
            LLViewerObject* root = gObjectList.findObject(id);
            if (!root || root->isDead() || root->mDrawable.isNull() || root->mDrawable->isDead())
            {
                ++v.gone;
                return;
            }
            LLDrawable* drawable = root->mDrawable;
            if (drawable->isActive())
            {
                ++v.active_again;
                return;
            }
            bool good = true;
            if (!samePlace(drawable->mXform.getWorldPosition(), drawable->mXform.getWorldRotation(),
                           root->getPositionAgent(), root->getRotation()))
            {
                ++v.root_mismatch;
                good = false;
            }
            bool child_good = true;
            for (const LLPointer<LLViewerObject>& child : root->getChildren())
            {
                if (!child || child->isAvatar() || child->mDrawable.isNull() || child->mDrawable->isDead())
                {
                    continue;
                }
                LLDrawable* child_drawable = child->mDrawable;
                ++v.children;
                const LLVector3 want_position = root->getPositionAgent() + child->getPosition() * root->getRotation();
                const LLQuaternion want_rotation = child->getRotation() * root->getRotation();
                if (child_drawable->isActive() ||
                    !samePlace(child_drawable->mXform.getWorldPosition(), child_drawable->mXform.getWorldRotation(),
                               want_position, want_rotation))
                {
                    child_good = false;
                }
                if (child_drawable->isState(LLDrawable::REBUILD_VOLUME | LLDrawable::REBUILD_POSITION))
                {
                    ++v.children_unbuilt;
                }
            }
            if (!child_good)
            {
                ++v.child_mismatch;
                good = false;
            }
            if (good)
            {
                ++v.ok;
            }
        }

        void verifyDue()
        {
            const U32 now = LLFrameTimer::getFrameCount();
            size_t kept = 0;
            for (size_t i = 0; i < sPending.size(); ++i)
            {
                if ((S32)(now - sPending[i].due) >= 0)
                {
                    verify(sPending[i].id, sPending[i].kind);
                }
                else
                {
                    sPending[kept++] = sPending[i];
                }
            }
            sPending.resize(kept);
            sVerifyPending = !sPending.empty();
        }

        void appendReasons(std::ostringstream& out, const U32* roots, const U32* prims)
        {
            bool any = false;
            for (U32 r = 0; r < WHY_COUNT; ++r)
            {
                if (roots[r] || (prims && prims[r]))
                {
                    out << (any ? ", " : " ") << WHY_NAMES[r] << " " << roots[r];
                    if (prims)
                    {
                        out << "/" << prims[r];
                    }
                    any = true;
                }
            }
            if (!any)
            {
                out << " none";
            }
        }

        U32 sourceKey(U8 source)
        {
            const U32 base = llmin((U32)(source & SRC_MASK), (U32)SRC_COUNT - 1);
            return base + ((source & SRC_VIA_CHILD) ? SRC_COUNT : 0);
        }

        const char* sourceName(U32 key)
        {
            return SRC_NAMES[key % SRC_COUNT];
        }

        void appendSourceName(std::ostringstream& out, U8 source)
        {
            if (!(source & SRC_IN_MOVE))
            {
                out << "not in a move";
                return;
            }
            const U32 key = sourceKey(source);
            out << (key >= SRC_COUNT ? "child " : "") << sourceName(key) << ((source & SRC_SAME) ? " (same)" : "");
        }

        // The moves by source: "[child ]source moves/same" and what the
        // others changed, the moves of static drawables, and the prims made
        // active in them.
        void appendMoves(std::ostringstream& out)
        {
            bool any = false;
            for (U32 k = 0; k < SRC_KEYS; ++k)
            {
                if (!sMoves.moves[k] && !sMoves.act_prims[k])
                {
                    continue;
                }
                out << (any ? ", " : " ") << (k >= SRC_COUNT ? "child " : "") << sourceName(k) << " " << sMoves.moves[k]
                    << "/" << sMoves.same[k] << " (changed position " << sMoves.position[k] << ", rotation "
                    << sMoves.rotation[k] << ", scale " << sMoves.scale[k] << "; static " << sMoves.static_moves[k]
                    << "/" << sMoves.static_same[k] << "; made active " << sMoves.act_roots[k] << "/" << sMoves.act_prims[k]
                    << ", in moves changing nothing " << sMoves.act_same_prims[k] << ")";
                any = true;
            }
            if (!any)
            {
                out << " none";
            }
        }

        // One moving linkset, named in the sample line: what of it changed
        // since the last line.
        struct Sample
        {
            LLUUID id;
            U8 reason;
            U8 source;
            bool root;                      // its own matrix changed
            bool children;                  // a child's that does not spin
            bool on_move_list;
            S32 moves;                      // updateMoves of the root since the last line
            U32 prims;
            U32 spinning;
        };

        // The objects as they are now (every 5 s, census only).
        void census(std::ostringstream& out, std::vector<Sample>& samples)
        {
            U32 active_prims[WHY_COUNT] = {};
            U32 roots[WHY_COUNT] = {};
            U32 root_prims[WHY_COUNT] = {};
            U32 still_roots[WHY_COUNT] = {};
            U32 still_prims[WHY_COUNT] = {};
            U32 source_roots[SRC_KEYS] = {};
            U32 source_prims[SRC_KEYS] = {};
            U32 source_same_roots = 0, source_same_prims = 0;
            U32 moving_roots = 0, settling_roots = 0, returnable_roots = 0, returnable_prims = 0, total_active = 0;
            U32 moved_root = 0, moved_children = 0, on_move_list = 0, moved_in_move = 0, wobble_only = 0, still_moved = 0;
            U32 spin_roots = 0, spin_prims = 0, spinning_prims = 0;
            U32 held_roots = 0, held_prims = 0, held_spinning = 0;
            sSpinNow = SpinNow();
            const F32 now = gFrameTimeSeconds;
            const S32 count = gObjectList.getNumObjects();
            for (S32 i = 0; i < count; ++i)
            {
                LLViewerObject* vobj = gObjectList.getObject(i);
                if (!vobj || vobj->isDead() || vobj->isAvatar() || vobj->mDrawable.isNull())
                {
                    continue;
                }
                LLDrawable* drawable = vobj->mDrawable;
                if (drawable->isDead() || !drawable->isActive())
                {
                    continue;
                }
                const U8 reason = llmin(drawable->mPlusActiveReason, (U8)(WHY_COUNT - 1));
                ++active_prims[reason];
                ++total_active;
                if (!drawable->isRoot() || vobj->getParent())
                {
                    continue;   // a child, or an attachment (its root is the avatar's): counted with the prims only
                }
                bool held = false;
                LinksetHashes hashes;
                linksetHashes(vobj, hashes);
                const bool had_hash = drawable->mPlusMatrixHash != 0;
                const bool root_changed = drawable->mPlusMatrixHash != hashes.root;
                const bool children_changed = drawable->mPlusChildHash != hashes.children;
                const bool listed = drawable->isState(LLDrawable::ON_MOVE_LIST);
                const S32 moves = drawable->getGeneration() - drawable->mPlusCensusGeneration;
                if (!root_changed && !children_changed && !listed)
                {
                    drawable->mPlusStillReports = (U8)llmin((U32)drawable->mPlusStillReports + 1, 255u);
                    if (drawable->mPlusRawHash != hashes.raw)
                    {
                        ++wobble_only;      // the low bits moved, nothing else (counted still)
                    }
                    if (moves > 0)
                    {
                        ++still_moved;      // moved by updateMove, to where it was: moves changing nothing
                    }
                }
                else
                {
                    drawable->mPlusStillReports = 0;
                    if (had_hash)
                    {
                        moved_root += root_changed ? 1 : 0;
                        moved_children += (!root_changed && children_changed) ? 1 : 0;
                        on_move_list += listed ? 1 : 0;
                        moved_in_move += moves > 0 ? 1 : 0;
                        if (samples.size() < SAMPLES_MAX)
                        {
                            samples.push_back({ vobj->getID(), reason, drawable->mPlusActiveSource, root_changed,
                                                children_changed, listed, moves, hashes.prims, hashes.spinning });
                        }
                    }
                }
                drawable->mPlusMatrixHash = hashes.root;
                drawable->mPlusChildHash = hashes.children;
                drawable->mPlusRawHash = hashes.raw;
                drawable->mPlusSpinHash = hashes.spin;
                drawable->mPlusCensusGeneration = drawable->getGeneration();
                ++roots[reason];
                root_prims[reason] += hashes.prims;
                if (drawable->mPlusActiveSource & SRC_IN_MOVE)
                {
                    const U32 key = sourceKey(drawable->mPlusActiveSource);
                    ++source_roots[key];
                    source_prims[key] += hashes.prims;
                    if (drawable->mPlusActiveSource & SRC_SAME)
                    {
                        ++source_same_roots;
                        source_same_prims += hashes.prims;
                    }
                }
                const bool root_spins = !zero(vobj->getAngularVelocity());
                if (hashes.spinning || root_spins)
                {
                    ++spin_roots;
                    spin_prims += hashes.prims;
                    spinning_prims += hashes.spinning + (root_spins ? 1 : 0);
                }
                if (drawable->mPlusStillReports >= STILL_REPORTS)
                {
                    ++still_roots[reason];
                    still_prims[reason] += hashes.prims;
                    if (returnable(vobj, false))
                    {
                        ++returnable_roots;
                        returnable_prims += hashes.prims;
                    }
                    else if (hashes.spinning && returnable(vobj, true))
                    {
                        ++held_roots;       // only its spinning children keep it active
                        held_prims += hashes.prims;
                        held_spinning += hashes.spinning;
                        held = true;
                        spinHeld(drawable, hashes.prims, hashes.spinning, now);
                    }
                }
                else if (drawable->mPlusStillReports == 0)
                {
                    ++moving_roots;
                }
                else
                {
                    ++settling_roots;
                }
                drawable->mPlusSpinHeld = held;     // research/186: its costs are timed until the next line
            }
            out << "; now active: prims " << total_active << " by reason";
            appendReasons(out, active_prims, nullptr);
            out << "; linksets (roots/prims)";
            appendReasons(out, roots, root_prims);
            out << "; linksets by the move they became active in (roots/prims)";
            bool any = false;
            for (U32 k = 0; k < SRC_KEYS; ++k)
            {
                if (source_roots[k])
                {
                    out << (any ? ", " : " ") << (k >= SRC_COUNT ? "child " : "") << sourceName(k) << " "
                        << source_roots[k] << "/" << source_prims[k];
                    any = true;
                }
            }
            out << (any ? "" : " none") << ", of those in a move changing nothing " << source_same_roots << "/"
                << source_same_prims;
            out << "; with a spin (roots/prims, spinning prims) " << spin_roots << "/" << spin_prims << ", " << spinning_prims;
            out << "; moving since the last line " << moving_roots << " (its own matrix " << moved_root
                << ", only a child's that does not spin " << moved_children << ", on the move list " << on_move_list
                << ", moved by updateMove " << moved_in_move << "), still 5 s " << settling_roots
                << ", low bits only (counted still) " << wobble_only << ", still but moved by updateMove " << still_moved
                << ", still 10 s or more (roots/prims, spinning children aside)";
            appendReasons(out, still_roots, still_prims);
            out << "; of those returnable (no spin, physics, selection, seat, attachment, flex, animesh) "
                << returnable_roots << "/" << returnable_prims << ", held only by spinning children " << held_roots
                << "/" << held_prims << " (spinning " << held_spinning << ")";
        }

        void appendVerify(std::ostringstream& out, VerifyKind kind, bool on)
        {
            const Verify& v = sVerify[kind];
            out << "; " << VERIFY_NAMES[kind] << " [" << (on ? "on" : "off") << "] checked " << v.checked
                << ", ok " << v.ok << ", root mismatch " << v.root_mismatch
                << ", child mismatch " << v.child_mismatch << ", active again " << v.active_again
                << ", gone " << v.gone << ", children " << v.children
                << " (rebuild still queued " << v.children_unbuilt << "), not checked (over " << VERIFY_MAX << ") "
                << v.dropped;
        }

        // research/186 step 1: the "spin census" line, after census() has
        // flagged the held linksets anew. The costs are of the ones flagged
        // at the line before (none on the first line).
        void reportSpin(U32 frames)
        {
            const SpinNow& s = sSpinNow;
            const SpinCost& c = sSpinCost;
            const F64 per_frame = frames ? 1.0 / frames : 0.0;
            auto us = [&](CostKind kind) { return (F64)c.ns[kind] * 1.e-3 * per_frame; };
            std::ostringstream out;
            out.setf(std::ios::fixed);
            out.precision(1);
            out << "spin census over " << frames << " frames: held only by spinning children now (roots/prims, spinning) "
                << s.roots << "/" << s.prims << ", " << s.spinning << ", their bridges' draw infos " << s.infos
                << "; the root's own moves changing something (not its child's): never " << s.never
                << ", once (as a rule its placing at the load) " << s.once << ", more, the last within "
                << (U32)SPIN_RECENT_SECONDS << " s " << s.recent << ", within " << (U32)SPIN_MINUTE_SECONDS << " s "
                << s.minute << ", longer ago " << s.older << " (the last one's source:";
            bool any = false;
            for (U32 k = 0; k < SRC_COUNT; ++k)
            {
                if (s.last_source[k])
                {
                    out << (any ? ", " : " ") << SRC_NAMES[k] << " " << s.last_source[k];
                    any = true;
                }
            }
            out << (any ? ")" : " none)");
            out << "; per frame, of the " << sSpinFlagged << "/" << sSpinFlaggedPrims
                << " flagged at the last line (us, calls):";
            F64 total = 0.0;
            for (U32 k = 0; k < COST_KINDS; ++k)
            {
                if (k == COST_MOVE || k == COST_CULL)
                {
                    continue;   // resolved to the kinds after them
                }
                const F64 t = us((CostKind)k);
                out << (k == COST_ROOT_MOVE ? " " : ", ") << COST_NAMES[k] << " " << t << " ("
                    << (F64)c.calls[k] * per_frame << ")";
                if (k == COST_SORT)
                {
                    out << " [LOD updates " << (F64)c.lod_updates * per_frame << ", prims "
                        << (F64)c.lod_prims * per_frame << "]";
                }
                total += k == COST_ROOT_OWN ? 0.0 : t;
            }
            out << "; total " << total << " us a frame (own moves aside), "
                << (sSpinFlagged ? total / sSpinFlagged : 0.0) << " us a linkset";
            out << "; not timed: the G-buffer's object matrix draws (draw infos of the sorted bridges a frame "
                << (F64)c.seen_infos * per_frame << "), the sun shadows' moving casters (bridge groups a frame "
                << (F64)c.shadow_groups * per_frame << ", draw infos x cascades " << (F64)c.shadow_infos * per_frame
                << ", before plus/shadowfar)";
            out << "; step 2 gate (" << (U32)SPIN_GATE_US << " us): "
                << (!sSpinFlagged ? "nothing flagged" : total >= SPIN_GATE_US ? "over" : "under");
            LL_INFOS("FrameCost") << out.str() << LL_ENDL;
            sSpinCost = SpinCost();
            sSpinFlagged = s.roots;
            sSpinFlaggedPrims = s.prims;
        }

        void report(bool with_census, U32 frames)
        {
            std::ostringstream out;
            std::vector<Sample> samples;
            out << "active census over " << frames << " frames: became active (roots/prims)";
            appendReasons(out, sActivated.roots, sActivated.prims);
            if (with_census)
            {
                out << "; moves by source (moves/changing nothing)";
                appendMoves(out);
                census(out, samples);
            }
            out << "; orphan reunions " << sReunions.reunions << ", unmoved static parents " << sReunions.eligible
                << ", kept static " << sReunions.kept << " [R1 " << (LLRHI::sActiveOrphan ? "on" : "off") << "], upstream's way:";
            for (U32 r = 0; r < REFUSE_COUNT; ++r)
            {
                out << (r ? ", " : " ") << REFUSAL_NAMES[r] << " " << sReunions.refused[r];
            }
            const Verify& orphan = sVerify[VERIFY_ORPHAN];
            if (orphan.checked || orphan.dropped || LLRHI::sActiveOrphanVerify)
            {
                appendVerify(out, VERIFY_ORPHAN, LLRHI::sActiveOrphanVerify);
            }
            if (LLRHI::sMoveSame || sSame.kept || sSame.checked || sVerify[VERIFY_SAME].checked)
            {
                out << "; move same [" << (LLRHI::sMoveSame ? "on" : "off") << "] kept static " << sSame.kept
                    << ", changing nothing but upstream's way: source " << sSame.refused_source << ", kind "
                    << sSame.refused_kind << ", parent " << sSame.refused_parent << "; move same verify ["
                    << (LLRHI::sMoveSameVerify ? "on" : "off") << "] checked " << sSame.checked << ", ok " << sSame.ok
                    << ", mismatch " << sSame.mismatch << ", made active anyway " << sSame.active_after;
                appendVerify(out, VERIFY_SAME, LLRHI::sMoveSameVerify);
            }
            LL_INFOS("FrameCost") << out.str() << LL_ENDL;
            if (!samples.empty())
            {
                std::ostringstream line;
                line << "active census moving linksets (up to " << SAMPLES_MAX << "):";
                for (const Sample& sample : samples)
                {
                    line << " " << sample.id << " " << WHY_NAMES[sample.reason] << " [";
                    appendSourceName(line, sample.source);
                    line << "] changed:" << (sample.root ? " root" : "") << (sample.children ? " child" : "")
                         << (sample.on_move_list ? " on the move list" : "") << ", root moves " << sample.moves
                         << ", prims " << sample.prims << ", spinning children " << sample.spinning << ";";
                }
                LL_INFOS("FrameCost") << line.str() << LL_ENDL;
            }
            if (with_census)
            {
                reportSpin(frames);
            }
            sActivated = Activated();
            sReunions = Reunions();
            for (Verify& v : sVerify)
            {
                v = Verify();
            }
            sMoves = Moves();
            sSame = Same();
        }

        // The linkset's root (an attachment's own root, not the avatar).
        LLViewerObject* linksetRoot(LLViewerObject* vobj)
        {
            LLViewerObject* root = vobj;
            for (LLViewerObject* parent = (LLViewerObject*)root->getParent(); parent && !parent->isAvatar();
                 parent = (LLViewerObject*)root->getParent())
            {
                root = parent;
            }
            return root;
        }

        void queueVerify(const LLUUID& id, VerifyKind kind)
        {
            if (!sPending.empty() && sPending.back().id == id && sPending.back().kind == kind)
            {
                return;     // one check for a linkset's prims in one go
            }
            if (sPending.size() < VERIFY_MAX)
            {
                sPending.push_back({ id, LLFrameTimer::getFrameCount() + VERIFY_AFTER_FRAMES, kind });
                sVerifyPending = true;
            }
            else
            {
                ++sVerify[kind].dropped;
            }
        }

        // What upstream's updateXform would set: position (the agent's for a
        // root, the parent's for a child), rotation, scale. True when all
        // three are already the drawable's, bit for bit (the move changes nothing).
        bool sameAsObject(LLDrawable* drawable, const LLViewerObject* vobj, bool& position, bool& rotation, bool& scale)
        {
            const LLVector3 target = drawable->mXform.isRoot() ? vobj->getPositionAgent() : vobj->getPosition();
            position = drawable->mXform.getPosition() != target;
            rotation = drawable->mXform.getRotation() != vobj->getRotation();
            scale = drawable->getScale() != vobj->getScale();
            return !position && !rotation && !scale;
        }

        bool sameSource(U8 source)
        {
            switch (source & SRC_MASK)
            {
            case SRC_FULL:
            case SRC_TERSE:
            case SRC_COMPRESSED:
            case SRC_CACHED:
            case SRC_IDLE_SPIN:     // (a parent put on the list by its spinning child; the child itself changes)
            case SRC_IDLE_MOTION:
            case SRC_IDLE_OTHER:
                return true;
            default:
                return false;       // a link change, an orphan reunion, an edit, an attachment, unknown
            }
        }

        // research/180 10.15: what a static drawable kept static may not be.
        bool sameKind(LLDrawable* drawable, LLViewerObject* vobj)
        {
            return drawable->getVOVolume() && vobj->getPCode() == LL_PCODE_VOLUME && !vobj->isAvatar() &&
                   !vobj->isAttachment() && !vobj->isHUDAttachment() && !vobj->isAnimatedObject() &&
                   !vobj->isFlexible() && !selected(vobj) && zero(vobj->getVelocity()) &&
                   zero(vobj->getAcceleration()) && zero(vobj->getAngularVelocity()) && !vobj->flagUsePhysics() &&
                   !vobj->isChanged(LLXform::SHIFTED) && !drawable->isState(LLDrawable::ANIMATED_CHILD);
        }

        // A root's xform has no parent; a child's is its static parent's.
        bool sameParent(LLDrawable* drawable)
        {
            LLDrawable* parent = drawable->getParent();
            if (!parent)
            {
                return drawable->mXform.getParent() == nullptr;
            }
            return !parent->isDead() && !parent->isAvatar() && !parent->isActive() &&
                   drawable->mXform.getParent() == &parent->mXform;
        }

        // Right after a move kept static: the drawable where its object is.
        void verifyKept(LLDrawable* drawable)
        {
            LLViewerObject* vobj = drawable->getVObj();
            if (!vobj || drawable->isDead())
            {
                return;
            }
            ++sSame.checked;
            if (drawable->isActive())
            {
                ++sSame.active_after;
                return;
            }
            LLVector3 want_position = vobj->getPositionAgent();
            LLQuaternion want_rotation = vobj->getRotation();
            if (drawable->getParent())
            {
                const LLViewerObject* parent = (const LLViewerObject*)vobj->getParent();
                if (!parent)
                {
                    ++sSame.mismatch;
                    return;
                }
                want_position = parent->getPositionAgent() + vobj->getPosition() * parent->getRotation();
                want_rotation = vobj->getRotation() * parent->getRotation();
            }
            if (samePlace(drawable->mXform.getWorldPosition(), drawable->mXform.getWorldRotation(), want_position, want_rotation))
            {
                ++sSame.ok;
            }
            else
            {
                ++sSame.mismatch;
            }
            queueVerify(linksetRoot(vobj)->getID(), VERIFY_SAME);
        }

        // Who is putting the drawable on the move list now.
        U8 sourceNow(LLDrawable* drawable)
        {
            const U8 via = sViaChild ? SRC_VIA_CHILD : 0;
            if (drawable->mPlusMoveReason == WHY_ORPHAN)
            {
                return SRC_ORPHAN | via;
            }
            const LLViewerObject* vobj = drawable->getVObj();
            if (!vobj)
            {
                return SRC_OTHER | via;
            }
            if (vobj->isAttachment())
            {
                return SRC_ATTACHMENT | via;
            }
            if (selected(vobj))
            {
                return SRC_EDIT | via;
            }
            switch (sMoveSource)
            {
            case SRC_FULL:
            case SRC_TERSE:
            case SRC_COMPRESSED:
            case SRC_CACHED:
                // an update moving another object, not through markMoved's
                // recursion: its new or old parent (a link change)
                return (U8)((!via && sMoveObject && sMoveObject != vobj) ? SRC_UPDATE_PARENT : sMoveSource) | via;
            case SRC_IDLE:
            {
                // the object idling (for a parent put on the list by it: its child's motion)
                const LLViewerObject* idle = sMoveObject ? sMoveObject : vobj;
                if (!zero(idle->getAngularVelocity()))
                {
                    return SRC_IDLE_SPIN | via;
                }
                if (!zero(idle->getVelocity()) || !zero(idle->getAcceleration()))
                {
                    return SRC_IDLE_MOTION | via;
                }
                return SRC_IDLE_OTHER | via;
            }
            case SRC_BEFORE:
                return SRC_OTHER | via;
            default:
                return (U8)sMoveSource | via;
            }
        }
    }

    MoveSource updateSource(U32 update_type)
    {
        switch (update_type)
        {
        case OUT_FULL:            return SRC_FULL;
        case OUT_TERSE_IMPROVED:  return SRC_TERSE;
        case OUT_FULL_COMPRESSED: return SRC_COMPRESSED;
        case OUT_FULL_CACHED:     return SRC_CACHED;
        default:                  return SRC_OTHER;
        }
    }

    void MoveScope::push(MoveSource source, const LLViewerObject* vobj)
    {
        mPreviousSource = sMoveSource;
        mPreviousObject = sMoveObject;
        sMoveSource = source;
        sMoveObject = vobj;
    }

    void MoveScope::pop()
    {
        sMoveSource = mPreviousSource;
        sMoveObject = mPreviousObject;
    }

    void ViaChild::enter()
    {
        ++sViaChild;
    }

    void ViaChild::leave()
    {
        llassert(sViaChild > 0);
        if (sViaChild)
        {
            --sViaChild;
        }
    }

    void markedSlow(LLDrawable* drawable)
    {
        drawable->mPlusMoveSource = sourceNow(drawable);   // (SRC_COUNTED clear: its next updateMove is counted)
    }

    void MoveCheck::begin(LLDrawable* drawable)
    {
        mDrawable = drawable;
        mPreviousMove = sMoveNow;
        LLViewerObject* vobj = drawable->getVObj();
        if (!vobj || vobj->isAvatar())
        {
            sMoveNow = 0;   // (avatars move every frame: not what the census asks)
            return;
        }
        const U8 source = drawable->mPlusMoveSource & (SRC_MASK | SRC_VIA_CHILD);
        bool position = false, rotation = false, scale = false;
        const bool same = sameAsObject(drawable, vobj, position, rotation, scale);
        const bool was_static = !drawable->isState(LLDrawable::ACTIVE);
        sMoveNow = source | SRC_IN_MOVE | (same ? SRC_SAME : 0);
        if (censusOn() && !(drawable->mPlusMoveSource & SRC_COUNTED))
        {
            drawable->mPlusMoveSource |= SRC_COUNTED;   // the first updateMove since it went on the list
            const U32 key = sourceKey(source);
            ++sMoves.moves[key];
            sMoves.same[key] += same ? 1 : 0;
            sMoves.position[key] += position ? 1 : 0;
            sMoves.rotation[key] += rotation ? 1 : 0;
            sMoves.scale[key] += scale ? 1 : 0;
            sMoves.static_moves[key] += was_static ? 1 : 0;
            sMoves.static_same[key] += (was_static && same) ? 1 : 0;
            // research/186: a root moved for its own reason (not put on the list by its child)
            if (!same && !(source & SRC_VIA_CHILD) && drawable->isRoot() && !vobj->getParent())
            {
                drawable->mPlusOwnMoves = (U8)llmin((U32)drawable->mPlusOwnMoves + 1, 255u);
                drawable->mPlusOwnMoveSource = source & SRC_MASK;
                drawable->mPlusOwnMoveAt = gFrameTimeSeconds;
            }
        }
        if (!LLRHI::sMoveSame || !same || !was_static)
        {
            return;
        }
        if (!sameSource(source))
        {
            ++sSame.refused_source;
        }
        else if (!sameKind(drawable, vobj))
        {
            ++sSame.refused_kind;
        }
        else if (!sameParent(drawable))
        {
            ++sSame.refused_parent;
        }
        else
        {
            mKeep = true;
            ++sSame.kept;
        }
    }

    void MoveCheck::end()
    {
        if (mKeep && LLRHI::sMoveSameVerify)
        {
            verifyKept(mDrawable);
        }
        sMoveNow = mPreviousMove;
        mDrawable = nullptr;
    }

    void Activation::begin(LLDrawable* drawable)
    {
        if (sDepth++ == 0)
        {
            sCurrent = classify(drawable);
        }
        const LLViewerObject* vobj = drawable->getVObj();
        if (!vobj || vobj->isAvatar())
        {
            return;
        }
        drawable->mPlusActiveReason = sCurrent;
        drawable->mPlusActiveSource = sMoveNow;
        drawable->mPlusStillReports = 0;
        drawable->mPlusMatrixHash = 0;
        if (drawable->isRoot())
        {
            ++sActivated.roots[sCurrent];
        }
        ++sActivated.prims[sCurrent];
        if (sMoveNow & SRC_IN_MOVE)
        {
            const U32 key = sourceKey(sMoveNow);
            sMoves.act_roots[key] += drawable->isRoot() ? 1 : 0;
            ++sMoves.act_prims[key];
            sMoves.act_same_prims[key] += (sMoveNow & SRC_SAME) ? 1 : 0;
        }
    }

    void Activation::end()
    {
        llassert(sDepth > 0);
        if (sDepth)
        {
            --sDepth;
        }
    }

    Reason ReasonScope::push(Reason reason)
    {
        const Reason previous = sScope;
        sScope = reason;
        return previous;
    }

    void CostScope::begin(LLDrawable* drawable, CostKind kind)
    {
        if (sCostDepth || !drawable || drawable->isDead())
        {
            return;     // outermost only
        }
        LLDrawable* root = flaggedRoot(drawable);
        if (!root || !on_main_thread())
        {
            return;
        }
        switch (kind)
        {
        case COST_MOVE:
            kind = drawable->isSpatialBridge() ? COST_BRIDGE_MOVE
                 : (drawable->mPlusMoveSource & SRC_VIA_CHILD) ? COST_ROOT_MOVE : COST_ROOT_OWN;
            break;
        case COST_CULL:
            if (!drawable->isSpatialBridge())
            {
                return;     // the root itself: culled with its bridge
            }
            kind = LLPipeline::sShadowRender ? COST_CULL_SHADOW
                 : (LLViewerCamera::sCurCameraID == LLViewerCamera::CAMERA_WORLD && !gCubeSnapshot) ? COST_CULL_MAIN
                 : COST_CULL_OTHER;
            break;
        case COST_SORT:
            sSpinCost.seen_infos += root->mPlusSpinInfos;
            break;
        default:
            break;
        }
        ++sCostDepth;
        mTiming = true;
        mKind = kind;
        mStart = nowNanoseconds();
    }

    void CostScope::end()
    {
        const S64 elapsed = nowNanoseconds() - mStart;
        sSpinCost.ns[mKind] += (U64)llmax(elapsed, (S64)0);
        ++sSpinCost.calls[mKind];
        llassert(sCostDepth > 0);
        if (sCostDepth)
        {
            --sCostDepth;
        }
    }

    void lodUpdatedSlow(LLDrawable* bridge)
    {
        LLDrawable* root = bridge ? flaggedRoot(bridge) : nullptr;
        if (!root || !on_main_thread())
        {
            return;
        }
        ++sSpinCost.lod_updates;
        const LLViewerObject* vobj = root->getVObj();
        sSpinCost.lod_prims += 1 + (vobj ? (U32)vobj->getChildren().size() : 0);
    }

    void shadowGroupsSlow(const std::vector<LLSpatialGroup*>& groups)
    {
        if (!sSpinFlagged)
        {
            return;     // nothing flagged
        }
        for (LLSpatialGroup* group : groups)
        {
            if (!group || group->isDead())
            {
                continue;
            }
            LLSpatialPartition* part = group->getSpatialPartition();
            LLSpatialBridge* bridge = part ? part->asBridge() : nullptr;
            if (!bridge || !flaggedRoot(bridge))
            {
                continue;
            }
            U32 cascades = 0;
            for (U32 mask = group->mRHIShadowMask; mask; mask &= mask - 1)
            {
                ++cascades;
            }
            if (!cascades)
            {
                continue;
            }
            ++sSpinCost.shadow_groups;
            U32 infos = 0;
            for (const auto& type : group->mDrawMap)
            {
                infos += (U32)type.second.size();
            }
            sSpinCost.shadow_infos += infos * cascades;
        }
    }

    bool reunionKeepsStatic(LLViewerObject* parent, LLViewerObject* child)
    {
        const bool census = censusOn();
        if (!census && !LLRHI::sActiveOrphan)
        {
            return false;
        }
        LLDrawable* drawable = parent->mDrawable;
        if (!drawable || drawable->isDead() || !child)
        {
            return false;   // upstream's markMoved does nothing either
        }
        ++sReunions.reunions;
        bool eligible = false;
        const Refusal why = refusal(parent, child, drawable, eligible);
        if (eligible)
        {
            ++sReunions.eligible;
            if (LLRHI::sActiveOrphan)
            {
                ++sReunions.kept;
                if (LLRHI::sActiveOrphanVerify)
                {
                    queueVerify(parent->getID(), VERIFY_ORPHAN);  // (one check for a parent's children reunited in one go)
                }
                return true;
            }
        }
        else
        {
            ++sReunions.refused[why];
        }
        if (census && !drawable->isActive())
        {
            drawable->mPlusMoveReason = WHY_ORPHAN;  // its move (updateMove) makes it active: the census names why
        }
        return false;
    }

    void frameSlow()
    {
        static F32 logged_at = 0.f;
        static U32 frames = 0;
        if (sVerifyPending)
        {
            verifyDue();
        }
        ++frames;
        if (gFrameTimeSeconds - logged_at < REPORT_SECONDS)
        {
            return;
        }
        logged_at = gFrameTimeSeconds;
        const bool census = censusOn();
        const Verify& orphan = sVerify[VERIFY_ORPHAN];
        if (census || sReunions.reunions || orphan.checked || orphan.dropped || LLRHI::sMoveSame || sSame.kept ||
            sVerify[VERIFY_SAME].checked)
        {
            report(census, frames);
        }
        frames = 0;
    }
}
