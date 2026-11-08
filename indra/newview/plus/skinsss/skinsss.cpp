/**
 * @file skinsss.cpp
 * @brief Skin subsurface scattering (research/100-skin-sss.md). Stage 1: the
 *        skin mark in the main view's stencil, and a view of it. Registers
 *        with the plus foundation.
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

// The mark: two stencil bits (LLRHI::STENCIL_SKIN, LLRHI::STENCIL_EYE) of the
// main view's depth. While something needs it (PlusSkinSssDebug, or a feature
// that uses the mark such as plus/skinfill, all off by default; Vulkan only,
// not with LLRHI_SKIN_SSS=0), that depth is D24S8 instead of the official
// D24, and during the main view's G-buffer pass every draw that passes the
// depth test writes the bits: skin, eye or neither.
// What is left is whether the nearest opaque surface of a pixel is skin or
// eye; later passes test it with the stencil (research/100, section 3).
//
// Skin is (section 1):
//  - a rigged batch of an avatar whose texture is that avatar's Bakes on Mesh
//    head, upper body, lower body, left arm, left leg or AUX1..3 bake (not the
//    eyes, hair or skirt bakes; not a default image standing in for a bake);
//  - the avatar's system body (head, upper and lower body meshes);
//  - for avatars near enough that the scatter would cover a pixel (a cut with
//    some hysteresis), not animesh or UI avatars.
// Eyes are (research/110 section 14), of such an avatar whose face is skin
// (its head bake or system head was marked within the last frames: eyes in
// a face that is not lit with it are left alone):
//  - a rigged batch whose texture is the avatar's Bakes on Mesh eyes bake;
//  - a rigged batch of a mesh weighted to the eye joints only (at most with
//    the head's): mesh eyes with textures of their own;
//  - a batch, rigged or not, of an object worn on an eye attachment point
//    (Left / Right Eyeball, Alt Left / Right Eye): mesh eyes whatever their
//    texture and weights (fix-round41: eyes weighted to other joints too, or
//    not rigged at all, stayed dark in a lit face);
//  - the system eyeballs.
// While a feature asks for it (addAvatarMarkUser: plus/wetlook keeps avatars
// dry), a third bit (LLRHI::STENCIL_AVATAR) marks every batch of an avatar,
// at any distance, with the skin and eye bits or alone: its rigged meshes
// (skin, clothes, hair, mesh bodies), its system body and eyeballs; not
// animesh, not attachments that are not rigged (a hat, shoes). With it a
// fourth (LLRHI::STENCIL_AVATAR_SKIN) marks the avatar's bare skin and eyes
// at any distance: a batch showing one of its skin bakes, its eyes bake, mesh
// eyes (as above), the system body and eyeballs; the wet look's snow does not
// lie there.
// Otherwise nothing changes: the depth keeps its format and the stencil is
// never touched.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "skinsss.h"

#include "llappviewer.h"
#include "llavatarappearancedefines.h"
#include "lldrawable.h"
#include "llface.h"
#include "llfetchedgltfmaterial.h"
#include "llmodel.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhigl.h"
#include "llrhiskinsss.h"
#include "llspatialpartition.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llviewerjointattachment.h"
#include "llviewertexture.h"
#include "llvoavatar.h"
#include "llvovolume.h"
#include "pipeline.h"

#include "presets/presets.h"

#include <chrono>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn

namespace plus::skinsss::detail
{
    bool sMarking = false;
}

namespace
{
    using namespace LLAvatarAppearanceDefines;

    constexpr char SETTING_DEBUG[] = "PlusSkinSssDebug";

    // The avatar texture entries that hold the skin bakes (research/100, section 1).
    constexpr ETextureIndex SKIN_BAKES[] =
    {
        TEX_HEAD_BAKED, TEX_UPPER_BAKED, TEX_LOWER_BAKED, TEX_LEFT_ARM_BAKED, TEX_LEFT_LEG_BAKED,
        TEX_AUX1_BAKED, TEX_AUX2_BAKED, TEX_AUX3_BAKED,
    };

    // The texture IDs a face uses for those bakes (Bakes on Mesh).
    bool isSkinBakeId(const LLUUID& id)
    {
        return id == IMG_USE_BAKED_HEAD || id == IMG_USE_BAKED_UPPER || id == IMG_USE_BAKED_LOWER || id == IMG_USE_BAKED_LEFTARM ||
               id == IMG_USE_BAKED_LEFTLEG || id == IMG_USE_BAKED_AUX1 || id == IMG_USE_BAKED_AUX2 || id == IMG_USE_BAKED_AUX3;
    }

    // The mark a face's texture ID asks for (texture index batching keeps
    // faces of different marks apart).
    U32 markOfBakeId(const LLUUID& id)
    {
        if (isSkinBakeId(id))
        {
            return LLRHI::STENCIL_SKIN;
        }
        return id == IMG_USE_BAKED_EYES ? LLRHI::STENCIL_EYE : 0;
    }

    constexpr U32 STENCIL_MARKS = LLRHI::STENCIL_SKIN | LLRHI::STENCIL_EYE;
    constexpr U32 STENCIL_MARKS_AVATAR = STENCIL_MARKS | LLRHI::STENCIL_AVATAR | LLRHI::STENCIL_AVATAR_SKIN;

    // A rigged mesh is eyes when its joints are among these, with at least
    // one eye joint (the Bento eye joints as well as the classic ones).
    constexpr const char* EYE_JOINTS[] = { "mEyeLeft", "mEyeRight", "mFaceEyeAltLeft", "mFaceEyeAltRight" };
    constexpr const char* HEAD_JOINTS[] = { "mHead", "mSkull", "mFaceRoot" };
    // The rigged meshes worked out (LLMeshSkinInfo::mHash -> eyes or not);
    // started again when it grows this large.
    constexpr size_t MAX_SKINS_KNOWN = 4096;

    // An avatar's face is skin while its head bake or system head was marked
    // within this many frames (its eyes may be drawn before its face).
    constexpr U32 FACE_SKIN_FRAMES = 2;

    // How wide the skin scatters, in m, for the cut below. Jimenez's default
    // width; stage 2 sets the real one.
    constexpr F32 SCATTER_WIDTH = 0.012f;
    // An avatar is marked while that width covers at least this many pixels,
    // and stays marked down to HYSTERESIS times as many (no flicker at the edge).
    constexpr F32 MIN_SCATTER_PIXELS = 1.f;
    constexpr F32 HYSTERESIS = 0.8f;
    // Avatars not seen for this many frames are forgotten.
    constexpr U32 FORGET_AFTER_FRAMES = 64;

    // The overlay of PlusSkinSssDebug: magenta on skin, cyan on eyes, mostly
    // opaque (the shading underneath still shows the shapes).
    constexpr F32 DEBUG_COLOR[3] = { 1.f, 0.f, 1.f };
    constexpr F32 DEBUG_EYE_COLOR[3] = { 0.f, 1.f, 1.f };
    constexpr F32 DEBUG_OPACITY = 0.75f;

    // How often the counters go to the log, as a VKPerf line.
    constexpr F64 REPORT_SECONDS = 5.0;

    // ------------------------------------------------------------------

    bool backendReady()
    {
        return LLRHI::sSkinSss && LLRHI::sSkinSssDebug;
    }

    bool debugWanted()
    {
        static LLCachedControl<bool> debug(gSavedSettings, SETTING_DEBUG, false);
        return debug;
    }

    // The features that use the mark (plus::skinsss::addMarkUser).
    std::vector<bool (*)()>& markUsers()
    {
        static std::vector<bool (*)()> users;
        return users;
    }

    // The features that use the avatar bit (plus::skinsss::addAvatarMarkUser).
    std::vector<bool (*)()>& avatarMarkUsers()
    {
        static std::vector<bool (*)()> users;
        return users;
    }

    bool anyWants(const std::vector<bool (*)()>& users)
    {
        for (bool (*wanted)() : users)
        {
            if (wanted())
            {
                return true;
            }
        }
        return false;
    }

    // Whether the avatar bit is needed.
    bool avatarBitWanted()
    {
        return backendReady() && anyWants(avatarMarkUsers());
    }

    // Whether the mark is needed (stage 2 adds the scattering itself).
    bool markWanted()
    {
        return backendReady() && (debugWanted() || anyWants(markUsers()) || anyWants(avatarMarkUsers()));
    }

    // The main view's depth as onScreenBuffers left it with a stencil.
    struct MarkedDepth
    {
        U32 name = 0;
        U32 width = 0;
        U32 height = 0;
    };
    MarkedDepth sMarkedDepth;
    U32 sScreenBuffersGeneration = 0;

    bool depthHasStencil()
    {
        const LLRenderTarget& screen = gPipeline.mMainRT.deferredScreen;
        return sMarkedDepth.name && sMarkedDepth.name == screen.getDepth() && sMarkedDepth.width == screen.getWidth() &&
               sMarkedDepth.height == screen.getHeight();
    }

    // The frame whose G-buffer pass marked skin (the debug view shows only that).
    U32 sMarkedFrame = U32_MAX;
    // Skin and eye batches, system bodies and eyeballs that pass marked (none:
    // nothing to draw on).
    U32 sSkinDrawsThisPass = 0;
    U32 sSkinDrawsMarked = 0;

    // Whether this G-buffer pass writes the avatar bit; the bits it writes;
    // the frame whose pass wrote the avatar bit.
    bool sAvatarBit = false;
    U32 sMarkBits = STENCIL_MARKS;
    U32 sAvatarMarkedFrame = U32_MAX;

    // ------------------------------------------------------------------
    // Which avatars are marked

    struct AvatarState
    {
        U32 frame = U32_MAX;            // when `marked` was last worked out
        bool marked = false;
        U32 face_skin_frame = U32_MAX;  // when its head bake or system head was last marked
    };
    std::unordered_map<const LLVOAvatar*, AvatarState> sAvatars;

    // This G-buffer pass: how far from the camera (m) the scatter width still
    // covers MIN_SCATTER_PIXELS.
    F32 sMarkDistance = 0.f;
    LLVector3 sCameraOrigin;

    // The last avatar asked about (its batches come one after another) and
    // its state while marked (null: not marked). The map's elements stay put
    // while others are added; it is pruned only between G-buffer passes.
    const LLVOAvatar* sLastAvatar = nullptr;
    AvatarState* sLastState = nullptr;

    struct Counters
    {
        U32 frames = 0;
        U32 skin_batches = 0;
        U32 eye_batches = 0;
        U32 system_bodies = 0;
        U32 system_eyes = 0;
        U32 avatar_batches = 0;     // with the avatar bit (skin and eye batches among them)
        U32 avatars_marked = 0;
        U32 avatars_too_far = 0;
    };
    Counters sCounters;
    std::chrono::steady_clock::time_point sReportStart;

    // avatar's state when it is marked this pass, else null.
    AvatarState* markedState(const LLVOAvatar* avatar)
    {
        if (avatar == sLastAvatar)
        {
            return sLastState;
        }
        sLastAvatar = avatar;
        sLastState = nullptr;
        if (avatar->isDead() || avatar->isControlAvatar() || avatar->isUIAvatar())
        {
            return nullptr;
        }
        AvatarState& state = sAvatars[avatar];
        if (state.frame != gFrameCount)
        {
            const F32 reach = state.marked ? sMarkDistance / HYSTERESIS : sMarkDistance;
            state.marked = dist_vec_squared(avatar->getPositionAgent(), sCameraOrigin) <= reach * reach;
            state.frame = gFrameCount;
            if (state.marked)
            {
                ++sCounters.avatars_marked;
            }
            else
            {
                ++sCounters.avatars_too_far;
            }
        }
        sLastState = state.marked ? &state : nullptr;
        return sLastState;
    }

    bool avatarMarked(const LLVOAvatar* avatar)
    {
        return markedState(avatar) != nullptr;
    }

    // Whether the avatar's face is skin now: only then are its eyes marked.
    bool faceIsSkin(const AvatarState& state)
    {
        return state.face_skin_frame != U32_MAX && gFrameCount - state.face_skin_frame <= FACE_SKIN_FRAMES;
    }

    void forgetOldAvatars()
    {
        for (auto it = sAvatars.begin(); it != sAvatars.end();)
        {
            it = gFrameCount - it->second.frame > FORGET_AFTER_FRAMES ? sAvatars.erase(it) : std::next(it);
        }
    }

    // Whether texture is avatar's bake (not a stand-in for one).
    bool isBakeOf(const LLVOAvatar* avatar, ETextureIndex bake, const LLViewerTexture* texture)
    {
        if (avatar->getTEImage((U8)bake) != texture)
        {
            return false;
        }
        const LLUUID& id = texture->getID();
        return texture != LLViewerFetchedTexture::sDefaultImagep.get() && id != IMG_DEFAULT && id != IMG_DEFAULT_AVATAR &&
               id != IMG_INVISIBLE;
    }

    // Which of avatar's skin bakes texture is; TEX_NUM_INDICES when none.
    ETextureIndex skinBakeOf(const LLVOAvatar* avatar, const LLViewerTexture* texture)
    {
        for (ETextureIndex bake : SKIN_BAKES)
        {
            if (avatar->getTEImage((U8)bake) == texture)
            {
                return isBakeOf(avatar, bake, texture) ? bake : TEX_NUM_INDICES;
            }
        }
        return TEX_NUM_INDICES;
    }

    bool isJointIn(const std::string& name, const char* const* joints, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            if (name == joints[i])
            {
                return true;
            }
        }
        return false;
    }

    // Whether a rigged mesh is eyes: weighted to eye joints only, perhaps
    // with the head's (mesh eyes with textures of their own). Worked out once
    // per skin (the joint names are compared), then looked up.
    std::unordered_map<U64, bool> sEyeSkins;
    const LLMeshSkinInfo* sLastSkin = nullptr;      // within one G-buffer pass
    bool sLastSkinIsEyes = false;

    bool isEyeSkin(const LLMeshSkinInfo* skin)
    {
        if (!skin || !skin->mHash)
        {
            return false;
        }
        if (skin == sLastSkin)
        {
            return sLastSkinIsEyes;
        }
        sLastSkin = skin;
        auto known = sEyeSkins.find(skin->mHash);
        if (known != sEyeSkins.end())
        {
            sLastSkinIsEyes = known->second;
            return sLastSkinIsEyes;
        }
        bool eye = false;
        bool other = false;
        for (const std::string& name : skin->mJointNames)
        {
            if (isJointIn(name, EYE_JOINTS, std::size(EYE_JOINTS)))
            {
                eye = true;
            }
            else if (!isJointIn(name, HEAD_JOINTS, std::size(HEAD_JOINTS)))
            {
                other = true;
                break;
            }
        }
        if (sEyeSkins.size() >= MAX_SKINS_KNOWN)
        {
            sEyeSkins.clear();
        }
        sLastSkinIsEyes = eye && !other;
        sEyeSkins[skin->mHash] = sLastSkinIsEyes;
        return sLastSkinIsEyes;
    }

    // The eye attachment points (character/avatar_lad.xml): Left Eyeball,
    // Right Eyeball, Alt Left Eye, Alt Right Eye. Mesh eyes are worn there.
    constexpr S32 EYE_ATTACHMENT_POINTS[] = { 15, 16, 50, 51 };

    // What avatars wear on those points, gathered at the start of each
    // G-buffer pass (a few entries; looked up linearly). Rigid objects are
    // known by the model matrix their batches carry: registerFace takes the
    // drawable's own (world or render) matrix for an attachment and never
    // batches faces of different model matrices together, so such a batch
    // holds the faces of that object alone. Rigged ones by their skin (the
    // object's own LLMeshSkinInfo, which its faces and batches point to; not
    // its hash: meshes from one rig, a head and its clothes, may share the
    // hash) and the avatar that wears them.
    struct EyeMatrix
    {
        const LLMatrix4* matrix;
        const LLVOAvatar* avatar;
    };
    struct EyeAttachmentSkin
    {
        const LLMeshSkinInfo* skin;
        const LLVOAvatar* avatar;
    };
    std::vector<EyeMatrix> sEyeMatrices;
    std::vector<EyeAttachmentSkin> sEyeAttachmentSkins;

    void addEyeObject(LLViewerObject* object, const LLVOAvatar* avatar)
    {
        if (!object || object->isDead())
        {
            return;
        }
        if (object->isRiggedMesh())
        {
            // Only volumes are rigged meshes.
            const LLMeshSkinInfo* skin = static_cast<LLVOVolume*>(object)->getSkinInfo();
            if (skin)
            {
                sEyeAttachmentSkins.push_back({ skin, avatar });
            }
            return;
        }
        const LLDrawable* drawable = object->mDrawable.get();
        if (!drawable)
        {
            return;
        }
        // registerFace's choice: the world matrix of an animated child, else
        // the render matrix (a root's world matrix, or its parent's, which is
        // the root of this object's link set: added as the root itself).
        sEyeMatrices.push_back({ &drawable->getWorldMatrix(), avatar });
        if (&drawable->getRenderMatrix() != &drawable->getWorldMatrix())
        {
            sEyeMatrices.push_back({ &drawable->getRenderMatrix(), avatar });
        }
    }

    // Whether face is of an object worn on an eye attachment point.
    bool wornOnEyePoint(const LLFace* face)
    {
        LLViewerObject* object = face->getViewerObject();
        LLViewerObject* root = object ? object->getRootEdit() : nullptr;
        if (!root || !root->isAttachment())
        {
            return false;
        }
        const S32 point = ATTACHMENT_ID_FROM_STATE(root->getAttachmentState());
        for (S32 eye_point : EYE_ATTACHMENT_POINTS)
        {
            if (point == eye_point)
            {
                return true;
            }
        }
        return false;
    }

    // Gathers this pass's eye attachments, of every avatar but animesh and
    // UI ones (whether an avatar is marked is asked when its batches come).
    void gatherEyeAttachments()
    {
        sEyeMatrices.clear();
        sEyeAttachmentSkins.clear();
        for (LLCharacter* character : LLCharacter::sInstances)
        {
            const LLVOAvatar* avatar = static_cast<LLVOAvatar*>(character);
            if (!avatar || avatar->isDead() || avatar->isControlAvatar() || avatar->isUIAvatar())
            {
                continue;
            }
            for (S32 point : EYE_ATTACHMENT_POINTS)
            {
                const auto found = avatar->mAttachmentPoints.find(point);
                if (found == avatar->mAttachmentPoints.end() || !found->second)
                {
                    continue;
                }
                for (const LLPointer<LLViewerObject>& object : found->second->mAttachedObjects)
                {
                    if (object.isNull())
                    {
                        continue;
                    }
                    addEyeObject(object.get(), avatar);
                    for (const LLPointer<LLViewerObject>& child : object->getChildren())
                    {
                        addEyeObject(child.get(), avatar);
                    }
                }
            }
        }
    }

    // The avatar wearing on an eye point the rigid object whose batches
    // carry matrix; null when none does.
    const LLVOAvatar* eyeAttachmentWearer(const LLMatrix4* matrix)
    {
        if (!matrix)
        {
            return nullptr;
        }
        for (const EyeMatrix& eye : sEyeMatrices)
        {
            if (eye.matrix == matrix)
            {
                return eye.avatar;
            }
        }
        return nullptr;
    }

    // Whether avatar wears a rigged mesh of skin on an eye point.
    bool isEyeAttachmentSkin(const LLVOAvatar* avatar, const LLMeshSkinInfo* skin)
    {
        if (!skin)
        {
            return false;
        }
        for (const EyeAttachmentSkin& eye : sEyeAttachmentSkins)
        {
            if (eye.skin == skin && eye.avatar == avatar)
            {
                return true;
            }
        }
        return false;
    }

    // What a batch of an avatar shows, at any distance: by its texture (a GLTF
    // material's base color texture in place of the face's), one of the
    // avatar's skin bakes, or the avatar's eyes (the eyes bake, a mesh weighted
    // to the eyes or one worn on an eye point). Neither for a batch of no avatar.
    struct BatchKind
    {
        ETextureIndex skin_bake = TEX_NUM_INDICES;  // TEX_NUM_INDICES: not skin
        bool eyes = false;
    };

    BatchKind batchKind(const LLDrawInfo& params)
    {
        BatchKind kind;
        const LLVOAvatar* avatar = params.mAvatar.get();
        if (!avatar)
        {
            return kind;
        }
        const LLViewerTexture* texture = params.mGLTFMaterial.notNull() ? params.mGLTFMaterial->mBaseColorTexture.get()
                                                                         : params.mTexture.get();
        kind.skin_bake = texture ? skinBakeOf(avatar, texture) : TEX_NUM_INDICES;
        kind.eyes = kind.skin_bake == TEX_NUM_INDICES &&
                    ((texture && isBakeOf(avatar, TEX_EYES_BAKED, texture)) || isEyeSkin(params.mSkinInfo) ||
                     isEyeAttachmentSkin(avatar, params.mSkinInfo));
        return kind;
    }

    // The mark of a batch of an avatar (kind: batchKind's): skin when it shows
    // one of the avatar's skin bakes, eyes when it is the avatar's eyes and the
    // avatar's face is skin, for an avatar marked this pass; 0 otherwise. A
    // batch of no avatar (not rigged) is eyes when it is of an object worn on
    // an eye point, in a face that is skin.
    U32 batchMark(const LLDrawInfo& params, const BatchKind& kind)
    {
        const LLVOAvatar* avatar = params.mAvatar.get();
        if (!avatar)
        {
            const LLVOAvatar* wearer = eyeAttachmentWearer(params.mModelMatrix);
            AvatarState* state = wearer ? markedState(wearer) : nullptr;
            return state && faceIsSkin(*state) ? LLRHI::STENCIL_EYE : 0;
        }
        if (kind.skin_bake != TEX_NUM_INDICES)
        {
            AvatarState* state = markedState(avatar);
            if (!state)
            {
                return 0;
            }
            if (kind.skin_bake == TEX_HEAD_BAKED)
            {
                state->face_skin_frame = gFrameCount;
            }
            return LLRHI::STENCIL_SKIN;
        }
        if (!kind.eyes)
        {
            return 0;
        }
        AvatarState* state = markedState(avatar);
        return state && faceIsSkin(*state) ? LLRHI::STENCIL_EYE : 0;
    }

    // The avatar bit of a batch of avatar: any (not animesh, not a UI
    // avatar), at any distance, while this pass writes the bit.
    U32 avatarBit(const LLVOAvatar* avatar)
    {
        return sAvatarBit && avatar && !avatar->isControlAvatar() && !avatar->isUIAvatar() ? LLRHI::STENCIL_AVATAR : 0;
    }

    // The avatar bit with the bare skin bit, for the avatar's skin or eyes
    // (the system body and eyeballs, or a batch batchKind found to be either).
    U32 avatarSkinBits(const LLVOAvatar* avatar)
    {
        return avatarBit(avatar) ? LLRHI::STENCIL_AVATAR | LLRHI::STENCIL_AVATAR_SKIN : 0;
    }

    // ------------------------------------------------------------------
    // Stencil state (GL calls: the backend records them as dynamic state)

    // The mark (STENCIL_SKIN, STENCIL_EYE, STENCIL_AVATAR, some of them or
    // none) draws that pass the depth test write.
    void setMark(U32 mark)
    {
        gGL.flush();
        glStencilFunc(GL_ALWAYS, mark, sMarkBits);
    }

    void startMarking()
    {
        gGL.flush();
        glStencilMask(0xFF);
        glClearStencil(0);
        LLRHI::clear(GL_STENCIL_BUFFER_BIT);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        glStencilFunc(GL_ALWAYS, 0, sMarkBits);
        glStencilMask(sMarkBits);
    }

    // Back to GL's defaults (the stencil test itself is turned off by GBufferScope's LLGLState).
    void stopMarking()
    {
        gGL.flush();
        glStencilFunc(GL_ALWAYS, 0, 0xFFFFFFFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glStencilMask(0xFFFFFFFF);
    }

    // Whether this G-buffer pass marks skin: the main view, drawn into the
    // main deferred target whose depth has the stencil.
    bool marksPass(const LLCamera& camera)
    {
        return markWanted() && depthHasStencil() && &camera == LLViewerCamera::getInstance() && !gCubeSnapshot &&
               !LLPipeline::sImpostorRender && !LLPipeline::sReflectionRender && gPipeline.mRT == &gPipeline.mMainRT &&
               LLRenderTarget::getCurrentBoundTarget() == &gPipeline.mMainRT.deferredScreen;
    }

    // Once every REPORT_SECONDS while marking: one VKPerf line.
    void reportCounters()
    {
        using Clock = std::chrono::steady_clock;
        const Clock::time_point now = Clock::now();
        ++sCounters.frames;
        if (sReportStart == Clock::time_point())
        {
            sReportStart = now;
            return;
        }
        if (std::chrono::duration<F64>(now - sReportStart).count() < REPORT_SECONDS)
        {
            return;
        }
        // fix-logcache: the counts start over every REPORT_SECONDS either way;
        // written in development starts (LLRHI::sPeriodicLogs) or while the
        // mark is shown (PlusSkinSssDebug). The presets turn PlusSkinFill on,
        // so in a plain start this line came every 5 s all day.
        if (!LLRHI::sPeriodicLogs && !debugWanted())
        {
            sCounters = Counters();
            sReportStart = now;
            return;
        }
        const F32 frames = (F32)sCounters.frames;
        LL_INFOS("VKPerf") << "Skin: " << sCounters.frames << " frames; per frame " << sCounters.skin_batches / frames
                           << " skin batches, " << sCounters.eye_batches / frames << " eye batches, "
                           << sCounters.system_bodies / frames << " system bodies, " << sCounters.system_eyes / frames
                           << " system eyes, " << sCounters.avatar_batches / frames << " avatar batches, "
                           << sCounters.avatars_marked / frames << " avatars marked, " << sCounters.avatars_too_far / frames
                           << " too far (beyond " << sMarkDistance << " m)" << LL_ENDL;
        sCounters = Counters();
        sReportStart = now;
    }

    void init()
    {
        // Settings: app_settings/settings_plus.xml (PlusSkinSssDebug).
        // Check box: skins/default/xui/<lang>/panel_preferences_charis.xml ("c_skin_sss_debug").
        // The depth gets or loses its stencil when the screen buffers are made
        // again: ask for that whenever the mark comes or goes.
        if (LLControlVariable* control = gSavedSettings.getControl(SETTING_DEBUG))
        {
            control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { gResizeScreenTexture = true; });
        }
        // Mark users may want the mark only while a snapshot renders: a snapshot
        // the size of the window does not make the screen buffers again.
        plus::presets::addSnapshotBoostListener([](bool) { plus::skinsss::markWantChanged(); });
    }

    plus::FeatureRegistration sRegistration({ "skinsss", &init, {} });
}

namespace plus::skinsss
{
    bool detail::beginBatch(const LLDrawInfo& params)
    {
        const BatchKind kind = batchKind(params);
        const U32 skin_mark = batchMark(params, kind);
        const LLVOAvatar* avatar = params.mAvatar.get();
        const bool bare = kind.skin_bake != TEX_NUM_INDICES || kind.eyes;
        const U32 mark = skin_mark | (bare ? avatarSkinBits(avatar) : avatarBit(avatar));
        if (!mark)
        {
            return false;
        }
        if (skin_mark)
        {
            ++(skin_mark == LLRHI::STENCIL_SKIN ? sCounters.skin_batches : sCounters.eye_batches);
            ++sSkinDrawsThisPass;
        }
        if (mark & LLRHI::STENCIL_AVATAR)
        {
            ++sCounters.avatar_batches;
        }
        setMark(mark);
        return true;
    }

    void detail::endBatch()
    {
        setMark(0);
    }

    bool detail::isEyeAttachment(const LLDrawInfo& params)
    {
        return params.mAvatar.isNull() && eyeAttachmentWearer(params.mModelMatrix) != nullptr;
    }

    void onScreenBuffers()
    {
        ++sScreenBuffersGeneration;
        sMarkedDepth = MarkedDepth();
        LLRenderTarget& screen = gPipeline.mMainRT.deferredScreen;
        const U32 depth = screen.getDepth();
        if (!markWanted() || !depth)
        {
            return;     // as allocated: the official D24, no stencil
        }
        // GL_DEPTH24_STENCIL8 in place of LLRenderTarget's GL_DEPTH_COMPONENT24,
        // with the same name and size. (Not through LLImageGL::setManualImage:
        // its memory accounting does not know the format; the allocation it
        // recorded for this name stays, at the same size.)
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, depth);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, screen.getWidth(), screen.getHeight(), 0, GL_DEPTH_STENCIL,
                     GL_UNSIGNED_INT_24_8, nullptr);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        sMarkedDepth = { depth, screen.getWidth(), screen.getHeight() };
        static plus::ResizeLog resize_log;     // once a second while the window frame is dragged (fix-round95 D)
        const LLError::ELevel log_level = resize_log.level(screen.getWidth(), screen.getHeight());    // once: LL_VLOGS reads its level twice
        LL_VLOGS(log_level, "SkinSss") << "Main depth with a stencil for the skin mark: " << screen.getWidth() << "x" << screen.getHeight()
                            << LL_ENDL;
    }

    void addMarkUser(bool (*wanted)())
    {
        markUsers().push_back(wanted);
    }

    void addAvatarMarkUser(bool (*wanted)())
    {
        avatarMarkUsers().push_back(wanted);
    }

    void markWantChanged()
    {
        LLRenderTarget& screen = gPipeline.mMainRT.deferredScreen;
        const U32 depth = screen.getDepth();
        const bool wanted = markWanted();
        if (!depth || wanted == depthHasStencil())
        {
            return;
        }
        if (wanted)
        {
            onScreenBuffers();
            return;
        }
        // Back to LLRenderTarget::allocateDepth's format, with the same name and size.
        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE, depth);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, screen.getWidth(), screen.getHeight(), 0, GL_DEPTH_COMPONENT,
                     GL_UNSIGNED_INT, nullptr);
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        sMarkedDepth = MarkedDepth();
        LL_INFOS("SkinSss") << "Main depth back to the official format (no skin mark)" << LL_ENDL;
    }

    bool markedThisFrame()
    {
        return sMarkedFrame == gFrameCount && sSkinDrawsMarked > 0;
    }

    bool markingWanted()
    {
        return markWanted();
    }

    // ==== scene mark plan rule: begin ====
    // research/219 (8.2, the selftest sim-riggedviewer takes these lines):
    // whether the retained scene's draw of a rigged pass's records keeps the
    // marks the plan made at postSort: the pass does not mark, or it marks
    // with the avatar bit as planned.
    bool mark_plan_holds(bool marking_now, bool avatar_bit_now, bool plan_marking, bool plan_avatar_bit)
    {
        return !marking_now || (plan_marking && avatar_bit_now == plan_avatar_bit);
    }
    // ==== scene mark plan rule: end ====

    // research/219 (8.2). The eye attachments gathered here are this frame's
    // (GBufferScope gathers them again for its pass, and clears them after).
    MarkPlan mainViewMark()
    {
        MarkPlan plan;
        plan.marking = markWanted() && depthHasStencil();
        if (plan.marking)
        {
            plan.avatar_bit = avatarBitWanted();
            plan.avatar_mark = plan.avatar_bit ? LLRHI::STENCIL_AVATAR : 0;
            gatherEyeAttachments();
        }
        return plan;
    }

    bool sceneMarkOf(const LLDrawInfo& params, const MarkPlan& plan, U32& mark)
    {
        mark = 0;
        const LLVOAvatar* avatar = params.mAvatar.get();
        if (!avatar)
        {
            return eyeAttachmentWearer(params.mModelMatrix) == nullptr;     // an eye point's object: eyes, per batch
        }
        const BatchKind kind = batchKind(params);
        if (kind.skin_bake != TEX_NUM_INDICES || kind.eyes)
        {
            return false;   // skin or eyes (the bare skin bit too): per batch, by the avatar's distance
        }
        mark = !avatar->isControlAvatar() && !avatar->isUIAvatar() ? plan.avatar_mark : 0;
        return true;
    }

    SceneMarkScope::SceneMarkScope(U32 mark, const MarkPlan& plan)
        : mHolds(mark_plan_holds(detail::sMarking, sAvatarBit, plan.marking, plan.avatar_bit))
    {
        if (mHolds && detail::sMarking && mark)
        {
            setMark(mark);
            mSet = true;
        }
    }

    SceneMarkScope::~SceneMarkScope()
    {
        if (mSet)
        {
            setMark(0);
        }
    }

    bool avatarsMarkedThisFrame()
    {
        return sAvatarMarkedFrame == gFrameCount;
    }

    U32 screenBuffersGeneration()
    {
        return sScreenBuffersGeneration;
    }

    GBufferScope::GBufferScope(const LLCamera& camera)
        : mActive(marksPass(camera)),
          mStencilTest(mActive ? GL_STENCIL_TEST : 0, LLGLState::ENABLED_STATE)
    {
        if (!mActive)
        {
            return;
        }
        if (gFrameCount % FORGET_AFTER_FRAMES == 0)
        {
            forgetOldAvatars();
        }
        sLastAvatar = nullptr;
        sLastSkin = nullptr;
        sSkinDrawsThisPass = 0;
        sAvatarBit = avatarBitWanted();
        sMarkBits = sAvatarBit ? STENCIL_MARKS_AVATAR : STENCIL_MARKS;
        sCameraOrigin = LLViewerCamera::getInstance()->getOrigin();
        // The scatter width on screen: width * [1][1] * height / 2 / view depth,
        // where the projection's [1][1] is 1 / tan(vertical field of view / 2).
        const F32 half_fov_tan = tanf(0.5f * LLViewerCamera::getInstance()->getView());
        const F32 half_height = 0.5f * (F32)gPipeline.mMainRT.deferredScreen.getHeight();
        const F32 pixels_at_1m = half_fov_tan > 0.f ? SCATTER_WIDTH * half_height / half_fov_tan : 0.f;
        sMarkDistance = pixels_at_1m / MIN_SCATTER_PIXELS;
        gatherEyeAttachments();
        startMarking();
        detail::sMarking = true;
    }

    GBufferScope::~GBufferScope()
    {
        if (!mActive)
        {
            return;
        }
        detail::sMarking = false;
        sLastAvatar = nullptr;
        sLastSkin = nullptr;
        sEyeMatrices.clear();           // their pointers hold for this pass only
        sEyeAttachmentSkins.clear();
        stopMarking();
        sMarkedFrame = gFrameCount;
        sSkinDrawsMarked = sSkinDrawsThisPass;
        sAvatarMarkedFrame = sAvatarBit ? gFrameCount : U32_MAX;
        sAvatarBit = false;
        reportCounters();
    }

    SystemBodyScope::SystemBodyScope(const LLVOAvatar* avatar)
        : mMark(detail::sMarking && avatar ? (avatarMarked(avatar) ? LLRHI::STENCIL_SKIN : 0) | avatarSkinBits(avatar) : 0)
    {
        if (mMark & LLRHI::STENCIL_SKIN)
        {
            // The system head is drawn as renderSkinned draws it (not hidden by an alpha).
            if (avatar->isTextureVisible(TEX_HEAD_BAKED))
            {
                markedState(avatar)->face_skin_frame = gFrameCount;
            }
            ++sCounters.system_bodies;
            ++sSkinDrawsThisPass;
        }
        if (mMark)
        {
            setMark(mMark);
        }
    }

    SystemBodyScope::~SystemBodyScope()
    {
        if (mMark)
        {
            setMark(0);
        }
    }

    SystemEyesScope::SystemEyesScope(const LLVOAvatar* avatar)
        : mMark(detail::sMarking && avatar
                    ? (avatarMarked(avatar) && faceIsSkin(*markedState(avatar)) ? LLRHI::STENCIL_EYE : 0) | avatarSkinBits(avatar)
                    : 0)
    {
        if (mMark & LLRHI::STENCIL_EYE)
        {
            ++sCounters.system_eyes;
            ++sSkinDrawsThisPass;
        }
        if (mMark)
        {
            setMark(mMark);
        }
    }

    SystemEyesScope::~SystemEyesScope()
    {
        if (mMark)
        {
            setMark(0);
        }
    }

    OcclusionPause::OcclusionPause()
        : mPaused(detail::sMarking),
          mStencilTest(mPaused ? GL_STENCIL_TEST : 0, LLGLState::DISABLED_STATE)
    {
        detail::sMarking = false;
    }

    OcclusionPause::~OcclusionPause()
    {
        detail::sMarking = mPaused;
    }

    bool splitsBatch(const LLFace* first, const LLFace* next)
    {
        // Texture index batching takes only faces without materials (no
        // normal, specular or GLTF material), whose texture is the face's own.
        if (!backendReady())
        {
            return false;
        }
        const LLTextureEntry* first_te = first->getTextureEntry();
        const LLTextureEntry* next_te = next->getTextureEntry();
        if (first_te && next_te && markOfBakeId(first_te->getID()) != markOfBakeId(next_te->getID()))
        {
            return true;
        }
        // Eyes worn on an eye point and another object (rigged ones of one
        // rig may share a batch; the batch's skin is its first face's).
        return wornOnEyePoint(first) != wornOnEyePoint(next);
    }

    void render()
    {
        if (sMarkedFrame != gFrameCount || !debugWanted() || !backendReady() || gCubeSnapshot ||
            LLRenderTarget::getCurrentBoundTarget() != &gPipeline.mMainRT.screen)
        {
            return;
        }
        LLRHI::gpuMark("skin");
        LLRHI::SkinSssDebug pass;
        for (U32 c = 0; c < 3; ++c)
        {
            pass.color[c] = DEBUG_COLOR[c] * DEBUG_OPACITY;
            pass.eye_color[c] = DEBUG_EYE_COLOR[c] * DEBUG_OPACITY;
        }
        pass.color[3] = DEBUG_OPACITY;
        pass.eye_color[3] = DEBUG_OPACITY;
        gGL.flush();
        LLRHI::sSkinSssDebug(pass);
        LLRHI::gpuMark("lighting");     // the GPU ms line: the rest goes back to its segment
    }
}
