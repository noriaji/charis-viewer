/**
 * @file skinsss.h
 * @brief Skin subsurface scattering (research/100). Stage 1: the skin mark
 *        in the main view's stencil, and a view of it.
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

#ifndef PLUS_SKINSSS_H
#define PLUS_SKINSSS_H

#include "llgl.h"

class LLCamera;
class LLDrawInfo;
class LLFace;
class LLVOAvatar;

// The hooks below are the <plus> lines in official files. Each returns at
// once (a flag test) unless skin is being marked: only while something needs
// the mark (PlusSkinSssDebug, or a mark user such as plus/skinfill, all off by
// default) and only with the Vulkan backend (not with LLRHI_SKIN_SSS=0).
namespace plus::skinsss
{
    namespace detail
    {
        // Set by GBufferScope while it marks skin.
        extern bool sMarking;
        bool beginBatch(const LLDrawInfo& params);
        void endBatch();
        // While marking: whether params is a batch of a rigid object worn on
        // an eye attachment point (beginBatch may mark it, though it has no
        // avatar): the draw lists leave it to pushDirectBatch as they leave
        // an avatar's batches. Read only during the pass (cull threads too).
        bool isEyeAttachment(const LLDrawInfo& params);
    }

    // In LLPipeline::allocateScreenBuffer(), after the screen buffers are made:
    // while skin is marked, the main view's depth is made again with a stencil
    // (same size; both of its framebuffers pick it up by name).
    void onScreenBuffers();

    // Features that draw on the mark (plus/skinfill) register, from their
    // Feature::init, whether they need it now (cheap: asked at every G-buffer
    // pass). The mark is made while PlusSkinSssDebug is on or any user wants it.
    void addMarkUser(bool (*wanted)());

    // The same for features that need the avatar bit (LLRHI::STENCIL_AVATAR:
    // every batch of an avatar, at any distance; plus/wetlook keeps avatars
    // dry), with LLRHI::STENCIL_AVATAR_SKIN on its bare skin and eyes (no
    // snow there). The bits are written only while one of them wants it.
    void addAvatarMarkUser(bool (*wanted)());

    // When what a user wants changes outside the frame (its setting, on the
    // main thread): the main view's depth gets or loses its stencil at once,
    // in place (same name and size), so the mark costs nothing while no one
    // wants it. The snapshot boost's start and end call it too.
    void markWantChanged();

    // Whether this frame's G-buffer pass of the main view left the mark with
    // something in it (a skin or eye batch, a system body or eyeballs).
    // Passes on the mark record nothing otherwise.
    bool markedThisFrame();

    // Whether the mark is wanted now (what GBufferScope asks first at the
    // main view's G-buffer pass). The retained scene (research/218 vk-s4b2)
    // leaves the worn objects' groups of the main view to the usual way while
    // it is: an object worn on an eye point is marked per batch.
    bool markingWanted();

    // research/219 (8.2): the retained scene's records in the main view's
    // G-buffer pass, which draws a pass's records with one mark.
    // mainViewMark, at the main view's postSort: whether its G-buffer pass
    // will mark (marking: the mark wanted and the main depth with a
    // stencil) and write the avatar bit, as far as can be told before it;
    // it gathers the eye attachments sceneMarkOf asks about.
    struct MarkPlan
    {
        bool marking = false;
        bool avatar_bit = false;
        U32 avatar_mark = 0;    // what a rigged batch of an avatar (not animesh) that is neither skin nor eyes writes
    };
    MarkPlan mainViewMark();
    // The mark the pass would give the batch, when it is one a pass of the
    // scene may draw with that mark: a rigged batch neither of its avatar's
    // skin nor eyes (the avatar bit by the plan), a batch of no avatar not of
    // an object worn on an eye point (0). False: the usual way marks it.
    // Read only (no marking state changes); after mainViewMark.
    bool sceneMarkOf(const LLDrawInfo& params, const MarkPlan& plan, U32& mark);
    // Around the scene's draw of a rigged pass's records in the main view's
    // G-buffer pass: those draws write mark while it marks (set back to 0
    // after). holds(): the pass marks as the plan said (else the records'
    // marks may be wrong: draw them the usual way, outside the scope).
    class SceneMarkScope
    {
    public:
        SceneMarkScope(U32 mark, const MarkPlan& plan);
        ~SceneMarkScope();
        bool holds() const { return mHolds; }
        SceneMarkScope(const SceneMarkScope&) = delete;
        SceneMarkScope& operator=(const SceneMarkScope&) = delete;

    private:
        const bool mHolds;
        bool mSet = false;
    };

    // Whether this frame's G-buffer pass of the main view wrote the avatar
    // bit (the stencil then tells avatars from the rest, even with none in
    // view).
    bool avatarsMarkedThisFrame();

    // Counts the times the screen buffers were made (onScreenBuffers): a
    // target of a mark user that shares the main view's depth is made again
    // when it changes.
    U32 screenBuffersGeneration();

    // At the start of LLPipeline::renderGeomDeferred(), for the whole G-buffer
    // pass of the main view (not probe faces, mirrors, impostors): clears the
    // stencil and has every draw that passes the depth test write the skin and
    // eye bits (0 unless a scope below sets one), so the bits end up telling
    // whether the nearest opaque surface is skin or eye.
    class GBufferScope
    {
    public:
        explicit GBufferScope(const LLCamera& camera);
        ~GBufferScope();
        GBufferScope(const GBufferScope&) = delete;
        GBufferScope& operator=(const GBufferScope&) = delete;

    private:
        const bool mActive;
        LLGLState mStencilTest;
    };

    // At the start of LLRenderPass::pushDirectBatch(): a batch of an avatar's
    // Bakes on Mesh skin writes the skin bit, one of its eyes (the eyes bake,
    // a mesh weighted to the eye joints only, or an object worn on an eye
    // attachment point, rigged or not, while its face is skin) the
    // eye bit, any batch of an avatar the avatar bit (while it is wanted).
    // The batch is drawn (recorded) within the call, so the bits are set
    // back when the call returns. A batch pushDirectBatch refuses is drawn
    // through GL by its caller after it returns: the caller has its own scope
    // around that draw (when = the refusal; lldrawpool.cpp, fix-round7).
    class BatchScope
    {
    public:
        explicit BatchScope(const LLDrawInfo& params, bool when = true)
            : mMarked(when && detail::sMarking && detail::beginBatch(params)) {}
        ~BatchScope()
        {
            if (mMarked)
            {
                detail::endBatch();
            }
        }
        BatchScope(const BatchScope&) = delete;
        BatchScope& operator=(const BatchScope&) = delete;

    private:
        const bool mMarked;
    };

    // Around LLVOAvatar::renderSkinned() in LLDrawPoolAvatar::renderAvatars():
    // the system body (in the G-buffer pass only its opaque head, upper and
    // lower body) is skin, and avatar.
    class SystemBodyScope
    {
    public:
        explicit SystemBodyScope(const LLVOAvatar* avatar);
        ~SystemBodyScope();
        SystemBodyScope(const SystemBodyScope&) = delete;
        SystemBodyScope& operator=(const SystemBodyScope&) = delete;

    private:
        const U32 mMark;    // the bits written (0: none, nothing to set back)
    };

    // Around LLVOAvatar::renderRigid() in LLDrawPoolAvatar::renderAvatars():
    // the system eyeballs are eyes (STENCIL_EYE) while the avatar's face is
    // skin (its head bake or system head was marked in the last frames), and
    // avatar.
    class SystemEyesScope
    {
    public:
        explicit SystemEyesScope(const LLVOAvatar* avatar);
        ~SystemEyesScope();
        SystemEyesScope(const SystemEyesScope&) = delete;
        SystemEyesScope& operator=(const SystemEyesScope&) = delete;

    private:
        const U32 mMark;    // the bits written (0: none, nothing to set back)
    };

    // In LLPipeline::doOcclusion(): the occlusion boxes pass the depth test
    // without writing depth; they must not write the stencil either.
    class OcclusionPause
    {
    public:
        OcclusionPause();
        ~OcclusionPause();
        OcclusionPause(const OcclusionPause&) = delete;
        OcclusionPause& operator=(const OcclusionPause&) = delete;

    private:
        const bool mPaused;
        LLGLState mStencilTest;
    };

    // In LLVolumeGeometryManager::genDrawInfo(), where faces of different
    // textures are joined into one batch (texture index batching): faces of
    // different marks (an avatar's Bakes on Mesh skin, its eyes bake, other)
    // are never in the same batch, so every batch is all one mark.
    bool splitsBatch(const LLFace* first, const LLFace* next);

    // In LLPipeline::renderDeferredLighting(), after the opaque lighting and
    // before the alpha geometry, with mRT->screen bound. Stage 1: with
    // PlusSkinSssDebug, the skin pixels are tinted magenta.
    void render();
}

#endif // PLUS_SKINSSS_H
