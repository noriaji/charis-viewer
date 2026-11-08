/**
 * @file llvkgl_glquery.cpp
 * @brief The GPU (LLRHI::deviceInfo, research/207 T3) and GL's queries answered by the Vulkan side (glGetIntegerv, glIsEnabled ...): research/117 G2.
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

#include "llvkgl_internal.h"

#if LL_GL_FUNC_POINTER

#include <dxgi.h>

// The GPU LLGLManager::initGL fills gGLManager from (LLRHI::DeviceInfo,
// research/207 T3), and what the viewer still asks GL about the limits and the
// current state, answered from the Vulkan device and the tracked state
// (research/117 1.4). The strings take the forms the viewer parses
// (parse_gl_version, parse_glsl_version) and matches (vendor names, upper
// cased, by substring), and the values those of the GL drivers where the
// viewer reads them (research/117 7). There is no GL context to ask.

namespace llvkgl
{
namespace
{
    // GL's error flag (answer_glGetError, raise_gl_error), one per thread as
    // GL has one per context: the main context's calls are made (and its
    // glGetError answered) on the main thread, or all on the translation
    // thread while it runs; a texture thread (LLImageGLThread: media) has
    // its own. Shared, a media texture the device could not make there
    // would fail the main thread's next render target or shader load.
    thread_local GLenum tGLError = GL_NO_ERROR;

    // The GPU (LLRHI::DeviceInfo, research/207 T3) and the limits GL's
    // queries still answer at run time (int_values, float_values).
    struct QueryAnswers
    {
        bool ready = false;
        LLRHI::DeviceInfo device;
        GLint max_cube_size = 4096, max_3d_size = 256, max_layers = 256;
        GLint max_vertex_attribs = 16, max_draw_buffers = 4;
        GLint vidmem_kb = 0;                    // GL_NVX_gpu_memory_info (named for every GPU: vram_bytes)
    };

    // NVIDIA's GL driver answers GL_NVX_gpu_memory_info's dedicated memory
    // with the card's whole memory (8,192 MB on an 8 GB card); Vulkan's device
    // local heap (as DXGI's dedicated memory) leaves out what the driver keeps
    // (7,989 MB), and the viewer sizes its texture budget by it
    // (research/117 7.2). NVML (nvml.dll, which the NVIDIA driver installs)
    // gives the whole: the bytes of the GPU with this UUID, 0 if not found.
    U64 nvml_total_memory(const U8 (&uuid)[VK_UUID_SIZE])
    {
        typedef int (*NvmlInit)();
        typedef int (*NvmlShutdown)();
        typedef int (*NvmlCount)(unsigned int* count);
        typedef int (*NvmlHandleByIndex)(unsigned int index, void** device);
        typedef int (*NvmlUUID)(void* device, char* uuid, unsigned int length);
        struct NvmlMemory { unsigned long long total, free, used; };
        typedef int (*NvmlMemoryInfo)(void* device, NvmlMemory* memory);
        HMODULE nvml = LoadLibraryExA("nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!nvml)
        {
            return 0;
        }
        // Released on every return (the loader counts references: whoever
        // else loaded nvml.dll keeps it).
        struct Release
        {
            HMODULE module;
            ~Release() { FreeLibrary(module); }
        } release{ nvml };
        NvmlInit init = (NvmlInit)GetProcAddress(nvml, "nvmlInit_v2");
        NvmlShutdown shutdown = (NvmlShutdown)GetProcAddress(nvml, "nvmlShutdown");
        NvmlCount count = (NvmlCount)GetProcAddress(nvml, "nvmlDeviceGetCount_v2");
        NvmlHandleByIndex handle = (NvmlHandleByIndex)GetProcAddress(nvml, "nvmlDeviceGetHandleByIndex_v2");
        NvmlUUID get_uuid = (NvmlUUID)GetProcAddress(nvml, "nvmlDeviceGetUUID");
        NvmlMemoryInfo memory_info = (NvmlMemoryInfo)GetProcAddress(nvml, "nvmlDeviceGetMemoryInfo");
        if (!init || !shutdown || !count || !handle || !get_uuid || !memory_info || init() != 0)
        {
            return 0;
        }
        // NVML writes "GPU-" and the 16 bytes as 8-4-4-4-12 hex digits.
        std::ostringstream wanted;
        wanted << "GPU-" << std::hex << std::setfill('0');
        for (U32 i = 0; i < VK_UUID_SIZE; ++i)
        {
            if (i == 4 || i == 6 || i == 8 || i == 10)
            {
                wanted << '-';
            }
            wanted << std::setw(2) << (U32)uuid[i];
        }
        U64 total = 0;
        unsigned int devices = 0;
        for (unsigned int i = 0; count(&devices) == 0 && i < devices && !total; ++i)
        {
            void* device = nullptr;
            char name[96] = {};
            NvmlMemory memory = {};
            if (handle(i, &device) == 0 && get_uuid(device, name, sizeof(name)) == 0 && _stricmp(name, wanted.str().c_str()) == 0 &&
                memory_info(device, &memory) == 0)
            {
                total = memory.total;
            }
        }
        shutdown();     // NVML counts its users: llrhivkmirror's sensors may have it open too
        return total;
    }

    // The DXGI adapter with this LUID (the Vulkan device's): its dedicated video memory and
    // its shared system memory, in bytes. False: no such adapter (or no DXGI). Every adapter
    // goes in the log (LLWindowWin32 checkDXMem listed them; it no longer runs).
    bool dxgi_adapter_memory(const U8 (&luid)[VK_LUID_SIZE], U64& dedicated, U64& shared)
    {
        typedef HRESULT (WINAPI *CreateFactory1)(REFIID riid, void** factory);
        // Already in the process (LLWindowWin32 links it); loaded by name, so that this
        // library needs no import of its own.
        HMODULE dxgi = LoadLibraryExA("dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dxgi)
        {
            return false;
        }
        bool found = false;
        CreateFactory1 create = (CreateFactory1)GetProcAddress(dxgi, "CreateDXGIFactory1");
        IDXGIFactory1* factory = nullptr;
        if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), (void**)&factory)) && factory)
        {
            LUID wanted;
            static_assert(sizeof(wanted) == VK_LUID_SIZE, "a LUID is 8 bytes");
            memcpy(&wanted, luid, sizeof(wanted));
            IDXGIAdapter1* adapter = nullptr;
            for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)); ++i)
            {
                DXGI_ADAPTER_DESC1 desc = {};
                if (adapter && SUCCEEDED(adapter->GetDesc1(&desc)))
                {
                    const bool match = !found && desc.AdapterLuid.LowPart == wanted.LowPart && desc.AdapterLuid.HighPart == wanted.HighPart;
                    if (match)
                    {
                        dedicated = desc.DedicatedVideoMemory;
                        shared = desc.SharedSystemMemory;
                        found = true;
                    }
                    LL_INFOS("VKGL") << "DXGI adapter " << i << ": " << ll_convert_wide_to_string(std::wstring(desc.Description))
                                     << ", LUID " << desc.AdapterLuid.HighPart << "_" << desc.AdapterLuid.LowPart << ", dedicated "
                                     << (desc.DedicatedVideoMemory >> 20) << " MB, shared " << (desc.SharedSystemMemory >> 20) << " MB"
                                     << (match ? " (the Vulkan device)" : "") << LL_ENDL;
                }
                if (adapter)
                {
                    adapter->Release();
                    adapter = nullptr;
                }
            }
            factory->Release();
        }
        FreeLibrary(dxgi);
        return found;
    }

    // research/124 V3 (fix-round9b 3): the VRAM of the GPU Vulkan renders with, in bytes.
    // The viewer reads it as GL_NVX_gpu_memory_info's dedicated memory (llgl.cpp initGL), on
    // every GPU, and then does not ask DXGI itself (LLWindowWin32 checkDXMem): that took
    // adapter 0's budget, which on a PC with two GPUs (an Intel one beside a Radeon) may be
    // the one that does not render, and capped it for Intel by the rendering GPU's vendor.
    // - NVIDIA: the whole card, as its GL driver tells (NVML).
    // - A discrete GPU of another maker: the dedicated video memory of the DXGI adapter with
    //   the device's LUID (as GL's WGL_AMD_gpu_association told on AMD: the card's memory).
    // - An integrated GPU (it shares the PC's memory): that adapter's dedicated and shared
    //   memory, at most an eighth of the PC's memory and 4 GB (igpu-safe, below; a quarter
    //   before, the cap checkDXMem put on Intel's), but never less than its dedicated memory
    //   (what the firmware set aside for it).
    //   The same for any other kind that is not a discrete GPU (virtual, CPU rendering).
    // Not found there: the largest device local heap, with the same cap when not discrete.
    // source: where the value came from, for the log.
    //
    // igpu-safe: on such a GPU all the viewer gives the GPU comes out of the PC's memory, next
    // to what the viewer keeps itself (objects, meshes, the CPU copies of textures: some GB in a
    // busy place) and what Windows and other programs need. The texture budget is half of this
    // value (LLViewerTexture::updateClass), the sun's shadow maps a sixteenth (plus/shadowdist),
    // and LLVKDevice holds what the GPU takes from the PC to it (setSharedMemoryBudget). A quarter
    // let a 32 GB PC count 8 GB (textures up to 3.2 GB of the PC's memory, Ultra recommended for
    // a fast one); an eighth and at most 4 GB: 4 GB there, 2 GB on 16 GB, 1 GB on 8 GB.
    constexpr U64 SHARED_SHARE_DIVISOR = 8;
    constexpr U64 SHARED_SHARE_MAX = 4096ull << 20;

    U64 vram_bytes(const VkPhysicalDeviceProperties& gpu, const VkPhysicalDeviceIDProperties& ids, VkDeviceSize heap,
                   const char*& source)
    {
        if (gpu.vendorID == 0x10DE)
        {
            if (const U64 total = nvml_total_memory(ids.deviceUUID))
            {
                source = "NVML";
                return total;
            }
        }
        const bool integrated = gpu.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;   // shares the PC's memory
        U64 dedicated = 0, shared = 0;
        U64 bytes = heap;
        U64 set_aside = 0;      // the memory set aside for the GPU alone: never capped below it
        source = "the largest device local heap";
        if (gpu.vendorID != 0x10DE && ids.deviceLUIDValid && dxgi_adapter_memory(ids.deviceLUID, dedicated, shared))
        {
            const U64 adapter = integrated ? dedicated + shared : dedicated;
            if (adapter)
            {
                bytes = adapter;
                set_aside = integrated ? dedicated : 0;
                source = integrated ? "DXGI, the adapter with the device's LUID: dedicated and shared memory"
                                    : "DXGI, the adapter with the device's LUID: dedicated memory";
            }
        }
        if (integrated)
        {
            // max(dedicated, min(dedicated + shared, the share of the PC's memory)) (fix-round10 4,
            // igpu-safe). The memory set aside for the GPU in the firmware (an APU's "UMA frame
            // buffer": 96 GB of 128 on a Strix Halo, for one) is not in ullTotalPhys, so the share
            // of that alone may be far less than the GPU has.
            MEMORYSTATUSEX memory = { sizeof(memory) };
            if (GlobalMemoryStatusEx(&memory))
            {
                const U64 share = llmin(memory.ullTotalPhys / SHARED_SHARE_DIVISOR, SHARED_SHARE_MAX);
                const U64 capped = share > set_aside ? share : set_aside;
                if (capped < bytes)
                {
                    bytes = capped;
                    source = capped == share ? "not a discrete GPU: an eighth of the PC's memory, at most 4 GB"
                                             : "not a discrete GPU: the dedicated memory (more than the share of the PC's memory)";
                }
            }
        }
        return bytes;
    }

    GLint highest_sample_count(VkSampleCountFlags flags)
    {
        for (S32 bit = 6; bit >= 0; --bit)
        {
            if (flags & (1u << bit))
            {
                return 1 << bit;
            }
        }
        return 1;
    }

    QueryAnswers make_answers()
    {
        QueryAnswers a;
        LLVKDevice& device = LLVKDevice::instance();
        if (!device.isReady())
        {
            LL_WARNS("VKGL") << "GPU asked about before the Vulkan device is ready" << LL_ENDL;
            return a;
        }
        VkPhysicalDeviceIDProperties ids = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceDriverProperties driver = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        driver.pNext = &ids;
        VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props.pNext = &driver;
        vkGetPhysicalDeviceProperties2(device.getPhysicalDevice(), &props);
        const VkPhysicalDeviceLimits& limits = props.properties.limits;
        LLRHI::DeviceInfo& d = a.device;
        d.vendor_id = props.properties.vendorID;
        d.renderer = props.properties.deviceName;
        const std::string driver_info = driver.driverInfo;
        // The vendor as the GL drivers write it (the viewer matches "ATI ",
        // "NVIDIA ", "INTEL" in the upper cased string); the version's vendor
        // part after "4.6.0 " as they write it (the viewer keeps it as
        // mDriverVersionVendorString, research/117 1.4).
        std::string version_tail;
        switch (d.vendor_id)
        {
        case 0x10DE:
            d.vendor = "NVIDIA Corporation";
            version_tail = "NVIDIA " + driver_info;
            // NVIDIA's GL driver names the bus and CPU after the GPU (research/117 7.1:
            // the viewer keeps the whole string as LastGPUString).
            d.renderer += "/PCIe/SSE2";
            break;
        case 0x1002:
            d.vendor = "ATI Technologies Inc.";
            version_tail = "Compatibility Profile Context " + driver_info;
            break;
        case 0x8086:
            d.vendor = "Intel";
            version_tail = "- Build " + driver_info;
            break;
        default:
            {
                std::ostringstream name;
                name << "Vulkan vendor 0x" << std::hex << d.vendor_id;
                d.vendor = name.str();
                version_tail = std::string(driver.driverName) + " " + driver_info;
            }
            break;
        }
        d.version = "4.6.0 " + version_tail;
        d.glsl_version = d.vendor_id == 0x10DE ? "4.60 NVIDIA" : "4.60";

        VkPhysicalDeviceMemoryProperties memory = {};
        vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice(), &memory);
        VkDeviceSize vidmem = 0;
        for (U32 i = 0; i < memory.memoryHeapCount; ++i)
        {
            if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            {
                vidmem = llmax(vidmem, memory.memoryHeaps[i].size);
            }
        }
        // The VRAM of the GPU that renders, on every GPU (vram_bytes; research/124 V3).
        const char* vram_source = "";
        const U64 vram = vram_bytes(props.properties, ids, vidmem, vram_source);
        a.vidmem_kb = (GLint)llmin<U64>(vram / 1024, (U64)S32_MAX);
        d.vram_mb = (U32)(a.vidmem_kb / 1024);
        LL_INFOS("VKGL") << "VRAM of the GPU that renders: " << (vram >> 20) << " MB (" << vram_source << ")" << LL_ENDL;
        // igpu-safe: a GPU without VRAM of its own takes all it is given from the PC's memory:
        // the device holds it to this value (the budgets, where new memory goes).
        d.shared_memory = props.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (d.shared_memory)
        {
            device.setSharedMemoryBudget(vram);
        }

        // The translation keeps 32 texture units and 32 vertex attributes, 4 draw buffers.
        d.max_texture_units = (S32)llmin(32u, limits.maxPerStageDescriptorSamplers);
        d.max_color_texture_samples = highest_sample_count(limits.sampledImageColorSampleCounts);
        d.max_depth_texture_samples = highest_sample_count(limits.sampledImageDepthSampleCounts);
        d.max_integer_samples = highest_sample_count(limits.sampledImageIntegerSampleCounts);
        d.max_samples = highest_sample_count(limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts);
        d.max_sample_mask_words = 1;
        d.max_varying_vectors = (S32)(limits.maxFragmentInputComponents / 4);
        d.max_uniform_block_size = (S32)llmin(65536u, limits.maxUniformBufferRange);
        d.max_texture_size = (S32)limits.maxImageDimension2D;
        a.max_cube_size = (GLint)limits.maxImageDimensionCube;
        a.max_3d_size = (GLint)limits.maxImageDimension3D;
        a.max_layers = (GLint)limits.maxImageArrayLayers;
        a.max_vertex_attribs = (GLint)llmin(32u, limits.maxVertexInputAttributes);
        a.max_draw_buffers = (GLint)llmin(4u, limits.maxColorAttachments);
        // GL drivers (NVIDIA's: 1 to 10) allow less than Vulkan's range (1 to 64
        // there). LLRender clamps line widths to it (research/117 7.2); the
        // translation draws lines 1 wide either way.
        d.line_width_range[0] = limits.lineWidthRange[0];
        d.line_width_range[1] = llmin(limits.lineWidthRange[1], 10.f);
        d.max_anisotropy = limits.maxSamplerAnisotropy;
        a.ready = true;
        return a;
    }

    const QueryAnswers& answers()
    {
        static const QueryAnswers a = make_answers();
        return a;
    }

    // The values of pname into v (at most 4; count set); false: not answered here.
    bool int_values(GLenum pname, GLint* v, U32& count)
    {
        const QueryAnswers& a = answers();
        count = 1;
        switch (pname)
        {
        // Constants
        case GL_MAJOR_VERSION: v[0] = 4; return true;
        case GL_MINOR_VERSION: v[0] = 6; return true;
        case GL_NUM_PROGRAM_BINARY_FORMATS: v[0] = 0; return true;   // no GL program binaries
        case GL_MAX_TEXTURE_IMAGE_UNITS: v[0] = a.device.max_texture_units; return true;
        case GL_MAX_COLOR_TEXTURE_SAMPLES: v[0] = a.device.max_color_texture_samples; return true;
        case GL_MAX_DEPTH_TEXTURE_SAMPLES: v[0] = a.device.max_depth_texture_samples; return true;
        case GL_MAX_INTEGER_SAMPLES: v[0] = a.device.max_integer_samples; return true;
        case GL_MAX_SAMPLE_MASK_WORDS: v[0] = a.device.max_sample_mask_words; return true;
        case GL_MAX_SAMPLES: v[0] = a.device.max_samples; return true;
        case GL_MAX_VARYING_VECTORS: v[0] = a.device.max_varying_vectors; return true;
        case GL_MAX_UNIFORM_BLOCK_SIZE: v[0] = a.device.max_uniform_block_size; return true;
        case GL_MAX_ELEMENTS_VERTICES: v[0] = a.device.max_elements_vertices; return true;
        case GL_MAX_ELEMENTS_INDICES: v[0] = a.device.max_elements_indices; return true;
        case GL_MAX_TEXTURE_SIZE: v[0] = a.device.max_texture_size; return true;
        case GL_MAX_CUBE_MAP_TEXTURE_SIZE: v[0] = a.max_cube_size; return true;
        case GL_MAX_3D_TEXTURE_SIZE: v[0] = a.max_3d_size; return true;
        case GL_MAX_ARRAY_TEXTURE_LAYERS: v[0] = a.max_layers; return true;
        case GL_MAX_VERTEX_ATTRIBS: v[0] = a.max_vertex_attribs; return true;
        case GL_MAX_DRAW_BUFFERS: v[0] = a.max_draw_buffers; return true;
        case GL_MAX_COLOR_ATTACHMENTS: v[0] = a.max_draw_buffers; return true;
        case GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX: v[0] = a.vidmem_kb; return true;

        // State (per context, as tracked)
        case GL_VIEWPORT: memcpy(v, sState.viewport, sizeof(sState.viewport)); count = 4; return true;
        case GL_SCISSOR_BOX: memcpy(v, sState.scissor_rect, sizeof(sState.scissor_rect)); count = 4; return true;
        case GL_ACTIVE_TEXTURE: v[0] = (GLint)(GL_TEXTURE0 + sActiveUnit); return true;
        case GL_TEXTURE_BINDING_2D: v[0] = (GLint)sUnits[sActiveUnit].names[target_slot(GL_TEXTURE_2D)]; return true;
        case GL_TEXTURE_BINDING_CUBE_MAP: v[0] = (GLint)sUnits[sActiveUnit].names[target_slot(GL_TEXTURE_CUBE_MAP)]; return true;
        case GL_TEXTURE_BINDING_2D_ARRAY: v[0] = (GLint)sUnits[sActiveUnit].names[target_slot(GL_TEXTURE_2D_ARRAY)]; return true;
        case GL_TEXTURE_BINDING_CUBE_MAP_ARRAY: v[0] = (GLint)sUnits[sActiveUnit].names[target_slot(GL_TEXTURE_CUBE_MAP_ARRAY)]; return true;
        case GL_BLEND_SRC_RGB: v[0] = (GLint)sState.blend_src_rgb; return true;
        case GL_BLEND_DST_RGB: v[0] = (GLint)sState.blend_dst_rgb; return true;
        case GL_BLEND_SRC_ALPHA: v[0] = (GLint)sState.blend_src_a; return true;
        case GL_BLEND_DST_ALPHA: v[0] = (GLint)sState.blend_dst_a; return true;
        case GL_BLEND_EQUATION_RGB: v[0] = (GLint)sState.blend_eq_rgb; return true;
        case GL_BLEND_EQUATION_ALPHA: v[0] = (GLint)sState.blend_eq_a; return true;
        case GL_DEPTH_FUNC: v[0] = (GLint)sState.depth_func; return true;
        case GL_DEPTH_WRITEMASK: v[0] = sState.depth_write ? GL_TRUE : GL_FALSE; return true;
        case GL_CULL_FACE_MODE: v[0] = (GLint)sState.cull_face; return true;
        case GL_FRONT_FACE: v[0] = (GLint)sState.front_face; return true;
        case GL_POLYGON_MODE: v[0] = v[1] = (GLint)sState.polygon_mode; count = 2; return true;
        case GL_ARRAY_BUFFER_BINDING: v[0] = (GLint)sArrayBuffer; return true;
        case GL_ELEMENT_ARRAY_BUFFER_BINDING: v[0] = (GLint)sElementBuffer; return true;
        case GL_UNIFORM_BUFFER_BINDING: v[0] = (GLint)sUniformBuffer; return true;
        case GL_PIXEL_UNPACK_BUFFER_BINDING: v[0] = (GLint)sUnpackBuffer; return true;
        case GL_CURRENT_PROGRAM: v[0] = (GLint)sCurrentProgram; return true;
        case GL_VERTEX_ARRAY_BINDING: v[0] = (GLint)sVertexArray; return true;
        case GL_DRAW_FRAMEBUFFER_BINDING: v[0] = (GLint)sState.draw_framebuffer; return true;   // = GL_FRAMEBUFFER_BINDING
        case GL_READ_FRAMEBUFFER_BINDING: v[0] = (GLint)sState.read_framebuffer; return true;
        case GL_UNPACK_ALIGNMENT: v[0] = sUnpackAlignment; return true;
        case GL_UNPACK_ROW_LENGTH: v[0] = sUnpackRowLength; return true;
        case GL_PACK_ALIGNMENT: v[0] = sPackAlignment; return true;
        case GL_PACK_ROW_LENGTH: v[0] = sPackRowLength; return true;
        case GL_STENCIL_CLEAR_VALUE: v[0] = sState.clear_stencil; return true;
        default:
            return false;
        }
    }

    bool float_values(GLenum pname, GLfloat* v, U32& count)
    {
        const QueryAnswers& a = answers();
        count = 1;
        switch (pname)
        {
        case GL_ALIASED_LINE_WIDTH_RANGE:
        case GL_SMOOTH_LINE_WIDTH_RANGE:
            v[0] = a.device.line_width_range[0]; v[1] = a.device.line_width_range[1]; count = 2; return true;
        case GL_MAX_TEXTURE_MAX_ANISOTROPY: v[0] = a.device.max_anisotropy; return true;
        case GL_LINE_WIDTH: v[0] = sState.line_width; return true;
        case GL_COLOR_CLEAR_VALUE: memcpy(v, sState.clear_color, sizeof(sState.clear_color)); count = 4; return true;
        case GL_DEPTH_CLEAR_VALUE: v[0] = sState.clear_depth; return true;
        case GL_POLYGON_OFFSET_FACTOR: v[0] = sState.offset_factor; return true;
        case GL_POLYGON_OFFSET_UNITS: v[0] = sState.offset_units; return true;
        default:
            {
                GLint ints[4];
                if (!int_values(pname, ints, count))
                {
                    return false;
                }
                for (U32 i = 0; i < count; ++i)
                {
                    v[i] = (GLfloat)ints[i];
                }
                return true;
            }
        }
    }

    // Caps set_cap does not keep (GL_MULTISAMPLE, GL_DITHER, ...), as glEnable /
    // glDisable left them (per context like GL; GL's defaults otherwise).
    thread_local std::vector<std::pair<GLenum, bool>> sOtherCaps;

    bool cap_enabled(GLenum cap)
    {
        switch (cap)
        {
        case GL_BLEND: return sState.blend;
        case GL_CULL_FACE: return sState.cull;
        case GL_DEPTH_TEST: return sState.depth_test;
        case GL_SCISSOR_TEST: return sState.scissor;
        case GL_POLYGON_OFFSET_FILL: return sState.polygon_offset;
        case GL_DEPTH_CLAMP: return sState.depth_clamp;
        case GL_STENCIL_TEST: return sState.stencil;
        default:
            break;
        }
        for (const auto& other : sOtherCaps)
        {
            if (other.first == cap)
            {
                return other.second;
            }
        }
        return cap == GL_DITHER || cap == GL_MULTISAMPLE;   // GL's defaults
    }

    std::string pname_text(GLenum pname)
    {
        return hex(pname);
    }

    // Logged once per name, as unhandled() logs. A lock of its own, not
    // sLock: the queries of constants are answered where they are called
    // (the texture threads ask too).
    std::mutex sUnansweredMutex;
    std::unordered_set<std::string> sUnansweredOnce;

    void unanswered(const std::string& what)
    {
        std::lock_guard<std::mutex> lock(sUnansweredMutex);
        if (sUnansweredOnce.insert(what).second)
        {
            LL_INFOS("VKGL") << "not handled yet: " << what << " (not answered without GL, 0)" << LL_ENDL;
        }
    }
} // namespace

    void note_other_cap(GLenum cap, bool on)
    {
        for (auto& other : sOtherCaps)
        {
            if (other.first == cap)
            {
                other.second = on;
                return;
            }
        }
        sOtherCaps.emplace_back(cap, on);
    }

    void APIENTRY answer_glGetIntegerv(GLenum pname, GLint* data)
    {
        LLVKGL_REDIRECT_QUERY("glGetIntegerv", answer_glGetIntegerv(pname, data))     // reads GL state
        if (!data)
        {
            return;
        }
        GLint v[4];
        U32 count = 0;
        if (int_values(pname, v, count))
        {
            memcpy(data, v, count * sizeof(GLint));
            return;
        }
        unanswered("glGetIntegerv " + pname_text(pname));
        *data = 0;
    }

    void APIENTRY answer_glGetFloatv(GLenum pname, GLfloat* data)
    {
        LLVKGL_REDIRECT_QUERY("glGetFloatv", answer_glGetFloatv(pname, data))     // reads GL state
        if (!data)
        {
            return;
        }
        GLfloat v[4];
        U32 count = 0;
        if (float_values(pname, v, count))
        {
            memcpy(data, v, count * sizeof(GLfloat));
            return;
        }
        unanswered("glGetFloatv " + pname_text(pname));
        *data = 0.f;
    }

    void APIENTRY answer_glGetBooleanv(GLenum pname, GLboolean* data)
    {
        LLVKGL_REDIRECT_QUERY("glGetBooleanv", answer_glGetBooleanv(pname, data))     // reads GL state
        if (!data)
        {
            return;
        }
        if (pname == GL_COLOR_WRITEMASK)
        {
            for (U32 i = 0; i < 4; ++i)
            {
                data[i] = sState.color_mask[i] ? GL_TRUE : GL_FALSE;
            }
            return;
        }
        GLint v[4];
        U32 count = 0;
        if (int_values(pname, v, count))
        {
            for (U32 i = 0; i < count; ++i)
            {
                data[i] = v[i] ? GL_TRUE : GL_FALSE;
            }
            return;
        }
        unanswered("glGetBooleanv " + pname_text(pname));
        *data = GL_FALSE;
    }

    GLboolean APIENTRY answer_glIsEnabled(GLenum cap)
    {
        LLVKGL_REDIRECT_QUERY("glIsEnabled", answer_glIsEnabled(cap))     // reads GL state
        return cap_enabled(cap) ? GL_TRUE : GL_FALSE;
    }

    // The translation reports its own faults in the log (unhandled, skips):
    // GL's error queue stays empty, but for an image the device could not make
    // (raise_gl_error), which LLRenderTarget::allocate must see to fail.
    // Redirected: read after every call queued before it has made its images.
    // (Rare: the viewer reads it around allocations and shader loading, and
    // every frame only with RenderDebugGL.)
    GLenum APIENTRY answer_glGetError()
    {
        LLVKGL_REDIRECT_QUERY("glGetError", answer_glGetError())
        const GLenum error = tGLError;
        tGLError = GL_NO_ERROR;
        return error;
    }

    void raise_gl_error(GLenum error)
    {
        if (tGLError == GL_NO_ERROR)
        {
            tGLError = error;   // the first kept until read
        }
    }

    void APIENTRY answer_glGetBufferParameteriv(GLenum target, GLenum pname, GLint* params)
    {
        LLVKGL_REDIRECT_QUERY("glGetBufferParameteriv", answer_glGetBufferParameteriv(target, pname, params))     // reads GL state
        if (!params)
        {
            return;
        }
        {
            std::lock_guard<LLVKTrackerMutex> lock(sLock);
            const Buffer* buffer = bound_buffer(target);
            switch (pname)
            {
            case GL_BUFFER_SIZE: *params = buffer ? (GLint)llmin(buffer->data.size(), (size_t)S32_MAX) : 0; return;
            case GL_BUFFER_IMMUTABLE_STORAGE: *params = GL_FALSE; return;
            case GL_BUFFER_MAPPED: *params = GL_FALSE; return;
            case GL_BUFFER_USAGE: *params = GL_DYNAMIC_DRAW; return;
            default:
                break;
            }
        }
        unanswered("glGetBufferParameteriv " + pname_text(pname));
        *params = 0;
    }

} // namespace llvkgl

// research/207 T3: LLGLManager::initGL's GPU (llrhi.h).
bool LLVKGL::deviceInfo(LLRHI::DeviceInfo& out)
{
    const llvkgl::QueryAnswers& a = llvkgl::answers();
    if (!a.ready)
    {
        return false;
    }
    out = a.device;
    return true;
}

#else // !LL_GL_FUNC_POINTER

bool LLVKGL::deviceInfo(LLRHI::DeviceInfo&)
{
    return false;
}

#endif // LL_GL_FUNC_POINTER
