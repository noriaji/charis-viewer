/**
 * @file llgraywire.cpp
 * @brief Wireframe instead of the gray stand-in texture (see llgraywire.h).
 */

#include "llviewerprecompiledheaders.h"

#include "llgraywire.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "llappviewer.h"
#include "llattachmentsmgr.h"   // research/277 A: the agent's attachment requests
#include "lldrawable.h"
#include "lldrawpool.h"         // research/256: the render passes a band draws
#include "llface.h"
#include "llfetchedgltfmaterial.h"
#include "llglslshader.h"
#include "llimagegl.h"
#include "llmeshrepository.h"   // research/277 review L1: gMeshRepo.meshRezEnabled
#include "llprogressview.h"     // research/278: the login curtain's fade
#include "llspatialpartition.h"
#include "llstartup.h"
#include "lltextureentry.h"     // research/284: which faces a texture's alpha mask decides the pool of
#include "lltexturefetch.h"     // research/276: the fetch worker's steps
#include "llviewercamera.h"     // research/277 re-review L-1: the main view's frustum
#include "llviewercontrol.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h" // research/281 review H1: a kept bake, by its ID
#include "llviewerwindow.h"
#include "llvoavatar.h"
#include "llvolume.h"           // research/277 A: an attachment's faces still to be built
#include "llworldpassstate.h"   // gCubeSnapshot
#include "pipeline.h"
#include "plus/materialize/materialize.h"
// <plus> research/299: a texture waiting for a retry is the gray stand-in
#include "plus/texprio/fetchretry.h"
#include "plus/texprio/texprio.h"        // research/302: noteGrayDrawn
// </plus>

namespace
{
    // Per avatar: the frame it was looked at last, and what it found.
    struct AvatarEntry
    {
        U32 checked = 0;
        bool gray = false;
        bool loading = false;   // research/243: the agent's avatar still loading (gray as a whole)
        F32 bakes_since = -1.f; // research/243: since when a body bake is being fetched (<0: none is)
        // research/259: when its band started (research/278: plus::materialize::bandClock; <0: none), and this
        // frame's heights (agent space) of its bottom and top and of its eyes.
        F32 band_start = -1.f;
        F32 band_low = 0.f;
        F32 band_high = 0.f;
        F32 band_eyes = 0.f;
        // research/277 A: since when the avatar, a wireframe as a whole, has had nothing but its attachments
        // to wait for (<0: not in this wireframe yet; counted once per wireframe).
        F32 attachments_since = -1.f;
        // research/277 A (another's): its attachments the region listed (noteListedAttachments): how many, a
        // digest of which (re-review L-a), and the most of them seen attached at once since that list changed.
        U32 listed = 0;
        U32 listed_digest = 0;
        U32 listed_seen = 0;
        bool listed_by_id = false;  // an attachment was found in the list by its ID once (they compare)
        // Review M1, re-review M-1: its shortfall waived (not waited for), after a wait for it was waited
        // out in the main view, until the list changes or more of it are seen attached at once than then
        // (waived_at, the most seen: third review L1, so that one taken off and put on again does not count).
        bool listed_waived = false;
        U32 listed_waived_at = 0;
        // research/281: seen whole once (its wireframe as a whole over, or whole without one: seenWhole), so
        // never a wireframe as a whole again, until the avatar is made again (forgetAvatar); for one never a
        // wireframe, since when its look is known and since when its attachments are all there (<0: not).
        bool materialized = false;
        F32 look_since = -1.f;
        F32 whole_since = -1.f;
        // research/281 review L2: from when (the bands' clock) a worn batch's own textures' flash may show:
        // when it materialized, or the end of the band it started then (what came before was in it).
        F32 own_flash_from = -1.f;
        // research/281 review H1: the bakes whose BOM faces show their old bake while a new one loads
        // (bakeShown; a bit for each LLAvatarAppearanceDefines::EBakedTextureIndex, the new one's ID awaited:
        // re-review L4, so that only its use takes them again), since when (sCurrentTime; <0: none kept), and
        // whether that ran out (BAKE_KEEP_SECONDS; until none is left to keep).
        U32 bakes_kept = 0;
        std::array<LLUUID, LLAvatarAppearanceDefines::BAKED_NUM_INDICES> bake_awaited;
        F32 bake_kept_since = -1.f;
        bool bakes_kept_out = false;
        bool self = false;  // re-review L3: the agent's avatar (isSelf, not asked in ~LLVOAvatar: forgetAvatar)
        // research/289: its bounds (agent space: its spatial extents, with its attachments) as of its last look,
        // for TAA's jitter by a worn batch's wireframe (whose wearer, LLDrawInfo::mWireOwner, is only compared)
        bool bounds_known = false;
        LLVector3 bounds_low;
        LLVector3 bounds_high;
    };
    constexpr U32 CHECK_EVERY_FRAMES = 8;

    std::unordered_map<const LLVOAvatar*, AvatarEntry> sAvatars;
    U32 sGrayAvatars = 0;   // entries with gray set: 0 keeps every batch from asking
    // research/281 review L1: whether the agent's avatar is a wireframe as a whole (its entry's gray), for
    // drawsLoading, which any thread may ask (not the table).
    std::atomic<bool> sSelfWire{ false };

    F32 sAvatarBandsUntil = 0.f;    // research/259: the end of the last avatar band; before it no batch asks

    constexpr U32 HOLD_FRAMES = 2 * CHECK_EVERY_FRAMES;

    // What a walk over an avatar's attachments does with each face's image: looks for a gray one (SCAN);
    // holds it on the GL path while the avatar is a wireframe (HOLD_WIRE: LLImageGL::wireHeld, where
    // LLTexUnit::directTexture binds it to its unit, for the look on unit 0); or (research/259) while its
    // band sweeps it, until `band_until` (HOLD_BAND: LLImageGL::bandHeld, which only routes its batches to
    // drawBatch: their loaded textures go by their numbers there, mat-fix), letting go of the wireframe's.
    // research/277 A: or looks for a face still to come (PENDING: facePending; its image is not looked at).
    // research/277 review L4: HOLD_BAND_KEEP_WIRE, the band's hold that keeps an image's wireframe hold (a
    // batch made during a band while another avatar is a wireframe: it may wear the same image, and its
    // lines would show the image, not the look, until its next HOLD_WIRE).
    // research/281 review L3: PEEK, as SCAN without asking isGray (which keeps the waits): whether a face's
    // texture has an image without its GL name, for PlusMaterializeLog's count only.
    enum class Walk : U8 { SCAN, HOLD_WIRE, HOLD_BAND, HOLD_BAND_KEEP_WIRE, PENDING, PEEK };

    void holdImage(LLViewerTexture* texture, Walk walk, F32 band_until)
    {
        LLImageGL* image = texture ? texture->getGLTexture() : nullptr;
        if (!image)
        {
            return;
        }
        if (walk == Walk::HOLD_BAND || walk == Walk::HOLD_BAND_KEEP_WIRE)
        {
            image->mBandHold = llmax(image->mBandHold, band_until);
            if (walk == Walk::HOLD_BAND)
            {
                image->mWireHold = 0;   // still held on the GL path (heldOnGL): a wireframe owner holds it again
            }
        }
        else
        {
            image->mWireHold = LLImageGL::sFrameCount + HOLD_FRAMES;
        }
    }

    // Whether an attachment's texture makes its avatar a wireframe as a whole: gray past the time grace,
    // as before the grace by state (research/276 leaves the avatar's wireframe and band, research/259, as
    // they were: a texture that comes at once is a wireframe on its own batch only).
    bool grayForAvatar(LLViewerTexture* texture)
    {
        return GrayWire::isGray(texture) &&
               LLViewerTexture::sCurrentTime - texture->mGrayWireSince >= GrayWire::GRACE_SECONDS;
    }

    // The texture a face's batch is drawn gray with while it loads, as visitDrawnTextures takes a batch's:
    // its diffuse texture; research/277 B: a PBR face's base colour (the face's diffuse is null then, unless
    // it shows media), with the avatar band on (off: the diffuse only, as before).
    LLViewerTexture* drawnTexture(const LLFace* face)
    {
        LLViewerTexture* texture = face->getTexture();
        if (!texture && plus::materialize::avatarOn())
        {
            texture = face->getTexture(LLRender::BASECOLOR_MAP);
        }
        return texture;
    }

    // research/277 A: whether a face of an attachment is still to come: its PBR material being fetched
    // (drawn white until it comes), or its shape: the volume has the face, with vertices, but the face has
    // none yet (its mesh came, its rebuild has not: a frame or a few). A face past the volume's faces, or
    // one empty at this level of detail, never gets one (LLVOVolume::updateFaceSize); review L1: nor a mesh
    // that failed to load, or any while meshes are not rezzed (rebuildGeom does not build them; as
    // check_object_for_mesh_loading, they are not waited for).
    bool facePending(const LLViewerObject* object, S32 index, const LLFace* face)
    {
        const LLTextureEntry* te = face->getTextureEntry();
        const LLGLTFMaterial* material = te ? te->getGLTFMaterial() : nullptr;
        if (material && static_cast<const LLFetchedGLTFMaterial*>(material)->isFetching())
        {
            return true;
        }
        if (face->hasGeometry())
        {
            return false;
        }
        const LLVolume* volume = object->getVolume();
        if (!volume || (object->isMesh() && (volume->isMeshAssetUnavaliable() || !gMeshRepo.meshRezEnabled())))
        {
            return false;
        }
        // Review L3: with indices too (a face with vertices and no indices draws nothing: not waited for).
        // research/281 (280's R1): and a triangle's vertices at least: an empty submesh at this level of detail
        // (NoGeometry) is one vertex and one triangle on it (LLVolume::unpackVolumeFacesInternal).
        return index < volume->getNumVolumeFaces() && volume->getVolumeFace(index).mNumVertices >= 3 &&
               volume->getVolumeFace(index).mNumIndices > 0;
    }

    // research/281 (280's R1): whether a face with a shape is never seen: an empty submesh at this level of
    // detail (NoGeometry: its volume face has under a triangle's vertices; review M2: LLFace::genVolumeBBoxes
    // leaves such a face's bounds as they were, so its volume face is asked first), or one whose bounds are a
    // point (LLVOVolume::updateTextureVirtualSize skips such a face: its texture is never fetched while so). It
    // does not make its avatar a wireframe, nor is it held. With the avatar band on (off: as before).
    bool unseenFace(const LLVolume* volume, S32 index, const LLFace* face)
    {
        const bool empty = volume && index < volume->getNumVolumeFaces() && volume->getVolumeFace(index).mNumVertices < 3;
        return (empty || face->mExtents[0].equals3(face->mExtents[1])) && plus::materialize::avatarOn();
    }

    // research/281 review L3: whether a texture has an image without its GL name, read only (GrayWire::isGray
    // keeps its wait's timers).
    bool unloaded(const LLViewerTexture* texture)
    {
        const LLImageGL* image = texture ? texture->getGLTexture() : nullptr;
        return image && !image->getTexName() && !texture->isMissingAsset() &&
               (texture->getType() == LLViewerTexture::FETCHED_TEXTURE || texture->getType() == LLViewerTexture::LOD_TEXTURE);
    }

