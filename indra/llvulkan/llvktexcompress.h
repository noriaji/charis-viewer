/**
 * @file llvktexcompress.h
 * @brief Textures kept block-compressed (BC7) in VRAM, encoded on the GPU (research/88).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#ifndef LL_LLVKTEXCOMPRESS_H
#define LL_LLVKTEXCOMPRESS_H

#include "stdtypes.h"

#include <vulkan/vulkan.h>

#include <string>
#include <vector>

typedef struct VmaAllocation_T* VmaAllocation;

// research/88: the GL textures the viewer lets the backend compress
// (LLRHI::tTexCompress: the colour, normal and specular maps of objects) are
// made plain RGBA8 as before, mips and all. llvkgl then queues them, and at
// the start of a frame a few of them go through the encoder here: a compute
// shader (llvkbc7.glsl.inl) writes the BC7 blocks of every level, they are
// copied into a new BC7 image, and that image takes the texture's place (a
// quarter of the memory). This file owns the encoder: its pipelines, the
// per-frame scratch memory, the quality check and the counters. Render
// thread, under the tracker lock (the pipelines are made on threads of
// their own: prepare).
namespace LLVKTexCompress
{
    constexpr VkFormat ENCODED_FORMAT = VK_FORMAT_BC7_UNORM_BLOCK;

    // The device samples, filters, copies into and blits from BC7, the
    // encoder can encode (its first pipelines are made: see prepare), and its
    // self check has not failed. bc7-fault: nor did a frame's encodings take
    // far too long on the GPU, nor the last session lose the GPU while
    // encoding on this GPU and driver (noteDeviceLost). False: textures stay
    // as they are.
    bool ready();
    // bc7-fast-start (research/88 section 13): the encoder's pipelines are
    // made on compile threads of their own (the first call of prepare, ready
    // or built starts them), never in the frame: the AMD driver took 8
    // minutes over the encoder in one pipeline. On AMD the encoder is six
    // small pipelines (stages), and it encodes with the first one (mode 6)
    // while the others are made. Once a frame (compress_queued_textures):
    // starts the making once, takes what the threads have made; true while
    // it is being made and nothing can encode yet (the textures waiting then
    // stay queued, plain).
    bool prepare();
    // Every pipeline of the encoder is made (waits for the compile threads;
    // the shader bake, research/159), also while ready() is false for the
    // session (they are made all the same).
    bool built();
    // At shutdown (LLVKGL::stopPrewarm): no more pipelines are begun, and a
    // compile thread in glslang (the viewer's statics) is waited for, up to
    // 10 s. One in the driver's compile is left to the process exit (it
    // touches nothing of the viewer's after it).
    void stopBuilding();

    // Textures smaller than this (texels at level 0) are left plain: the
    // saving is small next to the work. LLRHI_TEX_BCN_MIN=<side> (default 256).
    U64 minTexels();

    // Start of a frame, before anything of it is recorded: the slot's last
    // quality checks are read, its scratch memory and descriptor sets are
    // free again.
    void beginFrame();

    // What this frame may encode (once a frame, while textures wait).
    // texels: level-0 texels, LLRHI_TEX_BCN_MPIX (default 2 million), up to
    // 4 times that while much waits (queued_bytes of plain images).
    // bc7-fault (research/88 section 11): blocks: blocks of every level,
    // started small and held to the GPU time earlier frames' encodings took
    // (LLRHI_TEX_BCN_MS, default 8 ms a frame, times the same boost). single:
    // the blocks a texture may take when it is the frame's first (from the
    // GPU's speed once known; a larger one waits). never: once the GPU's
    // speed is sure (a few frames timed, the fastest of the recent ones), a
    // texture larger than this is never encoded on this GPU (it is left
    // plain, noteLeftPlain); 0: not sure yet. All 0 before the world is
    // shown and for the first seconds after (LLRHI_TEX_BCN_HOLD, default 3;
    // 0: no hold at all, the world not waited for either).
    struct Budget
    {
        U64 texels = 0;
        U64 blocks = 0;
        U64 single = 0;
        U64 never = 0;
    };
    Budget frameBudget(U64 queued_bytes);
    // bc7-fault: a texture left plain, out of the queue, for being larger
    // than Budget::never (the counters and the report).
    void noteLeftPlain();
    // research/285 X3: a texture of this size may still be stored BC7 this
    // session: the encoder is not off (the device cannot, the self check
    // failed, a fault blocked it, a frame's encodings took too long) and,
    // once the GPU's speed is sure, it is not past Budget::never. Under
    // LLVKGL's sLock, as frameBudget and the encoder's flags (ensure_image).
    bool mayEncode(U32 width, U32 height, U32 levels);
    // The BC7 blocks of levels 0..levels-1 of a width x height texture.
    U64 blocksFor(U32 width, U32 height, U32 levels);
    // bc7-fault: after the frame's encodings, into the same command buffer
    // as encode's (the frame's upload commands): the end of their GPU time,
    // and how the frame's share grows. Once a frame, after frameBudget.
    // too_large: a texture was left waiting for being larger than the share.
    void endFrameEncodes(VkCommandBuffer cmd, bool too_large);
    // bc7-fault: the device was lost (the device's fault report; markers and
    // their stages: the graphics queue's checkpoints, LLRHI_VK_CHECKPOINTS).
    // When encodings were on the GPU (with checkpoints, on the graphics
    // queue: the last command finished, BOTTOM_OF_PIPE's marker, is the start
    // of one), the GPU, driver and encoder are noted in
    // <LLVKShader::cacheDir()>/bc7-fault.txt (a line a GPU), and the next
    // start with the same ones within 30 days leaves textures plain (one line
    // in the log; LLRHI_TEX_BCN_RETRY=1 tries again). A new driver tries
    // again by itself.
    void noteDeviceLost(const std::vector<U64>& markers, const std::vector<U32>& stages);
    // Checkpoint markers (LLRHI_VK_CHECKPOINTS) around a texture's encoding
    // in the frame: (marker << 32) | its GL name.
    constexpr U32 CHECKPOINT_BEGIN = 0xfffffffeu;
    constexpr U32 CHECKPOINT_END = 0xfffffffdu;

    // Room this frame for one more texture (descriptor sets).
    bool hasRoom();

    struct Encoded
    {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
    };
    // Records into cmd (the frame's upload commands) the encoding of levels
    // 0..levels-1 of source (R8G8B8A8_UNORM, sampled usage, every level in
    // SHADER_READ_ONLY_OPTIMAL, written by earlier commands) into a new BC7
    // image of the same size and level count, left in
    // SHADER_READ_ONLY_OPTIMAL. The source must live until the GPU is done
    // with these commands. name: the GL texture (logs). False: nothing
    // recorded, out untouched. verify: verifySerial's number (research/200),
    // 0 for none.
    bool encode(VkCommandBuffer cmd, VkImage source, U32 width, U32 height, U32 levels, U32 name, Encoded& out, U64 verify = 0);

    // ---- research/200: encoding on the compute queue (LLVKQueues) ----
    // OFF: no compute queue (LLRHI_ASYNC_QUEUES off, no compute-only family,
    // a compute submission failed, or auto found a mismatch). VERIFY: textures
    // are encoded into the frame's commands as before and some of them also
    // on the compute queue, the two results compared (that encoding is thrown
    // away). ON: the textures whose image is shared with the compute queue are
    // encoded there only.
    enum class Async : U8 { OFF, VERIFY, ON };
    Async asyncMode();
    // asyncMode() is not OFF: a texture's new image that may be stored BC7 is
    // made shared with the compute queue (LLVKQueues::shareWithCompute).
    bool asyncWanted();
    // This frame's compute batch (begun at the first call of the frame) has
    // room for one more texture. False: no free batch (none waited for), no
    // room, or asyncMode() is OFF.
    bool asyncRoom();
    // As encode, into this frame's compute batch (asyncRoom first): source is
    // shared with the compute queue and was written by graphics submissions
    // made before this frame's (the batch waits for them); out is shared too.
    // out may be put in the texture's place only after the compute timeline
    // reached the value submitAsync returns, and the graphics submissions
    // from then on wait for it (LLVKQueues::graphicsWaitsFor). verify: the
    // same number as the frame's encode of source (VERIFY).
    bool encodeAsync(VkImage source, U32 width, U32 height, U32 levels, U32 name, Encoded& out, U64 verify = 0);
    // Submits this frame's compute batch: its compute timeline value, or 0
    // when none was begun or the submission failed (then nothing encodeAsync
    // made this frame was ever written: the caller drops it).
    U64 submitAsync();
    // VERIFY: a number for a texture encoded both ways this frame (encode and
    // encodeAsync with it), whose blocks are compared once both are done; 0
    // when no more are compared this frame. dropVerify: one of the two was
    // not recorded after all.
    U64 verifySerial(U32 name);
    void dropVerify(U64 serial);
    // An encodeAsync result was put in its texture's place (used), or thrown
    // away because the texture changed meanwhile.
    void noteAsyncResult(bool used);

    // Records the decoding of levels 0..levels-1 of an encoded image
    // (SHADER_READ_ONLY_OPTIMAL) into plain (R8G8B8A8_UNORM, transfer
    // destination usage, contents undefined): one blit per level. Both end in
    // SHADER_READ_ONLY_OPTIMAL. For a compressed texture that is about to be
    // written (a sub-image, a copy, an attachment).
    void decode(VkCommandBuffer cmd, VkImage encoded, VkImage plain, U32 width, U32 height, U32 levels);

    // Counters: a texture's plain image replaced by its encoded one, and an
    // encoded image freed (or decoded back); the queue llvkgl keeps.
    void noteCompressed(U64 plain_bytes, U64 encoded_bytes);
    void noteReleased(U64 plain_bytes, U64 encoded_bytes);
    void noteQueue(U32 textures, U64 plain_bytes);

    // One line for the texture memory log and the census: textures stored
    // compressed, their MB against the plain MB, the queue, the quality.
    std::string report();
    // Textures encoded this session (research/109: the log after a snapshot).
    U64 encodedTotal();
}

#endif // LL_LLVKTEXCOMPRESS_H
