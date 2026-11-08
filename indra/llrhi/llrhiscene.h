/**
 * @file llrhiscene.h
 * @brief The retained scene (research/197 stage 2): batches kept on the GPU, chosen and drawn per view by the GPU.
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

#ifndef LL_LLRHISCENE_H
#define LL_LLRHISCENE_H

#include "stdtypes.h"
#include "llrhi.h"      // TexRef (research/197 stage 1a, research/201)

#include <string>
#include <vector>

// The retained scene (research/197 2.2 and 3.3).
//
// The viewer registers a record per batch it wants the GPU to keep (an
// LLDrawInfo of a static or bridge group's opaque pass): where its indices are
// in the attribute pool, which render pass and vertex format it draws with,
// its transform, its texture transform, its material and its bounds. Records
// are written when they are made or change, never per frame. A group is a set
// of records whose visibility the CPU cull decides per view.
//
// Per frame and view, the viewer gives the view's planes and the visible
// groups (one bit each); drawPass then draws a pass's records of the visible
// groups whose bounds are not outside the view: a compute shader keeps them in
// the order of the record table (a bucket's pages, then the lanes in a page;
// research/197 2.2 "順"), and each bucket (pass x vertex format x pool chunk x
// index size) is one indirect draw with a count.
//
// Ids: a record id stays the record's while it lives (updates that move it to
// another bucket keep it). Transform and texture transform slot 0 is the
// identity (none). Group ids are small numbers (bit positions in a view's
// visible bits) and are given again soon after removal.
namespace LLRHI
{
    // LLRHI_SCENE: off, verify (the GPU culls, the old path draws, and both
    // are compared), auto (verify until 600 world frames agree, then on; one
    // difference: off), on. Unset: SCENE_DEFAULT.
    enum class SceneMode : U8
    {
        Off,
        Verify,
        Auto,
        On,
    };
    // Off. The scene draws since research/198 5.5 (LLRHI_SCENE=on, or auto
    // after its clean frames), the main view's opaque G-buffer batches of one
    // diffuse texture first (5.6: and the masks, the legacy materials, the
    // fullbright and glow batches), but it has never been looked at on a real GPU:
    // it becomes Auto after a visual check (capture-diff); the device
    // then requires what the scene draws with (llvkdevice.cpp
    // missing_required: any mode but Off asks for it).
    constexpr SceneMode SCENE_DEFAULT = SceneMode::Off;
    SceneMode sceneMode();
    const char* sceneModeName(SceneMode mode);

    // research/218 vk-s2f: what LLRHI_SCENE_ALPHA, _SHADOW and _PROBE are
    // when unset. Each is off whenever LLRHI_SCENE is off, and its auto is
    // read as verify while LLRHI_SCENE is verify (nothing of the scene is
    // drawn then).
    constexpr SceneMode SCENE_ALPHA_DEFAULT = SceneMode::Off;
    constexpr SceneMode SCENE_SHADOW_DEFAULT = SceneMode::Off;
    constexpr SceneMode SCENE_PROBE_DEFAULT = SceneMode::Off;

    // research/218 vk-s2f: auto of a part of the scene (LLRHI_SCENE_ALPHA,
    // _SHADOW, _PROBE): verify until 600 world frames of the part's own views
    // (frames with at least one of its checks) had no difference, then on;
    // one difference (its own checks, the backend's, a failed draw): off for
    // the run. Pure (selftest sim-sceneauto takes it from here); the viewer
    // keeps one per part and asks it what the part does now.
    // ==== scene auto types: begin ====
    struct SceneAuto
    {
        static constexpr U32 CLEAN_FRAMES = 600;
        SceneMode now = SceneMode::Verify;  // Verify, On or Off
        U32 clean_frames = 0;               // world frames with checks and no difference so far
        U32 last_frame = 0;
        bool counted_any = false;
        // One check in `frame` (several in a frame count once); `differ`: it
        // found a difference. Returns what the part does from now on.
        SceneMode step(U32 frame, bool differ);
    };
    // ==== scene auto types: end ====
    // The mode a part goes on in: as asked, or auto's progress.
    inline SceneMode scenePartNow(SceneMode asked, const SceneAuto& progress)
    {
        return asked == SceneMode::Auto ? progress.now : asked;
    }

    // research/205 (stage 3a): the alpha pool's batches on the scene's way.
    // LLRHI_SCENE_ALPHA: off (default), verify (the alpha pool draws as
    // before; the viewer keeps an alpha record per batch and compares what
    // the records would draw, in which order and with which OIT priority,
    // with what the pool drew; in an OIT "all" session the scene also
    // chooses and expands the main view's plain and fullbright alpha
    // records and compares its rows, priorities included, with the CPU's),
    // on (research/205 section 9: in an OIT "all" session those records are
    // drawn by the scene, while the scene draws: LLRHI_SCENE on, or auto
    // once it draws; the rest of the alpha as before; 3a-4: the legacy
    // materials' batches too; 3a-5: in a hybrid session's world half and
    // without OIT, span by span in the pool's order). Off whenever
    // LLRHI_SCENE is off. auto (research/218 vk-s2f): verify until the
    // alpha's checks (llscenealpha's, the alpha view's spans, the backend's)
    // agreed 600 world frames, then on; a difference: off. While it is not off the
    // alpha programs read draw rows (llviewershadermgr.cpp) and their OIT
    // variants read the priority from the draw's row
    // (llvkgl_oitshaders.cpp): with it off the shaders stay as the bundle
    // has them baked.
    SceneMode sceneAlphaMode();

    // LLRHI_SCENE_SHADOW (research/206, stage 5): off, verify (the sun
    // shadow cascades' views are chosen and compared, the usual path draws),
    // on (the scene draws what the viewer cut). Unset: SCENE_SHADOW_DEFAULT.
    // Off whenever LLRHI_SCENE is off (the records are stage 2's). auto
    // (research/218 vk-s2f): verify until the layers' checks agreed 600
    // world frames, then on; a difference: off.
    SceneMode sceneShadowMode();

    // LLRHI_SCENE_PROBE (research/209 stage 2e): the reflection probes'
    // faces. off (default: their batches are drawn the usual way and the
    // scene is not asked), verify (the faces are chosen and compared in turn,
    // the usual path draws), on (the scene draws what the viewer cut of a
    // face while the scene draws). Off whenever LLRHI_SCENE is off; auto
    // (research/218 vk-s2f, research/209 4.3 7): verify until the faces'
    // checks agreed 600 world frames, then on; a difference: off (the faces
    // are then not asked of the scene). The mirrors (the hero probe) and the water mirror
    // (plus/avatarrefl) follow LLRHI_SCENE as the main view does: cut while
    // the scene draws, compared in turn while it does not. A face costs a
    // choice (3 dispatches and barriers) and an expand per pass (research/209
    // 3.4): until the choices of a frame's faces are recorded together (209
    // E4) the probes' faces may cost more than they save, hence the switch.
    SceneMode sceneProbeMode();

    // LLRHI_SCENE_RIGGED (research/219): the rigged batches (worn rigged
    // meshes, animesh) on the scene's way. off (default, unset too: they
    // keep no records, as before), verify (records are made and chosen and
    // compared in the main view, the probes', the mirrors' and the shadows';
    // the usual path draws), auto (verify until the rigged batches' checks
    // agreed 600 world frames, then on; a difference: off for the run), on
    // (the scene draws what the viewer cut). Off whenever LLRHI_SCENE is
    // off; auto read as verify while LLRHI_SCENE is verify. A rigged record
    // reads its joints from its avatar x skin's resident palette row
    // (LLRHIAvatar, research/199): while this is not off and
    // LLRHI_AVATAR_PALETTE is unset, the rows are written as its verify.
    constexpr SceneMode SCENE_RIGGED_DEFAULT = SceneMode::Off;
    SceneMode sceneRiggedMode();

    // LLRHI_SCENE_WORLD (research/204 10, vk-s4c2): the terrain's, the
    // trees' and the water's faces (their pools' face lists, not render
    // maps) on the scene's way, in the main view. off (default, unset too:
    // no records), verify (records are made, the main view's world groups
    // are chosen, and the pools' face lists compared with the choice; the
    // pools draw every face), auto (verify until 600 world frames of those
    // checks had no difference, then on; one: off for the run), on (while
    // the scene draws, a world pool's pass is drawn from the records of
    // the groups whose every face matched its record this view, and the
    // pool's loop leaves their faces out). Off whenever LLRHI_SCENE is off;
    // auto read as verify while LLRHI_SCENE is verify. While it is not off
    // the water's two programs read draw rows (llviewershadermgr.cpp).
    constexpr SceneMode SCENE_WORLD_DEFAULT = SceneMode::Off;
    SceneMode sceneWorldMode();

    // The views stage 2 draws (research/197 3.3). SCENE_VIEW_COUNT marks
    // "none of stage 2's" in the viewer. research/206 (stage 5): the sun
    // shadow cascades' views follow (drawShadow only; 5b: and the two spot
    // light shadows' after the alpha view); the backend holds SCENE_VIEWS
    // views.
    // research/205 3a-3: the main view's alpha (an OIT "all" session's plain
    // and fullbright batches) has a view of its own: the main view's planes,
    // the alpha groups the pool draws visible (drawPass only).
    // research/209 (stage 2e): the probe faces and the mirrors are views of
    // their own kind, each drawn face after face (a face's setView starts a
    // new choice for the kind): their shares of the frame's rows and their
    // refused passes are the kind's, never the main view's.
    enum SceneView : U32
    {
        SCENE_VIEW_MAIN = 0,
        SCENE_VIEW_PROBE = 1,       // the reflection probe face being drawn
        SCENE_VIEW_MIRROR = 2,      // a mirror's face (the hero probe)
        SCENE_VIEW_WATER = 3,       // research/209: the water mirror (plus/avatarrefl watermirror)
        SCENE_VIEW_COUNT = 4,
        SCENE_VIEW_SUN0 = 4,        // sun shadow cascade 0 (.. SCENE_VIEW_SUN_END - 1)
        SCENE_VIEW_SUN_END = 8,
        SCENE_VIEW_ALPHA = 8,       // research/205 3a-3
        SCENE_VIEW_SPOT0 = 9,       // research/206 5b: a spot light's shadow (.. SCENE_VIEW_SPOT_END - 1; drawShadow only)
        SCENE_VIEW_SPOT_END = 11,
        SCENE_VIEWS = 11,
    };
    // research/206: the views drawShadow takes (the sun cascades' and, 5b, the spot lights').
    inline bool sceneShadowView(U32 view)
    {
        return (view >= SCENE_VIEW_SUN0 && view < SCENE_VIEW_SUN_END) || (view >= SCENE_VIEW_SPOT0 && view < SCENE_VIEW_SPOT_END);
    }

    constexpr U32 SCENE_MAX_PASSES = 128;       // render types (LLRenderPass::eRenderType) the scene may hold
    constexpr U32 SCENE_MAX_PLANES = 7;         // LLCamera::AGENT_PLANE_USER_CLIP_NUM

    // Where a batch's indices are. The viewer names its vertex buffer by
    // number (stage 1b, LLRHI::BufferRef: the same for the buffer's whole
    // life) and the batch's indices in it; where that buffer is in the
    // attribute pool (chunk, vertex offset, first index there) is the
    // backend's to know, since the place moves (copy on write, the buffer
    // allocated again) while the number stays (research/202 2). Until the
    // backend resolves numbers to places (research/198 5) chunk and
    // vertex_offset stay 0 and first_index is in the buffer: such a record is
    // chosen (verify) but never drawn.
    struct SceneRange
    {
        U32 buffer = 0;             // the vertex buffer's number (BufferRef::id)
        U32 chunk = 0;              // pool chunk (its vertex streams and index buffer)
        S32 vertex_offset = 0;      // VkDrawIndexedIndirectCommand::vertexOffset
        U32 first_index = 0;        // in the chunk's index buffer (unresolved: in the buffer), in its index size
        U32 index_count = 0;
        bool index32 = false;       // 32-bit indices (else 16); the pool holds every index as 32-bit
        // research/198 5.5: the vertex attributes the pass's program reads
        // (a bit per attribute location, LLVertexBuffer::AttributeType). The
        // backend draws the record only once its buffer's place in the pool
        // holds all of them (the pool copies what draws read). 0: none asked.
        U32 attributes = 0;
    };
    // The chunk of a record whose buffer has no place in the pool now (the
    // backend's: it resolves the number when the record is written and when
    // the place moves or goes). Such records are chosen but never drawn.
    constexpr U32 SCENE_UNPLACED = 0xffffffu;

    // A batch's material as its draw reads it (96 bytes; research/197 2.2
    // MaterialRecord). Equal descriptions share one material row. Each
    // texture is its number and its sampler (Textures::samplerIndex, as a
    // draw handle carries them: LLImageGL::rhiTex, rhiSampler). The words'
    // meaning is the viewer's (llscenerecords.h SceneMaterialWord) and the
    // shader permutation's that reads it.
    struct SceneMaterialDesc
    {
        static constexpr U32 TEXTURES = 4;
        static constexpr U32 VALUES = 18;
        TexRef textures[TEXTURES];
        U16 samplers[TEXTURES] = {};
        F32 values[VALUES] = {};
    };
    static_assert(sizeof(SceneMaterialDesc) == 96, "a material row is 96 bytes");

    // research/205 H5 (vk-s3a6): a batch's texture list (LLDrawInfo::mTextureList
    // of several: its draw binds entry u to texture unit u, and its vertices
    // name the entry they sample). Equal lists share one row of the backend's
    // list table (64 bytes: the numbers, 0 past count); a record without one
    // (count 0) has none. A draw context's unit_list says which units take
    // the record's list entries.
    struct SceneTextureList
    {
        static constexpr U32 MAX = 16;
        TexRef textures[MAX];
        U32 count = 0;              // 0: no list
    };

    // research/219: SceneRecordDesc::palette of a record that is not rigged.
    constexpr U32 SCENE_NO_PALETTE = 0xffffffffu;
    // The palette bases a record can hold (24 bits; the backend refuses a
    // record past it).
    constexpr U32 SCENE_PALETTE_LIMIT = 1u << 24;

    struct SceneRecordDesc
    {
        U32 group = 0;              // addGroup's id
        U32 pass = 0;               // render type, < SCENE_MAX_PASSES
        U32 vertex_format = 0;      // the vertex buffer's type mask (LLVertexBuffer::getTypeMask)
        SceneRange range;
        U32 transform = 0;          // addTransform's slot, 0: identity
        U32 uv_transform = 0;       // addUVTransform's slot, 0: none
        SceneMaterialDesc material;
        SceneTextureList list;      // research/205 H5
        F32 lo[3] = {};             // bounds of the batch's vertices in its transform's space
        F32 hi[3] = {};
        bool loose = false;         // bounds not known: drawn whenever its group is visible
        // research/219: a rigged record's palette, the first vec4 of its
        // avatar x skin's resident palette row in the palette table
        // (LLVOAvatar::rhiPaletteSlot, LLRHIAvatar::Backend::allocateRow):
        // its draws read their joints from there (the pass's program's
        // palette word, the shadows' skinned shaders). Such a record is
        // loose (its group's CPU cull bounds it) and has no texture list;
        // its transform is the identity (a rigged batch ignores its model
        // matrix). SCENE_NO_PALETTE: not rigged.
        U32 palette = SCENE_NO_PALETTE;
    };

    // A view: its planes in the space the transforms lead to (agent space,
    // LLCamera's: a point is outside a plane where dot(n, p) + d > 0).
    struct SceneViewDesc
    {
        F32 planes[SCENE_MAX_PLANES][4] = {};
        U32 plane_count = 0;
        F32 origin[3] = {};         // the camera (scales the float slack of the tests)
    };

    // A pass the scene draws (research/198 5.5), as the viewer hands it to
    // drawPass at the pass's entry, with the pass's program bound.
    //  draw: postSort left the pass's batches of the view's drawable groups
    //    out of the render map (cut); false: only choose (and verify).
    //  scope: the viewer draws the pass from the scene when it draws (the
    //    backend verifies its rows and commands for such passes also while
    //    it only chooses).
    //  view_matrix, projection: what the usual path's batches start from
    //    (gGLModelView, the projection; column-major).
    //  fill: the viewer's arithmetic for a draw's matrices (what
    //    LLRenderPass::pushDirectBatch and LLRender::fillDirectMatrices make
    //    of view x model, so the scene's rows are the usual path's bit for
    //    bit). model null: none (the view as it is). normal: 9 floats.
    //  attributes: the vertex layout the pass's program reads
    //    (LLVertexBuffer::getAttributeLayout; offsets 0, the pool's streams
    //    start at the place).
    //  unit_texture: per texture unit the material texture (SceneMaterialDesc
    //    index) a record's draw binds there, -1: the unit keeps the pass's.
    //  unit_list (research/205 H5): per texture unit the entry of the
    //    record's texture list a record with a list binds there (it wins over
    //    unit_texture; a record without a list keeps unit_texture's, or the
    //    pass's), -1: none. A unit the program does not sample is left out.
    //  textures: the texture numbers the view's drawn records sample (the
    //    cut groups' textures, sorted, each once): resolved to bindless slots
    //    for this pass as the usual way's draws of them would (only these:
    //    a texture nothing draws this frame keeps no slot busy).
    //  materials (research/198 5.6, stage 2d): the material uniforms the
    //    pool sets per batch (the specular color, the alpha cutoff, ...),
    //    each from the record's material values; the backend gives each
    //    record a material row of the pass's program with those in place
    //    (the program's own layout, research/46 G4a) and the rest as the
    //    program holds it now. None: every record reads the pass's row.
    //  group_priority (research/205 3a-3, the alpha view): per group slot
    //    (group id - 1) its OIT priority this frame, for the visible groups
    //    (the others are not read). In an OIT session whose variant of the
    //    pass's program reads its priority from the draw's row, each
    //    record's row gets its group's priority with its place in the
    //    group's draw map (material value place_value, at most 4094) in the
    //    low 12 bits, as the pool gives a batch (oit_group_priority).
    //    glow_priority (research/205, vk-s3a6: the glow pass): the low 12
    //    bits are 0xfff instead, as the pool's glow draws of a group come
    //    after all of its batches (setOITPriority).
    //  once: the pass's draws are not to be drawn twice (alpha blending): a
    //    bucket that cannot be bound after others were drawn is left out
    //    (counted as a problem) rather than the pass reported as not drawn.
    //  transform_slots, uv_slots (research/209 S5): the transform and
    //    texture transform rows the view's visible groups' records read
    //    (each once; 0, none, is always made): the view's matrix rows are
    //    made for these only (the others are left as they are: no record of
    //    the view reads them). Null: every row.
    //  verify_view (research/209 S7): a probe face's or a mirror's turn to
    //    be compared (the viewer's own check of its render map, the
    //    backend's of its choice and of a pass's expand). The main, shadow
    //    and alpha views keep their turns by frame.
    //  group_base, group_base_count, sorted_commands (research/205 3a-5,
    //    the alpha view's sorted way: a hybrid OIT session's world half, or
    //    alpha without OIT, where the order of the draws shows): per group
    //    slot the first of the group's commands in the pass's array of
    //    sorted_commands commands (the visible groups'; the others are not
    //    read). With it, drawPass expands the pass's records with each
    //    record's command at its group's base plus its place (material
    //    value place_value), every other command of the array drawing
    //    nothing, and draws nothing itself: drawSorted draws spans of the
    //    array later in the frame, in the order the viewer draws its groups.
    typedef void (*SceneMatrixFill)(const F32* view_matrix, const F32* projection, const F32* model, F32* modelview,
                                    F32* modelview_projection, F32* normal);
    // A material uniform a record's draw sets: the uniform's name in the
    // shaders, and its components from SceneMaterialDesc::values[value] on.
    struct SceneMaterialUniform
    {
        const char* name = nullptr;
        U8 value = 0;
        U8 components = 1;
    };
    struct SceneDrawContext
    {
        static constexpr U32 UNITS = 16;    // research/205 H5: a texture list's units (SceneTextureList::MAX)
        bool draw = false;
        bool scope = false;
        const F32* view_matrix = nullptr;
        const F32* projection = nullptr;
        SceneMatrixFill fill = nullptr;
        const DirectDraw::Attribute* attributes = nullptr;
        U32 attribute_count = 0;
        S8 unit_texture[UNITS] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
        S8 unit_list[UNITS] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
        const U32* textures = nullptr;
        U32 texture_count = 0;
        const SceneMaterialUniform* materials = nullptr;
        U32 material_count = 0;
        const U32* group_priority = nullptr;
        U32 group_priority_count = 0;
        U8 place_value = 0;
        bool glow_priority = false;         // research/205 (vk-s3a6)
        bool once = false;
        const U32* transform_slots = nullptr;
        U32 transform_slot_count = 0;
        const U32* uv_slots = nullptr;
        U32 uv_slot_count = 0;
        bool verify_view = false;
        const U32* group_base = nullptr;    // research/205 3a-5
        U32 group_base_count = 0;
        U32 sorted_commands = 0;
    };

    // research/206 (stage 5): a shadow pass's casters of the scene, as the
    // viewer hands them to drawShadow right before the native shadow pass
    // (LLRHI::sDrawShadowPackets): a sun cascade's moving casters (5a), its
    // cached depth's static casters when it is drawn again (5b), a spot
    // light's casters (5b).
    //  passes: the render types to draw (the opaque shadow types the viewer
    //    cut), each drawn depth only from the view's choice.
    //  view_proj: the pass's projection x view (column-major), what the
    //    pass's packets are drawn with.
    //  draw: false: only choose (verify).
    //  modes (5c, research/206 section 8): per pass how its records are
    //    drawn, as the shadow's packets of that type (ShadowPacket::Mode):
    //    0 depth only; the alpha masks' modes test the diffuse texture's
    //    alpha (through the record's texture transform, a texture
    //    animation's description at LLRHI::sTexAnimTime) against the
    //    record's cutoff (SceneMaterialDesc value 5), as the shadow's alpha
    //    mask shaders. Null: every pass depth only. Only while shadowMasks().
    //    research/219: a mode with SCENE_SHADOW_SKINNED set draws the
    //    pass's records (rigged ones: SceneRecordDesc::palette) skinned with
    //    their palette rows, depth only or (the low bits) as that mask.
    //    Only while shadowSkins() (and, for a mask, shadowMasks()).
    //  textures, texture_names, texture_count (5c): the texture numbers
    //    the masks' records sample and each one's GL name as its image has
    //    it now: resolved to bindless slots for the pass (as its packets'
    //    textures are), the images readied for sampling in it.
    // research/219: SceneShadowContext::modes bit of a rigged pass (the
    // low 7 bits are the ShadowPacket::Mode).
    constexpr U8 SCENE_SHADOW_SKINNED = 0x80;
    constexpr U8 SCENE_SHADOW_MODE_MASK = 0x7f;

    struct SceneShadowContext
    {
        const U32* passes = nullptr;
        U32 pass_count = 0;
        const F32* view_proj = nullptr;
        bool draw = false;
        const U8* modes = nullptr;
        const U32* textures = nullptr;
        const U32* texture_names = nullptr;
        U32 texture_count = 0;
    };

    class Scene
    {
    public:
        virtual ~Scene() = default;

        // The device can hold and draw the scene (made on first use).
        virtual bool ready() = 0;

        virtual U32 addGroup() = 0;                     // 0: refused
        virtual void removeGroup(U32 group) = 0;        // and its records

        virtual U32 addRecord(const SceneRecordDesc& desc) = 0;     // 0: refused
        virtual bool updateRecord(U32 record, const SceneRecordDesc& desc) = 0;
        virtual void removeRecord(U32 record) = 0;

        virtual U32 addTransform(const F32 matrix[16]) = 0;         // column-major; 0: refused
        virtual void setTransform(U32 slot, const F32 matrix[16]) = 0;
        virtual void removeTransform(U32 slot) = 0;
        // Texture transforms: the texture matrix, or research/194's 16-word
        // description of a texture animation (its m[3][3] marks which).
        virtual U32 addUVTransform(const F32 words[16]) = 0;
        virtual void setUVTransform(U32 slot, const F32 words[16]) = 0;
        virtual void removeUVTransform(U32 slot) = 0;

        // The view as it is drawn next, and its visible groups (bit g - 1 for
        // group id g). Setting either again starts a new choice for the view.
        virtual void setView(U32 view, const SceneViewDesc& desc) = 0;
        virtual void setGroupVisible(U32 view, const U32* bits, U32 words) = 0;

        // The pass's records of the view: chosen, and (ctx.draw) drawn into
        // the rendering scope the pass's program draws into. True: drawn (or,
        // ctx.draw false, chosen). False with ctx.draw: nothing of it was
        // drawn; the caller draws the cut batches itself, and the backend
        // stops taking the pass (canDrawPass) if the program cannot be drawn
        // from rows (in every view), or for a while in the view.
        virtual bool drawPass(U32 view, U32 pass, const SceneDrawContext& ctx) = 0;

        // research/205 3a-5: a span of a sorted pass that drawPass expanded
        // this frame (ctx.group_base): the commands [first, first + count)
        // of the pass's array, whose records (their ids, in the span's
        // order) are drawn in that order with the pass's program bound and
        // the state the span's batches are drawn with set (the words the
        // rows were made from must still be the program's), one indirect
        // draw per run of records in one pool chunk. False: nothing of it
        // was drawn (a record not placed now, the words changed, a chunk
        // that cannot be bound): the caller draws the span's batches.
        virtual bool drawSorted(U32 view, U32 pass, U32 first, const U32* records, U32 count) = 0;

        // research/206: a shadow view's (sceneShadowView) choice (setView,
        // setGroupVisible this frame), and (ctx.draw) its records of
        // ctx.passes made ready for the next native shadow pass, which draws
        // them first, depth only (the backend's shadow renderer takes them at
        // its entry, drawn or not). A view may be chosen again in a frame (a
        // cascade's cached depth, then its moving casters): each choice has
        // its own commands. False: nothing of it will be drawn (the caller
        // draws what it cut the usual way); also when a visible group has a
        // record without its place now (5b: the cached depth would keep the
        // hole).
        virtual bool drawShadow(U32 view, const SceneShadowContext& ctx) = 0;
        // research/206 5c: whether drawShadow can draw passes with a mask
        // mode (SceneShadowContext::modes): the shadow renderer made its
        // mask shaders and the texture slot table is readable by address.
        // False until the first native shadow pass made the renderer.
        virtual bool shadowMasks() const = 0;
        // research/219: whether drawShadow can draw passes with
        // SCENE_SHADOW_SKINNED (rigged records): the shadow renderer made
        // its skinned shaders, the scene can draw, no translation thread.
        // False until the first native shadow pass made the renderer.
        virtual bool shadowSkins() const = 0;

        // verify and auto start with drawing off (drawPass only chooses);
        // auto turns it on once the viewer's checks agree long enough, and
        // off again at a difference. on starts with it on.
        virtual void setDrawing(bool on) = 0;
        virtual bool drawing() const = 0;
        // Whether drawPass may draw the pass in the view (false once its
        // program was found not to draw from rows, research/46 G4b: in every
        // view; research/209 S6: false for a while after the view could not
        // draw it, a full share or table: in that view only).
        virtual bool canDrawPass(U32 view, U32 pass) const = 0;
        // research/204 10 (vk-s4c2): the pass is another pool's now, or its
        // pool binds another program: what was found about it before (its
        // refusals in every view) no longer holds.
        virtual void forgetPass(U32 pass) = 0;
        // Every record of the group has its buffer placed with the attributes
        // its pass reads: postSort may cut the group's batches of a drawn pass.
        virtual bool groupDrawable(U32 group) const = 0;
        // research/209 S7: whether this frame is a turn of the view's kind (a
        // probe face, a mirror, the water mirror; one kind a turn) to be
        // compared: the viewer then draws one face of it the usual way
        // without the probes' own shortcuts and compares its render map, and
        // hands the turn to drawPass (SceneDrawContext::verify_view). False
        // once a face of the kind took it this frame.
        virtual bool viewTurn(U32 view) const = 0;
        // Differences the backend's own checks found since the start (the
        // GPU's choice against the CPU's, the rows and commands drawPass
        // made against the CPU's, the records' places against the pool's):
        // auto turns drawing off when it grows.
        virtual U32 problems() const = 0;

        // Verify: what the GPU's choice for (view, pass) is, as the CPU works
        // it out from the same tables (records in draw order), and the
        // records of visible groups left out only by their bounds.
        virtual void collect(U32 view, U32 pass, std::vector<U32>& drawn, std::vector<U32>& bounded_out) = 0;

        // Counters since the last call (empty when unused).
        virtual std::string takeStats(U32 frames) = 0;
    };

    // Set by the backend that has a scene (null: none).
    extern Scene* sScene;
}

#endif // LL_LLRHISCENE_H
