/**
 * @file derez.cpp
 * @brief Derez: a deleted object crumbles into glowing blocks that fly up, instead of vanishing at once.
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

#include "llviewerprecompiledheaders.h"

#include "plus/derez/derez.h"
#include "plus/objectfx/objectfx_palette.h"
#include "plus/plus.h"

#include "llagent.h"
#include "llapp.h"
#include "llappviewer.h"
#include "lldrawable.h"
#include "lldrawpool.h"
#include "llevents.h"
#include "llface.h"
#include "llfetchedgltfmaterial.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llglslshader.h"
#include "llmaterial.h"
#include "llrender.h"
#include "llrhi.h"
#include "llshadermgr.h"
#include "llspatialpartition.h"
#include "llstaticstringtable.h"
#include "lltextureentry.h"
#include "llthread.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llvieweroctree.h"
#include "llviewerobject.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llvovolume.h"
#include "pipeline.h"
#include "v3color.h"
#include "v4color.h"

#include <cmath>
#include <unordered_set>
#include <utility>
#include <vector>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn

LLGLSLShader gDeferredDerezProgram;

namespace plus::derez
{
    namespace detail
    {
        U32 sLiveCount = 0;
    }

    namespace
    {
        // The ghost table's length: PlusDerezMax is held to it.
        constexpr U32 MAX_GHOSTS = 64;
        // Budgets: one kill, and all the live ghosts together. A static object's faces hold their group's
        // whole vertex buffer (shared with the objects around it), so the bytes are the buffers' sizes.
        constexpr U32 MAX_FACES_PER_GHOST = 1024;
        constexpr U32 MAX_VERTICES_PER_GHOST = 100000;
        constexpr U32 MAX_VERTICES = 200000;
        constexpr U32 MAX_FACES = 4096;                 // all the live ghosts' faces: the draws a frame
        constexpr U64 MAX_BUFFER_BYTES = 24ull << 20;
        // A cell is about this share of the object's largest side, within these bounds (m).
        constexpr F32 CELLS_ACROSS = 12.f;
        constexpr F32 MIN_CELL = 0.03f;
        constexpr F32 MAX_CELL = 2.f;
        // The object's largest side (m): outside these, no ghost (a speck, a megaprim, a broken scale).
        constexpr F32 MIN_SIZE = 0.01f;
        constexpr F32 MAX_SIZE = 256.f;
        // After the agent's region changed (a teleport, a crossing): no ghost for this long (s).
        constexpr F64 REGION_CHANGE_QUIET = 3.0;
        // An object this close to its region's edge (m) may be crossing it: its kill is the old region's.
        constexpr F32 EDGE_MARGIN = 4.f;
        // Faster than this (m/s): a moving object, left out.
        constexpr F32 MOVING_SPEED = 0.2f;
        // A temporary object smaller than this (its box's largest side, m): a bullet, a spark, a
        // short-lived effect. The viewer does not know an object's age, so the size stands for it.
        constexpr F32 SMALL_TEMPORARY = 2.f;
        // Fainter faces (their colour's alpha) are not drawn (invisible prims).
        constexpr F32 MIN_FACE_ALPHA = 0.05f;
        // A legacy face drawn as an alpha mask without a material: its texture's alpha is cut here.
        constexpr F32 LEGACY_MASK_CUTOFF = 0.5f;
        // The glow on the screen (the tone mapping's units: a lit white is about 1), the bloom takes it;
        // under "no post" (plus::isNoPost(): no exposure, every channel clamped to 1) held under 1.
        constexpr F32 GLOW_SCREEN = 3.f;
        constexpr F32 GLOW_NO_POST = 0.8f;
        // Without the emissive target (RenderEnableEmissiveBuffer off) this share of the glow goes into
        // the colour instead.
        constexpr F32 GLOW_INTO_COLOUR = 0.6f;
        // The palette (PlusDerezPalette 0-3, sRGB), research/256's, shared with materialize
        // (plus/objectfx/objectfx_palette.h). PALETTE_CUSTOM takes PlusDerezColor.
        using plus::objectfx::PALETTE;
        using plus::objectfx::PALETTE_CUSTOM;
        using plus::objectfx::PALETTE_SIZE;
        // The attributes derezV.glsl reads (every volume face's buffer has them).
        constexpr U32 NEEDED_MASK = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_TEXCOORD0;

        enum AlphaMode : U32
        {
            ALPHA_OPAQUE = 0,
            ALPHA_BLEND = 1,    // dithered by the texture's alpha times the face's
            ALPHA_MASK = 2,     // cut at the cutoff
        };

        // One face of a ghost, as its batch drew it (llvovolume.cpp registerFace()).
        struct GhostFace
        {
            LLPointer<LLVertexBuffer> buffer;
            LLPointer<LLViewerTexture> texture;     // never null (white when the face had none)
            LLMatrix4 model;                        // the face's vertices to the agent's frame
            LLVector4 colour;                       // derez_color: linear rgb, a: the face's alpha
            LLVector4 face;                         // derez_face: alpha mode, cutoff, fullbright, warp
            LLVector4 emissive;                     // derez_emissive: rgb (linear)
            LLVector4 uv[2];                        // derez_uv0/1: the base colour's texture transform
            U32 start = 0;
            U32 end = 0;
            U32 count = 0;
            U32 offset = 0;
        };

        struct Ghost
        {
            std::vector<GhostFace> faces;           // made once when it starts, let go when it ends
            LLVector4 grid;                         // derez_grid (derezClock.glsl): xyz the grid's corner (agent), w a cell
            F32 bottom = 0.f;                       // the box's bottom (agent z)
            F32 inv_height = 1.f;                   // 1 / its height
            F64 started = 0.0;                      // LLFrameTimer::getElapsedSeconds()
            F32 seconds = 1.f;
            U32 vertices = 0;
            U64 bytes = 0;
        };

        // The live ghosts are [0, detail::sLiveCount).
        Ghost sGhosts[MAX_GHOSTS];
        U32 sVertices = 0;          // of the live ghosts
        U32 sFaces = 0;
        U64 sBytes = 0;

        F64 sRegionChangedAt = -1.0e9;
        U32 sStartFrame = 0;
        U32 sStartsThisFrame = 0;

        // Reused at each kill (no allocation once they have grown).
        std::vector<LLVOVolume*> sPrims;
        std::vector<GhostFace> sScratch;
        std::unordered_set<const LLVertexBuffer*> sBuffersSeen;

        const LLStaticHashedString sModel("derez_model");
        const LLStaticHashedString sGrid("derez_grid");
        const LLStaticHashedString sShape("derez_shape");
        const LLStaticHashedString sColour("derez_color");
        const LLStaticHashedString sFace("derez_face");
        const LLStaticHashedString sEmissive("derez_emissive");
        const LLStaticHashedString sUV0("derez_uv0");
        const LLStaticHashedString sUV1("derez_uv1");
        const LLStaticHashedString sGlow("derez_glow");

        struct Box
        {
            LLVector3 min;
            LLVector3 max;

            LLVector3 size() const { return max - min; }
            F32 largest() const
            {
                const LLVector3 s = size();
                return llmax(s.mV[VX], s.mV[VY], s.mV[VZ]);
            }
        };

        bool enabled()
        {
            static LLCachedControl<bool> on(gSavedSettings, "PlusDerez", true);
            return on;
        }

        bool teleporting()
        {
            return gAgent.getTeleportState() != LLAgent::TELEPORT_NONE;
        }

        // The main view's G-buffer (as plus/avatarrefl's live()): not a probe face, a mirror, a shadow
        // map, an impostor, the water's reflection or the HUDs.
        bool mainView()
        {
            return !gCubeSnapshot && gPipeline.mRT == &gPipeline.mMainRT && !LLPipeline::sShadowRender &&
                   !LLPipeline::sImpostorRender && !LLPipeline::sReflectionRender && !LLPipeline::sRenderingHUDs;
        }

        void endGhost(U32 index)
        {
            Ghost& ghost = sGhosts[index];
            sVertices -= llmin(sVertices, ghost.vertices);
            sFaces -= llmin(sFaces, (U32)ghost.faces.size());
            sBytes -= llmin(sBytes, ghost.bytes);
            std::vector<GhostFace>().swap(ghost.faces);     // its buffers and textures go now
            const U32 last = --detail::sLiveCount;
            if (index != last)
            {
                std::swap(sGhosts[index], sGhosts[last]);
            }
        }

        void expire(F64 now)
        {
            for (U32 i = 0; i < detail::sLiveCount; )
            {
                const Ghost& ghost = sGhosts[i];
                if (now - ghost.started >= (F64)ghost.seconds)
                {
                    endGhost(i);
                }
                else
                {
                    ++i;
                }
            }
        }

        // A slot and this frame's share of starts (PlusDerezMax, PlusDerezStartsPerFrame).
        bool startAllowed()
        {
            static LLCachedControl<S32> max_ghosts(gSavedSettings, "PlusDerezMax", 24);
            static LLCachedControl<S32> per_frame(gSavedSettings, "PlusDerezStartsPerFrame", 6);
            if (sStartFrame != gFrameCount)
            {
                sStartFrame = gFrameCount;
                sStartsThisFrame = 0;
            }
            return detail::sLiveCount < (U32)llclamp((S32)max_ghosts, 0, (S32)MAX_GHOSTS) &&
                   sStartsThisFrame < (U32)llclamp((S32)per_frame, 0, (S32)MAX_GHOSTS);
        }

        bool moving(const LLViewerObject* object)
        {
            return object->getVelocity().magVecSquared() > MOVING_SPEED * MOVING_SPEED;
        }

        // The object as a whole: true when it is left out (see derez.h).
        bool leftOut(LLViewerObject* objectp)
        {
            if (objectp->getPCode() != LL_PCODE_VOLUME || objectp->isAvatar() || objectp->isDead() ||
                objectp->isAttachment() || objectp->isHUDAttachment())
            {
                return true;
            }
            const LLViewerObject* root = objectp->getRootEdit();
            if (!root || root->isAttachment() || root->isHUDAttachment() || root->isAnimatedObject() ||
                root->flagUsePhysics() || objectp->flagUsePhysics() || moving(root) || moving(objectp))
            {
                return true;
            }
            const LLViewerRegion* region = objectp->getRegion();
            if (!region)
            {
                return true;
            }
            const LLVector3& position = objectp->getPositionRegion();
            const F32 width = region->getWidth();
            return !position.isFinite() || position.mV[VX] < EDGE_MARGIN || position.mV[VY] < EDGE_MARGIN ||
                   position.mV[VX] > width - EDGE_MARGIN || position.mV[VY] > width - EDGE_MARGIN;
        }

        // The prim, when it can be a part of a ghost; false when someone sits on it (the whole object
        // is left out then). Rigged meshes and prims without a drawable are skipped.
        bool addPrim(LLViewerObject* object)
        {
            for (const LLPointer<LLViewerObject>& child : object->getChildren())
            {
                if (child.notNull() && child->isAvatar())
                {
                    return false;
                }
            }
            if (object->getPCode() != LL_PCODE_VOLUME || object->isDead())
            {
                return true;
            }
            LLVOVolume* prim = static_cast<LLVOVolume*>(object);
            if (!prim->isRiggedMesh() && prim->mDrawable.notNull() && !prim->mDrawable->isDead())
            {
                sPrims.push_back(prim);
            }
            return true;
        }

        // sPrims: the killed prim and, when it is a root, the rest of its linkset (the root's kill
        // kills them all: markDead). False when the object is left out as a whole.
        bool gatherPrims(LLViewerObject* objectp)
        {
            sPrims.clear();
            if (!addPrim(objectp))
            {
                return false;
            }
            if (objectp->isRoot())
            {
                for (const LLPointer<LLViewerObject>& child : objectp->getChildren())
                {
                    if (child.notNull() && !child->isAvatar() && !addPrim(child.get()))
                    {
                        return false;
                    }
                }
            }
            return !sPrims.empty();
        }

        // The linkset's box in the agent's frame (the prims' rotated boxes). False when it is not a
        // size to crumble (or not finite).
        bool boxOf(Box& box)
        {
            box.min.setVec(F32_MAX, F32_MAX, F32_MAX);
            box.max.setVec(-F32_MAX, -F32_MAX, -F32_MAX);
            for (const LLVOVolume* prim : sPrims)
            {
                const LLVector3 centre = prim->getPositionAgent();
                const LLVector3 half = prim->getScale() * 0.5f;
                const LLQuaternion rotation = prim->getRenderRotation();
                const LLVector3 axes[3] = { LLVector3(half.mV[VX], 0.f, 0.f) * rotation,
                                            LLVector3(0.f, half.mV[VY], 0.f) * rotation,
                                            LLVector3(0.f, 0.f, half.mV[VZ]) * rotation };
                for (U32 i = 0; i < 3; ++i)
                {
                    const F32 reach = fabsf(axes[0].mV[i]) + fabsf(axes[1].mV[i]) + fabsf(axes[2].mV[i]);
                    box.min.mV[i] = llmin(box.min.mV[i], centre.mV[i] - reach);
                    box.max.mV[i] = llmax(box.max.mV[i], centre.mV[i] + reach);
                }
            }
            const F32 largest = box.largest();
            return box.min.isFinite() && box.max.isFinite() && largest >= MIN_SIZE && largest <= MAX_SIZE;
        }

        bool nearCamera(const Box& box)
        {
            static LLCachedControl<F32> max_distance(gSavedSettings, "PlusDerezMaxDistance", 40.f);
            const F32 limit = llclamp(llfinite((F32)max_distance) ? (F32)max_distance : 40.f, 0.f, 512.f);
            const LLVector3 eye = LLViewerCamera::getInstance()->getOrigin();
            LLVector3 closest;
            for (U32 i = 0; i < 3; ++i)
            {
                closest.mV[i] = llclamp(eye.mV[i], box.min.mV[i], box.max.mV[i]);
            }
            return dist_vec_squared(eye, closest) <= limit * limit;
        }

        // Drawn in the main view in this frame or the one before: the drawable itself (active objects,
        // and static ones when their group's LOD was sorted), or its group for the world camera (a
        // static drawable is not marked one by one while its group is drawn).
        bool seenLately(const LLDrawable* drawable)
        {
            if (drawable->isRecentlyVisible())
            {
                return true;
            }
            const LLSpatialGroup* group = drawable->getSpatialGroup();
            return group && LLViewerOctreeEntryData::getCurrentFrame() - group->getVisible(LLViewerCamera::CAMERA_WORLD) < 2;
        }

        bool anySeenLately()
        {
            for (const LLVOVolume* prim : sPrims)
            {
                if (seenLately(prim->mDrawable.get()))
                {
                    return true;
                }
            }
            return false;
        }

        // How far the prim's vertices move with their cells (0-1). A vertex moved with its cell drags
        // its triangles across to the cells that have not moved yet, so a coarse mesh (a box prim: four
        // corners a side) stays in place and only crumbles in derezF.glsl; a fine one flies in blocks.
        F32 warpOf(const LLVOVolume* prim, U32 vertices, F32 cell)
        {
            const LLVector3& s = prim->getScale();
            const F32 area = 2.f * (s.mV[VX] * s.mV[VY] + s.mV[VY] * s.mV[VZ] + s.mV[VZ] * s.mV[VX]);
            if (!vertices || !(area > 0.f))
            {
                return 0.f;
            }
            const F32 spacing = sqrtf(area / (F32)vertices);
            return llclamp((2.5f * cell - spacing) / (1.5f * cell), 0.f, 1.f);
        }

        // The face's model matrix as registerFace() (llvovolume.cpp) picks it: rigged faces are not here.
        const LLMatrix4* modelOf(const LLDrawable* drawable)
        {
            if (drawable->isState(LLDrawable::ANIMATED_CHILD))
            {
                return &drawable->getWorldMatrix();
            }
            if (drawable->isActive())
            {
                return &drawable->getRenderMatrix();
            }
            const LLViewerRegion* region = drawable->getRegion();
            return region ? &region->mRenderMatrix : nullptr;
        }

        // What a face looks like in its pass: the uniforms of derezF.glsl before they are packed (the
        // cells' clock's, derez_grid and derez_shape, are derezClock.glsl's: Ghost).
        struct Look
        {
            LLViewerTexture* texture = nullptr;
            LLColor4 colour;                        // linear
            U32 mode = ALPHA_OPAQUE;
            F32 cutoff = 0.f;
            F32 fullbright = 0.f;
            LLColor3 emissive;
            LLGLTFMaterial::TextureTransform::Pack uv = { 1.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
        };

        // PBR (research/257 5): the base colour's texture (or the face's media), factor and texture
        // transform, the material's alpha mode, and its emissive factor when no texture masks it (else
        // the whole face would glow).
        void pbrLook(LLFace* facep, LLFetchedGLTFMaterial* gltf, Look& look)
        {
            LLViewerTexture* face_texture = facep->getTexture();
            const bool media = facep->hasMedia() && face_texture && face_texture->getType() == LLViewerTexture::MEDIA_TEXTURE;
            look.texture = media ? face_texture : gltf->mBaseColorTexture.get();
            look.colour = gltf->mBaseColor;
            look.mode = gltf->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_BLEND ? ALPHA_BLEND :
                        gltf->mAlphaMode == LLGLTFMaterial::ALPHA_MODE_MASK ? ALPHA_MASK : ALPHA_OPAQUE;
            look.cutoff = gltf->mAlphaCutoff;
            if (gltf->mTextureId[LLGLTFMaterial::GLTF_TEXTURE_INFO_EMISSIVE].isNull())
            {
                look.emissive = gltf->mEmissiveColor;
            }
            gltf->mTextureTransform[LLGLTFMaterial::GLTF_TEXTURE_INFO_BASE_COLOR].getPacked(look.uv);
        }

        // The legacy face: its diffuse texture and TE colour, alpha as llvovolume.cpp's registerFace() sorts
        // it (the alpha pool: a mask when it can be drawn as one, else blended; a material's mask), fullbright.
        // False for an invisiprim.
        bool legacyLook(LLFace* facep, const LLTextureEntry* te, Look& look)
        {
            look.texture = facep->getTexture();
            if (look.texture && look.texture->getPrimaryFormat() == GL_ALPHA)
            {
                return false;
            }
            look.colour = linearColor4(te->getColor());
            look.fullbright = te->getFullbright() ? 1.f : 0.f;
            const LLMaterial* material = te->getMaterialParams().get();
            if (facep->getPoolType() == LLDrawPool::POOL_ALPHA)
            {
                look.mode = facep->canRenderAsMask() ? ALPHA_MASK : ALPHA_BLEND;
                look.cutoff = LEGACY_MASK_CUTOFF;
            }
            else if (material && material->getDiffuseAlphaMode() == LLMaterial::DIFFUSE_ALPHA_MODE_MASK)
            {
                look.mode = ALPHA_MASK;
                look.cutoff = material->getAlphaMaskCutoff() / 255.f;
            }
            return true;
        }

        // The face's colour, alpha, texture and light into out. False: not drawn (an invisible prim, a
        // face too faint, no texture to bind at all).
        bool lookOf(LLFace* facep, const LLTextureEntry* te, GhostFace& out)
        {
            Look look;
            LLFetchedGLTFMaterial* gltf = (LLFetchedGLTFMaterial*)te->getGLTFRenderMaterial();
            if (gltf)
            {
                pbrLook(facep, gltf, look);
            }
            else if (!legacyLook(facep, te, look))
            {
                return false;
            }
            if (!(look.colour.mV[VALPHA] >= MIN_FACE_ALPHA))
            {
                return false;
            }
            if (!look.texture || !look.texture->getGLTexture())
            {
                look.texture = LLViewerFetchedTexture::sWhiteImagep.get();
            }
            if (!look.texture || !look.texture->getGLTexture())
            {
                return false;
            }
            out.texture = look.texture;
            out.colour.setVec(look.colour.mV[VRED], look.colour.mV[VGREEN], look.colour.mV[VBLUE], look.colour.mV[VALPHA]);
            out.face.setVec((F32)look.mode, look.cutoff, look.fullbright, 0.f);
            out.emissive.setVec(look.emissive.mV[VRED], look.emissive.mV[VGREEN], look.emissive.mV[VBLUE], 0.f);
            out.uv[0].setVec(look.uv[0], look.uv[1], look.uv[2], look.uv[3]);
            out.uv[1].setVec(look.uv[4], look.uv[5], look.uv[6], look.uv[7]);
            return true;
        }

        // The vertices of all the prim's faces (for warpOf()).
        U32 primVertices(const LLDrawable* drawable)
        {
            U32 vertices = 0;
            for (S32 i = 0; i < drawable->getNumFaces(); ++i)
            {
                const LLFace* facep = drawable->getFace(i);
                vertices += facep ? facep->getGeomCount() : 0;
            }
            return vertices;
        }

        // A face with geometry derezV.glsl can draw: the attributes it reads, and its ranges inside its
        // buffer (as they were when it was last built).
        bool drawableRange(const LLFace* facep, const LLVertexBuffer* buffer)
        {
            return buffer && facep->getIndicesCount() && facep->getGeomCount() &&
                   (buffer->getTypeMask() & NEEDED_MASK) == NEEDED_MASK &&
                   (U32)facep->getGeomIndex() + facep->getGeomCount() <= buffer->getNumVerts() &&
                   (U32)facep->getIndicesStart() + facep->getIndicesCount() <= buffer->getNumIndices();
        }

        // sScratch: the faces of the prims in sPrims, within the budgets. False when over them (no
        // ghost then). vertices and bytes: the ghost's.
        bool collectFaces(F32 cell, U32& vertices, U64& bytes)
        {
            sScratch.clear();
            sBuffersSeen.clear();
            vertices = 0;
            bytes = 0;
            for (LLVOVolume* prim : sPrims)
            {
                LLDrawable* drawable = prim->mDrawable.get();
                const LLMatrix4* model = modelOf(drawable);
                if (!model)
                {
                    continue;
                }
                const F32 warp = warpOf(prim, primVertices(drawable), cell);
                for (S32 i = 0; i < drawable->getNumFaces(); ++i)
                {
                    LLFace* facep = drawable->getFace(i);
                    LLVertexBuffer* buffer = facep ? facep->getVertexBuffer() : nullptr;
                    const LLTextureEntry* te = facep ? facep->getTextureEntry() : nullptr;
                    if (!te || !drawableRange(facep, buffer))
                    {
                        continue;
                    }
                    GhostFace face;
                    if (!lookOf(facep, te, face))
                    {
                        continue;
                    }
                    face.buffer = buffer;
                    face.model = *model;
                    face.face.mV[VW] = warp;
                    face.start = facep->getGeomIndex();
                    face.end = face.start + facep->getGeomCount() - 1;
                    face.count = facep->getIndicesCount();
                    face.offset = (U32)facep->getIndicesStart();
                    vertices += facep->getGeomCount();
                    if (sBuffersSeen.insert(buffer).second)
                    {
                        bytes += (U64)buffer->getSize() + buffer->getIndicesSize();
                    }
                    if (sScratch.size() >= MAX_FACES_PER_GHOST || vertices > MAX_VERTICES_PER_GHOST)
                    {
                        sScratch.clear();
                        return false;
                    }
                    sScratch.push_back(std::move(face));
                }
            }
            sBuffersSeen.clear();
            if (sScratch.empty() || sVertices + vertices > MAX_VERTICES || sFaces + (U32)sScratch.size() > MAX_FACES ||
                sBytes + bytes > MAX_BUFFER_BYTES)
            {
                sScratch.clear();
                return false;
            }
            return true;
        }

        void start(LLViewerObject* objectp, F64 now)
        {
            if (leftOut(objectp) || !gatherPrims(objectp))
            {
                return;
            }
            Box box;
            if (!boxOf(box) || !nearCamera(box) || !anySeenLately())
            {
                return;
            }
            const F32 largest = box.largest();
            const LLViewerObject* root = objectp->getRootEdit();
            if (root && root->flagTemporaryOnRez() && largest < SMALL_TEMPORARY)
            {
                return;
            }

            const F32 cell = llclamp(largest / CELLS_ACROSS, MIN_CELL, MAX_CELL);
            U32 vertices = 0;
            U64 bytes = 0;
            if (!collectFaces(cell, vertices, bytes))
            {
                return;
            }

            static LLCachedControl<F32> seconds(gSavedSettings, "PlusDerezSeconds", 1.2f);
            Ghost& ghost = sGhosts[detail::sLiveCount];
            ghost.faces.reserve(sScratch.size());
            ghost.faces.assign(std::make_move_iterator(sScratch.begin()), std::make_move_iterator(sScratch.end()));
            sScratch.clear();
            // the grid's corner half a cell out, so the box's own faces (a prim's sides) lie in the
            // middle of their cells rather than on the cuts between them
            ghost.grid.setVec(box.min.mV[VX] - 0.5f * cell, box.min.mV[VY] - 0.5f * cell, box.min.mV[VZ] - 0.5f * cell, cell);
            ghost.bottom = box.min.mV[VZ];
            ghost.inv_height = 1.f / llmax(box.size().mV[VZ], cell);
            ghost.started = now;
            ghost.seconds = llclamp(llfinite((F32)seconds) ? (F32)seconds : 1.2f, 0.4f, 4.f);
            ghost.vertices = vertices;
            ghost.bytes = bytes;
            ++detail::sLiveCount;
            sVertices += vertices;
            sFaces += (U32)ghost.faces.size();
            sBytes += bytes;
            ++sStartsThisFrame;
        }

        // The glow's colour and brightness for derez_glow (linear, the scene's units): the palette's
        // colour at its full strength (the brightest channel 1), at GLOW_SCREEN on the screen whatever
        // the exposure (plus::effectiveExposure()), held under 1 under "no post".
        LLColor3 glowColour()
        {
            static LLCachedControl<S32> palette(gSavedSettings, "PlusDerezPalette", 0);
            static LLCachedControl<LLColor3> custom(gSavedSettings, "PlusDerezColor", PALETTE[0]);
            const S32 index = palette;
            const LLColor3 srgb = plus::objectfx::paletteColour(index == PALETTE_CUSTOM ? PALETTE_CUSTOM
                                                                : llclamp(index, 0, PALETTE_SIZE - 1), custom);
            const LLColor3 linear = linearColor3(srgb);
            const F32 brightest = llmax(linear.mV[VRED], linear.mV[VGREEN], linear.mV[VBLUE]);
            const F32 strength = plus::isNoPost() ? GLOW_NO_POST : GLOW_SCREEN / plus::effectiveExposure();
            return linear * (strength / brightest);
        }

        void drawGhost(const Ghost& ghost, F64 now)
        {
            LLGLSLShader& shader = gDeferredDerezProgram;
            const F32 time = (F32)llclamp((now - ghost.started) / (F64)ghost.seconds, 0.0, 1.0);
            shader.uniform4fv(sGrid, 1, ghost.grid.mV);
            shader.uniform4f(sShape, ghost.bottom, ghost.inv_height, time, 0.f);
            for (const GhostFace& face : ghost.faces)
            {
                LLVertexBuffer* buffer = face.buffer.get();
                if (buffer->isMapped())
                {
                    continue;   // being written this frame (pushDirectBatch() leaves such a buffer too)
                }
                shader.uniformMatrix4fv(sModel, 1, GL_FALSE, (const F32*)face.model.mMatrix);
                shader.uniform4fv(sColour, 1, face.colour.mV);
                shader.uniform4fv(sFace, 1, face.face.mV);
                shader.uniform4fv(sEmissive, 1, face.emissive.mV);
                shader.uniform4fv(sUV0, 1, face.uv[0].mV);
                shader.uniform4fv(sUV1, 1, face.uv[1].mV);
                shader.bindTexture(LLShaderMgr::DIFFUSE_MAP, face.texture.get());
                buffer->setBuffer();
                buffer->drawRange(LLRender::TRIANGLES, face.start, face.end, face.count, face.offset);
            }
        }

        void onRegionChanged()
        {
            sRegionChangedAt = LLFrameTimer::getElapsedSeconds();
            clear();
        }

        void init()
        {
            // Settings: app_settings/settings_plus.xml (PlusDerez*). Hooks: process_kill_object()
            // (llviewermessage.cpp), LLPipeline::renderGeomDeferred() (pipeline.cpp), the program in
            // llviewershadermgr.cpp. research/257.
            gAgent.addRegionChangedCallback(&onRegionChanged);
            // The app quitting (or failing): the ghosts' vertex buffers and textures go while the
            // renderer is up, not in sGhosts' static destructor after it (as plus/hairsway does).
            LLEventPumps::instance().obtain("LLApp").listen("PlusDerez", [](const LLSD& event)
            {
                if (event["status"].asString() != "running" && on_main_thread())
                {
                    clear();
                }
                return false;
            });
        }

        plus::FeatureRegistration sRegistration({ "derez", &init });
    }

    void detail::render()
    {
        if (!enabled() || teleporting() || LLApp::isExiting())
        {
            clear();
            return;
        }
        const F64 now = LLFrameTimer::getElapsedSeconds();
        expire(now);
        if (!sLiveCount || !mainView() || !gDeferredDerezProgram.isComplete())
        {
            return;
        }

        LL_PROFILE_ZONE_SCOPED_CATEGORY_DRAWPOOL;
        LLRHI::gpuMark("derez");
        {
            LLGLDepthTest depth(GL_TRUE, GL_TRUE, GL_LEQUAL);
            LLGLDisable blend(GL_BLEND);
            // both sides: through the holes the far side's blocks show (lit towards the eye: derezF.glsl's
            // normal), and a double-sided PBR face is whole. The cull mode is not in the pipeline's key.
            LLGLDisable cull(GL_CULL_FACE);
            gDeferredDerezProgram.bind();
            const LLColor3 glow = glowColour();
            gDeferredDerezProgram.uniform4f(sGlow, glow.mV[VRED], glow.mV[VGREEN], glow.mV[VBLUE], GLOW_INTO_COLOUR);
            gGL.matrixMode(LLRender::MM_MODELVIEW);
            gGL.loadMatrix(gGLModelView);
            for (U32 i = 0; i < sLiveCount; ++i)
            {
                drawGhost(sGhosts[i], now);
            }
            gGLLastMatrix = nullptr;    // the pools' applyModelMatrix() loads its own again
            gDeferredDerezProgram.unbind();
        }
        LLVertexBuffer::unbind();
        LLRHI::gpuMark("gbuffer");
    }

    void onKill(LLViewerObject* objectp)
    {
        if (!objectp || !enabled())
        {
            return;
        }
        if (LLApp::isExiting() || teleporting())
        {
            clear();
            return;
        }
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (now - sRegionChangedAt < REGION_CHANGE_QUIET)
        {
            return;
        }
        expire(now);
        if (startAllowed())
        {
            start(objectp, now);
        }
    }

    void clear()
    {
        while (detail::sLiveCount)
        {
            endGhost(detail::sLiveCount - 1);
        }
        sVertices = 0;
        sFaces = 0;
        sBytes = 0;
    }
}
