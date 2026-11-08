/**
 * @file llvktexcompress.cpp
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

#include "linden_common.h"

#include "llvktexcompress.h"

#include "llvkdevice.h"
#include "llvkmirror.h"
#include "llvkqueues.h"
#include "llvkshader.h"
#include "hbxxh.h"
#include "llfile.h"
#include "llrhi.h"      // bc7-fault: LLRHI::sWorldShown
#include "llrhihash.h"  // bc7-fast-start: the encoder's source in the fault note

#include "vk_mem_alloc.h"
#include "llvkdispatch.h"     // after VMA: it declares members with Vulkan function names

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

namespace
{
    // The encoder: one invocation per 4x4 block of one level.
    const char* const ENCODER_SOURCE =
#include "llvkbc7.glsl.inl"
        ;

    // The quality check: the squared error of each channel and the largest
    // error between a texture and its encoding (level 0), per 16x16 tile.
    // bc7-fault2: one invocation a tile, its sums in its own variables: no
    // shared memory, barrier or atomic (CHECK_SOURCE_OLD has them; the same
    // sums). A tile's squared errors fit: 256 x 255^2 < 2^32.
    const char* const CHECK_SOURCE = R"GLSL(
layout(local_size_x = 64) in;
layout(set = 0, binding = 0) uniform sampler2D u_source;
layout(set = 0, binding = 1) uniform sampler2D u_encoded;
layout(std430, set = 0, binding = 2) writeonly buffer Sums { uint sums[]; };
layout(push_constant) uniform Params
{
    uint width;
    uint height;
    uint tiles_x;
    uint first;     // in words
    uint tiles;
} params;
void main()
{
    uint tile = gl_GlobalInvocationID.x;
    if (tile >= params.tiles || params.tiles_x == 0u)
    {
        return;
    }
    uvec2 origin = uvec2(tile % params.tiles_x, tile / params.tiles_x) * 16u;
    uvec4 squares = uvec4(0u);
    uint largest = 0u;
    for (uint y = 0u; y < 16u; ++y)
    {
        for (uint x = 0u; x < 16u; ++x)
        {
            uvec2 xy = origin + uvec2(x, y);
            if (xy.x < params.width && xy.y < params.height)
            {
                ivec4 a = ivec4(floor(texelFetch(u_source, ivec2(xy), 0) * 255.0 + vec4(0.5)));
                ivec4 b = ivec4(floor(texelFetch(u_encoded, ivec2(xy), 0) * 255.0 + vec4(0.5)));
                uvec4 d = uvec4(abs(a - b));
                squares += d * d;
                largest = max(largest, max(max(d.x, d.y), max(d.z, d.w)));
            }
        }
    }
    uint at = params.first + tile * 5u;
    sums[at] = squares.x;
    sums[at + 1u] = squares.y;
    sums[at + 2u] = squares.z;
    sums[at + 3u] = squares.w;
    sums[at + 4u] = largest;
}
)GLSL";

    // The form up to 2026-11-03 (LLRHI_TEX_BCN_CHECK=old): a group a tile, an
    // invocation a texel, the sums by shared-memory atomics between barriers.
    const char* const CHECK_SOURCE_OLD = R"GLSL(
layout(local_size_x = 16, local_size_y = 16) in;
layout(set = 0, binding = 0) uniform sampler2D u_source;
layout(set = 0, binding = 1) uniform sampler2D u_encoded;
layout(std430, set = 0, binding = 2) writeonly buffer Sums { uint sums[]; };
layout(push_constant) uniform Params
{
    uint width;
    uint height;
    uint tiles_x;
    uint first;     // in words
    uint tiles;     // (not read here)
} params;
shared uint s_sums[5];
void main()
{
    uint lane = gl_LocalInvocationIndex;
    if (lane < 5u)
    {
        s_sums[lane] = 0u;
    }
    barrier();
    uvec2 xy = gl_GlobalInvocationID.xy;
    if (xy.x < params.width && xy.y < params.height)
    {
        ivec4 a = ivec4(floor(texelFetch(u_source, ivec2(xy), 0) * 255.0 + vec4(0.5)));
        ivec4 b = ivec4(floor(texelFetch(u_encoded, ivec2(xy), 0) * 255.0 + vec4(0.5)));
        ivec4 d = abs(a - b);
        atomicAdd(s_sums[0], uint(d.x * d.x));
        atomicAdd(s_sums[1], uint(d.y * d.y));
        atomicAdd(s_sums[2], uint(d.z * d.z));
        atomicAdd(s_sums[3], uint(d.w * d.w));
        atomicMax(s_sums[4], uint(max(max(d.x, d.y), max(d.z, d.w))));
    }
    barrier();
    if (lane < 5u)
    {
        sums[params.first + (gl_WorkGroupID.y * params.tiles_x + gl_WorkGroupID.x) * 5u + lane] = s_sums[lane];
    }
}
)GLSL";

    constexpr U32 BLOCK_BYTES = 16;
    constexpr U32 ENCODE_GROUP = 64;            // the encoder's local_size_x
    constexpr U32 CHECK_TILE = 16;              // the check's tile (each way)
    constexpr U32 CHECK_GROUP = 64;             // bc7-fault2: the safe check's local_size_x (tiles a group)
    constexpr U32 CHECK_WORDS = 5;              // per tile: squared error R, G, B, A, largest error
    constexpr U32 MAX_LEVELS = 16;
    // The first textures of a session are always checked: when their average
    // colour PSNR is under this, the encoder is broken on this GPU or driver
    // and compression turns off.
    constexpr U32 SELF_CHECK_TEXTURES = 8;
    constexpr F64 SELF_CHECK_MIN_PSNR = 30.0;
    // Textures under this PSNR are named in the log (up to LOW_PSNR_LOGS).
    constexpr F64 LOW_PSNR = 35.0;
    constexpr U32 LOW_PSNR_LOGS = 20;
    constexpr F64 PERFECT_PSNR = 99.0;
    // Per frame: descriptor sets (an encode takes one, a check another) and checks.
    constexpr U32 SETS_PER_FRAME = 128;
    constexpr U32 CHECKS_PER_FRAME = 2;
    constexpr VkDeviceSize READBACK_BYTES = (VkDeviceSize)CHECKS_PER_FRAME * (2048 / CHECK_TILE) * (2048 / CHECK_TILE) * CHECK_WORDS * 4;
    constexpr VkDeviceSize SCRATCH_MIN_BYTES = 8 << 20;
    // research/200 VERIFY: textures encoded both ways a frame, pairs waiting at
    // most, the comparisons auto needs before it moves on, and each slot's
    // copies of the blocks (a texture whose blocks do not fit is not compared).
    constexpr U32 VERIFY_PER_FRAME = 2;
    constexpr size_t VERIFY_PAIRS = 32;
    constexpr U32 VERIFY_AUTO_TEXTURES = 16;
    constexpr U32 MISMATCH_LOGS = 4;
    constexpr VkDeviceSize VERIFY_BYTES = 8 << 20;
    constexpr U32 SIDE_FRAME = 0;       // encode (the frame's commands)
    constexpr U32 SIDE_ASYNC = 1;       // encodeAsync (the compute queue)
    // The frame's texels grow by the base amount per this much waiting, up to MAX_BOOST times.
    constexpr U64 QUEUE_BOOST_BYTES = 64 << 20;
    constexpr U64 MAX_BOOST = 4;
    // A slot's scratch and readback memory go when it has encoded nothing this long.
    constexpr F64 IDLE_RELEASE_SECONDS = 30.0;
    constexpr F64 MB = 1024.0 * 1024.0;

    // bc7-fault (research/88 section 11): a driver (NVIDIA 610.88) faulted
    // the GPU in the encoder, a frame after the first encodings of a login.
    // A level is encoded by dispatches of at most DISPATCH_BLOCKS blocks
    // (256K texels: a large GPU filled once; LLRHI_TEX_BCN_CHUNK, 0: one
    // dispatch a level). The frames' encodings are timed on the GPU, and a
    // frame takes blocks for the time it may spend (Settings::target_ms):
    // START_BLOCKS until the first time is known (a texture alone up to
    // SINGLE_START_BLOCKS, a 1024x1024 one with its mips), then at most
    // twice the last frame's share, at least MIN_BLOCKS (a 256x256 texture
    // with its mips); a texture alone up to SINGLE_MS of the GPU. A frame of
    // encodings that took STALL_MS (well short of Windows' 2 s timeout) means
    // the encoder is far too slow on this GPU or driver: off for the session.
    constexpr U32 DISPATCH_BLOCKS = 16384;
    constexpr U64 START_BLOCKS = 16384;
    constexpr U64 SINGLE_START_BLOCKS = 87382;
    constexpr U64 MIN_BLOCKS = 5462;
    constexpr U64 MAX_BLOCKS = 1ull << 30;
    constexpr F64 SINGLE_MS = 50.0;
    constexpr F64 STALL_MS = 400.0;
    // The hold after the first texture was queued: at least this many frames too.
    constexpr U32 HOLD_FRAMES = 30;
    // Frames of encodings that may pass before the first GPU time comes back
    // (2 normally: the frames in flight); then the share grows untimed.
    constexpr U32 UNTIMED_FRAMES = 8;
    // Frames timed before a texture too large for SINGLE_MS of the GPU is
    // left plain for good (until then it waits at the back of the queue);
    // that takes the fastest of the last RECENT_RATES frames (not the shares'
    // careful speed: the small first frames and a GPU clocked down for a
    // moment show it slower than it is).
    constexpr U32 SURE_MEASURES = 4;
    constexpr U32 RECENT_RATES = 8;
    // KNOWN_BAD's entries name the encoders up to a revision, and the fault
    // note (bc7-fault.txt) matches by it: a new one (this number moved) is
    // tried again on a driver that faulted an older one. Move it whenever the
    // code a GPU runs changes. The note also holds the hash of the encoder's
    // source (make_fault_key), for reading only: a comment or the stages of
    // other GPUs change that hash while the code an NVIDIA GPU runs stays.
    // 3 (bc7-break-fix): principal_line's power iteration without a break,
    // what NVIDIA 610.88 and later hung on; KNOWN_BAD's entries for 2 are left behind.
    constexpr U32 ENCODER_REVISION = 3;
    // bc7-fault2: the BC7 modes the encoder has (bit n: mode n), and the quality check's forms.
    constexpr U32 ALL_MODES = (1u << 1) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7);
    constexpr U32 CHECK_SAFE = 0;
    constexpr U32 CHECK_OLD = 1;
    constexpr U32 CHECK_OFF = 2;
    constexpr U32 MAX_PASSES = 3;
    // bc7-fast-start (research/88 section 13): the encoder in stages, a
    // pipeline each, in their order (BC7_STAGE in llvkbc7.glsl.inl). The
    // search is a group a block; the stages after it run twice, the second
    // time on the second shape. KERNEL_AUTO: the stages on AMD (VENDOR_AMD:
    // its compiler took 8 minutes and 11 GB and more over the one pipeline,
    // 14 s and 2 GB at most over the stages), the one pipeline elsewhere.
    constexpr U32 STAGE_COUNT = 6;
    const char* const STAGE_DEFINES[STAGE_COUNT] = { "6", "4", "5", "BC7_STAGE_SEARCH", "1", "7" };
    constexpr U32 SEARCH_STAGE = 3;
    constexpr U32 MAX_PIPELINES = STAGE_COUNT > MAX_PASSES ? STAGE_COUNT : MAX_PASSES;
    constexpr U32 KERNEL_AUTO = 0;
    constexpr U32 KERNEL_PASSES = 1;
    constexpr U32 KERNEL_STAGES = 2;
    constexpr U32 VENDOR_AMD = 0x1002;
    // The compile threads for the pipelines after the first (LLRHI_TEX_BCN_BUILD_THREADS):
    // 2 (six at once took the AMD driver 9.7 s against 14.3 s one after the other,
    // with 2.9 GB against 1.9 GB more memory); 1 on an integrated GPU, whose PC
    // shares its memory with the GPU (the most a compile takes, 2 GB, under
    // the memory guard's line sooner with two at once; stage 6 is made first
    // all the same, the rest some seconds later).
    constexpr U32 BUILD_THREADS = 2;
    constexpr U32 BUILD_THREADS_INTEGRATED = 1;
    constexpr U32 MAX_BUILD_THREADS = STAGE_COUNT;
    // At shutdown, the longest wait for a compile thread in glslang (stopBuilding).
    constexpr S64 STOP_WAIT_SECONDS = 10;
    constexpr U32 NO_FAIL = 0xffffffffu;    // Settings::fail_piece

    const VkPipelineStageFlags2 SAMPLED_STAGES =
        VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;

    struct Settings
    {
        U64 min_texels = 256 * 256;
        U64 frame_texels = 2 << 20;
        bool tint = false;          // LLRHI_TEX_BCN_TINT=1: compressed textures drawn with less green
        bool check_all = false;     // LLRHI_TEX_BCN_PSNR=1: every texture checked (CHECKS_PER_FRAME a frame)
        // bc7-fault (above). LLRHI_TEX_BCN_CHUNK=<blocks>: a dispatch's most (0: a level's all);
        // LLRHI_TEX_BCN_MS=<ms>: a frame's GPU time for encodings (0: not timed, the share by
        // texels alone as before); LLRHI_TEX_BCN_HOLD=<s>: the hold (0: none, nor the wait for the world);
        // LLRHI_TEX_BCN_SAFE=<mask>: the encoder's safe forms (BC7_SAFE, llvkbc7.glsl.inl; 0: the
        // forms of 2026-11-02); LLRHI_TEX_BCN_RETRY=1: past a fault note (noteDeviceLost).
        U32 chunk_blocks = DISPATCH_BLOCKS;
        F64 target_ms = 8.0;
        F64 hold_seconds = 3.0;
        U32 safe = 15;
        bool retry = false;
        // bc7-fault2 (research/88 section 12), for finding the part a driver faults in:
        // LLRHI_TEX_BCN_MODES=<mask>: the BC7 modes the encoder may use (bit n: mode n; 1, 4,
        // 5, 6, 7; default 0xF2, all; BC7_MODES). LLRHI_TEX_BCN_KERNEL=<passes>[s][r]: the
        // encoder in 1, 2 or 3 dispatches a level (BC7_PASSES), s: the texels in shared memory
        // (BC7_PX_SHARED), r: the large loops kept as loops (BC7_ROLLED).
        // bc7-fast-start: LLRHI_TEX_BCN_KERNEL=stages[r]: the six stages (BC7_STAGE); auto
        // (the default): the stages on AMD, 1 elsewhere. LLRHI_TEX_BCN_BUILD_THREADS=<1..6>:
        // the compile threads for the pipelines after the first (default 2, 1 on an integrated GPU).
        // LLRHI_TEX_BCN_CHECK=1|old|0: the quality check without shared memory, barriers or
        // atomics (default), as up to 2026-11-03, or none (the self check then never decides).
        U32 modes = ALL_MODES;
        U32 kernel = KERNEL_AUTO;
        U32 passes = 1;
        bool px_shared = false;
        bool rolled = false;
        U32 build_threads = 0;      // 0: not given (BUILD_THREADS, or BUILD_THREADS_INTEGRATED)
        // LLRHI_TEX_BCN_FAIL=<n> (a test of the build's failure paths): piece n
        // of the build is given a line glslang refuses (0: the quality check,
        // 1..6: the encoder's pipelines in their order). NO_FAIL: none.
        U32 fail_piece = NO_FAIL;
        U32 check = CHECK_SAFE;
        std::string rejected;       // those of the above that were not a number in range (the default kept; logged)
    };

    // bc7-fault2: whole numbers (decimal, or hex with 0x) of up to 32 bits;
    // false (value untouched): not one.
    bool read_integer(const char* env, U32& value)
    {
        char* end = nullptr;
        const unsigned long long number = std::strtoull(env, &end, 0);
        if (end == env || *end != '\0' || number > 0xffffffffull)
        {
            return false;
        }
        value = (U32)number;
        return true;
    }

    // LLRHI_TEX_BCN_KERNEL: auto; or a digit 1..3, then s and r in any order,
    // each at most once; or stages, then r at most once (no s: the search
    // stage keeps a group's texels in no shared memory of their own).
    bool read_kernel(const char* env, U32& kernel, U32& passes, bool& px_shared, bool& rolled)
    {
        if (!strcmp(env, "auto"))
        {
            kernel = KERNEL_AUTO;
            return true;
        }
        const bool stages = !strncmp(env, "stages", 6);
        if (!stages && (env[0] < '1' || env[0] > '3'))
        {
            return false;
        }
        bool s = false, r = false;
        for (const char* c = env + (stages ? 6 : 1); *c; ++c)
        {
            bool& flag = *c == 's' ? s : r;
            if ((*c != 's' && *c != 'r') || flag || (stages && *c == 's'))
            {
                return false;
            }
            flag = true;
        }
        kernel = stages ? KERNEL_STAGES : KERNEL_PASSES;
        passes = stages ? 1 : (U32)(env[0] - '0');
        px_shared = s;
        rolled = r;
        return true;
    }

    // value: env as a number within lo..hi; false (value untouched): not one.
    bool read_number(const char* env, F64 lo, F64 hi, F64& value)
    {
        char* end = nullptr;
        const F64 number = std::strtod(env, &end);
        if (end == env || *end != '\0' || !(number >= lo && number <= hi))
        {
            return false;
        }
        value = number;
        return true;
    }

    const Settings& settings()
    {
        static const Settings s = []()
        {
            Settings out;
            if (const char* env = std::getenv("LLRHI_TEX_BCN_MIN"))
            {
                const U64 side = strtoull(env, nullptr, 10);
                out.min_texels = side * side;
            }
            if (const char* env = std::getenv("LLRHI_TEX_BCN_MPIX"))
            {
                out.frame_texels = llmax(strtoull(env, nullptr, 10), (unsigned long long)1) << 20;
            }
            const char* tint = std::getenv("LLRHI_TEX_BCN_TINT");
            out.tint = tint && *tint && strcmp(tint, "0");
            const char* check = std::getenv("LLRHI_TEX_BCN_PSNR");
            out.check_all = check && *check && strcmp(check, "0");
            // bc7-fault: a value that is not a number in range keeps the default
            // (never all the old forms, no split or no timing by a typing slip).
            auto reject = [&out](const char* name, const char* env)
            {
                out.rejected += llformat("%s%s=%.32s", out.rejected.empty() ? "" : ", ", name, env);
            };
            F64 number = 0.0;
            if (const char* env = std::getenv("LLRHI_TEX_BCN_CHUNK"))
            {
                // Whole groups of the encoder, at most 2^24 blocks (0: no split).
                if (read_number(env, 0.0, (F64)(1u << 24), number))
                {
                    const U32 chunk = (U32)number;
                    out.chunk_blocks = chunk ? llmax(chunk / ENCODE_GROUP * ENCODE_GROUP, ENCODE_GROUP) : 0;
                }
                else
                {
                    reject("LLRHI_TEX_BCN_CHUNK", env);
                }
            }
            if (const char* env = std::getenv("LLRHI_TEX_BCN_MS"))
            {
                if (!read_number(env, 0.0, 1000.0, out.target_ms))
                {
                    reject("LLRHI_TEX_BCN_MS", env);
                }
            }
            if (const char* env = std::getenv("LLRHI_TEX_BCN_HOLD"))
            {
                if (!read_number(env, 0.0, 600.0, out.hold_seconds))
                {
                    reject("LLRHI_TEX_BCN_HOLD", env);
                }
            }
            if (const char* env = std::getenv("LLRHI_TEX_BCN_SAFE"))
            {
                if (read_number(env, 0.0, 15.0, number) && number == std::floor(number))
                {
                    out.safe = (U32)number;
                }
                else
                {
                    reject("LLRHI_TEX_BCN_SAFE", env);
                }
            }
            const char* retry = std::getenv("LLRHI_TEX_BCN_RETRY");
            out.retry = retry && *retry && strcmp(retry, "0");
            if (const char* env = std::getenv("LLRHI_TEX_BCN_MODES"))
            {
                U32 modes = 0;
                if (read_integer(env, modes) && modes && !(modes & ~ALL_MODES))
                {
                    out.modes = modes;
                }
                else
                {
                    reject("LLRHI_TEX_BCN_MODES", env);
                }
            }
            if (const char* env = std::getenv("LLRHI_TEX_BCN_KERNEL"))
            {
                if (!read_kernel(env, out.kernel, out.passes, out.px_shared, out.rolled))
                {
                    reject("LLRHI_TEX_BCN_KERNEL", env);
                }
            }
            if (const char* env = std::getenv("LLRHI_TEX_BCN_BUILD_THREADS"))
            {
                U32 threads = 0;
                if (read_integer(env, threads) && threads >= 1 && threads <= MAX_BUILD_THREADS)
                {
                    out.build_threads = threads;
                }
                else
                {
                    reject("LLRHI_TEX_BCN_BUILD_THREADS", env);
                }
            }
            if (const char* env = std::getenv("LLRHI_TEX_BCN_FAIL"))
            {
                U32 piece = 0;
                if (read_integer(env, piece) && piece <= MAX_PIPELINES)
                {
                    out.fail_piece = piece;
                }
                else
                {
                    reject("LLRHI_TEX_BCN_FAIL", env);
                }
            }
            if (const char* env = std::getenv("LLRHI_TEX_BCN_CHECK"))
            {
                if (!strcmp(env, "1"))
                {
                    out.check = CHECK_SAFE;
                }
                else if (!strcmp(env, "old"))
                {
                    out.check = CHECK_OLD;
                }
                else if (!strcmp(env, "0"))
                {
                    out.check = CHECK_OFF;
                }
                else
                {
                    reject("LLRHI_TEX_BCN_CHECK", env);
                }
            }
            return out;
        }();
        return s;
    }

    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        VkDeviceSize size = 0;
        void* mapped = nullptr;
    };

    // A quality check waiting for its frame to finish.
    struct Check
    {
        U32 name = 0;
        U32 width = 0;
        U32 height = 0;
        U32 tiles = 0;
        VkDeviceSize offset = 0;    // bytes into the slot's readback buffer
    };

    // research/200 VERIFY: a copy of one texture's blocks in a slot's verify buffer.
    struct VerifyPart
    {
        U64 serial = 0;
        VkDeviceSize offset = 0;
        VkDeviceSize bytes = 0;
    };

    // What one frame in flight uses; free again when the slot's next frame
    // begins. research/200: also what one compute batch uses (Encoder::
    // async_slots, by the batch's index in LLVKQueues' ring); free again once
    // the compute timeline reached its value.
    struct Slot
    {
        U64 begins = 0;             // LLVKMirror::frameBegins() of its frame
        VkDescriptorPool pool = VK_NULL_HANDLE;
        U32 sets = 0;
        Buffer scratch;             // the encoded blocks, before they are copied into their images
        VkDeviceSize scratch_head = 0;
        Buffer readback;            // the checks' sums
        VkDeviceSize readback_head = 0;
        std::vector<Check> checks;
        std::chrono::steady_clock::time_point last_used;
        Buffer verify;              // research/200: copies of the blocks of textures encoded both ways
        VkDeviceSize verify_head = 0;
        std::vector<VerifyPart> verify_parts;
        // Compute slots: something was recorded and is not read and freed yet
        // (pending), and the compute value its batch signals (0: not submitted
        // yet, or its submission failed).
        bool pending = false;
        U64 value = 0;
        // bc7-fault, frame slots: encodings recorded and not known to be done
        // (on_gpu), and their GPU time (timestamps 2 * the slot's index and
        // the one after: begun, both recorded, the blocks between them).
        bool on_gpu = false;
        bool timing = false;
        bool timed = false;
        U64 timed_blocks = 0;
        U32 timed_kernel = 0;       // bc7-fast-start: Pace::kernel when timed (another: not read)
    };

    // bc7-fast-start (research/88 section 13): the encoder's pipelines, made
    // on compile threads of their own (LLVKShader::Workers at the lowest
    // priority), never while a frame waits. Pieces: the quality check (when
    // on), then the encoder's pipelines in their order. The check and the
    // first pipeline are one job, made first; it posts the others, which the
    // threads share. A piece's fields are its thread's until its state leaves
    // WAITING (release; the render thread reads them after an acquire, in
    // collect). Allocated once, never freed: a thread may still be in the
    // driver's compile when the viewer exits.
    constexpr U32 PIECE_WAITING = 0;
    constexpr U32 PIECE_MADE = 1;
    constexpr U32 PIECE_FAILED = 2;
    constexpr U32 PIECE_DROPPED = 3;    // not made: stopBuilding, or a first piece failed (no log line of its own)

    struct Build
    {
        struct Piece
        {
            std::string source;                 // the whole GLSL (#version, defines, shader)
            VkPipelineLayout layout = VK_NULL_HANDLE;
            std::string what;                   // for the logs
            LLVKShader::Program program;        // kept: the module lives as long as the pipeline
            VkPipeline pipeline = VK_NULL_HANDLE;
            F64 seconds = 0.0;                  // glslang (or the SPIR-V cache) and the driver
            std::atomic<U32> state{ PIECE_WAITING };
        };
        std::vector<std::unique_ptr<Piece>> pieces;
        U32 check_pieces = 0;                   // 1: pieces[0] is the quality check
        LLVKShader::Workers* workers = nullptr;
        std::chrono::steady_clock::time_point started;
        // stopBuilding: no piece is begun after stop, and it waits for in_glslang to be 0.
        std::mutex mutex;
        std::condition_variable left_glslang;
        bool stop = false;
        U32 in_glslang = 0;
    };
    // The build's threads at normal priority while someone waits on them
    // (built, stopBuilding), at the lowest again after, however the wait ends.
    class Foreground
    {
    public:
        explicit Foreground(LLVKShader::Workers& workers) : mWorkers(workers) { mWorkers.setBackground(false); }
        ~Foreground() { mWorkers.setBackground(true); }
        Foreground(const Foreground&) = delete;
        Foreground& operator=(const Foreground&) = delete;

    private:
        LLVKShader::Workers& mWorkers;
    };

    // The build for stopBuilding, which runs on the main thread at shutdown
    // while a frame may still run on the present thread (Encoder::build is
    // that thread's), and whether it has run (no build begins after it).
    std::mutex sStopMutex;
    Build* sBuildToStop = nullptr;
    bool sStopped = false;

    struct Encoder
    {
        bool tried = false;
        bool unavailable = false;   // the device cannot, or the first pipelines failed: textures stay as they are
        bool failed = false;        // the self check failed: off for the session
        // bc7-fault: the last session lost the GPU while encoding on this GPU,
        // driver, encoder and forms (fault_*, in bc7-fault.txt): off for the session.
        bool blocked = false;
        std::string blocked_why;    // bc7-fault2: for the report
        std::string fault_gpu;      // make_fault_key
        std::string fault_encoder;
        std::string fault_source;
        std::string fault_forms;
        VkQueryPool timestamps = VK_NULL_HANDLE;    // null: the frames' encodings are not timed
        U64 timestamp_mask = 0;
        F64 ms_per_tick = 0.0;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout encode_sets = VK_NULL_HANDLE;
        VkDescriptorSetLayout check_sets = VK_NULL_HANDLE;
        VkPipelineLayout encode_layout = VK_NULL_HANDLE;
        VkPipelineLayout check_layout = VK_NULL_HANDLE;
        // The encoder's pipelines: bc7-fault2's passes (Settings::passes of
        // them; one: the whole encoder), or bc7-fast-start's stages. usable:
        // how many of them, from the first, encode now (the passes: all or
        // none; the stages: those made so far in a row).
        bool stages = false;
        U32 pipeline_count = 0;
        U32 build_threads = 1;      // for the pipelines after the first (start)
        U32 usable = 0;
        VkPipeline encode_pipelines[MAX_PIPELINES] = {};
        VkPipeline check_pipeline = VK_NULL_HANDLE;
        Build* build = nullptr;     // null: not begun
        bool taken[MAX_PIPELINES + 1] = {};     // collect: the build's pieces taken
        bool settled = false;       // every piece made or failed, and taken
        bool all_made = false;
        Slot slots[LLVKMirror::FRAMES_IN_FLIGHT];
        // research/200: the compute batches' slots, and this frame's batch while it is open.
        Slot async_slots[LLVKQueues::COMPUTE_RING];
        LLVKQueues::Batch batch;
        bool batch_open = false;
    };
    Encoder sEncoder;

    // bc7-fault: how much a frame encodes (frameBudget), from the GPU time
    // earlier frames' encodings took.
    struct Pace
    {
        bool waiting = false;       // textures waited with the world shown: the hold has begun
        std::chrono::steady_clock::time_point since;
        U64 since_frame = 0;        // LLVKMirror::frameBegins() then
        bool held = false;          // this frame is in the hold
        U64 frame_blocks = 0;       // this frame's encodings so far
        U64 allowance = START_BLOCKS;
        U64 single = SINGLE_START_BLOCKS;   // until the speed is known
        U64 never = 0;              // the last Budget::never (0: not sure yet), for mayEncode
        F64 blocks_per_ms = 0.0;    // measured (0: not yet)
        U32 measured = 0;           // frames timed
        F64 recent_rates[RECENT_RATES] = {};    // the last frames' blocks a ms (a ring)
        F64 longest_ms = 0.0;
        U64 longest_blocks = 0;
        bool stalled = false;       // a frame's encodings took STALL_MS or more: off
        bool first_noted = false;   // the fault report's event for the first encodings
        U32 untimed_frames = 0;     // frames of encodings before the first time came back
        // bc7-fast-start: moves on when more stages encode (restart_pace): the
        // frames timed before were of a faster encoder, their times are dropped.
        U32 kernel = 0;
    };
    Pace sPace;

    struct EncodeParams
    {
        U32 level;
        U32 width;
        U32 height;
        U32 blocks_x;
        U32 block_count;
        U32 first_block;
        U32 flags;
        U32 base;       // the first of the level's blocks a dispatch encodes (bc7-fault)
        U32 aux_block;  // bc7-fault2, the passes: the level's first aux block
    };
    constexpr U32 FLAG_TINT = 1;
    constexpr U32 FLAG_SECOND_SHAPE = 2;    // bc7-fast-start: a two-subset stage's second run

    struct CheckParams
    {
        U32 width;
        U32 height;
        U32 tiles_x;
        U32 first;
        U32 tiles;      // bc7-fault2: the safe form's invocations
    };

    struct Counters
    {
        U32 textures = 0;           // stored compressed now
        U64 plain_bytes = 0;        // what their plain images took
        U64 encoded_bytes = 0;      // what their encoded images take
        U64 encoded_total = 0;      // textures encoded this session
        U64 texels_total = 0;       // their level-0 texels
        U32 queued = 0;
        U64 queued_bytes = 0;
        U64 left_plain = 0;         // bc7-fault: too large for this GPU's speed (noteLeftPlain)
    };
    Counters sCounters;

    struct Quality
    {
        U32 checked = 0;
        F64 rgb_sum = 0.0, alpha_sum = 0.0;
        F64 rgb_lowest = PERFECT_PSNR, alpha_lowest = PERFECT_PSNR;
        U32 largest_error = 0;
        U32 self_checked = 0;
        F64 self_rgb_sum = 0.0;
        U32 low_logged = 0;
    };
    Quality sQuality;

    // research/200: the compute queue's encodings, and VERIFY's comparisons.
    struct AsyncState
    {
        enum Decision : U8 { UNDECIDED, PASSED, FAILED };
        Decision decision = UNDECIDED;  // auto: VERIFY until PASSED (ON) or FAILED (OFF)
        // A texture encoded both ways: the blocks' hash of each side once done.
        struct Pair
        {
            U64 serial = 0;
            U32 name = 0;
            U64 hash[2] = {};
            U8 have = 0;            // bit per side
            bool missing = false;   // a side could not copy its blocks: not compared
        };
        std::vector<Pair> pairs;
        U64 next_serial = 1;
        U32 frame_serials = 0;      // verifySerial's this frame
        U32 compared = 0;
        U32 mismatched = 0;
        U64 encoded = 0;            // encodeAsync's (VERIFY's included)
        U64 used = 0;               // put in their texture's place
        U64 dropped = 0;            // thrown away, the texture changed meanwhile
        U32 failed = 0;             // batches whose submission failed
    };
    AsyncState sAsync;

    U32 level_size(U32 size, U32 level)
    {
        return llmax(1u, size >> level);
    }

    U32 blocks(U32 size)
    {
        return (size + 3) / 4;
    }

    // The blocks of levels 0..levels-1 (at most MAX_LEVELS), one level after
    // the other: the total, and where each level begins (first_block, when given).
    U32 level_layout(U32 width, U32 height, U32 levels, U32* first_block)
    {
        U32 total = 0;
        for (U32 level = 0; level < llmin(levels, MAX_LEVELS); ++level)
        {
            if (first_block)
            {
                first_block[level] = total;
            }
            total += blocks(level_size(width, level)) * blocks(level_size(height, level));
        }
        return total;
    }

    bool format_has(VkFormat format, VkFormatFeatureFlags wanted)
    {
        VkFormatProperties props = {};
        vkGetPhysicalDeviceFormatProperties(LLVKDevice::instance().getPhysicalDevice(), format, &props);
        return (props.optimalTilingFeatures & wanted) == wanted;
    }

    bool supported()
    {
        const LLVKDevice& device = LLVKDevice::instance();
        return device.isReady() &&
               format_has(LLVKTexCompress::ENCODED_FORMAT,
                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                          VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT) &&
               format_has(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_BLIT_DST_BIT);
    }

    VkDescriptorSetLayout make_set_layout(U32 samplers)
    {
        VkDescriptorSetLayoutBinding bindings[3] = {};
        for (U32 i = 0; i <= samplers; ++i)
        {
            bindings[i].binding = i;
            bindings[i].descriptorType = i < samplers ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        info.bindingCount = samplers + 1;
        info.pBindings = bindings;
        VkDescriptorSetLayout layout = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(LLVKDevice::instance().getDevice(), &info, nullptr, &layout);
        return layout;
    }

    VkPipelineLayout make_pipeline_layout(VkDescriptorSetLayout sets, U32 push_bytes)
    {
        VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes };
        VkPipelineLayoutCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        info.setLayoutCount = 1;
        info.pSetLayouts = &sets;
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &push;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        vkCreatePipelineLayout(LLVKDevice::instance().getDevice(), &info, nullptr, &layout);
        return layout;
    }

    // bc7-fast-start: a piece of the build, on a compile thread: glslang (or
    // the SPIR-V cache), then the driver's compile (or the pipeline cache).
    // Once stopBuilding has run, nothing more of the viewer's is touched
    // (the process is ending): the piece only fails.
    void make_piece(Build& build, Build::Piece& piece)
    {
        const auto start = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(build.mutex);
            if (build.stop)
            {
                piece.state.store(PIECE_DROPPED, std::memory_order_release);
                return;
            }
            ++build.in_glslang;
        }
        bool compiled = false;
        try
        {
            compiled = LLVKShader::build({ { LLVKShader::Stage::Compute, piece.source } }, piece.program) && !piece.program.modules.empty();
        }
        catch (...)
        {
            compiled = false;   // (out of memory, say): the piece fails, the build goes on
        }
        VkDevice device = VK_NULL_HANDLE;
        VkPipelineCache cache = VK_NULL_HANDLE;
        VkPipelineCreateFlags flags = 0;
        {
            // The device's handles under the same lock as stop: a thread held
            // up between here and the compile never reads LLVKDevice (a static)
            // after stopBuilding.
            std::lock_guard<std::mutex> lock(build.mutex);
            --build.in_glslang;
            build.left_glslang.notify_all();
            if (build.stop || !compiled)
            {
                piece.state.store(build.stop ? PIECE_DROPPED : PIECE_FAILED, std::memory_order_release);
                return;
            }
            LLVKDevice& vk = LLVKDevice::instance();
            device = vk.getDevice();
            cache = vk.usePipelineCache();
            flags = vk.pipelineStatisticsFlags();       // bc7-fault2: LLRHI_VK_PIPELINE_STATS=1
        }
        // Compiled: the program has its module (only now: glslang's failure,
        // an exception in it and a failed vkCreateShaderModule leave none).
        VkComputePipelineCreateInfo info = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        info.stage.module = piece.program.modules[0];
        info.stage.pName = "main";
        info.layout = piece.layout;
        info.flags = flags;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(device, cache, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
        {
            pipeline = VK_NULL_HANDLE;
        }
        std::lock_guard<std::mutex> lock(build.mutex);
        if (pipeline && !build.stop)
        {
            // Marked for the periodic save again now that it is in the cache:
            // a save while the driver compiled cleared the first mark.
            LLVKDevice::instance().usePipelineCache();
        }
        piece.pipeline = pipeline;
        piece.seconds = std::chrono::duration<F64>(std::chrono::steady_clock::now() - start).count();
        piece.state.store(pipeline ? PIECE_MADE : PIECE_FAILED, std::memory_order_release);
    }

    // A piece an exception left (a failed allocation, say) fails, so collect
    // settles: none stays WAITING, the textures queued never wait for it.
    void fail_if_waiting(Build::Piece& piece)
    {
        U32 waiting = PIECE_WAITING;
        piece.state.compare_exchange_strong(waiting, PIECE_FAILED, std::memory_order_release);
    }

    void make_piece_or_fail(Build& build, Build::Piece& piece)
    {
        try
        {
            make_piece(build, piece);
        }
        catch (...)
        {
            fail_if_waiting(piece);
        }
    }

    // The build's first job: the pieces up to `first` (the check, the first
    // pipeline: what the first encodings need), one after the other before
    // anything else takes the threads, then the others posted for every
    // thread. Once a piece is not made (failed, or dropped for the shutdown),
    // nothing could encode with the ones after it: they are not made either
    // (no compile for nothing, the one pipeline's the heaviest).
    void run_first(Build& build, size_t first)
    {
        size_t next = 0;    // the pieces from here on are neither made nor posted yet
        try
        {
            bool made = true;
            for (; next < build.pieces.size(); ++next)
            {
                Build::Piece* piece = build.pieces[next].get();
                if (!made)
                {
                    piece->state.store(PIECE_DROPPED, std::memory_order_release);
                    continue;
                }
                if (next < first)
                {
                    make_piece(build, *piece);
                    made = piece->state.load(std::memory_order_relaxed) == PIECE_MADE;
                    continue;
                }
                build.workers->post([&build, piece]() { make_piece_or_fail(build, *piece); });
            }
        }
        catch (...)
        {
            for (size_t i = next; i < build.pieces.size(); ++i)
            {
                fail_if_waiting(*build.pieces[i]);
            }
        }
    }

    // bc7-fault2: the driver's figures for a pipeline (registers, local and
    // shared memory, ...), with LLRHI_VK_PIPELINE_STATS=1.
    void log_statistics(VkPipeline pipeline, const char* what)
    {
        const std::string figures = LLVKDevice::instance().pipelineStatistics(pipeline);
        if (!figures.empty())
        {
            LL_INFOS("TexBCN") << what << " (LLRHI_VK_PIPELINE_STATS): " << figures << LL_ENDL;
        }
    }

    // bc7-fault: two timestamps a frame slot for the frames' encodings, when
    // the graphics queue has timestamps and the share is timed (target_ms).
    // Without them the share grows from START_BLOCKS by itself. Kept until
    // the process ends, like the encoder's pipelines and sampler: the device
    // is never destroyed at exit (LLVKDevice::shutdown runs only for a
    // failed init, before the encoder is made).
    void create_timestamps()
    {
        Encoder& e = sEncoder;
        const LLVKDevice& device = LLVKDevice::instance();
        U32 families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device.getPhysicalDevice(), &families, nullptr);
        std::vector<VkQueueFamilyProperties> properties(families);
        vkGetPhysicalDeviceQueueFamilyProperties(device.getPhysicalDevice(), &families, properties.data());
        const U32 family = device.getGraphicsQueueFamily();
        const U32 bits = family < families ? properties[family].timestampValidBits : 0;
        const F64 period = device.getLimits().timestampPeriod;
        if (!bits || period <= 0.0 || settings().target_ms <= 0.0)
        {
            return;
        }
        VkQueryPoolCreateInfo info = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = 2 * LLVKMirror::FRAMES_IN_FLIGHT;
        if (vkCreateQueryPool(device.getDevice(), &info, nullptr, &e.timestamps) != VK_SUCCESS)
        {
            e.timestamps = VK_NULL_HANDLE;
            return;
        }
        e.timestamp_mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
        e.ms_per_tick = period * 1e-6;
    }

    // bc7-fault: the fault notes (noteDeviceLost), a line a GPU and forms (a
    // PC with two GPUs keeps both, a start with diagnostic switches its own):
    // "<gpu> <driver and encoder> <forms> at <seconds since 1970> <source>"
    // (make_fault_key; the source is not matched). A note counts for
    // FAULT_NOTE_SECONDS; at most FAULT_NOTE_LINES lines are kept.
    constexpr size_t FAULT_NOTE_LINES = 16;
    constexpr S64 FAULT_NOTE_SECONDS = 30 * 24 * 3600;

    std::string fault_path()
    {
        const std::string& dir = LLVKShader::cacheDir();
        return dir.empty() ? std::string() : dir + "/bc7-fault.txt";
    }

    // The kernel as LLRHI_TEX_BCN_KERNEL names it ("stages", "1", "2s", ...), for the logs and the fault note.
    std::string kernel_name()
    {
        const Settings& s = settings();
        return (sEncoder.stages ? std::string("stages") : std::to_string(s.passes)) + (s.px_shared ? "s" : "") + (s.rolled ? "r" : "");
    }

    // This GPU ("vendor V device D"), its driver and the encoder ("driver X
    // encoder E": E is ENCODER_REVISION), the encoder's source ("source H",
    // the FNV-1a of its GLSL: in the note and the log, not matched), and the
    // forms the settings ask for ("safe S modes M kernel K check C").
    void make_fault_key()
    {
        Encoder& e = sEncoder;
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(LLVKDevice::instance().getPhysicalDevice(), &props);
        const Settings& s = settings();
        const U64 source = LLRHI::fnv1a(LLRHI::FNV1A_BASIS, ENCODER_SOURCE, strlen(ENCODER_SOURCE));
        e.fault_gpu = llformat("vendor %u device %u", props.vendorID, props.deviceID);
        e.fault_encoder = llformat("driver %u encoder %u", props.driverVersion, ENCODER_REVISION);
        e.fault_source = llformat("source %016llx", (unsigned long long)source);
        e.fault_forms = llformat("safe %u modes 0x%X kernel %s check %u", s.safe, s.modes, kernel_name().c_str(), s.check);
    }

    // A note's line without its "source H" (matched without it; the lines
    // written before it was there, by bc7-fault and bc7-fault2, have none;
    // it is at the end, a line of 11828b81e6 has it before the forms).
    std::string without_source(const std::string& line)
    {
        const size_t at = line.find(" source ");
        if (at == std::string::npos)
        {
            return line;
        }
        const size_t end = line.find(' ', at + 8);
        return end == std::string::npos ? line.substr(0, at) : line.substr(0, at) + line.substr(end);
    }

    // bc7-fault2: drivers known to fault the GPU in this encoder (research/88
    // sections 11 and 12): textures stay plain on them unless
    // LLRHI_TEX_BCN_RETRY=1. Each: a vendor's drivers from a version on (later
    // ones too, until the encoder is checked on one: a new ENCODER_REVISION
    // then leaves the entry behind), for the encoders up to a revision.
    // NVIDIA's Vulkan driverVersion: major << 22 | minor << 14 | ...
    // Only NVIDIA's entries are read (known_bad skips any other vendor: an
    // AMD or Intel entry would be ignored until known_bad reads that
    // vendor's driverVersion, which is laid out otherwise).
    struct KnownBad
    {
        U32 vendor;
        U32 major;
        U32 minor;
        U32 encoder;
        const char* what;
    };
    constexpr U32 VENDOR_NVIDIA = 0x10DE;
    constexpr KnownBad KNOWN_BAD[] = {
        { VENDOR_NVIDIA, 610, 88, 2, "NVIDIA 610.88 and later: the GPU stops in the encoder seconds after the first textures "
                                     "(RTX 5070, 2026-11-03; nvlddmkm 153)" },
    };

    const KnownBad* known_bad()
    {
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(LLVKDevice::instance().getPhysicalDevice(), &props);
        for (const KnownBad& bad : KNOWN_BAD)
        {
            if (props.vendorID != bad.vendor || ENCODER_REVISION > bad.encoder || bad.vendor != VENDOR_NVIDIA)
            {
                continue;
            }
            const U32 major = (props.driverVersion >> 22) & 0x3ff;
            const U32 minor = (props.driverVersion >> 14) & 0xff;
            if (major > bad.major || (major == bad.major && minor >= bad.minor))
            {
                return &bad;
            }
        }
        return nullptr;
    }

    // The driver is on the list: off for the session (one line in the log).
    void read_known_bad()
    {
        Encoder& e = sEncoder;
        const KnownBad* bad = known_bad();
        if (!bad)
        {
            return;
        }
        if (settings().retry)
        {
            LL_WARNS("TexBCN") << "BC7 (bc7-fault2): tried on a driver known to fault the GPU in it (" << bad->what
                               << "), as LLRHI_TEX_BCN_RETRY=1 asks" << LL_ENDL;
            return;
        }
        e.blocked = true;
        e.blocked_why = bad->what;
        LL_WARNS("TexBCN") << "BC7 texture compression off (bc7-fault2): " << bad->what << ". LLRHI_TEX_BCN_RETRY=1 tries it" << LL_ENDL;
    }

    bool is_line_of(const std::string& line, const std::string& gpu)
    {
        return line.size() > gpu.size() && line.compare(0, gpu.size(), gpu) == 0 && line[gpu.size()] == ' ';
    }

    // The note's lines (a longer line ends the reading: none is that long).
    std::vector<std::string> read_fault_lines(const std::string& path)
    {
        std::vector<std::string> lines;
        if (!LLFile::isfile(path))
        {
            return lines;
        }
        llifstream in(path);
        char line[256];
        while (lines.size() < FAULT_NOTE_LINES && in.getline(line, sizeof(line)))
        {
            if (*line)
            {
                lines.push_back(line);
            }
        }
        return lines;
    }

    // False: the note could not be written (or removed, when no line is
    // left). Written whole into a temporary file of this process and thread,
    // then put in the note's place (LLVKShader::writeCacheFile): a write cut
    // short (the viewer ends right after a lost device) leaves the old note
    // whole, never half a line, and two viewers on one cache never write the
    // same temporary file.
    bool write_fault_lines(const std::string& path, const std::vector<std::string>& lines)
    {
        if (lines.empty())
        {
            return LLFile::remove(path, ENOENT) == 0 || !LLFile::isfile(path);
        }
        std::string data;
        for (const std::string& line : lines)
        {
            data += line;
            data += '\n';
        }
        return LLVKShader::writeCacheFile(path, { { data.data(), data.size() } });
    }

    // The start of this GPU's lines for this driver and encoder, and of the
    // one for these forms too (its time after it), as without_source leaves them.
    std::string fault_line_encoder()
    {
        return sEncoder.fault_gpu + " " + sEncoder.fault_encoder + " ";
    }

    std::string fault_line_mine()
    {
        return fault_line_encoder() + sEncoder.fault_forms + " at ";
    }

    // The line noteDeviceLost writes now: the source's hash, for reading, at
    // the end after the time, where the versions before bc7-fast-start read
    // the line as their own (their time is read up to the space).
    std::string fault_line_now()
    {
        const Encoder& e = sEncoder;
        return fault_line_mine() + std::to_string((S64)std::time(nullptr)) + " " + e.fault_source;
    }

    // This GPU's line for this driver, encoder and forms, when recent: off for
    // the session (one line in the log); older, or LLRHI_TEX_BCN_RETRY=1, it
    // goes and the encoder is tried. This GPU's lines for another driver or
    // encoder are old and go too. Its lines for the same driver and encoder
    // with other forms stay (a start with diagnostic switches leaves the
    // default forms' note alone, and the other way round), as do other GPUs'.
    void read_fault_note()
    {
        Encoder& e = sEncoder;
        const std::string path = fault_path();
        std::vector<std::string> lines = path.empty() ? std::vector<std::string>() : read_fault_lines(path);
        const std::string same_encoder = fault_line_encoder();
        const std::string mine = fault_line_mine();
        const S64 now = (S64)std::time(nullptr);
        const char* why = nullptr;
        for (auto it = lines.begin(); it != lines.end();)
        {
            if (!is_line_of(*it, e.fault_gpu))
            {
                ++it;
                continue;
            }
            const std::string plain = without_source(*it);
            if (plain.compare(0, same_encoder.size(), same_encoder) != 0)
            {
                why = "another driver or encoder";
                it = lines.erase(it);
                continue;
            }
            const size_t at_word = plain.rfind(" at ");
            const S64 at = at_word == std::string::npos ? 0 : std::strtoll(plain.c_str() + at_word + 4, nullptr, 10);
            // Why the line no longer counts (null: recent, it does).
            const char* expired = at <= 0 ? "unreadable time" : at > now ? "in the future" : now - at >= FAULT_NOTE_SECONDS ? "older than 30 days" : nullptr;
            const bool recent = !expired;
            if (plain.compare(0, mine.size(), mine) != 0)
            {
                // Other forms: kept while recent (FAULT_NOTE_SECONDS, as this one's).
                if (recent)
                {
                    ++it;
                }
                else
                {
                    why = expired;
                    it = lines.erase(it);
                }
                continue;
            }
            if (recent && !settings().retry)
            {
                e.blocked = true;
                e.blocked_why = "the last session lost the GPU while encoding on this GPU and driver (bc7-fault.txt)";
                LL_WARNS("TexBCN") << "BC7 texture compression off (bc7-fault): the last session lost the GPU while textures were "
                                   << "being encoded on this GPU and driver (" << *it << "; " << path
                                   << "). LLRHI_TEX_BCN_RETRY=1 tries again; a new driver does by itself, so does the note in 30 days"
                                   << LL_ENDL;
                ++it;
                continue;
            }
            why = recent ? "LLRHI_TEX_BCN_RETRY=1" : expired;
            it = lines.erase(it);
        }
        if (why)
        {
            const bool written = write_fault_lines(path, lines);
            LL_INFOS("TexBCN") << "BC7 (bc7-fault): a fault note of this GPU (" << why << ") " << (written ? "removed" : "could not be removed")
                               << (e.blocked ? "" : "; encoding is tried") << LL_ENDL;
        }
    }

    // The sampler and the layouts the pipelines are made with (render thread, at the start).
    bool make_layouts()
    {
        Encoder& e = sEncoder;
        VkDevice device = LLVKDevice::instance().getDevice();
        VkSamplerCreateInfo sampler = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sampler.magFilter = VK_FILTER_NEAREST;
        sampler.minFilter = VK_FILTER_NEAREST;
        sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler.maxLod = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(device, &sampler, nullptr, &e.sampler) != VK_SUCCESS)
        {
            return false;
        }
        e.encode_sets = make_set_layout(1);
        e.check_sets = make_set_layout(2);
        if (!e.encode_sets || !e.check_sets)
        {
            return false;
        }
        e.encode_layout = make_pipeline_layout(e.encode_sets, sizeof(EncodeParams));
        e.check_layout = make_pipeline_layout(e.check_sets, sizeof(CheckParams));
        return e.encode_layout && e.check_layout;
    }

    // bc7-fast-start: the build begun (render thread): the quality check, then
    // the encoder's pipelines with the forms the settings ask for (bc7-fault,
    // bc7-fault2), a pass or a stage each (the same GLSL as before for the one
    // pipeline and the passes: the same SPIR-V, the same pipeline cache entries).
    void start_build()
    {
        Encoder& e = sEncoder;
        const Settings& s = settings();
        std::lock_guard<std::mutex> stop_lock(sStopMutex);
        if (sStopped)
        {
            return;     // shutting down
        }
        Build* build = new Build();     // never freed (Build)
        build->started = std::chrono::steady_clock::now();
        auto add = [build](const std::string& source, VkPipelineLayout layout, const std::string& what)
        {
            build->pieces.push_back(std::make_unique<Build::Piece>());
            Build::Piece& piece = *build->pieces.back();
            piece.source = source;
            piece.layout = layout;
            piece.what = what;
        };
        if (s.check != CHECK_OFF)
        {
            add(std::string("#version 450\n") + (s.check == CHECK_OLD ? CHECK_SOURCE_OLD : CHECK_SOURCE), e.check_layout, "BC7 quality check");
            build->check_pieces = 1;
        }
        std::string forms = s.rolled ? "#extension GL_EXT_control_flow_attributes : require\n" : "";
        forms += llformat("#define BC7_SAFE %u\n#define BC7_MODES 0x%X\n#define BC7_PASSES %u\n", s.safe, s.modes, s.passes);
        forms += s.px_shared ? "#define BC7_PX_SHARED 1\n" : "";
        forms += s.rolled ? "#define BC7_ROLLED 1\n" : "";
        for (U32 p = 0; p < e.pipeline_count; ++p)
        {
            const std::string own = e.stages ? llformat("#define BC7_STAGE %s\n", STAGE_DEFINES[p])
                                             : llformat("#define BC7_PASS %u\n", e.pipeline_count == 1 ? 0 : p + 1);
            const std::string what = e.stages ? llformat("BC7 encoder, stage %s (%u of %u)", STAGE_DEFINES[p], p + 1, STAGE_COUNT)
                                              : llformat("BC7 encoder, pass %u of %u", p + 1, e.pipeline_count);
            add(std::string("#version 450\n") + forms + own + ENCODER_SOURCE, e.encode_layout, what);
        }
        // LLRHI_TEX_BCN_FAIL: a piece that glslang refuses (its program has no module).
        const size_t fail = s.fail_piece == NO_FAIL ? build->pieces.size()
                          : s.fail_piece == 0 ? (build->check_pieces ? 0 : build->pieces.size())
                                              : build->check_pieces + s.fail_piece - 1;
        if (fail < build->pieces.size())
        {
            build->pieces[fail]->source += "\n#error LLRHI_TEX_BCN_FAIL\n";
            LL_WARNS("TexBCN") << "BC7 (bc7-fast-start): LLRHI_TEX_BCN_FAIL=" << s.fail_piece << ": " << build->pieces[fail]->what
                               << " is made to fail (a test of the failure paths)" << LL_ENDL;
        }
        else if (s.fail_piece != NO_FAIL)
        {
            LL_WARNS("TexBCN") << "BC7 (bc7-fast-start): LLRHI_TEX_BCN_FAIL=" << s.fail_piece << " names no piece of this build" << LL_ENDL;
        }
        // Never destroyed either: stopBuilding drops what has not begun, a
        // thread still compiling is left to the process exit.
        build->workers = new LLVKShader::Workers("VKBC7Build", e.build_threads, true);
        e.build = build;
        sBuildToStop = build;
        const size_t first = build->check_pieces + 1;
        build->workers->post([build, first]() { run_first(*build, first); });
    }

    // bc7-fast-start: more stages encode (the encoder is slower): the GPU's
    // speed is learnt again from START_BLOCKS, the frames timed before are
    // not read (Slot::timed_kernel).
    void restart_pace()
    {
        Pace& p = sPace;
        ++p.kernel;
        p.allowance = START_BLOCKS;
        p.single = SINGLE_START_BLOCKS;
        p.blocks_per_ms = 0.0;
        p.measured = 0;
        p.never = 0;    // not sure again (as frameBudget's Budget::never)
        std::fill(std::begin(p.recent_rates), std::end(p.recent_rates), 0.0);
        p.untimed_frames = 0;
    }

    F64 seconds_since(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<F64>(std::chrono::steady_clock::now() - start).count();
    }

    // The first time the encoder can encode: the settings' line, as before
    // the build went off the frame ("ready").
    void log_ready(F64 seconds)
    {
        const Encoder& e = sEncoder;
        const Settings& s = settings();
        LL_INFOS("TexBCN") << "BC7 texture compression ready (research/88): textures of " << s.min_texels << " texels and up, "
                           << (s.frame_texels >> 20) << " Mtexels a frame; bc7-fault: dispatches of "
                           << (s.chunk_blocks ? std::to_string(s.chunk_blocks) + " blocks" : std::string("a whole level")) << ", "
                           << (s.target_ms > 0.0 ? llformat("%.1f ms of the GPU a frame", s.target_ms) : std::string("not timed")) << " ("
                           << (e.timestamps ? "timestamps" : "no timestamps: grown by itself") << "), held "
                           << s.hold_seconds << " s after the world is shown, encoder forms " << s.safe << " of 15"
                           << llformat("; bc7-fault2: modes 0x%X, kernel %s, check %s", s.modes, kernel_name().c_str(),
                                       s.check == CHECK_OLD ? "old" : s.check == CHECK_OFF ? "off" : "safe")
                           << llformat("; bc7-fast-start: made off the frame in %.1f s", seconds)
                           << (e.stages && e.usable < STAGE_COUNT ? llformat(", encoding with %u of %u stages while the others are made",
                                                                             e.usable, STAGE_COUNT)
                                                                  : std::string())
                           << LL_ENDL;
    }

    // bc7-fast-start: what the compile threads have made since the last call
    // (render thread, or the bake's): their pipelines into the encoder, how
    // many of them encode now, a line in the log as that changes.
    void collect()
    {
        Encoder& e = sEncoder;
        Build* build = e.build;
        if (!build || e.settled)
        {
            return;
        }
        bool settled = true;
        // Held from the look at stop to the end of the loop: a piece is
        // dropped for the shutdown only under this lock (make_piece), so none
        // taken here was (the closing lines below never tell of the shutdown).
        std::unique_lock<std::mutex> lock(build->mutex);
        if (build->stop)
        {
            return;     // shutting down: what was dropped is neither taken nor logged
        }
        for (size_t i = 0; i < build->pieces.size(); ++i)
        {
            if (e.taken[i])
            {
                continue;
            }
            Build::Piece& piece = *build->pieces[i];
            const U32 state = piece.state.load(std::memory_order_acquire);
            if (state == PIECE_WAITING)
            {
                settled = false;
                continue;
            }
            e.taken[i] = true;
            if (state != PIECE_MADE)
            {
                if (state == PIECE_FAILED)
                {
                    LL_WARNS("TexBCN") << piece.what << " could not be made"
                                       << (piece.program.log.empty() ? std::string() : ": " + piece.program.log) << LL_ENDL;
                }
                continue;
            }
            (i < build->check_pieces ? e.check_pipeline : e.encode_pipelines[i - build->check_pieces]) = piece.pipeline;
            log_statistics(piece.pipeline, piece.what.c_str());
        }
        lock.unlock();
        U32 usable = 0;
        while (usable < e.pipeline_count && e.encode_pipelines[usable])
        {
            ++usable;
        }
        // The passes: all of them. The stages: without mode 6 (LLRHI_TEX_BCN_MODES),
        // the first stage alone would give every block its mean colour, kept
        // for good: all of them then too. The self check from the first texture on.
        const bool all_needed = !e.stages || !(settings().modes & (1u << 6));
        if ((all_needed && usable < e.pipeline_count) || (build->check_pieces && !e.check_pipeline))
        {
            usable = 0;
        }
        const F64 seconds = seconds_since(build->started);
        if (usable != e.usable)
        {
            const U32 before = e.usable;
            e.usable = usable;
            if (!before)
            {
                log_ready(seconds);
                LLVKDevice::instance().noteEvent("BC7: the encoder can encode");     // research/109's fault report
            }
            else
            {
                restart_pace();
                LLVKDevice::instance().noteEvent("BC7: more stages of the encoder encode");
                LL_INFOS("TexBCN") << "BC7 encoder (bc7-fast-start): " << usable << " of " << STAGE_COUNT << " stages encode now, "
                                   << llformat("%.1f", seconds) << " s after the build began" << LL_ENDL;
            }
        }
        if (!settled)
        {
            return;
        }
        e.settled = true;
        e.all_made = usable == e.pipeline_count;
        std::string times;
        for (const std::unique_ptr<Build::Piece>& piece : build->pieces)
        {
            times += llformat("%s%s %.1f s", times.empty() ? "" : ", ", piece->what.c_str(), piece->seconds);
        }
        LL_INFOS("TexBCN") << "BC7 encoder (bc7-fast-start): " << (e.all_made ? "made" : "made in part") << " in "
                           << llformat("%.1f", seconds) << " s off the frame (" << times << ")" << LL_ENDL;
        if (!usable)
        {
            e.unavailable = true;
            LL_WARNS("TexBCN") << "BC7 texture compression unavailable (research/88): the encoder's pipelines could not be made; "
                               << "textures stay RGBA8" << LL_ENDL;
        }
        else if (!e.all_made)
        {
            LL_WARNS("TexBCN") << "BC7 encoder (bc7-fast-start): encoding with the first " << usable << " of " << STAGE_COUNT
                               << " stages for this session (the others could not be made)" << LL_ENDL;
        }
    }

    // The first call of prepare, ready or built (render thread, or the
    // bake's): what the device can, the kernel for this GPU, the fault notes,
    // then the build begun; not while encoding is off for the session (a
    // known bad driver, a fault note), unless the shader bake asks.
    void start(bool bake)
    {
        Encoder& e = sEncoder;
        e.tried = true;
        if (!supported() || !make_layouts())
        {
            e.unavailable = true;
            LL_WARNS("TexBCN") << "BC7 texture compression unavailable (research/88): textures stay RGBA8" << LL_ENDL;
            return;
        }
        const Settings& s = settings();
        VkPhysicalDeviceProperties props = {};
        vkGetPhysicalDeviceProperties(LLVKDevice::instance().getPhysicalDevice(), &props);
        e.stages = s.kernel == KERNEL_STAGES || (s.kernel == KERNEL_AUTO && props.vendorID == VENDOR_AMD);
        e.pipeline_count = e.stages ? STAGE_COUNT : s.passes;
        const bool integrated = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        e.build_threads = e.pipeline_count == 1 ? 1 : s.build_threads ? s.build_threads : integrated ? BUILD_THREADS_INTEGRATED : BUILD_THREADS;
        create_timestamps();
        make_fault_key();
        const char* threads_why = e.pipeline_count == 1 ? "" : s.build_threads ? " (LLRHI_TEX_BCN_BUILD_THREADS)" : integrated ? " (integrated GPU)" : "";
        LL_INFOS("TexBCN") << "BC7 encoder (bc7-fast-start): encoder " << ENCODER_REVISION << ", " << e.fault_source << ", kernel " << kernel_name()
                           << (s.kernel != KERNEL_AUTO ? " (LLRHI_TEX_BCN_KERNEL)"
                               : e.stages ? " (auto: AMD, whose compiler took minutes and gigabytes over the one pipeline)"
                                          : " (auto: one pipeline; LLRHI_TEX_BCN_KERNEL=stages for the stages)")
                           << ", made off the frame on " << e.build_threads << " compile thread(s)" << threads_why
                           << (e.stages ? ", the first stage alone first" : "") << LL_ENDL;
        if (!s.rejected.empty())
        {
            LL_WARNS("TexBCN") << "BC7 (bc7-fault): not a number in range, the default kept: " << s.rejected << LL_ENDL;
        }
        read_known_bad();
        if (!e.blocked)
        {
            read_fault_note();
        }
        if (!e.blocked || bake)
        {
            start_build();
        }
    }

    U32 current_slot_index()
    {
        return LLVKMirror::instance().getSlot() % LLVKMirror::FRAMES_IN_FLIGHT;
    }

    Slot& current_slot()
    {
        return sEncoder.slots[current_slot_index()];
    }

    // bc7-fault: the GPU time of the frame's encodings begins (at the first
    // one; the frame's upload commands, outside any rendering).
    void begin_timing(Slot& slot, U32 index, VkCommandBuffer cmd)
    {
        const Encoder& e = sEncoder;
        if (!e.timestamps || slot.timing || slot.timed)
        {
            return;
        }
        vkCmdResetQueryPool(cmd, e.timestamps, index * 2, 2);
        // Written once every command before it has completed: from here only the encodings run.
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, e.timestamps, index * 2);
        slot.timing = true;
        slot.timed_blocks = 0;
        slot.timed_kernel = sPace.kernel;
    }

    // A frame's encodings took ms on the GPU for blocks.
    void note_gpu_time(F64 ms, U64 blocks)
    {
        Pace& p = sPace;
        if (!blocks || ms < 0.0 || ms > 10000.0)
        {
            return;     // not a duration (out of order)
        }
        ++p.measured;
        if (ms > p.longest_ms)
        {
            p.longest_ms = ms;
            p.longest_blocks = blocks;
        }
        // A slower frame counts at once, a faster one by a quarter (the
        // shares: careful). The recent ones are kept for Budget::never.
        const F64 rate = (F64)blocks / llmax(ms, 0.01);
        p.blocks_per_ms = (p.blocks_per_ms <= 0.0 || rate < p.blocks_per_ms) ? rate : 0.75 * p.blocks_per_ms + 0.25 * rate;
        p.recent_rates[(p.measured - 1) % RECENT_RATES] = rate;
        if (p.measured == 1)
        {
            LL_INFOS("TexBCN") << "BC7 GPU time (bc7-fault): " << blocks << " blocks in " << llformat("%.2f", ms) << " ms ("
                               << llformat("%.1f", rate / 1000.0) << " Mblocks/s); a frame encodes about "
                               << settings().target_ms << " ms of them (more while many wait)" << LL_ENDL;
        }
        if (ms >= STALL_MS && !p.stalled)
        {
            p.stalled = true;
            LLVKDevice::instance().noteEvent("BC7 off: a frame's encodings took too long on the GPU");
            LL_WARNS("TexBCN") << "BC7 texture compression off for this session (bc7-fault): a frame's encodings took "
                               << llformat("%.0f", ms) << " ms on the GPU for " << blocks << " blocks (the encoder is far too slow "
                               << "on this GPU or driver); textures stay RGBA8" << LL_ENDL;
        }
    }

    // The slot's last frame has finished: its encodings' GPU time.
    void read_timing(Slot& slot, U32 index)
    {
        const Encoder& e = sEncoder;
        // bc7-fast-start: a frame timed with fewer stages than now is not read.
        const bool timed = slot.timed && slot.timed_kernel == sPace.kernel;
        const U64 blocks = slot.timed_blocks;
        slot.timing = false;
        slot.timed = false;
        slot.timed_blocks = 0;
        if (!timed || !e.timestamps)
        {
            return;
        }
        U64 ticks[2] = {};
        // No wait flag: the slot's fence has passed. A frame that never reached
        // the GPU reports VK_NOT_READY and is skipped.
        if (vkGetQueryPoolResults(LLVKDevice::instance().getDevice(), e.timestamps, index * 2, 2, sizeof(ticks), ticks, sizeof(U64),
                                  VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
        {
            note_gpu_time((F64)((ticks[1] - ticks[0]) & e.timestamp_mask) * e.ms_per_tick, blocks);
        }
    }

    bool create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host, Buffer& out)
    {
        VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = size;
        info.usage = usage;
        VmaAllocationCreateInfo alloc = {};
        alloc.usage = host ? VMA_MEMORY_USAGE_AUTO_PREFER_HOST : VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (host)
        {
            alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        }
        VmaAllocationInfo mapped = {};
        out = Buffer();
        if (vmaCreateBuffer(LLVKDevice::instance().getAllocator(), &info, &alloc, &out.buffer, &out.memory, &mapped) != VK_SUCCESS)
        {
            out = Buffer();
            return false;
        }
        out.size = size;
        out.mapped = mapped.pMappedData;
        if (host && !out.mapped)
        {
            vmaDestroyBuffer(LLVKDevice::instance().getAllocator(), out.buffer, out.memory);
            out = Buffer();
            return false;
        }
        return true;
    }

    // Destroyed once the GPU is done with everything recorded so far.
    void release(Buffer& buffer)
    {
        if (buffer.buffer)
        {
            const VkBuffer handle = buffer.buffer;
            const VmaAllocation memory = buffer.memory;
            LLVKMirror::instance().defer([handle, memory]() { vmaDestroyBuffer(LLVKDevice::instance().getAllocator(), handle, memory); });
        }
        buffer = Buffer();
    }

    // The most a storage buffer descriptor may cover (128 MB on some GPUs):
    // the encoder's blocks must lie within it.
    VkDeviceSize storage_range()
    {
        return LLVKDevice::instance().getLimits().maxStorageBufferRange;
    }

    // bytes of the slot's scratch memory this frame (grown when short: the
    // old buffer stays until the GPU is done with it). Never grown past
    // storage_range() (encode turns down anything larger than that).
    bool take_scratch(Slot& slot, VkDeviceSize bytes, VkDeviceSize& offset)
    {
        if (slot.scratch_head + bytes > slot.scratch.size)
        {
            const VkDeviceSize size = llmin(llmax(llmax(bytes, SCRATCH_MIN_BYTES), slot.scratch.size * 2), llmax(bytes, storage_range()));
            release(slot.scratch);
            if (!create_buffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, slot.scratch))
            {
                return false;
            }
            slot.scratch_head = 0;
        }
        offset = slot.scratch_head;
        slot.scratch_head += bytes;
        return true;
    }

    VkDescriptorSet allocate_set(Slot& slot, VkDescriptorSetLayout layout)
    {
        VkDevice device = LLVKDevice::instance().getDevice();
        if (!slot.pool)
        {
            const VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SETS_PER_FRAME * 2 },
                                                    { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SETS_PER_FRAME } };
            VkDescriptorPoolCreateInfo info = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            info.maxSets = SETS_PER_FRAME;
            info.poolSizeCount = 2;
            info.pPoolSizes = sizes;
            if (vkCreateDescriptorPool(device, &info, nullptr, &slot.pool) != VK_SUCCESS)
            {
                slot.pool = VK_NULL_HANDLE;
                return VK_NULL_HANDLE;
            }
        }
        VkDescriptorSetAllocateInfo alloc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        alloc.descriptorPool = slot.pool;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(device, &alloc, &set) != VK_SUCCESS)
        {
            return VK_NULL_HANDLE;
        }
        ++slot.sets;
        return set;
    }

    void write_sets(VkDescriptorSet set, const VkImageView* views, U32 view_count, const Buffer& buffer)
    {
        VkDescriptorImageInfo images[2] = {};
        VkWriteDescriptorSet writes[3] = {};
        for (U32 i = 0; i < view_count; ++i)
        {
            images[i] = { sEncoder.sampler, views[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo = &images[i];
        }
        // Not VK_WHOLE_SIZE: the buffer may be larger than a storage descriptor
        // may cover (VUID-VkWriteDescriptorSet-descriptorType-00333).
        const VkDescriptorBufferInfo storage = { buffer.buffer, 0, llmin(buffer.size, storage_range()) };
        VkWriteDescriptorSet& w = writes[view_count];
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = view_count;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &storage;
        vkUpdateDescriptorSets(LLVKDevice::instance().getDevice(), view_count + 1, writes, 0, nullptr);
    }

    VkImageView make_view(VkImage image, VkFormat format, U32 levels)
    {
        VkImageViewCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        info.image = image;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = format;
        info.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
        VkImageView view = VK_NULL_HANDLE;
        if (vkCreateImageView(LLVKDevice::instance().getDevice(), &info, nullptr, &view) != VK_SUCCESS)
        {
            return VK_NULL_HANDLE;
        }
        return view;
    }

    void destroy_view_later(VkImageView view)
    {
        if (view)
        {
            LLVKMirror::instance().defer([view]() { vkDestroyImageView(LLVKDevice::instance().getDevice(), view, nullptr); });
        }
    }

    void image_barrier(VkCommandBuffer cmd, VkImage image, U32 levels, VkImageLayout from, VkImageLayout to,
                       VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
    {
        VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = src_stage;
        barrier.srcAccessMask = src_access;
        barrier.dstStageMask = dst_stage;
        barrier.dstAccessMask = dst_access;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1 };
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    void buffer_barrier(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size,
                        VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access)
    {
        VkBufferMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2 };
        barrier.srcStageMask = src_stage;
        barrier.srcAccessMask = src_access;
        barrier.dstStageMask = dst_stage;
        barrier.dstAccessMask = dst_access;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer;
        barrier.offset = offset;
        barrier.size = size;
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    F64 psnr(U64 squared_error, U64 values)
    {
        if (!squared_error || !values)
        {
            return PERFECT_PSNR;
        }
        const F64 mse = (F64)squared_error / (F64)values;
        return llmin(10.0 * std::log10(255.0 * 255.0 / mse), PERFECT_PSNR);
    }

    // Records the check of level 0 of encoded against source (both sampled
    // by compute already), read back when the slot's frame has finished.
    // on_compute: cmd is a compute batch's (research/200): no barrier for the
    // graphics stages there (the compute timeline orders the frames after it).
    void record_check(VkCommandBuffer cmd, Slot& slot, VkImageView source_view, VkImage encoded, U32 width, U32 height, U32 name,
                      bool on_compute)
    {
        Encoder& e = sEncoder;
        const U32 tiles_x = (width + CHECK_TILE - 1) / CHECK_TILE;
        const U32 tiles_y = (height + CHECK_TILE - 1) / CHECK_TILE;
        const VkDeviceSize bytes = (VkDeviceSize)tiles_x * tiles_y * CHECK_WORDS * 4;
        if (!slot.readback.buffer && !create_buffer(READBACK_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, slot.readback))
        {
            return;
        }
        if (slot.readback_head + bytes > slot.readback.size)
        {
            return;
        }
        const VkImageView encoded_view = make_view(encoded, LLVKTexCompress::ENCODED_FORMAT, 1);
        const VkDescriptorSet set = encoded_view ? allocate_set(slot, e.check_sets) : VK_NULL_HANDLE;
        if (!set)
        {
            destroy_view_later(encoded_view);
            return;
        }
        const VkImageView views[2] = { source_view, encoded_view };
        write_sets(set, views, 2, slot.readback);
        Check check;
        check.name = name;
        check.width = width;
        check.height = height;
        check.tiles = tiles_x * tiles_y;
        check.offset = slot.readback_head;
        slot.readback_head += bytes;

        const CheckParams params = { width, height, tiles_x, (U32)(check.offset / 4), tiles_x * tiles_y };
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, e.check_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, e.check_layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, e.check_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
        // bc7-fault2: the safe form takes a tile an invocation, the old one a tile a group.
        if (settings().check == CHECK_OLD)
        {
            vkCmdDispatch(cmd, tiles_x, tiles_y, 1);
        }
        else
        {
            vkCmdDispatch(cmd, (params.tiles + CHECK_GROUP - 1) / CHECK_GROUP, 1, 1);
        }
        buffer_barrier(cmd, slot.readback.buffer, check.offset, bytes, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        // llvkgl's next barrier on the texture waits for draws' shader stages:
        // those wait for this read first, so a layout change after it is ordered too.
        if (!on_compute)
        {
            image_barrier(cmd, encoded, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, 0,
                          VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0);
        }
        destroy_view_later(encoded_view);
        slot.checks.push_back(check);
    }

    // The slot's checks, now that its frame has finished.
    void read_checks(Slot& slot)
    {
        if (slot.checks.empty())
        {
            return;
        }
        vmaInvalidateAllocation(LLVKDevice::instance().getAllocator(), slot.readback.memory, 0, VK_WHOLE_SIZE);
        Quality& q = sQuality;
        for (const Check& check : slot.checks)
        {
            const U32* words = (const U32*)((const U8*)slot.readback.mapped + check.offset);
            U64 sums[4] = {};
            U32 largest = 0;
            for (U32 t = 0; t < check.tiles; ++t)
            {
                for (U32 c = 0; c < 4; ++c)
                {
                    sums[c] += words[t * CHECK_WORDS + c];
                }
                largest = llmax(largest, words[t * CHECK_WORDS + 4]);
            }
            const U64 texels = (U64)check.width * check.height;
            const F64 rgb = psnr(sums[0] + sums[1] + sums[2], texels * 3);
            const F64 alpha = psnr(sums[3], texels);
            ++q.checked;
            q.rgb_sum += rgb;
            q.alpha_sum += alpha;
            q.rgb_lowest = llmin(q.rgb_lowest, rgb);
            q.alpha_lowest = llmin(q.alpha_lowest, alpha);
            q.largest_error = llmax(q.largest_error, largest);

            const bool self_check = q.self_checked < SELF_CHECK_TEXTURES;
            if (self_check || (rgb < LOW_PSNR && q.low_logged < LOW_PSNR_LOGS))
            {
                q.low_logged += self_check ? 0 : 1;
                LL_INFOS("TexBCN") << "BC7 check (research/88): texture " << check.name << ", " << check.width << "x" << check.height
                                   << ", PSNR RGB " << llformat("%.1f", rgb) << " dB, alpha " << llformat("%.1f", alpha)
                                   << " dB, largest error " << largest << (self_check ? " (self check)" : "") << LL_ENDL;
            }
            if (!self_check)
            {
                continue;
            }
            ++q.self_checked;
            q.self_rgb_sum += rgb;
            // (The tint changes the colours on purpose: no verdict then.)
            if (q.self_checked == SELF_CHECK_TEXTURES && !settings().tint &&
                q.self_rgb_sum / SELF_CHECK_TEXTURES < SELF_CHECK_MIN_PSNR)
            {
                sEncoder.failed = true;
                LL_WARNS("TexBCN") << "BC7 self check failed (research/88): average PSNR "
                                   << llformat("%.1f", q.self_rgb_sum / SELF_CHECK_TEXTURES)
                                   << " dB over the first textures; texture compression off for this session" << LL_ENDL;
            }
        }
        slot.checks.clear();
    }

    void release_idle(Slot& slot)
    {
        const F64 idle = std::chrono::duration<F64>(std::chrono::steady_clock::now() - slot.last_used).count();
        if ((slot.scratch.buffer || slot.readback.buffer || slot.verify.buffer) && idle > IDLE_RELEASE_SECONDS)
        {
            release(slot.scratch);
            release(slot.readback);
            release(slot.verify);
            slot.scratch_head = 0;
            slot.readback_head = 0;
            slot.verify_head = 0;
        }
    }

    // ---- research/200: the compute queue ----

    // VERIFY: one side of a texture encoded both ways is done (missing: its
    // blocks could not be copied or were never written). Compared once both are.
    void note_hash(U64 serial, U32 side, U64 hash, bool missing)
    {
        AsyncState& a = sAsync;
        auto it = std::find_if(a.pairs.begin(), a.pairs.end(), [serial](const AsyncState::Pair& p) { return p.serial == serial; });
        if (it == a.pairs.end())
        {
            return;     // dropped
        }
        it->hash[side] = hash;
        it->have |= (U8)(1u << side);
        it->missing = it->missing || missing;
        if (it->have != 3)
        {
            return;
        }
        const AsyncState::Pair pair = *it;
        a.pairs.erase(it);
        if (pair.missing)
        {
            return;
        }
        ++a.compared;
        if (pair.hash[SIDE_FRAME] != pair.hash[SIDE_ASYNC])
        {
            ++a.mismatched;
            if (a.mismatched <= MISMATCH_LOGS)
            {
                LL_WARNS("TexBCN") << "async BC7 (research/200): texture " << pair.name
                                   << " encoded on the compute queue differs from its encoding in the frame (" << a.mismatched << " of "
                                   << a.compared << " compared)" << LL_ENDL;
            }
        }
        if (LLVKQueues::instance().mode() == LLVKQueues::MODE_AUTO && a.decision == AsyncState::UNDECIDED)
        {
            if (a.mismatched)
            {
                a.decision = AsyncState::FAILED;
                LL_WARNS("TexBCN") << "async BC7 (research/200, auto): a mismatch; textures are encoded in the frame for the rest of the session"
                                   << LL_ENDL;
            }
            else if (a.compared >= VERIFY_AUTO_TEXTURES)
            {
                a.decision = AsyncState::PASSED;
                LL_INFOS("TexBCN") << "async BC7 (research/200, auto): " << a.compared
                                   << " textures encoded the same both ways; encoding on the compute queue from now on" << LL_ENDL;
            }
        }
    }

    // The slot's copies of blocks, now that its commands have completed
    // (written false: they never ran).
    void read_verify(Slot& slot, U32 side, bool written)
    {
        if (slot.verify_parts.empty())
        {
            return;
        }
        const bool readable = written && slot.verify.mapped;
        if (readable)
        {
            vmaInvalidateAllocation(LLVKDevice::instance().getAllocator(), slot.verify.memory, 0, VK_WHOLE_SIZE);
        }
        for (const VerifyPart& part : slot.verify_parts)
        {
            // XXH3 (several GB/s: a large texture's blocks in well under a millisecond).
            note_hash(part.serial, side, readable ? HBXXH64::digest((const U8*)slot.verify.mapped + part.offset, (size_t)part.bytes) : 0,
                      !readable);
        }
        slot.verify_parts.clear();
    }

    // A compute slot whose batch completed (written) or never ran: its checks
    // and copies read, its descriptor sets and memory free for the next batch.
    void retire_async(Slot& slot, bool written)
    {
        if (written)
        {
            read_checks(slot);
        }
        else
        {
            slot.checks.clear();
        }
        read_verify(slot, SIDE_ASYNC, written);
        if (slot.pool)
        {
            vkResetDescriptorPool(LLVKDevice::instance().getDevice(), slot.pool, 0);
        }
        slot.sets = 0;
        slot.scratch_head = 0;
        slot.readback_head = 0;
        slot.verify_head = 0;
        slot.pending = false;
        slot.value = 0;
    }

    // Start of a frame: the compute slots whose batches have completed.
    void poll_async()
    {
        Encoder& e = sEncoder;
        bool pending = false;
        for (const Slot& slot : e.async_slots)
        {
            pending = pending || slot.pending;
        }
        const U64 done = pending ? LLVKQueues::instance().computeCompleted() : 0;
        for (U32 i = 0; i < LLVKQueues::COMPUTE_RING; ++i)
        {
            Slot& slot = e.async_slots[i];
            if (e.batch_open && e.batch.index == i)
            {
                continue;
            }
            if (slot.pending && slot.value && slot.value <= done)
            {
                retire_async(slot, true);
            }
            if (!slot.pending)
            {
                release_idle(slot);
            }
        }
    }

    // This frame's compute batch and its slot, begun at the first call.
    // Null: asyncMode() is OFF, or no batch is free (none waited for).
    Slot* open_batch()
    {
        Encoder& e = sEncoder;
        if (!e.batch_open)
        {
            if (LLVKTexCompress::asyncMode() == LLVKTexCompress::Async::OFF || !LLVKTexCompress::ready() ||
                !LLVKQueues::instance().beginCompute(e.batch))
            {
                return nullptr;
            }
            e.batch_open = true;
            Slot& slot = e.async_slots[e.batch.index];
            if (slot.pending)
            {
                // beginCompute handed the index out again: the slot's last batch has completed.
                retire_async(slot, slot.value != 0);
            }
        }
        return &e.async_slots[e.batch.index];
    }

    // encode (on_compute false: the frame's commands, the frame's slot) and
    // encodeAsync (on_compute: a compute batch and its slot). verify: a
    // VERIFY pair's number (0: none); an encoding on the compute queue with
    // one is VERIFY's second encoding of the texture, not counted or checked.
    bool encode_into(Slot& slot, VkCommandBuffer cmd, bool on_compute, VkImage source, U32 width, U32 height, U32 levels, U32 name,
                     LLVKTexCompress::Encoded& out, U64 verify)
    {
        Encoder& e = sEncoder;
        if (!cmd || !source || !width || !height || !levels || levels > MAX_LEVELS || !LLVKTexCompress::ready() ||
            slot.sets + 2 > SETS_PER_FRAME)
        {
            return false;
        }
        const bool second = on_compute && verify;

        // The blocks of each level, one level after the other in the scratch memory.
        U32 first_block[MAX_LEVELS];
        const U32 total_blocks = level_layout(width, height, levels, first_block);
        const VkDeviceSize bytes = (VkDeviceSize)total_blocks * BLOCK_BYTES;
        // bc7-fault2, bc7-fast-start: the encoder in passes or stages keeps an
        // aux block a block after the blocks (what is between them; the first
        // stage alone writes them too); in one pipeline none.
        const bool aux = e.stages || e.pipeline_count > 1;
        const VkDeviceSize scratch_bytes = aux ? bytes * 2 : bytes;
        // The blocks must lie within what one storage descriptor covers: larger
        // stays plain (not split into parts).
        if (scratch_bytes > storage_range())
        {
            LL_WARNS_ONCE("TexBCN") << "texture too large for the encoder's storage range, left uncompressed" << LL_ENDL;
            return false;
        }
        VkDeviceSize offset = 0;
        if (!take_scratch(slot, scratch_bytes, offset) || offset + scratch_bytes > storage_range())
        {
            return false;
        }

        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = LLVKTexCompress::ENCODED_FORMAT;
        info.extent = { width, height, 1 };
        info.mipLevels = levels;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        // Transfer source: readbacks and decoding blit from it.
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        // research/200: written on the compute queue, sampled by the graphics
        // queue: shared by both (block compressed: nothing to lose by it).
        if (on_compute && !LLVKQueues::instance().shareWithCompute(info))
        {
            return false;
        }
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        if (LLVKDevice::instance().createImage(info, image, memory) != VK_SUCCESS)
        {
            return false;
        }
        const VkImageView source_view = make_view(source, VK_FORMAT_R8G8B8A8_UNORM, levels);
        const VkDescriptorSet set = source_view ? allocate_set(slot, e.encode_sets) : VK_NULL_HANDLE;
        if (!set)
        {
            destroy_view_later(source_view);
            LLVKDevice::instance().forgetImage(memory);
            vmaDestroyImage(LLVKDevice::instance().getAllocator(), image, memory);
            return false;
        }
        write_sets(set, &source_view, 1, slot.scratch);

        // bc7-fault: in the frame, the encodings' GPU time begins with the
        // first, and a checkpoint (LLRHI_VK_CHECKPOINTS) names each in a fault report.
        if (!on_compute)
        {
            begin_timing(slot, current_slot_index(), cmd);
            LLVKDevice::instance().checkpoint(cmd, ((U64)LLVKTexCompress::CHECKPOINT_BEGIN << 32) | name);
        }

        // The source's earlier writes (uploads, mips: earlier frames) before the
        // encoder reads it. On the compute queue the batch's wait for the
        // graphics timeline orders them (research/200 H1); this then orders
        // nothing more, and is no layout change.
        image_barrier(cmd, source, levels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                      VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        VkBufferImageCopy regions[MAX_LEVELS] = {};     // no allocation per texture (research/200 5)
        EncodeParams level_params[MAX_LEVELS];
        for (U32 level = 0; level < levels; ++level)
        {
            const U32 w = level_size(width, level), h = level_size(height, level);
            EncodeParams& params = level_params[level];
            params.level = level;
            params.width = w;
            params.height = h;
            params.blocks_x = blocks(w);
            params.block_count = blocks(w) * blocks(h);
            params.first_block = (U32)(offset / BLOCK_BYTES) + first_block[level];
            params.flags = settings().tint ? FLAG_TINT : 0;
            params.base = 0;
            params.aux_block = aux ? (U32)((offset + bytes) / BLOCK_BYTES) + first_block[level] : 0;

            VkBufferImageCopy& region = regions[level];
            region.bufferOffset = offset + (VkDeviceSize)first_block[level] * BLOCK_BYTES;
            region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
            region.imageExtent = { w, h, 1 };
        }
        // A run: a pipeline over every level (bc7-fault2: the one pipeline, or
        // the encoder in two or three passes; bc7-fast-start: the stages that
        // encode now, the search a group a block, the stages after it twice,
        // a shape a run), a barrier between runs; within a run, dispatches of
        // at most chunk blocks each (bc7-fault; no barrier between them: they
        // write blocks of their own), and of no more groups than the GPU takes.
        const U32 max_groups = LLVKDevice::instance().getLimits().maxComputeWorkGroupCount[0];
        U32 run = 0;
        for (U32 p = 0; p < e.usable; ++p)
        {
            const bool by_block = e.stages && p == SEARCH_STAGE;
            const U32 times = e.stages && p > SEARCH_STAGE ? 2 : 1;
            for (U32 t = 0; t < times; ++t, ++run)
            {
                if (run)
                {
                    buffer_barrier(cmd, slot.scratch.buffer, offset, scratch_bytes, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                }
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, e.encode_pipelines[p]);
                if (!run)
                {
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, e.encode_layout, 0, 1, &set, 0, nullptr);
                }
                for (U32 level = 0; level < levels; ++level)
                {
                    EncodeParams params = level_params[level];
                    params.flags |= t ? FLAG_SECOND_SHAPE : 0;
                    const U32 most = by_block ? max_groups : (U32)llmin((U64)max_groups * ENCODE_GROUP, (U64)0xffffffffu);
                    const U32 chunk = llmin(settings().chunk_blocks ? settings().chunk_blocks : params.block_count, most);
                    for (U32 base = 0; base < params.block_count; base += chunk)
                    {
                        params.base = base;
                        vkCmdPushConstants(cmd, e.encode_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
                        const U32 count = llmin(chunk, params.block_count - base);
                        vkCmdDispatch(cmd, by_block ? count : (count + ENCODE_GROUP - 1) / ENCODE_GROUP, 1, 1);
                    }
                }
            }
        }

        // Blocks into the image, then it is sampled like any texture. On the
        // compute queue the graphics stages are not this queue's: the image is
        // left for the compute stage (the check), and the graphics queue's
        // submissions wait for the compute timeline before they sample it
        // (research/200 H5, H6).
        buffer_barrier(cmd, slot.scratch.buffer, offset, bytes, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        image_barrier(cmd, image, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdCopyBufferToImage(cmd, slot.scratch.buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, regions);
        image_barrier(cmd, image, levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                      on_compute ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : SAMPLED_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

        // research/200 VERIFY: the blocks copied for the host, compared with
        // the other side's once both are done.
        if (verify)
        {
            const U32 side = on_compute ? SIDE_ASYNC : SIDE_FRAME;
            if ((slot.verify.buffer || create_buffer(VERIFY_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, slot.verify)) &&
                slot.verify_head + bytes <= slot.verify.size)
            {
                // After the barrier above: the scratch's writes are visible to copies.
                const VkBufferCopy copy = { offset, slot.verify_head, bytes };
                vkCmdCopyBuffer(cmd, slot.scratch.buffer, slot.verify.buffer, 1, &copy);
                buffer_barrier(cmd, slot.verify.buffer, slot.verify_head, bytes, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
                slot.verify_parts.push_back({ verify, slot.verify_head, bytes });
                slot.verify_head += bytes;
            }
            else
            {
                note_hash(verify, side, 0, true);
            }
        }

        const bool check = !second && settings().check != CHECK_OFF && (sQuality.self_checked < SELF_CHECK_TEXTURES || settings().check_all) &&
                           slot.checks.size() < CHECKS_PER_FRAME;
        if (check)
        {
            record_check(cmd, slot, source_view, image, width, height, name, on_compute);
        }
        destroy_view_later(source_view);

        if (!on_compute)
        {
            LLVKDevice::instance().checkpoint(cmd, ((U64)LLVKTexCompress::CHECKPOINT_END << 32) | name);
            slot.on_gpu = true;
            slot.timed_blocks += slot.timing ? total_blocks : 0;
        }
        slot.last_used = std::chrono::steady_clock::now();
        if (!second)
        {
            ++sCounters.encoded_total;
            sCounters.texels_total += (U64)width * height;
            sPace.frame_blocks += total_blocks;
        }
        if (!sPace.first_noted)
        {
            sPace.first_noted = true;
            LLVKDevice::instance().noteEvent("BC7: the first encodings of the session");     // research/109's fault report
        }
        out.image = image;
        out.memory = memory;
        return true;
    }
}

bool LLVKTexCompress::ready()
{
    Encoder& e = sEncoder;
    if (!e.tried)
    {
        start(false);
    }
    return e.usable && !e.unavailable && !e.failed && !e.blocked && !sPace.stalled;
}

bool LLVKTexCompress::prepare()
{
    Encoder& e = sEncoder;
    if (!e.tried)
    {
        start(false);
    }
    collect();
    return e.build && !e.usable && !e.settled && !e.failed && !e.blocked && !sPace.stalled;
}

bool LLVKTexCompress::built()
{
    Encoder& e = sEncoder;
    if (!e.tried)
    {
        start(true);
    }
    else if (!e.build && !e.unavailable)
    {
        start_build();      // off for the session (start left it): made all the same
    }
    if (!e.build)
    {
        return false;
    }
    {
        Foreground waited_for(*e.build->workers);
        e.build->workers->wait();
    }
    collect();
    return e.all_made;
}

void LLVKTexCompress::stopBuilding()
{
    Build* build = nullptr;
    {
        std::lock_guard<std::mutex> stop_lock(sStopMutex);
        sStopped = true;
        build = sBuildToStop;
    }
    if (!build)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(build->mutex);
        build->stop = true;     // a piece that begins from now on fails at once
    }
    build->workers->cancel();
    Foreground waited_for(*build->workers);
    std::unique_lock<std::mutex> lock(build->mutex);
    if (!build->left_glslang.wait_for(lock, std::chrono::seconds(STOP_WAIT_SECONDS), [build]() { return build->in_glslang == 0; }))
    {
        LL_WARNS("TexBCN") << "BC7 encoder (bc7-fast-start): a compile thread was still in glslang at shutdown" << LL_ENDL;
    }
}

U64 LLVKTexCompress::minTexels()
{
    return settings().min_texels;
}

void LLVKTexCompress::beginFrame()
{
    Encoder& e = sEncoder;
    LLVKMirror& mirror = LLVKMirror::instance();
    if (!e.usable || !mirror.isRecording())
    {
        return;     // (nothing encoded before the encoder could: the slots are unused)
    }
    Slot& slot = current_slot();
    if (slot.begins == mirror.frameBegins())
    {
        return;
    }
    // The GPU has finished the slot's last frame (beginFrame waited for it).
    slot.begins = mirror.frameBegins();
    slot.on_gpu = false;
    read_timing(slot, current_slot_index());
    read_checks(slot);
    read_verify(slot, SIDE_FRAME, true);
    if (slot.pool)
    {
        vkResetDescriptorPool(LLVKDevice::instance().getDevice(), slot.pool, 0);
    }
    slot.sets = 0;
    slot.scratch_head = 0;
    slot.readback_head = 0;
    slot.verify_head = 0;
    release_idle(slot);
    // research/200: the compute batches that completed.
    sAsync.frame_serials = 0;
    poll_async();
}

LLVKTexCompress::Budget LLVKTexCompress::frameBudget(U64 queued_bytes)
{
    const Settings& s = settings();
    Pace& p = sPace;
    p.frame_blocks = 0;
    p.held = false;
    // The hold: the first textures of a login come while the world is first
    // drawn (shaders, pipelines, uploads). None is encoded before the world
    // is shown (LLRHI::sWorldShown: the main view's G-buffer pass after the
    // login completed), nor in the seconds and frames after that.
    // LLRHI_TEX_BCN_HOLD=0 leaves out both: the world is not waited for
    // either (encoding as soon as textures wait, as before bc7-fault).
    if (s.hold_seconds > 0.0)
    {
        if (!LLRHI::sWorldShown)
        {
            p.held = true;
            return Budget();
        }
        const U64 frame = LLVKMirror::instance().frameBegins();
        const auto now = std::chrono::steady_clock::now();
        if (!p.waiting)
        {
            p.waiting = true;
            p.since = now;
            p.since_frame = frame;
        }
        if (std::chrono::duration<F64>(now - p.since).count() < s.hold_seconds || frame < p.since_frame + HOLD_FRAMES)
        {
            p.held = true;
            return Budget();
        }
    }
    const U64 boost = llmin((U64)1 + queued_bytes / QUEUE_BOOST_BYTES, MAX_BOOST);
    Budget budget;
    budget.texels = s.frame_texels * boost;
    if (s.target_ms <= 0.0)
    {
        budget.blocks = budget.single = MAX_BLOCKS;     // not timed: the texels alone, as before
        return budget;
    }
    if (p.blocks_per_ms > 0.0)
    {
        // The GPU's speed known: the frame's time, a texture alone up to SINGLE_MS.
        const U64 time_share = (U64)llmin(p.blocks_per_ms * s.target_ms * (F64)boost, (F64)MAX_BLOCKS);
        p.allowance = llclamp(p.allowance, MIN_BLOCKS, llmax(time_share, MIN_BLOCKS));
        p.single = llmax(p.allowance, (U64)llmin(p.blocks_per_ms * SINGLE_MS, (F64)MAX_BLOCKS));
    }
    budget.blocks = p.allowance;
    budget.single = llmax(p.single, p.allowance);
    if (p.measured >= SURE_MEASURES)
    {
        F64 fastest = 0.0;
        for (U32 i = 0; i < llmin(p.measured, RECENT_RATES); ++i)
        {
            fastest = llmax(fastest, p.recent_rates[i]);
        }
        budget.never = llmax(budget.single, (U64)llmin(fastest * SINGLE_MS, (F64)MAX_BLOCKS));
        p.never = budget.never;     // mayEncode
    }
    return budget;
}

void LLVKTexCompress::noteLeftPlain()
{
    ++sCounters.left_plain;
}

bool LLVKTexCompress::mayEncode(U32 width, U32 height, U32 levels)
{
    const Encoder& e = sEncoder;
    if (e.unavailable || e.failed || e.blocked || sPace.stalled)
    {
        return false;
    }
    const U64 never = sPace.never;
    return !never || blocksFor(width, height, levels) <= never;
}

U64 LLVKTexCompress::blocksFor(U32 width, U32 height, U32 levels)
{
    return level_layout(width, height, levels, nullptr);
}

void LLVKTexCompress::endFrameEncodes(VkCommandBuffer cmd, bool too_large)
{
    Encoder& e = sEncoder;
    Pace& p = sPace;
    Slot& slot = current_slot();
    if (slot.timing && cmd)
    {
        vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, e.timestamps, current_slot_index() * 2 + 1);
        slot.timing = false;
        slot.timed = true;
    }
    if (p.held || settings().target_ms <= 0.0)
    {
        return;
    }
    if (!p.frame_blocks)
    {
        // Nothing encoded: when a texture waited for being too large for one
        // alone so far, that grows (before the speed is known; after, the speed sets it).
        if (too_large && p.blocks_per_ms <= 0.0)
        {
            p.single = llmin(p.single * 2, MAX_BLOCKS);
        }
        return;
    }
    // The share grows by doubling: once the frames are timed, after the
    // first time is known (frameBudget then holds it to the GPU's speed);
    // untimed (no timestamps, the compute queue does them all, or no time
    // came back for UNTIMED_FRAMES frames of encodings), by itself.
    const bool timed = e.timestamps && asyncMode() != Async::ON;
    if (timed && p.blocks_per_ms <= 0.0 && p.untimed_frames < UNTIMED_FRAMES)
    {
        ++p.untimed_frames;
    }
    else
    {
        p.allowance = llmin(p.allowance * 2, MAX_BLOCKS);
        p.single = llmax(p.single, p.allowance);
    }
}

void LLVKTexCompress::noteDeviceLost(const std::vector<U64>& markers, const std::vector<U32>& stages)
{
    const Encoder& e = sEncoder;
    // The graphics queue: the frames' encodings not known to be done, or,
    // with checkpoints (the graphics queue's), the last command the GPU
    // finished (BOTTOM_OF_PIPE) is the start of an encoding.
    bool graphics = false;
    for (const Slot& slot : e.slots)
    {
        graphics = graphics || slot.on_gpu;
    }
    bool checked = false;
    for (size_t i = 0; i < markers.size() && i < stages.size(); ++i)
    {
        if (stages[i] == (U32)VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT)
        {
            checked = true;
            graphics = (U32)(markers[i] >> 32) == CHECKPOINT_BEGIN;
        }
    }
    // The compute queue (research/200): its batches not known to be done (no checkpoints there).
    bool compute = false;
    for (const Slot& slot : e.async_slots)
    {
        compute = compute || slot.pending;
    }
    const bool on_gpu = graphics || compute;
    const char* how = graphics ? (checked ? " (checkpoints)" : "") : " (the compute queue)";
    const std::string path = fault_path();
    if (!on_gpu || e.fault_gpu.empty() || path.empty())
    {
        return;
    }
    // This GPU's line for these forms in place of its last one (its lines
    // for other forms stay: read_fault_note); the oldest go past FAULT_NOTE_LINES.
    const std::string mine = fault_line_mine();
    std::vector<std::string> lines = read_fault_lines(path);
    lines.erase(std::remove_if(lines.begin(), lines.end(),
                               [&mine](const std::string& line) { return without_source(line).compare(0, mine.size(), mine) == 0; }),
                lines.end());
    while (lines.size() + 1 > FAULT_NOTE_LINES)
    {
        lines.erase(lines.begin());
    }
    const std::string line = fault_line_now();
    lines.push_back(line);
    if (!write_fault_lines(path, lines))
    {
        LL_WARNS("TexBCN") << "BC7 (bc7-fault): the GPU was lost with texture encodings on it" << how
                           << "; the note could not be written (" << path << ")" << LL_ENDL;
        return;
    }
    LL_WARNS("TexBCN") << "BC7 (bc7-fault): the GPU was lost with texture encodings on it" << how
                       << "; the next start on this GPU and driver leaves textures plain (noted in " << path << ": " << line
                       << "; LLRHI_TEX_BCN_RETRY=1 tries again)" << LL_ENDL;
}

bool LLVKTexCompress::hasRoom()
{
    return current_slot().sets + 2 <= SETS_PER_FRAME;
}

bool LLVKTexCompress::encode(VkCommandBuffer cmd, VkImage source, U32 width, U32 height, U32 levels, U32 name, Encoded& out, U64 verify)
{
    return encode_into(current_slot(), cmd, false, source, width, height, levels, name, out, verify);
}

LLVKTexCompress::Async LLVKTexCompress::asyncMode()
{
    const LLVKQueues& queues = LLVKQueues::instance();
    if (!queues.computeUsable())
    {
        return Async::OFF;
    }
    switch (queues.mode())
    {
    case LLVKQueues::MODE_VERIFY:
        return Async::VERIFY;
    case LLVKQueues::MODE_AUTO:
        return sAsync.decision == AsyncState::PASSED ? Async::ON : sAsync.decision == AsyncState::FAILED ? Async::OFF : Async::VERIFY;
    case LLVKQueues::MODE_ON:
        return Async::ON;
    default:
        return Async::OFF;
    }
}

bool LLVKTexCompress::asyncWanted()
{
    return asyncMode() != Async::OFF;
}

bool LLVKTexCompress::asyncRoom()
{
    const Slot* slot = open_batch();
    return slot && slot->sets + 2 <= SETS_PER_FRAME;
}

bool LLVKTexCompress::encodeAsync(VkImage source, U32 width, U32 height, U32 levels, U32 name, Encoded& out, U64 verify)
{
    Encoder& e = sEncoder;
    Slot* slot = e.batch_open ? &e.async_slots[e.batch.index] : nullptr;
    if (!slot || !encode_into(*slot, e.batch.cmd, true, source, width, height, levels, name, out, verify))
    {
        return false;
    }
    slot->pending = true;
    ++sAsync.encoded;
    return true;
}

U64 LLVKTexCompress::submitAsync()
{
    Encoder& e = sEncoder;
    if (!e.batch_open)
    {
        return 0;
    }
    e.batch_open = false;
    Slot& slot = e.async_slots[e.batch.index];
    if (!slot.pending)
    {
        LLVKQueues::instance().abandonCompute(e.batch);     // begun, nothing recorded
        return 0;
    }
    slot.value = LLVKQueues::instance().submitCompute(e.batch);
    if (!slot.value)
    {
        // Nothing of it ran: its copies are missing, its sets and memory free again.
        ++sAsync.failed;
        retire_async(slot, false);
    }
    return slot.value;
}

U64 LLVKTexCompress::verifySerial(U32 name)
{
    AsyncState& a = sAsync;
    if (asyncMode() != Async::VERIFY || a.frame_serials >= VERIFY_PER_FRAME || a.pairs.size() >= VERIFY_PAIRS)
    {
        return 0;
    }
    ++a.frame_serials;
    AsyncState::Pair pair;
    pair.serial = a.next_serial++;
    pair.name = name;
    a.pairs.push_back(pair);
    return pair.serial;
}

void LLVKTexCompress::dropVerify(U64 serial)
{
    AsyncState& a = sAsync;
    a.pairs.erase(std::remove_if(a.pairs.begin(), a.pairs.end(), [serial](const AsyncState::Pair& p) { return p.serial == serial; }),
                  a.pairs.end());
}

void LLVKTexCompress::noteAsyncResult(bool used)
{
    ++(used ? sAsync.used : sAsync.dropped);
}

void LLVKTexCompress::decode(VkCommandBuffer cmd, VkImage encoded, VkImage plain, U32 width, U32 height, U32 levels)
{
    image_barrier(cmd, encoded, levels, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  SAMPLED_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    image_barrier(cmd, plain, levels, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    // At most MAX_LEVELS (encode makes no more): no allocation per texture (research/200 5).
    VkImageBlit blits[MAX_LEVELS] = {};
    const U32 count = llmin(levels, MAX_LEVELS);
    for (U32 level = 0; level < count; ++level)
    {
        const S32 w = (S32)level_size(width, level), h = (S32)level_size(height, level);
        VkImageBlit& blit = blits[level];
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
        blit.srcOffsets[1] = { w, h, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
        blit.dstOffsets[1] = { w, h, 1 };
    }
    vkCmdBlitImage(cmd, encoded, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, plain, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   count, blits, VK_FILTER_NEAREST);
    image_barrier(cmd, encoded, levels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_BLIT_BIT, 0, SAMPLED_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    image_barrier(cmd, plain, levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, SAMPLED_STAGES, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void LLVKTexCompress::noteCompressed(U64 plain_bytes, U64 encoded_bytes)
{
    ++sCounters.textures;
    sCounters.plain_bytes += plain_bytes;
    sCounters.encoded_bytes += encoded_bytes;
}

void LLVKTexCompress::noteReleased(U64 plain_bytes, U64 encoded_bytes)
{
    Counters& c = sCounters;
    c.textures -= llmin(c.textures, 1u);
    c.plain_bytes -= llmin(c.plain_bytes, plain_bytes);
    c.encoded_bytes -= llmin(c.encoded_bytes, encoded_bytes);
}

void LLVKTexCompress::noteQueue(U32 textures, U64 plain_bytes)
{
    sCounters.queued = textures;
    sCounters.queued_bytes = plain_bytes;
}

U64 LLVKTexCompress::encodedTotal()
{
    return sCounters.encoded_total;
}

std::string LLVKTexCompress::report()
{
    const Encoder& e = sEncoder;
    const Counters& c = sCounters;
    const Quality& q = sQuality;
    std::ostringstream out;
    out << "block compressed (BC7, research/88): ";
    if (e.unavailable)
    {
        out << "unavailable on this device";
        return out.str();
    }
    out << c.textures << " textures, " << llformat("%.0f", c.encoded_bytes / MB) << " MB for " << llformat("%.0f", c.plain_bytes / MB)
        << " MB plain (saves " << llformat("%.0f", (c.plain_bytes - llmin(c.plain_bytes, c.encoded_bytes)) / MB) << " MB); waiting "
        << c.queued << " (" << llformat("%.0f", c.queued_bytes / MB) << " MB); encoded " << c.encoded_total << " ("
        << llformat("%.0f", c.texels_total / MB) << " Mtexels) since start";
    // bc7-fast-start: the encoder while its compile threads make it.
    if (e.build && !e.settled)
    {
        U32 taken = 0;
        for (size_t i = 0; i < e.build->pieces.size(); ++i)
        {
            taken += e.taken[i] ? 1 : 0;
        }
        out << "; encoder being made (" << taken << " of " << e.build->pieces.size() << " pipelines, "
            << llformat("%.0f", seconds_since(e.build->started)) << " s, kernel " << kernel_name() << ")";
    }
    if (e.stages && e.usable && e.usable < STAGE_COUNT)
    {
        out << "; encoding with " << e.usable << " of " << STAGE_COUNT << " stages";
    }
    if (q.checked)
    {
        out << "; checked " << q.checked << ": PSNR RGB " << llformat("%.1f", q.rgb_sum / q.checked) << " dB (lowest "
            << llformat("%.1f", q.rgb_lowest) << "), alpha " << llformat("%.1f", q.alpha_sum / q.checked) << " dB (lowest "
            << llformat("%.1f", q.alpha_lowest) << "), largest error " << q.largest_error;
    }
    if (e.failed)
    {
        out << "; OFF: the self check failed";
    }
    // bc7-fault: the GPU time of the frames' encodings, and why it is off.
    const Pace& p = sPace;
    if (e.blocked)
    {
        out << "; OFF: " << e.blocked_why;
    }
    if (p.stalled)
    {
        out << "; OFF: a frame's encodings took " << llformat("%.0f", p.longest_ms) << " ms on the GPU";
    }
    else if (p.measured)
    {
        out << "; GPU " << llformat("%.1f", p.blocks_per_ms / 1000.0) << " Mblocks/s, up to " << p.allowance
            << " blocks a frame, longest frame " << llformat("%.1f", p.longest_ms) << " ms (" << p.longest_blocks << " blocks)";
    }
    if (c.left_plain)
    {
        out << "; left plain " << c.left_plain << " (too large for the GPU's speed)";
    }
    // research/200: the compute queue's share, while it is in use.
    LLVKQueues& queues = LLVKQueues::instance();
    if (queues.active())
    {
        const AsyncState& a = sAsync;
        const Async mode = asyncMode();
        out << "; compute queue [" << (mode == Async::ON ? "on" : mode == Async::VERIFY ? "verify" : "off") << ", encoded " << a.encoded
            << ", used " << a.used << ", dropped " << a.dropped << ", compared " << a.compared << ", mismatched " << a.mismatched;
        if (a.failed)
        {
            out << ", failed batches " << a.failed;
        }
        out << "]; " << queues.report();
    }
    return out.str();
}
