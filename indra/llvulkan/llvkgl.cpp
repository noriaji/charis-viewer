/**
 * @file llvkgl.cpp
 * @brief The GL subset the viewer uses, executed on Vulkan (state tracker + draw translation): LLVKGL (install, the reports) and the parts' shared core; the parts are the llvkgl_*.cpp files (llvkgl_internal.h lists them).
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

#include "linden_common.h"

#include "llvkgl_internal.h"
#include "llvkgl_drawpath.h"
#include "llrhidrawlist.h"
#include "llrhihash.h"
#include "llvkdrawlist.h"
#include "llvkresource.h"
#include "llrhiimmediate.h"   // research/137 N9: its VKPerf part
#include "llrhipass.h"        // research/156 N12c: sBindTarget
#include "llvkimmediate.h"

#if LL_GL_FUNC_POINTER

namespace llvkgl
{
    // Every wrapper body runs under this lock: GL objects are shared between
    // the render thread and the texture threads, and all Vulkan recording
    // goes into one frame command buffer.
    LLVKTrackerMutex sLock;

    // Vulkan alone renders, and there is no GL context (research/117): every
    // GL call the viewer makes ends here.

    // ------------------------------------------------------------------
    // Frame statistics: what was replayed, and why anything was not.

    U32 sDrawsReplayed = 0;
namespace
{
    // std::less<>: skip() looks reasons up without making a std::string (a
    // heap allocation per skipped draw for reasons over 15 characters).
    std::map<std::string, U32, std::less<>> sSkipped;
    std::map<std::string, U32> sUnhandledOnce;  // logged the first time only
    std::atomic<U32> sSkips{ 0 };   // every skip() so far (LLRHI::sBackendCounters)
} // namespace

    void skip(const char* reason)
    {
        sSkips.fetch_add(1, std::memory_order_relaxed);
        auto it = sSkipped.find(std::string_view(reason));
        if (it != sSkipped.end())
        {
            ++it->second;
        }
        else
        {
            sSkipped.emplace(reason, 1u);
        }
    }

    void unhandled(const std::string& what)
    {
        if (sUnhandledOnce[what]++ == 0)
        {
            LL_INFOS("VKGL") << "not handled yet: " << what << LL_ENDL;
        }
    }

    void unhandled(const std::string& what, const std::string& first_detail)
    {
        if (sUnhandledOnce[what]++ == 0)
        {
            LL_INFOS("VKGL") << "not handled yet: " << what << first_detail << LL_ENDL;
        }
    }

    std::string hex(U32 value)
    {
        std::ostringstream out;
        out << "0x" << std::hex << value;
        return out.str();
    }

    VkFormatFeatureFlags format_features(VkFormat format)
    {
        static std::unordered_map<U32, VkFormatFeatureFlags> cache;
        auto it = cache.find(format);
        if (it != cache.end())
        {
            return it->second;
        }
        VkFormatProperties props = {};
        vkGetPhysicalDeviceFormatProperties(LLVKDevice::instance().getPhysicalDevice(), format, &props);
        cache[format] = props.optimalTilingFeatures;
        return props.optimalTilingFeatures;
    }

    // Change counters, so draws can tell "nothing changed since last time" cheaply.
    U32 sTextureDeletes = 0;        // a GL texture was deleted (cached Texture* may dangle)
    U32 sBufferDeletes = 0;         // a GL buffer was deleted (cached Buffer* may dangle)
    U32 sFrame = 0;
namespace
{
    // For the fault report's address log (LLRHI_VK_ADDRESS_LOG): what a Vulkan
    // image or buffer was made for, kept after it is destroyed. The newest
    // OBJECT_NAME_RING names only (like LLVKDevice's resource ring): the oldest
    // name goes when a new one comes. sObjectNamesMutex is a leaf lock (nothing
    // else is locked under it): name_object() runs on texture threads too, and
    // describe_object() under the device's own locks in the fault report.
    constexpr size_t OBJECT_NAME_RING = 16384;
    struct ObjectName
    {
        U64 serial = 0;
        std::string name;
    };
    std::unordered_map<U64, ObjectName> sObjectNames;
    std::vector<std::pair<U64, U64>> sObjectNameRing;   // (handle, serial), oldest overwritten
    U64 sObjectNameCount = 0;
    std::mutex sObjectNamesMutex;
} // namespace
    const bool sNameObjects = LLRHI::envFirstChar("LLRHI_VK_ADDRESS_LOG") == '1';

    void name_object(U64 handle, const std::string& name)
    {
        if (sNameObjects)
        {
            std::string text = name + ", made in frame " + std::to_string(sFrame);
            std::lock_guard<std::mutex> lock(sObjectNamesMutex);
            if (sObjectNameRing.empty())
            {
                sObjectNameRing.resize(OBJECT_NAME_RING);
            }
            const U64 serial = ++sObjectNameCount;  // 0: an empty ring slot
            std::pair<U64, U64>& slot = sObjectNameRing[serial % OBJECT_NAME_RING];
            if (slot.second)
            {
                auto old = sObjectNames.find(slot.first);
                if (old != sObjectNames.end() && old->second.serial == slot.second)
                {
                    sObjectNames.erase(old);    // not renamed since: its oldest name goes
                }
            }
            slot = { handle, serial };
            ObjectName& entry = sObjectNames[handle];
            entry.serial = serial;
            entry.name = std::move(text);
        }
    }
namespace
{

    std::string describe_object(U64 handle, VkObjectType)
    {
        std::lock_guard<std::mutex> lock(sObjectNamesMutex);
        auto it = sObjectNames.find(handle);
        return it != sObjectNames.end() ? "(" + it->second.name + ")" : std::string();
    }
} // namespace
    U32 sTextureState = 0;          // any texture image, view or parameter changed (bindings: sUnitBindings)
    // Bumped by the render thread and (under sLock) by texture threads; atomic so
    // the bumps never race. Readers snapshot them once per check.
    std::atomic<U32> sTargetState{ 0 };     // framebuffer attachments / draw buffers / attached images changed
    std::atomic<U32> sPipelineState{ 0 };   // any GL state baked into pipelines changed (caps, blend, color mask, polygon mode)
    U32 sAttribConstants = 0;       // glVertexAttrib4f*: a disabled attribute's constant changed
    U32 sVertexState = 0;           // a vertex attribute's source or constant changed, or buffer contents were written
    // For the pass cache (research/39 N3), with the counters above:
    U32 sLayoutState = 0;           // an image layout changed in the frame's commands (transition, default target)
    U32 sUniformBlockState = 0;     // uniform block bindings changed, or a buffer read as a uniform block was written
    U32 sRecordEpoch = 0;           // sRecorded was reset or bypassed (new frame, depth-only path, native passes)
    // Dynamic render state (depth, cull, front face, stencil, viewport, scissor,
    // polygon offset) changed. Per context like the state itself (sState).
    thread_local U32 sFixedState = 0;
    void (*sTextureDeleted)(GLuint) = nullptr;     // LLVKTracker::setTextureDeletedCallback
    void (*sTextureDeletedMore)(GLuint) = nullptr; // LLVKTracker::addTextureDeletedCallback
    void (*sBufferChanged)(GLuint) = nullptr;      // LLVKTracker::setBufferChangedCallback
    void (*sBufferChangedMore)(GLuint) = nullptr;  // LLVKTracker::addBufferChangedCallback
namespace
{

    // ------------------------------------------------------------------
    // Per-frame cost accounting (LLRHI_VK_PERF): where the CPU time goes.

    const char* const sPerfNames[PERF_COUNT] = { "draw", "vertex", "descriptors", "(slots", "ubo", "images)", "set_update", "barriers",
                                                 "pipeline", "record", "(begin_rendering", "dynamic_state)", "gl_calls" };
} // namespace
    U64 sPerfTicks[PERF_COUNT] = {};
    U32 sSetsAllocated = 0, sSetsReused = 0, sSetsCached = 0, sUboCopies = 0, sUboReused = 0, sImagesReused = 0;
    // Skinning palettes: copied into the palette table, reused by their program,
    // shared with another program's copy of the same joints, draws skipped (table full).
    U32 sPaletteCopies = 0, sPaletteReused = 0, sPaletteShared = 0, sPaletteSkips = 0;
    // Research/56 (LLRHI::sPalettePrebuild): palettes copied into the table when
    // built (sPlacePalette), and draws that read such a row (palette_row).
    U32 sPalettePlaced = 0, sPaletteNamed = 0;
    // Research/56, with LLRHI::sAvatarStats only (the RHIAvatar line): palette
    // uniform writes (apply_palette) and table copies (palette_row, its own
    // copies only) with their time stamp ticks; direct draws of rigged mesh
    // programs (a mat3x4 palette), those that went into a draw run (any way),
    // straight (run_continue) and by template.
    U32 sAvPaletteWrites = 0, sAvPaletteRows = 0, sAvPaletteNamed = 0;
    U32 sAvRiggedDraws = 0, sAvRiggedRun = 0, sAvRiggedStraight = 0, sAvRiggedTemplate = 0;
    U64 sAvPaletteWriteTicks = 0, sAvPaletteRowTicks = 0;
    // Material rows (research/46 G4a): copied into the same table, reused by
    // their program, shared with an earlier copy of the same values.
    U32 sMaterialCopies = 0, sMaterialReused = 0, sMaterialShared = 0;
    // Constants rows (research/50 step 3): the same for the shared layout's constants.
    U32 sConstsCopies = 0, sConstsReused = 0, sConstsShared = 0;
namespace
{
    using PerfClock = std::chrono::steady_clock;
} // namespace
    const bool sPerf = std::getenv("LLRHI_VK_PERF") != nullptr;
namespace
{

    // The CPU's time stamp counter: steady_clock (QueryPerformanceCounter) around
    // every step of every draw cost ~9% of the main thread at a heavy place.
    // Ticks become nanoseconds at report time against the clock since startup.
    const U64 sPerfTsc0 = __rdtsc();
    const PerfClock::time_point sPerfClock0 = PerfClock::now();

} // namespace
    F64 perf_ticks_per_ns()
    {
        const F64 ns = (F64)std::chrono::duration_cast<std::chrono::nanoseconds>(PerfClock::now() - sPerfClock0).count();
        return ns > 0.0 ? (F64)(__rdtsc() - sPerfTsc0) / ns : 1.0;
    }
namespace
{

    // LLRHI::sObjectVersions (the reflection probe's static G-buffer cache).
    bool native_object_versions(const U32* textures, U32 texture_count, const U32* buffers, U32 buffer_count, U64& hash)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        auto mix = [&hash](U64 value)
        {
            hash = LLRHI::fnv1aMix(hash, value);
        };
        for (U32 i = 0; i < texture_count; ++i)
        {
            const Texture* tex = textures[i] ? sTextures.find(textures[i]) : nullptr;
            if (!tex || !tex->image || !tex->version || tex->rendered_to)
            {
                return false;
            }
            mix(((U64)textures[i] << 32) | tex->version);
        }
        for (U32 i = 0; i < buffer_count; ++i)
        {
            const Buffer* buf = buffers[i] ? sBuffers.find(buffers[i]) : nullptr;
            if (LLRHIVBCensus::sOn && buf && buf->owner_number)     // research/293 branch 0: counting only
            {
                LLRHIVBCensus::nameRead(LLRHIVBCensus::READ_VERSIONS, buf->owner_number);
            }
            if (!buf || !buf->version)
            {
                return false;
            }
            mix(((U64)buffers[i] << 32) | buf->version);
        }
        return true;
    }

    // LLRHI::sBackendCounters.
    void native_backend_counters(U32& program_changes, U32& skips, U32& texture_versions)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        program_changes = sProgramChanges;
        skips = sSkips.load(std::memory_order_relaxed);
        texture_versions = sTextureVersions.load(std::memory_order_relaxed);
    }
    // research/77 (LLRHI_VK_LIVE=1): live objects by kind, logged with each
    // memory report (once a minute), so a long run shows which kind grows: GL
    // buffers and their Vulkan copies, texture images, those of them in memory
    // of their own, and bindless slots.
    const bool sLiveReport = LLRHI::envSet("LLRHI_VK_LIVE");

    void log_live_objects()
    {
        VmaAllocator allocator = LLVKDevice::instance().getAllocator();
        if (!allocator)
        {
            return;
        }
        struct Count
        {
            U32 count = 0;
            U64 bytes = 0;
            void add(U64 b) { ++count; bytes += b; }
        };
        Count gl_buffers, gpu_copies, buffer_dedicated, cpu_copies;
        U64 gpu_used = 0;
        sBuffers.forEach([&](const Buffer& b)
        {
            gl_buffers.add(b.data.size());
            cpu_copies.add(b.data.capacity());
            if (b.gpu && b.gpu_memory)
            {
                VmaAllocationInfo2 info = {};
                vmaGetAllocationInfo2(allocator, b.gpu_memory, &info);
                gpu_copies.add(info.allocationInfo.size);
                gpu_used += b.data.size();
                if (info.dedicatedMemory)
                {
                    buffer_dedicated.add(info.allocationInfo.size);
                }
            }
        });
        Count textures, images, image_dedicated, attachable;
        sTextures.forEach([&](const Texture& tex)
        {
            textures.add(0);
            if (tex.image && tex.memory)
            {
                VmaAllocationInfo2 info = {};
                vmaGetAllocationInfo2(allocator, tex.memory, &info);
                images.add(info.allocationInfo.size);
                if (info.dedicatedMemory)
                {
                    image_dedicated.add(info.allocationInfo.size);
                }
                if (tex.attachable)
                {
                    attachable.add(info.allocationInfo.size);
                }
            }
        });
        U32 slots = 0, waiting = 0, table = 0;
        LLVKBindless::slotStats(slots, waiting, table);
        LL_INFOS("TextureMemory") << "live objects (research/77): GL buffers " << gl_buffers.count << " (" << (gl_buffers.bytes >> 20)
                                  << " MB, CPU copies " << (cpu_copies.bytes >> 20) << " MB), with a Vulkan copy " << gpu_copies.count
                                  << " (" << (gpu_copies.bytes >> 20) << " MB for " << (gpu_used >> 20) << " MB of data, "
                                  << buffer_dedicated.count << " dedicated); textures " << textures.count << ", VkImages " << images.count
                                  << " (" << (images.bytes >> 20) << " MB, attachable " << attachable.count << " / "
                                  << (attachable.bytes >> 20) << " MB), dedicated " << image_dedicated.count << " ("
                                  << (image_dedicated.bytes >> 20) << " MB); bindless slots " << slots << " in use, " << waiting
                                  << " waiting, " << table << " in the table" << LL_ENDL;
    }

    // LLRHI::sTextureImageBytes (research/79): what each named texture's image
    // takes as allocated (every level), for the texture census. Reads only.
    void native_texture_image_bytes(const U32* names, U32 count, U64* bytes)
    {
        VmaAllocator allocator = LLVKDevice::instance().getAllocator();
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        for (U32 i = 0; i < count; ++i)
        {
            bytes[i] = 0;
            const Texture* tex = names[i] ? sTextures.find(names[i]) : nullptr;
            if (allocator && tex && tex->image && tex->memory)
            {
                VmaAllocationInfo info = {};
                vmaGetAllocationInfo(allocator, tex->memory, &info);
                bytes[i] = info.size;
            }
        }
    }

    // The text of native_memory_report, under the tracker's lock.
    std::string memory_report_text()
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        if (sLiveReport)
        {
            log_live_objects();
        }
        std::string out = LLVKDevice::instance().memoryReport();
        for (const std::string& pool : { LLVKAttribPool::memoryReport(), LLVKGPUDriven::memoryReport() })
        {
            if (!pool.empty())
            {
                out += (out.empty() ? "" : ", ") + pool;
            }
        }
        out += "; " + LLVKTexCompress::report();
        return out;
    }

    // LLRHI::sMemoryReport (research/54): the device's VRAM and our pools.
    std::string native_memory_report()
    {
        std::string out = memory_report_text();
        if (LLRHIVBCensus::sOn)
        {
            // research/293 branch 0: its own "VBCensus" lines, about once a minute. Outside the tracker's lock: it
            // loads symbols (DbgHelp) the first time, which can take seconds.
            LLRHIVBCensus::report();
        }
        return out;
    }
}
} // namespace llvkgl

using namespace llvkgl;

void LLVKGL::install()
{
    LLRHI::sGLDrawsDisabled = true;
    LLRHI::sVulkanGL = true;
    LLRHI::sDrawRunPrograms = &draw_run_programs;
    install_link_hooks();   // research/111 B-SC, B-PW
    // The draws at a fault, and (bc7-fault) the BC7 encodings that were on the GPU.
    LLVKDevice::instance().setDeviceLostHook([](const std::vector<U64>& markers, const std::vector<U32>& stages)
    {
        report_lost_draws(markers);
        LLVKTexCompress::noteDeviceLost(markers, stages);
    });
    LLVKDevice::instance().setDescribeHook(&describe_object);
    LLVKNative::install();
    LLVKVolumetric::install();
    LLVKShader::init();
    if (LLRHI::sDirect)
    {
        LLRHI::sDirectDraw = &direct_draw;
        // Draw templates (research/46 G4c), used while LLRHI::sDrawTemplates is on.
        LLRHI::sDrawTemplate = &draw_template;
        LLRHI::sDrawTemplateReleased = &draw_template_released;
        // Draw packets (research/180 stage 4).
        LLRHI::sPacketDraw = &packet_draw;
        LLRHI::sBufferData = &native_buffer_data;
        LLRHI::sBufferSubData = &native_buffer_sub_data;
    }
    // The reflection probe's static G-buffer cache (research/44 P5).
    LLRHI::sObjectVersions = &native_object_versions;
    LLRHI::sBackendCounters = &native_backend_counters;
    LLRHI::sMemoryReport = &native_memory_report;
    // research/245: what the GL buffer compaction gave back (plus/tplag's timeline).
    LLRHI::sBufferCompactCounts = [](LLRHI::BufferCompactCounts& out)
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        buffer_compact_counts(out);
    };
    // research/109: the fault report's frame events, the counts for the log after a snapshot.
    LLRHI::sNoteEvent = [](const char* text) { LLVKDevice::instance().noteEvent(text); };
    LLRHI::sResourceCounts = [](U64& made, U64& released, U64& bc7)
    {
        const LLVKDevice& device = LLVKDevice::instance();
        made = device.resourceCount(LLVKDevice::RESOURCE_MADE);
        released = device.resourceCount(LLVKDevice::RESOURCE_RELEASED);
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        bc7 = LLVKTexCompress::encodedTotal();
    };
    LLRHI::sDumpAllocations = [](const std::string& path) { LLVKDevice::instance().dumpAllocations(path); };
    // The texture census (research/79): image sizes by name, the pools one by one.
    LLRHI::sTextureImageBytes = &native_texture_image_bytes;
    LLRHI::sPoolDetail = []() { return LLVKDevice::instance().poolDetail(); };
    // research/78: texture budget against the driver's VRAM budget (Vulkan alone in VRAM).
    LLRHI::sVRAMBudget = [](U64& texture_bytes, U64& usage, U64& budget)
    { return LLVKDevice::instance().vramBudget(texture_bytes, usage, budget); };
    // research/88: textures stored BC7.
    LLRHI::sTextureCompressed = &native_texture_compressed;
    LLRHI::sTextureMoveHold = &native_texture_move_hold;    // research/285 compact-hysteresis
    LLRHI::sTexBCNReport = []()
    {
        std::lock_guard<LLVKTrackerMutex> lock(sLock);
        return LLVKTexCompress::report();
    };
    // Order-independent transparency (research/41): sessions open only while
    // LLRHI::sOIT is on and the device can do it (oit_init).
    LLRHI::sOITBegin = &oit_begin;
    LLRHI::sOITWorld = &oit_world;
    LLRHI::sOITVariant = &oit_program_variant;
    LLRHI::sOITResolve = &oit_resolve;
    LLRHI::sOITSupported = &oit_supported;
    LLRHI::sOITSnapshotCheck = &oit_snapshot_check;
    // The alpha and material shaders' early outs (research/54, 59): only with
    // the features they need.
    if ((LLRHI::sAlphaFast || LLRHI::sMaterialFast) && !LLVKDevice::instance().hasAlphaEarlyOut())
    {
        LLRHI::sAlphaFast = false;
        LLRHI::sMaterialFast = false;
        LL_INFOS("VKGL") << "alpha and material early outs off: the GPU lacks terminateInvocation, demote or fragment quad operations" << LL_ENDL;
    }
    LL_INFOS("VKGL") << "early outs: alpha " << (LLRHI::sAlphaFast ? "on" : "off") << ", material " << (LLRHI::sMaterialFast ? "on" : "off")
                     << ", probe fuse " << (LLRHI::sProbeFuse ? "on" : "off") << LL_ENDL;
    // Batched point lights (research/47).
    LLRHI::sLightTableProgram = &native_light_table_program;
    LLRHI::sTableRows = &native_table_rows;
    // The GL entry points (llvkgl_entry.cpp), then the native API that replaces
    // them where they are wrapped (llvkgl_hooks.cpp).
    install_gl_wraps();
    install_native_hooks();
    install_texref();   // research/197 1a: LLRHI::Textures (texture numbers)
    // research/293 branch 0: the census (LLRHI_VB_CENSUS, off unless set) before the first buffer takes a number
    LLRHIVBCensus::setOwnerHooks(&census_set_owner, &census_clear_owner);
    LLRHIVBCensus::init();
    LLVKAttribPool::installNumbers();   // research/197 1b: LLRHI::Buffers (vertex buffer numbers)
    install_scene_binder();             // research/198 5.5: the retained scene draws through the bound program
}

std::string LLVKGL::frameReport()
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    std::ostringstream out;
    out << "draws replayed " << sDrawsReplayed;
    for (const auto& reason : sSkipped)
    {
        if (reason.second)      // reasons of earlier frames stay at 0 (resetFrameStats)
        {
            out << ", skipped " << reason.second << " (" << reason.first << ")";
        }
    }
    // LLRHI_BINDLESS_DEBUG=1: the last slots of each drawing program and what the table holds there.
    static const bool debug = std::getenv("LLRHI_BINDLESS_DEBUG") != nullptr;
    if (debug)
    {
        for (const auto& p : sPrograms)
        {
            if (!p.second.generic_draws || p.second.bindless.empty())
            {
                continue;
            }
            out << "\n  program " << p.first << " (" << p.second.generic_draws << " draws)";
            for (size_t i = 0; i < p.second.bindless.size() && i < p.second.bindless_slots.size(); ++i)
            {
                out << "\n    " << p.second.bindless[i].name << " unit " << p.second.bindless[i].unit << ": "
                    << LLVKBindless::describe(p.second.bindless_slots[i]);
            }
        }
    }
    return out.str();
}

std::string LLVKGL::perfReport(U32 frames)
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    std::ostringstream out;
    frames = llmax(frames, 1u);
    U32 palette_peak = 0, palette_capacity = 0;
    LLVKBindless::paletteStats(palette_peak, palette_capacity);
    if (sPerf)
    {
        out << "per frame ms:";
        const F64 ticks_per_ns = perf_ticks_per_ns();
        for (int i = 0; i < PERF_COUNT; ++i)
        {
            out << " " << sPerfNames[i] << " " << (F64)sPerfTicks[i] / ticks_per_ns / frames / 1e6;
        }
        out << "; ";
    }
    out << "per frame: sets new " << sSetsAllocated / frames << " reused " << sSetsReused / frames << " cached " << sSetsCached / frames
        << ", uniform copies " << sUboCopies / frames << " reused " << sUboReused / frames
        << ", palette copies " << sPaletteCopies / frames << " reused " << sPaletteReused / frames << " shared " << sPaletteShared / frames
        << " skipped " << sPaletteSkips << " prebuilt " << (LLRHI::sPalettePrebuild ? "on" : "off") << " [rows " << sPalettePlaced / frames
        << ", draws on them " << sPaletteNamed / frames << "]"
        << " (table peak " << palette_peak << " vec4s/frame of " << palette_capacity << ")"
        << ", material rows " << sMaterialCopies / frames << " reused " << sMaterialReused / frames << " shared " << sMaterialShared / frames
        << ", constants rows " << sConstsCopies / frames << " reused " << sConstsReused / frames << " shared " << sConstsShared / frames
        << " (programs on the shared layout " << shared_layout_programs() << ")"
        << ", gl calls " << sGLCalls / frames << " native state " << sNativeStateCalls / frames << " (" << gl_call_report(frames) << "), depth-only draws " << sDepthOnlyDraws / frames << ", image lists reused " << sImagesReused / frames << ", vertex inputs reused " << sVertexInputsReused / frames << " (direct " << sDirectInputsReused / frames << ")"
        << ", direct draws " << sDirectDraws / frames << " (pass cache hits " << sPassHits / frames << " [uniform " << sPassUniformHits / frames
        << ", rebound " << sPassRebinds / frames << "] misses " << sPassMisses / frames
        << (sPerf ? " [" + pass_miss_report(frames) + "]" : std::string())
        << (sPassCacheEnabled ? "" : ", off") << "), texture slots: by unit binding " << sSlotsUnitCached / frames
        << ", resolved this frame " << sSlotsFrameCached / frames << ", resolved " << sSlotsResolved / frames << slot_publish_report(frames) << packet_report(frames)
        << (LLRHI::sSlotCache ? "" : " (slot cache off)") << ", fans as lists " << sFansConverted / frames << ", light batches " << sLightBatches / frames << " (lights " << sLightInstances / frames << ")"
        << ", draw runs " << sRunsOpened / frames << " (draws " << sRunDraws / frames << ", rows " << sRunRows / frames
        << ", table full " << sRunRowsFull / frames << ", straight " << sRunFast / frames << ", templates " << sTemplateDraws / frames
        << " [refused " << sTemplateRefused / frames << ", made " << sTemplatesMade / frames << ", live "
        << (sTemplates.size() - sTemplateFree.size()) << ", full " << sTemplatesFull / frames << "], refused [" << run_refusals(frames)
        << "], generated executes " << sGeneratedExecutes / frames << "), line loops as strips " << sLoopsConverted / frames << ", occlusion queries " << sQueriesIssued / frames << " (unmarked draws " << sQueriesUnmarked / frames;
    if (sReadbacks)
    {
        out << ", pixel reads " << sReadbacks << " (frame waits " << sReadbackWaits << ")";
    }
    for (const auto& failure : sDepthOnlyFailures)
    {
        out << ", depth-only: " << failure.first << " " << failure.second / frames;
    }
    for (const auto& failure : sMarkFailures)
    {
        out << ", " << failure.first << " " << failure.second / frames;
    }
    out << ")"
        << ", draws writing nothing skipped " << sInvisibleDraws / frames
        << ", direct draws refused [" << direct_refusals(frames) << "]"
        << ", texture uploads " << sTextureUploads / frames << " (before draws " << sTextureUploadsEarly / frames << ")"
        << ", " << LLVKNative::takeStats(frames);
    if (sOIT.ready)
    {
        out << ", " << oit_report(frames);
    }
    out << texref_report(frames);   // research/197 1a: ", texture numbers [...]" (empty while never on)
    out << LLVKAttribPool::numbersReport(frames);   // research/197 1b: ", vertex buffer numbers [...]"
    {
        const std::string pool = LLVKAttribPool::takeStats(frames);
        if (!pool.empty())
        {
            out << ", " << pool;
        }
    }
    {
        uint64_t packets = 0, bytes = 0;
        double wait_ms = 0.0;
        LLVKRecorder::takeStats(packets, bytes, wait_ms);
        out << "; recording worker: " << packets / frames << " packets " << bytes / frames / 1024 << " KB, end-of-frame wait "
            << wait_ms / frames << " ms, " << LLVKRecorder::takeWorkerStats(frames);
    }
    // research/137 N7: the draw lists, the backend's and the viewer side's
    // counts (empty while LLRHI_DRAW_LISTS is 0 and no list was drawn).
    {
        const std::string backend = LLVKDrawList::report(frames);
        const std::string viewer = LLRHI::DrawList::report(frames);
        if (!backend.empty() || !viewer.empty())
        {
            out << "; draw lists: " << backend << (backend.empty() || viewer.empty() ? "" : "; ") << viewer;
        }
    }
    // research/137 N9: the immediate mode ring, while its hook is installed
    // (LLVKImmediate::install); empty while LLRHI_IMMEDIATE_RING is 0 and no
    // draw came. report() takes sLock again (a recursive mutex).
    if (LLRHI::sImmediateDraw)
    {
        const std::string immediate = LLVKImmediate::report(frames);
        if (!immediate.empty())
        {
            out << "; immediate ring: " << immediate;
        }
    }
    // research/137 N14: buffer writes and texture data (both ways, to compare LLRHI_NATIVE_RESOURCES 0 and 1).
    out << "; " << LLVKResource::report(frames);
    // Calls recorded into spliced streams were counted by their stream (research/66 step 1).
    LLVKRecorder::takeStreamCounts(LLVKDispatch::sCounts, LLVKDispatch::COUNTER_COUNT);
    out << "; vulkan calls per frame:";
    for (U32 i = 0; i < LLVKDispatch::COUNTER_COUNT; ++i)
    {
        if (LLVKDispatch::sCounts[i] / frames)
        {
            out << " " << LLVKDispatch::sCounterNames[i] + 2 << " " << LLVKDispatch::sCounts[i] / frames;
        }
        LLVKDispatch::sCounts[i] = 0;
    }
    ++sDispatchResets;  // an open draw run must not take the counts for unchanged (run_draw)
    if (sPerf)
    {
        // Which programs the generic path spends its draws on (inputs and samplers identify the shader).
        std::vector<std::pair<U32, GLuint>> busiest;
        for (auto& p : sPrograms)
        {
            if (p.second.generic_draws)
            {
                busiest.emplace_back(p.second.generic_draws, p.first);
            }
        }
        std::sort(busiest.rbegin(), busiest.rend());
        out << "; generic draws per frame by program:";
        for (size_t i = 0; i < busiest.size() && i < 12; ++i)
        {
            const Program& program = sPrograms[busiest[i].second];
            out << " [" << busiest[i].second << " " << busiest[i].first / frames << " in:";
            for (const auto& input : program.vk.inputs)
            {
                out << input.name << ",";
            }
            out << " tex:";
            for (size_t s = 0; s < program.sampler_slots.size() && s < 4; ++s)
            {
                out << program.sampler_slots[s].name << ",";
            }
            out << (program.sampler_slots.size() > 4 ? "..." : "") << "]";
        }
        for (auto& p : sPrograms)
        {
            p.second.generic_draws = 0;
        }
    }
    for (U64& ns : sPerfTicks)
    {
        ns = 0;
    }
    sSetsAllocated = sSetsReused = sSetsCached = sUboCopies = sUboReused = sDepthOnlyDraws = sImagesReused = sVertexInputsReused = sDirectInputsReused = 0;
    sPaletteCopies = sPaletteReused = sPaletteShared = sPaletteSkips = 0;
    sPalettePlaced = sPaletteNamed = 0;
    sMaterialCopies = sMaterialReused = sMaterialShared = 0;
    sConstsCopies = sConstsReused = sConstsShared = 0;
    sGLCalls = 0;
    sNativeStateCalls = 0;
    sDirectDraws = 0;
    sPassHits = sPassMisses = sPassUniformHits = sPassRebinds = 0;
    memset(sPassMissWhy, 0, sizeof(sPassMissWhy));
    sSlotsUnitCached = sSlotsFrameCached = sSlotsResolved = 0;
    slot_publish_reset_counters();
    packet_reset_counters();
    sQueriesIssued = sQueriesUnmarked = 0;
    sMarkFailures.clear();
    texref_reset_counters();    // research/197 1a
    sDepthOnlyFailures.clear();
    sFansConverted = 0;
    sLightBatches = sLightInstances = 0;
    sRunsOpened = sRunDraws = sRunRows = sRunRowsFull = sRunFast = 0;
    memset(sRunRefused, 0, sizeof(sRunRefused));
    sGeneratedExecutes = 0;
    sTemplateDraws = sTemplateRefused = sTemplatesMade = sTemplatesFull = 0;
    sLoopsConverted = 0;
    sReadbacks = sReadbackWaits = 0;
    sTextureUploads = sTextureUploadsEarly = 0;
    sInvisibleDraws = 0;
    memset(sDirectRefused, 0, sizeof(sDirectRefused));
    return out.str();
}

std::string LLVKGL::counterReport(U32 frames)
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    frames = llmax(frames, 1u);
    // The target state's moves since the last report (no extra counting where it moves).
    static U32 sTargetStateReported = 0;
    const U32 target_state = sTargetState.load(std::memory_order_relaxed);
    const U32 target_changes = target_state - sTargetStateReported;
    sTargetStateReported = target_state;
    const U32 vb_misses = LLRHI::sVBCacheMisses.exchange(0, std::memory_order_relaxed);
    std::ostringstream out;
    out << "; counters: pass misses [" << pass_miss_columns(frames) << "], target changes " << target_changes / frames
        << " (same draw buffers kept " << sDrawBuffersKept / frames << "), VB cache misses " << vb_misses / frames
        << ", persistent sets [" << (sSetPersist ? "on" : "off") << ", kept " << sPersistHits / frames << ", new " << sPersistNew / frames
        << ", retired " << sPersistRetires << ", pools " << LLVKMirror::instance().persistentPoolCount() << "]"
        << ", bind target " << (LLRHI::sBindTarget ? "on " : "off ") << sBindTargets / frames
        << ", units rechecked " << (LLRHI::sPassUnitsRecheck ? "on " : "off ") << sPassUnitsKept / frames;
    sDrawBuffersKept = 0;
    sPassUnitsKept = 0;
    sBindTargets = 0;
    sPersistHits = sPersistNew = sPersistRetires = 0;
    return out.str();
}

void LLVKGL::compressTextures()
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    compress_queued_textures();
}

void LLVKGL::compactBuffers()
{
    log_held_remakes();     // research/285: also once compaction is off
    const bool halted = vram_compaction_halted();      // research/285: the device lost or the viewer quitting
    if (halted || !buffer_compaction_allowed() || !LLRHI::sBufferCompact.load(std::memory_order_relaxed))
    {
        if (halted || !LLRHI::sTexCompact || !LLRHI::sBufferCompact.load(std::memory_order_relaxed))
        {
            vram_compaction_stop();     // research/285: neither runs (a flag test while it is not compacting)
        }
        return;     // off: no lock taken
    }
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    compact_buffers();     // research/245
}

bool LLVKGL::compactTextures()
{
    if (!compact_textures_due())
    {
        return false;   // off, or no check due: no lock taken
    }
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    return compact_textures();     // research/285 X2
}

void LLVKGL::resetFrameStats()
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    sDrawsReplayed = 0;
    // The counts start over; the reasons stay (string literals, a few dozen
    // at most), so skip() does not allocate them again every frame (fix-pipeleak).
    for (auto& reason : sSkipped)
    {
        reason.second = 0;
    }
    std::vector<U8>().swap(sReadback.data);    // stale once the frame ended; can be large
    ++sFrame;
    LLVKDevice::instance().setFrame(sFrame);
    // research/285: VMA keeps its empty block again once the compactions'
    // frees are done (every frame, also one not recorded; a flag test).
    vram_compaction_tick();
}

void LLVKGL::drawCounts(U32& draws, U32& direct)
{
    std::lock_guard<LLVKTrackerMutex> lock(sLock);
    draws = sDrawsReplayed;
    direct = sDirectDraws;
}

LLVKGL::DrawGlobals LLVKGL::drawGlobalsNow()
{
    DrawGlobals g;
    g.depth_only = LLRHI::sDepthOnlyPass;
    g.suppress_translation = LLRHI::sSuppressTranslation;
    g.occlusion_scope = LLRHI::sOcclusionScope;
    g.census_tag = LLRHI::sCensusTag;
    g.oit_priority = LLRHI::sOITPriority;
    g.shadow_bias = LLRHI::sShadowDepthBias;
    return g;
}

LLVKGL::QueuedGlobals::QueuedGlobals(const DrawGlobals& globals)
{
    tQueuedDraw = &globals;
}

LLVKGL::QueuedGlobals::~QueuedGlobals()
{
    tQueuedDraw = nullptr;
}

void* LLVKGL::getProcAddress(const char* name)
{
    return (void*)get_proc_address(name);
}

#else // !LL_GL_FUNC_POINTER

void LLVKGL::install() {}
void* LLVKGL::getProcAddress(const char*) { return nullptr; }
void LLVKGL::compressTextures() {}
void LLVKGL::compactBuffers() {}
bool LLVKGL::compactTextures() { return false; }
std::string LLVKGL::frameReport() { return std::string(); }
std::string LLVKGL::perfReport(U32) { return std::string(); }
std::string LLVKGL::counterReport(U32) { return std::string(); }
void LLVKGL::resetFrameStats() {}
void LLVKGL::drawCounts(U32& draws, U32& direct) { draws = direct = 0; }
LLVKGL::DrawGlobals LLVKGL::drawGlobalsNow() { return DrawGlobals(); }
LLVKGL::QueuedGlobals::QueuedGlobals(const DrawGlobals&) {}
LLVKGL::QueuedGlobals::~QueuedGlobals() {}
bool LLVKGLProbe::lastRun(Run&) { return false; }
bool LLVKGLProbe::templateWords(Words&) { return false; }
bool LLVKGLProbe::matrixRow(const LLRHI::DirectDraw&, U32&) { return false; }
U32 LLVKGLProbe::textureSlot(U32, bool& readable) { readable = false; return 0; }
void LLVKGLProbe::flushSlots() {}
U64 LLVKGLProbe::commands() { return 0; }
void LLVKGLProbe::forgiveUploads(U64) {}
bool LLVKGLProbe::drawIndirect(const Run&, VkBuffer, VkDeviceSize, U32) { return false; }
U64 LLVKGLProbe::drawFormat(const LLRHI::DirectDraw&) { return 0; }
U32 LLVKGLProbe::programChanges() { return 0; }
void LLVKGLProbe::beginCapture() {}
void LLVKGLProbe::endCapture() {}
U32 LLVKGLProbe::captureCount() { return 0; }
const LLVKGLProbe::Capture* LLVKGLProbe::capture(U32) { return nullptr; }
bool LLVKGLProbe::generatedPipeline(const Run&, VkPipeline&, Streams&) { return false; }
bool LLVKGLProbe::executeGenerated(const Run&, VkPipeline, const VkGeneratedCommandsInfoEXT&) { return false; }

#endif // LL_GL_FUNC_POINTER
