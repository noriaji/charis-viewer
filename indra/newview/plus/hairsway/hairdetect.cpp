/**
 * @file hairdetect.cpp
 * @brief "Hair sway": which of the agent's attachments are hair, judged by
 *        their shape and textures (not their names), or set by hand
 *        (research/hair-detect.md).
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

// The judgement, per attachment (its inventory item), in steps:
//  1. By hand first: the attachment's context menu sets "sway as hair" or
//     "do not sway" (PlusHairSwayManual, per account, by item UUID); "automatic"
//     takes it back. Without PlusHairSwayAutoDetect only "sway as hair" sways.
//  2. On the head: the prims that could be hair are the non-rigged ones of an
//     attachment on a head joint (mHead, a joint under it, mNeck) and the
//     rigged ones whose skin weights lie mostly on the head and the upper
//     body (RIGGED_HEAD_SHARE on the head and neck, RIGGED_UPPER_SHARE with
//     the chest, collars, shoulders and upper spine). None: not hair.
//     Flexible prims are never hair (they sway by themselves).
//  3. Loaded: their meshes (and skins), and their textures at least down to
//     TEXTURE_READY_DISCARD. Until then the attachment waits, unswayed.
//     Without its meshes it stays undecided however long they take (never
//     judged without them); WAIT_SECONDS after they are all in it is judged
//     on what textures there are, and such a judgement (logged with how many
//     textures were missing) is made again once they are in (looked at every
//     RECHECK_SECONDS; the first one is kept meanwhile).
//  4. Points: their vertices and the centres of their triangles when both fit
//     MAX_POINTS, else the vertices alone, thinned out to it (no centres then),
//     in the agent frame (rigged: skinned on the CPU as LLRiggedVolume does),
//     then in mHead's frame, scaled to the default skeleton's head. A mesh is
//     taken at its highest LOD (asked for; a rigged one only once its weights,
//     as it shows now, lie on the head, or it shows under SPARSE_RIGGED
//     vertices and has some weight on the head or neck), not at the LOD it
//     shows: a hair far off shows a few vertices. One that did not come in in HIGH_LOD_WAIT_SECONDS is
//     judged on the shown LOD and again when it does (for LATE_HIGH_LOD_SECONDS).
//  5. Shape score (research/hair-detect.md; tools/hair-sway-sim/detect.py
//     computes the same features on made-up shapes, from vertices only):
//     + 2 x how much of the scalp it lies on (directions from the head's
//       centre with points 0-3.5 cm off the head, against 60 %),
//     + 1 x how much hangs 5 cm or more below the hairline (against 20 %),
//     - 2 x how much of the face (brows to mouth) it covers (against 30 %),
//     - 2 x how much stands out 5 cm or more above the hairline beyond 15 %
//       (brims, horns, animal ears, headset cups; a bun stands out a little),
//     + 0.5 if its points lie behind the head's centre on average.
//  6. Texture score: + 1.5 x the alpha share (vertices of faces drawn with
//     alpha: blend or mask, by material, GLTF material, face colour or a
//     texture with alpha), + 0.5 x how much of their textures is clear or
//     partly clear (LLImageGL::getAlphaParts, against 30 %), - 1 if under
//     20 % of it is alpha (a knit cap or a helmet: opaque).
//  7. Hair when the sum reaches PlusHairSwayDetectThreshold + MARGIN: a score
//     at the threshold is not hair (the safe side). The judgement is kept
//     until the attachment's meshes or textures change (or, made without
//     every texture, until they are in: step 3), and logged once
//     with every part of the score; the sway then starts from nothing
//     (hairsway.cpp fades it in).
// PlusHairSwayDetectOnly judges and logs, nothing sways (for setting the
// threshold in world). Names are not used: they miss hair named otherwise
// and catch "hair" named things that are not; the manual setting covers
// what the score gets wrong (a veil or lace on the scalp scores as hair).

#include "llviewerprecompiledheaders.h"

#include "hairdetect.h"

#include "llcontrol.h"
#include "llgltfmaterial.h"
#include "llimagegl.h"
#include "llinventorymodel.h"
#include "llmaterial.h"
#include "llmatrix4a.h"
#include "llmenugl.h"
#include "llmeshrepository.h"
#include "llmodel.h"
#include "llprimitive.h"
#include "llsdutil.h"
#include "llselectmgr.h"
#include "llskinningutil.h"
#include "lltextureentry.h"
#include "lluictrl.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewermenu.h"
#include "llviewertexture.h"
#include "llvoavatarself.h"
#include "llvovolume.h"
#include "llvolume.h"
#include "llvolumemgr.h"

#include <array>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace
{
    constexpr char SETTING_AUTO[]      = "PlusHairSwayAutoDetect";
    constexpr char SETTING_ONLY[]      = "PlusHairSwayDetectOnly";
    constexpr char SETTING_THRESHOLD[] = "PlusHairSwayDetectThreshold";
    constexpr char SETTING_MANUAL[]    = "PlusHairSwayManual";      // per account: { item UUID: "hair" | "not" }
    constexpr char MENU_FILE[]         = "menu_plus_hairsway.xml";  // items added to the agent's attachment menu

    constexpr F32 MARGIN                = 0.25f;  // above the threshold by this much to be hair
    constexpr F32 RIGGED_HEAD_SHARE     = 0.3f;
    constexpr F32 RIGGED_UPPER_SHARE    = 0.85f;
    constexpr S32 TEXTURE_READY_DISCARD = 1;
    constexpr F64 WAIT_SECONDS          = 30.0;   // for textures, from when the meshes are in
    constexpr F64 RECHECK_SECONDS       = 5.0;    // a judgement made without every texture: looked at again this often
    constexpr U32 MAX_POINTS            = 6000;
    constexpr U32 SPARSE_RIGGED         = 64;     // a rigged prim whose shown LOD has fewer weighted vertices says nothing yet about where it is worn
    constexpr F64 HIGH_LOD_WAIT_SECONDS = 20.0;   // for a mesh's highest LOD, from asking; then its shown LOD is judged
    constexpr F64 HIGH_LOD_ASK_SECONDS  = 8.0;    // asking for it again after this long without it
    constexpr S32 MAX_HIGH_LOD_ASKS     = 2;      // the first request and one more; a failed fetch is not hammered
    constexpr F64 LATE_HIGH_LOD_SECONDS = 120.0;  // a judgement made on a shown LOD waits this long for the highest to come in

    // The head in mHead's frame of the default skeleton (meters, x forward,
    // y left, z up); hairsway.cpp, detect.py and weights.py use the same.
    constexpr F32 DEFAULT_SKULL    = 0.079f;
    constexpr F32 CENTRE_X = -0.005f, CENTRE_Z = 0.08f;
    constexpr F32 RADIUS_X = 0.100f, RADIUS_Y = 0.078f, RADIUS_Z = 0.110f;
    constexpr F32 HAIRLINE_FRONT_X = 0.09f, HAIRLINE_FRONT_Z = 0.15f;
    constexpr F32 HAIRLINE_BACK_X = -0.09f, HAIRLINE_BACK_Z = -0.01f;

    // Features (detect.py).
    constexpr F32 SCALP_LOW = -0.005f, SCALP_HIGH = 0.035f;  // m off the head: lies on the scalp
    constexpr F32 FACE_NEAR = 0.04f;                          // m off the head: covers the face
    constexpr F32 STANDS_OUT = 0.05f;                         // m off the head above the hairline
    constexpr F32 HANGS = 0.05f;                              // m below the hairline
    constexpr S32 BINS = 18;                                  // 10 degree rows from the top, 20 degree columns around

    enum class Verdict
    {
        Waiting,
        Hair,
        NotHair
    };

    // A mesh prim's highest LOD, held (a reference in the volume manager) while
    // the judgement needs it. The LOD the prim shows now is the wrong one to
    // judge on: worn at a distance it is the lowest, which creators often
    // leave a few vertices or one triangle (a hair judged on 6 points).
    struct HighLod
    {
        std::shared_ptr<LLVolume> volume;       // from LLVolumeMgr::refVolume, let go with unrefVolume
        F64                       asked_at = 0.0;
        F64                       next_ask = 0.0;
        S32                       asks     = 0;     // at most MAX_HIGH_LOD_ASKS
    };
    using HighLods = std::map<LLUUID, HighLod>;     // by the prim's id

    struct Entry
    {
        U64         signature = 0;
        Verdict     verdict   = Verdict::Waiting;
        F64         loaded_at = -1.0;       // when its meshes (and skins) were all in; < 0: not yet
        std::set<std::string> told_wait;    // what it was logged waiting for
        bool        partial    = false;     // judged before every texture was in: judged again when they are
        F64         next_check = 0.0;       // partial: when to look at its textures again
        HighLods    high;                   // let go with the verdict (kept while partial or late), by the scan that does not see
                                            // the attachment, by hand / automatic off, and by releaseHeld()
        std::vector<LLUUID> late;           // prims judged on the LOD they show, their highest not in after HIGH_LOD_WAIT_SECONDS:
                                            // judged again when it comes in (until late_until)
        F64         late_until = 0.0;       // set when late goes from empty to not empty, back to 0 when it is emptied
        bool        rejudge = false;        // it let go of a late judgement (the LODs were released): judged again when it is next seen
    };
    std::map<LLUUID, Entry> sEntries;   // by inventory item
    std::set<LLUUID>        sSeen;      // in the current scan

    F64 now()
    {
        return LLTimer::getTotalSeconds();
    }

    std::string itemName(const LLUUID& item_id)
    {
        const LLViewerInventoryItem* item = gInventory.getItem(item_id);
        return item ? item->getName() : item_id.asString();
    }

    // ---- by hand ----

    // "hair", "not", or "" (automatic).
    std::string manual(const LLUUID& item_id)
    {
        if (item_id.isNull() || !gSavedPerAccountSettings.controlExists(SETTING_MANUAL))
        {
            return std::string();
        }
        const LLSD map = gSavedPerAccountSettings.getLLSD(SETTING_MANUAL);
        const std::string key = item_id.asString();
        return map.isMap() && map.has(key) ? map[key].asString() : std::string();
    }

    void setManual(const LLUUID& item_id, const std::string& value)
    {
        if (item_id.isNull() || !gSavedPerAccountSettings.controlExists(SETTING_MANUAL))
        {
            return;
        }
        LLSD map = gSavedPerAccountSettings.getLLSD(SETTING_MANUAL);
        if (!map.isMap())
        {
            map = LLSD::emptyMap();
        }
        const std::string key = item_id.asString();
        if (value.empty())
        {
            map.erase(key);
        }
        else
        {
            map[key] = value;
        }
        gSavedPerAccountSettings.setLLSD(SETTING_MANUAL, map);
        LL_INFOS("Plus") << "Hair sway: '" << itemName(item_id) << "' set to " << (value.empty() ? std::string("automatic") : value)
                         << " by hand" << LL_ENDL;
    }

    // The agent's attachment the context menu is for (its item), or null.
    LLUUID selectedAttachment()
    {
        LLViewerObject* object = LLSelectMgr::getInstance()->getSelection()->getPrimaryObject();
        LLViewerObject* root   = object ? object->getRootEdit() : nullptr;
        if (!root || root->isDead() || !root->isAttachment() || root->isHUDAttachment() || !isAgentAvatarValid() ||
            root->getAvatar() != gAgentAvatarp.get())
        {
            return LLUUID::null;
        }
        return root->getAttachmentItemID();
    }

    // ---- the head ----

    struct Head
    {
        LLVector3 base;
        LLVector3 forward, left, up;    // unit
        F32       scale = 1.f;          // against the default skeleton's head
    };

    LLVector3 rowXYZ(const LLMatrix4a& m, int row)
    {
        const F32* v = m.mMatrix[row].getF32ptr();
        return LLVector3(v[0], v[1], v[2]);
    }

    bool headOf(LLVOAvatar* avatar, Head& out)
    {
        if (!avatar->mHeadp)
        {
            return false;
        }
        const LLMatrix4a& m = avatar->mHeadp->getWorldMatrix4a();
        out.forward = rowXYZ(m, 0);
        out.up      = rowXYZ(m, 2);
        if (out.forward.normalize() < 1e-6f || out.up.normalize() < 1e-6f)
        {
            return false;
        }
        out.left  = out.up % out.forward;
        out.base  = rowXYZ(m, 3);
        out.scale = 1.f;
        if (avatar->mSkullp)
        {
            out.scale = llclamp(dist_vec(rowXYZ(avatar->mSkullp->getWorldMatrix4a(), 3), out.base) / DEFAULT_SKULL, 0.6f, 1.6f);
        }
        return out.base.isFinite() && out.left.isFinite();
    }

    // ---- what is gathered from an attachment ----

    struct Gathered
    {
        std::vector<LLVector3> points;  // head frame, default size
        F64 faces_weight = 0.0;         // vertices of the candidate faces
        F64 alpha_weight = 0.0;         // of those drawn with alpha
        F64 parts_weight = 0.0;         // of those whose texture's alpha parts are known
        F64 parts_sum    = 0.0;         // clear + partly clear, weighted
        S32 prims = 0;
        S32 textures_waiting = 0;
        S32 low_lod = 0;                // meshes judged on the LOD they show (no highest LOD, or it did not come in)
        std::vector<LLUUID> late;       // of those, the ones whose highest LOD was asked for and did not come in in time
    };

    enum class Gather
    {
        Ready,
        Waiting,
        NoCandidate
    };

    bool isFlexible(LLVOVolume* volume)
    {
        return volume->isFlexible();
    }

    void unrefVolume(LLVolume* volume)
    {
        if (LLVolumeMgr* manager = LLPrimitive::getVolumeManager())     // none after the viewer's cleanup
        {
            manager->unrefVolume(volume);
        }
    }

    // The faces to judge a prim on, whose shown volume (current) is in: a
    // mesh's highest LOD (asked for, as LLVOVolume::getLODTriangleCount
    // does), else null while it is not in. After HIGH_LOD_WAIT_SECONDS
    // without it (or when the mesh has no such LOD) the shown one, counted in
    // out.low_lod (and out.late for the one that was asked for).
    const LLVolume* shapeOf(LLVOVolume* volume, const LLVolume* current, HighLods& high, Gathered& out)
    {
        if (!volume->isMesh())
        {
            return current;
        }
        const F64 t = now();
        auto it = high.find(volume->getID());
        if (it == high.end())
        {
            const S32 lod = gMeshRepo.getActualMeshLOD(current->getParams(), LLModel::LOD_HIGH);
            LLVolumeMgr* manager = LLPrimitive::getVolumeManager();
            LLVolume*    ref     = lod >= 0 && manager ? manager->refVolume(current->getParams(), lod) : nullptr;
            if (!ref)
            {
                ++out.low_lod;
                return current;     // no usable mesh LOD
            }
            HighLod held;
            held.volume   = std::shared_ptr<LLVolume>(ref, &unrefVolume);
            held.asked_at = t;
            held.next_ask = t;
            it = high.emplace(volume->getID(), std::move(held)).first;
        }
        HighLod&  held = it->second;
        LLVolume* ref  = held.volume.get();
        if (ref->isMeshAssetLoaded() && ref->getNumVolumeFaces() > 0)
        {
            return ref;
        }
        if (t - held.asked_at > HIGH_LOD_WAIT_SECONDS)
        {
            ++out.low_lod;
            out.late.push_back(volume->getID());
            return current;
        }
        if (t >= held.next_ask && held.asks < MAX_HIGH_LOD_ASKS)
        {
            held.next_ask = t + HIGH_LOD_ASK_SECONDS;
            ++held.asks;
            gMeshRepo.loadMesh(volume, ref->getParams(), LLVolumeLODGroup::getVolumeDetailFromScale(ref->getDetail()));
        }
        return nullptr;
    }

    // A face's triangle centres, so a sparse mesh (a few big quads) lies on
    // the bins its surface covers and not only where its corners are. pts:
    // every vertex of the face (nothing is added for a thinned-out face).
    void addCentres(const LLVolumeFace& face, const std::vector<LLVector3>& pts, std::vector<LLVector3>& out)
    {
        if (!face.mIndices || pts.size() != (size_t)face.mNumVertices)
        {
            return;
        }
        for (S32 i = 0; i + 2 < face.mNumIndices; i += 3)
        {
            const size_t a = face.mIndices[i], b = face.mIndices[i + 1], c = face.mIndices[i + 2];
            if (a < pts.size() && b < pts.size() && c < pts.size())
            {
                out.push_back((pts[a] + pts[b] + pts[c]) * (1.f / 3.f));
            }
        }
    }

    // mHead or under it, or mNeck: 0; the chest, collars, shoulders, upper
    // spine: 1; anything else: 2.
    int jointPlace(LLJoint* joint, LLVOAvatar* avatar)
    {
        if (!joint)
        {
            return 2;
        }
        if (joint == avatar->mNeckp)
        {
            return 0;
        }
        for (LLJoint* j = joint; j; j = j->getParent())
        {
            if (j == avatar->mHeadp)
            {
                return 0;
            }
        }
        static const char* const UPPER[] = { "mChest", "mCollarLeft", "mCollarRight", "mShoulderLeft", "mShoulderRight",
                                             "mSpine3", "mSpine4", "mTorso" };
        for (const char* name : UPPER)
        {
            if (joint->getName() == name)
            {
                return 1;
            }
        }
        return 2;
    }

    // Where one vertex's skin weights lie (jointPlace), as shares of its total
    // (all 0 without weights). joints: the skin's joint count.
    void vertexPlaces(const LLMeshSkinInfo* skin, const F32* w, S32 joints, LLVOAvatarSelf* self, F32 part[3])
    {
        LLVOAvatar* avatar = self;     // LLVOAvatarSelf hides getJoint(S32)
        F32 sum = 0.f;
        part[0] = part[1] = part[2] = 0.f;
        for (int k = 0; k < 4; ++k)
        {
            const S32 index  = llclamp((S32)std::floor(w[k]), 0, joints - 1);
            const F32 weight = w[k] - std::floor(w[k]);
            LLJoint*  joint  = index < (S32)skin->mJointNums.size() ? avatar->getJoint(skin->mJointNums[index]) : nullptr;
            part[jointPlace(joint, self)] += weight;
            sum += weight;
        }
        if (sum > 0.f)
        {
            for (int p = 0; p < 3; ++p)
            {
                part[p] /= sum;
            }
        }
    }

    // Step 2 for a rigged prim: its summed weights lie mostly on the head and the upper body.
    bool wornOnHead(const F64 share[3])
    {
        const F64 total = share[0] + share[1] + share[2];
        return total > 0.0 && share[0] / total >= RIGGED_HEAD_SHARE && (share[0] + share[1]) / total >= RIGGED_UPPER_SHARE;
    }

    // The same for a prim as it shows now (a thinned-out look at the vertices
    // of the LOD it shows), before anything is asked for: a body, a dress or
    // shoes (hundreds of vertices even at their lowest LOD) do not fetch their
    // highest LOD. True too when the shown LOD has under SPARSE_RIGGED vertices
    // and some weight on the head or neck (not a guarantee for a body or a dress
    // whose lowest LOD is tiny: it then fetches its highest, and is dropped there).
    bool riggedOnHead(LLVOVolume* volume, const LLVolume* shape, LLVOAvatarSelf* self)
    {
        const LLMeshSkinInfo* skin = volume->getSkinInfo();
        const U32 joints = LLSkinningUtil::getMeshJointCount(skin);
        if (!joints || skin->mInvBindMatrix.size() < joints)
        {
            return false;
        }
        std::vector<LLMatrix4a> palette(joints);
        LLSkinningUtil::initSkinningMatrixPalette(palette.data(), (S32)joints, skin, self);   // also sets mJointNums
        U32 vertices = 0;
        for (S32 i = 0; i < shape->getNumVolumeFaces(); ++i)
        {
            vertices += (U32)shape->getVolumeFace(i).mNumVertices;
        }
        const U32 stride = llmax(1u, vertices / 1500u);
        U32 sampled = 0;
        F64 share[3] = { 0.0, 0.0, 0.0 };
        for (S32 i = 0; i < shape->getNumVolumeFaces(); ++i)
        {
            const LLVolumeFace& face = shape->getVolumeFace(i);
            if (!face.mWeights)
            {
                continue;
            }
            for (S32 v = 0; v < face.mNumVertices; v += (S32)stride)
            {
                F32 part[3];
                vertexPlaces(skin, face.mWeights[v].getF32ptr(), (S32)joints, self, part);
                for (int p = 0; p < 3; ++p)
                {
                    share[p] += part[p];
                }
                ++sampled;
            }
        }
        // A far off prim shows its lowest LOD: a few vertices (some with a stray
        // weight) are no ground to drop it for good: with some weight on the
        // head or neck it is let through, and the check on its highest LOD
        // (below) decides.
        return sampled < SPARSE_RIGGED ? share[0] > 0.0 : wornOnHead(share);
    }

    // The faces' textures: how much of the candidate's vertices are drawn
    // with alpha, and how clear their textures are; counts textures not in yet.
    void gatherTextures(LLVOVolume* volume, const LLVolume* shape, Gathered& out)
    {
        const S32 faces = llmin((S32)volume->getNumTEs(), shape->getNumVolumeFaces());
        for (S32 i = 0; i < faces; ++i)
        {
            const F64             weight = shape->getVolumeFace(i).mNumVertices;
            const LLTextureEntry* te     = volume->getTE((U8)i);
            if (!te || weight <= 0.0)
            {
                continue;
            }
            LLViewerTexture* texture = volume->getTEImage((U8)i);
            LLImageGL*       image   = texture ? texture->getGLTexture() : nullptr;
            if (LLViewerFetchedTexture* fetched = LLViewerTextureManager::staticCastToFetchedTexture(texture))
            {
                const S32 discard = fetched->getDiscardLevel();
                if (!fetched->isFullyLoaded() && (discard < 0 || discard > TEXTURE_READY_DISCARD || !image || !image->getHasGLTexture()))
                {
                    ++out.textures_waiting;
                }
            }

            bool alpha = te->getColor().mV[VALPHA] < 0.999f;
            if (const LLGLTFMaterial* gltf = te->getGLTFRenderMaterial())
            {
                alpha = alpha || gltf->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND || gltf->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK;
            }
            else if (const LLMaterial* material = te->getMaterialParams().get())
            {
                const U8 mode = material->getDiffuseAlphaMode();
                alpha = alpha || mode == LLMaterial::DIFFUSE_ALPHA_MODE_BLEND || mode == LLMaterial::DIFFUSE_ALPHA_MODE_MASK;
            }
            else
            {
                alpha = alpha || (texture && texture->getComponents() == 4);
            }

            out.faces_weight += weight;
            if (alpha)
            {
                out.alpha_weight += weight;
                const U32 parts = image ? image->getAlphaParts() : 0;
                if (parts & LLImageGL::ALPHA_PARTS_VALID)
                {
                    const F64 clear   = (F64)(parts & 0x7fff) / LLImageGL::ALPHA_PARTS_ONE;
                    const F64 partial = (F64)((parts >> 15) & 0x7fff) / LLImageGL::ALPHA_PARTS_ONE;
                    out.parts_weight += weight;
                    out.parts_sum += weight * (clear + partial);
                }
            }
        }
    }

    // The candidate prims of an attachment (step 2) and, when they are in
    // (step 3), their points and textures. wait: what is missing.
    Gather gather(LLViewerObject* root, const LLViewerJointAttachment* attachment, LLVOAvatarSelf* self, const Head& head,
                  bool points, HighLods& high, Gathered& out, std::string& wait)
    {
        const bool on_head = plus::hairsway::detect::onHead(attachment, self);
        std::vector<LLVOVolume*> prims;
        prims.push_back(dynamic_cast<LLVOVolume*>(root));
        for (LLViewerObject* child : root->getChildren())
        {
            prims.push_back(dynamic_cast<LLVOVolume*>(child));
        }

        // Which prims, and are they in.
        struct Candidate
        {
            LLVOVolume*     volume;
            const LLVolume* shape;      // the faces judged (a mesh's highest LOD)
            bool            rigged;
        };
        std::vector<Candidate> candidates;
        bool waiting = false;
        for (LLVOVolume* volume : prims)
        {
            if (!volume || volume->isDead() || isFlexible(volume))
            {
                continue;
            }
            const LLVolume* shape = volume->getVolume();
            if (!shape || (volume->isMesh() && !shape->isMeshAssetLoaded()) || shape->getNumVolumeFaces() <= 0)
            {
                waiting = true;
                wait = "mesh";
                continue;
            }
            bool weighted = false;
            for (S32 i = 0; i < shape->getNumVolumeFaces() && !weighted; ++i)
            {
                weighted = shape->getVolumeFace(i).mWeights != nullptr;
            }
            if (weighted && !volume->getSkinInfo())
            {
                waiting = true;
                wait = "skin";
                continue;
            }
            if (weighted || on_head)
            {
                // Only a prim that could be hair asks for its highest LOD (a rigged
                // one that is not worn on the head, by the LOD it shows, does not).
                if (weighted && !riggedOnHead(volume, shape, self))
                {
                    continue;
                }
                const LLVolume* judged = shapeOf(volume, shape, high, out);
                if (!judged)
                {
                    waiting = true;
                    wait = "high LOD";
                    continue;
                }
                candidates.push_back({ volume, judged, weighted });
            }
        }
        if (waiting)
        {
            return Gather::Waiting;
        }
        if (candidates.empty())
        {
            return Gather::NoCandidate;
        }

        // Rigged prims: where their weights lie (step 2), and their points.
        U32 vertices = 0;
        for (const Candidate& c : candidates)
        {
            const LLVolume* shape = c.shape;
            for (S32 i = 0; i < shape->getNumVolumeFaces(); ++i)
            {
                const LLVolumeFace& face = shape->getVolumeFace(i);
                vertices += (U32)face.mNumVertices + (U32)(llmax(face.mNumIndices, 0) / 3);   // its points: the vertices and the triangle centres
            }
        }
        // The points are the vertices and the triangle centres when both fit
        // MAX_POINTS, else the vertices thinned out to it (never both over it).
        const bool with_centres = vertices <= MAX_POINTS;
        U32 plain = 0;
        for (const Candidate& c : candidates)
        {
            for (S32 i = 0; i < c.shape->getNumVolumeFaces(); ++i)
            {
                plain += (U32)c.shape->getVolumeFace(i).mNumVertices;
            }
        }
        const U32 stride = with_centres ? 1u : llmax(1u, (plain + MAX_POINTS - 1) / MAX_POINTS);

        std::vector<Candidate> kept;
        for (const Candidate& c : candidates)
        {
            const LLVolume* shape = c.shape;
            if (!c.rigged)
            {
                kept.push_back(c);
                continue;
            }
            const LLMeshSkinInfo* skin = c.volume->getSkinInfo();
            const U32 joints = LLSkinningUtil::getMeshJointCount(skin);
            if (!joints || skin->mInvBindMatrix.size() < joints)
            {
                continue;
            }
            std::vector<LLMatrix4a> palette(joints);
            LLSkinningUtil::initSkinningMatrixPalette(palette.data(), (S32)joints, skin, self);   // also sets mJointNums
            F64 share[3] = { 0.0, 0.0, 0.0 };
            std::vector<LLVector3> rigged_points;
            for (S32 i = 0; i < shape->getNumVolumeFaces(); ++i)
            {
                const LLVolumeFace& face = shape->getVolumeFace(i);
                if (!face.mWeights || !face.mPositions)
                {
                    continue;
                }
                std::vector<LLVector3> face_points;
                for (S32 v = 0; v < face.mNumVertices; v += (S32)stride)
                {
                    F32* w = face.mWeights[v].getF32ptr();
                    F32 part[3];
                    vertexPlaces(skin, w, (S32)joints, self, part);
                    for (int p = 0; p < 3; ++p)
                    {
                        share[p] += part[p];
                    }
                    if (points)
                    {
                        LLMatrix4a final_mat;
                        LLSkinningUtil::getPerVertexSkinMatrix(w, palette.data(), false, final_mat, joints);
                        LLVector4a t, dst;
                        skin->mBindShapeMatrix.affineTransform(face.mPositions[v], t);
                        final_mat.affineTransform(t, dst);
                        face_points.emplace_back(dst.getF32ptr()[0], dst.getF32ptr()[1], dst.getF32ptr()[2]);
                    }
                }
                rigged_points.insert(rigged_points.end(), face_points.begin(), face_points.end());
                if (with_centres)
                {
                    addCentres(face, face_points, rigged_points);
                }
            }
            if (!wornOnHead(share))
            {
                continue;   // not worn on the head
            }
            kept.push_back(c);
            for (const LLVector3& p : rigged_points)
            {
                const LLVector3 v = p - head.base;
                out.points.emplace_back(v * head.forward / head.scale, v * head.left / head.scale, v * head.up / head.scale);
            }
        }
        if (kept.empty())
        {
            return Gather::NoCandidate;
        }

        // Non-rigged points, and every candidate's textures.
        for (const Candidate& c : kept)
        {
            const LLVolume* shape = c.shape;
            gatherTextures(c.volume, shape, out);
            ++out.prims;
            if (c.rigged || !points)
            {
                continue;
            }
            for (S32 i = 0; i < shape->getNumVolumeFaces(); ++i)
            {
                const LLVolumeFace& face = shape->getVolumeFace(i);
                if (!face.mPositions)
                {
                    continue;
                }
                std::vector<LLVector3> face_points;
                for (S32 v = 0; v < face.mNumVertices; v += (S32)stride)
                {
                    const F32*      src = face.mPositions[v].getF32ptr();
                    const LLVector3 p   = c.volume->volumePositionToAgent(LLVector3(src[0], src[1], src[2]));
                    const LLVector3 d   = p - head.base;
                    face_points.emplace_back(d * head.forward / head.scale, d * head.left / head.scale, d * head.up / head.scale);
                }
                out.points.insert(out.points.end(), face_points.begin(), face_points.end());
                if (with_centres)
                {
                    addCentres(face, face_points, out.points);
                }
            }
        }
        if (out.textures_waiting)
        {
            wait = "textures";
            return Gather::Waiting;
        }
        return Gather::Ready;
    }

    // ---- the score ----

    struct Score
    {
        F32 scalp = 0.f, face = 0.f, out = 0.f, hang = 0.f, back = 0.f;    // shape features
        F32 alpha = 0.f, clear = -1.f;                                     // texture features (clear: -1 unknown)
        F32 shape = 0.f, texture = 0.f;
        F32 total() const { return shape + texture; }
    };

    Score score(const Gathered& g)
    {
        Score s;
        std::array<bool, BINS * BINS> scalp_bins{}, face_bins{};
        U32 out = 0, hang = 0;
        F64 back = 0.0;
        const F32 slope = (HAIRLINE_FRONT_Z - HAIRLINE_BACK_Z) / (HAIRLINE_FRONT_X - HAIRLINE_BACK_X);
        for (const LLVector3& p : g.points)
        {
            const LLVector3 d(p.mV[VX] - CENTRE_X, p.mV[VY], p.mV[VZ] - CENTRE_Z);
            const F32 r = d.length();
            if (r < 1e-4f)
            {
                continue;
            }
            const LLVector3 u = d * (1.f / r);
            const F32 theta = std::acos(llclamp(u.mV[VZ], -1.f, 1.f));
            const F32 phi   = std::atan2(u.mV[VY], u.mV[VX]);
            const F32 k = 1.f / std::sqrt((u.mV[VX] / RADIUS_X) * (u.mV[VX] / RADIUS_X) + (u.mV[VY] / RADIUS_Y) * (u.mV[VY] / RADIUS_Y) +
                                          (u.mV[VZ] / RADIUS_Z) * (u.mV[VZ] / RADIUS_Z));
            const F32 off   = r - k;
            const F32 depth = (HAIRLINE_BACK_Z + slope * (p.mV[VX] - HAIRLINE_BACK_X)) - p.mV[VZ];
            const S32 row = llclamp((S32)(theta / (F_PI / BINS)), 0, BINS - 1);
            const S32 col = llclamp((S32)((phi + F_PI) / (F_TWO_PI / BINS)), 0, BINS - 1);
            if (off > SCALP_LOW && off < SCALP_HIGH)
            {
                scalp_bins[row * BINS + col] = true;
            }
            if (off < FACE_NEAR)
            {
                face_bins[row * BINS + col] = true;
            }
            out += (off > STANDS_OUT && depth < 0.f) ? 1 : 0;
            hang += depth > HANGS ? 1 : 0;
            back -= d.mV[VX];
        }
        // Which bins count: the scalp (above the ears, but the face), the face (brows to mouth).
        U32 scalp_total = 0, scalp_hit = 0, face_total = 0, face_hit = 0;
        for (S32 row = 0; row < BINS; ++row)
        {
            for (S32 col = 0; col < BINS; ++col)
            {
                const F32 theta = (row + 0.5f) * (F_PI / BINS);
                const F32 phi   = (col + 0.5f) * (F_TWO_PI / BINS) - F_PI;
                const bool front = std::fabs(phi) < 60.f * DEG_TO_RAD;
                if (theta < 90.f * DEG_TO_RAD && !(front && theta > 52.f * DEG_TO_RAD))
                {
                    ++scalp_total;
                    scalp_hit += scalp_bins[row * BINS + col] ? 1 : 0;
                }
                if (theta > 85.f * DEG_TO_RAD && theta < 125.f * DEG_TO_RAD && std::fabs(phi) < 40.f * DEG_TO_RAD)
                {
                    ++face_total;
                    face_hit += face_bins[row * BINS + col] ? 1 : 0;
                }
            }
        }
        const F32 n = (F32)llmax((size_t)1, g.points.size());
        s.scalp = scalp_total ? (F32)scalp_hit / scalp_total : 0.f;
        s.face  = face_total ? (F32)face_hit / face_total : 0.f;
        s.out   = out / n;
        s.hang  = hang / n;
        s.back  = (F32)(back / n * 100.0);
        s.shape = 2.f * llmin(s.scalp / 0.6f, 1.f) + 1.f * llmin(s.hang / 0.2f, 1.f) - 2.f * llmin(s.face / 0.3f, 1.f) -
                  2.f * llmin(llmax(s.out - 0.15f, 0.f) / 0.25f, 1.f) + (s.back > 1.f ? 0.5f : 0.f);

        s.alpha   = g.faces_weight > 0.0 ? (F32)(g.alpha_weight / g.faces_weight) : 0.f;
        s.clear   = g.parts_weight > 0.0 ? (F32)(g.parts_sum / g.parts_weight) : -1.f;
        s.texture = 1.5f * s.alpha + (s.clear >= 0.f ? 0.5f * llmin(s.clear / 0.3f, 1.f) : 0.f) - (s.alpha < 0.2f ? 1.f : 0.f);
        return s;
    }

    // ---- the cache ----

    U64 mix(U64 h, U64 v)
    {
        h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
        return h;
    }

    U64 mixId(U64 h, const LLUUID& id)
    {
        U64 a, b;
        memcpy(&a, id.mData, 8);
        memcpy(&b, id.mData + 8, 8);
        return mix(mix(h, a), b);
    }

    // What the judgement depends on: the prims' meshes and their textures.
    U64 signature(LLViewerObject* root)
    {
        U64 h = 0;
        auto add = [&](LLViewerObject* prim)
        {
            LLVOVolume* volume = dynamic_cast<LLVOVolume*>(prim);
            if (!volume || volume->isDead())
            {
                return;
            }
            if (const LLVolume* shape = volume->getVolume())
            {
                h = mixId(h, shape->getParams().getSculptID());
            }
            h = mix(h, (U64)volume->getNumTEs());
            for (U8 i = 0; i < volume->getNumTEs(); ++i)
            {
                if (const LLTextureEntry* te = volume->getTE(i))
                {
                    h = mixId(h, te->getID());
                    h = mixId(h, te->getMaterialID().asUUID());
                    h = mix(h, (U64)(te->getColor().mV[VALPHA] * 255.f));
                    h = mix(h, te->getGLTFMaterial() ? 1 : 0);
                }
            }
        };
        add(root);
        for (LLViewerObject* child : root->getChildren())
        {
            add(child);
        }
        return h;
    }

    void logVerdict(const LLUUID& item_id, bool hair, const Score& s, F32 threshold, const Gathered& g, const char* again)
    {
        static LLCachedControl<bool> detect_only(gSavedSettings, SETTING_ONLY, false);
        std::string missing;
        if (g.textures_waiting > 0)
        {
            missing = ", judged with " + std::to_string(g.textures_waiting) + " texture(s) not in " +
                      std::to_string((int)WAIT_SECONDS) + " s after its meshes were (judged again when they are in)";
        }
        LL_INFOS("Plus") << "Hair sway detect: '" << itemName(item_id) << "' (" << item_id << "): "
                         << (again ? again : "") << (hair ? "HAIR" : "not hair")
                         << (detect_only ? " (detect only)" : "") << ", score " << s.total() << " (hair from " << threshold + MARGIN
                         << ") = shape " << s.shape << " [scalp " << s.scalp << ", face " << s.face << ", stands out " << s.out
                         << ", hangs " << s.hang << ", behind " << s.back << " cm] + textures " << s.texture << " [alpha "
                         << s.alpha << ", clear " << (s.clear >= 0.f ? std::to_string(s.clear) : std::string("unknown")) << "]; "
                         << g.prims << " prim(s), " << g.points.size() << " point(s)"
                         << (!g.late.empty() ? ", " + std::to_string(g.late.size()) + " mesh(es) on the LOD they show (the highest did not come in; judged again when it does)" : std::string())
                         << (g.low_lod > (S32)g.late.size() ? ", " + std::to_string(g.low_lod - (S32)g.late.size()) + " mesh(es) with no higher LOD" : std::string())
                         << missing << LL_ENDL;
    }

    // Logs once per thing it waits for ("mesh", "skin", "textures").
    void tellWaiting(Entry& entry, const LLUUID& item_id, const std::string& wait)
    {
        if (entry.told_wait.insert(wait).second)
        {
            LL_INFOS("Plus") << "Hair sway detect: '" << itemName(item_id) << "' waits for its " << wait
                             << (wait == "textures" ? " (at most " + std::to_string((int)WAIT_SECONDS) + " s)" : std::string())
                             << LL_ENDL;
        }
    }

    // Lets go of the highest LODs an entry holds (its judgement is kept). One
    // made on a shown LOD (late) is marked to be made again when the attachment
    // is next seen (again: no mark, the judgement stands).
    void letGo(Entry& entry, bool again)
    {
        if (again && !entry.late.empty())
        {
            entry.rejudge = true;
            entry.loaded_at = -1.0;     // the textures' WAIT_SECONDS start again with the next judgement
        }
        entry.high.clear();
        entry.late.clear();
        entry.late_until = 0.0;
    }

    // Nothing of the attachment is worn on the head. Judged on a shown LOD
    // (seen.late) it stays late: judged again when the highest LOD is in.
    Verdict notWorn(Entry& entry, const Gathered& seen)
    {
        entry.partial = false;
        entry.rejudge = false;
        if (seen.late.empty())
        {
            letGo(entry, false);
        }
        else
        {
            if (entry.late.empty())
            {
                entry.late_until = now() + LATE_HIGH_LOD_SECONDS;
            }
            entry.late = seen.late;
        }
        return Verdict::NotHair;
    }

    void releaseHeld(const LLUUID& item_id)
    {
        auto it = sEntries.find(item_id);
        if (it != sEntries.end())
        {
            letGo(it->second, true);
        }
    }

    // A prim judged on the LOD it shows whose highest has come in since.
    bool lateArrived(const Entry& entry)
    {
        for (const LLUUID& id : entry.late)
        {
            auto it = entry.high.find(id);
            if (it != entry.high.end() && it->second.volume->isMeshAssetLoaded() && it->second.volume->getNumVolumeFaces() > 0)
            {
                return true;
            }
        }
        return false;
    }

    // Steps 2-7 for one attachment: the new verdict, or entry.verdict as it
    // is while something is not in. Again (entry.partial, a verdict made
    // without every texture): only once they are all in, else quietly kept.
    Verdict judge(LLViewerObject* root, const LLViewerJointAttachment* attachment, LLVOAvatarSelf* self, Entry& entry, const LLUUID& item_id)
    {
        const bool again = entry.verdict != Verdict::Waiting;
        const bool was_partial = entry.partial;
        const bool was_rejudge = entry.rejudge;
        Head head;
        if (!headOf(self, head))
        {
            return entry.verdict;
        }
        std::string wait;
        Gathered    probe;
        Gather      state = gather(root, attachment, self, head, false, entry.high, probe, wait);
        if (state == Gather::NoCandidate)
        {
            LL_INFOS("Plus") << "Hair sway detect: '" << itemName(item_id) << "' (" << item_id << "): not hair (not worn on the head)"
                             << (probe.late.empty() ? "" : " (on a shown LOD: judged again when its highest LOD is in)") << LL_ENDL;
            return notWorn(entry, probe);
        }
        if (state == Gather::Waiting && wait != "textures")
        {
            // Its meshes (or skins) are not in: undecided however long they
            // take; the textures' WAIT_SECONDS start when they are.
            if (!again)
            {
                tellWaiting(entry, item_id, wait);
            }
            return entry.verdict;
        }
        if (entry.loaded_at < 0.0)
        {
            entry.loaded_at = now();
        }
        const bool timed_out = (!again || was_rejudge) && now() - entry.loaded_at > WAIT_SECONDS;
        if (state == Gather::Waiting && !timed_out)
        {
            if (!again)
            {
                tellWaiting(entry, item_id, wait);
            }
            return entry.verdict;
        }

        Gathered g;
        state = gather(root, attachment, self, head, true, entry.high, g, wait);
        if (state == Gather::NoCandidate)
        {
            return notWorn(entry, g);
        }
        if (state == Gather::Waiting && !(wait == "textures" && timed_out))
        {
            return entry.verdict;
        }
        static LLCachedControl<F32> threshold(gSavedSettings, SETTING_THRESHOLD, 3.f);
        const Score s    = score(g);
        const bool  hair = s.total() >= (F32)threshold + MARGIN;
        entry.partial    = g.textures_waiting > 0;
        entry.next_check = now() + RECHECK_SECONDS;
        logVerdict(item_id, hair, s, threshold, g,
                   !again         ? nullptr
                   : was_partial  ? "judged again with its textures in: "
                   : was_rejudge  ? "judged again after its LODs were let go: "
                   : g.late.empty() ? "judged again with its highest LOD in: "
                                    : "judged again with some of its highest LODs in: ");
        entry.rejudge = false;
        if (g.late.empty())
        {
            entry.late_until = 0.0;
        }
        else if (entry.late.empty())
        {
            entry.late_until = now() + LATE_HIGH_LOD_SECONDS;   // from the first judgement on a shown LOD; not extended
        }
        entry.late = g.late;
        if (!entry.partial && entry.late.empty())
        {
            entry.high.clear();     // the highest LODs are let go with the verdict
        }
        return hair ? Verdict::Hair : Verdict::NotHair;
    }
}

namespace plus::hairsway::detect
{
    bool onHead(const LLViewerJointAttachment* attachment, LLVOAvatar* avatar)
    {
        LLJoint* joint = attachment ? const_cast<LLViewerJointAttachment*>(attachment)->getParent() : nullptr;
        if (!joint || !avatar->mHeadp)
        {
            return false;
        }
        if (joint == avatar->mNeckp)
        {
            return true;
        }
        for (S32 depth = 0; joint && depth < 32; ++depth, joint = joint->getParent())
        {
            if (joint == avatar->mHeadp)
            {
                return true;
            }
        }
        return false;
    }

    bool sways(LLViewerObject* root, const LLViewerJointAttachment* attachment, LLVOAvatarSelf* self)
    {
        static LLCachedControl<bool> auto_detect(gSavedSettings, SETTING_AUTO, true);
        static LLCachedControl<bool> detect_only(gSavedSettings, SETTING_ONLY, false);
        if (!root || root->isDead() || !attachment || attachment->getIsHUDAttachment())
        {
            return false;
        }
        const LLUUID item_id = root->getAttachmentItemID();
        if (item_id.isNull())
        {
            return false;
        }
        sSeen.insert(item_id);

        const std::string by_hand = manual(item_id);
        if (by_hand == "hair" || by_hand == "not")
        {
            releaseHeld(item_id);
            return by_hand == "hair" && !detect_only;
        }
        if (!auto_detect && !detect_only)
        {
            releaseHeld(item_id);
            return false;
        }

        Entry&    entry = sEntries[item_id];
        const U64 sig   = signature(root);
        if (entry.signature != sig)
        {
            entry           = Entry();
            entry.signature = sig;
        }
        if (entry.verdict == Verdict::Waiting)
        {
            entry.verdict = judge(root, attachment, self, entry, item_id);
        }
        else if ((entry.partial || entry.rejudge || !entry.late.empty()) && now() >= entry.next_check)
        {
            entry.next_check = now() + RECHECK_SECONDS;
            if (entry.partial || entry.rejudge)
            {
                // partial: the textures; rejudge: it let go of a late judgement. Looked at
                // again every RECHECK_SECONDS until it is made (it waits quietly for its LODs).
                entry.verdict = judge(root, attachment, self, entry, item_id);
            }
            else if (now() > entry.late_until)
            {
                letGo(entry, false);    // the highest LOD never came: the judgement stands
            }
            else if (lateArrived(entry))
            {
                entry.verdict = judge(root, attachment, self, entry, item_id);
            }
        }
        return entry.verdict == Verdict::Hair && !detect_only;
    }

    void shutdown()
    {
        sEntries.clear();   // lets go of the held LODs while the volume manager is there
        sSeen.clear();
    }

    void releaseAll()
    {
        for (auto& entry : sEntries)
        {
            letGo(entry.second, true);
        }
    }

    void endScan()
    {
        for (auto it = sEntries.begin(); it != sEntries.end();)
        {
            if (sSeen.count(it->first))
            {
                ++it;
            }
            else if (it->second.verdict == Verdict::Waiting)
            {
                it = sEntries.erase(it);
            }
            else
            {
                letGo(it->second, true);    // not worn now: its judgement is kept, the LODs are not
                ++it;
            }
        }
        sSeen.clear();
    }

    void init()
    {
        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.HairSway.SetManual", [](LLUICtrl*, const LLSD& param)
        {
            const std::string value = param.asString();
            setManual(selectedAttachment(), value == "hair" || value == "not" ? value : std::string());
        });
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.HairSway.IsManual", [](LLUICtrl*, const LLSD& param)
        {
            const LLUUID item_id = selectedAttachment();
            if (item_id.isNull())
            {
                return false;
            }
            const std::string value = manual(item_id);
            return param.asString() == (value.empty() ? std::string("auto") : value);
        });
        LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.HairSway.IsOwnAttachment",
                                                                 [](LLUICtrl*, const LLSD&) { return selectedAttachment().notNull(); });
    }

    void onUIReady()
    {
        // Moves the items of menu_plus_hairsway.xml to the end of the agent's
        // attachment context menu (menu_attachment_self.xml is not touched).
        if (!gMenuAttachmentSelf)
        {
            LL_WARNS("Plus") << "No attachment menu; the hair sway's manual items are not shown" << LL_ENDL;
            return;
        }
        LLMenuGL* source = LLUICtrlFactory::getInstance()->createFromFile<LLMenuGL>(MENU_FILE, gMenuHolder,
                                                                                    LLViewerMenuHolderGL::child_registry_t::instance());
        if (!source)
        {
            LL_WARNS("Plus") << "Cannot build " << MENU_FILE << LL_ENDL;
            return;
        }
        std::vector<LLMenuItemGL*> items;
        for (U32 i = 0; i < source->getItemCount(); ++i)
        {
            items.push_back(source->getItem(i));
        }
        for (LLMenuItemGL* item : items)
        {
            if (dynamic_cast<LLMenuItemTearOffGL*>(item))
            {
                continue;
            }
            source->removeChild(item);
            gMenuAttachmentSelf->addChild(item);
        }
        if (source->getParent())
        {
            source->getParent()->removeChild(source);
        }
        delete source;
        gMenuAttachmentSelf->needsArrange();
    }
}
