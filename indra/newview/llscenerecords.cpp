/**
 * @file llscenerecords.cpp
 * @brief The viewer's side of the retained scene (research/197 stage 2): LLDrawInfo to records, resident groups, views.
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

#include "llscenerecords.h"
#include "llscenealpha.h"     // research/205

#include "llcamera.h"
#include "lldrawpool.h"
#include "lldrawpoolalpha.h"
#include "llframetimer.h"
#include "llrhiframe.h"     // research/208 S0: the render thread census
#include "llrhihash.h"
#include "llglslshader.h"
#include "llgraywire.h"
#include "llimagegl.h"
#include "llmaterial.h"     // research/205 3a-4: SHADER_COUNT
#include "llrender.h"
#include "llshadermgr.h"
#include "llspatialpartition.h"
#include "lltimer.h"
#include "llvertexbuffer.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewershadermgr.h"
#include "llviewertexture.h"
#include "plus/alphapass/alphapass.h"   // research/205 (vk-s3a6): LLDrawInfo::mRHIGlow
#include "pipeline.h"
#include "plus/hairsway/hairsway.h"   // research/219: plusHairSway for a rigged pass's records
#include "plus/skinsss/skinsss.h"     // research/218 vk-s4b2: the worn objects' groups while skin is marked
#include "llmeshrepository.h"         // research/219: LLMeshSkinInfo
#include "llrhiavatar.h"              // research/219: LLRHIAvatar::SLOT_NONE
#include "llvoavatar.h"               // research/219: the rigged records' palette rows
#include "lldrawable.h"               // research/204 10 (vk-s4c2): the world pools' faces
#include "llface.h"

#include "glm/matrix.hpp"
#include "glm/gtc/type_ptr.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn

LLSceneRecords gSceneRecords;
U32 LLSceneRecords::sViewOverride = U32_MAX;

namespace
{
    // Stage 1a (research/201): a texture's number and its sampler, as a draw
    // handle carries them. No texture: 0 (white). A texture without a number
    // yet (no image made) cannot be described (REFUSED_NO_TEXTURE).
    bool texture_ref(const LLViewerTexture* texture, LLRHI::TexRef& out, U16& sampler)
    {
        out = LLRHI::TexRef();
        sampler = 0;
        if (!texture)
        {
            return true;
        }
        const LLImageGL* image = texture->getGLTexture();
        if (!image)
        {
            return false;
        }
        out = image->rhiTex();
        sampler = image->rhiSampler();
        return out.id != 0;
    }

    // Stage 1b (research/202): the vertex buffer by its number and the
    // batch's indices in it. The number stays the buffer's for its whole life
    // (allocated again, written, its place in the pool moved), so the record
    // never needs telling when the place moves: the backend keeps "number ->
    // place" (research/198 5). The pool holds every index as 32-bit. No
    // number (LLRHI_VB_HANDLES off, or the switch not on yet): no record.
    bool buffer_range(const LLDrawInfo& info, LLRHI::SceneRange& out)
    {
        out = LLRHI::SceneRange();
        const LLVertexBuffer* vb = info.mVertexBuffer.get();
        out.buffer = vb ? vb->rhiDrawNumber() : 0;
        if (!out.buffer)
        {
            return false;
        }
        out.first_index = info.mOffset;
        out.index_count = info.mCount;
        out.index32 = true;
        return true;
    }

    const char* const REFUSAL_NAMES[LLSceneRecords::REFUSED_COUNT] = {
        "rigged", "hud", "partition", "no buffer", "no buffer number", "no texture number", "texture list", "map matrices", "gltf",
        "no matrix row", "table", "program not loaded", "map missing", "alpha kind", "no palette row",
    };

    // ==== scene rigged passes: begin ====
    // research/219: the scope passes' rigged maps (registerFace: the rigged
    // variant of a render type is the type + 1) and the scope pass each is
    // drawn as. U32_MAX: none (the rigged GLTF, alpha, bump and shiny maps
    // stay on the usual way).
    U32 rigged_base(U32 pass)
    {
        switch (pass)
        {
            case LLRenderPass::PASS_SIMPLE_RIGGED:
            case LLRenderPass::PASS_FULLBRIGHT_RIGGED:
            case LLRenderPass::PASS_MATERIAL_RIGGED:
            case LLRenderPass::PASS_MATERIAL_ALPHA_MASK_RIGGED:
            case LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE_RIGGED:
            case LLRenderPass::PASS_SPECMAP_RIGGED:
            case LLRenderPass::PASS_SPECMAP_MASK_RIGGED:
            case LLRenderPass::PASS_SPECMAP_EMISSIVE_RIGGED:
            case LLRenderPass::PASS_NORMMAP_RIGGED:
            case LLRenderPass::PASS_NORMMAP_MASK_RIGGED:
            case LLRenderPass::PASS_NORMMAP_EMISSIVE_RIGGED:
            case LLRenderPass::PASS_NORMSPEC_RIGGED:
            case LLRenderPass::PASS_NORMSPEC_MASK_RIGGED:
            case LLRenderPass::PASS_NORMSPEC_EMISSIVE_RIGGED:
            case LLRenderPass::PASS_GLOW_RIGGED:
            case LLRenderPass::PASS_ALPHA_MASK_RIGGED:
            case LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK_RIGGED:
                return pass - 1;
            default:
                return U32_MAX;
        }
    }

    // research/219 (8.2): whether a record of a group the main view would cut
    // while its G-buffer pass marks skin is drawn with the mark the usual way
    // would give it: a pass the view does not cut leaves it to the usual way
    // (fits); else its mark must be known (known: plus::skinsss::sceneMarkOf)
    // and be its pass's: the plan's avatar mark for a rigged record (the
    // rigged passes' SceneMarkScope), none for the others.
    bool record_mark_fits(bool pass_cut, bool known, U32 mark, bool rigged, U32 avatar_mark)
    {
        if (!pass_cut)
        {
            return true;
        }
        return known && mark == (rigged ? avatar_mark : 0u);
    }
    // ==== scene rigged passes: end ====
    static_assert(LLRenderPass::PASS_SIMPLE_RIGGED == LLRenderPass::PASS_SIMPLE + 1 &&
                  LLRenderPass::PASS_GLOW_RIGGED == LLRenderPass::PASS_GLOW + 1 &&
                  LLRenderPass::PASS_ALPHA_MASK_RIGGED == LLRenderPass::PASS_ALPHA_MASK + 1 &&
                  LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK_RIGGED == LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK + 1 &&
                  LLRenderPass::PASS_NORMSPEC_EMISSIVE_RIGGED == LLRenderPass::PASS_NORMSPEC_EMISSIVE + 1,
                  "a rigged map is its render type + 1 (registerFace)");

    // The pass a scope pass's rules are read from: a rigged map's base, else itself.
    U32 scope_base(U32 pass)
    {
        const U32 base = rigged_base(pass);
        return base != U32_MAX ? base : pass;
    }

    // research/198 5.5 (LLRHI::SceneDrawContext::fill): a draw's matrices as
    // LLRenderPass::pushDirectBatch and LLRender::fillDirectMatrices make
    // them (the same glm arithmetic, so the scene's rows are the usual
    // path's bit for bit): the view times the model matrix (none: the view
    // as it is), the projection times that, and the normal matrix.
    void fill_matrices(const F32* view, const F32* projection, const F32* model, F32* modelview, F32* modelview_projection, F32* normal)
    {
        glm::mat4 mv = glm::make_mat4(view);
        if (model)
        {
            mv *= glm::make_mat4(model);
        }
        const glm::mat4 mvp = glm::make_mat4(projection) * mv;
        const glm::mat4 normal_matrix = glm::transpose(glm::inverse(mv));
        memcpy(modelview, glm::value_ptr(mv), 16 * sizeof(F32));
        memcpy(modelview_projection, glm::value_ptr(mvp), 16 * sizeof(F32));
        const F32* n = glm::value_ptr(normal_matrix);
        const F32 columns[9] = { n[0], n[1], n[2], n[4], n[5], n[6], n[8], n[9], n[10] };
        memcpy(normal, columns, sizeof(columns));
    }

    // LLImageGL's sampler changes (LLRHI::Textures::samplerChanged) may come
    // from the image thread: they are only queued here, and the main thread
    // takes them at the next frame's head.
    std::mutex sSamplerMutex;
    std::vector<U32> sSamplerChanged;

    void on_sampler_changed(LLRHI::TexRef ref)
    {
        std::lock_guard<std::mutex> lock(sSamplerMutex);
        sSamplerChanged.push_back(ref.id);
    }

    void hash_bytes(U64& h, const void* data, size_t size)
    {
        h = LLRHI::fnv1a(h, data, size);
    }

    bool same_words(const F32* a, const F32* b)
    {
        return memcmp(a, b, 16 * sizeof(F32)) == 0;
    }

    // research/205 3a-3: the alpha records' passes, past the render types
    // (3a-4: then one per legacy material shader mask).
    constexpr U32 ALPHA_PASS_PLAIN = LLRenderPass::NUM_RENDER_TYPES;
    constexpr U32 ALPHA_PASS_FULLBRIGHT = LLRenderPass::NUM_RENDER_TYPES + 1;
    constexpr U32 ALPHA_PASS_MATERIAL = LLRenderPass::NUM_RENDER_TYPES + 2;
    // research/205 (vk-s3a6): the glow pass, past the material masks' (gDeferredEmissiveProgram).
    constexpr U32 ALPHA_PASS_GLOW = ALPHA_PASS_MATERIAL + LLMaterial::SHADER_COUNT;
    static_assert(LLSceneRecords::ALPHA_PASSES == 3 + LLMaterial::SHADER_COUNT, "an alpha pass per material shader mask, and the glow");
    static_assert(ALPHA_PASS_GLOW == ALPHA_PASS_PLAIN + LLSceneRecords::ALPHA_GLOW_INDEX, "the glow pass is the last alpha pass");
    static_assert(ALPHA_PASS_PLAIN + LLSceneRecords::ALPHA_PASSES <= LLRHI::SCENE_MAX_PASSES, "the alpha records' passes are scene passes");
    static_assert(LLSceneRecords::ALPHA_PASSES <= 32, "the alpha passes are bits of a word");
    static_assert(SCENE_MAT_ALPHA_PLACE < LLRHI::SceneMaterialDesc::VALUES, "the place is a material value");
    // research/205 3a-5: a draw map longer than the places an OIT priority holds has no spans.
    constexpr U32 ALPHA_SPAN_PLACES = 4095;

    // research/204 10 (vk-s4c2): the world pools' slots' passes, past the alpha passes.
    constexpr U32 WORLD_PASS_FIRST = ALPHA_PASS_PLAIN + LLSceneRecords::ALPHA_PASSES;
    static_assert(WORLD_PASS_FIRST + LLSceneRecords::WORLD_SLOTS <= LLRHI::SCENE_MAX_PASSES, "the world pools' passes are scene passes");
    static_assert(LLSceneRecords::WORLD_SLOTS <= 255, "a world face keeps its slot in a byte");

    // ==== scene world rules: begin ====
    // research/204 10 (vk-s4c2): the pure rules of the world pools' records
    // (selftest sim-sceneworld takes them from here).
    //
    // A face as it is now, and as its record was made: its pool's slot (and
    // the slot's serial: its pool and the attributes its program reads), its
    // buffer's number, its indices and its model matrix.
    struct WorldFaceKey
    {
        U32 slot = 0;
        U32 slot_serial = 0;
        U32 buffer = 0;
        U32 first = 0;
        U32 count = 0;
        const void* model = nullptr;
    };
    // The record still draws what the face's draw would (a face drawing
    // nothing, count 0, holds against a record of nothing whatever else).
    bool world_face_holds(const WorldFaceKey& now, const WorldFaceKey& record)
    {
        if (now.slot != record.slot || now.slot_serial != record.slot_serial || now.count != record.count)
        {
            return false;
        }
        if (!now.count)
        {
            return true;
        }
        return now.buffer == record.buffer && now.first == record.first && now.model == record.model;
    }
    // A group after its faces in the pools' lists were walked this view: shown
    // to the scene (its records drawn, its faces left out of the loops) only
    // when no face broke, each of its records was seen once (seen of faces),
    // it has a record to draw, the backend can draw every record now
    // (drawable: placed with what its pass reads), its model rows are still
    // its matrices' (rows_hold) and its id is within the view's bits.
    bool world_group_shows(bool broken, U32 seen, U32 faces, U32 records, bool drawable, bool rows_hold, bool in_bits)
    {
        return !broken && faces && seen == faces && records && drawable && rows_hold && in_bits;
    }
    // A group not shown is described again from this view's faces when it
    // broke or never was (needs), its wait is over and the frame's budget
    // has room for its faces. A group that only waits for its places (not
    // drawable yet) keeps its records.
    bool world_group_describes(bool needs, U32 frame, U32 retry_frame, U32 budget_used, U32 budget, U32 walked)
    {
        return needs && walked && (S32)(frame - retry_frame) >= 0 && budget_used + walked <= budget;
    }
    // The wait before the next description of a group that needs one again:
    // none after a description that held a while, else doubled up to 32 frames.
    U16 world_next_gap(U16 gap, U32 frame, U32 described_frame, bool described_before)
    {
        if (!described_before || (frame - described_frame) >= 8)
        {
            return 0;
        }
        return (U16)llmin(gap ? gap * 2u : 1u, 32u);
    }
    // What a world pool's pass entry does: the scene draws it (cut: on, the
    // scene drawing, some faces of the pool's list shown) or only chooses it
    // and verifies (any mode but on, or on while the scene does not draw).
    // Neither: nothing is asked of the scene.
    enum WorldEntry
    {
        WORLD_ENTRY_NONE,
        WORLD_ENTRY_CHOOSE,
        WORLD_ENTRY_DRAW,
    };
    WorldEntry world_entry(LLRHI::SceneMode mode, bool drawing, U32 shown_faces)
    {
        if (mode == LLRHI::SceneMode::Off)
        {
            return WORLD_ENTRY_NONE;
        }
        if (mode == LLRHI::SceneMode::On && drawing)
        {
            return shown_faces ? WORLD_ENTRY_DRAW : WORLD_ENTRY_NONE;
        }
        return WORLD_ENTRY_CHOOSE;
    }
    // The loop leaves a face out only when the scene drew the pass and the
    // face's group was shown this view (serial: the view's, never 0).
    bool world_face_left_out(bool drew, U32 shown, U32 serial)
    {
        return drew && serial && shown == serial;
    }
    // ==== scene world rules: end ====

    // research/204 10: the partitions whose groups' faces the world pools draw.
    bool world_partition(U32 partition)
    {
        return partition == LLViewerRegion::PARTITION_TERRAIN || partition == LLViewerRegion::PARTITION_TREE ||
               partition == LLViewerRegion::PARTITION_WATER || partition == LLViewerRegion::PARTITION_VOIDWATER;
    }

    bool world_pool_type(U32 type)
    {
        return type == LLDrawPool::POOL_TERRAIN || type == LLDrawPool::POOL_TREE || type == LLDrawPool::POOL_WATER;
    }

    // The locations of the attributes a program reads, as LLVertexBuffer lays them out.
    U32 attribute_locations(U32 mask)
    {
        LLRHI::DirectDraw::Attribute attributes[LLRHI::DirectDraw::MAX_ATTRIBUTES];
        const U32 count = LLVertexBuffer::getAttributeLayout(mask, attributes);
        U32 bits = 0;
        for (U32 i = 0; i < count; ++i)
        {
            bits |= attributes[i].location < 32 ? 1u << attributes[i].location : 0u;
        }
        return bits;
    }

    // research/205 3a-4: a legacy material's alpha pass (not the glow's after them).
    bool alpha_material_pass(U32 pass)
    {
        return pass >= ALPHA_PASS_MATERIAL && pass < ALPHA_PASS_GLOW;
    }

    // research/205 3a-4: what the alpha pool sets per batch with a legacy
    // material (renderAlpha: SPECULAR_COLOR mSpecColor, ENVIRONMENT_INTENSITY
    // mEnvIntensity, EMISSIVE_BRIGHTNESS mFullbright 1 or 0; the minimum alpha
    // is the pool's, not the batch's).
    const LLRHI::SceneMaterialUniform ALPHA_MATERIAL_UNIFORMS[] = {
        { "specular_color", (U8)SCENE_MAT_SPEC_R, 4 },
        { "env_intensity", (U8)SCENE_MAT_ENV_INTENSITY, 1 },
        { "emissive_brightness", (U8)SCENE_MAT_FULLBRIGHT, 1 },
    };

    // ==== scene alpha span rules: begin ====
    // research/205 3a-5: a group's spans: the runs of places next to each
    // other in its draw map whose batches have records (records[k] not 0)
    // of one alpha pass (passes[k]), in draw map order, each as long as it
    // goes. A place without a record ends the run (the loop draws it there).
    template <class Span>
    void alpha_spans(const U32* records, const U8* passes, U32 places, std::vector<Span>& out)
    {
        out.clear();
        for (U32 k = 0; k < places; ++k)
        {
            if (!records[k])
            {
                continue;
            }
            if (!out.empty() && (U32)out.back().first + out.back().count == k && out.back().pass_index == passes[k])
            {
                ++out.back().count;
                continue;
            }
            Span span;
            span.first = (decltype(span.first))k;
            span.count = 1;
            span.pass_index = passes[k];
            out.push_back(span);
        }
    }
    // ==== scene alpha span rules: end ====

    // ==== scene worn rules: begin ====
    // research/218 vk-s4b2 (research/206 8.4 (a)): an attachment's linkset
    // (PARTITION_AVATAR, the avatar's spatial bridge) and an animesh's
    // (PARTITION_CONTROL_AV) are the worn objects' partitions.
    bool worn_partition(U32 partition)
    {
        return partition == LLViewerRegion::PARTITION_AVATAR || partition == LLViewerRegion::PARTITION_CONTROL_AV;
    }

    // Whether a batch of the partition may have a record: a volume's and a
    // spatial bridge's, and with LLRHI_SCENE_ATTACHMENTS not off (records) a
    // worn object's opaque one (its alpha is the alpha pool's, by its avatar
    // rules: none). Not the HUD, terrain, trees, grass, water, particles.
    bool partition_has_records(U32 partition, bool alpha, bool worn_records)
    {
        if (worn_partition(partition))
        {
            return worn_records && !alpha;
        }
        return partition == LLViewerRegion::PARTITION_VOLUME || partition == LLViewerRegion::PARTITION_BRIDGE;
    }

    // Whether a resident group the scene can draw in a view now is cut there.
    // A worn object's group only with LLRHI_SCENE_ATTACHMENTS on (worn_cut;
    // verify: chosen and compared, drawn the usual way). A group with batches
    // worn by avatars (wearers) neither in the main view while its G-buffer
    // pass marks skin (an object worn on an eye point is marked per batch) nor
    // while GrayWire draws one of them as a wireframe (the scene draws none).
    template <class Wearer, class IsGray>
    bool worn_group_cut(bool worn, bool worn_cut, const std::vector<Wearer>& wearers, bool main_view, bool skin_marking,
                        bool gray_wire, IsGray is_gray)
    {
        if (worn && !worn_cut)
        {
            return false;
        }
        if (wearers.empty())
        {
            return true;
        }
        if (main_view && skin_marking)
        {
            return false;
        }
        if (gray_wire)
        {
            for (const Wearer& wearer : wearers)
            {
                if (is_gray(wearer))
                {
                    return false;
                }
            }
        }
        return true;
    }

    // Whether a shadow pass may take a group of the partition: a volume's and
    // a bridge's, and a worn object's while it has records and either is cut
    // (worn_cut) or the pass only chooses (verify: compared, never cut).
    bool shadow_takes_partition(U32 partition, bool worn_records, bool worn_cut, bool cutting)
    {
        if (worn_partition(partition))
        {
            return worn_records && (worn_cut || !cutting);
        }
        return partition == LLViewerRegion::PARTITION_VOLUME || partition == LLViewerRegion::PARTITION_BRIDGE;
    }
    // ==== scene worn rules: end ====

    // An alpha batch's place in its group's PASS_ALPHA draw map (the low
    // bits of its OIT priority), -1: not there. The group's places are
    // indexed once per frame and draw map (registration and redescription
    // come in runs of one group's batches; main thread only).
    S32 alpha_place(const LLDrawInfo& info, const LLSpatialGroup* group)
    {
        static const LLSpatialGroup* s_group = nullptr;
        static U32 s_frame = 0;
        static const void* s_data = nullptr;
        static size_t s_size = 0;
        static std::unordered_map<const LLDrawInfo*, S32> s_places;
        const auto map = group->mDrawMap.find(LLRenderPass::PASS_ALPHA);
        if (map == group->mDrawMap.end())
        {
            return -1;
        }
        const LLSpatialGroup::drawmap_elem_t& draw_vec = map->second;
        const U32 frame = LLFrameTimer::getFrameCount();
        if (group != s_group || frame != s_frame || draw_vec.data() != s_data || draw_vec.size() != s_size)
        {
            s_group = group;
            s_frame = frame;
            s_data = draw_vec.data();
            s_size = draw_vec.size();
            s_places.clear();
            for (size_t i = 0; i < draw_vec.size(); ++i)
            {
                s_places.emplace(draw_vec[i].get(), (S32)i);
            }
        }
        const auto it = s_places.find(&info);
        return it != s_places.end() ? it->second : -1;
    }
}

U32 LLSceneRecords::alphaPass(bool fullbright)
{
    return fullbright ? ALPHA_PASS_FULLBRIGHT : ALPHA_PASS_PLAIN;
}

U32 LLSceneRecords::alphaMaterialPass(U32 shader_mask)
{
    return ALPHA_PASS_MATERIAL + llmin(shader_mask, (U32)LLMaterial::SHADER_COUNT - 1);
}

U32 LLSceneRecords::alphaGlowPass()
{
    return ALPHA_PASS_GLOW;
}

U32 LLSceneRecords::alphaPassAt(U32 index)
{
    return ALPHA_PASS_PLAIN + llmin(index, ALPHA_PASSES - 1);
}

U32 LLSceneRecords::alphaPassIndex(U32 pass)
{
    return isAlphaPass(pass) ? pass - ALPHA_PASS_PLAIN : ALPHA_PASSES;
}

bool LLSceneRecords::isAlphaPass(U32 pass)
{
    return pass >= ALPHA_PASS_PLAIN && pass < ALPHA_PASS_PLAIN + ALPHA_PASSES;
}

bool LLSceneRecords::alphaWanted()
{
    static const bool wanted = LLRHI::sceneAlphaMode() != LLRHI::SceneMode::Off;
    return wanted;
}

// research/218 vk-s4b2: read once, as LLRHI's scene modes (off with LLRHI_SCENE
// off). Unset: on (the worn objects' groups go as the bridges do).
LLRHI::SceneMode LLSceneRecords::wornMode()
{
    static const LLRHI::SceneMode mode = []()
    {
        if (LLRHI::sceneMode() == LLRHI::SceneMode::Off)
        {
            return LLRHI::SceneMode::Off;
        }
        const char* env = std::getenv("LLRHI_SCENE_ATTACHMENTS");
        if (env && (!strcmp(env, "off") || !strcmp(env, "0")))
        {
            return LLRHI::SceneMode::Off;
        }
        if (env && !strcmp(env, "verify"))
        {
            return LLRHI::SceneMode::Verify;
        }
        return LLRHI::SceneMode::On;
    }();
    return mode;
}

bool LLSceneRecords::shadowTakes(U32 partition, bool cutting)
{
    const LLRHI::SceneMode mode = wornMode();
    return shadow_takes_partition(partition, mode != LLRHI::SceneMode::Off, mode == LLRHI::SceneMode::On, cutting);
}

// PASS_BUMP, PASS_SHINY and PASS_FULLBRIGHT_SHINY wait: their pools bind a
// texture made from the batch's own (LLDrawPoolBump::bindBumpMap) and the
// environment map per batch, which a material row does not name yet
// (research/198 3.4).
bool LLSceneRecords::targetPass(U32 pass)
{
    switch (pass)
    {
        case LLRenderPass::PASS_SIMPLE:
        case LLRenderPass::PASS_FULLBRIGHT:
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
        case LLRenderPass::PASS_GLOW:
        case LLRenderPass::PASS_ALPHA_MASK:
        case LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK:
            return true;
        default:
            // research/219: their rigged maps with LLRHI_SCENE_RIGGED not off (read once)
            return LLRHI::sceneRiggedMode() != LLRHI::SceneMode::Off && rigged_base(pass) != U32_MAX;
    }
}

U32 LLSceneRecords::riggedBase(U32 pass)
{
    return rigged_base(pass);
}

namespace
{
    // LLDrawPoolMaterials::beginDeferredPass's program for a legacy material
    // pass (gDeferredMaterialProgram's index: bit 0x8 a normal map, 0x4 a
    // specular map), -1: not one. research/219: a rigged map's, its base's
    // (beginDeferredPass binds that program's rigged variant).
    S32 material_program_index(U32 pass)
    {
        switch (scope_base(pass))
        {
            case LLRenderPass::PASS_MATERIAL:                return 0;
            case LLRenderPass::PASS_MATERIAL_ALPHA_MASK:     return 2;
            case LLRenderPass::PASS_MATERIAL_ALPHA_EMISSIVE: return 3;
            case LLRenderPass::PASS_SPECMAP:                 return 4;
            case LLRenderPass::PASS_SPECMAP_MASK:            return 6;
            case LLRenderPass::PASS_SPECMAP_EMISSIVE:        return 7;
            case LLRenderPass::PASS_NORMMAP:                 return 8;
            case LLRenderPass::PASS_NORMMAP_MASK:            return 10;
            case LLRenderPass::PASS_NORMMAP_EMISSIVE:        return 11;
            case LLRenderPass::PASS_NORMSPEC:                return 12;
            case LLRenderPass::PASS_NORMSPEC_MASK:           return 14;
            case LLRenderPass::PASS_NORMSPEC_EMISSIVE:       return 15;
            default:                                         return -1;
        }
    }

    // research/198 5.6: what the pools set per batch, from the record's
    // material values. pushMaskBatches: setMinimumAlpha(mAlphaMaskCutoff).
    // LLDrawPoolMaterials::renderDeferred: SPECULAR_COLOR mSpecColor,
    // ENVIRONMENT_INTENSITY mEnvIntensity, MINIMUM_ALPHA mAlphaMaskCutoff,
    // EMISSIVE_BRIGHTNESS mFullbright (1 or 0). The names are the shaders'
    // (LLShaderMgr's reserved uniforms).
    const LLRHI::SceneMaterialUniform MASK_UNIFORMS[] = {
        { "minimum_alpha", (U8)SCENE_MAT_ALPHA_CUTOFF, 1 },
    };
    const LLRHI::SceneMaterialUniform MATERIAL_UNIFORMS[] = {
        { "specular_color", (U8)SCENE_MAT_SPEC_R, 4 },
        { "env_intensity", (U8)SCENE_MAT_ENV_INTENSITY, 1 },
        { "minimum_alpha", (U8)SCENE_MAT_ALPHA_CUTOFF, 1 },
        { "emissive_brightness", (U8)SCENE_MAT_FULLBRIGHT, 1 },
    };

    // The scope passes LLDrawPoolAlpha::renderDebugAlpha draws from their
    // render maps (pushUntexturedBatches) while the alpha highlight is on
    // (research/219: and their rigged maps, pushRiggedBatches untextured).
    bool debug_alpha_reads(U32 pass)
    {
        pass = scope_base(pass);
        return pass == LLRenderPass::PASS_ALPHA_MASK || pass == LLRenderPass::PASS_MATERIAL_ALPHA_MASK ||
               pass == LLRenderPass::PASS_NORMMAP_MASK || pass == LLRenderPass::PASS_SPECMAP_MASK ||
               pass == LLRenderPass::PASS_NORMSPEC_MASK || pass == LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK;
    }
}

// research/198 5.5, 5.6: the passes the scene draws when it draws. Each pool
// binds one program for the pass (passProgram) and draws its batches with
// the batch's diffuse texture (a list of one; none: unbound, the white
// stand-in), its texture matrix when it has one, its model matrix when it
// has one: PASS_SIMPLE (LLDrawPoolSimple::renderDeferred, pushBatches),
// PASS_ALPHA_MASK and PASS_FULLBRIGHT_ALPHA_MASK (pushMaskBatches: and the
// batch's cutoff), PASS_FULLBRIGHT and PASS_GLOW (pushBatches), the legacy
// materials (LLDrawPoolMaterials::renderDeferred: and their normal and
// specular maps on the program's units, their four uniforms).
// research/219: and their rigged maps (riggedBase), drawn by the pools'
// rigged variants of those programs (bind(true), beginDeferredPass).
bool LLSceneRecords::scopePass(U32 pass)
{
    switch (scope_base(pass))
    {
        case LLRenderPass::PASS_SIMPLE:
        case LLRenderPass::PASS_ALPHA_MASK:
        case LLRenderPass::PASS_FULLBRIGHT:
        case LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK:
        case LLRenderPass::PASS_GLOW:
            return true;
        default:
            return material_program_index(pass) >= 0;
    }
}

// The program the main view's pool binds for the scope pass (not the HUDs':
// the scene draws none). research/219: a rigged map's, its base program's
// rigged variant.
LLGLSLShader* LLSceneRecords::passProgram(U32 pass)
{
    const U32 base = rigged_base(pass);
    if (base != U32_MAX)
    {
        LLGLSLShader* program = passProgram(base);
        return program ? program->mRiggedVariant : nullptr;
    }
    switch (pass)
    {
        case LLRenderPass::PASS_SIMPLE:                 return &gDeferredDiffuseProgram;
        case LLRenderPass::PASS_ALPHA_MASK:             return &gDeferredDiffuseAlphaMaskProgram;
        case LLRenderPass::PASS_FULLBRIGHT:             return &gDeferredFullbrightProgram;
        case LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK:  return &gDeferredFullbrightAlphaMaskProgram;
        case LLRenderPass::PASS_GLOW:                   return &gDeferredEmissiveProgram;
        case ALPHA_PASS_PLAIN:                          return &gDeferredAlphaProgram;      // research/205 3a-3
        case ALPHA_PASS_FULLBRIGHT:                     return &gDeferredFullbrightAlphaMaskAlphaProgram;
        case ALPHA_PASS_GLOW:                           return &gDeferredEmissiveProgram;   // research/205 (vk-s3a6): renderEmissives'
        default:
        {
            if (alpha_material_pass(pass))
            {
                return &gDeferredMaterialProgram[pass - ALPHA_PASS_MATERIAL];   // research/205 3a-4: renderAlpha's for a material's mask
            }
            const S32 index = material_program_index(pass);
            return index >= 0 ? &gDeferredMaterialProgram[index] : nullptr;
        }
    }
}

U32 LLSceneRecords::passTextures(U32 pass)
{
    U32 bits = scopePass(pass) || isAlphaPass(pass) ? 1u << SCENE_TEX_DIFFUSE : 0u;
    // research/205 3a-4: an alpha material pass's mask (the same bits as the program index).
    const S32 index = alpha_material_pass(pass) ? (S32)(pass - ALPHA_PASS_MATERIAL) : material_program_index(pass);
    if (index >= 0)
    {
        bits |= (index & 0x8) ? 1u << SCENE_TEX_NORMAL : 0u;
        bits |= (index & 0x4) ? 1u << SCENE_TEX_SPECULAR : 0u;
    }
    return bits;
}

const LLRHI::SceneMaterialUniform* LLSceneRecords::passMaterials(U32 pass, U32& count)
{
    pass = scope_base(pass);    // research/219: a rigged map's pool sets its base's
    if (pass == LLRenderPass::PASS_ALPHA_MASK || pass == LLRenderPass::PASS_FULLBRIGHT_ALPHA_MASK)
    {
        count = (U32)LL_ARRAY_SIZE(MASK_UNIFORMS);
        return MASK_UNIFORMS;
    }
    if (material_program_index(pass) >= 0)
    {
        count = (U32)LL_ARRAY_SIZE(MATERIAL_UNIFORMS);
        return MATERIAL_UNIFORMS;
    }
    if (alpha_material_pass(pass))
    {
        count = (U32)LL_ARRAY_SIZE(ALPHA_MATERIAL_UNIFORMS);    // research/205 3a-4
        return ALPHA_MATERIAL_UNIFORMS;
    }
    count = 0;
    return nullptr;
}

// The locations of the attributes the scope pass's program reads, as
// LLVertexBuffer lays them out for it (a record is drawn from its buffer's
// place only once the place holds all of them). Kept per pass while the
// program's attribute mask stays the same.
U32 LLSceneRecords::passAttributes(U32 pass)
{
    const LLGLSLShader* program = pass < LLRHI::SCENE_MAX_PASSES ? passProgram(pass) : nullptr;
    if (!program)
    {
        return 0;
    }
    const U32 mask = program->mAttributeMask;
    static U32 cached_mask[LLRHI::SCENE_MAX_PASSES] = {};
    static U32 cached[LLRHI::SCENE_MAX_PASSES] = {};
    if (mask != cached_mask[pass])
    {
        LLRHI::DirectDraw::Attribute attributes[LLRHI::DirectDraw::MAX_ATTRIBUTES];
        const U32 count = LLVertexBuffer::getAttributeLayout(mask, attributes);
        cached[pass] = 0;
        for (U32 i = 0; i < count; ++i)
        {
            cached[pass] |= attributes[i].location < 32 ? 1u << attributes[i].location : 0u;
        }
        cached_mask[pass] = mask;
    }
    return cached[pass];
}

// research/198 3.3: the main view, a reflection probe's face, or a mirror
// (the hero probe). Not the shadows (stage 5), the HUDs, impostors or other
// cameras. research/209: the water mirror and the avatar reflection (plus
// features drawn under a probe face's rules) say which they are (ViewScope).
U32 LLSceneRecords::currentView()
{
    if (LLPipeline::sShadowRender || LLPipeline::sRenderingHUDs || LLPipeline::sImpostorRender || LLPipeline::sReflectionRender)
    {
        return LLRHI::SCENE_VIEW_COUNT;
    }
    if (sViewOverride != U32_MAX)
    {
        return sViewOverride;
    }
    if (gCubeSnapshot)
    {
        return gPipeline.mHeroProbeManager.isMirrorPass() ? LLRHI::SCENE_VIEW_MIRROR : LLRHI::SCENE_VIEW_PROBE;
    }
    if (LLViewerCamera::sCurCameraID != LLViewerCamera::CAMERA_WORLD)
    {
        return LLRHI::SCENE_VIEW_COUNT;
    }
    return LLRHI::SCENE_VIEW_MAIN;
}

// research/209: the reflection probes' faces only with LLRHI_SCENE_PROBE not
// off (verify: compared in turn, on: cut while the scene draws); the mirrors
// and the water mirror as the main view (cut while the scene draws).
// research/218 vk-s2f: auto's progress (probeMode): off after a difference,
// so the faces are not asked of the scene any more.
bool LLSceneRecords::viewWanted(U32 view) const
{
    return view != LLRHI::SCENE_VIEW_PROBE || probeMode() != LLRHI::SceneMode::Off;
}

bool LLSceneRecords::viewCuts(U32 view) const
{
    return view != LLRHI::SCENE_VIEW_PROBE || probeMode() == LLRHI::SceneMode::On;
}

LLSceneRecords::GroupState* LLSceneRecords::groupState(LLSpatialGroup* group, bool create, bool alpha)
{
    auto& groups = alpha ? mAlphaGroups : mGroups;      // research/205 3a-3: the group's alpha records, a group of their own
    const auto it = groups.find(group);
    if (it != groups.end())
    {
        return &it->second;
    }
    if (!create)
    {
        return nullptr;
    }
    const U32 id = LLRHI::sScene->addGroup();
    if (!id)
    {
        return nullptr;
    }
    GroupState& g = groups[group];
    g.id = id;
    // research/218 vk-s4b2: a group stays in its partition's octree for its life.
    const LLSpatialPartition* part = group->getSpatialPartition();
    g.worn = part && worn_partition(part->mPartitionType);
    mGroupSpan = llmax(mGroupSpan, id);
    return &g;
}

void LLSceneRecords::refuse(Refusal why, GroupState* group, InfoState* state, const LLDrawInfo* info)
{
    ++mRefusals[why];
    if (group)
    {
        ++group->refused;
    }
    if (state)
    {
        state->refused = (U8)why;
        if (info && softRefusal(why))
        {
            // Tried again after 1, 2, 4 ... up to RETRY_MAX_GAP rounds (a texture
            // that never gets a number costs little).
            state->retry_gap = (U16)llmin(state->retry_gap ? state->retry_gap * 2u : 1u, RETRY_MAX_GAP);
            state->retry_frame = mFrame + state->retry_gap * RETRY_EVERY;
            mRetry.push_back(info);
        }
    }
}

// A texture gets its number with its first image, a vertex buffer its number
// when the switch turns on: a batch refused for either is tried again now and
// then (else its group would stay on the usual path until it is rebuilt).
bool LLSceneRecords::softRefusal(Refusal why)
{
    return why == REFUSED_NO_TEXTURE || why == REFUSED_NO_PLACEMENT || why == REFUSED_PROGRAM ||
           why == REFUSED_NO_PALETTE;  // research/219: the palette region grows at a later frame's head
}

void LLSceneRecords::retryRefused()
{
    size_t kept = 0;
    for (const LLDrawInfo* info : mRetry)
    {
        const auto it = mInfos.find(info);
        if (it == mInfos.end())
        {
            continue;   // gone
        }
        InfoState& s = it->second;
        if (s.record || s.pending || s.refused >= REFUSED_COUNT || !softRefusal((Refusal)s.refused))
        {
            continue;   // registered, waiting, or refused for good (another batch at the address)
        }
        GroupState* g = groupOf(s);
        if (!g)
        {
            continue;
        }
        if ((S32)(mFrame - s.retry_frame) < 0)
        {
            mRetry[kept++] = info;  // not yet
            continue;
        }
        --g->refused;
        ++g->pending;
        s.refused = REFUSED_COUNT;
        s.pending = true;
        mPending.push_back(info);
        ++mRetried;
    }
    mRetry.resize(kept);
}

void LLSceneRecords::disable(const char* why)
{
    if (mDisabled)
    {
        return;
    }
    LL_WARNS("Scene") << "retained scene records off: " << why << "; every batch is drawn the usual way" << LL_ENDL;
    mDisabled = true;
    for (auto& kv : mInfos)
    {
        if (kv.second.record && kv.second.avatar.notNull())
        {
            kv.second.avatar->rhiPinPaletteSlot(kv.second.skin_hash, -1);  // research/219: the rows go back to the avatars' sweep
        }
    }
    mInfos.clear();
    mSkinRedescribe.clear();
    mGroups.clear();
    mAlphaGroups.clear();
    mAlphaOpen = false;
    mByTexture.clear();
    mTransforms.live.clear();
    mTransforms.orphans.clear();
    mUVTransforms.live.clear();
    mUVTransforms.orphans.clear();
    mPending.clear();
    mRetry.clear();
    // research/204 10 (vk-s4c2): the world groups and slots (their records went with the scene).
    mWorldGroups.clear();
    mWorldTouched.clear();
    for (WorldSlot& slot : mWorldSlots)
    {
        slot = WorldSlot();
    }
    if (mListening)
    {
        LLRHI::Textures::setSamplerListener(nullptr);
        mListening = false;
    }
}

// --- events ----------------------------------------------------------------------

void LLSceneRecords::onCreated(LLDrawInfo& info, LLSpatialGroup* group, U32 pass)
{
    gSceneAlpha.onCreated(info, group, pass);     // research/205: the alpha records (verify)
    if (!active())
    {
        return;
    }
    ++mCreated;
    // research/205 3a-3: the alpha pool's batches, in the group's alpha group.
    const bool alpha = pass == LLRenderPass::PASS_ALPHA && alphaWanted();
    if ((!targetPass(pass) && !alpha) || !group)
    {
        return;     // not the scene's: the group's residency does not depend on it
    }
    const auto old = mInfos.find(&info);
    if (old != mInfos.end())
    {
        // The address of a batch that went without onDestroyed: forget the old one.
        dropInfo(&info, old->second, false);
        mInfos.erase(old);
    }
    GroupState* g = groupState(group, true, alpha);
    if (!g)
    {
        refuse(REFUSED_TABLE, nullptr, nullptr);
        return;
    }
    InfoState& s = mInfos[&info];
    s = InfoState();
    s.group = group;
    s.pass = pass;
    s.alpha = alpha;
    s.pending = true;
    g->infos.push_back(&info);
    g->textures_known = false;
    g->spans_known = false;     // research/205 3a-5
    g->slots_known = false;
    ++g->pending;
    mPending.push_back(&info);
}

bool LLSceneRecords::acquireMatrix(MatrixRows& rows, const LLMatrix4* matrix, U32& slot)
{
    slot = 0;
    if (!matrix)
    {
        return true;    // identity / none
    }
    const auto it = rows.live.find(matrix);
    if (it != rows.live.end())
    {
        ++it->second.users;
        slot = it->second.slot;
        return true;
    }
    const F32* words = &matrix->mMatrix[0][0];     // GL's column-major order
    slot = rows.uv ? LLRHI::sScene->addUVTransform(words) : LLRHI::sScene->addTransform(words);
    if (!slot)
    {
        return false;
    }
    MatrixRow& row = rows.live[matrix];
    row.slot = slot;
    row.users = 1;
    memcpy(row.words, words, sizeof(row.words));
    return true;
}

// A record's use of a row ends (the row goes with its last user).
void LLSceneRecords::releaseMatrix(MatrixRows& rows, const LLMatrix4* matrix, U32 slot)
{
    if (!slot)
    {
        return;
    }
    const auto it = rows.live.find(matrix);
    if (it != rows.live.end() && it->second.slot == slot)
    {
        if (--it->second.users)
        {
            return;
        }
        rows.live.erase(it);
    }
    else
    {
        const auto orphan = rows.orphans.find(slot);
        if (orphan == rows.orphans.end() || --orphan->second)
        {
            return;
        }
        rows.orphans.erase(orphan);
    }
    if (rows.uv)
    {
        LLRHI::sScene->removeUVTransform(slot);
    }
    else
    {
        LLRHI::sScene->removeTransform(slot);
    }
}

// The matrix's memory goes before its records: the row stays theirs (by
// slot), and a matrix made later at the same address gets a new row.
void LLSceneRecords::freeMatrix(MatrixRows& rows, const LLMatrix4* matrix)
{
    const auto it = rows.live.find(matrix);
    if (it == rows.live.end())
    {
        return;
    }
    rows.orphans[it->second.slot] += it->second.users;
    rows.live.erase(it);
}

void LLSceneRecords::matrixWritten(MatrixRows& rows, const LLMatrix4* matrix)
{
    const auto it = rows.live.find(matrix);
    if (it == rows.live.end())
    {
        return;
    }
    const F32* words = &matrix->mMatrix[0][0];
    if (same_words(it->second.words, words))
    {
        return;     // written with what it held (a static drawable's move that changes nothing)
    }
    memcpy(it->second.words, words, sizeof(it->second.words));
    if (rows.uv)
    {
        LLRHI::sScene->setUVTransform(it->second.slot, words);
    }
    else
    {
        LLRHI::sScene->setTransform(it->second.slot, words);
    }
}

// Every live row against its matrix (a few hundred rows: regions, bridges,
// animated texture matrices): the texture matrices' writers are many
// (animateTextures, plus/texanim's descriptions, an animation's end) and
// this is how their rows follow them; a model matrix found changed here was
// written without an event (a place research/198 3.3 missed: counted). Safe
// because a matrix's memory leaves `live` before it goes (onMatrixFreed,
// onTextureMatrixFreed).
U32 LLSceneRecords::sweepRows(MatrixRows& rows)
{
    U32 changed = 0;
    for (auto& kv : rows.live)
    {
        const F32* words = &kv.first->mMatrix[0][0];
        MatrixRow& row = kv.second;
        if (same_words(row.words, words))
        {
            continue;
        }
        memcpy(row.words, words, sizeof(row.words));
        if (rows.uv)
        {
            LLRHI::sScene->setUVTransform(row.slot, words);
        }
        else
        {
            LLRHI::sScene->setTransform(row.slot, words);
        }
        ++changed;
    }
    return changed;
}

// Verify: the row the record reads holds the matrix as it is now.
bool LLSceneRecords::matrixStale(const MatrixRows& rows, const LLMatrix4* matrix, U32 slot) const
{
    if (!matrix || !slot)
    {
        return matrix != nullptr || slot != 0;
    }
    const auto it = rows.live.find(matrix);
    return it == rows.live.end() || it->second.slot != slot || !same_words(it->second.words, &matrix->mMatrix[0][0]);
}

bool LLSceneRecords::describe(const LLDrawInfo& info, LLSpatialGroup* group, U32 pass, LLRHI::SceneRecordDesc& desc, Refusal& why)
{
    desc = LLRHI::SceneRecordDesc();
    // research/205 3a-3: an alpha batch's record is of the alpha pass its
    // program draws (renderAlpha's choice, below).
    const bool alpha = pass == LLRenderPass::PASS_ALPHA || isAlphaPass(pass);
    // research/218 vk-s4b2: an opaque batch worn by an avatar (mWireOwner) has
    // a record: while GrayWire draws its avatar as a wireframe its group is not
    // cut (markVisibleInView). An alpha one none (the scene's alpha does not ask).
    // research/219: a rigged batch only of a scope pass's rigged map (LLRHI_SCENE_RIGGED not off),
    // of an avatar and skin (not a UI avatar's: its own views draw it).
    const bool rigged = info.mAvatar.notNull() || info.mSkinInfo;
    const bool rigged_record = rigged && !alpha && LLRHI::sceneRiggedMode() != LLRHI::SceneMode::Off && rigged_base(pass) != U32_MAX &&
                               info.mAvatar.notNull() && info.mSkinInfo && !info.mAvatar->isUIAvatar();
    if ((rigged && !rigged_record) || (alpha && info.mWireOwner && GrayWire::enabled()))
    {
        why = REFUSED_RIGGED;
        return false;
    }
    if (group->isHUDGroup())
    {
        why = REFUSED_HUD;
        return false;
    }
    const LLSpatialPartition* part = group->getSpatialPartition();
    if (!part || !partition_has_records(part->mPartitionType, alpha, wornMode() != LLRHI::SceneMode::Off))
    {
        why = REFUSED_PARTITION;
        return false;
    }
    if (info.mVertexBuffer.isNull() || !info.mCount)
    {
        why = REFUSED_NO_BUFFER;
        return false;
    }
    if (info.mGLTFMaterial.notNull())
    {
        why = REFUSED_GLTF;
        return false;
    }
    // research/205 3a-3: with the usual blend (a custom one sets the cutoff to
    // 0 around it). 3a-4: a batch with a legacy material is drawn by
    // gDeferredMaterialProgram[its shader mask].
    if (alpha)
    {
        if (info.mBlendFuncSrc != LLRender::BF_SOURCE_ALPHA || info.mBlendFuncDst != LLRender::BF_ONE_MINUS_SOURCE_ALPHA ||
            (info.mMaterial.notNull() && info.mShaderMask >= LLMaterial::SHADER_COUNT))
        {
            why = REFUSED_ALPHA_KIND;
            return false;
        }
        pass = info.mMaterial.notNull() ? alphaMaterialPass(info.mShaderMask) : alphaPass(info.mFullbright);
    }
    // registerFace gives most batches a list of one (texture index 0, the same
    // texture as mTexture); a list of several is drawn with the units bound
    // from it (pushBatch with batch_textures). research/205 H5 (vk-s3a6): a
    // plain or fullbright alpha batch's list is its record's (TexSetup binds
    // entry u to unit u, none missing, and loads no texture matrix: such a
    // batch has none; registerFace batches no material's faces).
    const size_t list_size = info.mTextureList.size();
    if (list_size > 1)
    {
        const LLGLSLShader* program = alpha ? passProgram(pass) : nullptr;
        if (!alpha || info.mMaterial.notNull() || info.mTextureMatrix || list_size > LLRHI::SceneTextureList::MAX || !program ||
            list_size > (size_t)llmax(program->mFeatures.mIndexedTextureChannels, 0))
        {
            why = REFUSED_TEXTURE_LIST;
            return false;
        }
        U16 sampler = 0;    // a list's samplers are its textures' own (the slots follow them)
        for (size_t i = 0; i < list_size; ++i)
        {
            if (info.mTextureList[i].isNull())
            {
                why = REFUSED_TEXTURE_LIST;     // TexSetup leaves that unit as it was
                return false;
            }
            if (!texture_ref(info.mTextureList[i].get(), desc.list.textures[i], sampler))
            {
                why = REFUSED_NO_TEXTURE;
                return false;
            }
        }
        desc.list.count = (U32)list_size;
    }
    if ((info.mNormalMapMatrix && info.mNormalMapMatrix != info.mTextureMatrix) ||
        (info.mSpecularMapMatrix && info.mSpecularMapMatrix != info.mTextureMatrix))
    {
        why = REFUSED_TEXTURE_MATRICES;
        return false;
    }
    if (!buffer_range(info, desc.range))
    {
        why = REFUSED_NO_PLACEMENT;
        return false;
    }
    if (scopePass(pass) || alpha)
    {
        // research/198 5.5: drawn from the place only once it holds what the program reads.
        desc.range.attributes = passAttributes(pass);
        if (!desc.range.attributes)
        {
            why = REFUSED_PROGRAM;
            return false;
        }
        // research/198 5.6: a legacy material pass's program samples its
        // maps; without one the pool's loop keeps the unit's last binding
        // (another batch's): the usual way only.
        const U32 maps = passTextures(pass);
        if (((maps & (1u << SCENE_TEX_NORMAL)) && info.mNormalMap.isNull()) ||
            ((maps & (1u << SCENE_TEX_SPECULAR)) && info.mSpecularMap.isNull()))
        {
            why = REFUSED_MISSING_MAP;
            return false;
        }
    }
    LLRHI::SceneMaterialDesc& m = desc.material;
    if (!texture_ref(info.mTexture.get(), m.textures[SCENE_TEX_DIFFUSE], m.samplers[SCENE_TEX_DIFFUSE]) ||
        !texture_ref(info.mNormalMap.get(), m.textures[SCENE_TEX_NORMAL], m.samplers[SCENE_TEX_NORMAL]) ||
        !texture_ref(info.mSpecularMap.get(), m.textures[SCENE_TEX_SPECULAR], m.samplers[SCENE_TEX_SPECULAR]))
    {
        why = REFUSED_NO_TEXTURE;
        return false;
    }
    m.values[SCENE_MAT_SPEC_R] = info.mSpecColor.mV[0];
    m.values[SCENE_MAT_SPEC_G] = info.mSpecColor.mV[1];
    m.values[SCENE_MAT_SPEC_B] = info.mSpecColor.mV[2];
    m.values[SCENE_MAT_SPEC_EXP] = info.mSpecColor.mV[3];
    m.values[SCENE_MAT_ENV_INTENSITY] = info.mEnvIntensity;
    m.values[SCENE_MAT_ALPHA_CUTOFF] = info.mAlphaMaskCutoff;
    m.values[SCENE_MAT_ALPHA_MODE] = (F32)info.mDiffuseAlphaMode;
    m.values[SCENE_MAT_BUMP] = (F32)info.mBump;
    m.values[SCENE_MAT_SHINY] = (F32)info.mShiny;
    m.values[SCENE_MAT_FULLBRIGHT] = info.mFullbright ? 1.f : 0.f;
    m.values[SCENE_MAT_SHADER_MASK] = (F32)info.mShaderMask;
    if (alpha)
    {
        const S32 place = alpha_place(info, group);
        if (place < 0)
        {
            why = REFUSED_TABLE;    // not in its group's draw map (never so after the rebuild)
            return false;
        }
        m.values[SCENE_MAT_ALPHA_PLACE] = (F32)place;   // exact: a draw map is far shorter than 2^24
    }

    desc.pass = pass;
    desc.vertex_format = info.mVertexBuffer->getTypeMask();
    // research/219: a rigged record's joints from its avatar x skin's resident row (the same row
    // whenever asked again); its bounds are its group's (the CPU cull's, by the avatar), and it
    // has no texture list (a list of several is refused above: not an alpha batch).
    if (rigged_record)
    {
        desc.palette = info.mAvatar.get()->rhiPaletteSlot(info.mSkinInfo);
        if (desc.palette == LLRHIAvatar::SLOT_NONE || desc.palette >= LLRHI::SCENE_PALETTE_LIMIT || desc.list.count)
        {
            why = REFUSED_NO_PALETTE;
            return false;
        }
        desc.loose = true;
        return true;
    }
    desc.loose = !info.mHasExtents;
    if (info.mHasExtents)
    {
        const F32* lo = info.mExtents[0].getF32ptr();
        const F32* hi = info.mExtents[1].getF32ptr();
        for (U32 i = 0; i < 3; ++i)
        {
            desc.lo[i] = lo[i];
            desc.hi[i] = hi[i];
        }
    }
    return true;
}

// What describe makes of the batch (not the rows and group the viewer gives it).
U64 LLSceneRecords::descHash(const LLRHI::SceneRecordDesc& desc)
{
    U64 h = LLRHI::FNV1A_BASIS_SHORT;
    hash_bytes(h, &desc.pass, sizeof(desc.pass));
    hash_bytes(h, &desc.vertex_format, sizeof(desc.vertex_format));
    hash_bytes(h, &desc.range.buffer, sizeof(desc.range.buffer));
    hash_bytes(h, &desc.range.first_index, sizeof(desc.range.first_index));
    hash_bytes(h, &desc.range.index_count, sizeof(desc.range.index_count));
    hash_bytes(h, &desc.range.attributes, sizeof(desc.range.attributes));
    hash_bytes(h, &desc.material, sizeof(desc.material));
    hash_bytes(h, &desc.list, sizeof(desc.list));   // research/205 H5 (0 past its count)
    const U8 loose = desc.loose ? 1 : 0;
    hash_bytes(h, &loose, 1);
    hash_bytes(h, desc.lo, sizeof(desc.lo));
    hash_bytes(h, desc.hi, sizeof(desc.hi));
    hash_bytes(h, &desc.palette, sizeof(desc.palette));     // research/219: a rigged record's row
    return h;
}

// research/205 (vk-s3a6): the glow pass's record of an alpha batch whose
// buffer has glow: its record as it is (its textures or list, texture and
// model matrices, place), drawn by renderEmissives' program (what it reads
// of the buffer). False: no glow record (the loop draws the batch's glow).
bool LLSceneRecords::describeGlow(const LLRHI::SceneRecordDesc& desc, LLRHI::SceneRecordDesc& glow)
{
    glow = desc;
    glow.pass = ALPHA_PASS_GLOW;
    glow.range.attributes = passAttributes(ALPHA_PASS_GLOW);
    return glow.range.attributes != 0;
}

// The batch's textures in the number index (add) or out of it.
void LLSceneRecords::indexTextures(const LLDrawInfo* info, const InfoState& state, bool add)
{
    for (U32 k = 0; k < SCENE_TEX_USED; ++k)
    {
        const U32 id = state.textures[k];
        if (!id || (k && id == state.textures[0]) || (k == SCENE_TEX_SPECULAR && id == state.textures[SCENE_TEX_NORMAL]))
        {
            continue;   // none, or listed once already
        }
        if (add)
        {
            mByTexture.emplace(id, info);
            continue;
        }
        const auto range = mByTexture.equal_range(id);
        for (auto it = range.first; it != range.second; ++it)
        {
            if (it->second == info)
            {
                mByTexture.erase(it);
                break;
            }
        }
    }
}

void LLSceneRecords::registerInfo(const LLDrawInfo* info)
{
    const auto it = mInfos.find(info);
    if (it == mInfos.end() || !it->second.pending)
    {
        return;     // gone, or done already
    }
    InfoState& s = it->second;
    GroupState* g = groupOf(s);
    s.pending = false;
    if (g)
    {
        --g->pending;
    }
    LLRHI::SceneRecordDesc desc;
    Refusal why = REFUSED_TABLE;
    if (!g || !describe(*info, s.group, s.pass, desc, why))
    {
        refuse(why, g, &s, info);
        return;
    }
    const U64 hash = descHash(desc);
    if (!acquireMatrix(mTransforms, info->mModelMatrix, desc.transform))
    {
        refuse(REFUSED_MATRIX, g, &s);
        return;
    }
    if (!acquireMatrix(mUVTransforms, info->mTextureMatrix, desc.uv_transform))
    {
        releaseMatrix(mTransforms, info->mModelMatrix, desc.transform);
        refuse(REFUSED_MATRIX, g, &s);
        return;
    }
    desc.group = g->id;
    s.pass = desc.pass;     // research/205 3a-3: an alpha batch's alpha pass
    s.record = LLRHI::sScene->addRecord(desc);
    if (!s.record)
    {
        releaseMatrix(mTransforms, info->mModelMatrix, desc.transform);
        releaseMatrix(mUVTransforms, info->mTextureMatrix, desc.uv_transform);
        refuse(REFUSED_TABLE, g, &s);
        return;
    }
    s.model = info->mModelMatrix;
    s.texture = info->mTextureMatrix;
    s.model_slot = desc.transform;
    s.texture_slot = desc.uv_transform;
    s.desc_hash = hash;
    s.list = desc.list.count != 0;
    for (U32 k = 0; k < SCENE_TEX_USED; ++k)
    {
        s.textures[k] = desc.material.textures[k].id;
    }
    indexTextures(info, s, true);
    // research/219: a rigged record names its skin's row: the avatar keeps it while it does.
    if (desc.palette != LLRHI::SCENE_NO_PALETTE)
    {
        s.avatar = info->mAvatar;
        s.skin = info->mSkinInfo;
        s.skin_hash = info->mSkinInfo->mHash;
        s.palette = desc.palette;
        s.avatar->rhiPinPaletteSlot(s.skin_hash, 1);
    }
    // research/205 (vk-s3a6): its glow, a record of the glow pass (none: the loop draws it). Not
    // for a batch no face of which glows: plus/alphapass leaves its glow draw out where it can, so
    // nothing may ever copy the emissive array to the pool, and a record never placed would keep
    // its whole group off the scene's alpha.
    LLRHI::SceneRecordDesc glow;
    if (s.alpha && info->mVertexBuffer->hasDataType(LLVertexBuffer::TYPE_EMISSIVE) && info->mRHIGlow != plus::alphapass::GLOW_NONE &&
        describeGlow(desc, glow))
    {
        s.glow_record = LLRHI::sScene->addRecord(glow);
    }
    ++g->records;
    g->textures_known = false;     // research/205 3a-3: the alpha group lists its records' textures
    g->spans_known = false;        // research/205 3a-5
    g->slots_known = false;
    ++mRegistered;
}

// The batch's record goes (or its wait, or its refusal).
void LLSceneRecords::dropInfo(const LLDrawInfo* info, InfoState& state, bool keep_in_group)
{
    GroupState* g = groupOf(state);
    if (state.record)
    {
        dropGlow(state);    // research/205 (vk-s3a6)
        if (state.avatar.notNull())
        {
            state.avatar->rhiPinPaletteSlot(state.skin_hash, -1);  // research/219: the row is the avatar's to sweep again
        }
        LLRHI::sScene->removeRecord(state.record);
        releaseMatrix(mTransforms, state.model, state.model_slot);
        releaseMatrix(mUVTransforms, state.texture, state.texture_slot);
        indexTextures(info, state, false);
        if (g)
        {
            --g->records;
            g->slots_known = false;
            g->spans_known = false;     // research/205 3a-5
        }
        ++mDropped;
    }
    else if (g)
    {
        if (state.pending)
        {
            --g->pending;
        }
        else
        {
            --g->refused;
        }
    }
    state = InfoState();
    if (g && !keep_in_group)
    {
        const auto at = std::find(g->infos.begin(), g->infos.end(), info);
        if (at != g->infos.end())
        {
            *at = g->infos.back();
            g->infos.pop_back();
        }
        g->textures_known = false;
    }
}

// research/205 (vk-s3a6): the batch's glow record goes (its record's rows stay the record's).
// Its group's glow spans and textures are made again (else the spans' check
// would find the old record and count a difference).
void LLSceneRecords::dropGlow(InfoState& state)
{
    if (state.glow_record)
    {
        LLRHI::sScene->removeRecord(state.glow_record);
        state.glow_record = 0;
        if (GroupState* g = groupOf(state))
        {
            g->spans_known = false;
            g->textures_known = false;
        }
    }
}

// A batch with a record that cannot keep it: the record goes, the batch
// stays in its group as refused (the group is drawn the usual way).
void LLSceneRecords::demote(const LLDrawInfo* info, InfoState& state, Refusal why)
{
    LLSpatialGroup* group = state.group;
    const U32 pass = state.pass;
    const bool alpha = state.alpha;
    dropInfo(info, state, true);
    state.group = group;
    state.pass = pass;
    state.alpha = alpha;
    refuse(why, groupOf(state), &state, info);
}

void LLSceneRecords::onDestroyed(const LLDrawInfo& info)
{
    gSceneAlpha.onDestroyed(info);    // research/205
    if (!active())
    {
        // Security audit B 16: the key goes even without a scene (after
        // LLVKScene::shutdown), so a later draw info at the same address
        // never finds this one's state. Nothing of the scene to drop.
        if (!mInfos.empty())
        {
            mInfos.erase(&info);
        }
        return;
    }
    const auto it = mInfos.find(&info);
    if (it == mInfos.end())
    {
        return;
    }
    dropInfo(&info, it->second, false);
    mInfos.erase(it);
}

void LLSceneRecords::onGroupCleared(LLSpatialGroup* group)
{
    gSceneAlpha.onGroupCleared(group);    // research/205
    if (!active())
    {
        return;
    }
    if (GroupState* g = groupState(group, false))
    {
        clearGroup(*g);
    }
    if (GroupState* g = groupState(group, false, true))
    {
        clearGroup(*g);     // research/205 3a-3
    }
}

void LLSceneRecords::clearGroup(GroupState& g)
{
    for (const LLDrawInfo* info : g.infos)
    {
        const auto it = mInfos.find(info);
        if (it != mInfos.end())
        {
            dropInfo(info, it->second, true);
            mInfos.erase(it);
        }
    }
    g.infos.clear();
    g.textures.clear();
    g.textures_known = false;
    g.skins.clear();            // research/219
    g.spans.clear();            // research/205 3a-5
    g.place_records.clear();
    g.glow_spans.clear();       // vk-s3a6
    g.place_glow_records.clear();
    g.places = 0;
    g.spans_known = false;
    g.model_slots.clear();
    g.uv_slots.clear();
    g.slots_known = false;
}

void LLSceneRecords::onGroupDestroyed(LLSpatialGroup* group)
{
    gSceneAlpha.onGroupCleared(group);    // research/205
    if (!active())
    {
        return;
    }
    onGroupCleared(group);
    // research/204 10 (vk-s4c2): a world group's records go with it.
    const auto world = mWorldGroups.find(group);
    if (world != mWorldGroups.end())
    {
        dropWorld(world->second);
        if (world->second.id)
        {
            LLRHI::sScene->removeGroup(world->second.id);
        }
        mWorldGroups.erase(world);
    }
    for (auto* groups : { &mGroups, &mAlphaGroups })
    {
        const auto it = groups->find(group);
        if (it != groups->end())
        {
            LLRHI::sScene->removeGroup(it->second.id);
            groups->erase(it);
        }
    }
}

void LLSceneRecords::onMatrixChanged(const LLMatrix4* matrix)
{
    if (active())
    {
        matrixWritten(mTransforms, matrix);
    }
}

void LLSceneRecords::onMatrixFreed(const LLMatrix4* matrix)
{
    if (active())
    {
        freeMatrix(mTransforms, matrix);
    }
}

void LLSceneRecords::onTextureMatrixFreed(const LLMatrix4* matrix)
{
    if (active())
    {
        freeMatrix(mUVTransforms, matrix);
    }
}

// Textures whose sampler changed (queued by on_sampler_changed): their
// batches' records are written again with the samplers as they are now.
void LLSceneRecords::samplersChanged()
{
    {
        std::lock_guard<std::mutex> lock(sSamplerMutex);
        if (sSamplerChanged.empty())
        {
            return;
        }
        mSamplerScratch.swap(sSamplerChanged);
    }
    std::sort(mSamplerScratch.begin(), mSamplerScratch.end());
    mSamplerScratch.erase(std::unique(mSamplerScratch.begin(), mSamplerScratch.end()), mSamplerScratch.end());
    static std::vector<const LLDrawInfo*> infos;    // main thread; kept for its capacity
    for (U32 id : mSamplerScratch)
    {
        infos.clear();
        const auto range = mByTexture.equal_range(id);
        for (auto it = range.first; it != range.second; ++it)
        {
            infos.push_back(it->second);
        }
        for (const LLDrawInfo* info : infos)
        {
            const auto it = mInfos.find(info);
            if (it == mInfos.end() || !it->second.record)
            {
                continue;
            }
            InfoState& s = it->second;
            LLRHI::SceneRecordDesc desc;
            Refusal why = REFUSED_TABLE;
            const GroupState* g = groupOf(s);
            if (!g || !describe(*info, s.group, s.pass, desc, why))
            {
                demote(info, s, why);   // its group goes to the usual path with it
                continue;
            }
            desc.group = g->id;
            desc.transform = s.model_slot;
            desc.uv_transform = s.texture_slot;
            if (!LLRHI::sScene->updateRecord(s.record, desc))
            {
                demote(info, s, REFUSED_TABLE);
                continue;
            }
            indexTextures(info, s, false);
            s.pass = desc.pass;
            s.desc_hash = descHash(desc);
            if (s.palette != desc.palette && desc.palette != LLRHI::SCENE_NO_PALETTE)
            {
                s.palette = desc.palette;   // research/219: the skin's row now (the pin is the skin's, not the row's)
                if (GroupState* written = groupOf(s))
                {
                    written->textures_known = false;    // its skins' list
                }
            }
            s.list = desc.list.count != 0;
            for (U32 k = 0; k < SCENE_TEX_USED; ++k)
            {
                s.textures[k] = desc.material.textures[k].id;
            }
            indexTextures(info, s, true);
            // research/205 (vk-s3a6): its glow record as its record now (gone if it cannot be).
            LLRHI::SceneRecordDesc glow;
            if (s.glow_record && (!describeGlow(desc, glow) || !LLRHI::sScene->updateRecord(s.glow_record, glow)))
            {
                dropGlow(s);
            }
            ++mRedescribed;
        }
    }
    mSamplerScratch.clear();
}

// --- per frame -------------------------------------------------------------------

void LLSceneRecords::beginFrame()
{
    LLRHI::Scene* scene = LLRHI::sScene;
    if (!scene || mDisabled)
    {
        return;
    }
    const LLRHI::CensusTimer census(LLRHI::CENSUS_SCENE_BEGIN_FRAME);    // research/208 S0
    // Whether the device can hold the scene (asked once by the backend; it
    // also stops for good when it breaks or the device shuts down).
    if (!scene->ready())
    {
        disable(mListening ? "the scene stopped" : "the device cannot hold it");
        return;
    }
    if (!mListening)
    {
        LLRHI::Textures::setSamplerListener(&on_sampler_changed);
        mListening = true;
        mReportAt = LLTimer::getTotalSeconds() + REPORT_SECONDS;
    }
    ++mFrame;
    ++mReportFrames;

    // The last verified frame without a difference counts towards auto.
    if (mVerifyFrame)
    {
        bool any = false;
        for (U64 bits : mPassVerified)
        {
            any = any || bits;
        }
        mCleanFrames = mFrameDiffered ? 0 : (any ? mCleanFrames + VERIFY_EVERY : mCleanFrames);
    }
    // research/198 5.5: the backend's own checks (the GPU's choice, the rows
    // and commands it made, the records' places) count as differences too.
    const U32 problems = scene->problems();
    if (problems != mSeenProblems)
    {
        mSeenProblems = problems;
        mCleanFrames = 0;
        // research/218 vk-s2f: the backend counts them for every view: the parts' autos go off too.
        partChecked(mAlphaAuto, LLRHI::sceneAlphaMode(), "LLRHI_SCENE_ALPHA", true, "the backend's checks found a difference");
        partChecked(mProbeAuto, LLRHI::sceneProbeMode(), "LLRHI_SCENE_PROBE", true, "the backend's checks found a difference");
        worldChecked(true, "the backend's checks found a difference");     // research/204 10 (vk-s4c2)
        if (LLRHI::sceneMode() == LLRHI::SceneMode::Auto && scene->drawing())
        {
            scene->setDrawing(false);
            LL_WARNS("Scene") << "retained scene: the backend's checks found a difference (VKPerf scene line): drawing stops" << LL_ENDL;
        }
        else if (!mProblemLogged)
        {
            mProblemLogged = true;
            LL_WARNS("Scene") << "retained scene: the backend's checks found a difference (VKPerf scene line)" << LL_ENDL;
        }
    }
    if (LLRHI::sceneMode() == LLRHI::SceneMode::Auto && !scene->drawing() && mCleanFrames >= AUTO_FRAMES)
    {
        scene->setDrawing(true);
        if (scene->drawing())
        {
            LL_INFOS("Scene") << "retained scene: " << mCleanFrames << " frames without a difference, drawing" << LL_ENDL;
        }
        else if (!mDrawFailedLogged)
        {
            mDrawFailedLogged = true;
            LL_INFOS("Scene") << "retained scene: " << mCleanFrames
                              << " frames without a difference, but it cannot draw (no binder or expand, research/198 5.5): choosing only"
                              << LL_ENDL;
        }
    }
    mVerifyFrame = !scene->drawing() && mFrame % VERIFY_EVERY == 0;
    mFrameDiffered = false;
    memset(mPassVerified, 0, sizeof(mPassVerified));
    mPaletteChecks = 0;     // research/219
    mWorldBudget = 0;       // research/204 10 (vk-s4c2): the world groups described this frame

    samplersChanged();
    if (mFrame % RETRY_EVERY == 0)
    {
        retryRefused();
    }

    // research/219 (5.3): the rigged records whose skin's row changed, described again now (with
    // the batches waiting: their groups are drawn the usual way until then).
    for (const LLDrawInfo* info : mSkinRedescribe)
    {
        const auto it = mInfos.find(info);
        if (it == mInfos.end() || !it->second.record)
        {
            continue;   // gone, or queued twice (done already)
        }
        InfoState& s = it->second;
        LLSpatialGroup* const group = s.group;
        const U32 pass = s.pass;
        const bool alpha = s.alpha;
        dropInfo(info, s, true);
        s.group = group;
        s.pass = pass;
        s.alpha = alpha;
        if (GroupState* g = groupOf(s))
        {
            s.pending = true;
            ++g->pending;
            g->textures_known = false;
            mPending.push_back(info);
            ++mSkinRedescribed;
        }
    }
    mSkinRedescribe.clear();

    U32 done = 0;
    size_t i = 0;
    for (; i < mPending.size() && done < BUDGET; ++i)
    {
        registerInfo(mPending[i]);
        ++done;
    }
    mPending.erase(mPending.begin(), mPending.begin() + i);
    mDeferred += (U32)mPending.size();

    mSweptTransforms += sweepRows(mTransforms);
    mSweptUV += sweepRows(mUVTransforms);
    mCaughtUp = LLFrameTimer::getFrameCount();  // research/209 S4

    const F64 now = LLTimer::getTotalSeconds();
    if (now >= mReportAt)
    {
        mReportAt = now + REPORT_SECONDS;
        LL_INFOS("Scene") << takeReport(mReportFrames) << LL_ENDL;
        mReportFrames = 0;
    }
}

bool LLSceneRecords::resident(const LLSpatialGroup* group) const
{
    if (!active())
    {
        return false;
    }
    const auto it = mGroups.find(group);
    return it != mGroups.end() && residentState(it->second);
}

// research/206 (stage 5).
U32 LLSceneRecords::shadowGroup(const LLSpatialGroup* group) const
{
    if (!active())
    {
        return 0;
    }
    const auto it = mGroups.find(group);
    if (it == mGroups.end() || !residentState(it->second))
    {
        return 0;
    }
    return LLRHI::sScene->groupDrawable(it->second.id) ? it->second.id : 0;
}

U32 LLSceneRecords::shadowRecord(const LLDrawInfo& info, U32 pass, bool& same) const
{
    same = false;
    if (!active())
    {
        return 0;
    }
    const auto it = mInfos.find(&info);
    if (it == mInfos.end() || !it->second.record)
    {
        return 0;
    }
    const InfoState& s = it->second;
    LLRHI::SceneRecordDesc desc;
    Refusal why = REFUSED_TABLE;
    same = s.pass == pass && describe(info, s.group, s.pass, desc, why) && descHash(desc) == s.desc_hash &&
           s.model == info.mModelMatrix && !matrixStale(mTransforms, s.model, s.model_slot);
    return s.record;
}

bool LLSceneRecords::shadowTextures(const LLSpatialGroup* group, const U64* passes, std::vector<LLViewerTexture*>& out)
{
    if (!active() || !passes)
    {
        return false;
    }
    const auto it = mGroups.find(group);
    if (it == mGroups.end() || !residentState(it->second))
    {
        return false;
    }
    GroupState& g = it->second;
    listTextures(g);
    const size_t first = out.size();
    for (const GroupState::PassTexture& t : g.textures)
    {
        if (!t.diffuse || t.pass >= LLRHI::SCENE_MAX_PASSES || !((passes[t.pass / 64] >> (t.pass % 64)) & 1))
        {
            continue;
        }
        // As markVisibleInView (LLTexUnit::directTexture): no image, or options waiting for a bind.
        const LLImageGL* image = t.texture->getGLTexture();
        if (!image || !image->getTexName() || image->rhiOptionsDirty() || !image->rhiTex().id)
        {
            out.resize(first);
            return false;
        }
        if (std::find(out.begin() + first, out.end(), t.texture) == out.end())
        {
            out.push_back(t.texture);
        }
    }
    return true;
}

// research/198 5.5, 5.6: the textures its scope batches' draws bind (the
// diffuse texture, and the normal and specular maps of the passes whose
// draws bind them), each once per pass.
void LLSceneRecords::listTextures(GroupState& g)
{
    if (g.textures_known)
    {
        return;
    }
    g.textures.clear();
    g.wearers.clear();
    g.skins.clear();
    for (const LLDrawInfo* info : g.infos)
    {
        const auto state = mInfos.find(info);
        if (state == mInfos.end() || !scopePass(state->second.pass))
        {
            continue;
        }
        // research/219: the skins of its rigged records (the record holds the avatar).
        const InfoState& rigged = state->second;
        if (rigged.record && rigged.palette != LLRHI::SCENE_NO_PALETTE && rigged.avatar.notNull() &&
            std::find_if(g.skins.begin(), g.skins.end(), [&](const GroupState::Skin& k)
                         { return k.avatar == rigged.avatar.get() && k.skin == rigged.skin && k.base == rigged.palette; }) == g.skins.end())
        {
            g.skins.push_back({ rigged.avatar.get(), rigged.skin, rigged.palette });
        }
        // research/218 vk-s4b2: the avatars wearing the batches the scene may draw.
        if (info->mWireOwner && std::find(g.wearers.begin(), g.wearers.end(), info->mWireOwner) == g.wearers.end())
        {
            g.wearers.push_back(info->mWireOwner);
        }
        // research/198 5.6: the maps of the passes whose draws bind them too.
        const U32 pass = state->second.pass;
        const U32 maps = passTextures(pass);
        LLViewerTexture* const used[SCENE_TEX_USED] = {
            (maps & (1u << SCENE_TEX_DIFFUSE)) ? info->mTexture.get() : nullptr,
            (maps & (1u << SCENE_TEX_NORMAL)) ? info->mNormalMap.get() : nullptr,
            (maps & (1u << SCENE_TEX_SPECULAR)) ? info->mSpecularMap.get() : nullptr,
        };
        for (U32 k = 0; k < SCENE_TEX_USED; ++k)
        {
            LLViewerTexture* const texture = used[k];
            if (!texture)
            {
                continue;
            }
            const bool diffuse = k == SCENE_TEX_DIFFUSE;
            const auto listed = std::find_if(g.textures.begin(), g.textures.end(), [&](const GroupState::PassTexture& t)
                                             { return t.pass == pass && t.texture == texture; });
            if (listed == g.textures.end())
            {
                g.textures.push_back({ pass, texture, diffuse });
            }
            else
            {
                listed->diffuse = listed->diffuse || diffuse;
            }
        }
    }
    g.textures_known = true;
}

// research/219 (5.2). Each skin's palette is built once a frame by the first
// view or pass that needs it (LLVOAvatar::updateSkinInfoMatrixPalette), so
// the order of the views does not matter. A row that changed (the avatar's
// rows given back, another row given) queues the records naming the old one
// to be described again at the next frame's head (not now: the backend's
// table may be the render thread's during the frame).
bool LLSceneRecords::readySkins(GroupState& g, U64* reasons)
{
    static_assert(PALETTE_REASONS == LLVOAvatar::RHI_PALETTE_REASONS, "a count per reason a skin is not ready");
    listTextures(g);
    for (const GroupState::Skin& k : g.skins)
    {
        const LLVOAvatar::RHIScenePalette ready = k.avatar->rhiScenePalette(k.skin, k.base);
        if (ready == LLVOAvatar::RHI_PALETTE_READY)
        {
            continue;
        }
        ++reasons[ready];
        if (ready == LLVOAvatar::RHI_PALETTE_ROW_CHANGED)
        {
            for (const LLDrawInfo* info : g.infos)
            {
                const auto state = mInfos.find(info);
                if (state != mInfos.end() && state->second.record && state->second.palette == k.base &&
                    state->second.skin == k.skin && state->second.avatar.get() == k.avatar)
                {
                    mSkinRedescribe.push_back(info);
                }
            }
        }
        return false;
    }
    return true;
}

// research/219 (8.2): the records the view's cut passes would draw, each with
// the mark its pass draws with: the rigged passes' records with the plan's
// avatar mark (SceneMarkScope), the others' with none (between the usual
// way's batches the mark is 0). Records of passes the view does not cut stay
// on the usual way (which marks them per batch).
bool LLSceneRecords::marksFit(const GroupState& g, U32 view) const
{
    plus::skinsss::MarkPlan plan;
    plan.marking = mMarkPlanMarking;
    plan.avatar_bit = mMarkPlanAvatarBit;
    plan.avatar_mark = mMarkPlanAvatarMark;
    for (const LLDrawInfo* info : g.infos)
    {
        const auto state = mInfos.find(info);
        if (state == mInfos.end() || !state->second.record || !cutsPass(view, state->second.pass))
        {
            continue;
        }
        U32 mark = 0;
        const bool known = plus::skinsss::sceneMarkOf(*info, plan, mark);
        if (!record_mark_fits(true, known, mark, state->second.palette != LLRHI::SCENE_NO_PALETTE, plan.avatar_mark))
        {
            return false;
        }
    }
    return true;
}

bool LLSceneRecords::shadowSkinsReady(const LLSpatialGroup* group)
{
    if (!active())
    {
        return false;
    }
    const auto it = mGroups.find(group);
    if (it == mGroups.end())
    {
        return false;
    }
    return readySkins(it->second, mShadowSkinsNotReady);
}

// research/219: a check of rigged batches (several in a frame count once); a
// difference turns the rigged passes' auto off (only theirs).
void LLSceneRecords::riggedChecked(bool differ, const char* why)
{
    mRiggedDifferences += differ ? 1 : 0;
    partChecked(mRiggedAuto, LLRHI::sceneRiggedMode(), "LLRHI_SCENE_RIGGED", differ, why);
}

bool LLSceneRecords::drawing() const
{
    return active() && LLRHI::sScene->drawing();
}

// research/209 S4: the mirrors and the water mirror are drawn after the idle
// (which writes the texture matrices and animations) but before beginFrame,
// whose sweep their rows would wait for: the first such face after the idle
// sweeps them (and takes the samplers changed) itself. The reflection probes'
// faces come after the display, whose beginFrame caught up with this idle.
void LLSceneRecords::catchUp()
{
    const U32 count = LLFrameTimer::getFrameCount();
    if (mCaughtUp == count || !mListening)
    {
        return;     // swept since the idle (or beginFrame has not run once: nothing has rows yet)
    }
    mCaughtUp = count;
    samplersChanged();
    mSweptTransforms += sweepRows(mTransforms);
    mSweptUV += sweepRows(mUVTransforms);
}

// research/209 S5: the group's rows to the view's lists (each once).
void LLSceneRecords::listSlots(U32 view, GroupState& g)
{
    if (!g.slots_known)
    {
        g.model_slots.clear();
        g.uv_slots.clear();
        for (const LLDrawInfo* info : g.infos)
        {
            const auto state = mInfos.find(info);
            if (state == mInfos.end() || !state->second.record)
            {
                continue;
            }
            const U32 model = state->second.model_slot;
            const U32 uv = state->second.texture_slot;
            if (model && std::find(g.model_slots.begin(), g.model_slots.end(), model) == g.model_slots.end())
            {
                g.model_slots.push_back(model);
            }
            if (uv && std::find(g.uv_slots.begin(), g.uv_slots.end(), uv) == g.uv_slots.end())
            {
                g.uv_slots.push_back(uv);
            }
        }
        g.slots_known = true;
    }
    auto add = [&](std::vector<U32>& listed, std::vector<U32>& out, U32 slot)
    {
        if (slot >= listed.size())
        {
            listed.resize((size_t)slot + 1, 0);
        }
        if (listed[slot] != mViewSerial)
        {
            listed[slot] = mViewSerial;
            out.push_back(slot);
        }
    };
    for (U32 slot : g.model_slots)
    {
        add(mTransformListed, mViewTransforms[view], slot);
    }
    for (U32 slot : g.uv_slots)
    {
        add(mUVListed, mViewUVs[view], slot);
    }
}

U32 LLSceneRecords::beginView(LLCamera& camera)
{
    if (!active())
    {
        return LLRHI::SCENE_VIEW_COUNT;
    }
    const LLRHI::CensusTimer census(LLRHI::CENSUS_SCENE_VIEWS);  // research/208 S0
    const U32 view = currentView();
    if (view >= LLRHI::SCENE_VIEW_COUNT)
    {
        return view;
    }
    // research/198 5.5: the passes this view's postSort cuts, cleared first
    // (research/209: a probe face of a kind the scene does not take keeps
    // none, so the probe list and drawPass see nothing of a face before).
    memset(mCutPasses[view], 0, sizeof(mCutPasses[view]));
    mCutGroups[view].clear();
    mViewTextures[view].clear();
    mViewTexturesSorted[view] = false;
    mViewTurn[view] = false;
    mViewCompare[view] = false;
    memset(mViewPassVerified[view], 0, sizeof(mViewPassVerified[view]));
    if (view != LLRHI::SCENE_VIEW_MAIN)
    {
        if (!viewWanted(view))
        {
            return LLRHI::SCENE_VIEW_COUNT;     // research/209: the probes' faces with LLRHI_SCENE_PROBE off
        }
        catchUp();      // research/209 S4
        ++mViewFaces[view];
    }
    // research/209 S5: the rows of the view's visible groups (0, none, first).
    mViewSerial = mViewSerial + 1 ? mViewSerial + 1 : 1;
    if (mViewSerial == 1)
    {
        std::fill(mTransformListed.begin(), mTransformListed.end(), 0u);
        std::fill(mUVListed.begin(), mUVListed.end(), 0u);
    }
    mViewTransforms[view].assign(1, 0);
    mViewUVs[view].assign(1, 0);
    ViewInput& v = mViews[view];
    v.desc = LLRHI::SceneViewDesc();
    const U32 planes = llmin(camera.getAgentPlaneCount(), (U32)LLCamera::AGENT_PLANE_USER_CLIP_NUM);
    for (U32 i = 0; i < planes; ++i)
    {
        if (camera.getAgentPlaneMask(i) >= LLCamera::PLANE_MASK_NUM)
        {
            continue;   // a plane LLCamera does not test (AABBInFrustum skips it too)
        }
        const LLPlane& p = camera.getAgentPlane(i);
        F32* out = v.desc.planes[v.desc.plane_count++];
        for (S32 k = 0; k < 4; ++k)
        {
            out[k] = p[k];
        }
    }
    const LLVector3& origin = camera.getOrigin();
    v.desc.origin[0] = origin.mV[0];
    v.desc.origin[1] = origin.mV[1];
    v.desc.origin[2] = origin.mV[2];
    v.bits.assign((mGroupSpan + 31) / 32, 0);
    if (view == LLRHI::SCENE_VIEW_MAIN)
    {
        // research/204 10 (vk-s4c2): the world pools' faces are walked under this view's serial (never 0).
        mWorldSerial = mViewSerial;
        mWorldTouched.clear();
        mMainViewFrame = mFrame;    // research/205 3a-3: the alpha view takes its planes
        // research/218 vk-s4b2: whether this frame's G-buffer pass will mark skin (asked once a frame).
        mSkinMarking = plus::skinsss::markingWanted();
        // research/219 (8.2): and how, for the rigged records (only while there are any).
        plus::skinsss::MarkPlan plan;
        if (mSkinMarking && riggedMode() == LLRHI::SceneMode::On)
        {
            plan = plus::skinsss::mainViewMark();
        }
        mMarkPlanMarking = plan.marking;
        mMarkPlanAvatarBit = plan.avatar_bit;
        mMarkPlanAvatarMark = plan.avatar_mark;
    }
    mRiggedCuts[view] = false;

    // research/198 5.5: the passes this view's postSort cuts while the scene
    // draws. research/209 (stage 2e): the main view, the mirrors' faces and
    // the water mirror, and the reflection probes' faces with
    // LLRHI_SCENE_PROBE=on (the probe list leaves out what is cut, rhiProbeFace;
    // the probes' static prefix keeps PASS_SIMPLE, keepPass).
    bool any_cut = false;
    if (viewCuts(view) && LLRHI::sScene->drawing())
    {
        // research/198 5.6: other readers of a scope pass's render map. The
        // alpha pool's debug highlight draws the masks' again
        // (renderDebugAlpha). The depth pre-pass (display, RenderDepthPrePass;
        // the main view only, a cube face has none) draws PASS_SIMPLE's and
        // PASS_FULLBRIGHT's depth before the G-buffer: without PASS_SIMPLE's
        // the G-buffer comes out the same (its own batches win LEQUAL there
        // anyway), but fullbright is drawn after the lighting, so the
        // G-buffer under it would differ: not cut then.
        // research/219: the rigged maps (the pre-pass draws none) only while the rigged passes are on.
        static LLCachedControl<bool> depth_pre_pass(gSavedSettings, "RenderDepthPrePass", false);
        const bool main_view = view == LLRHI::SCENE_VIEW_MAIN;
        const bool rigged_on = riggedMode() == LLRHI::SceneMode::On;
        for (U32 pass = 0; pass < LLRHI::SCENE_MAX_PASSES; ++pass)
        {
            const bool other_reader = (LLDrawPoolAlpha::sShowDebugAlpha && debug_alpha_reads(pass)) ||
                                      (main_view && depth_pre_pass && pass == LLRenderPass::PASS_FULLBRIGHT);
            const bool rigged = rigged_base(pass) != U32_MAX;
            if (scopePass(pass) && (!rigged || rigged_on) && !other_reader && LLRHI::sScene->canDrawPass(view, pass))
            {
                mCutPasses[view][pass / 64] |= 1ull << (pass % 64);
                any_cut = true;
                mRiggedCuts[view] = mRiggedCuts[view] || rigged;
            }
        }
    }
    // research/209 S7: a probe face's or mirror's turn to be compared (one
    // face of one kind a turn, the backend's): the backend compares its choice
    // and an expand; with nothing cut the viewer compares its render map too,
    // which postSort then builds as LL does (comparesView).
    if (view != LLRHI::SCENE_VIEW_MAIN)
    {
        mViewTurn[view] = LLRHI::sScene->viewTurn(view);
        mViewCompare[view] = mViewTurn[view] && !any_cut;
    }
    return view;
}

// While the view cuts passes, only a group the scene can draw now (every
// record placed with what its pass reads, every texture of its scope batches
// made with its options applied) is visible to the scene and cut; the others
// stay whole on the usual way. Else every resident group is (the choice
// verify compares).
bool LLSceneRecords::markVisibleInView(U32 view, const LLSpatialGroup* group)
{
    if (!active())
    {
        return false;
    }
    const LLRHI::CensusTimer census(LLRHI::CENSUS_SCENE_VIEWS);  // research/208 S0
    const auto it = mGroups.find(group);
    if (it == mGroups.end() || !residentState(it->second))
    {
        return false;
    }
    GroupState& g = it->second;
    const U32 bit = g.id - 1;
    std::vector<U32>& bits = mViews[view].bits;
    if (bit / 32 >= bits.size())
    {
        return false;   // made after the view began: the usual path draws it this time
    }
    bool cuts = false;
    for (U64 word : mCutPasses[view])
    {
        cuts = cuts || word;
    }
    // The textures of its scope batches: for the scene's draws (cut), and
    // for the backend's verify of the rows (the main view, whose verified
    // frames the backend picks by its own frame count; research/209: a probe
    // face's or mirror's turn).
    const bool textures = cuts || view == LLRHI::SCENE_VIEW_MAIN || mViewTurn[view];
    if (textures)
    {
        listTextures(g);
    }
    if (cuts)
    {
        U64& uncut = view == LLRHI::SCENE_VIEW_MAIN ? mUncutResident : mViewUncut[view];
        if (!LLRHI::sScene->groupDrawable(g.id))
        {
            ++uncut;
            return false;
        }
        // research/218 vk-s4b2: a worn object's group, or one of a wearer drawn
        // apart (a wireframe, the skin mark), whole on the usual way: not
        // visible to the scene's choice of this view either (it would draw it).
        // research/219 (8.2): a group with rigged records under the skin mark
        // only when each record's mark is its pass's (marksFit).
        const bool main_view = view == LLRHI::SCENE_VIEW_MAIN;
        const bool marks_fit = main_view && mSkinMarking && mMarkPlanMarking && !g.skins.empty() && marksFit(g, view);
        if (main_view && mSkinMarking && mRiggedCuts[view] && !g.skins.empty() && !marks_fit)
        {
            ++mRiggedKeptForMark;
        }
        if (!worn_group_cut(g.worn, wornMode() == LLRHI::SceneMode::On, g.wearers, view == LLRHI::SCENE_VIEW_MAIN, mSkinMarking && !marks_fit,
                            GrayWire::enabled(), [](const LLVOAvatar* wearer) { return GrayWire::avatarDrawnApart(wearer); })) // <plus> research/259: its band too </plus>
        {
            ++mWornKept;
            return false;
        }
        // research/219 (5.2): its rigged records' palettes ready in their rows, where a rigged pass is cut.
        if (mRiggedCuts[view] && !g.skins.empty() && !readySkins(g, mSkinsNotReady))
        {
            ++uncut;
            return false;
        }
        for (const GroupState::PassTexture& t : g.textures)
        {
            // As LLTexUnit::directTexture: no image, or options waiting for a
            // bind: the usual way binds it (and applies them) this frame.
            const LLImageGL* image = t.texture->getGLTexture();
            // <plus> research/256: nor while the materialize band sweeps its surfaces (GrayWire draws them the usual way)
            if (!image || !image->getTexName() || image->rhiOptionsDirty() || image->bandHeld())
            // </plus>
            {
                ++uncut;
                return false;
            }
        }
    }
    if (textures)
    {
        for (const GroupState::PassTexture& t : g.textures)
        {
            const LLImageGL* image = t.texture->getGLTexture();
            const U32 id = image ? image->rhiTex().id : 0;
            if (id)
            {
                mViewTextures[view].push_back(((U64)t.pass << 32) | id);
            }
            if (cuts)
            {
                t.texture->setActive();     // what the usual way's draw of it does
            }
        }
        mViewTexturesSorted[view] = false;
    }
    bits[bit / 32] |= 1u << (bit % 32);
    listSlots(view, g);     // research/209 S5
    if (!cuts)
    {
        return false;
    }
    mCutGroups[view].push_back(group);
    mWornCut += g.worn ? 1 : 0;     // research/218 vk-s4b2
    mRiggedGroupsCut += mRiggedCuts[view] && !g.skins.empty() ? 1 : 0;     // research/219
    if (view == LLRHI::SCENE_VIEW_MAIN)
    {
        ++mCutGroupCount;
    }
    else
    {
        ++mViewCutGroups[view];
    }
    return true;
}

void LLSceneRecords::endView(U32 view)
{
    if (!active() || view >= LLRHI::SCENE_VIEW_COUNT)
    {
        return;
    }
    const LLRHI::CensusTimer census(LLRHI::CENSUS_SCENE_VIEWS);  // research/208 S0
    // research/204 10 (vk-s4c2): the world groups markWorldPool walked, shown (or not) before the bits go.
    if (worldWanted(view))
    {
        decideWorld(view);
    }
    const ViewInput& v = mViews[view];
    LLRHI::sScene->setView(view, v.desc);
    LLRHI::sScene->setGroupVisible(view, v.bits.data(), (U32)v.bits.size());
}

const std::vector<LLDrawInfo*>& LLSceneRecords::drawPassInView(U32 pass)
{
    const LLRHI::CensusTimer census(LLRHI::CENSUS_SCENE_PASS_ENTRY);     // research/208 S0 (the backend's drawPass apart)
    mRescue.clear();
    const U32 view = currentView();
    if (view >= LLRHI::SCENE_VIEW_COUNT || !targetPass(pass) || !gPipeline.hasRenderType(pass))
    {
        return mRescue;     // the render map has none of the pass (postSort's hasRenderType)
    }
    // Verify: the main view's pass, once a verified frame (a pass a pool draws
    // twice is compared once). research/209 S7: a probe face's or mirror's,
    // once in its turn.
    const bool main_view = view == LLRHI::SCENE_VIEW_MAIN;
    U64& verified = main_view ? mPassVerified[pass / 64] : mViewPassVerified[view][pass / 64];
    const U64 bit = 1ull << (pass % 64);
    if ((main_view ? mVerifyFrame : mViewCompare[view]) && !(verified & bit))
    {
        verified |= bit;
        mOldDrawn.clear();
        const LLCullResult::drawinfo_iterator end = gPipeline.endRenderMap(pass);
        for (LLCullResult::drawinfo_iterator i = gPipeline.beginRenderMap(pass); i != end; ++i)
        {
            if (*i)
            {
                noteDrawn(view, pass, **i);
            }
        }
        endPassVerify(view, pass);
        mViewCompared[view] += main_view ? 0 : 1;
    }
    // research/198 5.5: the pass as the scene draws it (cut), or only chosen.
    // research/209: a probe face or mirror is chosen only when cut or in its
    // turn (else nothing of it is asked of the backend).
    const bool cut = cutsPass(view, pass) && !mCutGroups[view].empty();
    if (!cut && (main_view ? LLRHI::sScene->drawing() : !mViewTurn[view]))
    {
        return mRescue;     // drawing, and nothing of this view's pass is the scene's
    }
    LLRHI::SceneDrawContext ctx;
    ctx.draw = cut;
    ctx.scope = scopePass(pass);
    // research/198 5.6: the pass's pool binds its program before the pass's
    // entry; a pool that bound another (a setting, a debug mode) draws the
    // usual way (its records were described for the pass's program).
    LLGLSLShader* const shader = LLGLSLShader::sCurBoundShaderPtr;
    bool drawable = ctx.scope && shader && shader == passProgram(pass);
    if (drawable)
    {
        // What the usual way's batches of the pass start from (pushDirectBatch).
        ctx.view_matrix = gGLModelView;
        ctx.projection = glm::value_ptr(gGL.getProjectionMatrix());
        ctx.fill = &fill_matrices;
        ctx.attribute_count = LLVertexBuffer::getAttributeLayout(shader->mAttributeMask, mLayout);
        ctx.attributes = mLayout;
        // The units the batch's textures go on: pushBatch's unit 0 (a list
        // of one), LLDrawPoolMaterials' channels of the program.
        const U32 maps = passTextures(pass);
        auto unit = [&](S32 channel, SceneMaterialTexture texture)
        {
            if (channel < 0 || channel >= (S32)LLRHI::SceneDrawContext::UNITS)
            {
                drawable = false;   // a unit the context cannot name
                return;
            }
            ctx.unit_texture[channel] = (S8)texture;
        };
        if (material_program_index(pass) >= 0)
        {
            unit(shader->getTextureChannel(LLShaderMgr::DIFFUSE_MAP), SCENE_TEX_DIFFUSE);
            if (maps & (1u << SCENE_TEX_NORMAL))
            {
                unit(shader->getTextureChannel(LLShaderMgr::BUMP_MAP), SCENE_TEX_NORMAL);
            }
            if (maps & (1u << SCENE_TEX_SPECULAR))
            {
                unit(shader->getTextureChannel(LLShaderMgr::SPECULAR_MAP), SCENE_TEX_SPECULAR);
            }
        }
        else
        {
            unit(0, SCENE_TEX_DIFFUSE);
        }
        ctx.materials = passMaterials(pass, ctx.material_count);
        std::vector<U64>& textures = mViewTextures[view];
        if (!mViewTexturesSorted[view])
        {
            std::sort(textures.begin(), textures.end());
            textures.erase(std::unique(textures.begin(), textures.end()), textures.end());
            mViewTexturesSorted[view] = true;
        }
        // The pass's numbers (sorted by pass, then number: each once).
        mPassTextures.clear();
        for (auto it = std::lower_bound(textures.begin(), textures.end(), (U64)pass << 32);
             it != textures.end() && (U32)(*it >> 32) == pass; ++it)
        {
            mPassTextures.push_back((U32)*it);
        }
        ctx.textures = mPassTextures.data();
        ctx.texture_count = (U32)mPassTextures.size();
        // research/209 S5: the rows the view's records read (0 first: never empty).
        ctx.transform_slots = mViewTransforms[view].data();
        ctx.transform_slot_count = (U32)mViewTransforms[view].size();
        ctx.uv_slots = mViewUVs[view].data();
        ctx.uv_slot_count = (U32)mViewUVs[view].size();
    }
    if (cut && main_view)
    {
        ++mScenePasses;
    }
    else if (cut)
    {
        ++mViewPasses[view];
    }
    // research/219: a rigged pass's records are drawn with one mark in the main view's G-buffer
    // pass (8.2: the plan's, while the scope lives; a plan the pass turned out not to follow: the
    // usual way draws them, and marks them) and one value of plusHairSway (4: none).
    const bool rigged_draw = cut && drawable && rigged_base(pass) != U32_MAX;
    plus::skinsss::MarkPlan plan;
    plan.marking = mMarkPlanMarking;
    plan.avatar_bit = mMarkPlanAvatarBit;
    plan.avatar_mark = mMarkPlanAvatarMark;
    const plus::skinsss::SceneMarkScope mark_scope(rigged_draw && main_view ? plan.avatar_mark : 0u, plan);
    const bool mark_wrong = rigged_draw && main_view && !mark_scope.holds();
    if (mark_wrong)
    {
        drawable = false;
        ++mMarkPredictedWrong;
    }
    else if (rigged_draw)
    {
        plus::hairsway::clearShader(shader);
    }
    if (ctx.scope && !drawable)
    {
        ctx = LLRHI::SceneDrawContext();    // nothing to draw or verify with: only chosen (a cut pass is drawn the usual way below)
        ctx.draw = cut;
    }
    ctx.verify_view = !main_view && mViewTurn[view];    // research/209 S7
    if ((cut && !drawable) || (!LLRHI::sScene->drawPass(view, pass, ctx) && cut))
    {
        // postSort left these groups' batches of the pass out of the render
        // map and the scene could not draw them: the caller draws them the
        // usual way (the backend stops taking the pass for a while, and
        // gives the reason in its VKPerf line).
        if (!mark_wrong)
        {
            ++mDrawFailed;
            LL_WARNS_ONCE("Scene") << "retained scene: the scene could not draw pass " << pass << " of view " << view
                                   << "; its cut batches are drawn the usual way" << LL_ENDL;
        }
        for (const LLSpatialGroup* group : mCutGroups[view])
        {
            const auto batches = group->mDrawMap.find(pass);
            if (batches == group->mDrawMap.end())
            {
                continue;
            }
            for (const LLPointer<LLDrawInfo>& info : batches->second)
            {
                if (info.notNull())
                {
                    mRescue.push_back(info.get());
                }
            }
        }
        mRescued += mRescue.size();
        LLRHI::renderThreadCensusRescue((U32)mRescue.size());  // research/208 2.4: an answer the main thread waits for
    }
    return mRescue;
}

// --- research/205 3a-3: an OIT "all" session's alpha ---------------------------
// (3a-5: and the sorted way, a hybrid session's world half and alpha without OIT)

// The alpha view: the main view's planes this frame, no group visible yet.
bool LLSceneRecords::beginAlphaView(bool sorted)
{
    mAlphaOpen = false;
    mAlphaSorted = false;
    if (!active() || !alphaWanted() || mMainViewFrame != mFrame || currentView() != LLRHI::SCENE_VIEW_MAIN)
    {
        return false;   // no main view this frame (its planes are the alpha view's), or a probe face's or mirror's alpha
    }
    if (alphaMode() == LLRHI::SceneMode::Off)
    {
        return false;   // research/218 vk-s2f: its auto found a difference: the pool's loops draw it all
    }
    mAlphaView.desc = mViews[LLRHI::SCENE_VIEW_MAIN].desc;
    mAlphaView.bits.assign((mGroupSpan + 31) / 32, 0);
    if (mAlphaPriority.size() < mGroupSpan)
    {
        mAlphaPriority.resize(mGroupSpan, 0);   // only the visible groups' are read (each written below)
    }
    if (sorted && mAlphaBase.size() < mGroupSpan)
    {
        mAlphaBase.resize(mGroupSpan, 0);       // as the priorities
    }
    mAlphaTextures.clear();
    mAlphaSerial = mAlphaSerial + 1 ? mAlphaSerial + 1 : 1;
    std::fill(std::begin(mAlphaPassDrawn), std::end(mAlphaPassDrawn), false);
    mAlphaAnyDrawn = false;
    mAlphaPassMask = 0;
    mAlphaCommands = 0;
    mAlphaSorted = sorted;
    mAlphaOpen = true;
    ++mAlphaPasses;
    mSortedPasses += sorted ? 1 : 0;
    return true;
}

// A group the pool's loop draws in the pass: visible to the alpha view when
// its alpha records can be drawn now (every record of its alpha group placed
// with what its pass reads, every texture of them made with its options
// applied: else the loop draws all of its batches, as for the opaque cut).
// Batches without a record (pending, refused) are the loop's either way.
// research/205 3a-5, sorted: and when its spans are known (made again after
// its records changed); its commands then follow the visible groups' before
// it in the passes' arrays, a command per place of its draw map.
void LLSceneRecords::markAlphaVisible(LLSpatialGroup* group, U32 priority)
{
    if (!mAlphaOpen)
    {
        return;
    }
    const auto it = mAlphaGroups.find(group);
    if (it == mAlphaGroups.end() || !it->second.records)
    {
        return;
    }
    GroupState& g = it->second;
    const U32 bit = g.id - 1;
    if (bit / 32 >= mAlphaView.bits.size() || bit >= mAlphaPriority.size() || (mAlphaSorted && bit >= mAlphaBase.size()))
    {
        return;     // made after the view began: the loop draws it this time
    }
    if (!LLRHI::sScene->groupDrawable(g.id))
    {
        ++mAlphaUncut;
        return;
    }
    if (mAlphaSorted)
    {
        if (!g.spans_known)
        {
            makeAlphaSpans(group, g.spans, g.place_records, g.places, &g.glow_spans, &g.place_glow_records);
            g.spans_known = true;
        }
        else if (mFrame % VERIFY_EVERY == 0)
        {
            checkAlphaSpans(group, g);
        }
        if (!g.places || (g.spans.empty() && g.glow_spans.empty()) || mAlphaCommands + g.places < mAlphaCommands)
        {
            ++mAlphaUncut;
            return;     // a draw map too long for the places, or no record in it now: the loop draws it
        }
    }
    if (!g.textures_known)
    {
        g.textures.clear();
        g.alpha_passes = 0;
        for (const LLDrawInfo* info : g.infos)
        {
            const auto state = mInfos.find(info);
            if (state == mInfos.end() || !state->second.record || !isAlphaPass(state->second.pass))
            {
                continue;
            }
            const U32 pass = state->second.pass;
            g.alpha_passes |= 1u << alphaPassIndex(pass);
            g.alpha_passes |= state->second.glow_record ? 1u << ALPHA_GLOW_INDEX : 0u;     // research/205 (vk-s3a6)
            auto add = [&](U32 at, LLViewerTexture* texture)
            {
                const bool listed = std::find_if(g.textures.begin(), g.textures.end(), [&](const GroupState::PassTexture& t)
                                                 { return t.pass == at && t.texture == texture; }) != g.textures.end();
                if (!listed)
                {
                    g.textures.push_back({ at, texture });
                }
            };
            // research/205 3a-4: a material's normal and specular maps too (the
            // textures its pass's draws sample), each with its pass. H5: a
            // record's list instead of its diffuse texture. The glow pass
            // samples the diffuse texture or the list (TexSetup's for it).
            const U32 maps = passTextures(pass);
            LLViewerTexture* const textures[SCENE_TEX_USED] = { info->mTexture.get(), info->mNormalMap.get(), info->mSpecularMap.get() };
            for (U32 k = 0; k < SCENE_TEX_USED; ++k)
            {
                LLViewerTexture* texture = textures[k];
                if (!texture || !(maps & (1u << k)) || (k == SCENE_TEX_DIFFUSE && state->second.list))
                {
                    continue;
                }
                add(pass, texture);
                if (k == SCENE_TEX_DIFFUSE && state->second.glow_record)
                {
                    add(ALPHA_PASS_GLOW, texture);
                }
            }
            if (state->second.list)
            {
                for (const LLPointer<LLViewerTexture>& texture : info->mTextureList)
                {
                    if (texture.notNull())
                    {
                        add(pass, texture.get());
                        if (state->second.glow_record)
                        {
                            add(ALPHA_PASS_GLOW, texture.get());
                        }
                    }
                }
            }
        }
        g.textures_known = true;
    }
    for (const GroupState::PassTexture& t : g.textures)
    {
        // As LLTexUnit::directTexture: no image, or options waiting for a
        // bind: the loop binds it (and applies them) this frame.
        const LLImageGL* image = t.texture->getGLTexture();
        if (!image || !image->getTexName() || image->rhiOptionsDirty())
        {
            ++mAlphaUncut;
            return;
        }
    }
    for (const GroupState::PassTexture& t : g.textures)
    {
        const U32 id = t.texture->getGLTexture()->rhiTex().id;
        if (id)
        {
            mAlphaTextures.push_back(((U64)t.pass << 32) | id);
        }
        t.texture->setActive();     // what the loop's draw of it does
    }
    mAlphaView.bits[bit / 32] |= 1u << (bit % 32);
    mAlphaPriority[bit] = priority;
    if (mAlphaSorted)
    {
        mAlphaBase[bit] = mAlphaCommands;
        mAlphaCommands += g.places;
    }
    mAlphaPassMask |= g.alpha_passes;
    g.cut_serial = mAlphaSerial;
    ++mAlphaGroupsCut;
}

void LLSceneRecords::endAlphaView()
{
    if (!mAlphaOpen)
    {
        return;
    }
    std::sort(mAlphaTextures.begin(), mAlphaTextures.end());
    mAlphaTextures.erase(std::unique(mAlphaTextures.begin(), mAlphaTextures.end()), mAlphaTextures.end());
    LLRHI::sScene->setView(LLRHI::SCENE_VIEW_ALPHA, mAlphaView.desc);
    LLRHI::sScene->setGroupVisible(LLRHI::SCENE_VIEW_ALPHA, mAlphaView.bits.data(), (U32)mAlphaView.bits.size());
}

// The pass's records of the visible groups, with the pass's program bound
// and the pool's state for its batches set (renderAlpha's for a plain or
// fullbright batch; 3a-4: a material's program, its maps and uniforms per
// record). Drawn while LLRHI_SCENE_ALPHA is on and the scene draws; else
// (verify) chosen and expanded only. research/205 3a-5, sorted: expanded
// with each record's command at its group's base plus its place, for the
// spans the loop has the scene draw (drawAlphaSpan); nothing drawn here.
bool LLSceneRecords::drawAlphaPass(U32 pass)
{
    const U32 index = alphaPassIndex(pass);
    if (!mAlphaOpen || index >= ALPHA_PASSES || !(mAlphaPassMask & (1u << index)) ||
        !LLRHI::sScene->canDrawPass(LLRHI::SCENE_VIEW_ALPHA, pass))
    {
        return false;   // canDrawPass: refused a while ago (its program, a full table), tried again later
    }
    LLGLSLShader* const shader = LLGLSLShader::sCurBoundShaderPtr;
    if (!shader || shader != passProgram(pass))
    {
        return false;   // the pool bound another program (a debug mode): the loop draws them
    }
    LLRHI::SceneDrawContext ctx;
    ctx.draw = alphaMode() == LLRHI::SceneMode::On && LLRHI::sScene->drawing();     // research/218 vk-s2f: or auto's on
    ctx.scope = true;
    ctx.view_matrix = gGLModelView;
    ctx.projection = glm::value_ptr(gGL.getProjectionMatrix());
    ctx.fill = &fill_matrices;
    ctx.attribute_count = LLVertexBuffer::getAttributeLayout(shader->mAttributeMask, mLayout);
    ctx.attributes = mLayout;
    // The units the batch's textures go on (TexSetup): unit 0 for a plain or
    // fullbright batch (a list of one); 3a-4: a material's program's
    // channels for its diffuse, normal and specular maps. H5: a record's
    // list entry u on unit u (a plain, fullbright or glow record; the units
    // the program does not sample are left out by the backend). The glow
    // pass (vk-s3a6): as a plain batch, its priority's low bits 0xfff.
    if (alpha_material_pass(pass))
    {
        const U32 maps = passTextures(pass);
        const S32 channels[SCENE_TEX_USED] = { shader->getTextureChannel(LLShaderMgr::DIFFUSE_MAP),
                                               shader->getTextureChannel(LLShaderMgr::BUMP_MAP),
                                               shader->getTextureChannel(LLShaderMgr::SPECULAR_MAP) };
        for (U32 k = 0; k < SCENE_TEX_USED; ++k)
        {
            if (!(maps & (1u << k)))
            {
                continue;
            }
            if (channels[k] < 0 || channels[k] >= (S32)LLRHI::SceneDrawContext::UNITS)
            {
                return false;   // a unit the context cannot name: the loop draws them
            }
            ctx.unit_texture[channels[k]] = (S8)k;
        }
        ctx.materials = passMaterials(pass, ctx.material_count);
    }
    else
    {
        ctx.unit_texture[0] = (S8)SCENE_TEX_DIFFUSE;
        // Only the program's indexed textures' units (tex0.. on units 0..): the
        // units past them are its other samplers' (the plain program's flat
        // normal and white specular), which a list's entry must not take.
        const U32 channels = (U32)llclamp(shader->mFeatures.mIndexedTextureChannels, 0, (S32)LLRHI::SceneTextureList::MAX);
        for (U32 u = 0; u < channels && u < LLRHI::SceneDrawContext::UNITS; ++u)
        {
            ctx.unit_list[u] = (S8)u;
        }
        ctx.glow_priority = pass == ALPHA_PASS_GLOW;
    }
    mPassTextures.clear();
    for (auto t = std::lower_bound(mAlphaTextures.begin(), mAlphaTextures.end(), (U64)pass << 32);
         t != mAlphaTextures.end() && (U32)(*t >> 32) == pass; ++t)
    {
        mPassTextures.push_back((U32)*t);
    }
    ctx.textures = mPassTextures.data();
    ctx.texture_count = (U32)mPassTextures.size();
    ctx.group_priority = mAlphaPriority.data();
    ctx.group_priority_count = (U32)mAlphaPriority.size();
    ctx.place_value = (U8)SCENE_MAT_ALPHA_PLACE;
    ctx.once = true;
    if (mAlphaSorted)
    {
        if (!mAlphaCommands)
        {
            return false;
        }
        ctx.group_base = mAlphaBase.data();
        ctx.group_base_count = (U32)mAlphaBase.size();
        ctx.sorted_commands = mAlphaCommands;
    }
    const bool done = LLRHI::sScene->drawPass(LLRHI::SCENE_VIEW_ALPHA, pass, ctx);
    mAlphaPassDrawn[index] = ctx.draw && done;
    mAlphaAnyDrawn = mAlphaAnyDrawn || mAlphaPassDrawn[index];
    mAlphaDrawnPasses += mAlphaPassDrawn[index] ? 1 : 0;
    mAlphaFailed += ctx.draw && !done ? 1 : 0;
    return mAlphaPassDrawn[index];
}

// The program the scene drew the batch with this pass (its record's pass
// drawn, its group visible), null: the loop draws it.
const LLGLSLShader* LLSceneRecords::alphaDrawnByScene(const LLDrawInfo& info) const
{
    const auto it = mInfos.find(&info);
    if (it == mInfos.end() || !it->second.record || !it->second.alpha || !isAlphaPass(it->second.pass))
    {
        return nullptr;
    }
    const InfoState& s = it->second;
    if (!mAlphaPassDrawn[alphaPassIndex(s.pass)])
    {
        return nullptr;
    }
    const auto g = mAlphaGroups.find(s.group);
    return g != mAlphaGroups.end() && g->second.cut_serial == mAlphaSerial ? passProgram(s.pass) : nullptr;
}

// research/205 (vk-s3a6): the glow pass drew the batch's glow record (its group visible).
bool LLSceneRecords::glowDrawnByScene(const LLDrawInfo& info) const
{
    const auto it = mInfos.find(&info);
    if (it == mInfos.end() || !it->second.glow_record || !it->second.alpha)
    {
        return false;
    }
    const auto g = mAlphaGroups.find(it->second.group);
    return g != mAlphaGroups.end() && g->second.cut_serial == mAlphaSerial;
}

// research/205 3a-5: the group's spans from its PASS_ALPHA draw map as it
// is and the records its batches have (of an alpha pass, in this group).
// No spans (places 0) for a draw map longer than an OIT priority's places.
// vk-s3a6: and the glow spans of the glow records (the same rule, every
// glow record of the glow pass).
void LLSceneRecords::makeAlphaSpans(const LLSpatialGroup* group, std::vector<AlphaSpan>& spans, std::vector<U32>& records,
                                    U32& places, std::vector<AlphaSpan>* glow_spans, std::vector<U32>* glow_records) const
{
    spans.clear();
    records.clear();
    places = 0;
    if (glow_spans && glow_records)
    {
        glow_spans->clear();
        glow_records->clear();
    }
    const auto map = group->mDrawMap.find(LLRenderPass::PASS_ALPHA);
    if (map == group->mDrawMap.end() || map->second.empty() || map->second.size() > ALPHA_SPAN_PLACES)
    {
        return;
    }
    const LLSpatialGroup::drawmap_elem_t& draw_vec = map->second;
    const U32 count = (U32)draw_vec.size();
    records.assign(count, 0);
    static std::vector<U8> passes;  // main thread; kept for its capacity
    passes.assign(count, 0);
    const bool glow = glow_spans && glow_records;
    if (glow)
    {
        glow_records->assign(count, 0);
    }
    for (U32 k = 0; k < count; ++k)
    {
        const auto it = draw_vec[k].notNull() ? mInfos.find(draw_vec[k].get()) : mInfos.end();
        if (it == mInfos.end() || !it->second.record || !it->second.alpha || it->second.group != group || !isAlphaPass(it->second.pass))
        {
            continue;
        }
        records[k] = it->second.record;
        passes[k] = (U8)alphaPassIndex(it->second.pass);
        if (glow)
        {
            (*glow_records)[k] = it->second.glow_record;
        }
    }
    alpha_spans(records.data(), passes.data(), count, spans);
    if (glow)
    {
        passes.assign(count, (U8)ALPHA_GLOW_INDEX);
        alpha_spans(glow_records->data(), passes.data(), count, *glow_spans);
    }
    places = count;
}

// Verify (every VERIFY_EVERY frames, the sorted way's visible groups): the
// cached spans against the draw map as it is now. A difference means an
// event the records missed (the draw map changed without its clear): the
// spans are made again and the difference is counted (auto stops).
void LLSceneRecords::checkAlphaSpans(const LLSpatialGroup* group, GroupState& g)
{
    U32 places = 0;
    makeAlphaSpans(group, mSpanScratch, mSpanRecordScratch, places, &mGlowSpanScratch, &mGlowRecordScratch);
    ++mSpanChecks;
    auto same_spans = [](const std::vector<AlphaSpan>& x, const std::vector<AlphaSpan>& y)
    {
        bool same = x.size() == y.size();
        for (size_t i = 0; same && i < x.size(); ++i)
        {
            same = x[i].first == y[i].first && x[i].count == y[i].count && x[i].pass_index == y[i].pass_index;
        }
        return same;
    };
    const bool same = places == g.places && mSpanRecordScratch == g.place_records && same_spans(mSpanScratch, g.spans) &&
                      mGlowRecordScratch == g.place_glow_records && same_spans(mGlowSpanScratch, g.glow_spans);
    if (same)
    {
        return;
    }
    ++mSpanDiffers;
    noteDifference("alpha spans differ from the group's draw map (made again)", LLRHI::SCENE_VIEW_ALPHA, LLRenderPass::PASS_ALPHA, 1);
    g.spans.swap(mSpanScratch);
    g.place_records.swap(mSpanRecordScratch);
    g.glow_spans.swap(mGlowSpanScratch);
    g.place_glow_records.swap(mGlowRecordScratch);
    g.places = places;
}

LLSceneRecords::AlphaSpans LLSceneRecords::alphaSpans(const LLSpatialGroup* group) const
{
    AlphaSpans out;
    if (!mAlphaOpen || !mAlphaSorted || !mAlphaAnyDrawn)
    {
        return out;
    }
    const auto it = mAlphaGroups.find(group);
    if (it == mAlphaGroups.end() || it->second.cut_serial != mAlphaSerial || (it->second.spans.empty() && it->second.glow_spans.empty()))
    {
        return out;
    }
    const GroupState& g = it->second;
    const U32 bit = g.id - 1;
    if (bit >= mAlphaBase.size())
    {
        return out;
    }
    out.spans = g.spans.data();
    out.count = (U32)g.spans.size();
    out.records = g.place_records.data();
    out.base = mAlphaBase[bit];
    if (mAlphaPassDrawn[ALPHA_GLOW_INDEX] && g.place_glow_records.size() == g.places)     // vk-s3a6: the glow pass expanded
    {
        out.glow_spans = g.glow_spans.data();
        out.glow_count = (U32)g.glow_spans.size();
        out.glow_records = g.place_glow_records.data();
    }
    return out;
}

bool LLSceneRecords::drawAlphaSpan(const AlphaSpans& spans, const AlphaSpan& span)
{
    const U32 pass = alphaPassAt(span.pass_index);
    const bool glow = span.pass_index == ALPHA_GLOW_INDEX;     // research/205 (vk-s3a6): a glow span
    const U32* records = glow ? spans.glow_records : spans.records;
    U64& loop = glow ? mGlowSpansLoop : mSpansLoop;
    if (!mAlphaOpen || !mAlphaSorted || !alphaSpanReady(span) || LLGLSLShader::sCurBoundShaderPtr != passProgram(pass) || !records)
    {
        ++loop;
        return false;
    }
    if (!LLRHI::sScene->drawSorted(LLRHI::SCENE_VIEW_ALPHA, pass, spans.base + span.first, records + span.first, span.count))
    {
        ++loop;
        return false;
    }
    ++(glow ? mGlowSpansDrawn : mSpansDrawn);
    mSpanBatches += glow ? 0 : span.count;
    return true;
}

// --- verify ----------------------------------------------------------------------

// A batch of the render map (what the usual path draws): one of the scene's
// if its group is resident; and then its description and rows as they are now.
void LLSceneRecords::noteDrawn(U32 view, U32 pass, const LLDrawInfo& info)
{
    ++mMapBatches;
    const auto it = mInfos.find(&info);
    if (it == mInfos.end() || !it->second.record)
    {
        return;
    }
    InfoState& s = it->second;
    const GroupState* g = groupOf(s);
    if (!g || !residentState(*g))
    {
        return;     // not resident: the scene's view has none of its group's records
    }
    ++mMapScene;
    mOldDrawn.push_back(s.record);

    LLRHI::SceneRecordDesc desc;
    Refusal why = REFUSED_TABLE;
    if (s.pass != pass || !describe(info, s.group, s.pass, desc, why) || descHash(desc) != s.desc_hash)
    {
        ++mContentDiffers;
        noteDifference("batch whose record differs from it now", view, pass, 1);
    }
    if (s.model != info.mModelMatrix || s.texture != info.mTextureMatrix || matrixStale(mTransforms, s.model, s.model_slot) ||
        matrixStale(mUVTransforms, s.texture, s.texture_slot))
    {
        ++mStaleMatrices;
        noteDifference("batch whose transform row is not its matrix now", view, pass, 1);
    }
    // research/219 (9.3): a rigged record's row as the frame's table holds it against its skin's
    // palette now (PALETTE_CHECKS a verified frame at most).
    if (s.avatar.notNull() && mPaletteChecks < PALETTE_CHECKS)
    {
        U32 first = 0;
        const S32 same = s.avatar->rhiComparePaletteRow(s.skin, s.palette, first);
        if (same >= 0)
        {
            ++mPaletteChecks;
            ++mPaletteRowsCompared;
            if (!same)
            {
                ++mPaletteRowsDiffer;
                if (mPaletteLogged < 8)
                {
                    ++mPaletteLogged;
                    LL_WARNS("Scene") << "retained scene verify: rigged record " << s.record << " (avatar " << s.avatar->getID() << ", skin "
                                      << s.skin_hash << "): its palette row at " << s.palette << " differs from the palette from vec4 "
                                      << first << LL_ENDL;
                }
                noteDifference("rigged record whose palette row is not its skin's palette now", view, pass, 1);
            }
        }
    }
}

void LLSceneRecords::noteDifference(const char* what, U32 view, U32 pass, U32 count)
{
    ++mDifferences;
    if (mLogged < 8)
    {
        ++mLogged;
        LL_WARNS("Scene") << "retained scene verify: view " << view << " pass " << pass << ": " << count << " " << what << LL_ENDL;
    }
    // research/219: a rigged pass's difference stops the rigged passes only (their auto; the rest
    // of the scene goes on as it was).
    if (rigged_base(pass) != U32_MAX)
    {
        riggedChecked(true, what);
        return;
    }
    mFrameDiffered = true;
    LLRHI::Scene* scene = LLRHI::sScene;
    if (scene->drawing() && LLRHI::sceneMode() == LLRHI::SceneMode::Auto)
    {
        scene->setDrawing(false);
    }
    mCleanFrames = 0;
    // research/218 vk-s2f: the alpha view's and the probe faces' own autos (research/209 4.3 7: a
    // probe face's difference stops what draws the faces, not only the main view's auto).
    if (view == LLRHI::SCENE_VIEW_ALPHA)
    {
        partChecked(mAlphaAuto, LLRHI::sceneAlphaMode(), "LLRHI_SCENE_ALPHA", true, what);
    }
    else if (view == LLRHI::SCENE_VIEW_PROBE)
    {
        partChecked(mProbeAuto, LLRHI::sceneProbeMode(), "LLRHI_SCENE_PROBE", true, what);
    }
}

// research/218 vk-s2f: auto's step at a check of the part (several in a
// frame count once). Nothing while the part was asked for in another mode.
void LLSceneRecords::partChecked(LLRHI::SceneAuto& progress, LLRHI::SceneMode asked, const char* part, bool differ, const char* why)
{
    if (asked != LLRHI::SceneMode::Auto)
    {
        return;
    }
    const LLRHI::SceneMode before = progress.now;
    const LLRHI::SceneMode now = progress.step(mFrame, differ);
    if (now == before)
    {
        return;
    }
    if (now == LLRHI::SceneMode::On)
    {
        LL_INFOS("Scene") << part << "=auto: " << progress.clean_frames << " world frames without a difference, on" << LL_ENDL;
    }
    else
    {
        LL_WARNS("Scene") << part << "=auto: off for the run after " << progress.clean_frames << " clean world frames (" << why << ")"
                          << LL_ENDL;
    }
}

void LLSceneRecords::alphaChecked(bool differ)
{
    partChecked(mAlphaAuto, LLRHI::sceneAlphaMode(), "LLRHI_SCENE_ALPHA", differ, "llscenealpha's verify found a difference");
}

// The usual path's batches of the resident groups against the scene's choice:
// every chosen record is in the render map, and every batch of the render map
// was chosen or left out only by its bounds (the scene tests each batch's box,
// the usual path only the group's).
void LLSceneRecords::endPassVerify(U32 view, U32 pass)
{
    LLRHI::sScene->collect(view, pass, mDrawn, mBounded);
    ++mVerifiedPasses;
    std::sort(mOldDrawn.begin(), mOldDrawn.end());
    std::sort(mBounded.begin(), mBounded.end());
    U32 chosen_not_drawn = 0;
    U32 drawn_not_chosen = 0;
    for (U32 record : mDrawn)
    {
        if (!std::binary_search(mOldDrawn.begin(), mOldDrawn.end(), record))
        {
            ++chosen_not_drawn;
        }
    }
    std::sort(mDrawn.begin(), mDrawn.end());
    for (U32 record : mOldDrawn)
    {
        if (!std::binary_search(mDrawn.begin(), mDrawn.end(), record) && !std::binary_search(mBounded.begin(), mBounded.end(), record))
        {
            ++drawn_not_chosen;
        }
    }
    if (chosen_not_drawn)
    {
        noteDifference("records chosen that the usual path does not draw", view, pass, chosen_not_drawn);
    }
    if (drawn_not_chosen)
    {
        noteDifference("batches the usual path draws that were not chosen", view, pass, drawn_not_chosen);
    }
    // research/219: a rigged pass with rigged batches compared: a check of the rigged passes' world
    // frame (its differences, above and in noteDrawn, turned their auto off already).
    if (rigged_base(pass) != U32_MAX && (!mOldDrawn.empty() || !mDrawn.empty()))
    {
        ++mRiggedVerifiedPasses;
        mRiggedBatchesCompared += mOldDrawn.size();
        partChecked(mRiggedAuto, LLRHI::sceneRiggedMode(), "LLRHI_SCENE_RIGGED", false, "");
    }
    mOldDrawn.clear();
    // research/218 vk-s2f: a probe face's pass compared (its differences above and in noteDrawn
    // turned its auto off already): a check of the probe faces' world frame.
    if (view == LLRHI::SCENE_VIEW_PROBE)
    {
        partChecked(mProbeAuto, LLRHI::sceneProbeMode(), "LLRHI_SCENE_PROBE", false, "");
    }
}

std::string LLSceneRecords::takeReport(U32 frames)
{
    frames = llmax(frames, 1u);
    // The census (research/197 C5): the target batches alive now, by what became of them.
    U32 records = 0, waiting = 0;
    U32 rigged_records = 0;                     // research/219
    U32 alpha_batches = 0, alpha_records = 0;   // research/205 3a-3: counted apart
    U32 alpha_lists = 0, alpha_glows = 0;       // research/205 H5, glow (vk-s3a6)
    U32 refused[REFUSED_COUNT] = {};
    for (const auto& kv : mInfos)
    {
        const InfoState& s = kv.second;
        if (s.alpha)
        {
            ++alpha_batches;
            alpha_records += s.record ? 1 : 0;
            alpha_lists += s.record && s.list ? 1 : 0;
            alpha_glows += s.glow_record ? 1 : 0;
            continue;   // in the alpha line (its refusals in the refusals since the last line)
        }
        if (s.record)
        {
            ++records;
            rigged_records += s.palette != LLRHI::SCENE_NO_PALETTE ? 1 : 0;
        }
        else if (s.pending)
        {
            ++waiting;
        }
        else if (s.refused < REFUSED_COUNT)
        {
            ++refused[s.refused];
        }
    }
    U32 resident_groups = 0;
    U32 worn_groups = 0, worn_resident = 0;     // research/218 vk-s4b2
    for (const auto& kv : mGroups)
    {
        const bool resident = residentState(kv.second);
        resident_groups += resident ? 1 : 0;
        worn_groups += kv.second.worn ? 1 : 0;
        worn_resident += kv.second.worn && resident ? 1 : 0;
    }
    const size_t batches = mInfos.size() - alpha_batches;
    std::ostringstream out;
    out << "scene records (" << LLRHI::sceneModeName(LLRHI::sceneMode()) << (drawing() ? ", drawing" : ", choosing")
        << "): target batches " << batches << ", with records " << records << " ("
        << (batches ? (U32)((U64)records * 100 / batches) : 0) << "%), waiting " << waiting << "; groups " << mGroups.size()
        << " (resident " << resident_groups << ")";
    // research/218 vk-s4b2: the attachments' and animesh's groups (LLRHI_SCENE_ATTACHMENTS), cut in
    // every view, and those the scene could draw that a wearer (GrayWire, the skin mark) or verify kept whole.
    out << "; attachments and animesh (" << LLRHI::sceneModeName(wornMode()) << "): groups " << worn_groups << " (resident "
        << worn_resident << "), cut " << mWornCut / frames << " a frame, kept whole " << mWornKept / frames << " a frame";
    bool any = false;
    for (U32 i = 0; i < REFUSED_COUNT; ++i)
    {
        if (refused[i])
        {
            out << (any ? ", " : "; refused now: ") << REFUSAL_NAMES[i] << " " << refused[i];
            any = true;
        }
    }
    out << "; since the last line: made " << mCreated << ", registered " << mRegistered << ", dropped " << mDropped
        << ", described again " << mRedescribed << ", tried again " << mRetried << ", waiting " << mDeferred / frames << " a frame, texture transforms written "
        << mSweptUV << ", transforms written without an event " << mSweptTransforms;
    any = false;
    for (U32 i = 0; i < REFUSED_COUNT; ++i)
    {
        if (mRefusals[i])
        {
            out << (any ? ", " : ", refusals: ") << REFUSAL_NAMES[i] << " " << mRefusals[i];
            any = true;
            mRefusals[i] = 0;
        }
    }
    if (mVerifiedPasses)
    {
        out << "; verify " << mVerifiedPasses << " passes, render map batches " << mMapBatches << " of which the scene's "
            << mMapScene << " (" << (mMapBatches ? (U32)(mMapScene * 100 / mMapBatches) : 0) << "%), " << mDifferences
            << " differences (content " << mContentDiffers << ", transforms " << mStaleMatrices << "), clean " << mCleanFrames
            << " frames";
    }
    if (mScenePasses || mUncutResident)
    {
        // research/198 5.5: the main view's cut (groups the scene drew in its
        // passes) and the resident groups it could not cut (a record without
        // its place yet, a texture without an image or with options waiting).
        out << "; drawn by the scene: passes " << (F32)mScenePasses / frames << " a frame, groups cut " << mCutGroupCount / frames
            << " a frame, resident not cut " << mUncutResident / frames << " a frame";
    }
    // research/209: the probe faces and the mirrors (faces begun; groups cut,
    // passes drawn from the records, resident groups not cut; passes
    // compared in their turns).
    // research/218 vk-s2f: a part's auto as it is now.
    const auto part = [](LLRHI::SceneMode asked, const LLRHI::SceneAuto& progress)
    {
        std::string name = LLRHI::sceneModeName(asked);
        if (asked == LLRHI::SceneMode::Auto)
        {
            name += std::string(": ") + LLRHI::sceneModeName(progress.now) + ", clean " + std::to_string(progress.clean_frames);
        }
        return name;
    };
    if (LLRHI::sceneProbeMode() == LLRHI::SceneMode::Auto)
    {
        out << "; probe faces (" << part(LLRHI::sceneProbeMode(), mProbeAuto) << ")";
    }
    // research/219: the rigged records (LLRHI_SCENE_RIGGED): the groups with rigged records cut
    // (every view), those the skin mark kept whole in the main view, the skins not ready by reason
    // (LLVOAvatar::RHIScenePalette: off, not loaded, row changed, sway, not written).
    if (LLRHI::sceneRiggedMode() != LLRHI::SceneMode::Off)
    {
        const auto reasons = [](const U64* counts, U32 frames_seen)
        {
            std::ostringstream text;
            text << "off " << counts[LLVOAvatar::RHI_PALETTE_OFF] / frames_seen << ", not loaded "
                 << counts[LLVOAvatar::RHI_PALETTE_NOT_LOADED] / frames_seen << ", row changed "
                 << counts[LLVOAvatar::RHI_PALETTE_ROW_CHANGED] / frames_seen << ", sway " << counts[LLVOAvatar::RHI_PALETTE_SWAY] / frames_seen
                 << ", not written " << counts[LLVOAvatar::RHI_PALETTE_NOT_WRITTEN] / frames_seen;
            return text.str();
        };
        out << "; scene rigged (" << part(LLRHI::sceneRiggedMode(), mRiggedAuto) << "): records " << rigged_records
            << ", no palette row " << refused[REFUSED_NO_PALETTE] << "; groups cut " << mRiggedGroupsCut / frames
            << " a frame, kept for the mark " << mRiggedKeptForMark / frames << " a frame (predicted wrong " << mMarkPredictedWrong
            << "), skins not ready a frame (" << reasons(mSkinsNotReady, frames) << "; shadows " << reasons(mShadowSkinsNotReady, frames)
            << "), records described again for a row " << mSkinRedescribed << "; verify: rigged passes " << mRiggedVerifiedPasses
            << ", batches " << mRiggedBatchesCompared << ", differences " << mRiggedDifferences << ", palette rows compared "
            << mPaletteRowsCompared << " differ " << mPaletteRowsDiffer;
    }
    mRiggedGroupsCut = mRiggedKeptForMark = mMarkPredictedWrong = mSkinRedescribed = 0;
    memset(mSkinsNotReady, 0, sizeof(mSkinsNotReady));
    memset(mShadowSkinsNotReady, 0, sizeof(mShadowSkinsNotReady));
    mRiggedVerifiedPasses = mRiggedDifferences = 0;
    mRiggedBatchesCompared = mPaletteRowsCompared = mPaletteRowsDiffer = 0;
    static const char* const VIEW_NAMES[LLRHI::SCENE_VIEW_COUNT] = { "main", "probe", "mirror", "water" };
    for (U32 view = LLRHI::SCENE_VIEW_PROBE; view < LLRHI::SCENE_VIEW_COUNT; ++view)
    {
        if (mViewFaces[view])
        {
            out << "; " << VIEW_NAMES[view] << " faces " << mViewFaces[view] << ": groups cut " << mViewCutGroups[view] / frames
                << " a frame, passes by the scene " << mViewPasses[view] << ", resident not cut " << mViewUncut[view] / frames
                << " a frame, passes compared " << mViewCompared[view];
        }
        mViewFaces[view] = mViewPasses[view] = mViewCompared[view] = 0;
        mViewCutGroups[view] = mViewUncut[view] = 0;
    }
    // research/204 10 (vk-s4c2): the world pools' groups (terrain, trees, water) and their passes.
    if (LLRHI::sceneWorldMode() != LLRHI::SceneMode::Off)
    {
        U32 world_records = 0, world_slots = 0;
        for (const auto& kv : mWorldGroups)
        {
            world_records += kv.second.records;
        }
        for (const WorldSlot& slot : mWorldSlots)
        {
            world_slots += slot.pool ? 1 : 0;
        }
        out << "; scene world (" << part(LLRHI::sceneWorldMode(), mWorldAuto) << "): groups " << mWorldGroups.size() << ", records "
            << world_records << ", pools with a slot " << world_slots << "/" << WORLD_SLOTS << " (none left " << mWorldNoSlot
            << "); a frame: groups shown " << mWorldShown / frames << ", to describe again " << mWorldBroken / frames
            << " (waiting " << mWorldWaiting / frames << "), not drawable yet " << mWorldUnplaced / frames << ", passes drawn by the scene "
            << (F32)mWorldPasses / frames << ", faces left to it " << mWorldFacesDrawn / frames << "; since the last line: described "
            << mWorldDescribed << " (refused " << mWorldRefused << "), programs changed " << mWorldProgramChanged << "; verify passes "
            << mWorldVerified << ", differ " << mWorldDiffer;
        if (mWorldFailed)
        {
            out << ", WORLD PASSES NOT DRAWN " << mWorldFailed << " (the pools drew every face)";
        }
    }
    mWorldShown = mWorldBroken = mWorldWaiting = mWorldUnplaced = mWorldPasses = mWorldFacesDrawn = 0;
    mWorldDescribed = mWorldRefused = mWorldProgramChanged = mWorldNoSlot = mWorldFailed = 0;
    mWorldVerified = mWorldDiffer = 0;
    if (mDrawFailed)
    {
        out << "; DRAW FAILED " << mDrawFailed << " (batches drawn the usual way instead " << mRescued << ")";
    }
    if (alpha_batches || mAlphaPasses)
    {
        // research/205 3a-3: the alpha records, and the OIT "all" sessions' passes.
        out << "; alpha (" << part(LLRHI::sceneAlphaMode(), mAlphaAuto) << "): batches " << alpha_batches << ", with records "
            << alpha_records << ", groups " << mAlphaGroups.size() << "; sessions " << mAlphaPasses << ", groups visible "
            << mAlphaGroupsCut / frames << " a frame, not drawable now " << mAlphaUncut / frames << " a frame, passes drawn by the scene "
            << mAlphaDrawnPasses;
        if (mAlphaFailed)
        {
            out << ", ALPHA PASSES NOT DRAWN " << mAlphaFailed << " (the pool's loops drew them)";
        }
        out << "; records with texture lists " << alpha_lists << ", glow records " << alpha_glows;     // research/205 H5, glow (vk-s3a6)
        if (mSortedPasses || mSpanChecks)
        {
            // research/205 3a-5: the sorted way's passes (of the sessions above), spans and their checks.
            out << "; sorted " << mSortedPasses << ", spans drawn " << mSpansDrawn << " (" << mSpanBatches << " batches), spans the loop drew "
                << mSpansLoop << ", glow spans drawn " << mGlowSpansDrawn << " (the loop drew " << mGlowSpansLoop << "), span checks "
                << mSpanChecks;
            if (mSpanDiffers)
            {
                out << ", SPANS DIFFERED " << mSpanDiffers;
            }
        }
    }
    mAlphaPasses = mAlphaGroupsCut = mAlphaUncut = mAlphaDrawnPasses = mAlphaFailed = 0;
    mSortedPasses = mSpansDrawn = mSpanBatches = mSpansLoop = mSpanChecks = mSpanDiffers = 0;
    mGlowSpansDrawn = mGlowSpansLoop = 0;
    mCreated = mRegistered = mDropped = mDeferred = mRedescribed = mRetried = 0;
    mSweptTransforms = mSweptUV = 0;
    mVerifiedPasses = mDifferences = mContentDiffers = mStaleMatrices = mDrawFailed = 0;
    mMapBatches = mMapScene = 0;
    mCutGroupCount = mUncutResident = mRescued = mScenePasses = 0;
    mWornCut = mWornKept = 0;
    return out.str();
}

// --- research/204 10 (vk-s4c2): the world pools (terrain, trees, water) --------

U32 LLSceneRecords::worldPass(U32 slot)
{
    return WORLD_PASS_FIRST + slot;
}

bool LLSceneRecords::worldWanted(U32 view) const
{
    return view == LLRHI::SCENE_VIEW_MAIN && active() && worldMode() != LLRHI::SceneMode::Off;
}

LLSceneRecords::WorldSlot* LLSceneRecords::worldSlotOf(const LLFacePool* pool)
{
    for (WorldSlot& slot : mWorldSlots)
    {
        if (slot.pool == pool)
        {
            return &slot;
        }
    }
    return nullptr;
}

// A slot for a pool that has none: a free one (no pool, no records), else
// one whose pool went (its records are of nothing now: they hold no more,
// their groups are described again). -1: none.
S32 LLSceneRecords::worldTakeSlot(const LLFacePool* pool)
{
    S32 taken = -1;
    for (U32 s = 0; s < WORLD_SLOTS && taken < 0; ++s)
    {
        taken = !mWorldSlots[s].pool && !mWorldSlots[s].users ? (S32)s : -1;
    }
    for (U32 s = 0; s < WORLD_SLOTS && taken < 0; ++s)
    {
        taken = !mWorldSlots[s].pool ? (S32)s : -1;
    }
    if (taken < 0)
    {
        ++mWorldNoSlot;
        return -1;
    }
    WorldSlot& slot = mWorldSlots[taken];
    slot.pool = pool;
    slot.program = nullptr;
    slot.attributes = 0;
    slot.serial = slot.serial + 1 ? slot.serial + 1 : 1;
    slot.marked_frame = U32_MAX;
    slot.faces = nullptr;
    slot.face_count = 0;
    slot.shown_faces = 0;
    LLRHI::sScene->forgetPass(worldPass((U32)taken));     // another pool's: what was found of the last one's program goes
    return taken;
}

void LLSceneRecords::onWorldPoolGone(const LLFacePool* pool)
{
    for (WorldSlot& slot : mWorldSlots)
    {
        if (slot.pool == pool)
        {
            slot.pool = nullptr;
            slot.faces = nullptr;
            slot.face_count = 0;
            slot.marked_frame = U32_MAX;
        }
    }
}

bool LLSceneRecords::worldFaceNow(const LLFace* face, WorldFace& out)
{
    out = WorldFace();
    out.face = face;
    const LLFacePool* pool = face->getPool();
    if (!pool || !world_pool_type(pool->getType()))
    {
        return false;
    }
    const WorldSlot* slot = worldSlotOf(pool);
    if (!slot)
    {
        return false;
    }
    out.slot = (U8)(slot - mWorldSlots);
    out.slot_serial = slot->serial;
    LLVertexBuffer* vb = face->getVertexBuffer();
    if (!vb)
    {
        return true;    // draws nothing (a tree past its last LOD)
    }
    out.buffer = vb->rhiDrawNumber();
    if (pool->getType() == LLDrawPool::POOL_TREE)
    {
        // LLDrawPoolTree::renderDeferred: the face's whole buffer.
        out.first = 0;
        out.count = vb->getNumIndices();
    }
    else
    {
        // LLFace::renderIndexed (the terrain's drawLoop, the water's pushWaterPlanes).
        out.first = (U32)llmax(face->getIndicesStart(), 0);
        out.count = face->getIndicesCount();
    }
    if (pool->getType() != LLDrawPool::POOL_WATER)
    {
        // drawLoop and the trees' loop apply the region's render matrix; the water none.
        const LLDrawable* drawable = face->getDrawable();
        LLViewerRegion* region = drawable ? drawable->getRegion() : nullptr;
        out.model = region ? &region->mRenderMatrix : nullptr;
        if (!out.model && out.count)
        {
            return false;
        }
    }
    return true;
}

void LLSceneRecords::dropWorld(WorldGroup& g)
{
    for (WorldFace& f : g.faces)
    {
        if (!f.record)
        {
            continue;
        }
        LLRHI::sScene->removeRecord(f.record);
        releaseMatrix(mTransforms, f.model, f.model_slot);
        WorldSlot& slot = mWorldSlots[f.slot];
        slot.users -= slot.users ? 1 : 0;
    }
    g.faces.clear();
    g.records = 0;
    g.models.clear();
    g.cursor = 0;
}

// The group's records again, from the faces its pools' lists hold this view
// (what stateSort put there: a hidden selection, a face without a pool, or
// one without geometry is not among them, nor is it among the records). All
// or nothing: a face that cannot have its record leaves the group without any.
void LLSceneRecords::describeWorld(WorldGroup& g)
{
    const bool described_before = g.described_frame != 0;
    g.retry_gap = world_next_gap(g.retry_gap, mFrame, g.described_frame, described_before);
    g.retry_frame = mFrame + g.retry_gap;
    g.described_frame = mFrame ? mFrame : 1;
    dropWorld(g);
    mWorldBudget += (U32)g.walked.size();
    ++mWorldDescribed;
    if (!g.id)
    {
        g.id = LLRHI::sScene->addGroup();
        if (!g.id)
        {
            ++mWorldRefused;
            return;
        }
        mGroupSpan = llmax(mGroupSpan, g.id);
    }
    bool ok = true;
    g.faces.reserve(g.walked.size());
    for (const LLFace* face : g.walked)
    {
        WorldFace f;
        if (!worldFaceNow(face, f))
        {
            ok = false;
            break;
        }
        if (f.count)
        {
            const WorldSlot& slot = mWorldSlots[f.slot];
            LLRHI::SceneRecordDesc desc;
            desc.group = g.id;
            desc.pass = worldPass(f.slot);
            desc.vertex_format = face->getVertexBuffer()->getTypeMask();
            desc.range.buffer = f.buffer;
            desc.range.first_index = f.first;
            desc.range.index_count = f.count;
            desc.range.index32 = true;
            desc.range.attributes = slot.attributes;
            desc.loose = true;      // the usual path draws every face of the group's drawables (no box test of its own)
            // No number yet, or the pool's program not seen yet: tried again later.
            if (!f.buffer || !slot.attributes || !acquireMatrix(mTransforms, f.model, desc.transform))
            {
                ok = false;
                break;
            }
            f.model_slot = desc.transform;
            f.record = LLRHI::sScene->addRecord(desc);
            if (!f.record)
            {
                releaseMatrix(mTransforms, f.model, f.model_slot);
                ok = false;
                break;
            }
            ++mWorldSlots[f.slot].users;
            ++g.records;
            if (f.model_slot &&
                std::find(g.models.begin(), g.models.end(), std::make_pair(f.model, f.model_slot)) == g.models.end())
            {
                g.models.emplace_back(f.model, f.model_slot);
            }
        }
        g.faces.push_back(f);
    }
    if (!ok)
    {
        ++mWorldRefused;
        dropWorld(g);
    }
}

// postSort, the main view: the pool's face list as it is drawn (stateSort
// made it; nothing changes it before the pool's pass entry).
void LLSceneRecords::markWorldPool(U32 view, LLFacePool* pool)
{
    if (!pool || !worldWanted(view) || !world_pool_type(pool->getType()))
    {
        return;
    }
    const LLRHI::CensusTimer census(LLRHI::CENSUS_SCENE_VIEWS);  // research/208 S0
    if (WorldSlot* slot = worldSlotOf(pool))
    {
        slot->marked_frame = mFrame;
        slot->marked_view = mWorldSerial;
        slot->faces = pool->mDrawFace.data();
        slot->face_count = pool->mDrawFace.size();
        slot->shown_faces = 0;
    }
    const LLSpatialGroup* last = nullptr;
    WorldGroup* g = nullptr;
    WorldFace now;
    for (LLFace* face : pool->mDrawFace)
    {
        const LLDrawable* drawable = face ? face->getDrawable() : nullptr;
        LLSpatialGroup* group = drawable ? drawable->getSpatialGroup() : nullptr;
        if (!group)
        {
            continue;   // no group: the scene never shows it (the loop draws it)
        }
        if (group != last)
        {
            last = group;
            g = nullptr;
            const LLSpatialPartition* part = group->getSpatialPartition();
            if (!part || !world_partition(part->mPartitionType) || group->isDead())
            {
                continue;
            }
            g = &mWorldGroups[group];
            if (g->touched != mWorldSerial)
            {
                g->touched = mWorldSerial;
                g->seen = 0;
                g->broken = false;
                g->cursor = 0;
                g->walked.clear();
                mWorldTouched.push_back(group);
            }
        }
        if (!g)
        {
            continue;
        }
        g->walked.push_back(face);
        if (g->broken)
        {
            continue;
        }
        // Its record: where the last one ended, else looked for (a tree group's faces come in several pools' lists).
        U32 at = g->cursor < g->faces.size() && g->faces[g->cursor].face == face ? g->cursor : U32_MAX;
        for (U32 i = 0; at == U32_MAX && i < g->faces.size(); ++i)
        {
            at = g->faces[i].face == face ? i : U32_MAX;
        }
        if (at == U32_MAX)
        {
            g->broken = true;   // not one of its records' faces
            continue;
        }
        g->cursor = at + 1;
        WorldFace& record = g->faces[at];
        if (record.seen == mWorldSerial || !worldFaceNow(face, now))
        {
            g->broken = true;   // twice in the lists, or its pool no world pool's (or slot's) now
            continue;
        }
        record.seen = mWorldSerial;
        const WorldFaceKey was{ record.slot, record.slot_serial, record.buffer, record.first, record.count, record.model };
        const WorldFaceKey is{ now.slot, now.slot_serial, now.buffer, now.first, now.count, now.model };
        if (!world_face_holds(is, was))
        {
            g->broken = true;
            continue;
        }
        ++g->seen;
    }
}

// endView, the main view, before its bits go to the scene: each group whose
// faces were walked is shown, or described again, or left to wait.
void LLSceneRecords::decideWorld(U32 view)
{
    std::vector<U32>& bits = mViews[view].bits;
    for (const LLSpatialGroup* group : mWorldTouched)
    {
        const auto it = mWorldGroups.find(group);
        if (it == mWorldGroups.end())
        {
            continue;
        }
        WorldGroup& g = it->second;
        bool rows_hold = true;
        for (const auto& model : g.models)
        {
            const auto row = mTransforms.live.find(model.first);
            rows_hold = rows_hold && row != mTransforms.live.end() && row->second.slot == model.second;
        }
        const bool in_bits = g.id && (g.id - 1) / 32 < bits.size();
        const bool faces_hold = !g.broken && !g.faces.empty() && g.seen == (U32)g.faces.size();
        // The backend's own test last (it takes the tables back from the render thread).
        const bool drawable = faces_hold && g.records && rows_hold && in_bits && LLRHI::sScene->groupDrawable(g.id);
        if (world_group_shows(g.broken, g.seen, (U32)g.faces.size(), g.records, drawable, rows_hold, in_bits))
        {
            bits[(g.id - 1) / 32] |= 1u << ((g.id - 1) % 32);
            for (const auto& model : g.models)
            {
                const U32 slot = model.second;
                if (slot >= mTransformListed.size())
                {
                    mTransformListed.resize((size_t)slot + 1, 0);
                }
                if (mTransformListed[slot] != mViewSerial)
                {
                    mTransformListed[slot] = mViewSerial;
                    mViewTransforms[view].push_back(slot);
                }
            }
            g.shown = mWorldSerial;
            for (const WorldFace& f : g.faces)
            {
                ++mWorldSlots[f.slot].shown_faces;
            }
            ++mWorldShown;
            continue;
        }
        // Broken, never described, or its rows not its matrices' any more: described again
        // (when its wait is over and the frame has room); else it waits for its places.
        const bool needs = g.broken || g.faces.empty() || g.seen != (U32)g.faces.size() || !rows_hold;
        mWorldBroken += needs ? 1 : 0;
        if (world_group_describes(needs, mFrame, g.retry_frame, mWorldBudget, BUDGET, (U32)g.walked.size()))
        {
            describeWorld(g);
        }
        else if (!needs)
        {
            ++mWorldUnplaced;
        }
        else
        {
            ++mWorldWaiting;
        }
    }
    mWorldTouched.clear();
}

bool LLSceneRecords::worldGroupDrawn(WorldDraw& draw, const LLFace* face) const
{
    const LLDrawable* drawable = face ? face->getDrawable() : nullptr;
    const LLSpatialGroup* group = drawable ? drawable->getSpatialGroup() : nullptr;
    if (group != draw.group)
    {
        draw.group = group;
        const auto it = group ? mWorldGroups.find(group) : mWorldGroups.end();
        draw.group_drawn = it != mWorldGroups.end() && world_face_left_out(true, it->second.shown, draw.serial);
    }
    return draw.group_drawn;
}

LLSceneRecords::WorldDraw LLSceneRecords::drawWorldPool(LLFacePool* pool)
{
    WorldDraw out;
    if (!pool || LLRHI::sceneWorldMode() == LLRHI::SceneMode::Off || !active() || currentView() != LLRHI::SCENE_VIEW_MAIN ||
        !world_pool_type(pool->getType()))
    {
        return out;
    }
    const LLRHI::CensusTimer census(LLRHI::CENSUS_SCENE_PASS_ENTRY);     // research/208 S0
    LLGLSLShader* const shader = LLGLSLShader::sCurBoundShaderPtr;
    if (!shader)
    {
        return out;
    }
    WorldSlot* slot = worldSlotOf(pool);
    if (!slot)
    {
        const S32 taken = worldTakeSlot(pool);
        if (taken < 0)
        {
            return out;
        }
        slot = &mWorldSlots[taken];
    }
    const U32 index = (U32)(slot - mWorldSlots);
    const U32 pass = worldPass(index);
    // The records were made for what the pool's program read last time: another
    // program starts the pass's refusals over, other attributes its records.
    const U32 attributes = attribute_locations(shader->mAttributeMask);
    if (shader != slot->program)
    {
        if (slot->program)
        {
            LLRHI::sScene->forgetPass(pass);
            ++mWorldProgramChanged;
        }
        slot->program = shader;
    }
    if (attributes != slot->attributes)
    {
        slot->attributes = attributes;
        slot->serial = slot->serial + 1 ? slot->serial + 1 : 1;
        return out;     // its records hold no more (described again at the next view)
    }
    // The pool's list must be the one postSort walked for this view.
    if (slot->marked_frame != mFrame || slot->marked_view != mWorldSerial || slot->faces != pool->mDrawFace.data() ||
        slot->face_count != pool->mDrawFace.size())
    {
        return out;
    }
    const WorldEntry entry = world_entry(worldMode(), LLRHI::sScene->drawing(), slot->shown_faces);
    if (entry == WORLD_ENTRY_NONE)
    {
        return out;
    }
    if (entry == WORLD_ENTRY_CHOOSE && mFrame % VERIFY_EVERY == 0 && slot->verified_frame != mFrame)
    {
        slot->verified_frame = mFrame;
        worldVerify(index, pool);
    }
    LLRHI::SceneDrawContext ctx;
    ctx.draw = entry == WORLD_ENTRY_DRAW;
    ctx.scope = true;
    // What the loop's draws start from: the terrain's and the trees' gGLModelView times the
    // region's matrix (applyModelMatrix), the water's the model view as it is (no model matrix).
    const bool water = pool->getType() == LLDrawPool::POOL_WATER;
    ctx.view_matrix = water ? glm::value_ptr(gGL.getModelviewMatrix()) : gGLModelView;
    ctx.projection = glm::value_ptr(gGL.getProjectionMatrix());
    ctx.fill = &fill_matrices;
    ctx.attribute_count = LLVertexBuffer::getAttributeLayout(shader->mAttributeMask, mLayout);
    ctx.attributes = mLayout;
    // Every unit keeps what the pool bound (unit_texture -1); the records have no material uniforms.
    ctx.transform_slots = mViewTransforms[LLRHI::SCENE_VIEW_MAIN].data();
    ctx.transform_slot_count = (U32)mViewTransforms[LLRHI::SCENE_VIEW_MAIN].size();
    ctx.uv_slots = mViewUVs[LLRHI::SCENE_VIEW_MAIN].data();
    ctx.uv_slot_count = (U32)mViewUVs[LLRHI::SCENE_VIEW_MAIN].size();
    // research/204 10 (design review 1): verify's and auto's drawPass answers true for a choice
    // only; nothing is left out of the loop then.
    const bool drawn = LLRHI::sScene->drawPass(LLRHI::SCENE_VIEW_MAIN, pass, ctx);
    if (entry != WORLD_ENTRY_DRAW)
    {
        return out;     // chosen only: the loop draws every face
    }
    if (!drawn)
    {
        ++mWorldFailed;
        LL_WARNS_ONCE("Scene") << "retained scene: the scene could not draw the world pass " << pass
                               << "; its faces are drawn the usual way" << LL_ENDL;
        worldChecked(true, "a world pass the scene could not draw");
        return out;
    }
    ++mWorldPasses;
    mWorldFacesDrawn += slot->shown_faces;
    out.drew = true;
    out.serial = mWorldSerial;
    return out;
}

// Verify: the records of the pool's faces of shown groups against the scene's
// choice of the pass (the same records; loose: none left out by bounds).
void LLSceneRecords::worldVerify(U32 slot, LLFacePool* pool)
{
    mWorldExpected.clear();
    const LLSpatialGroup* last = nullptr;
    const WorldGroup* g = nullptr;
    for (const LLFace* face : pool->mDrawFace)
    {
        const LLDrawable* drawable = face ? face->getDrawable() : nullptr;
        const LLSpatialGroup* group = drawable ? drawable->getSpatialGroup() : nullptr;
        if (group != last)
        {
            last = group;
            const auto it = group ? mWorldGroups.find(group) : mWorldGroups.end();
            g = it != mWorldGroups.end() && it->second.shown == mWorldSerial ? &it->second : nullptr;
        }
        if (!g)
        {
            continue;
        }
        for (const WorldFace& f : g->faces)
        {
            if (f.face == face && f.record)
            {
                mWorldExpected.push_back(f.record);
                break;
            }
        }
    }
    LLRHI::sScene->collect(LLRHI::SCENE_VIEW_MAIN, worldPass(slot), mDrawn, mBounded);
    mDrawn.insert(mDrawn.end(), mBounded.begin(), mBounded.end());
    std::sort(mDrawn.begin(), mDrawn.end());
    std::sort(mWorldExpected.begin(), mWorldExpected.end());
    ++mWorldVerified;
    if (mDrawn == mWorldExpected)
    {
        worldChecked(false, "");
        return;
    }
    ++mWorldDiffer;
    if (mWorldLogged < 8)
    {
        ++mWorldLogged;
        LL_WARNS("Scene") << "retained scene world verify " << mWorldLogged << "/8: pass " << worldPass(slot)
                          << ": the pool's faces' records " << mWorldExpected.size() << ", the scene's choice " << mDrawn.size() << LL_ENDL;
    }
    worldChecked(true, "the world verify found a difference");
}

void LLSceneRecords::worldChecked(bool differ, const char* why)
{
    partChecked(mWorldAuto, LLRHI::sceneWorldMode(), "LLRHI_SCENE_WORLD", differ, why);
}
