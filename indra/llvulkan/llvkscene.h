/**
 * @file llvkscene.h
 * @brief The retained scene on the GPU (research/197 stage 2): LLRHI::Scene for Vulkan.
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

#ifndef LL_LLVKSCENE_H
#define LL_LLVKSCENE_H

#include "stdtypes.h"
#include "llrhi.h"      // DirectDraw::Attribute
#include "llrhiscene.h" // SceneMaterialUniform, SceneMaterialDesc
#include "llvkrender.h" // research/208 S2: LLVKRender::Use

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

// LLRHI::sScene on Vulkan (research/197 3.3).
//
// The tables (LLVKSceneTable: records, transforms, texture transforms,
// materials, the pages' buckets) live in device-local buffers made at first
// use, as large as the table's limits. The rows written since the last view's
// choice are copied into them in the frame's upload commands, which run before
// every draw of the frame, followed by the view's choice: scene_cull.comp's
// three phases, between barriers, into this frame slot's output buffer (a
// count per bucket, then the commands).
//
// Drawing (research/198 5.5): the pass's program is one of the viewer's that
// read each draw's words from a row of the frame's palette table (research/46
// G4b, draw rows). Per drawn pass: the textures the records name are resolved
// to bindless slots (the table of slots by texture number, on the GPU), the
// view's matrix rows (one per transform row and per texture transform row,
// made with the viewer's arithmetic) go into the frame's matrix table, and a
// second dispatch (the expand) writes, for each chosen record, its row of
// words (the pass's words with its texture's slot and its matrix row in
// place) and its command naming that row as its first instance. Then per
// bucket of the pass the Binder binds the program's pipeline for the layout
// and the bucket's pool chunk, and one vkCmdDrawIndexedIndirectCount draws
// the bucket's commands. A record's place in the pool is its vertex buffer
// number's place (LLVKAttribPool::numberPlace), written into its row when the
// place is made, moves or goes (the pool tells: setNumberListener).
// research/198 5.6 (stage 2d): when the pool sets material uniforms per batch
// (the masks' cutoff, the legacy materials' colors), each record's row is
// followed by its own material row in the program's layout: the program's
// material as it is now, with the record's values in the floats the Binder
// found for the uniforms' names (per program, so another pass's or the alpha
// pool's programs map the same values their own way).
//
// Verify (LLRHI_SCENE=verify, or auto until the viewer turns drawing on):
// drawPass only chooses. Every 8th frame the main view's output is also copied
// to the host, and when the frame slot comes round again (its fence passed) it
// is compared with the CPU's run of the same functions over the same rows
// (LLVKSceneTable::emulate): counts, commands and order per bucket, leaving out
// records within the float slack of a plane.
//
// Render thread only; every entry takes LLVKTracker::lock().
namespace LLVKScene
{
    // Installs LLRHI::sScene unless LLRHI_SCENE is off (nothing is made until it is used).
    void install();
    // Destroys what the scene made on the device (after the device is idle,
    // before LLVKDevice::shutdown). The scene is unusable afterwards.
    void shutdown();

    // research/208 S3: a pass's indirect draws whose buckets the render
    // thread adds (the pass's choice and expand are its own), from values the
    // main thread made at the pass's entry (Binder::beginThreadDraws): the
    // pipeline, sets, dynamic state and words of the pass's head, and for
    // every chunk of the attribute pool its index buffer and the layout's
    // streams as they were then. Only its taker calls it, then release().
    class ThreadDraws
    {
    public:
        virtual ~ThreadDraws() = default;
        // A bucket's draw; false: its chunk had no index buffer or stream at the entry (nothing added).
        virtual bool addBucket(U32 chunk, VkBuffer commands, VkDeviceSize offset, VkBuffer counts, VkDeviceSize count_offset,
                               U32 max_count, U32 stride) = 0;
        // The binds and the buckets' draws, into the frame's command buffer (through llvkdispatch.h).
        virtual void recordDraws() = 0;
        // research/208 13.3 (vk-s4a4): the same binds and draws into a
        // secondary command buffer this thread begins and ends (inheriting the
        // rendering scope the entry opened); null: none could be begun or
        // ended. Its commands are recorded directly (not through the
        // recording worker): what it binds is all it uses.
        virtual VkCommandBuffer recordSecondary(U32 frame_slot, U64 frame_begins) = 0;
        // The commands the last recordDraws or recordSecondary recorded.
        virtual U32 recordedCommands() const = 0;
        // FNV-1a of the values made at the entry (verify).
        virtual U64 entryDigest() const = 0;
        // Free for the binder again.
        virtual void release() = 0;
    };

    // What drawPass needs from the translation layer (llvkgl_scene.cpp,
    // research/198 5.5). Called under LLVKTracker::lock(), in this order:
    // beginPass, textureSlot (any number of times), texturesDone, bindChunk
    // (once per bucket), endPass (also after a failure).
    class Binder
    {
    public:
        virtual ~Binder() = default;

        // The words a draw of the bound program pushes when it names no
        // texture of its own (each sampler as its unit is bound, the
        // program's matrix, material and constants rows), the rows word left
        // out: a record's row is these with its own texture slots and matrix
        // row in place.
        struct Words
        {
            static constexpr U32 MAX = 32;
            static constexpr U32 MATERIAL_VEC4S = 16;   // the largest material row a record gets its own copy of
            U32 words[MAX] = {};
            U32 count = 0;              // words in a row (the program's draw_row_words)
            S32 units[MAX] = {};        // per word: the texture unit of its 2D sampler (-1: not a sampler's)
            U32 matrix_word = U32_MAX;  // the matrix row's word (U32_MAX: none)
            // research/198 5.6: each record's own material row (the pass's
            // program's layout, research/46 G4a), when the viewer names
            // material uniforms the program reads from its material row:
            // the row's word (U32_MAX: none, every record reads the pass's
            // row), its vec4s, its floats as the program holds them now, and
            // per material value (LLRHI::SceneMaterialDesc::values) its float
            // in the row (-1: not one of the program's).
            U32 material_word = U32_MAX;
            U32 material_vec4s = 0;
            F32 material[MATERIAL_VEC4S * 4] = {};
            S16 material_at[LLRHI::SceneMaterialDesc::VALUES] = {};
            // research/205 3a-3: in an OIT session, the word of the row the
            // program's OIT variant reads the draw's priority from (the
            // rows word's place, `count`: one past the words), U32_MAX: none.
            U32 priority_word = U32_MAX;
            // research/219: the word the program reads its joints' palette
            // row from (llrhi_slots.palette: the row's first vec4 in the
            // palette table, 3 vec4s a joint), U32_MAX: it reads none. A
            // rigged record's row names its own resident palette row there.
            U32 palette_word = U32_MAX;
        };
        // The pass's program and target for draws of the vertex layout, and
        // (materials) the material uniforms a record's draw sets. False: it
        // cannot be drawn from rows now (why); permanent: never (the program
        // reads no rows, or keeps such a uniform outside its material row),
        // else maybe in a later frame.
        virtual bool beginPass(const LLRHI::DirectDraw::Attribute* attributes, U32 attribute_count,
                               const LLRHI::SceneMaterialUniform* materials, U32 material_count, Words& out, const char*& why,
                               bool& permanent) = 0;
        // The bindless slot a draw naming texture number `ref` samples, as
        // the usual way resolves it (WHITE_SLOT when it cannot be sampled: no
        // image yet, the table full). An image not in shader read layout is
        // moved there before the pass's draws (bindChunk).
        virtual U32 textureSlot(U32 ref) = 0;
        virtual void texturesDone() = 0;
        // Binds the pass's pipeline for the layout, its sets, the words with
        // the rows word saying "read the rows", and the pool chunk's vertex
        // streams and 32-bit index buffer. The command buffer to draw into
        // (null: nothing bound; the words changed since beginPass, or no pipeline).
        // In an OIT session (research/205 3a-3) the program's OIT variant
        // draws, its nodes taking each draw's priority from its row
        // (3a-5: also a hybrid session's world draws, as split draws).
        virtual VkCommandBuffer bindChunk(U32 chunk) = 0;
        // Whether bindChunk can find the chunk's index buffer and the
        // layout's streams (asked for every bucket before a pass that must
        // not draw twice draws any).
        virtual bool chunkBindable(U32 chunk) = 0;
        // research/205 3a-5: the textures textureSlot resolved moved into
        // shader read layout now (bindChunk does it for the pass's draws; a
        // sorted pass is expanded first and its spans drawn later, each
        // under a beginPass of its own).
        virtual void settleTextures() = 0;
        virtual void endPass() = 0;
        // research/208 S1 (stage 4a): whether the open pass binds from its
        // head, the values beginPass read of the main thread's GL context
        // (LLRHI_FRAME_SNAPSHOT on, or auto once its checks agreed). On that
        // way what bindChunk could fail at is made by admit, before any of
        // the pass is drawn (its pipeline and descriptor set; the caller asks
        // chunkBindable for every chunk first): false, nothing is drawn and
        // the caller draws the pass the usual way at its entry. bindChunk
        // failing after an admit is counted (a render thread could not hand
        // that back to the main thread).
        virtual bool headWay() const { return false; }
        virtual bool admit() { return true; }
        // research/208 S2: the pass's indirect draws as values, for the
        // render thread (LLVKRender). After admit on the head's way:
        // beginDraws does the main thread's half of a chunk's bind once for
        // the pass (the layouts the head's textures and target take, the
        // rendering begun: bindHead's first half), addDraw resolves a bucket's
        // chunk (its index buffer and the layout's streams, as bindHead) and
        // adds its indirect draw, endDraws records them: here (Use::Verify,
        // and the same values handed to the thread to compare) or by the
        // render thread into a stream spliced here (Use::Thread; true). What
        // the draws bind is not known to the GL translation afterwards (the
        // draws after them bind again). False from beginDraws (not the head's
        // way, an OIT session's pass, no free job): the caller binds each
        // chunk with bindChunk. False from addDraw (the chunk cannot be bound):
        // nothing is recorded; the caller calls abortDraws and refuses the pass.
        virtual bool beginDraws(LLVKRender::Use /*use*/, U32 /*pass*/) { return false; }
        virtual bool addDraw(U32 /*chunk*/, VkBuffer /*commands*/, VkDeviceSize /*offset*/, VkBuffer /*counts*/, VkDeviceSize /*count_offset*/,
                             U32 /*max_count*/, U32 /*stride*/)
        {
            return false;
        }
        virtual bool endDraws() { return false; }
        virtual void abortDraws() {}
        // research/208 S3: after admit on the head's way (not an OIT
        // session's pass), the main thread's half of the pass's binds once
        // (beginDraws' layouts and rendering) and the values the render
        // thread draws its buckets with (ThreadDraws). The draws after it bind
        // again (as after endDraws). Null: not now (the caller draws the pass
        // the usual way at its entry; at most the layouts were moved, which
        // that way's binds find done).
        // research/208 13.3 (vk-s4a4): secondary, the scope is opened for
        // secondary command buffers only (the caller executes the thread's
        // there, then ends the scope).
        virtual ThreadDraws* beginThreadDraws(U32 /*pass*/, bool /*secondary*/) { return nullptr; }
        // The head's counters since the last call, for the scene's VKPerf line (empty: none).
        virtual std::string takeReport(U32 /*frames*/) { return std::string(); }
    };
    void setBinder(Binder* binder);

    // research/206 (stage 5): what drawShadow made ready for the next native
    // shadow pass (llvknative.cpp ShadowRenderer): per bucket one
    // vkCmdDrawIndexedIndirectCount over the view's choice, depth only, with
    // the bucket's pool chunk's positions and 32-bit indices bound, a vertex
    // shader that reads each draw's record (its first instance is the
    // record's slot) and the record's transform from the tables by address.
    // research/206 5c: a bucket of a mask pass (mode, ShadowPacket::Mode not
    // 0) also binds the chunk's texture coordinates and colors, and is drawn
    // with the renderer's mask shaders: their vertex shader reads the
    // record's texture transform and material (its diffuse texture's number
    // and cutoff) and the texture's slot from the scene's slot table.
    struct ShadowDraws
    {
        struct Bucket
        {
            VkBuffer positions = VK_NULL_HANDLE;    // the chunk's position stream (location 0, stride 16)
            VkBuffer indices = VK_NULL_HANDLE;      // the chunk's 32-bit indices
            VkDeviceSize commands = 0;              // the bucket's first command in buffer
            VkDeviceSize count = 0;                 // its count in buffer
            U32 capacity = 0;
            U8 mode = 0;                            // 5c: 0 depth only, else the mask's mode
            VkBuffer texcoords = VK_NULL_HANDLE;    // 5c, a mask's: texcoord0 (location 2, stride 8)
            VkBuffer colors = VK_NULL_HANDLE;       // 5c, a mask's: diffuse_color (location 6, stride 4); null: white
            // research/219: a rigged pass's bucket (SCENE_SHADOW_SKINNED):
            // drawn with the renderer's skinned shaders, which read each
            // record's palette row (its flags) from the palette table, and
            // the chunk's weights bound after the other streams.
            bool skinned = false;
            VkBuffer weights = VK_NULL_HANDLE;      // weight4 (LLVertexBuffer::TYPE_WEIGHT4, location 10, stride 16)
        };
        bool any = false;
        bool masks = false;                         // 5c: a bucket has a mask mode
        bool skins = false;                         // research/219: a rigged bucket without a mask mode
        bool skin_masks = false;                    // research/219: a rigged bucket with a mask mode
        VkBuffer buffer = VK_NULL_HANDLE;           // the choice's output (counts and commands)
        VkDeviceAddress records = 0;                // the record table
        VkDeviceAddress transforms = 0;             // the transform table
        VkDeviceAddress uv_transforms = 0;          // 5c: the texture transform table
        VkDeviceAddress materials = 0;              // 5c: the material table
        VkDeviceAddress texture_slots = 0;          // 5c: the slot per texture number
        U32 texture_slot_count = 0;                 // 5c: numbers the slot table holds
        U32 white_slot = 0;                         // 5c: the slot of a number past them (or 0)
        F32 anim_time[2] = {};                      // 5c: LLRHI::sTexAnimTime as two floats (high, low)
        F32 view_proj[16] = {};
        std::vector<Bucket> buckets;
        std::vector<U32> sampled;                   // 5c: the masks' textures' GL names (readied for sampling)
    };
    // The draws made ready since the last call (cleared: a pass takes them
    // once, drawn or not). False: none (out.any false).
    bool takeShadowDraws(ShadowDraws& out);
    // The shadow renderer drew (or could not draw) what it took: counts for
    // the perf line.
    void shadowDrawn(bool drawn, U32 buckets);
    // research/206 5c: the shadow renderer has its mask shaders (once it is
    // made): LLRHI::Scene::shadowMasks.
    void setShadowMasks(bool ready);
    // research/219: the shadow renderer has its skinned shaders and pipelines
    // (LLRHI::Scene::shadowSkins).
    void setShadowSkins(bool ready);

    // Counters since the last call, for the periodic perf log (empty when unused).
    std::string takeStats(U32 frames);

    // research/230 R2: the records ray tracing builds its acceleration
    // structures from: placed in the attribute pool with their position
    // stream, not rigged, in one of the passes in the mask (a bit per pass,
    // SCENE_MAX_PASSES). Their place and transform as they are now.
    struct RayRecord
    {
        U32 id = 0;             // the viewer's record id
        U32 number = 0;         // its vertex buffer's number
        U32 chunk = 0;          // the pool chunk the place is in
        U32 vertex_base = 0;    // the place's first vertex in the chunk's streams
        U32 vertices = 0;       // the place's vertices (the indices lie in them)
        U32 first_index = 0;    // in the chunk's index buffer
        U32 index_count = 0;
        U64 serial = 0;         // the place's placement and writes (LLVKAttribPool::NumberPlace)
        U32 writes = 0;
        F32 matrix[16] = {};    // the record's transform, column-major
        F32 lo[3] = {};         // bounds in the transform's space
        F32 hi[3] = {};
        bool loose = false;     // bounds not known
    };
    // False: the scene is not ready. serial: moves when any record or transform row is written.
    bool collectRayRecords(const U32 pass_mask[4], std::vector<RayRecord>& out, U64& serial);
    // collectRayRecords' serial without the records (a cheap look for a change).
    U64 writeSerial();

    // What a device lacks to hold and draw the scene (empty: nothing), each
    // as "<name> (<what>)" with ", " between them (LLVKDevice's list of the
    // missing required features: asked for whenever LLRHI_SCENE is not off,
    // research/197 2.4, J3). Features: the indirect draws with a count and a
    // first instance; limits: the record table's draws and the cull's buffers.
    std::string deviceGaps(const VkPhysicalDeviceFeatures& features, bool draw_indirect_count, const VkPhysicalDeviceLimits& limits);
}

#endif // LL_LLVKSCENE_H
