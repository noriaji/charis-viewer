/**
 * @file hairsway.cpp
 * @brief "Hair sway": the agent's rigged hair lags behind the head's motion.
 *        Registers with the plus foundation (research/hair-sway.md).
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

// The agent's own avatar only. Which of its attachments are hair is judged
// by their shape and textures, or set by hand in their context menu
// (hairdetect.cpp, research/hair-detect.md); names are not used. An
// attachment judged hair starts swaying from nothing over FADE_SECONDS.
// Off by default (PlusHairSway).
//
// How it moves
// ------------
// Once a frame (idle, main thread) two points hang below the pivot (a little
// above mHead), a little behind it, as a chain: the middle of the hair
// (RAMP / 2 below the pivot) follows its anchor on the head through a damped
// spring, the tips (RAMP below) follow the middle through a slower one, so
// they come after it. The springs are loose and damped about critically: the
// damping (a dashpot to the head) carries the hair off quickly when the head
// starts or stops, the loose spring brings it back slowly. How much they ring
// is PlusHairSwayBounce (0: they settle without swinging back, default 0.2:
// one small, slow swing back, 0.5: a few, 1: keeps ringing), mapped to the
// springs' damping ratios. How far
// each is from where it would rest, forward swings weakened, times RESPONSE
// and PlusHairSwayStrength, softly limited (tanh) to PlusHairSwayMaxOffset
// sideways / front-back and MAX_BOUNCE up/down, is the middle's and the tips'
// offset. SL avatars start
// and stop walking far faster than a person: the springs' own swing at a walk
// start is some 60 cm at the tips, so RESPONSE scales it to what hair would do
// and the soft limit keeps the largest moves (flying, sudden stops) from
// hitting a wall and bouncing off it.
//
// Wind (PlusHairSwayWind: 0 off, 1 light (default), 2 medium, 3 strong):
// a push on both springs, horizontal in the world, so turning changes the
// side it comes from, and the tips (carried by the middle and pushed
// themselves) stream further than the middle. Its direction is the region's
// wind (gWindVec, smoothed over REGION_WIND_TIME) when there is some, else a
// made-up one, wandering slowly either way; its strength a level times a sum
// of slow waves of different periods plus an occasional gust (from a hash of
// the time, the same every run), times how windy the region is. It goes
// through RESPONSE, Strength, the forward weakening and the soft limit like
// any other motion, so bangs are not blown into the eyes. Off: no push, the
// motion is as before. tools/hair-sway-sim/wind.py has the shown tip offsets
// of the four levels (light: about 0.6 cm on average, at most about 2 cm).
//
// How it is drawn
// ---------------
// Hair on the scalp lies above the hairline; what hangs below it (bangs over
// the forehead, the sides of a bob, long hair) is free. The hairline is taken
// as a plane through the top of the forehead and the nape, level from side to
// side (HAIRLINE_FRONT / HAIRLINE_BACK, in the head frame of the default
// skeleton, scaled with the head). A hair vertex (after skinning) d below it
// along the head's up axis moves by
//     mid * smoothstep(DEAD, MID_DEPTH, d) + (tips - mid) * smoothstep(MID_DEPTH, TIP_DEPTH, d)
// i.e. not at all above the plane or less than DEAD below it (the crown, the
// parting, the hairline and all the hair on the scalp stay where the head
// puts them), by the middle's offset from MID_DEPTH down (bangs at the brows,
// a bob at the jaw) and by the tips' offset from TIP_DEPTH down (long hair
// moves no further). The part of that offset pointing into the head (from its
// centre) is cut to INWARD_KEEP, so bangs and the sides do not sink into the
// face and head. tools/hair-sway-sim/weights.py lists the weights over
// typical styles (short, bob, bangs, long). Very short hair that ends above
// the hairline does not sway, as it does not in life. The skinning shaders do it:
// the hook in LLVOAvatar::updateSkinInfoMatrixPalette appends two entries to
// the hair palette with the hairline plane, the head's centre, the depths and
// the offsets (the joints themselves are not changed), and
//  - objectSkinV.glsl (GL and the translated Vulkan shaders) reads them at
//    the index the uniform plusHairSway gives (set with every palette upload
//    by the hook in lldrawpool.cpp, 0 for any other palette);
//  - the native skinned shadow shaders (llvulkan/llvknative.cpp) find them
//    by SWAY_MARK at the end of the packet's palette.
// Both check SWAY_MARK before they move anything. With the feature off, no
// palette has the entries and the shaders take their old path.
//
// Non-rigged hair (a mesh only attached to the head, no skin): drawn by
// the shaders of every other object, with the attachment's matrix, so the
// shaders cannot tell it apart. Its vertex buffer is moved instead: once a
// frame (idle) the positions of its faces are written as built plus the same
// offset the skinning shaders give (hairOffset, the shaders' twin), and sent
// with the buffer's usual upload (LLVertexBuffer keeps a copy of its data in
// memory, mapped regions are flushed with glBufferSubData). Every pass that
// draws the buffer (the G buffer, alpha and OIT, the native shadow packets)
// draws the moved hair; no shader, vertex format or descriptor changes.
//  - Which: faces of non-rigged, non-flexible volumes of the agent's
//    attachments worn on a head joint (mHead, the joints under it or mNeck)
//    that are hair (hairdetect.cpp, or set by hand). An attachment worn
//    elsewhere moves with that joint, not the head: its non-rigged prims (a
//    hair clip on rigged hair worn on the chest) stay as they are, set by
//    hand or not; its rigged ones sway. Not: HUDs, flexible prims (rebuilt
//    every frame by their own motion), and buffers without a copy in memory.
//  - Rebuilds: a face that was rebuilt (LOD change, a new buffer, its range
//    moved, or its positions no longer the ones last written here) is taken
//    as built again; anything that does not add up (no copy, a range past the
//    buffer, a matrix that is not finite) leaves the face alone, as drawn
//    upstream.
//  - Off (or a face no longer hair): the positions as built are written back.
//  - Cost: hair over NONRIGGED_FULL_RATE_VERTICES vertices is written every
//    other frame, over NONRIGGED_MAX_VERTICES not at all.
//  - Shadows: attachments live in spatial bridges, which the shadow cache
//    always draws anew (research/19), so their shadows follow; the GPU pool
//    (research/46 G1) drops buffers rewritten this often by itself, only the
//    hair's.
//
// The palettes are cached per LLMeshSkinInfo::mHash and avatar, so a skin
// shared with another attachment of the agent (the same hash) would move that
// attachment too: such skins are left out.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "hairsway.h"
#include "hairdetect.h"
#include "plus/freeze/freeze.h"

#include "llappviewer.h"  // gWindVec
#include "llcallbacklist.h"
#include "lldrawable.h"
#include "llface.h"
#include "llvertexbuffer.h"
#include "llcontrol.h"
#include "llevents.h"
#include "llframetimer.h"
#include "llglslshader.h"
#include "llinventorymodel.h"
#include "llmatrix4a.h"
#include "llmodel.h"
#include "llskinningutil.h"
#include "llthread.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llvoavatarself.h"
#include "llvovolume.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <mutex>
#include <vector>

namespace
{
    constexpr char SETTING_ENABLED[]    = "PlusHairSway";
    constexpr char SETTING_STRENGTH[]   = "PlusHairSwayStrength";
    constexpr char SETTING_MAX_OFFSET[] = "PlusHairSwayMaxOffset";
    constexpr char SETTING_BOUNCE[]     = "PlusHairSwayBounce";
    constexpr char SETTING_WIND[]       = "PlusHairSwayWind";

    // Where the springs hang: RAMP / 2 and RAMP below the pivot, HANG_BACK
    // behind it (most hair is behind the head). Only the motion's source;
    // what moves how much is the hairline below.
    constexpr F32 PIVOT_UP  = 0.02f;             // m above mHead
    constexpr F32 RAMP      = 0.30f;             // m
    constexpr F32 HANG_BACK = 0.08f;             // m

    // The head in mHead's frame (x forward, z up; meters on the default
    // skeleton, whose eyes are at (0.098, +-0.036, 0.079) and mSkull at
    // (0, 0, 0.079)), scaled by the head's size (mHead to mSkull against
    // DEFAULT_SKULL). The hairline plane runs through the top of the forehead
    // and the nape, level from side to side; tools/hair-sway-sim/weights.py
    // has the same values.
    constexpr F32 DEFAULT_SKULL    = 0.079f;
    constexpr F32 HAIRLINE_FRONT_X = 0.09f, HAIRLINE_FRONT_Z = 0.15f;
    constexpr F32 HAIRLINE_BACK_X  = -0.09f, HAIRLINE_BACK_Z = -0.01f;
    constexpr F32 CENTRE_X         = -0.005f, CENTRE_Z = 0.08f;
    constexpr F32 DEAD             = 0.01f;      // depth below the hairline (along up) that never moves
    constexpr F32 MID_DEPTH        = 0.07f;      // the middle's offset from this depth
    constexpr F32 TIP_DEPTH        = 0.25f;      // the tips' offset from this depth
    constexpr F32 INWARD_KEEP      = 0.3f;       // part of an offset into the head (from its centre) kept

    // The springs: the tips slower than the middle (longer hair lags more).
    // Loose (1.0 / 0.75 Hz) and damped about critically. A stiff spring that
    // swings out as quickly (fix 3: 1.6 / 1.15 Hz) also swings back as
    // quickly, and with less damping back and forth; here the first swing
    // comes from the damping, its speed set by it, and the return from the
    // loose spring, slow. Damping ratios against the motion relative to the
    // anchor, from PlusHairSwayBounce b: still * (bouncy / still)^b, so 0
    // gives 1.2 / 1.15 (no swing back), the default 0.2 0.88 / 0.83 (the first
    // swing as fix 3's "lively", then one slow swing back of about a tenth),
    // 0.5 0.55 / 0.50 (a few), 1 0.25 / 0.22 (keeps ringing, slowly). Worked
    // out with tools/hair-sway-sim/sway4.py (walk start and stop at 3.2 m/s, a
    // 180 degree turn in 0.5 s; research/hair-sway.md, fix 5).
    constexpr F32 MID_HZ             = 1.0f;
    constexpr F32 TIP_HZ             = 0.75f;
    constexpr F32 MID_DAMPING_STILL  = 1.2f;
    constexpr F32 MID_DAMPING_BOUNCY = 0.25f;
    constexpr F32 TIP_DAMPING_STILL  = 1.15f;
    constexpr F32 TIP_DAMPING_BOUNCY = 0.22f;
    constexpr F32 AIR_DRAG           = 0.17f;    // 1/s against the motion in the world: about 2 cm of trailing at a walk
    constexpr F32 RESPONSE           = 0.35f;    // the shown offset per meter of the springs' offset, at strength 1

    // Wind (tools/hair-sway-sim/wind.py has the same values). The push in
    // m/s^2 of each level at strength 1: light keeps the tips moving by about
    // 0.6 cm on average when standing still, medium 1.2 cm, strong 2.2 cm.
    struct Wave
    {
        F32 amplitude;
        F32 period;     // s
        F32 phase;      // rad
    };
    constexpr F32  WIND_LEVELS[]          = { 0.f, 0.25f, 0.5f, 0.9f };
    constexpr S32  WIND_LEVEL_COUNT       = 4;
    constexpr F32  PSEUDO_WIND_DIRECTION  = 0.7f;   // rad in the agent frame (from +x toward +y), where the made-up wind blows
    constexpr Wave WIND_DIRECTION_WAVES[] = { { 0.5f, 41.f, 0.f }, { 0.25f, 11.7f, 1.3f }, { 0.2f, 3.7f, 0.4f }, { 0.15f, 1.7f, 2.6f } };  // rad
    constexpr F32  WIND_STRENGTH_BASE     = 0.7f;
    constexpr F32  WIND_STRENGTH_FLOOR    = 0.1f;   // the waves never turn it round
    constexpr Wave WIND_STRENGTH_WAVES[]  = { { 0.2f, 6.9f, 0.f }, { 0.25f, 2.3f, 0.7f }, { 0.25f, 1.1f, 2.1f } };
    constexpr F32  GUST_INTERVAL          = 9.f;    // s: at most one gust in each
    constexpr F32  GUST_TIME              = 2.f;    // s
    constexpr F32  GUST_SKIP              = 0.35f;  // part of the intervals without one
    constexpr F32  GUST_LOW               = 0.5f;   // added to the strength at the gust's height
    constexpr F32  GUST_HIGH              = 1.1f;
    constexpr F32  REGION_WIND_TIME       = 5.f;    // s, smoothing of the region's wind
    constexpr F32  REGION_WIND_MIN        = 0.5f;   // m/s: about here the region's wind takes over from the made-up one
    constexpr F32  REGION_WIND_REF        = 5.f;    // m/s: the region's wind at which the level is as set
    constexpr F32  REGION_WIND_LOW        = 0.6f;   // its factor on the strength, at least / at most
    constexpr F32  REGION_WIND_HIGH       = 1.4f;

    // Non-rigged hair: vertices written every frame up to this many, every
    // other frame up to the next, not at all beyond (a few hundred thousand
    // vertices would cost milliseconds and megabytes of upload a frame).
    constexpr U32 NONRIGGED_FULL_RATE_VERTICES = 80000;
    constexpr U32 NONRIGGED_MAX_VERTICES       = 240000;
    constexpr F32 NONRIGGED_STILL              = 1e-5f;  // m: offsets below are no motion

    constexpr F32 FORWARD_KEEP = 0.35f;          // part of a forward swing kept (into the neck and face)
    constexpr F32 MID_SHARE    = 0.6f;           // the middle's cap, part of PlusHairSwayMaxOffset
    constexpr F32 MAX_BOUNCE   = 0.03f;          // m up or down at the tips (half that at the middle), soft limit

    constexpr F32 MAX_STEP       = 1.f / 120.f;  // s, integration step
    constexpr S32 MAX_STEPS      = 30;
    constexpr F32 RESET_DT       = 0.25f;        // s: a longer frame restarts the springs at rest
    constexpr F32 RESET_JUMP     = 2.5f;         // m the anchor moves in a frame: teleport, region crossing, sit
    constexpr F32 RESCAN_SECONDS = 1.0f;         // attachments are looked at again this often
    constexpr F32 FADE_SECONDS   = 1.0f;         // an attachment newly found to be hair sways fully after this

    // ---- shared with adjustPalette (any thread) ----

    std::atomic<bool>              sActive{ false };
    std::atomic<bool>              sEverActive{ false };   // bindShader keeps resetting shaders once set
    std::atomic<const LLVOAvatar*> sAvatar{ nullptr };     // compared only, never dereferenced
    std::mutex                     sMutex;
    struct HairSkin
    {
        U64 hash;   // LLMeshSkinInfo::mHash
        F32 fade;   // 0 when found to be hair, 1 after FADE_SECONDS
    };
    std::vector<HairSkin>          sHairSkins;             // sorted by hash; under sMutex
    LLVector3                      sMidOffset;             // agent frame; under sMutex
    LLVector3                      sTipOffset;

    // ---- main thread ----

    struct Point
    {
        LLVector3 pos;
        LLVector3 vel;
    };

    struct Chain
    {
        bool      valid = false;
        LLVector3 mid_rest;     // the middle's anchor on the head, last frame
        LLVector3 tip_rest;     // where the tips rest when the middle rests, last frame
        LLVector3 tip_anchor;   // the tips' anchor (follows the middle), last frame
        Point     mid;
        Point     tip;
    };
    Chain        sChain;
    LLVector3    sRegionWind;   // the region's wind at the agent, smoothed
    LLFrameTimer sRescanTimer;
    bool         sScanned = false;

    struct HeadFrame
    {
        LLVector3 base;     // mHead
        LLVector3 pivot;    // where the springs hang from
        LLVector3 up;       // head's up axis, unit
        LLVector3 forward;  // head's forward axis, unit
        F32       scale;    // the head's size against the default skeleton's
    };

    LLVector3 rowXYZ(const LLMatrix4a& m, int row)
    {
        const F32* v = m.mMatrix[row].getF32ptr();
        return LLVector3(v[0], v[1], v[2]);
    }

    // The head's frame from mHead's world matrix, as the skinning palette
    // reads the joints (LLSkinningUtil::initSkinningMatrixPalette).
    bool headFrame(LLVOAvatar* avatar, HeadFrame& out)
    {
        LLJoint* head = avatar->mHeadp;
        if (!head)
        {
            return false;
        }
        const LLMatrix4a& m = head->getWorldMatrix4a();
        LLVector3 forward = rowXYZ(m, 0);
        LLVector3 up      = rowXYZ(m, 2);
        if (up.normalize() < 1e-6f || forward.normalize() < 1e-6f)
        {
            return false;
        }
        out.base    = rowXYZ(m, 3);
        out.up      = up;
        out.forward = forward;
        out.pivot   = out.base + up * PIVOT_UP;
        out.scale   = 1.f;
        if (avatar->mSkullp)
        {
            const F32 skull = dist_vec(rowXYZ(avatar->mSkullp->getWorldMatrix4a(), 3), out.base);
            out.scale = llclamp(skull / DEFAULT_SKULL, 0.6f, 1.6f);
        }
        return out.base.isFinite() && out.up.isFinite() && out.forward.isFinite() && std::isfinite(out.scale);
    }

    void addSkin(LLViewerObject* object, std::vector<U64>& skins)
    {
        LLVOVolume* volume = dynamic_cast<LLVOVolume*>(object);
        if (!volume || volume->isDead() || !volume->isRiggedMesh())
        {
            return;
        }
        const LLMeshSkinInfo* skin = volume->getSkinInfo();
        if (skin && skin->mHash)
        {
            skins.push_back(skin->mHash);
        }
    }

    // The skins (LLMeshSkinInfo::mHash) of the agent's rigged hair: rigged
    // prims of attachments that are hair (hairdetect.cpp), less any skin that a prim of
    // another attachment also uses (its palette is the same cache entry).
    std::vector<U64> scanHairSkins(LLVOAvatarSelf* self, std::string& names)
    {
        std::vector<U64> hair, other;
        for (const auto& point : self->mAttachmentPoints)
        {
            const LLViewerJointAttachment* attachment = point.second;
            if (!attachment || attachment->getIsHUDAttachment())
            {
                continue;
            }
            for (const LLPointer<LLViewerObject>& object : attachment->mAttachedObjects)
            {
                if (object.isNull() || object->isDead())
                {
                    continue;
                }
                const LLViewerInventoryItem* item = gInventory.getItem(object->getAttachmentItemID());
                const bool is_hair = plus::hairsway::detect::sways(object.get(), attachment, self);
                std::vector<U64>& skins = is_hair ? hair : other;
                const size_t before = skins.size();
                addSkin(object.get(), skins);
                for (LLViewerObject* child : object->getChildren())
                {
                    addSkin(child, skins);
                }
                if (is_hair && skins.size() > before)
                {
                    names += names.empty() ? "" : ", ";
                    names += item ? item->getName() : object->getAttachmentItemID().asString();
                }
            }
        }

        std::sort(hair.begin(), hair.end());
        hair.erase(std::unique(hair.begin(), hair.end()), hair.end());
        std::sort(other.begin(), other.end());
        other.erase(std::unique(other.begin(), other.end()), other.end());

        std::vector<U64> result;
        std::set_difference(hair.begin(), hair.end(), other.begin(), other.end(), std::back_inserter(result));
        if (result.size() < hair.size())
        {
            names += " (" + std::to_string(hair.size() - result.size()) + " skin(s) shared with other attachments left out)";
        }
        return result;
    }

    // The hairline plane, the head's centre and the depths for this frame's
    // head, in the agent frame (what the shaders get in the palette entries).
    struct HairShape
    {
        LLVector3 plane_n;      // depth below the hairline = plane_w - dot(plane_n, p)
        F32       plane_w = 0.f;
        LLVector3 centre;
        F32       dead      = 0.f;
        F32       mid_depth = 0.f;
        F32       tip_depth = 0.f;
    };

    HairShape hairShape(const HeadFrame& head)
    {
        // The plane z = front_z + slope * (x - front_x) in the head frame:
        // plane_n = up - slope * forward.
        const F32 s     = head.scale;
        const F32 slope = (HAIRLINE_FRONT_Z - HAIRLINE_BACK_Z) / (HAIRLINE_FRONT_X - HAIRLINE_BACK_X);
        HairShape shape;
        shape.plane_n   = head.up - head.forward * slope;
        shape.plane_w   = s * (HAIRLINE_BACK_Z - slope * HAIRLINE_BACK_X) + shape.plane_n * head.base;
        shape.centre    = head.base + head.forward * (CENTRE_X * s) + head.up * (CENTRE_Z * s);
        shape.dead      = DEAD * s;
        shape.mid_depth = MID_DEPTH * s;
        shape.tip_depth = TIP_DEPTH * s;
        return shape;
    }

    F32 smoothStep(F32 e0, F32 e1, F32 x)
    {
        const F32 t = llclamp((x - e0) / (e1 - e0), 0.f, 1.f);
        return t * t * (3.f - 2.f * t);
    }

    // How far a hair vertex at p (agent frame) moves: objectSkinV.glsl
    // plusHairSwayOffset and llvknative.cpp hair_sway on the CPU.
    LLVector3 hairOffset(const LLVector3& p, const HairShape& shape, const LLVector3& mid, const LLVector3& tip)
    {
        const F32 d = shape.plane_w - shape.plane_n * p;
        if (d <= shape.dead)
        {
            return LLVector3::zero;
        }
        LLVector3 o = mid * smoothStep(shape.dead, shape.mid_depth, d) + (tip - mid) * smoothStep(shape.mid_depth, shape.tip_depth, d);
        LLVector3 outward = p - shape.centre;
        const F32 length  = outward.length();
        if (length > 0.0001f)
        {
            outward *= 1.f / length;
            o -= outward * (llmin(o * outward, 0.f) * (1.f - INWARD_KEEP));
        }
        return o;
    }

    // ---- non-rigged hair (main thread) ----

    struct HairFace
    {
        LLPointer<LLViewerObject> object;
        S32                       face = -1;
        LLPointer<LLVertexBuffer> buffer;       // the buffer original belongs to
        U32                       first = 0;    // its range in buffer
        U32                       count = 0;
        std::vector<LLVector4a>   original;     // positions as built; empty: not taken yet
        std::vector<LLVector4a>   written;      // what this feature last wrote; empty: as built
        F32                       fade = 0.f;   // 0 when found to be hair, 1 after FADE_SECONDS
    };
    std::vector<HairFace> sFaces;
    std::vector<LLVector4a> sMoved;     // a face's new positions, kept between faces and frames (no allocation each)
    U32                   sFaceFrame = 0;
    bool                  sFacesTooLarge = false;   // over NONRIGGED_MAX_VERTICES: logged once for this set of faces

    // The face's positions in its buffer's copy in memory, or null.
    LLVector4a* facePositions(LLVertexBuffer* buffer, U32 first, U32 count)
    {
        if (!buffer || !count || !buffer->getMappedData() || !buffer->hasDataType(LLVertexBuffer::TYPE_VERTEX) ||
            first + count > buffer->getNumVerts())
        {
            return nullptr;
        }
        return reinterpret_cast<LLVector4a*>(buffer->getMappedData() + buffer->getOffset(LLVertexBuffer::TYPE_VERTEX)) + first;
    }

    LLFace* faceOf(const HairFace& rec)
    {
        if (rec.object.isNull() || rec.object->isDead())
        {
            return nullptr;
        }
        LLDrawable* drawable = rec.object->mDrawable;
        return drawable && rec.face < drawable->getNumFaces() ? drawable->getFace(rec.face) : nullptr;
    }

    // Writes positions over the face's range (marked for the next flush).
    void writePositions(HairFace& rec, const std::vector<LLVector4a>& positions)
    {
        LLStrider<LLVector4a> dst;
        if (rec.buffer->getVertexStrider(dst, rec.first, (S32)rec.count))
        {
            memcpy(dst.get(), positions.data(), rec.count * sizeof(LLVector4a));
        }
    }

    // Puts the positions as built back, if the buffer still holds ours.
    void restoreFace(HairFace& rec)
    {
        if (!rec.written.empty() && rec.original.size() == rec.count)
        {
            const LLVector4a* now = facePositions(rec.buffer, rec.first, rec.count);
            if (now && !memcmp(now, rec.written.data(), rec.count * sizeof(LLVector4a)))
            {
                writePositions(rec, rec.original);
            }
        }
        rec.written.clear();
    }

    void restoreFaces()
    {
        for (HairFace& rec : sFaces)
        {
            restoreFace(rec);
        }
        if (!sFaces.empty())
        {
            LLVertexBuffer::flushBuffers();
        }
        sFaces.clear();
        sFacesTooLarge = false;     // another set of faces is logged again if it is too large
    }

    // The faces of the agent's non-rigged hair (see the top of the file).
    std::vector<std::pair<LLPointer<LLViewerObject>, S32>> scanNonRiggedHair(LLVOAvatarSelf* self, std::string& names)
    {
        std::vector<std::pair<LLPointer<LLViewerObject>, S32>> faces;
        for (const auto& point : self->mAttachmentPoints)
        {
            const LLViewerJointAttachment* attachment = point.second;
            if (!attachment || attachment->getIsHUDAttachment() || !plus::hairsway::detect::onHead(attachment, self))
            {
                continue;   // worn elsewhere: its non-rigged prims do not follow the head (see the top of the file)
            }
            for (const LLPointer<LLViewerObject>& object : attachment->mAttachedObjects)
            {
                if (object.isNull() || object->isDead())
                {
                    continue;
                }
                if (!plus::hairsway::detect::sways(object.get(), attachment, self))
                {
                    continue;
                }
                const LLViewerInventoryItem* item = gInventory.getItem(object->getAttachmentItemID());
                const size_t before = faces.size();
                auto add = [&](LLViewerObject* prim)
                {
                    LLVOVolume* volume = dynamic_cast<LLVOVolume*>(prim);
                    if (!volume || volume->isDead() || volume->isRiggedMesh() || volume->isFlexible() || !volume->mDrawable)
                    {
                        return;
                    }
                    for (S32 i = 0; i < volume->mDrawable->getNumFaces(); ++i)
                    {
                        faces.emplace_back(prim, i);
                    }
                };
                add(object.get());
                for (LLViewerObject* child : object->getChildren())
                {
                    add(child);
                }
                if (faces.size() > before)
                {
                    names += names.empty() ? "" : ", ";
                    names += item ? item->getName() : object->getAttachmentItemID().asString();
                }
            }
        }
        return faces;
    }

    // Keeps the records of faces still hair, restores and drops the others.
    void setNonRiggedFaces(const std::vector<std::pair<LLPointer<LLViewerObject>, S32>>& faces)
    {
        std::vector<HairFace> kept;
        kept.reserve(faces.size());
        for (const auto& face : faces)
        {
            auto it = std::find_if(sFaces.begin(), sFaces.end(),
                                   [&](const HairFace& rec) { return rec.object == face.first && rec.face == face.second; });
            if (it != sFaces.end())
            {
                kept.push_back(std::move(*it));
                sFaces.erase(it);
            }
            else
            {
                HairFace rec;
                rec.object = face.first;
                rec.face   = face.second;
                kept.push_back(std::move(rec));
            }
        }
        restoreFaces();     // the ones no longer hair
        sFaces.swap(kept);
    }

    // Writes the non-rigged hair's positions for this frame's offsets.
    void swayNonRigged(const HeadFrame& head, const LLVector3& mid, const LLVector3& tip, F32 dt)
    {
        if (sFaces.empty())
        {
            return;
        }
        for (HairFace& rec : sFaces)
        {
            rec.fade = llmin(rec.fade + llmax(dt, 0.f) / FADE_SECONDS, 1.f);
        }
        const bool still = mid.length() + tip.length() < NONRIGGED_STILL;

        U32 vertices = 0;
        for (const HairFace& rec : sFaces)
        {
            const LLFace* face = faceOf(rec);
            vertices += face ? face->getGeomCount() : 0;
        }
        if (vertices > NONRIGGED_MAX_VERTICES)
        {
            if (!sFacesTooLarge)
            {
                LL_INFOS("Plus") << "Hair sway: non-rigged hair of " << vertices << " vertices is too large to move (limit "
                                 << NONRIGGED_MAX_VERTICES << ")" << LL_ENDL;
                sFacesTooLarge = true;
            }
            // The positions as built go back, but the records stay: emptied, the next rescan would
            // find them new, take them again and log them every second.
            bool wrote = false;
            for (HairFace& rec : sFaces)
            {
                wrote |= !rec.written.empty();
                restoreFace(rec);
                rec.buffer = nullptr;
                rec.original.clear();
            }
            if (wrote)
            {
                LLVertexBuffer::flushBuffers();
            }
            return;
        }
        // sFacesTooLarge stays until the faces change (restoreFaces()): a level of detail going
        // back and forth over the limit does not log each time.
        if (vertices > NONRIGGED_FULL_RATE_VERTICES && (++sFaceFrame & 1) && !still)
        {
            return; // every other frame
        }

        const HairShape shape = hairShape(head);
        std::vector<LLVector4a>& moved = sMoved;
        bool wrote = false;
        for (HairFace& rec : sFaces)
        {
            LLFace*         face   = faceOf(rec);
            LLDrawable*     draw   = face ? face->getDrawable() : nullptr;
            LLVertexBuffer* buffer = face ? face->getVertexBuffer() : nullptr;
            const U32       first  = face ? face->getGeomIndex() : 0;
            const U32       count  = face ? face->getGeomCount() : 0;
            if (!draw || !draw->isActive() || !facePositions(buffer, first, count))
            {
                // Not drawn from a buffer this feature can move: as upstream
                // (what it wrote before goes back, if still there).
                wrote |= !rec.written.empty();
                restoreFace(rec);
                rec.buffer = nullptr;
                rec.original.clear();
                continue;
            }

            LLVector4a* now = facePositions(buffer, first, count);
            // Rebuilt since this feature last looked: the buffer no longer holds what it wrote, or,
            // when it holds the positions as built (nothing written: the hair stood still, e.g. with
            // the wind off), no longer those. The latter is looked at only when the hair is about to
            // move, not every still frame. (There is no rebuild counter to ask: LLDrawable's
            // generation counts moves.)
            const bool rebuilt = rec.written.empty()
                                     ? (!still && rec.original.size() == count && memcmp(now, rec.original.data(), count * sizeof(LLVector4a)))
                                     : memcmp(now, rec.written.data(), count * sizeof(LLVector4a)) != 0;
            if (buffer != rec.buffer.get() || first != rec.first || count != rec.count || rec.original.size() != count || rebuilt)
            {
                // New or rebuilt: the positions in the buffer are as built.
                rec.buffer = buffer;
                rec.first  = first;
                rec.count  = count;
                rec.original.assign(now, now + count);
                rec.written.clear();
            }

            if (still)
            {
                if (!rec.written.empty())
                {
                    writePositions(rec, rec.original);
                    rec.written.clear();
                    wrote = true;
                }
                continue;
            }

            // The buffer's positions are in the space of the matrix the face is
            // drawn with (LLVolumeGeometryManager::registerFace).
            const LLMatrix4& model = draw->isState(LLDrawable::ANIMATED_CHILD) ? draw->getWorldMatrix() : draw->getRenderMatrix();
            LLMatrix4 inverse = model;
            inverse.invert();
            bool finite = true;
            for (int r = 0; r < 4 && finite; ++r)
            {
                for (int c = 0; c < 4; ++c)
                {
                    finite = finite && std::isfinite(model.mMatrix[r][c]) && std::isfinite(inverse.mMatrix[r][c]);
                }
            }
            if (!finite)
            {
                wrote |= !rec.written.empty();
                restoreFace(rec);
                continue;
            }

            moved.resize(count);
            for (U32 v = 0; v < count; ++v)
            {
                const F32* src = rec.original[v].getF32ptr();
                // row-vector convention: p * model
                const LLVector3 world(src[0] * model.mMatrix[0][0] + src[1] * model.mMatrix[1][0] + src[2] * model.mMatrix[2][0] + model.mMatrix[3][0],
                                      src[0] * model.mMatrix[0][1] + src[1] * model.mMatrix[1][1] + src[2] * model.mMatrix[2][1] + model.mMatrix[3][1],
                                      src[0] * model.mMatrix[0][2] + src[1] * model.mMatrix[1][2] + src[2] * model.mMatrix[2][2] + model.mMatrix[3][2]);
                const LLVector3 o = hairOffset(world, shape, mid * rec.fade, tip * rec.fade);
                // back into the buffer's space: o * inverse (a direction)
                const F32 dx = o.mV[VX] * inverse.mMatrix[0][0] + o.mV[VY] * inverse.mMatrix[1][0] + o.mV[VZ] * inverse.mMatrix[2][0];
                const F32 dy = o.mV[VX] * inverse.mMatrix[0][1] + o.mV[VY] * inverse.mMatrix[1][1] + o.mV[VZ] * inverse.mMatrix[2][1];
                const F32 dz = o.mV[VX] * inverse.mMatrix[0][2] + o.mV[VY] * inverse.mMatrix[1][2] + o.mV[VZ] * inverse.mMatrix[2][2];
                moved[v].set(src[0] + dx, src[1] + dy, src[2] + dz, src[3]);    // w: the texture index
            }
            writePositions(rec, moved);
            rec.written.assign(moved.begin(), moved.end());     // into its own room of last frame
            wrote = true;
        }
        if (wrote)
        {
            LLVertexBuffer::flushBuffers();
        }
    }

    void deactivate()
    {
        restoreFaces();
        std::vector<LLVector4a>().swap(sMoved);
        sRegionWind.clear();    // fix-round53 2: turned on again, the wind starts from calm, not from then
        if (!sActive.load(std::memory_order_relaxed) && !sScanned && !sChain.valid)
        {
            return;
        }
        plus::hairsway::detect::releaseAll();     // not on the early return: that is every frame while the sway is off
        sActive.store(false, std::memory_order_relaxed);
        sAvatar.store(nullptr, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(sMutex);
            if (!sHairSkins.empty())
            {
                LL_INFOS("Plus") << "Hair sway: off" << LL_ENDL;
            }
            sHairSkins.clear();
            sMidOffset.clear();
            sTipOffset.clear();
        }
        sChain   = Chain();
        sScanned = false;
    }

    // One damped spring step (semi-implicit Euler), anchor taken at the start
    // of the step (taken at its end, a moving anchor puts the point ahead of
    // it by its velocity times the step: hair leading at a walk).
    void springStep(Point& p, const LLVector3& anchor, const LLVector3& anchor_vel, F32 hz, F32 damping, const LLVector3& push, F32 h)
    {
        const F32       omega = F_TWO_PI * hz;
        const LLVector3 acc   = (anchor - p.pos) * (omega * omega) - (p.vel - anchor_vel) * (2.f * damping * omega) - p.vel * AIR_DRAG + push;
        p.vel += acc * h;
        p.pos += p.vel * h;
    }

    // Where the tips' anchor is: their rest point carried along by the
    // middle's swing, as if the hair above them leaned with the middle.
    LLVector3 tipAnchor(const LLVector3& tip_rest, const LLVector3& mid_rest, const LLVector3& mid_pos, const LLVector3& up)
    {
        LLVector3 swing = mid_pos - mid_rest;
        const F32 vertical = swing * up;
        swing -= up * vertical;
        return tip_rest + swing * 2.f + up * vertical;  // the tips hang twice as far below the pivot
    }

    // Advances the chain by dt, the head's rest points moving linearly from
    // last frame's to these.
    void stepChain(const LLVector3& mid_rest, const LLVector3& tip_rest, const LLVector3& up, F32 dt, F32 mid_damping, F32 tip_damping,
                   const LLVector3& wind)
    {
        const LLVector3 mid_rest_vel = (mid_rest - sChain.mid_rest) / dt;
        const LLVector3 tip_rest_vel = (tip_rest - sChain.tip_rest) / dt;
        const S32       steps        = llclamp((S32)std::ceil(dt / MAX_STEP), 1, MAX_STEPS);
        const F32       h            = dt / (F32)steps;
        LLVector3       tip_anchor   = sChain.tip_anchor;
        for (S32 i = 0; i < steps; ++i)
        {
            // Both anchors at the start of the step.
            const F32       t      = h * (F32)i;
            const LLVector3 mid    = sChain.mid_rest + mid_rest_vel * t;
            const LLVector3 tip    = sChain.tip_rest + tip_rest_vel * t;
            const LLVector3 anchor = tipAnchor(tip, mid, sChain.mid.pos, up);
            springStep(sChain.mid, mid, mid_rest_vel, MID_HZ, mid_damping, wind, h);
            springStep(sChain.tip, anchor, (anchor - tip_anchor) / h, TIP_HZ, tip_damping, wind, h);
            tip_anchor = anchor;
        }
        sChain.tip_anchor = tip_anchor;
    }

    // An unsigned integer to [0, 1), the same every run.
    F32 hash01(U32 i)
    {
        U32 h = i * 2654435761u;
        h ^= h >> 15;
        h *= 2246822519u;
        h ^= h >> 13;
        return (F32)(h & 0xffffffu) / 16777216.f;
    }

    // The gust part of the wind's strength at time t: in some of the
    // GUST_INTERVAL long intervals, a GUST_TIME long sin^2 bump.
    F32 gust(F64 t)
    {
        const F64 interval = std::floor(t / GUST_INTERVAL);
        const U32 i        = (U32)(S64)interval;
        if (hash01(3u * i) < GUST_SKIP)
        {
            return 0.f;
        }
        const F64 start = interval * GUST_INTERVAL + hash01(3u * i + 1u) * (GUST_INTERVAL - GUST_TIME);
        const F32 x     = (F32)((t - start) / GUST_TIME);
        if (x < 0.f || x >= 1.f)
        {
            return 0.f;
        }
        const F32 bump = std::sin(F_PI * x);
        return (GUST_LOW + (GUST_HIGH - GUST_LOW) * hash01(3u * i + 2u)) * bump * bump;
    }

    F32 waves(const Wave* list, size_t count, F64 t)
    {
        F32 sum = 0.f;
        for (size_t k = 0; k < count; ++k)
        {
            sum += list[k].amplitude * (F32)std::sin(F_TWO_PI * t / list[k].period + list[k].phase);
        }
        return sum;
    }

    // The wind's push (m/s^2, agent frame, horizontal) for a PlusHairSwayWind
    // level; zero when off. Main thread, once a frame (smooths the region's wind).
    LLVector3 windPush(S32 level, F32 dt)
    {
        if (level <= 0 || level >= WIND_LEVEL_COUNT)
        {
            return LLVector3::zero;
        }
        LLVector3 region(gWindVec.mV[VX], gWindVec.mV[VY], 0.f);
        if (!region.isFinite())
        {
            region.clear();
        }
        sRegionWind += (region - sRegionWind) * (1.f - std::exp(-llmax(dt, 0.f) / REGION_WIND_TIME));

        // Direction and strength go over smoothly from the made-up wind (calm
        // region) to the region's: its direction outweighs the made-up one
        // above REGION_WIND_MIN, its strength counts fully from 2 x that.
        const F64       t        = plus::freeze::worldSeconds();   // the wind stops with the world
        const F32       speed    = sRegionWind.length();
        const LLVector3 toward   = sRegionWind + LLVector3(std::cos(PSEUDO_WIND_DIRECTION), std::sin(PSEUDO_WIND_DIRECTION), 0.f) * REGION_WIND_MIN;
        const F32       base     = std::atan2(toward.mV[VY], toward.mV[VX]);
        const F32       regional = llclamp(REGION_WIND_LOW + (1.f - REGION_WIND_LOW) * speed / REGION_WIND_REF, REGION_WIND_LOW, REGION_WIND_HIGH);
        const F32       factor   = 1.f + (regional - 1.f) * llclamp(speed / (2.f * REGION_WIND_MIN), 0.f, 1.f);
        const F32       angle    = base + waves(WIND_DIRECTION_WAVES, LL_ARRAY_SIZE(WIND_DIRECTION_WAVES), t);
        const F32 strength = llmax(WIND_STRENGTH_BASE + waves(WIND_STRENGTH_WAVES, LL_ARRAY_SIZE(WIND_STRENGTH_WAVES), t),
                                   WIND_STRENGTH_FLOOR) + gust(t);
        const F32 push     = WIND_LEVELS[level] * strength * factor;
        return LLVector3(std::cos(angle) * push, std::sin(angle) * push, 0.f);
    }

    // x softly limited to (-limit, limit): about x while small, never past limit.
    F32 softLimit(F32 x, F32 limit)
    {
        return limit > 0.f ? limit * std::tanh(x / limit) : 0.f;
    }

    // A point's distance from its rest as the offset the shaders apply:
    // forward swings weakened, scaled by gain, sideways / front-back softly
    // limited to max_swing, up/down to max_bounce.
    LLVector3 swingOffset(LLVector3 offset, const HeadFrame& head, F32 gain, F32 max_swing, F32 max_bounce)
    {
        offset *= gain;
        const F32 vertical = softLimit(offset * head.up, max_bounce);
        offset -= head.up * (offset * head.up);
        const F32 forward = offset * head.forward;
        if (forward > 0.f)
        {
            offset -= head.forward * (forward * (1.f - FORWARD_KEEP));
        }
        const F32 length = offset.length();
        if (length > 1e-6f)
        {
            offset *= softLimit(length, max_swing) / length;
        }
        return offset + head.up * vertical;
    }

    // The springs' damping ratio for PlusHairSwayBounce b (0..1).
    F32 dampingFor(F32 still, F32 bouncy, F32 b)
    {
        return still * std::pow(bouncy / still, llclamp(b, 0.f, 1.f));
    }

    // Idle, every frame.
    void update()
    {
        static LLCachedControl<bool> enabled(gSavedSettings, SETTING_ENABLED, false);
        static LLCachedControl<F32>  strength_setting(gSavedSettings, SETTING_STRENGTH, 1.f);
        static LLCachedControl<F32>  max_offset_setting(gSavedSettings, SETTING_MAX_OFFSET, 0.08f);
        static LLCachedControl<F32>  bounce_setting(gSavedSettings, SETTING_BOUNCE, 0.2f);
        static LLCachedControl<S32>  wind_setting(gSavedSettings, SETTING_WIND, 1);

        // Quitting: off for good (the idle callbacks still run once after a
        // logout reply sets the app quitting; see init()).
        LLVOAvatarSelf* self = (enabled && isAgentAvatarValid() && !LLApp::isExiting()) ? gAgentAvatarp.get() : nullptr;
        HeadFrame       head;
        if (!self || !headFrame(self, head))
        {
            deactivate();
            return;
        }

        // Which skins are hair.
        if (!sScanned || sRescanTimer.getElapsedTimeF32() > RESCAN_SECONDS)
        {
            std::string      names;
            std::vector<U64> skins = scanHairSkins(self, names);
            sRescanTimer.reset();
            sScanned = true;

            std::string nonrigged_names;
            const auto  faces = scanNonRiggedHair(self, nonrigged_names);
            if (faces.size() != sFaces.size() ||
                !std::equal(faces.begin(), faces.end(), sFaces.begin(),
                            [](const auto& face, const HairFace& rec) { return face.first == rec.object && face.second == rec.face; }))
            {
                LL_INFOS("Plus") << "Hair sway: " << faces.size() << " non-rigged hair face(s)"
                                 << (nonrigged_names.empty() ? std::string() : ": " + nonrigged_names) << LL_ENDL;
                setNonRiggedFaces(faces);
            }

            plus::hairsway::detect::endScan();

            std::lock_guard<std::mutex> lock(sMutex);
            if (skins.size() != sHairSkins.size() ||
                !std::equal(skins.begin(), skins.end(), sHairSkins.begin(), [](U64 hash, const HairSkin& skin) { return hash == skin.hash; }))
            {
                LL_INFOS("Plus") << "Hair sway: " << skins.size() << " rigged hair skin(s)"
                                 << (names.empty() ? std::string() : ": " + names) << LL_ENDL;
                std::vector<HairSkin> kept;
                kept.reserve(skins.size());
                for (U64 hash : skins)
                {
                    auto it = std::lower_bound(sHairSkins.begin(), sHairSkins.end(), hash,
                                               [](const HairSkin& skin, U64 h) { return skin.hash < h; });
                    kept.push_back({ hash, it != sHairSkins.end() && it->hash == hash ? it->fade : 0.f });
                }
                sHairSkins.swap(kept);
            }
        }

        // The chain.
        const LLVector3 back     = head.forward * HANG_BACK;
        const LLVector3 mid_rest = head.pivot - back - head.up * (RAMP * 0.5f);
        const LLVector3 tip_rest = head.pivot - back - head.up * RAMP;
        const F32       dt       = LLFrameTimer::getFrameDeltaTimeF32();
        if (!sChain.valid || !(dt <= RESET_DT) || dist_vec(mid_rest, sChain.mid_rest) > RESET_JUMP ||
            dist_vec(sChain.mid.pos, mid_rest) > RESET_JUMP || dist_vec(sChain.tip.pos, tip_rest) > RESET_JUMP)
        {
            sChain            = Chain();
            sChain.valid      = true;
            sChain.mid.pos    = mid_rest;
            sChain.tip.pos    = tip_rest;
            sChain.tip_anchor = tip_rest;
        }
        else if (plus::freeze::isFrozen())
        {
            // fix-round22: the world is stopped, so the hair is too. The chain keeps its offset
            // from the rest points; a head the poser moves carries it along (no swing, no lag).
            const LLVector3 mid_move = mid_rest - sChain.mid_rest;
            const LLVector3 tip_move = tip_rest - sChain.tip_rest;
            sChain.mid.pos += mid_move;
            sChain.tip.pos += tip_move;
            sChain.tip_anchor += tip_move;
        }
        else if (dt > 0.f)
        {
            const F32 bounce = bounce_setting;
            stepChain(mid_rest, tip_rest, head.up, dt, dampingFor(MID_DAMPING_STILL, MID_DAMPING_BOUNCY, bounce),
                      dampingFor(TIP_DAMPING_STILL, TIP_DAMPING_BOUNCY, bounce), windPush(wind_setting, dt));
            if (!sChain.mid.pos.isFinite() || !sChain.mid.vel.isFinite() || !sChain.tip.pos.isFinite() || !sChain.tip.vel.isFinite())
            {
                sChain.mid        = { mid_rest, LLVector3::zero };
                sChain.tip        = { tip_rest, LLVector3::zero };
                sChain.tip_anchor = tip_rest;
            }
        }
        sChain.mid_rest = mid_rest;
        sChain.tip_rest = tip_rest;

        // The offsets.
        const F32       gain       = llmax((F32)strength_setting, 0.f) * RESPONSE;
        const F32       max_offset = llclamp((F32)max_offset_setting, 0.f, 0.5f);
        const LLVector3 mid = swingOffset(sChain.mid.pos - mid_rest, head, gain, max_offset * MID_SHARE, MAX_BOUNCE * 0.5f);
        const LLVector3 tip = swingOffset(sChain.tip.pos - tip_rest, head, gain, max_offset, MAX_BOUNCE);

        swayNonRigged(head, mid, tip, dt);

        bool any;
        {
            std::lock_guard<std::mutex> lock(sMutex);
            sMidOffset = mid;
            sTipOffset = tip;
            for (HairSkin& skin : sHairSkins)
            {
                skin.fade = llmin(skin.fade + llmax(dt, 0.f) / FADE_SECONDS, 1.f);
            }
            any        = !sHairSkins.empty();
        }
        sAvatar.store(self, std::memory_order_relaxed);
        if (any)
        {
            sEverActive.store(true, std::memory_order_relaxed);
        }
        sActive.store(any, std::memory_order_release);
    }

    bool onIdle()
    {
        update();
        return false; // every frame, for the viewer's lifetime
    }

    void init()
    {
        // Settings: app_settings/settings_plus.xml (PlusHairSway*).
        // Set in the photo tools (app_settings/plus/phototools/catalog.xml, tab "subject").
        // Drawing: the <plus> hooks in LLVOAvatar::updateSkinInfoMatrixPalette()
        // and in lldrawpool.cpp's palette upload; objectSkinV.glsl; llvknative.cpp.
        // Which attachments are hair: hairdetect.cpp (and its items in the
        // attachment context menu, menu_plus_hairsway.xml).
        plus::hairsway::detect::init();
        doOnIdleRepeating(&onIdle);

        // The app quitting (LLApp's "quitting", before cleanup()): the hair's
        // objects and vertex buffers let go now, not in sFaces' static
        // destructor after the object list, the pipeline and the renderer
        // have gone (as plus/ao does at "quitting").
        LLEventPumps::instance().obtain("LLApp").listen("PlusHairSway", [](const LLSD& event)
        {
            if (event["status"].asString() == "quitting" && on_main_thread())
            {
                deactivate();
                plus::hairsway::detect::shutdown();
            }
            return false;
        });
    }

    plus::FeatureRegistration sRegistration({ "hairsway", &init, &plus::hairsway::detect::onUIReady });

    // Column c of the entry as the shaders read it: row c's xyz and row 3's
    // component c (see updateSkinInfoMatrixPalette's copy).
    void setColumn(LLMatrix4a& entry, int c, const LLVector3& xyz, F32 w)
    {
        entry.mMatrix[c].set(xyz.mV[VX], xyz.mV[VY], xyz.mV[VZ], 0.f);
        entry.mMatrix[3].getF32ptr()[c] = w;
    }
}

namespace plus::hairsway
{
    U32 adjustPalette(LLVOAvatar* avatar, const LLMeshSkinInfo* skin, std::vector<LLMatrix4a>& palette, U32 count)
    {
        if (!sActive.load(std::memory_order_acquire) || avatar != sAvatar.load(std::memory_order_relaxed) || !skin || !count ||
            palette.size() != count || count + SWAY_ENTRIES > (U32)LLSkinningUtil::getMaxJointCount())
        {
            return count;
        }

        LLVector3 mid, tip;
        {
            std::lock_guard<std::mutex> lock(sMutex);
            auto it = std::lower_bound(sHairSkins.begin(), sHairSkins.end(), skin->mHash,
                                       [](const HairSkin& entry, U64 h) { return entry.hash < h; });
            if (it == sHairSkins.end() || it->hash != skin->mHash || it->fade <= 0.f)
            {
                return count;
            }
            mid = sMidOffset * it->fade;
            tip = sTipOffset * it->fade;
        }

        // The head as these joints were built (this frame's pose).
        HeadFrame head;
        if (!headFrame(avatar, head))
        {
            return count;
        }

        const HairShape shape = hairShape(head);

        palette.resize(count + SWAY_ENTRIES);
        LLMatrix4a& a = palette[count];
        LLMatrix4a& b = palette[count + 1];
        a.mMatrix[3].clear();
        b.mMatrix[3].clear();
        setColumn(a, 0, shape.plane_n, shape.plane_w);
        setColumn(a, 1, shape.centre, SWAY_MARK);
        setColumn(a, 2, mid, 0.f);
        setColumn(b, 0, tip, 0.f);
        setColumn(b, 1, LLVector3(shape.dead, shape.mid_depth, shape.tip_depth), INWARD_KEEP);
        setColumn(b, 2, LLVector3::zero, 0.f);
        return count + SWAY_ENTRIES;
    }

    U32 swayIndex(const std::vector<LLMatrix4a>& palette)
    {
        if (palette.size() < 1 + SWAY_ENTRIES)
        {
            return 0;
        }
        const size_t index = palette.size() - SWAY_ENTRIES;
        return palette[index].mMatrix[3].getF32ptr()[1] == SWAY_MARK ? (U32)index : 0;
    }

    void bindShader(LLGLSLShader* shader, const std::vector<LLMatrix4a>& palette)
    {
        static const LLStaticHashedString sUniform("plusHairSway");
        const U32 index = swayIndex(palette);
        if (!shader || (!index && !sEverActive.load(std::memory_order_relaxed)))
        {
            return;
        }
        shader->uniform1f(sUniform, (F32)index);
    }

    void clearShader(LLGLSLShader* shader)
    {
        static const LLStaticHashedString sUniform("plusHairSway");
        if (!shader || !sEverActive.load(std::memory_order_relaxed))
        {
            return;
        }
        shader->uniform1f(sUniform, 0.f);
    }
}