    // Walks the faces of object and its linkset children. SCAN: true at the first gray texture. A hold:
    // holds every face's image (holdImage), false. PENDING: true at the first face still to come. PEEK: true at
    // the first texture without its GL name (read only).
    bool walkObject(const LLViewerObject* object, S32 depth, Walk walk, F32 band_until)
    {
        if (!object)
        {
            return false;
        }
        const LLDrawable* drawable = object->mDrawable.get();
        if (drawable)
        {
            const LLVolume* volume = object->getVolume();
            for (S32 i = 0, n = drawable->getNumFaces(); i < n; ++i)
            {
                LLFace* face = drawable->getFace(i);
                if (!face)
                {
                    continue;
                }
                if (walk == Walk::PENDING)
                {
                    if (facePending(object, i, face))
                    {
                        return true;
                    }
                    continue;
                }
                if (!face->hasGeometry() || unseenFace(volume, i, face))
                {
                    continue;
                }
                LLViewerTexture* texture = drawnTexture(face);
                if (walk == Walk::PEEK)
                {
                    if (unloaded(texture))
                    {
                        return true;
                    }
                }
                else if (walk != Walk::SCAN)
                {
                    holdImage(texture, walk, band_until);
                }
                else if (grayForAvatar(texture))
                {
                    return true;
                }
            }
        }
        if (depth < 4)  // linkset children; the depth is only a guard
        {
            for (const LLPointer<LLViewerObject>& child : object->getChildren())
            {
                if (walkObject(child.get(), depth + 1, walk, band_until))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // SCAN: whether an attachment (not a HUD) has a gray texture. A hold: holds every attachment's images.
    // PENDING: whether an attachment has a face still to come.
    bool scanAttachments(const LLVOAvatar* avatar, Walk walk, F32 band_until = 0.f)
    {
        for (const auto& [index, attachment] : avatar->mAttachmentPoints)
        {
            if (!attachment || attachment->getIsHUDAttachment())
            {
                continue;
            }
            for (const LLPointer<LLViewerObject>& object : attachment->mAttachedObjects)
            {
                if (walkObject(object.get(), 0, walk, band_until))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // research/277 A: whether a point the region lists an attachment on is one its avatar can be seen to
    // wear: one it has (one it does not have cannot be filled), not a HUD's (review: none of a HUD is sent
    // to another viewer). The point without the add flag (ATTACHMENT_ADD).
    bool listedPoint(const LLVOAvatar* avatar, S32 point)
    {
        const auto it = avatar->mAttachmentPoints.find(point & ~(S32)ATTACHMENT_ADD);
        return it != avatar->mAttachmentPoints.end() && it->second && !it->second->getIsHUDAttachment();
    }

    // research/277 A: another avatar's attachments as the region listed them (LLVOAvatar::mSimAttachments,
    // with its last appearance), on its listed points (listedPoint), against those of them attached (re-
    // review L-a: found in the list by their inventory item or object ID, on a listed point (review L1), so
    // that one off the list, an old outfit's, does not stand in for one that is still coming; all attached
    // ones until one was ever found so, should the list's IDs be of another kind), into its entry: how many
    // are listed, a digest of which (the XOR of LLUUID::getCRC32, which adds up an ID's four words: no
    // allocation), and the most of them seen attached at once since that list changed (re-review L-a: its
    // contents, not only its count: a change of outfit with as many attachments starts again; review L2:
    // not lowered when the IDs are first found to compare). The list is not sent again when an attachment
    // is only taken off (review): one missing after all of them were there is gone, not coming. A waived
    // shortfall (waitsForAttachments) is waited for again once more of the list are seen attached at once
    // than when it was waived (re-review M-1: they do come), or the list changes. Every look
    // at the avatar (with the avatar band on), from its first, waiting or not (the most seen is over time):
    // the loops are over its listed attachments and its attachment points.
    void noteListedAttachments(const LLVOAvatar* avatar, AvatarEntry& entry)
    {
        const std::map<LLUUID, S32>& list = avatar->mSimAttachments;
        U32 listed = 0;
        U32 digest = 0;
        for (const auto& [id, point] : list)
        {
            if (listedPoint(avatar, point))
            {
                ++listed;
                digest ^= id.getCRC32();
            }
        }
        U32 found = 0;
        U32 present = 0;
        for (const auto& [index, attachment] : avatar->mAttachmentPoints)
        {
            if (!attachment || attachment->getIsHUDAttachment())
            {
                continue;
            }
            for (const LLPointer<LLViewerObject>& object : attachment->mAttachedObjects)
            {
                if (!object)
                {
                    continue;
                }
                ++present;
                auto it = list.find(object->getAttachmentItemID());
                if (it == list.end())
                {
                    it = list.find(object->getID());
                }
                if (it != list.end() && listedPoint(avatar, it->second))
                {
                    ++found;
                }
            }
        }
        entry.listed_by_id = entry.listed_by_id || found > 0;
        if (listed != entry.listed || digest != entry.listed_digest)
        {
            entry.listed = listed;
            entry.listed_digest = digest;
            entry.listed_seen = 0;
            entry.listed_waived = false;
        }
        entry.listed_seen = llmax(entry.listed_seen, entry.listed_by_id ? found : present);
        if (entry.listed_waived && entry.listed_seen > entry.listed_waived_at)
        {
            entry.listed_waived = false;    // re-review M-1: more of it came after all: it is waited for again
        }
    }

    // research/277 A: whether an avatar's attachments are still coming: one waiting to attach or for its
    // mesh (HUDs too, as the official waits), a face for its shape or material (Walk::PENDING); another's
    // not all seen yet of those the region lists (noteListedAttachments: the attachments part of
    // LLVOAvatar::getRezzedStatus' 4, not its bakes at full resolution, which a distant avatar never
    // fetches); the agent's requests not all answered (LLAttachmentsMgr: requested, being detached, their
    // outfit links), which its isFullyLoaded does not wait for.
    bool attachmentsComing(LLVOAvatar* avatar, const AvatarEntry& entry)
    {
        if (!avatar->mPendingAttachment.empty() || avatar->hasPendingAttachedMeshes())
        {
            return true;
        }
        if (avatar->isSelf() ? !LLAttachmentsMgr::instance().isAttachmentStateComplete()
                             : !entry.listed_waived && entry.listed_seen < entry.listed)
        {
            return true;
        }
        return scanAttachments(avatar, Walk::PENDING);
    }

    // research/243: the switch for the loading avatars (PlusAvatarWireframeLoading).
    bool loadingOn()
    {
        static LLCachedControl<bool> on(gSavedSettings, "PlusAvatarWireframeLoading", true);
        return on;
    }

    // The body's bakes (the head, upper and lower body's: those every avatar has).
    constexpr LLAvatarAppearanceDefines::ETextureIndex BODY_BAKES[] = {
        LLAvatarAppearanceDefines::TEX_HEAD_BAKED, LLAvatarAppearanceDefines::TEX_UPPER_BAKED,
        LLAvatarAppearanceDefines::TEX_LOWER_BAKED };

    // research/243: whether a body bake the avatar has (not the default nor
    // invisible: those are never fetched) is being fetched and has no GL image
    // yet. The body meshes are drawn with the bakes; until one comes the avatar
    // is a wireframe. Not GrayWire::isGray: its timers need a call every
    // frame (the body draw does not ask), this runs every CHECK_EVERY_FRAMES;
    // a bake nothing fetches (failed, given up) does not keep the wireframe.
    bool bakesHaveGray(const LLVOAvatar* avatar)
    {
        using namespace LLAvatarAppearanceDefines;
        for (const ETextureIndex index : BODY_BAKES)
        {
            if (!avatar->isTextureDefined(index))
            {
                continue;
            }
            const LLViewerTexture* bake = avatar->getTEImage((U8)index);
            if (!bake || bake->getID() == IMG_INVISIBLE ||
                (bake->getType() != LLViewerTexture::FETCHED_TEXTURE && bake->getType() != LLViewerTexture::LOD_TEXTURE))
            {
                continue;
            }
            const LLImageGL* image = bake->getGLTexture();
            const LLViewerFetchedTexture* fetched = static_cast<const LLViewerFetchedTexture*>(bake);
            if (image && !image->getTexName() && !fetched->isMissingAsset() && fetched->hasFetcher())
            {
                return true;
            }
        }
        return false;
    }

    // Whether the wait for a texture's GL image, `waited` seconds long, is given up (it is the gray
    // stand-in again): after GIVE_UP_SECONDS, research/243: FETCHED_GIVE_UP_SECONDS while still fetched.
    bool givenUp(const LLViewerTexture* texture, F32 waited)
    {
        // <plus> research/299: it waits for a retry of a failed fetch: the gray stand-in, as one flagged missing
        // (the cast moved up from below)
        const LLViewerFetchedTexture* fetched = static_cast<const LLViewerFetchedTexture*>(texture);
        if (plus::texprio::fetchretry::waiting(fetched))
        {
            return true;
        }
        // </plus>
        if (waited < GrayWire::GIVE_UP_SECONDS)
        {
            return false;
        }
        return !(fetched->hasFetcher() && waited < GrayWire::FETCHED_GIVE_UP_SECONDS);
    }

    // research/276: a texture whose image is about to come (comingSoon) stays the gray stand-in for
    // SOON_SECONDS of its wait, and at least SOON_FRAMES frames (at a low frame rate the fast cache and
    // the creation, done once a frame within a budget, get as many turns), but never past
    // SOON_MAX_SECONDS (a bound on stale states and stalls: the fetch state is as of the texture's last
    // updateFetch; at worst it is the time grace). Review low 1: in seconds first, not in frames (at a high
    // frame rate four frames are over before a fast cache lookup out of budget or a creation queue is).
    constexpr F32 SOON_SECONDS = 0.1f;
    constexpr U32 SOON_FRAMES = 4;
    constexpr F32 SOON_MAX_SECONDS = GrayWire::GRACE_SECONDS;

    // Whether the GL image of a texture without one is about to come (within SOON_SECONDS as a rule: the
    // fast cache's lookups and the creations run every frame): it is looked up in the fast cache (every
    // new texture is, first; one found there is decoded and waiting for its creation the frame after),
    // decoded (its fetch worker's step, LLTextureFetch::isDecodeStep), or decoded and waiting for its
    // creation (also while it is made on another thread). Else it waits for the network (review medium 1:
    // also while its worker looks in the disk cache, which holds next to nothing the fast cache does not:
    // a texture missing from the fast cache comes from the network as a rule), or for its request.
    bool comingSoon(LLViewerTexture* texture)
    {
        LLViewerFetchedTexture* fetched = static_cast<LLViewerFetchedTexture*>(texture);
        return fetched->isInFastCacheList() || fetched->needsCreateTexture() || LLTextureFetch::isDecodeStep(fetched->fetchState());
    }

    // Whether a texture without a GL image, `waited` seconds into its wait (not given up), is drawn as a
    // wireframe now: past the grace. Without PlusMaterialize the grace is a time (GRACE_SECONDS, as it
    // was). research/276: with it, the texture's state: once a Scope drew it as one in this wait, it
    // stays one (a fetched image is decoded after that: not the gray stand-in again); until then it is
    // one from the first frame, unless its image is about to come (the gray stand-in a moment, above).
    bool wireNow(LLViewerTexture* texture, F32 waited)
    {
        if (!plus::materialize::on())
        {
            return waited >= GrayWire::GRACE_SECONDS;
        }
        if (texture->mGrayWireShown >= 0.f || waited >= SOON_MAX_SECONDS)
        {
            return true;
        }
        const bool soon_over = waited >= SOON_SECONDS && gFrameCount - texture->mGrayWireFrame >= SOON_FRAMES;
        return soon_over || !comingSoon(texture);
    }

    // A texture's mGrayWireSeen (last asked) and mGrayWireShown (last drawn as a wireframe) older than this
    // (research/278 review M1: on the bands' clock, as they are noted, not the frame time: a hitch of more than
    // a second, after which the textures decoded meanwhile come at once, does not make their waits stale)
    // tell a wait that is over (not drawn: started again) or a wireframe not seen lately (no band).
    constexpr F32 STALE_SECONDS = 1.f;

    bool stale(F32 noted_at)
    {
        return plus::materialize::bandClock() - noted_at > STALE_SECONDS;
    }

    // research/276: whether a Scope drew a texture waiting for its image as a wireframe in this wait,
    // lately (not stale), and it still is one (not given up, not missing; `now`: the frame time): where its
    // band starts when its image comes (onImageCreated), and where onImageScheduled holds it.
    bool drawnAsWire(const LLViewerTexture* texture, F32 now)
    {
        const F32 since = texture->mGrayWireSince;
        return since >= 0.f && texture->mGrayWireShown >= 0.f && !stale(texture->mGrayWireShown) &&
               !texture->isMissingAsset() && !givenUp(texture, now - since);
    }

    // The textures of a batch that are drawn gray while they load, in grayTexture's order: its diffuse
    // texture; research/243: a GLTF batch's base colour (it has no diffuse one, only a media one); each of
    // its batch list. visit(texture) (texture may be null) returns true to stop; so does this then.
    template <typename Visit>
    bool visitDrawnTextures(const LLDrawInfo& params, Visit&& visit)
    {
        if (visit(params.mTexture.get()))
        {
            return true;
        }
        if (!params.mTexture && params.mGLTFMaterial.notNull() && visit(params.mGLTFMaterial->mBaseColorTexture.get()))
        {
            return true;
        }
        if (params.mTextureList.size() > 1)
        {
            for (const LLPointer<LLViewerTexture>& texture : params.mTextureList)
            {
                if (visit(texture.get()))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // Where a wireframe is drawn: not the whole-scene wireframe (it is one already), not the shadow and
    // reflection passes, nor the cube snapshots (research/243 review L1: in deferred the water mirror,
    // the avatar reflections and the probes draw there, not with sReflectionRender). research/281 (280's R3):
    // with the avatar band on, nor an avatar's impostor (a wireframe taken into it stayed until the impostor
    // was taken again, which the end of the wireframe does not ask for; the band is not drawn there either).
    bool wirePass()
    {
        return !gUseWireframe && !LLPipeline::sShadowRender && !LLPipeline::sReflectionRender && !gCubeSnapshot &&
               !(LLPipeline::sImpostorRender && plus::materialize::avatarOn());
    }

    // The main view's scene: where a wireframe is drawn, not the HUD (a band, TAA's jitter and
    // PlusMaterializeLog's counts of draws are of it).
    bool mainScene()
    {
        return wirePass() && !LLPipeline::sRenderingHUDs;
    }

    // <plus>
    // research/304: a texture of a drawn batch with an image object and no GL name is drawn as the gray stand-in
    // (whatever isGray says: a wait given up is drawn so too): noted for plus/texprio, which asks for its first level
    void stampDrawn(LLViewerTexture* texture)
    {
        LLImageGL* image = texture ? texture->getGLTexture() : nullptr;
        if (image && !image->getTexName() && (texture->getType() == LLViewerTexture::FETCHED_TEXTURE || texture->getType() == LLViewerTexture::LOD_TEXTURE))
        {
            texture->mGrayWireDrawnAt = LLViewerTexture::sCurrentTime;
        }
    }
    // </plus>

    // The first of a batch's textures (visitDrawnTextures' order) that is gray now (GrayWire::isGray), or
    // null. research/278 (275's 9): every one of them is asked, not only up to the first gray one (the others'
    // waits went stale after a second and started again, never noted as drawn: when the first came, they
    // came after a short wait or none drawn, without a band); a loaded batch's are all asked anyway.
    // research/276: the Scope then draws a wireframe (research/289: it asks only where one is drawn,
    // wirePass), so each gray one is noted as drawn as one.
    LLViewerTexture* grayTexture(const LLDrawInfo& params)
    {
        LLViewerTexture* gray = nullptr;
        visitDrawnTextures(params, [&gray](LLViewerTexture* texture)
        {
            stampDrawn(texture);    // <plus> research/304 </plus>
            if (GrayWire::isGray(texture))
            {
                gray = gray ? gray : texture;
                texture->mGrayWireShown = plus::materialize::bandClock();   // research/278 review M1 (stale)
            }
            return false;
        });
        return gray;
    }

    // research/256: the polygon mode GrayWire set (LINE while true; FILL is everyone else's), so that a
    // scope gives back the one before it, not FILL whatever it was.
    bool sLines = false;

    void setLines(bool lines)
    {
        LLRHI::polygonMode(GL_FRONT_AND_BACK, lines ? GL_LINE : GL_FILL);
        sLines = lines;
    }
}

namespace
{
    // The frame a Scope last drew a wireframe in the main view's scene (drawnLately; research/289: with
    // PlusWireframeTaaNearOnly, a near one, noteWire), and for how many frames after.
    std::atomic<U32> sDrawnFrame{ 0 };
    constexpr U32 DRAWN_LATELY_FRAMES = 8;

    // A wireframe drawn now that TAA's jitter waits for (noteWire: the main view's scene).
    void noteDrawn()
    {
        sDrawnFrame.store(gFrameCount ? gFrameCount : 1, std::memory_order_relaxed);
    }

    // Pulsing cyan: the pulse steps through LOOK_STEPS textures of one colour (dim to bright).
    constexpr U32 LOOK_STEPS = 8;
    constexpr F32 PULSE_HZ = 0.7f;
    LLPointer<LLViewerTexture> sLook[LOOK_STEPS];
    // research/256: what the look's textures were made for (plus::materialize::lookKey; 0: not made),
    // and whether that is the Tron look (PlusMaterialize), whose pulse is quicker and brief at its peak.
    U32 sLookKey = 0;
    bool sLookTron = false;
    constexpr F32 TRON_PULSE_HZ = 1.f;
    // Review L6: made again at most every this many frames (a colour dragged in the picker changes
    // every frame; the last one still comes), and the frame they were made in.
    constexpr U32 LOOK_REMAKE_FRAMES = 10;
    U32 sLookMadeFrame = 0;

    // The colour of one step (t: 0 dim .. 1 bright). research/256: the Tron look is the setting's
    // colour from dim to full, lifted toward white at the peak (the bright core of a Tron line).
    void lookColour(F32 t, bool tron, const LLColor3& colour, U8 rgb[3])
    {
        if (tron)
        {
            const F32 level = 0.55f + 0.45f * t;
            const F32 white = 0.35f * t * t;
            for (U32 k = 0; k < 3; ++k)
            {
                const F32 value = colour.mV[k] * level;
                rgb[k] = (U8)ll_round(llclamp(value + (1.f - value) * white, 0.f, 1.f) * 255.f);
            }
            return;
        }
        const F32 level = 0.45f + 0.55f * t;
        rgb[0] = (U8)(level * 40.f);   // 0.1 .. 0.16 red: a cool cyan, not a white
        rgb[1] = (U8)(level * 255.f);
        rgb[2] = 255;
    }

    // One step's texture (4x4, one colour).
    LLPointer<LLViewerTexture> makeLook(U32 step, bool tron, const LLColor3& colour)
    {
        U8 rgb[3];
        lookColour((F32)step / (F32)(LOOK_STEPS - 1), tron, colour, rgb);
        LLPointer<LLImageRaw> raw = new LLImageRaw(4, 4, 3);
        U8* data = raw->getData();
        for (S32 i = 0; i < 4 * 4 * 3; i += 3)
        {
            data[i] = rgb[0];
            data[i + 1] = rgb[1];
            data[i + 2] = rgb[2];
        }
        return LLViewerTextureManager::getLocalTexture(raw.get(), false);
    }

    // Every step for the settings now.
    void makeLooks()
    {
        sLookTron = plus::materialize::on();
        const LLColor3 colour = plus::materialize::colour();
        for (U32 step = 0; step < LOOK_STEPS; ++step)
        {
            sLook[step] = makeLook(step, sLookTron, colour);
        }
        sLookKey = plus::materialize::lookKey();
        sLookMadeFrame = gFrameCount;
    }

    // The pulse now, 0 dim .. 1 bright (the look's step, and the waiting wireframe's glow).
    F32 lookWave()
    {
        const F32 hz = sLookTron ? TRON_PULSE_HZ : PULSE_HZ;
        const F32 wave = 0.5f + 0.5f * sinf(LLViewerTexture::sCurrentTime * hz * F_TWO_PI);
        return sLookTron ? wave * wave : wave;
    }

    LLTexture* look()
    {
        const U32 step = llmin((U32)(lookWave() * LOOK_STEPS), LOOK_STEPS - 1);
        LLPointer<LLViewerTexture>& texture = sLook[step];
        if (texture.isNull())
        {
            // only when init() did not run (GrayWire::init makes them all)
            texture = makeLook(step, sLookTron, plus::materialize::colour());
        }
        return texture.get();
    }
}

namespace GrayWire
{
    // research/282: a sweep's line in agent space: its front goes from low to high along axis (VZ: up; VX
    // or VY: a flat batch's longer side), or the other way for sign -1 (low and high then along -axis).
    struct Sweep
    {
        F32 low = 0.f;
        F32 high = 0.f;
        U32 axis = VZ;
        F32 sign = 1.f;
    };
}

namespace
{
    // research/256: the band.
    // Its half width (in metres, of the batch's length it sweeps), and the flash's.
    constexpr F32 BAND_FRACTION = 0.08f;
    constexpr F32 BAND_MIN = 0.05f;
    constexpr F32 BAND_MAX = 0.75f;
    constexpr F32 FLASH_HALF = 1.f;
    // research/282: a flash (no bounds) takes PlusMaterializeSeconds as a sweep does. Its plane has no
    // normal: s is plane.w plus the shaders' ripple (at most half a half width, MATERIALIZE_RIPPLE in
    // globalF.glsl, in cells of half a metre, drifting with the eye and the time) everywhere. plane.w
    // crosses the ripple, from FLASH_FROM half widths (all wireframe) to FLASH_CROSSED (all surface), in
    // FLASH_CROSS_SECONDS (at most half the band) whatever the band's length (pre-merge review M1: a slow
    // crossing let the ripple drifting with the time turn patches back to the wireframe; re-review L1:
    // the ripple is taken at the eye space position, so while the eye walks or turns in those 0.4 s a
    // few patches may still turn back: a world space ripple needs the shaders reworked), so the wireframe melts into
    // the surface in patches; then on to FLASH_TO (the edge gone: a half width under the ripple) in the
    // rest of the band, the edge's glow fading. Review M2: the value noise keeps near its middle, most of
    // the surface turns at once and lies in the edge's hot core together: the edge at FLASH_EDGE of its
    // strength.
    constexpr F32 FLASH_FROM = 0.5f;
    constexpr F32 FLASH_CROSSED = -0.5f;
    constexpr F32 FLASH_TO = -1.5f;
    constexpr F32 FLASH_CROSS_SECONDS = 0.4f;
    constexpr F32 FLASH_EDGE = 0.5f;
    // research/282: a batch sweeps along its longer horizontal side (a floor, a road, a sidewalk, a flat
    // roof) when its height is both under FLAT_RATIO of that side, at most FLAT_MAX, and under FLAT_ACROSS
    // of its shorter horizontal side (lower than it is wide), or under two of the narrowest half widths
    // (fifth review 3: it would be all edge sweeping up). Review L1: slabs are 0.1 to 0.5 m thick mostly,
    // and 1.5 m for a stage or a skybox floor. Re-review L2 and the reviews after: a batch's bounds take in
    // all the faces of a texture in a spatial group: a row of shop fronts or a fence is long but thin
    // across, so higher than wide; FLAT_MAX for the walls around a square (64 m by 64 m, 6 m high), wide
    // both ways. Up: a wall 10 x 0.3 x 3 m, a row 64 x 1 x 6 m, a fence 30 x 0.1 x 2.5 m or 1.5 m, a hedge
    // 30 x 1 x 1.5 m, the walls of a room 20 x 20 x 6 m, of a square 64 x 64 x 6 or 3 m. Along: a floor
    // 20 x 20 x 0.3 m, a stage 20 x 20 x 1.5 m, a skybox floor 64 x 64 x 1 m, a road 256 x 10 or 4 x 0.5 m,
    // a sidewalk 40 x 2 x 0.3 m, a bridge deck 20 x 3 x 0.5 m, a kerb 20 x 0.3 x 0.2 m, a pipe under 0.1 m
    // thick (one 0.1 m or more thick and square across, or a beam, sweeps up: no harm).
    constexpr F32 FLAT_RATIO = 0.1f;
    constexpr F32 FLAT_MAX = 2.f;
    constexpr F32 FLAT_ACROSS = 1.f;
    // The shaders' ripple at most (in half widths, MATERIALIZE_RIPPLE in globalF.glsl): main review L3,
    // whether a flash's line pass has anything left to draw.
    constexpr F32 RIPPLE = 0.5f;
    // The noise seed (materialize_params.w): the time, wrapped so a float keeps its fraction (the waiting
    // wireframe's, mode 3, which the shaders do not ripple). A band's is the bands' clock since its start
    // (re-review L1: its ripple and its front on one clock, so a hitch does not move the ripple alone: a
    // flash's patches would turn back to the wireframe; research/289: under PlusMaterializeSeconds, never wrapped).
    constexpr F32 NOISE_PERIOD = 1000.f;
    // The image stays held this much after its band's end (research/278: both on the bands' clock now; kept as
    // a guard: a frame's draws must not move to the fast paths just before the end).
    constexpr F32 HOLD_MARGIN = 0.25f;
    // Review M1: at most this long held while its image is made on another thread (onImageScheduled to
    // onImageCreated, which ends it; a creation that never finishes lets go by itself). research/278 re-review
    // L3: on the bands' clock (LLImageGL::createHeld), so that a hitch while it is made does not end it.
    constexpr F32 MADE_ELSEWHERE_HOLD = 5.f;

    // The uniforms (research/256 6b; by name, not among LLShaderMgr's: the programs that do not
    // clip by them do not have them).
    const LLStaticHashedString sPlaneUniform("materialize_plane");
    const LLStaticHashedString sParamsUniform("materialize_params");
    const LLStaticHashedString sColourUniform("materialize_color");

    // Whether a band is set on a shader now (a drawBatch inside one draws once).
    bool sInBand = false;

    // The waiting wireframe's glow (materialize_params.x 3: no discard, the colour times z added to
    // the emissive; the shader branch's contract), for the lines to show at night too: the band
    // edge's strength (its caps for the sky) times the look's pulse, from WIRE_GLOW_MIN of it.
    constexpr F32 WIRE_GLOW_MODE = 3.f;
    constexpr F32 WIRE_GLOW_MIN = 0.55f;
    // Whether the band's uniforms can be set on the program: it declares them, and goes the native
    // shader way (fifth review 2: with LLRHI_NATIVE_SHADER=0, a uniform call the backend refuses falls
    // to glUniform, which writes the program bound in GL, not this one). Else the lines are drawn as
    // before (lit as a surface) and a band's texture shows at once.
    bool takesBand(LLGLSLShader* shader)
    {
        return shader && LLRHI::sNativeShader && shader->mNativeProgram && shader->getUniformLocation(sParamsUniform) >= 0;
    }

    // The program a Scope's glow is set on now (re-review L-1: a Scope inside another one sets its own
    // when the program bound for its draw is another, as in the alpha pool, whose Scope comes before
    // its batch's program is bound).
    LLGLSLShader* sGlowShader = nullptr;

    // Whether a Band may be on here: not inside another band nor a Scope's glow (that Scope has the
    // uniforms), not in the shadow, reflection, cube snapshot and HUD passes, the setting on.
    bool bandHere()
    {
        return !sInBand && !sGlowShader && mainScene() && GrayWire::enabled();
    }

    // The strength the lines glow with now (materialize_params.z of the waiting wireframe, mode 3, and
    // of the band's line pass, mode 2): the band edge's capped strength, pulsing with the look.
    F32 wireGlow(const plus::materialize::Edge& edge)
    {
        return edge.strength * (WIRE_GLOW_MIN + (1.f - WIRE_GLOW_MIN) * lookWave());
    }

    // The render passes whose G-buffer shaders clip by the band (research/256 3b, the shader branch's
    // list): the opaque and alpha masked ones of the simple, fullbright, bump, legacy material and
    // GLTF pools. Not alpha blending, glow, invisible, grass, nor the materials' blend passes.
    bool bandPass(U32 pass)
    {
        switch (pass)
        {
        case LLRenderPass::PASS_SIMPLE:
        case LLRenderPass::PASS_FULLBRIGHT:
        case LLRenderPass::PASS_BUMP:
        case LLRenderPass::PASS_ALPHA_MASK:
        case LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK:
        case LLRenderPass::PASS_MATERIAL:
        case LLRenderPass::PASS_MATERIAL_ALPHA_MASK:
        case LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE:
        case LLRenderPass::PASS_SPECMAP:
        case LLRenderPass::PASS_SPECMAP_MASK:
        case LLRenderPass::PASS_SPECMAP_EMISSIVE:
        case LLRenderPass::PASS_NORMMAP:
        case LLRenderPass::PASS_NORMMAP_MASK:
        case LLRenderPass::PASS_NORMMAP_EMISSIVE:
        case LLRenderPass::PASS_NORMSPEC:
        case LLRenderPass::PASS_NORMSPEC_MASK:
        case LLRenderPass::PASS_NORMSPEC_EMISSIVE:
        case LLRenderPass::PASS_GLTF_PBR:
        case LLRenderPass::PASS_GLTF_PBR_ALPHA_MASK:
            return true;
        default:
            return false;
        }
    }

    // Behind the login or teleport curtain: what comes there is not seen arriving. research/278 (275's 7): not
    // while the login curtain fades out to the world (it is visible until the fade ends: 0.7 s with plus/loginfast,
    // in which the world shows through and what came took no band); the teleport curtain does not fade.
    bool behindCurtain()
    {
        if (LLStartUp::getStartupState() < STATE_STARTED)
        {
            return true;
        }
        if (!gViewerWindow || !gViewerWindow->getShowProgress())
        {
            return false;
        }
        const LLProgressView* progress = gViewerWindow->getProgressView();
        return !(progress && progress->fadingToWorld());
    }

    // The bounds along an axis of agent space (VX, VY or VZ) of a sweeping batch: mExtents through its
    // model matrix (a row vector times LLMatrix4). False when they make no sense.
    bool batchRange(const LLDrawInfo& params, U32 axis, F32& low, F32& high)
    {
        const F32* lo = params.mExtents[0].getF32ptr();
        const F32* hi = params.mExtents[1].getF32ptr();
        low = lo[axis];
        high = hi[axis];
        if (params.mModelMatrix)
        {
            const LLMatrix4& m = *params.mModelMatrix;
            low = high = m.mMatrix[3][axis];
            for (U32 i = 0; i < 3; ++i)
            {
                const F32 a = lo[i] * m.mMatrix[i][axis];
                const F32 b = hi[i] * m.mMatrix[i][axis];
                low += llmin(a, b);
                high += llmax(a, b);
            }
        }
        return std::isfinite(low) && std::isfinite(high) && high >= low;
    }

    // Where agent axis `axis` (VX, VY, VZ; 3: the agent origin) goes in the eye space of the pass
    // (gGLModelView, the view vary_position is in; column major: its column).
    LLVector3 eyeColumn(U32 axis)
    {
        const F32* m = gGLModelView + 4 * axis;
        return LLVector3(m[0], m[1], m[2]);
    }

    // The plane across a sweep's line at `front` (along it, agent space) in the eye space of the pass,
    // as clipPlane is made (llsettingsvo.cpp): s = dot(plane.xyz, eye) + plane.w is how far past the
    // front along the line (up for VZ), times the view's scale (returned: 1 for a rigid view).
    F32 eyePlane(const GrayWire::Sweep& sweep, F32 front, F32 plane[4])
    {
        const LLVector3 along = eyeColumn(sweep.axis);
        const LLVector3 origin = eyeColumn(3);
        const F32 scale = along.length();
        if (!(scale > 0.f))
        {
            plane[0] = plane[1] = plane[2] = 0.f;
            plane[3] = 0.f;
            return 0.f;
        }
        const LLVector3 normal = along * (sweep.sign / scale);
        plane[0] = normal.mV[VX];
        plane[1] = normal.mV[VY];
        plane[2] = normal.mV[VZ];
        plane[3] = -(front * scale + normal * origin);
        return scale;
    }

    // research/282: the eye's place along an axis of agent space, from the same view as eyePlane's
    // (the agent point gGLModelView takes to its origin).
    F32 eyeAlong(U32 axis)
    {
        const LLVector3 along = eyeColumn(axis);
        const F32 scale2 = along.lengthSquared();
        return scale2 > 0.f ? -(along * eyeColumn(3)) / scale2 : 0.f;
    }

    // A batch's slot in a fixed table of `slots` by its address (sKeptLines, sCounted: the addresses are
    // only compared, never read through).
    U32 batchSlot(const LLDrawInfo& params, U32 slots)
    {
        return (U32)((reinterpret_cast<uintptr_t>(&params) / sizeof(LLDrawInfo)) % slots);
    }

    // research/282: the line of a batch's band that is kept to its end, once worked out at its first draw:
    // a flat batch's end nearer the eye (one stands on a floor, and the eye crossing its middle meanwhile
    // would make the front jump), review L3: and a moving one's axis (one turning about Z swaps its X and
    // Y lengths, one tilting goes from flat to upright: the front would jump to another axis). A static
    // upright batch's line does not change and is not kept. Review L2: 1024 slots by the batch's address,
    // each batch in its slot or the next (main review L2: its own older one first, then one whose band is
    // over, else its slot); one that finds neither works its line out again. Main review L1: a batch is
    // known by its address and its geometry (vertex buffer, offset, count), not its address alone: a full
    // rebuild may free a batch and make another at the same address in the same band (textures made in one
    // frame share their start).
    struct KeptLine
    {
        const LLDrawInfo* batch = nullptr;
        const LLVertexBuffer* buffer = nullptr;
        U32 offset = 0;
        U32 count = 0;
        F32 done = -1.f;
        U32 axis = VZ;
        F32 sign = 1.f;

        bool of(const LLDrawInfo& params) const
        {
            return batch == &params && buffer == params.mVertexBuffer.get() && offset == params.mOffset &&
                   count == params.mCount;
        }
    };
    constexpr U32 KEPT_LINE_SLOTS = 1024;
    KeptLine sKeptLines[KEPT_LINE_SLOTS];

    const KeptLine* keptLine(const LLDrawInfo& params, F32 done)
    {
        const U32 slot = batchSlot(params, KEPT_LINE_SLOTS);
        for (U32 k = 0; k < 2; ++k)
        {
            const KeptLine& line = sKeptLines[(slot + k) % KEPT_LINE_SLOTS];
            if (line.of(params) && line.done == done)
            {
                return &line;
            }
        }
        return nullptr;
    }

    void keepLine(const LLDrawInfo& params, F32 done, U32 axis, F32 sign)
    {
        const U32 slot = batchSlot(params, KEPT_LINE_SLOTS);
        KeptLine& first = sKeptLines[slot];
        KeptLine& next = sKeptLines[(slot + 1) % KEPT_LINE_SLOTS];
        const auto over = [](const KeptLine& line)
        {
            return !line.batch || plus::materialize::bandClock() - line.done >= plus::materialize::seconds();
        };
        KeptLine& line = first.batch == &params ? first
                       : next.batch == &params  ? next
                       : over(first) || !over(next) ? first
                                                    : next;
        line = { &params, params.mVertexBuffer.get(), params.mOffset, params.mCount, done, axis, sign };
    }

    // The batch's band as it can be drawn now (its textures' band started at `done`): a sweep along
    // `sweep`, a flash, or none; in `why`, PlusMaterializeLog's count of a draw of it (research/278).
    // research/282: a static batch whose vertices were rewritten in place (review L1: rebuildMesh)
    // dropped mHasExtents, but its mExtents are still those of its last full rebuild (registerFace writes
    // them only then): it sweeps by them. A flat batch (FLAT_RATIO) sweeps along its longer horizontal
    // side, from the end nearer the eye. Bounds that make no sense, and a batch under two of the narrowest
    // half widths every way: a flash.
    GrayWire::BandKind bandNow(const LLDrawInfo& params, F32 done, GrayWire::Sweep& sweep, plus::materialize::Tally& why)
    {
        namespace mat = plus::materialize;
        switch (params.mBandKind)
        {
        case GrayWire::BAND_SWEEP:
        case GrayWire::BAND_SWEEP_MOVING:
            break;
        case GrayWire::BAND_FLASH:
            why = mat::DRAWN_FLASH;
            return GrayWire::BAND_FLASH;
        default:
            return GrayWire::BAND_NONE;
        }
        const bool lost = params.mBandKind == GrayWire::BAND_SWEEP && !params.mHasExtents;
        sweep = GrayWire::Sweep();
        F32 low = 0.f, high = 0.f;
        if (const KeptLine* kept = keptLine(params, done))
        {
            sweep.axis = kept->axis;
            sweep.sign = kept->sign;
            if (!batchRange(params, sweep.axis, low, high))
            {
                why = mat::DRAWN_ODD;
                return GrayWire::BAND_FLASH;
            }
        }
        else
        {
            F32 lows[3], highs[3];
            for (U32 axis = 0; axis < 3; ++axis)
            {
                if (!batchRange(params, axis, lows[axis], highs[axis]))
                {
                    why = mat::DRAWN_ODD;
                    return GrayWire::BAND_FLASH;
                }
            }
            constexpr F32 SHORTEST = 2.f * BAND_MIN;
            const F32 x = highs[VX] - lows[VX];
            const F32 y = highs[VY] - lows[VY];
            const F32 height = highs[VZ] - lows[VZ];
            const bool flat = height < llmax(SHORTEST, llmin(FLAT_RATIO * llmax(x, y), FLAT_MAX)) &&
                              height < llmax(SHORTEST, FLAT_ACROSS * llmin(x, y));
            if (flat && llmax(x, y) < SHORTEST)
            {
                why = mat::DRAWN_ODD;
                return GrayWire::BAND_FLASH;
            }
            sweep.axis = !flat ? VZ : x >= y ? VX : VY;
            low = lows[sweep.axis];
            high = highs[sweep.axis];
            if (flat)
            {
                sweep.sign = eyeAlong(sweep.axis) > 0.5f * (low + high) ? -1.f : 1.f;
            }
            if (flat || params.mBandKind == GrayWire::BAND_SWEEP_MOVING)
            {
                keepLine(params, done, sweep.axis, sweep.sign);
            }
        }
        sweep.low = sweep.sign < 0.f ? -high : low;
        sweep.high = sweep.sign < 0.f ? -low : high;
        why = lost ? mat::DRAWN_LOST : sweep.axis != VZ ? mat::DRAWN_FLAT : mat::DRAWN_SWEEP;
        return GrayWire::BAND_SWEEP;
    }

    // The latest mGrayWireDone of a batch's textures (those grayTexture looks at); < 0: none.
    F32 latestDone(const LLDrawInfo& params)
    {
        F32 done = -1.f;
        visitDrawnTextures(params, [&done](const LLViewerTexture* texture)
        {
            if (texture)
            {
                done = llmax(done, texture->mGrayWireDone);
            }
            return false;
        });
        return done;
    }

    // How far a band is, 0 to 1, that started at `done` (latestDone); 1 or more: none (no band, or past
    // it). research/278: on the bands' clock, as their starts; research/282: a flash as long as a sweep.
    F32 progressFrom(F32 done)
    {
        if (done < 0.f)
        {
            return 1.f;
        }
        return llmax(plus::materialize::bandClock() - done, 0.f) / plus::materialize::seconds();
    }

    // When the band of a batch's textures that is under way now started (latestDone, under
    // PlusMaterializeSeconds ago; research/278: on the bands' clock), whatever the batch takes of it; < 0: none.
    F32 bandStartedAt(const LLDrawInfo& params)
    {
        const F32 done = latestDone(params);
        return done >= 0.f && plus::materialize::bandClock() - done < plus::materialize::seconds() ? done : -1.f;
    }

    // PlusMaterializeLog counts a batch's draws by render pass once a frame for a batch: a drawBatch inside
    // another one (the bump and material pools') asks again, and research/278 review L2: so does the alpha
    // pool's depth pass before its colour pass, batch after batch (renderPostDeferred).
    // The batches counted this frame, by their addresses (a fixed table: one that takes a slot another took
    // this frame is counted again, rarely; the addresses are only compared, never read through), and what as
    // (research/289: a batch unbanded and drawn gray is counted as both).
    constexpr U8 COUNTED_UNBANDED = 1;      // DRAWN_NONE or DRAWN_ALPHA, whichever came first
    // research/289: GRAY_GIVEN_UP, GRAY_GIVEN_UP_IDLE and GRAY_MISSING, a bit each (from 2 up)
    constexpr U8 countedGray(plus::materialize::Tally why)
    {
        return (U8)(2u << (why - plus::materialize::GRAY_GIVEN_UP));
    }
    static_assert(plus::materialize::GRAY_MISSING - plus::materialize::GRAY_GIVEN_UP < 7, "a bit each in Counted::kinds");
    struct Counted
    {
        const LLDrawInfo* batch = nullptr;
        U32 frame = 0;
        U8 kinds = 0;
    };
    constexpr U32 COUNTED_SLOTS = 1024;
    Counted sCounted[COUNTED_SLOTS];

    // Whether the batch is counted as `kind` (COUNTED_*) for the first time this frame; noted so if it is.
    bool firstThisFrame(const LLDrawInfo& params, U8 kind)
    {
        Counted& slot = sCounted[batchSlot(params, COUNTED_SLOTS)];
        if (slot.batch != &params || slot.frame != gFrameCount)
        {
            slot = { &params, gFrameCount, 0 };
        }
        if (slot.kinds & kind)
        {
            return false;
        }
        slot.kinds |= kind;
        return true;
    }

    // research/278: PlusMaterializeLog (asked while plus::materialize::sLogOn): a main view draw of a batch whose
    // textures' band is under way but which takes none (`what`: a pass without one, DRAWN_NONE, or alpha
    // blended, DRAWN_ALPHA), by its render pass.
    void tallyUnbanded(const LLDrawInfo& params, plus::materialize::Tally what)
    {
        if (!mainScene() || bandStartedAt(params) < 0.f || !firstThisFrame(params, COUNTED_UNBANDED))
        {
            return;
        }
        plus::materialize::tallyPass(what, params.mBandPass);
    }

    // research/289 (the integration check's M1): why a texture of a batch is drawn as the gray stand-in for
    // good, as GrayWire::isGray (asked first, by grayTexture) left it: a fetched one without its GL name whose
    // asset is missing (GRAY_MISSING), or whose wait was given up (mGrayWireGivenUp) while it is fetched
    // (GRAY_GIVEN_UP) or, pre-merge review L4, while nothing fetches it (GRAY_GIVEN_UP_IDLE: the VRAM's discard
    // bias or its priority asks for none); else TALLY_COUNT (loaded, still waiting, or not fetched).
    plus::materialize::Tally standIn(const LLViewerTexture* texture)
    {
        namespace mat = plus::materialize;
        const LLImageGL* image = texture ? texture->getGLTexture() : nullptr;
        if (!image || image->getTexName() ||
            (texture->getType() != LLViewerTexture::FETCHED_TEXTURE && texture->getType() != LLViewerTexture::LOD_TEXTURE))
        {
            return mat::TALLY_COUNT;
        }
        if (texture->isMissingAsset())
        {
            return mat::GRAY_MISSING;
        }
        if (!texture->mGrayWireGivenUp)
        {
            return mat::TALLY_COUNT;
        }
        return static_cast<const LLViewerFetchedTexture*>(texture)->hasFetcher() ? mat::GRAY_GIVEN_UP
                                                                                : mat::GRAY_GIVEN_UP_IDLE;
    }

    // research/289: PlusMaterializeLog (asked while plus::materialize::sLogOn): a main view draw of a batch not
    // drawn as a wireframe (its Scope off) with a texture drawn as the gray stand-in for good (standIn), by its
    // render pass, once a frame for a batch and cause.
    void tallyGray(const LLDrawInfo& params)
    {
        if (!mainScene())
        {
            return;
        }
        visitDrawnTextures(params, [&params](const LLViewerTexture* texture)
        {
            const plus::materialize::Tally why = standIn(texture);
            if (why != plus::materialize::TALLY_COUNT && firstThisFrame(params, countedGray(why)))
            {
                plus::materialize::tallyPass(why, params.mBandPass);
                // <plus> research/302: this texture's state, once a window, a few a window
                plus::texprio::noteGrayDrawn(const_cast<LLViewerFetchedTexture*>(static_cast<const LLViewerFetchedTexture*>(texture)),
                                             why == plus::materialize::GRAY_MISSING ? "missing"
                                             : why == plus::materialize::GRAY_GIVEN_UP ? "given up, fetching" : "given up, no fetcher",
                                             &params);
                // </plus>
            }
            return false;
        });
    }

    // research/278: PlusMaterializeLog (asked while plus::materialize::sLogOn): a band started for a draw
    // (`what`), or not for its program (it does not take the band's uniforms: the batch is drawn once, whole).
    void tallyDrawn(bool on, plus::materialize::Tally what)
    {
        plus::materialize::tally(on ? what : plus::materialize::DRAWN_NO_PROGRAM);
    }

    // A sweep's front (s = 0, agent space) `progress` of the way, and its half width in `half`: from a
    // half width under the bottom (all wireframe, past the shaders' ripple of half that) to one and a
    // half over the top (review L3: the edge's glow, a half width under the front plus the ripple, has
    // left the top by then: it does not go out at once).
    F32 sweepFront(F32 low, F32 high, F32 progress, F32& half)
    {
        const F32 height = llmax(high - low, 0.01f);
        half = llclamp(height * BAND_FRACTION, BAND_MIN, BAND_MAX);
        return low - half + progress * (height + 2.5f * half);
    }

    // research/282: a flash's plane.w (in half widths) `progress` of the way: across the ripple in
    // FLASH_CROSS_SECONDS (at most half the band), then the edge fading out in the rest.
    F32 flashFront(F32 progress)
    {
        const F32 cross = llmin(FLASH_CROSS_SECONDS / plus::materialize::seconds(), 0.5f);
        return progress < cross ? FLASH_FROM + (FLASH_CROSSED - FLASH_FROM) * (progress / cross)
                                : FLASH_CROSSED + (FLASH_TO - FLASH_CROSSED) * ((progress - cross) / (1.f - cross));
    }

    // The shape of a band (bandNow's kind and sweep) `progress` of the way: its plane
    // (materialize_plane) and, in band, its half width (y) and the edge's strength (z, from `strength`).
    // Main review L3: whether its line pass may keep anything: a flash's (s is plane.w and the ripple, at
    // most RIPPLE half widths up, everywhere) keeps nothing once plane.w is under -RIPPLE, and is not drawn
    // then (nor holds TAA's jitter, drawnLately). A sweep's to its end.
    bool bandShape(GrayWire::BandKind kind, const GrayWire::Sweep& sweep, F32 progress, F32 strength, F32 plane[4],
                   F32 band[4])
    {
        band[2] = strength;
        if (kind == GrayWire::BAND_SWEEP)
        {
            F32 half = 0.f;
            const F32 front = sweepFront(sweep.low, sweep.high, progress, half);
            band[1] = half * eyePlane(sweep, front, plane);
            // to its end: a part outside its bounds (an avatar's hat over its reach, lost bounds that
            // moved) stays a wireframe till then
            return true;
        }
        // research/282: a flash, the same everywhere but for the ripple (FLASH_FROM .. FLASH_TO)
        const F32 front = flashFront(progress);
        plane[0] = plane[1] = plane[2] = 0.f;
        plane[3] = front * FLASH_HALF;
        band[1] = FLASH_HALF;
        band[2] = strength * FLASH_EDGE;
        return front + RIPPLE > 0.f;
    }

    // research/259: the avatar's band. Its bounds are its joints' heights (agent space, as the
    // skeleton is placed), widened by these for the skin, the hair and the soles around them, and its
    // top (review L2) up to the avatar's last bounds (LLVOAvatar::getLastAnimExtents: its meshes'
    // joints, its rigged meshes' and unrigged attachments' boxes: a hat, raised wings), at most
    // AVATAR_REACH_MAX over its joints (an attachment as big as a building would make the band crawl).
    // Not its bottom: the bounds are padded by a quarter metre, the feet are the bottom.
    constexpr F32 AVATAR_BELOW_FEET = 0.1f;
    constexpr F32 AVATAR_ABOVE_TOP = 0.15f;
    constexpr F32 AVATAR_REACH_MAX = 1.f;

    // The heights of the avatar's bottom and top (its feet, pelvis, wrists, head and skull joints and
    // its bounds: lying, sitting and flying alike, in the world's up) and of its eyes. False without a
    // skeleton.
    bool avatarHeights(const LLVOAvatar* avatar, F32& low, F32& high, F32& eyes)
    {
        LLJoint* const joints[] = { avatar->mPelvisp, avatar->mFootLeftp, avatar->mFootRightp, avatar->mWristLeftp,
                                    avatar->mWristRightp, avatar->mHeadp, avatar->mSkullp };
        F32 joints_low = F32_MAX;
        F32 joints_high = -F32_MAX;
        for (LLJoint* joint : joints)
        {
            if (!joint)
            {
                return false;
            }
            const F32 z = joint->getLastWorldPosition().mV[VZ];
            joints_low = llmin(joints_low, z);
            joints_high = llmax(joints_high, z);
        }
        eyes = joints_high;
        if (avatar->mEyeLeftp && avatar->mEyeRightp)
        {
            eyes = llmax(avatar->mEyeLeftp->getLastWorldPosition().mV[VZ], avatar->mEyeRightp->getLastWorldPosition().mV[VZ]);
        }
        low = joints_low - AVATAR_BELOW_FEET;
        high = joints_high + AVATAR_ABOVE_TOP;
        const F32 bounds_high = avatar->getLastAnimExtents()[1].mV[VZ];
        if (std::isfinite(bounds_high))
        {
            high = llmax(high, llmin(bounds_high, joints_high + AVATAR_REACH_MAX));
        }
        return std::isfinite(low) && std::isfinite(high) && std::isfinite(eyes) && high >= low;
    }

    // The avatar's entry while its band sweeps it now (the setting on), else null.
    const AvatarEntry* bandEntry(const LLVOAvatar* avatar)
    {
        const F32 now = plus::materialize::bandClock();  // research/278: the bands' clock
        if (!avatar || !(sAvatarBandsUntil > now) || !plus::materialize::avatarOn())
        {
            return nullptr;
        }
        const auto it = sAvatars.find(avatar);
        if (it == sAvatars.end() || it->second.band_start < 0.f ||
            now - it->second.band_start >= plus::materialize::seconds())
        {
            return nullptr;
        }
        return &it->second;
    }

    // The avatar's band where it is drawn now (not into an impostor: research/259, no band there):
    // its entry (its heights), and how far it is in progress, 0 to under 1. Null: none.
    const AvatarEntry* avatarBandNow(const LLVOAvatar* avatar, F32& progress)
    {
        const AvatarEntry* entry = LLPipeline::sImpostorRender ? nullptr : bandEntry(avatar);
        if (entry)
        {
            progress = llmax(plus::materialize::bandClock() - entry->band_start, 0.f) / plus::materialize::seconds();
        }
        return entry;
    }

    // Whether the avatar's band sweeps it where it is drawn now (what takes no band stays a wireframe).
    bool bandHereFor(const LLVOAvatar* avatar)
    {
        F32 progress = 0.f;
        return avatarBandNow(avatar, progress) != nullptr;
    }

    // Whether the avatar's band has not reached its eyes yet (they stay a wireframe meanwhile).
    bool eyesUnderBand(const LLVOAvatar* avatar)
    {
        F32 progress = 0.f;
        const AvatarEntry* entry = avatarBandNow(avatar, progress);
        F32 half = 0.f;
        return entry && sweepFront(entry->band_low, entry->band_high, progress, half) < entry->band_eyes;
    }

    // Whether the avatar may take a band at all: the setting on, not the whole-scene wireframe, an avatar
    // of someone (not a UI or animesh one), not an impostor (no band there).
    bool avatarBandAllowed(LLVOAvatar* avatar)
    {
        return plus::materialize::avatarOn() && !gUseWireframe && !avatar->isUIAvatar() && !avatar->isControlAvatar() &&
               !avatar->isImpostor();
    }

    // Whether the avatar is seen in the main view now: its drawable visible after the culling
    // (LLVOAvatar::isVisible: not behind others, within the draw distance; re-review L-1: that is also true
    // of one only the shadow, reflection probe or water passes see, pipeline.cpp's stateSortDrawableGroups)
    // and its bounds in the main camera's frustum: its spatial extents, as the culling tests a bridge's
    // (LLSpatialBridge::setVisible; third review L2: with its attachments, a pair of wings or a wide skirt,
    // not the body's radius only). Main thread, out of the render passes (the main camera is the main
    // view's then).
    bool inMainView(const LLVOAvatar* avatar)
    {
        const LLDrawable* drawable = avatar->mDrawable.get();
        if (!drawable || !avatar->isVisible())
        {
            return false;
        }
        // <plus> research/319: the one box test (also used for the impostor pictures; was the same lines here)
        return avatar->extentsInCameraFrustum();
        // </plus>
    }

    // research/289 (the integration check's L5): TAA's jitter waits for a wireframe (drawnLately) only when it is
    // near (PlusWireframeTaaNearOnly; off: for any one, as before): its bounds' radius over their distance from the
    // eye, over the tangent of half the view's vertical angle (pre-merge review L1: a zoomed-in view, a narrow
    // field of view, shows a far one large), squared: about its share of the view's height, squared. HOLD_COVER
    // or more (at the default 60 degrees, 0.02 of radius over distance squared): a 1 m box within about 6 m, an
    // avatar within 7 m, a 10 m wall within 35 m, the eye inside its bounds; twice as far at half the angle. One
    // drawn so (a waiting one, a band's lines) holds it as before; the many far ones of a loading venue do not
    // (each alone: a worn batch takes its wearer's bounds, an avatar's batches would add up to many times its
    // size). Bounds not known (a batch that keeps none, worn by no avatar looked at): it holds, as before.
    constexpr F32 HOLD_COVER = 0.06f;

    bool nearOnly()
    {
        static LLCachedControl<bool> on(gSavedSettings, "PlusWireframeTaaNearOnly", true);
        return on;
    }

    // The share of the view of bounds from low to high (agent space); HOLD_COVER with the eye inside them.
    F32 boundsCover(const LLVector3& low, const LLVector3& high)
    {
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const F32 radius = 0.5f * (high - low).length();
        const F32 distance = (0.5f * (low + high) - camera->getOrigin()).length();
        // the view's half height at a distance of 1 (llclamp lets a NaN through: the default then)
        const F32 view = camera->getView();
        const F32 half_view = tanf(0.5f * (std::isfinite(view) ? llclamp(view, MIN_FIELD_OF_VIEW, MAX_FIELD_OF_VIEW)
                                                                : DEFAULT_FIELD_OF_VIEW));
        if (!std::isfinite(radius) || !std::isfinite(distance) || distance <= radius)
        {
            return HOLD_COVER;
        }
        const F32 ratio = radius / (distance * half_view);
        return ratio * ratio;
    }

    // An avatar's bounds (agent space: its spatial extents with its attachments, as inMainView's); false
    // without its drawable.
    bool avatarBounds(const LLVOAvatar* avatar, LLVector3& low, LLVector3& high)
    {
        const LLDrawable* drawable = avatar ? avatar->mDrawable.get() : nullptr;
        if (!drawable)
        {
            return false;
        }
        const LLVector4a* extents = drawable->getSpatialExtents();
        low.set(extents[0].getF32ptr());
        high.set(extents[1].getF32ptr());
        return true;
    }

    // Into its entry (updateAvatar, at each look).
    void noteBounds(const LLVOAvatar* avatar, AvatarEntry& entry)
    {
        entry.bounds_known = avatarBounds(avatar, entry.bounds_low, entry.bounds_high);
    }

    // An avatar drawn now (drawAvatar's: it is live).
    F32 avatarCover(const LLVOAvatar* avatar)
    {
        LLVector3 low, high;
        return avatarBounds(avatar, low, high) ? boundsCover(low, high) : HOLD_COVER;
    }

    // A worn batch's, by its wearer's bounds at its last look (noteBounds: the wearer is looked up, not read).
    F32 wearerCover(const LLVOAvatar* wearer)
    {
        const auto it = sAvatars.find(wearer);
        return it != sAvatars.end() && it->second.bounds_known ? boundsCover(it->second.bounds_low, it->second.bounds_high)
                                                               : HOLD_COVER;
    }

    // A batch's: its bounds (those a band sweeps, batchRange), or a worn one's wearer's.
    F32 batchCover(const LLDrawInfo& params)
    {
        if (!params.mHasExtents && params.mBandKind != GrayWire::BAND_SWEEP && params.mBandKind != GrayWire::BAND_SWEEP_MOVING)
        {
            return params.mWireOwner ? wearerCover(params.mWireOwner) : HOLD_COVER;
        }
        LLVector3 low, high;
        for (U32 axis = 0; axis < 3; ++axis)
        {
            if (!batchRange(params, axis, low.mV[axis], high.mV[axis]))
            {
                return HOLD_COVER;
            }
        }
        return boundsCover(low, high);
    }

    // Whether the jitter waits for this frame already (no bounds to work out for the next wireframe).
    bool heldThisFrame()
    {
        return sDrawnFrame.load(std::memory_order_relaxed) == (gFrameCount ? gFrameCount : 1);
    }

    // A wireframe drawn now, of the share of the view cover() works out (only when it is needed): TAA's jitter
    // waits for it in the main view's scene (not the HUD), when it is near (or for any, the setting off).
    template <typename Cover>
    void noteWireBy(Cover&& cover)
    {
        if (mainScene() && (!nearOnly() || (!heldThisFrame() && cover() >= HOLD_COVER)))
        {
            noteDrawn();
        }
    }

    // A batch's (a Scope's, a band's lines), an avatar's.
    void noteWire(const LLDrawInfo& params)
    {
        noteWireBy([&params]() { return batchCover(params); });
    }

    void noteWire(const LLVOAvatar* avatar)
    {
        noteWireBy([avatar]() { return avatarCover(avatar); });
    }

    // research/277 A: an avatar that was a wireframe as a whole waits for its attachments at most this long
    // (from when nothing else kept it one), then its band starts with what it has. The official viewer ends
    // another's first cloud after at most 15 s (FIRST_APPEARANCE_CLOUD_MAX_DELAY); its wait for missing
    // attachments, 60 s (MAX_ATTACHMENT_WAIT_TIME_SEC), is too long to keep a wireframe.
    constexpr F32 ATTACHMENTS_WAIT_SECONDS = 15.f;

    // research/277 A: whether the avatar, a wireframe as a whole until now with nothing else to keep it one,
    // stays one for its attachments (attachmentsComing; its body and attachments then come in one band, not
    // the body first): where its band can follow (also behind the curtain: the band runs if they come after
    // it), for at most ATTACHMENTS_WAIT_SECONDS of this wireframe.
    bool waitsForAttachments(LLVOAvatar* avatar, AvatarEntry& entry)
    {
        if (!entry.gray || !avatarBandAllowed(avatar))
        {
            return false;
        }
        const F32 now = LLViewerTexture::sCurrentTime;
        if (entry.attachments_since < 0.f)
        {
            entry.attachments_since = now;
        }
        if (now - entry.attachments_since >= ATTACHMENTS_WAIT_SECONDS)
        {
            // Review M1: waited out. When fewer of the region's list were seen attached than it lists, seen in
            // the main view (re-review M-1: one not seen may not have been sent all of its attachments yet, nor
            // built their shapes), the shortfall is waived: one listed that never comes, or one not found in
            // the list by its ID (a temporary attachment off the inventory), keeps the next wireframe no
            // more. Not past a change of the list, nor once more of it are seen attached at once than its
            // most so far (noteListedAttachments).
            if (entry.listed_seen < entry.listed && inMainView(avatar))
            {
                entry.listed_waived = true;
                entry.listed_waived_at = entry.listed_seen;
            }
            return false;
        }
        return attachmentsComing(avatar, entry);
    }

    // research/281: whether an avatar's look is known and shown: it is fully loaded (review M1: the official
    // viewer hides another's body and attachments until then, LLDrawPoolAvatar::renderAvatars and
    // LLSpatialBridge::setVisible, waiting up to a minute for its attachments; with RenderUnloadedAvatar, the
    // default, another is fully loaded at once), and another's three body bakes are defined (they come with
    // its appearance, as the region's list of its attachments does).
    bool lookKnown(LLVOAvatar* avatar)
    {
        if (!avatar->isFullyLoaded())
        {
            return false;
        }
        return avatar->isSelf() ||
               std::all_of(std::begin(BODY_BAKES), std::end(BODY_BAKES),
                           [avatar](LLAvatarAppearanceDefines::ETextureIndex index) { return avatar->isTextureDefined(index); });
    }

    // research/281: an avatar never a wireframe as a whole is seen whole once its attachments have all been
    // there this long: twice the time grace of an attachment's texture for its avatar (grayForAvatar), so that
    // one not loaded when its faces were built is gray to the scan by then (a wireframe as a whole and a band,
    // as before), and one that came in that time (from the cache) shows no wireframe and takes no band.
    constexpr F32 WHOLE_SETTLE_SECONDS = 2.f * GrayWire::GRACE_SECONDS;

    // research/281: whether an avatar that is not a wireframe as a whole, and never was one, is seen whole now
    // (materialized as if its band had run): its look known, and its attachments all there (attachmentsComing)
    // for WHOLE_SETTLE_SECONDS, or still coming after ATTACHMENTS_WAIT_SECONDS of a known look. One whose bakes
    // came from the cache before its attachments is not, so that an attachment that comes gray makes it a
    // wireframe and a band as before. Not a UI or animesh avatar (no band).
    bool seenWhole(LLVOAvatar* avatar, AvatarEntry& entry)
    {
        if (avatar->isUIAvatar() || avatar->isControlAvatar() || !lookKnown(avatar))
        {
            entry.look_since = entry.whole_since = -1.f;
            return false;
        }
        const F32 now = LLViewerTexture::sCurrentTime;
        if (entry.look_since < 0.f)
        {
            entry.look_since = now;
        }
        if (now - entry.look_since >= ATTACHMENTS_WAIT_SECONDS)
        {
            return true;
        }
        if (attachmentsComing(avatar, entry))
        {
            entry.whole_since = -1.f;
            return false;
        }
        if (entry.whole_since < 0.f)
        {
            entry.whole_since = now;
        }
        return now - entry.whole_since >= WHOLE_SETTLE_SECONDS;
    }

    // The avatar was a wireframe as a whole until now: its band starts when it may (and there is room
    // for it), its attachments held on the GL path (where drawBatch draws them).
    void startAvatarBand(LLVOAvatar* avatar, AvatarEntry& entry)
    {
        namespace mat = plus::materialize;  // research/278: tally, PlusMaterializeLog's counts
        entry.band_start = -1.f;
        if (!avatarBandAllowed(avatar))
        {
            mat::tally(mat::AVATAR_NOT_ALLOWED);
            return;
        }
        if (behindCurtain())
        {
            mat::tally(mat::AVATAR_CURTAIN);
            return;
        }
        if (!inMainView(avatar))
        {
            // research/277 review L4: one not seen in the main view (out of view, behind others, past the draw
            // distance) would take a place of MAX_AVATAR_BANDS with a band nobody sees (one waited out there for
            // shapes its groups do not build while not seen, many at once after a teleport)
            mat::tally(mat::AVATAR_NOT_SEEN);
            return;
        }
        if (!avatarHeights(avatar, entry.band_low, entry.band_high, entry.band_eyes))
        {
            mat::tally(mat::AVATAR_NO_SKELETON);
            return;
        }
        const F32 now = mat::bandClock();   // research/278: the bands' clock (its holds' too)
        const F32 until = now + mat::seconds();
        if (!mat::admitAvatar(now, until))
        {
            mat::tally(mat::AVATAR_FULL);
            return;     // research/259: too many at once, this one comes without a band
        }
        mat::tally(mat::AVATAR_BAND);
        entry.band_start = now;
        sAvatarBandsUntil = llmax(sAvatarBandsUntil, until);
        // Review M (mat-fix's rule): the band's hold, not the wireframe's (whose images directTexture binds
        // to their units): the band's draws name the loaded textures by their numbers.
        scanAttachments(avatar, Walk::HOLD_BAND, until + HOLD_MARGIN);
    }

    // A batch's textures (visitDrawnTextures') held as a walk holds a face's (holdImage).
    void holdBatch(const LLDrawInfo& info, Walk walk, F32 band_until)
    {
        visitDrawnTextures(info, [walk, band_until](LLViewerTexture* texture)
        {
            holdImage(texture, walk, band_until);
            return false;
        });
    }

    // A batch an avatar wears, made (an attachment that came, a face whose shape came, a batch made again
    // for a new texture) while its wearer is drawn apart, held on the GL path as the walks hold the others,
    // so that its first frame is drawn as theirs, not whole on a fast path: review L2, while the wearer is a
    // wireframe as a whole (HOLD_WIRE, until its next walk takes it); research/277 C, while its band sweeps
    // it (the band's hold to its end; review L4: keeping the wireframe holds while another avatar is one).
    // With the avatar band on (off: as before; a HUD's batch has no wearer then, wireOwner).
    void holdWornBatch(const LLDrawInfo& info)
    {
        const LLVOAvatar* wearer = info.mWireOwner;
        if (!wearer || !plus::materialize::avatarOn())
        {
            return;
        }
        if (GrayWire::avatarIsGray(wearer))
        {
            holdBatch(info, Walk::HOLD_WIRE, 0.f);
            return;
        }
        if (const AvatarEntry* entry = bandEntry(wearer))
        {
            const F32 until = entry->band_start + plus::materialize::seconds() + HOLD_MARGIN;
            holdBatch(info, sGrayAvatars ? Walk::HOLD_BAND_KEEP_WIRE : Walk::HOLD_BAND, until);
        }
    }

    // Every frame of the avatar's band: its heights now (it moves, sits, stands up), or its end.
    void updateAvatarBand(LLVOAvatar* avatar, AvatarEntry& entry)
    {
        if (plus::materialize::bandClock() - entry.band_start >= plus::materialize::seconds() ||
            !plus::materialize::avatarOn())
        {
            entry.band_start = -1.f;
            return;
        }
        F32 low = 0.f, high = 0.f, eyes = 0.f;
        if (avatarHeights(avatar, low, high, eyes))
        {
            entry.band_low = low;
            entry.band_high = high;
            entry.band_eyes = eyes;
        }
    }

    // Whether a texture whose image came now, after a wait from `since`, takes a band (CAME_BAND, if there is
    // room for it: admit), or research/278 (PlusMaterializeLog) why not: the settings, the whole-scene
    // wireframe, research/276: not drawn as a wireframe in this wait within the last second, or given up or
    // missing (drawnAsWire), a wait under BAND_AFTER_SECONDS (shown for a moment), behind the curtain.
    plus::materialize::Tally arrival(const LLViewerTexture* texture, F32 now, F32 since)
    {
        namespace mat = plus::materialize;
        if (!GrayWire::enabled() || !mat::on())
        {
            return mat::CAME_OFF;
        }
        if (gUseWireframe)
        {
            return mat::CAME_WIREFRAME;
        }
        if (!drawnAsWire(texture, now))
        {
            const F32 shown = texture->mGrayWireShown;
            return shown < 0.f || stale(shown) ? mat::CAME_NOT_SHOWN : mat::CAME_GIVEN_UP;
        }
        if (now - since < GrayWire::BAND_AFTER_SECONDS)
        {
            return mat::CAME_QUICK;
        }
        return behindCurtain() ? mat::CAME_CURTAIN : mat::CAME_BAND;
    }

    // A texture's band when its image came after a wait as a wireframe (onImageCreated; research/284: or once its
    // alpha analysis settled), `why` its arrival. Once per wait: the next resolution comes after an image.
    void startBand(LLViewerTexture* texture, LLImageGL* image, plus::materialize::Tally why)
    {
        namespace mat = plus::materialize;
        texture->mGrayWireSince = -1.f;
        if (why != mat::CAME_BAND)
        {
            mat::tally(why);
            return;
        }
        const F32 start = mat::bandClock();  // research/278: its start, end and hold on the bands' clock
        const F32 until = start + mat::seconds();
        if (!mat::admit(start, until))
        {
            mat::tally(mat::CAME_FULL);
            return;     // research/256 section 4: too many at once, this one comes without a band
        }
        mat::tally(mat::CAME_BAND);
        texture->mGrayWireDone = start;
        image->mBandHold = until + HOLD_MARGIN;
    }

    // research/281 review L2: whether a worn batch whose own textures' band started at `done` (the bands'
    // clock) flashes in it: its wearer materialized before (what came before its band's end was in that band,
    // what came under its wireframe as a whole came with it, and one whose band was refused comes as before).
    bool ownFlashAllowed(const LLVOAvatar* wearer, F32 done)
    {
        const auto it = wearer ? sAvatars.find(wearer) : sAvatars.end();
        return it != sAvatars.end() && it->second.materialized && done >= it->second.own_flash_from;
    }

    // research/281 review H1: a materialized avatar's BOM faces show their old bake at most this long while a
    // new one is still coming (bakeShown): as long as the official viewer waits for an avatar's textures at its
    // first load (MAX_TEXTURE_WAIT_TIME_SEC). One clock for the avatar, from its latest change of a bake
    // (pre-merge review L2), not running while it is not seen (L3); a new bake that has not come into use by
    // then is shown as it is, one that failed keeps the old bake for good, as the body does (L4).
    constexpr F32 BAKE_KEEP_SECONDS = 60.f;

    // research/281 re-review L2: whether an object's PBR materials name a bake (a GLTF BOM face: its textures
    // are taken in LLViewerObject::updateTEMaterialTextures, on a rebuild, not by refreshBakeTexture).
    bool namesBakeInMaterial(const LLViewerObject* object)
    {
        for (U8 te = 0, n = object->getNumTEs(); te < n; ++te)
        {
            const LLTextureEntry* entry = object->getTE(te);
            const LLGLTFMaterial* material = entry ? entry->getGLTFRenderMaterial() : nullptr;
            if (material &&
                std::any_of(material->mTextureId.begin(), material->mTextureId.end(), [](const LLUUID& id)
                            { return LLAvatarAppearanceDefines::LLAvatarAppearanceDictionary::isBakedImageId(id); }))
            {
                return true;
            }
        }
        return false;
    }

    // The BOM faces of one object take its avatar's bakes again: its texture entries (refreshBakeTexture), and
    // re-review L2: its PBR materials' with a rebuild of its group's geometry (pre-merge review M1: their base
    // colour is taken in LLVolumeGeometryManager::rebuildGeom, updateTEMaterialTextures, only for a group whose
    // geometry is dirty; a drawable's REBUILD_MATERIAL only dirties its mesh: LLViewerObject::dirtySpatialGroup).
    void refreshBakes(LLViewerObject* object)
    {
        object->refreshBakeTexture();
        if (namesBakeInMaterial(object))
        {
            object->dirtySpatialGroup();
        }
    }

    // research/281 review H1: the BOM faces of the avatar's attachments take its bakes again (as
    // LLVOAvatar::updateMeshTextures does it at its end: the attached objects and their children).
    void refreshWornBakes(LLVOAvatar* avatar)
    {
        for (const auto& [index, attachment] : avatar->mAttachmentPoints)
        {
            if (!attachment)
            {
                continue;
            }
            for (const LLPointer<LLViewerObject>& attached : attachment->mAttachedObjects)
            {
                LLViewerObject* object = attached.get();
                if (!object || object->isDead())
                {
                    continue;
                }
                refreshBakes(object);
                for (const LLPointer<LLViewerObject>& linked : object->getChildren())
                {
                    LLViewerObject* child = linked.get();
                    if (child && !child->isDead())
                    {
                        refreshBakes(child);
                    }
                }
            }
        }
    }

    // research/281 re-review L1: the avatar's BOM faces take its bakes as they are, when the old ones kept
    // there are let go of (the settings turned off, or its table about to be emptied): no use of the new
    // one (onBakeUsed) would take them again.
    void releaseKeptBakes(LLVOAvatar* avatar, AvatarEntry& entry)
    {
        if (!entry.bakes_kept && entry.bake_kept_since < 0.f && !entry.bakes_kept_out)
        {
            return;
        }
        entry.bakes_kept = 0;
        entry.bake_kept_since = -1.f;
        entry.bakes_kept_out = false;
        refreshWornBakes(avatar);
    }

    // research/281 pre-merge review L1: the parts whose old bake is kept but whose new one is not awaited any more
    // (a bake no longer defined: a skirt taken off, a BOM part that went; or its texture entry another bake now,
    // its BOM faces not refreshed since) are let go of, so that the time kept does not run out for them.
    void forgetStaleKeeps(const LLVOAvatar* avatar, AvatarEntry& entry)
    {
        using namespace LLAvatarAppearanceDefines;
        for (U32 index = 0; index < entry.bake_awaited.size(); ++index)
        {
            const U32 bit = 1u << index;
            if (!(entry.bakes_kept & bit))
            {
                continue;
            }
            const ETextureIndex texture = LLAvatarAppearance::getDictionary()->bakedToLocalTextureIndex((EBakedTextureIndex)index);
            const LLViewerTexture* now = avatar->isTextureDefined(texture) ? avatar->getTEImage((U8)texture) : nullptr;
            if (!now || now->getID() != entry.bake_awaited[index])
            {
                entry.bakes_kept &= ~bit;
            }
        }
        if (!entry.bakes_kept)
        {
            entry.bake_kept_since = -1.f;
            entry.bakes_kept_out = false;
        }
    }

    // research/281 pre-merge review L4: whether a kept part's new bake may still come (it has not failed: one that
    // failed keeps the old bake, as the system body keeps it; only a stalled one runs out of time).
    bool keptBakeComing(const AvatarEntry& entry)
    {
        for (U32 index = 0; index < entry.bake_awaited.size(); ++index)
        {
            if (!(entry.bakes_kept & (1u << index)))
            {
                continue;
            }
            const LLViewerFetchedTexture* awaited = LLViewerTextureManager::findFetchedTexture(entry.bake_awaited[index], TEX_LIST_STANDARD);
            if (!awaited || !awaited->isMissingAsset())
            {
                return true;
            }
        }
        return false;
    }

    // research/281: a materialized avatar's look (the avatar band on): never a wireframe as a whole again, for
    // any reason (its bakes fetched again after a change of outfit: the body keeps its old bakes until the new
    // ones load, LLVOAvatar::updateMeshTextures and useBakedTexture; an attachment's texture not loaded: a new
    // attachment, a texture put on, a face a level of detail needs first: that batch alone is a wireframe, its
    // Scope's); nor is its band taken back. One left a wireframe as a whole (the setting turned on after it was
    // made one) is not one any more. PlusMaterializeLog: a look that would have made it one counts as KEPT.
    void keepMaterialized(LLVOAvatar* avatar, AvatarEntry& entry, bool loading, bool loading_on)
    {
        if (entry.gray)
        {
            entry.gray = false;
            --sGrayAvatars;
            entry.attachments_since = -1.f;
        }
        entry.bakes_since = -1.f;
        if (entry.bakes_kept)
        {
            forgetStaleKeeps(avatar, entry);    // pre-merge review L1
        }
        if (entry.bake_kept_since >= 0.f && !avatar->isVisible())
        {
            // Pre-merge review L3: not counted while it is not seen (its bakes' loaded callbacks are paused then,
            // LLVOAvatar::updateMeshTextures: their use waits until it is seen again)
            entry.bake_kept_since = LLViewerTexture::sCurrentTime;
        }
        if (!entry.bakes_kept_out && entry.bake_kept_since >= 0.f &&
            LLViewerTexture::sCurrentTime - entry.bake_kept_since >= BAKE_KEEP_SECONDS && keptBakeComing(entry))
        {
            // Review H1: a new bake never came into use (its fetch stalled): its BOM faces take it as it is, until
            // a bake is used with none left to keep (onBakeUsed). Pre-merge review L4: not for parts whose new bake
            // failed (they keep the old one, as the body does).
            entry.bakes_kept_out = true;
            refreshWornBakes(avatar);
        }
        // Review L3: read only (PEEK), so that counting changes no wait
        if (plus::materialize::sLogOn &&
            (loading || (loading_on && bakesHaveGray(avatar)) || scanAttachments(avatar, Walk::PEEK)))
        {
            plus::materialize::tally(plus::materialize::AVATAR_KEPT);
        }
    }
}

namespace
{
    // research/284 (279 1(b)): an image of 64 texels or more made on the main thread has its alpha analysed on
    // another thread, and LLFace::canRenderAsMask (asked where a group is built) takes it as no mask until the
    // analysis comes. The faces of a texture that came, built for its change of components (addToCreateTexture),
    // stayed in the alpha pool (no band, sorted, slow) when nothing built them again: trees, grass, fences and
    // cut-out signs whose alpha is a mask. GrayWire waits for the analyses of the textures made (onImageCreated)
    // and, when one comes out other than their faces were built with (LLViewerFetchedTexture::mMaskBuilt), builds
    // their groups again, in the pool the analysis puts them in. Also an image made on the image thread (analysed
    // there at once, but its faces may be built before postCreateTexture), and a resolution whose analysis tells
    // otherwise than the one before. With or without PlusMaterialize and the wireframe (upstream misses it too).
    bool maskRebuildOn()
    {
        static LLCachedControl<bool> on(gSavedSettings, "PlusAlphaMaskRebuild", true);
        return on;
    }

    // A texture whose analysis GrayWire waits for, or whose faces it may build again.
    struct MaskWatch
    {
        LLPointer<LLViewerFetchedTexture> texture;
        F32 watched_at = 0.f;   // the frame time it came in (MASK_GIVE_UP_SECONDS)
        F32 settled_at = -1.f;  // the frame time its analysis was first seen settled (< 0: it waits); a group
                                // last built before that took the mask from before it (mMaskBuilt, or one not
                                // known), and one built after it took this one
        S8 settled_mask = -1;
        bool band = false;      // its band waits for the analysis (onImageCreated)
    };
    // At most MASK_WATCH_MAX at once (one more is as before: its faces stay in the pool they were built in until
    // something else builds them again); the faces of at most MASK_REBUILDS_PER_FRAME looked at a frame (review L2:
    // those whose analysis settled on another mask than they were built with; the others go at once); one whose
    // analysis waits longer than MASK_GIVE_UP_SECONDS let go.
    constexpr size_t MASK_WATCH_MAX = 1024;
    constexpr U32 MASK_REBUILDS_PER_FRAME = 16;
    constexpr F32 MASK_GIVE_UP_SECONDS = 30.f;
    // research/289 (the integration check's L4): one whose analysis settled on another mask than its faces were
    // built with is taken MASK_GATHER_SECONDS after that (the frame time), so that the textures of a group that
    // settle within that time of each other build it once, not once each (the first one's build comes after the
    // others' settles: rebuildMaskFaces passes a group built since). Its faces stay in the pool they were built
    // in meanwhile. Pre-merge review L1: not one whose band waits for it (MaskWatch::band): its groups are built
    // and its band starts at once, so that its band does not start ahead of them (its hold, MASK_BAND_WAIT, may
    // run out meanwhile) nor its wireframe stay 0.2 s longer; the busy login's textures, behind the curtain,
    // take no band and are still gathered. A group built again
    // under MASK_AGAIN_SECONDS after its last build (as a rule, the build its texture's change of components
    // asked for, addToCreateTexture's dirtyTexture, when the analysis came after it) counts as MASK_AGAIN.
    constexpr F32 MASK_GATHER_SECONDS = 0.2f;
    constexpr F32 MASK_AGAIN_SECONDS = 1.f;
    // A band put off for an analysis waits at most this long (the bands' clock: LLImageGL::mCreateHold, a
    // wireframe meanwhile), then starts all the same (its faces built again when the analysis settles).
    constexpr F32 MASK_BAND_WAIT = 0.5f;
    std::vector<MaskWatch> sMaskWatch;  // reserved in GrayWire::init: no allocation while it is used

    // Whether a texture's faces go to the alpha pool for its alpha (LLPipeline::getPoolTypeFromTE: 4 components,
    // or 2), where its mask decides between alpha blending and the mask pool.
    bool alphaTexture(const LLViewerTexture* texture)
    {
        const S8 components = texture->getComponents();
        return components == 4 || components == 2;
    }

    // An image's alpha mask as mMaskBuilt keeps it.
    S8 maskOf(const LLImageGL* image)
    {
        return image->getIsAlphaMask() ? 1 : 0;
    }

    // Whether the alpha mask of a face's diffuse texture decides the face's pool: LLFace::canRenderAsMask but for
    // the mask itself (rebuildGeom asks it of a face in the alpha pool). A volume's face, not PBR, not rigged, with
    // the face's colour opaque and no glow, the automatic masks on for its kind; and no legacy material (such a
    // face goes to the alpha pool only for its blend mode or its face's alpha, LLPipeline::getPoolTypeFromTE,
    // which canRenderAsMask refuses either way).
    bool maskDecides(const LLFace& face)
    {
        const LLTextureEntry* te = face.getTextureEntry();
        const LLViewerObject* object = face.getViewerObject();
        const LLDrawable* drawable = face.getDrawable();
        if (!te || !object || !drawable || drawable->isDead() || !drawable->getVOVolume() || LLPipeline::sNoAlpha ||
            te->getGLTFRenderMaterial() || face.isState(LLFace::RIGGED) || te->getMaterialParams().notNull() ||
            te->getColor().mV[VALPHA] != 1.f || te->getGlow() != 0.f)
        {
            return false;
        }
        return object->isHUDAttachment() || te->getFullbright() ? LLPipeline::sAutoMaskAlphaNonDeferred
                                                                 : LLPipeline::sAutoMaskAlphaDeferred;
    }

    // The faces of the texture's diffuse channel whose pool its mask decides (maskDecides): visit(face) for each,
    // until it returns true (so does this then).
    template <typename Visit>
    bool visitMaskFaces(LLViewerFetchedTexture* texture, Visit&& visit)
    {
        const LLViewerTexture::ll_face_list_t& faces = *texture->getFaceList(LLRender::DIFFUSE_MAP);
        const S32 count = llmin(texture->getNumFaces(LLRender::DIFFUSE_MAP), (S32)faces.size());
        for (S32 i = 0; i < count; ++i)
        {
            LLFace* face = faces[i];
            if (face && maskDecides(*face) && visit(*face))
            {
                return true;
            }
        }
        return false;
    }

    // The groups of the texture's faces whose pool its mask decides, built again (as
    // LLViewerObject::dirtySpatialGroup: dirtyGeom and the build queue; rebuildGeom puts each face in its pool
    // again) where they were not last built after the frame its analysis settled in (`settled_at`, the frame time:
    // second review L1, one built in that frame too, as gFrameTimeSeconds, a float, gives two frames the same time
    // after many hours; a group built in that frame after the analysis is built once more, no harm. Also an image
    // made at once, not queued: watchMask gives it settled_at, the frame time T, inside updateImages, and a group
    // built earlier in frame T took the old mask; >= left it out, > takes it) and are not to be built anyway
    // (GEOM_DIRTY: the first face found dirties its group, the others pass it).
    void rebuildMaskFaces(LLViewerFetchedTexture* texture, F32 settled_at)
    {
        if (!alphaTexture(texture))
        {
            return;     // not in the alpha pool for its texture any more
        }
        visitMaskFaces(texture, [settled_at](LLFace& face)
        {
            LLSpatialGroup* group = face.getDrawable()->getSpatialGroup();
            if (!group || group->isDead() || group->hasState(LLSpatialGroup::GEOM_DIRTY) ||
                group->mLastUpdateTime > settled_at)
            {
                return false;
            }
            if (plus::materialize::sLogOn && gFrameTimeSeconds - group->mLastUpdateTime < MASK_AGAIN_SECONDS)
            {
                plus::materialize::tally(plus::materialize::MASK_AGAIN);   // research/289 (L4)
            }
            group->dirtyGeom();
            gPipeline.markRebuild(group);
            plus::materialize::tally(plus::materialize::MASK_GROUPS);
            return false;
        });
    }

    // research/289 (the integration check's L3): whether the texture has a face whose pool its mask decides. For
    // one whose faces are all PBR, rigged, of a legacy material, tinted or glowing, or that has none, its band does
    // not wait for its analysis; with its analysis settled it takes no place in the list either (while it waits it
    // is watched all the same: pre-merge review L2, watchMask). Watching them all filled the list after a busy
    // login (MASK_FULL), and the textures it refused then had all their groups built again at their next settle.
    bool anyMaskFace(LLViewerFetchedTexture* texture)
    {
        return visitMaskFaces(texture, [](LLFace&) { return true; });
    }

    // A texture's entry in the list (one with mMaskWatched), or null.
    MaskWatch* findMaskWatch(const LLViewerFetchedTexture* texture)
    {
        const auto it = std::find_if(sMaskWatch.begin(), sMaskWatch.end(),
                                     [texture](const MaskWatch& entry) { return entry.texture.get() == texture; });
        return it != sMaskWatch.end() ? &*it : nullptr;
    }

    // An entry's analysis seen settled now (`now`, the frame time): what the groups built from now on take.
    void noteSettled(MaskWatch& watch, const LLImageGL* image, F32 now)
    {
        watch.settled_at = now;
        watch.settled_mask = maskOf(image);
    }

    // An entry whose analysis settled: the groups of its faces built before that, built again when it settled on
    // another mask than they were built with (they take it then, as those built since did).
    void rebuildIfChanged(MaskWatch& watch)
    {
        namespace mat = plus::materialize;
        LLViewerFetchedTexture* texture = watch.texture.get();
        if (watch.settled_mask == texture->mMaskBuilt)
        {
            mat::tally(mat::MASK_SAME);
            return;
        }
        mat::tally(mat::MASK_CHANGED);
        rebuildMaskFaces(texture, watch.settled_at);
        texture->mMaskBuilt = watch.settled_mask;
    }

    // research/284, onImageCreated (main thread): the texture made now goes in the list when its analysis waits
    // on another thread or came out other than its faces were built with (a texture never seen before takes the
    // mask it has now as theirs). Its entry (a new wait: its band no longer waits for an earlier one), or null:
    // not watched (the setting off, no alpha, nothing to wait for or build again, the list full), or research/289:
    // watched but its band not to wait (no face whose pool its mask decides). Review L1: an
    // entry whose earlier analysis settled but was not taken yet (MASK_REBUILDS_PER_FRAME) is taken first, before
    // the new one's wait forgets when and on what it settled (the groups built since took that mask). Second review
    // L2: one let go unseen (the setting off, the list full) has its mask not known any more (MASK_NOT_KNOWN: its
    // analysis may settle on another unseen, which the groups built since take): its next settle builds them all.
    MaskWatch* watchMask(LLViewerTexture* texture)
    {
        const S8 type = texture->getType();
        const LLImageGL* image = texture->getGLTexture();
        if ((type != LLViewerTexture::FETCHED_TEXTURE && type != LLViewerTexture::LOD_TEXTURE) || !image ||
            !alphaTexture(texture))
        {
            return nullptr;
        }
        LLViewerFetchedTexture* fetched = static_cast<LLViewerFetchedTexture*>(texture);
        if (!maskRebuildOn())
        {
            fetched->mMaskBuilt = LLViewerFetchedTexture::MASK_NOT_KNOWN;
            return nullptr;
        }
        const bool queued = image->maskQueued();
        const S8 mask = maskOf(image);
        if (fetched->mMaskBuilt == LLViewerFetchedTexture::MASK_NEVER_SEEN)
        {
            fetched->mMaskBuilt = mask;
        }
        MaskWatch* watch = fetched->mMaskWatched ? findMaskWatch(fetched) : nullptr;
        if (watch && watch->settled_at >= 0.f)
        {
            rebuildIfChanged(*watch);
        }
        if (!watch && !queued && mask == fetched->mMaskBuilt)
        {
            return nullptr;
        }
        // research/289 (L3): with no face whose pool its mask decides, its band does not wait for its analysis.
        // Settled now, nothing to build again: not watched (a face that comes later is built with this mask).
        // Pre-merge review L2: its analysis waiting, watched all the same (not its band): a face that decides
        // may come and be built before it settles (with no mask yet: in the alpha pool), which only its settle
        // seen builds again (the wait is short: a frame or a few, little of the list).
        const bool faces = anyMaskFace(fetched);
        if (!watch)
        {
            if (!faces && !queued)
            {
                fetched->mMaskBuilt = mask;
                return nullptr;
            }
            if (sMaskWatch.size() >= MASK_WATCH_MAX)
            {
                plus::materialize::tally(plus::materialize::MASK_FULL);
                fetched->mMaskBuilt = LLViewerFetchedTexture::MASK_NOT_KNOWN;
                return nullptr;
            }
            sMaskWatch.emplace_back();
            watch = &sMaskWatch.back();
            watch->texture = fetched;
            fetched->mMaskWatched = true;
        }
        watch->watched_at = gFrameTimeSeconds;
        watch->settled_at = -1.f;
        watch->settled_mask = -1;
        if (!queued)
        {
            noteSettled(*watch, image, gFrameTimeSeconds);  // made now: groups built before took the old mask
        }
        watch->band = false;
        return faces ? watch : nullptr;
    }

    // The band put off for the texture's analysis, now: as onImageCreated would have started it (its arrival
    // asked again: the settings, its wireframe seen lately), unless its wait ended meanwhile (another resolution's
    // band; not drawn for a second, isGray: it shows as it is when it is drawn again).
    void startWaitedBand(LLViewerTexture* texture, LLImageGL* image)
    {
        image->mCreateHold = 0.f;
        const F32 since = texture->mGrayWireSince;
        if (since >= 0.f && image->getTexName())
        {
            startBand(texture, image, arrival(texture, LLViewerTexture::sCurrentTime, since));
        }
    }

    // A band that waited for its texture's analysis, whose hold ran out (MASK_BAND_WAIT on the bands' clock
    // `clock`): it starts now, before its texture's faces are built again. Review L4: counted late only when its
    // wait was still on (a hold isGray ended, the texture not drawn for a while, starts nothing), and with
    // `still_waits` (its analysis has not come: not one that waits for its turn among MASK_REBUILDS_PER_FRAME).
    void startBandAfterWait(MaskWatch& watch, LLImageGL* image, F32 clock, bool still_waits)
    {
        if (!watch.band || clock < image->mCreateHold)
        {
            return;
        }
        watch.band = false;
        if (still_waits && image->mCreateHold != 0.f && watch.texture->mGrayWireSince >= 0.f)
        {
            plus::materialize::tally(plus::materialize::MASK_LATE);
        }
        startWaitedBand(watch.texture.get(), image);
    }

    // Once a frame (GrayWire::updateClass, after the bands' clock moved): the entries whose analysis settled (of
    // those whose faces are built again, MASK_REBUILDS_PER_FRAME a frame, the rest the next: review L2), then their
    // bands if they wait (after the groups were queued: the band sweeps them in their new pool once they are
    // built; research/279 1(a) 4 keeps an alpha batch a wireframe until then); the bands that waited for one
    // MASK_BAND_WAIT; the analyses that waited MASK_GIVE_UP_SECONDS. The setting off: every entry let go of (its
    // band started), and the images analysed again are no masks until the new analysis comes
    // (LLImageGL::sKeepMask), as upstream. Empty: one test.
    void updateMaskWatch()
    {
        namespace mat = plus::materialize;
        const bool on = maskRebuildOn();
        LLImageGL::sKeepMask = on;
        if (sMaskWatch.empty())
        {
            return;
        }
        const F32 now = gFrameTimeSeconds;
        const F32 clock = mat::bandClock();
        U32 rebuilt = 0;
        for (size_t i = 0; i < sMaskWatch.size();)
        {
            MaskWatch& watch = sMaskWatch[i];
            LLImageGL* image = watch.texture->getGLTexture();
            const bool queued = image && image->maskQueued();
            if (on && queued && now - watch.watched_at < MASK_GIVE_UP_SECONDS)
            {
                startBandAfterWait(watch, image, clock, true);
                ++i;
                continue;
            }
            const bool settled = on && image && !queued;
            if (settled && watch.settled_at < 0.f)
            {
                noteSettled(watch, image, now);
            }
            // research/289 (pre-merge review L2): one with no face its mask decides (watched only while its
            // analysis waited, watchMask) is let go of at its settle: its mask taken as the faces', nothing built,
            // counted (mask same / changed) or taken from the budget of the ones with faces to build again.
            const bool faces = settled && anyMaskFace(watch.texture.get());
            if (settled && !faces)
            {
                watch.texture->mMaskBuilt = watch.settled_mask;
            }
            else if (settled && watch.settled_mask != watch.texture->mMaskBuilt)
            {
                if (rebuilt >= MASK_REBUILDS_PER_FRAME || (!watch.band && now - watch.settled_at < MASK_GATHER_SECONDS))
                {
                    startBandAfterWait(watch, image, clock, false);
                    ++i;
                    continue;
                }
                ++rebuilt;
            }
            if (faces)
            {
                rebuildIfChanged(watch);
            }
            else if (!settled)
            {
                if (on)
                {
                    mat::tally(mat::MASK_GAVE_UP);  // no image, or its analysis never came
                }
                // second review L2: let go unseen (given up, the setting off): its analysis may settle unseen
                watch.texture->mMaskBuilt = LLViewerFetchedTexture::MASK_NOT_KNOWN;
            }
            if (watch.band && image)
            {
                startWaitedBand(watch.texture.get(), image);
            }
            watch.texture->mMaskWatched = false;
            if (i + 1 < sMaskWatch.size())
            {
                watch = std::move(sMaskWatch.back());   // the order does not matter
            }
            sMaskWatch.pop_back();
        }
    }

    void forgetMaskWatch()
    {
        for (MaskWatch& watch : sMaskWatch)
        {
            watch.texture->mMaskWatched = false;
        }
        sMaskWatch.clear();
    }
}

namespace GrayWire
{
    void init()
    {
        // Review retro-vulkan 2: all the steps up front, not in a pass the
        // first time each is needed (an upload there may end a render pass).
        makeLooks();
        sMaskWatch.reserve(MASK_WATCH_MAX);     // research/284
    }

    void cleanup()
    {
        for (LLPointer<LLViewerTexture>& texture : sLook)
        {
            texture = nullptr;
        }
        sLookKey = 0;
        sLookTron = false;
        LLTexUnit::sUnit0Override = nullptr;
        forgetMaskWatch();  // research/284: the textures it holds, before the texture manager's end
    }

    void updateClass()
    {
        plus::materialize::tick(gFrameTimeSeconds);  // research/278: the bands' clock, PlusMaterializeLog
        updateMaskWatch();  // research/284: after the clock (the bands that waited for an analysis)
        if (sLookKey && gFrameCount - sLookMadeFrame >= LOOK_REMAKE_FRAMES && sLookKey != plus::materialize::lookKey())
        {
            makeLooks();
        }
    }

    bool enabled()
    {
        static LLCachedControl<bool> on(gSavedSettings, "RenderGrayTextureWireframe", true);
        return on;
    }

    bool wirePassNow()
    {
        return wirePass();
    }

    bool isGray(LLViewerTexture* texture)
    {
        if (!texture)
        {
            return false;
        }
        const S8 type = texture->getType();
        if (type != LLViewerTexture::FETCHED_TEXTURE && type != LLViewerTexture::LOD_TEXTURE)
        {
            return false;   // local, media and dynamic textures are not fetched
        }
        LLImageGL* image = texture->getGLTexture();
        if (!image)
        {
            texture->mGrayWireSince = -1.f;
            return false;
        }
        const F32 now = LLViewerTexture::sCurrentTime;
        if (image->getTexName())
        {
            // research/256 review M1: an image made on another thread has its GL name a frame or more
            // before postCreateTexture. While onImageScheduled holds it (it was drawn as a wireframe), the
            // wait goes on (not started again): a wireframe until the band starts there. Not drawn for
            // a while (re-review M-1): the wait is over, that hold too (third review 1: only that one, a
            // band's stays); it shows when it is drawn again.
            if (texture->mGrayWireSince < 0.f || !image->createHeld() || stale(texture->mGrayWireSeen))
            {
                if (image->mCreateHold != 0.f)
                {
                    image->mCreateHold = 0.f;   // only when there is one (every loaded texture's batch comes here)
                }
                texture->mGrayWireSince = -1.f;
                return false;
            }
            texture->mGrayWireSeen = plus::materialize::bandClock();
            // research/276: held only after a Scope drew it as a wireframe (onImageScheduled): it stays one
            return !texture->isMissingAsset() && !givenUp(texture, now - texture->mGrayWireSince);
        }
        if (texture->mGrayWireSince < 0.f || stale(texture->mGrayWireSeen))
        {
            // First time, or not asked for a while (not drawn: not waiting): a new wait (research/276: not
            // drawn as a wireframe yet in it).
            texture->mGrayWireSince = now;
            texture->mGrayWireFrame = gFrameCount;
            texture->mGrayWireShown = -1.f;
        }
        texture->mGrayWireSeen = plus::materialize::bandClock();  // research/278 review M1 (stale)
        const F32 waited = now - texture->mGrayWireSince;
        const bool given_up = givenUp(texture, waited);
        if (texture->mGrayWireGivenUp != given_up)
        {
            texture->mGrayWireGivenUp = given_up;   // review low 4: plus/materiallist reads it
        }
        if (given_up || texture->isMissingAsset())
        {
            return false;
        }
        return wireNow(texture, waited);
    }

    void updateAvatar(LLVOAvatar* avatar)
    {
        if (!enabled())
        {
            if (!sAvatars.empty())
            {
                // Re-review L1: BOM faces showing an old bake take the bakes as they are first (no use of the new
                // one would, with the table empty). The keys are live avatars (forgetAvatar takes the dead out).
                for (auto& [kept_by, kept] : sAvatars)
                {
                    releaseKeptBakes(const_cast<LLVOAvatar*>(kept_by), kept);
                }
                sAvatars.clear();
                sGrayAvatars = 0;
                sSelfWire.store(false, std::memory_order_relaxed);
            }
            return;
        }
        // research/243: the agent's avatar while it loads (drawsLoading draws it
        // then) is a wireframe as a whole; looked at again at once when that changes.
        const bool loading_on = loadingOn() && !avatar->isUIAvatar() && !avatar->isControlAvatar();
        const bool loading = loading_on && avatar->isSelf() && !avatar->isFullyLoaded();
        AvatarEntry& entry = sAvatars[avatar];
        if (entry.band_start >= 0.f)
        {
            updateAvatarBand(avatar, entry);    // research/259
        }
        if (entry.checked && gFrameCount - entry.checked < CHECK_EVERY_FRAMES && loading == entry.loading)
        {
            return;
        }
        entry.checked = gFrameCount ? gFrameCount : 1;
        entry.loading = loading;
        entry.self = avatar->isSelf();
        noteBounds(avatar, entry);  // research/289 (L5)
        const bool avatar_band = plus::materialize::avatarOn();
        if (!avatar_band)
        {
            releaseKeptBakes(avatar, entry);    // re-review L1: the avatar band turned off while one was kept
        }
        if (entry.materialized && avatar_band)
        {
            keepMaterialized(avatar, entry, loading, loading_on);   // research/281
            if (avatar->isSelf())
            {
                sSelfWire.store(false, std::memory_order_relaxed);
            }
            return;
        }
        if (!avatar->isSelf() && !avatar->isControlAvatar() && !avatar->isUIAvatar() && avatar_band)
        {
            noteListedAttachments(avatar, entry);   // research/277 A: from its first look (a wait may follow)
        }
        // A body bake being fetched keeps the wireframe up to FETCHED_GIVE_UP_SECONDS
        // (a fetch that stalls does not hold the avatar as a wireframe for good).
        bool bakes_gray = false;
        if (loading_on && bakesHaveGray(avatar))
        {
            const F32 now = LLViewerTexture::sCurrentTime;
            if (entry.bakes_since < 0.f)
            {
                entry.bakes_since = now;
            }
            bakes_gray = now - entry.bakes_since < FETCHED_GIVE_UP_SECONDS;
        }
        else
        {
            entry.bakes_since = -1.f;
        }
        // A wireframe as a whole while it loads, a body bake is fetched, an attachment's texture is gray, or
        // (research/277 A) it waits for its attachments. research/277: not for the last two while its band
        // sweeps it: an attachment that turns gray then (one that came late) is a wireframe on its own batches
        // (their Scope) and joins the band when its image comes in time (holdWornBatch), not the whole avatar a
        // wireframe again and its band taken back.
        bool gray = loading || bakes_gray;
        if (!gray && entry.band_start < 0.f)
        {
            gray = scanAttachments(avatar, Walk::SCAN) || waitsForAttachments(avatar, entry);
        }
        if (gray)
        {
            scanAttachments(avatar, Walk::HOLD_WIRE);  // every attachment's batch on the GL path, where the Scope draws it
        }
        if (gray != entry.gray)
        {
            entry.gray = gray;
            if (gray)
            {
                ++sGrayAvatars;
                entry.band_start = -1.f;    // research/259: a wireframe again (a Scope draws it)
            }
            else
            {
                --sGrayAvatars;
                entry.attachments_since = -1.f;     // research/277 A: the next wireframe waits anew
                entry.materialized = true;          // research/281: band or not, it is seen whole now
                startAvatarBand(avatar, entry);     // research/259: it has all it waited for
                // research/281 review L2: what came before its band's end (or now) was in it, or under its wireframe
                entry.own_flash_from = entry.band_start >= 0.f ? entry.band_start + plus::materialize::seconds()
                                                               : plus::materialize::bandClock();
            }
        }
        else if (!gray && avatar_band && seenWhole(avatar, entry))
        {
            entry.materialized = true;  // research/281: whole without a wireframe
            entry.own_flash_from = plus::materialize::bandClock();
        }
        if (avatar->isSelf())
        {
            sSelfWire.store(entry.gray, std::memory_order_relaxed);    // research/281 review L1
        }
    }

    void forgetAvatar(const LLVOAvatar* avatar)
    {
        const auto it = sAvatars.find(avatar);
        if (it != sAvatars.end())
        {
            if (it->second.gray)
            {
                --sGrayAvatars;
            }
            // Re-review L3: from its entry, not isSelf (asked in ~LLVOAvatar, where it is the base class's: false)
            if (it->second.self)
            {
                sSelfWire.store(false, std::memory_order_relaxed);
            }
            sAvatars.erase(it);
        }
    }

    bool avatarIsGray(const LLVOAvatar* avatar)
    {
        if (!sGrayAvatars || !avatar)
        {
            return false;
        }
        const auto it = sAvatars.find(avatar);
        return it != sAvatars.end() && it->second.gray;
    }

    bool avatarInBand(const LLVOAvatar* avatar)
    {
        return bandEntry(avatar) != nullptr;
    }

    bool avatarDrawnApart(const LLVOAvatar* avatar)
    {
        return avatarIsGray(avatar) || avatarInBand(avatar);
    }

    bool drawnLately()
    {
        const U32 drawn = sDrawnFrame.load(std::memory_order_relaxed);
        return drawn && gFrameCount - drawn <= DRAWN_LATELY_FRAMES;
    }

    bool drawsLoading(const LLVOAvatar* avatar)
    {
        // Where a Scope draws a wireframe (Scope::begin), else hidden as before. research/281 review L1: only
        // while it is a wireframe as a whole (a materialized one is not one again: one that is not fully loaded
        // again, its look missing, is hidden as the official viewer hides it, not drawn whole).
        return avatar && avatar->isSelf() && enabled() && loadingOn() && sSelfWire.load(std::memory_order_relaxed) &&
               (gUseWireframe || (!LLPipeline::sShadowRender && !LLPipeline::sReflectionRender && !gCubeSnapshot));
    }

    LLViewerFetchedTexture* bakeShown(LLVOAvatar* avatar, U8 index, LLViewerFetchedTexture* current, const LLUUID& last_used)
    {
        if (!current || index >= LLAvatarAppearanceDefines::BAKED_NUM_INDICES || !enabled() ||
            avatar->isUIAvatar() || avatar->isControlAvatar())
        {
            return current;
        }
        const auto it = sAvatars.find(avatar);
        if (it == sAvatars.end())
        {
            return current;
        }
        AvatarEntry& entry = it->second;
        const U32 bit = 1u << index;
        // Kept only for a materialized avatar (not at its first look), while the bake its body uses differs
        // from the new one, the old one is a real bake (not a default; re-review: nor the new one invisible,
        // which shows at once) and still loaded. Pre-merge review L4: also when the new one failed (the body
        // keeps its old bake then too: the BOM faces do not take the default image).
        const LLUUID& id = current->getID();
        LLViewerFetchedTexture* last = nullptr;
        if (plus::materialize::avatarOn() && entry.materialized && !last_used.isNull() && id != last_used &&
            id != IMG_INVISIBLE && last_used != IMG_DEFAULT_AVATAR && last_used != IMG_DEFAULT &&
            last_used != IMG_INVISIBLE)
        {
            last = LLViewerTextureManager::findFetchedTexture(last_used, TEX_LIST_STANDARD);
        }
        const LLImageGL* image = last ? last->getGLTexture() : nullptr;
        if (!image || !image->getTexName() || last->isMissingAsset())
        {
            if (entry.bakes_kept & bit)
            {
                entry.bakes_kept &= ~bit;   // nothing to keep for this part any more
                if (!entry.bakes_kept)
                {
                    entry.bake_kept_since = -1.f;
                    entry.bakes_kept_out = false;
                }
            }
            return current;
        }
        if (!(entry.bakes_kept & bit) || entry.bake_awaited[index] != id)
        {
            // Pre-merge review L2: a new change of this part: the time kept counts from now (not from an earlier
            // change's first keep), and one kept too long before is kept again.
            entry.bakes_kept |= bit;
            entry.bake_awaited[index] = id;
            entry.bake_kept_since = LLViewerTexture::sCurrentTime;
            entry.bakes_kept_out = false;
        }
        if (entry.bakes_kept_out && !current->isMissingAsset())
        {
            return current;     // kept too long (BAKE_KEEP_SECONDS): as it is, until none is left to keep
        }
        return last;
    }

    void onBakeUsed(LLVOAvatar* avatar, const LLUUID& id)
    {
        const auto it = sAvatars.find(avatar);
        if (it == sAvatars.end() || !it->second.bakes_kept)
        {
            return;     // none kept: the official refresh is enough
        }
        // Re-review L4: only the use of a bake whose old one was kept takes the BOM faces again (the uses of the
        // bakes it already had, which LLVOAvatar::updateMeshTextures asks for, change nothing there).
        AvatarEntry& entry = it->second;
        U32 used = 0;
        for (U32 index = 0; index < entry.bake_awaited.size(); ++index)
        {
            if ((entry.bakes_kept & (1u << index)) && entry.bake_awaited[index] == id)
            {
                used |= 1u << index;
            }
        }
        if (!used)
        {
            return;
        }
        entry.bakes_kept &= ~used;
        if (!entry.bakes_kept)
        {
            entry.bake_kept_since = -1.f;
            entry.bakes_kept_out = false;
        }
        // The new bake on its BOM faces, as on its body now; one still loading for another part stays kept
        // (bakeShown; the time kept counted on the avatar's one clock, from its latest change of a bake).
        refreshWornBakes(avatar);
    }

    const LLVOAvatar* wireOwner(const LLFace& face)
    {
        const LLViewerObject* object = face.getViewerObject();
        const LLVOAvatar* owner = face.mAvatar ? face.mAvatar : (object && object->isAttachment() ? object->getAvatar() : nullptr);
        if (!owner || !plus::materialize::avatarOn())
        {
            return owner;
        }
        // research/277 review L2: a HUD's none (it is never drawn as its wearer's, and noteBatch holds a worn
        // batch by its wearer: a HUD's surfaces would go bound to their units).
        if (object && object->isHUDAttachment())
        {
            return nullptr;
        }
        // research/277 E: an attached animesh's faces go with its control avatar, which is never a wireframe
        // nor takes a band: the rigged ones are rigged to it, review L3: and LLViewerObject::getAvatar gives
        // it for its unrigged prims. Its wearer's (as LLDrawPoolAvatar::renderAvatars finds it).
        const LLVOAvatar* wearer = owner->isControlAvatar() ? owner->getAttachedAvatar() : nullptr;
        return wearer ? wearer : owner;
    }

    void noteBatch(LLDrawInfo& info, bool new_info, U32 pass, bool worn, bool region_space, const LLFace* moving)
    {
        holdWornBatch(info);    // research/277 C, review L2 (alpha ones too: held as the walks hold them)
        if (!new_info)
        {
            // A moving batch's bounds grow with its faces (a static one's: registerFace's mExtents).
            if (moving && info.mBandKind == BAND_SWEEP_MOVING)
            {
                info.mExtents[0].setMin(info.mExtents[0], moving->mExtents[0]);
                info.mExtents[1].setMax(info.mExtents[1], moving->mExtents[1]);
            }
            return;
        }
        info.mBandKind = BAND_NONE;
        info.mBandPass = (U8)llmin(pass, 255u);     // research/278: PlusMaterializeLog's counts by pass
        if (!bandPass(pass))
        {
            return;     // research/256: alpha, glow and the like
        }
        if (worn)
        {
            info.mBandKind = BAND_WORN;     // research/259: its wearer's band
            return;
        }
        if (region_space)
        {
            info.mBandKind = info.mHasExtents ? BAND_SWEEP : BAND_FLASH;
        }
        else if (moving)
        {
            info.mExtents[0] = moving->mExtents[0];
            info.mExtents[1] = moving->mExtents[1];
            info.mBandKind = BAND_SWEEP_MOVING;
        }
        else
        {
            info.mBandKind = BAND_FLASH;    // an animated child: its face extents and vertices may differ in space
        }
    }

    void onImageCreated(LLViewerTexture* texture)
    {
        namespace mat = plus::materialize;
        MaskWatch* mask_watch = watchMask(texture);     // research/284: first, whatever the band's settings
        const F32 since = texture->mGrayWireSince;
        if (since < 0.f)
        {
            mat::tally(mat::CAME_UNWAITED);
            return;     // not waited for as a wireframe (or its band started at an earlier resolution)
        }
        LLImageGL* image = texture->getGLTexture();
        if (!image)
        {
            mat::tally(mat::CAME_NO_IMAGE);
            return;
        }
        image->mCreateHold = 0.f;       // review M1, re-review L-2: onImageScheduled's hold ends here
        if (!image->getTexName())
        {
            mat::tally(mat::CAME_NO_IMAGE);
            return;     // no image after all (its creation failed): the wait goes on
        }
        const mat::Tally why = arrival(texture, LLViewerTexture::sCurrentTime, since);
        if (why == mat::CAME_BAND && mask_watch && image->maskQueued())
        {
            // research/284: its band waits for its alpha analysis (a wireframe meanwhile, held as one made
            // elsewhere: isGray, the fast paths), so that it sweeps its faces in the pool the analysis puts
            // them in; at most MASK_BAND_WAIT (updateMaskWatch starts it then)
            image->mCreateHold = mat::bandClock() + MASK_BAND_WAIT;
            mask_watch->band = true;
            mat::tally(mat::MASK_WAIT);
            return;
        }
        startBand(texture, image, why);
    }

    void onImageScheduled(LLViewerTexture* texture)
    {
        LLImageGL* image = texture->getGLTexture();
        const F32 now = LLViewerTexture::sCurrentTime;
        // Re-review M-1: only one drawn as a wireframe now (one not drawn as one yet shows its image as soon
        // as it has a name, not a frame of it and then the wireframe again). research/276: a Scope drew it.
        if (!image || !drawnAsWire(texture, now) || !enabled() || gUseWireframe || !plus::materialize::on())
        {
            return;
        }
        image->mCreateHold = plus::materialize::bandClock() + MADE_ELSEWHERE_HOLD;  // research/278 re-review L3
    }

    bool bandsActive()
    {
        return plus::materialize::anyBand(plus::materialize::bandClock());
    }

    Scope::Scope(const LLDrawInfo& params, bool blended)
    {
        // research/289 (the integration check's L2): nothing where no wireframe is drawn (begin would not draw
        // one): not even isGray, so that a texture drawn only in the shadow, reflection, cube snapshot or impostor
        // passes does not wait meanwhile (its wait given up unseen, it would come gray when first seen).
        // <plus> research/304: with the wireframe off (RenderGrayTextureWireframe) the stand-in is still drawn: noted
        // for plus/texprio all the same (the walk of the batch's textures is the cost: only with the wireframe off)
        if (!enabled() && wirePass())
        {
            visitDrawnTextures(params, [](LLViewerTexture* texture)
            {
                stampDrawn(texture);
                return false;
            });
            return;
        }
        // </plus>
        if (!enabled() || !wirePass())
        {
            return;
        }
        // research/243 review M1: a HUD object's batch may have its wearer as
        // mWireOwner (wireOwner: with the avatar band off; research/277 gives it
        // none with it on), but HUDs are not held back while an avatar loads:
        // only its own gray textures make it a wireframe.
        const LLViewerTexture* gray = grayTexture(params);    // research/276, 278: noted as drawn as a wireframe
        // research/284 (279 1(a) 4): an alpha blended batch stays a wireframe while its own textures' band is under
        // way (alpha takes none), so that one whose faces its alpha analysis moves to the mask pool does not show
        // whole for the frames before they are built there, ahead of the band; a real alpha blended one comes at
        // the band's end, with its opaque neighbours' end. Not on HUDs (their batches take no band). Review L3: it
        // covers the faces built again (PlusAlphaMaskRebuild): with that off, as before (alpha shows at once).
        const F32 own_start = blended && !LLPipeline::sRenderingHUDs && maskRebuildOn() && bandsActive()
                                  ? bandStartedAt(params) : -1.f;
        // research/289 (L1): a worn one only as its wearer's opaque batches take their own textures' band (Band:
        // its own flash, ownFlashAllowed): not one its wearer's materializing took in (its band refused, or it
        // was seen whole), where those showed at once; nor with the avatar band off, where they take none.
        const bool own_band = own_start >= 0.f &&
                              (!params.mWireOwner ||
                               (plus::materialize::avatarOn() && ownFlashAllowed(params.mWireOwner, own_start)));
        if (gray || own_band || (params.mWireOwner && !LLPipeline::sRenderingHUDs &&
                                 (avatarIsGray(params.mWireOwner) || (blended && bandHereFor(params.mWireOwner)))))
        {
            begin();
            if (mOn)
            {
                noteWire(params);   // research/289 (L5): TAA's jitter
            }
        }
        if (plus::materialize::sLogOn)
        {
            if (blended && (!mOn || own_band))
            {
                tallyUnbanded(params, plus::materialize::DRAWN_ALPHA);  // research/278: alpha takes no band
            }
            if (!mOn)
            {
                tallyGray(params);  // research/289: a texture of it drawn gray for good
            }
        }
    }

    Scope::Scope(const LLVOAvatar* avatar, AvatarPart part)
    {
        if (enabled() && (avatarIsGray(avatar) || (part == AVATAR_EYES && eyesUnderBand(avatar)) ||
                          (part == AVATAR_ALPHA && bandHereFor(avatar))))
        {
            begin();
            if (mOn)
            {
                noteWire(avatar);   // research/289 (L5): TAA's jitter
            }
        }
    }

    void Scope::begin()
    {
        if (!wirePass())
        {
            return;
        }
        mPreviousLines = sLines;
        setLines(true);
        mPrevious = LLTexUnit::sUnit0Override;
        LLTexUnit::sUnit0Override = look();
        mOn = true;
        beginGlow();
    }

    void Scope::beginGlow()
    {
        // research/256: with PlusMaterialize, a program that takes the band's uniforms lights the lines
        // itself (mode 3); any other draws them as before (lit as a surface). Not inside a band (it has the
        // uniforms) nor inside another Scope's glow, nor on HUDs.
        LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
        if (!shader || shader == sGlowShader || sInBand || LLPipeline::sRenderingHUDs || !plus::materialize::on() ||
            !takesBand(shader))
        {
            return;
        }
        const plus::materialize::Edge& edge = plus::materialize::edge();
        const F32 params[4] = { WIRE_GLOW_MODE, 0.f, wireGlow(edge), fmodf(LLViewerTexture::sCurrentTime, NOISE_PERIOD) };
        shader->uniform4fv(sColourUniform, 1, edge.colour);
        shader->uniform4fv(sParamsUniform, 1, params);
        mGlowShader = shader;
        mPreviousGlow = sGlowShader;
        sGlowShader = shader;
    }

    Scope::~Scope()
    {
        if (mGlowShader)
        {
            // Another program bound now than the one its glow was set on is normal (the alpha pool binds
            // its batch's program after its Scope; an inner Scope sets the glow there): the uniforms go back
            // to 0 on the program they were set on. Wrong (third review 4): an inner Scope's glow still
            // owning the uniforms when this one ends (it did not give this one's back).
            if (sGlowShader != mGlowShader)
            {
                LL_WARNS_ONCE("GrayWire") << "A wireframe's glow was not given back by a scope inside it (program "
                                          << mGlowShader->mName << ")" << LL_ENDL;
            }
            static const F32 ZERO[4] = { 0.f, 0.f, 0.f, 0.f };
            mGlowShader->uniform4fv(sParamsUniform, 1, ZERO);     // 0 before it (one glow per program at a time)
            mGlowShader->uniform4fv(sColourUniform, 1, ZERO);
            sGlowShader = mPreviousGlow;
        }
        if (mOn)
        {
            LLTexUnit::sUnit0Override = mPrevious;
            setLines(mPreviousLines);
        }
    }

    Band::Band(const LLDrawInfo& params)
        : mBatch(&params)
    {
        namespace mat = plus::materialize;
        if (params.mBandKind == BAND_NONE)
        {
            if (mat::sLogOn)
            {
                tallyUnbanded(params, mat::DRAWN_NONE);     // research/278: a pass that takes no band
            }
            return;
        }
        if (!bandHere())
        {
            return;
        }
        if (params.mBandKind == BAND_WORN)
        {
            // research/259: its wearer's band, the plane of the whole avatar
            F32 progress = 0.f;
            if (const AvatarEntry* entry = avatarBandNow(params.mWireOwner, progress))
            {
                start(BAND_SWEEP, Sweep{ entry->band_low, entry->band_high }, progress);
                if (mat::sLogOn)
                {
                    tallyDrawn(on(), mat::DRAWN_WORN);
                }
                return;
            }
            // research/281: without its wearer's band (one that came after its wearer materialized, a texture put
            // on: they are never a wireframe as a whole again), its own textures' band, as a flash (a worn batch
            // keeps no bounds), with the avatar band on.
            const F32 done = mat::avatarOn() ? bandStartedAt(params) : -1.f;
            const F32 own = done >= 0.f && ownFlashAllowed(params.mWireOwner, done) ? progressFrom(done) : 1.f;
            if (own < 1.f)
            {
                start(BAND_FLASH, Sweep(), own);
                if (mat::sLogOn)
                {
                    tallyDrawn(on(), mat::DRAWN_WORN_OWN);
                }
            }
            return;
        }
        // Re-review L-3: whether its textures have a band at all first (the bounds through the matrix after).
        const F32 done = bandStartedAt(params);
        if (done < 0.f)
        {
            return;
        }
        Sweep sweep;
        mat::Tally why = mat::DRAWN_SWEEP;
        const BandKind kind = bandNow(params, done, sweep, why);
        const F32 progress = progressFrom(done);
        if (kind == BAND_NONE || progress >= 1.f)
        {
            return;
        }
        start(kind, sweep, progress);
        if (mat::sLogOn)
        {
            tallyDrawn(on(), why);  // research/278, 282: an upright sweep, a flat one, one by its lost bounds, a flash
        }
    }

    Band::Band(const LLVOAvatar* avatar)
        : mAvatar(avatar)
    {
        if (!bandHere())
        {
            return;
        }
        F32 progress = 0.f;
        if (const AvatarEntry* entry = avatarBandNow(avatar, progress))
        {
            start(BAND_SWEEP, Sweep{ entry->band_low, entry->band_high }, progress);
            if (plus::materialize::sLogOn)
            {
                tallyDrawn(on(), plus::materialize::DRAWN_BODY);   // research/278
            }
        }
    }

    void Band::start(BandKind kind, const Sweep& sweep, F32 progress)
    {
        // Only a program that clips by the band declares its uniforms (the shaders of research/256's list;
        // globalF.glsl declares none): any other draws the batch once, whole, as before.
        LLGLSLShader* shader = LLGLSLShader::sCurBoundShaderPtr;
        if (!takesBand(shader))
        {
            return;
        }
        const plus::materialize::Edge& edge = plus::materialize::edge();
        mHasLines = bandShape(kind, sweep, progress, edge.strength, mPlane, mParams);
        mEdgeStrength = mParams[2];
        mLineGlow = wireGlow(edge);
        // Re-review L1: the band's clock; research/289 (L7): from the band's start (progress of its length), so
        // that it never wraps during a band (the clock itself past a NOISE_PERIOD would jump the ripple)
        mParams[3] = progress * plus::materialize::seconds();
        mShader = shader;
        mPrevious = LLTexUnit::sUnit0Override;
        mPreviousLines = sLines;
        sInBand = true;
        shader->uniform4fv(sPlaneUniform, 1, mPlane);
        shader->uniform4fv(sColourUniform, 1, edge.colour);
    }

    Band::~Band()
    {
        if (!mShader)
        {
            return;
        }
        static const F32 ZERO[4] = { 0.f, 0.f, 0.f, 0.f };
        mShader->uniform4fv(sParamsUniform, 1, ZERO);
        mShader->uniform4fv(sPlaneUniform, 1, ZERO);
        mShader->uniform4fv(sColourUniform, 1, ZERO);
        LLTexUnit::sUnit0Override = mPrevious;
        setLines(mPreviousLines);
        sInBand = false;
    }

    void Band::lines()
    {
        // thin lines: TAA's jitter waits (drawnLately; research/289: for ones near enough)
        if (mBatch)
        {
            noteWire(*mBatch);
        }
        else
        {
            noteWire(mAvatar);
        }
        setLines(true);
        LLTexUnit::sUnit0Override = look();
        mParams[2] = mLineGlow;     // the lines glow as the waiting wireframe's (the shaders add it in mode 2 too)
        setMode(2.f);
    }

    void Band::surface()
    {
        setLines(false);
        LLTexUnit::sUnit0Override = mPrevious;
        mParams[2] = mEdgeStrength; // the edge's glow
        setMode(1.f);
    }

    void Band::setMode(F32 mode)
    {
        mParams[0] = mode;
        mShader->uniform4fv(sParamsUniform, 1, mParams);
    }

    NoBand::NoBand()
        : mPrevious(sInBand)
    {
        sInBand = true;
    }

    NoBand::~NoBand()
    {
        sInBand = mPrevious;
    }
}
