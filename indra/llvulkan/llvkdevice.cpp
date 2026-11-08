/**
 * @file llvkdevice.cpp
 * @brief Vulkan instance, physical/logical device and memory allocator.
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

#include "llvkdevice.h"
#include "llvkbindless.h"
#include "llvkqueues.h"
#include "fsyspath.h"
#include "hbxxh.h"
#include "llapp.h"
#include "llfile.h"
#include "llrhi.h"
#include "llrhihash.h"
#include "llrhiscene.h"     // research/197 stage 2: LLRHI_SCENE asks for the GPU-driven draws
#include "llvkscene.h"
#include "llvkrtas.h"      // research/230 R2
#include "llvkshader.h"

#if LL_WINDOWS
#include "llwin32headers.h"     // LoadLibraryExA: vulkan-1.dll is delay loaded (llvulkan.cmake)
#endif

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 1
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 0
#if LL_WINDOWS
#pragma warning(push)
#pragma warning(disable : 4100 4127 4189 4324 4505)
#endif
#include "vk_mem_alloc.h"
#if LL_WINDOWS
#pragma warning(pop)
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace
{
    const char* const VALIDATION_LAYER = "VK_LAYER_KHRONOS_validation";

    VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                  VkDebugUtilsMessageTypeFlagsEXT,
                                                  const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                  void*)
    {
        if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        {
            LLVKDevice::instance().countValidationError();
            LL_WARNS("Vulkan") << "validation error: " << data->pMessage << LL_ENDL;
        }
        else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        {
            LL_WARNS("Vulkan") << "validation warning: " << data->pMessage << LL_ENDL;
        }
        return VK_FALSE;
    }

    // On by default (research/109): a GPU fault then names what lived at the
    // address. Costs a callback per memory bind and unbind (allocations only)
    // and at most 131072 entries (5 MB). LLRHI_VK_ADDRESS_LOG=0: off.
    bool address_log_wanted()
    {
        return LLRHI::envFirstChar("LLRHI_VK_ADDRESS_LOG") != '0';
    }

    // VK_EXT_device_generated_commands (research/50 step 4): whether the probe
    // faces may use it, by LLRHI::sGPUDrivenDGCChoice and the chosen GPU's
    // vendor (dgc-nvidia-only, research/124 V5: checked on the RTX 2080 only,
    // so the default is on for NVIDIA's GPUs only). why: the reason, for the
    // log. The device asks for the extension when this and LLRHI::sGPUDriven
    // are on; off, it stays off the device and the faces draw with G5.
    constexpr U32 VENDOR_NVIDIA = 0x10DE;

    bool generated_commands_chosen(U32 vendor, std::string& why)
    {
        switch (LLRHI::sGPUDrivenDGCChoice)
        {
        case LLRHI::DGCChoice::EnvOn:
            why = "environment LLRHI_GPU_DRIVEN_DGC";
            return true;
        case LLRHI::DGCChoice::EnvOff:
            why = "environment LLRHI_GPU_DRIVEN_DGC=0";
            return false;
        case LLRHI::DGCChoice::On:
            why = "set on (PlusGpuDrivenDGC 1)";
            return true;
        case LLRHI::DGCChoice::Off:
            why = "set off (PlusGpuDrivenDGC 2)";
            return false;
        case LLRHI::DGCChoice::Auto:
        default:
            break;
        }
        if (vendor == VENDOR_NVIDIA)
        {
            why = "auto: NVIDIA GPU";
            return true;
        }
        std::ostringstream text;
        text << "auto: not an NVIDIA GPU (vendor 0x" << std::hex << vendor << "), PlusGpuDrivenDGC 1 turns it on";
        why = text.str();
        return false;
    }

    VKAPI_ATTR VkBool32 VKAPI_CALL address_callback(VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                    VkDebugUtilsMessageTypeFlagsEXT type,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                    void*)
    {
        if (!(type & VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT) || !data)
        {
            return VK_FALSE;
        }
        for (const VkBaseInStructure* s = (const VkBaseInStructure*)data->pNext; s; s = s->pNext)
        {
            if (s->sType == VK_STRUCTURE_TYPE_DEVICE_ADDRESS_BINDING_CALLBACK_DATA_EXT)
            {
                const VkDeviceAddressBindingCallbackDataEXT* b = (const VkDeviceAddressBindingCallbackDataEXT*)s;
                LLVKDevice::AddressEvent event;
                event.base = b->baseAddress;
                event.size = b->size;
                event.bound = b->bindingType == VK_DEVICE_ADDRESS_BINDING_TYPE_BIND_EXT;
                if (data->objectCount)
                {
                    event.handle = data->pObjects[0].objectHandle;
                    event.type = data->pObjects[0].objectType;
                }
                LLVKDevice::instance().addAddressEvent(event);
            }
        }
        return VK_FALSE;
    }

    // pipelines.vkpc: this header, then the driver's vkGetPipelineCacheData blob.
    // The size and hash catch a damaged file before the driver sees it.
    constexpr U32 PIPELINE_CACHE_MAGIC = 0x43504c4c;   // "LLPC"
    // 2: the hash is HBXXH64 (XXH3, 64 bits), not a byte-at-a-time FNV-1a (fix-logcache:
    // the 50 MB file took about 140 ms to hash at each save). A format 1 file
    // is still read (and written again as 2 at the next save).
    constexpr U32 PIPELINE_CACHE_FORMAT = 2;
    constexpr U32 PIPELINE_CACHE_FORMAT_FNV = 1;
    constexpr F64 PIPELINE_CACHE_SAVE_SECONDS = 60.0;
    // fix-logcache: the driver keeps every pipeline ever made in the cache, so
    // the file only grew (47.8 MB, then 53.7 MB a day later). A file with more
    // driver data than this is not read: the cache starts empty and holds what
    // the next sessions use. Checked by the file's size, before it is read.
    // Only on reading: the driver's data cannot be cut down to the pipelines
    // used lately (its entries are the driver's own format), so a save writes
    // it whole and the start after it begins empty. That must stay rare, so
    // the limit is well above what one session makes: fix-round5 counted the
    // 53.7 MB file of the development PC (session 133, 1142 kept pipeline
    // keys, after many shader edits): 1379 entries of 39 KB on average. One
    // session made about 300 (the prewarm's 256 and the new ones, about
    // 12 MB); all 1142 kept keys would be about 45 MB. 128 MB holds about
    // 3400 pipelines: emptied only after much has piled up (shader changes of
    // updates, content not met again), never at every start (64 MB was near
    // the development PC's kept set already).
    constexpr size_t PIPELINE_CACHE_MAX_BYTES = 128u << 20;
    // A periodic save waits until the driver data grew by this much since the
    // last read (smaller additions are saved at the exit, savePipelineCache).
    constexpr size_t PIPELINE_CACHE_SAVE_GROWTH = 1u << 20;
    // ... or until this long after the last whole read (the load, or a save).
    constexpr auto PIPELINE_CACHE_SAVE_LONGEST = std::chrono::minutes(10);
    // When the cache was last read whole: one device, one cache (createPipelineCache, the background save).
    std::atomic<std::chrono::steady_clock::time_point> sPipelineCacheLastFullRead{ std::chrono::steady_clock::time_point() };

    struct PipelineCacheFileHeader
    {
        U32 magic = PIPELINE_CACHE_MAGIC;
        U32 format = PIPELINE_CACHE_FORMAT;
        U64 size = 0;       // bytes of driver data that follow
        U64 hash = 0;       // of them: HBXXH64 (format 2), FNV-1a (format 1)
    };

    U64 pipeline_cache_hash(U32 format, const void* data, size_t size)
    {
        return format == PIPELINE_CACHE_FORMAT_FNV ? LLRHI::fnv1a(LLRHI::FNV1A_BASIS, data, size) : HBXXH64::digest(data, size);
    }

    bool pipeline_cache_wanted()
    {
        return LLRHI::envFirstChar("LLRHI_PIPELINE_CACHE") != '0';
    }

    // A saved cache: the whole file in `data`, the driver data after its
    // header (sizeof(PipelineCacheFileHeader) on: not copied out, it is tens
    // of MB). False, and `data` empty, when the file is missing, over
    // PIPELINE_CACHE_MAX_BYTES, damaged, or was written by another GPU or driver.
    bool read_pipeline_cache(const std::string& path, const VkPhysicalDeviceProperties& props, std::string& data, U64& hash)
    {
        data.clear();
        PipelineCacheFileHeader header;
        std::error_code size_ec;
        const uintmax_t file_size = std::filesystem::file_size(fsyspath(path), size_ec);
        if (!size_ec && file_size > PIPELINE_CACHE_MAX_BYTES + sizeof(header))
        {
            LL_INFOS("Vulkan") << "pipeline cache " << path << " is " << (file_size >> 20) << " MB, over the "
                               << (PIPELINE_CACHE_MAX_BYTES >> 20) << " MB limit; starting empty" << LL_ENDL;
            return false;
        }
        if (!LLVKShader::readCacheFile(path, data))
        {
            data.clear();
            return false;
        }

        if (data.size() < sizeof(header))
        {
            LL_INFOS("Vulkan") << "pipeline cache " << path << " is truncated; discarded" << LL_ENDL;
            data.clear();
            return false;
        }
        memcpy(&header, data.data(), sizeof(header));
        const char* blob = data.data() + sizeof(header);
        const size_t blob_size = data.size() - sizeof(header);
        if (header.magic != PIPELINE_CACHE_MAGIC || (header.format != PIPELINE_CACHE_FORMAT && header.format != PIPELINE_CACHE_FORMAT_FNV)
            || header.size != blob_size || blob_size > PIPELINE_CACHE_MAX_BYTES
            || header.hash != pipeline_cache_hash(header.format, blob, blob_size))
        {
            LL_INFOS("Vulkan") << "pipeline cache " << path << " is damaged, of another format or over the limit; discarded" << LL_ENDL;
            data.clear();
            return false;
        }

        VkPipelineCacheHeaderVersionOne driver;
        if (blob_size < sizeof(driver))
        {
            LL_INFOS("Vulkan") << "pipeline cache " << path << " has no driver header; discarded" << LL_ENDL;
            data.clear();
            return false;
        }
        memcpy(&driver, blob, sizeof(driver));
        if (driver.headerSize < sizeof(driver) || driver.headerSize > blob_size
            || driver.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE
            || driver.vendorID != props.vendorID || driver.deviceID != props.deviceID
            || memcmp(driver.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE))
        {
            LL_INFOS("Vulkan") << "pipeline cache " << path << " is from another GPU or driver; discarded" << LL_ENDL;
            data.clear();
            return false;
        }
        // A format 1 file's hash is FNV-1a: never equal to the HBXXH64 of the
        // same data, so the next save writes it again as format 2.
        hash = header.hash;
        return true;
    }

    // Header, hash, then write and rename (LLVKShader::writeCacheFile: a
    // temporary file of this process and thread, so a second viewer writing
    // at the same time cannot mix into it), so the cache file is never half
    // written. Skips data identical to what the file already holds.
    void write_pipeline_cache(const std::string& path, const std::vector<U8>& data, std::atomic<U64>& file_hash,
                              const char* where)
    {
        const auto start = std::chrono::steady_clock::now();
        PipelineCacheFileHeader header;
        header.size = data.size();
        header.hash = pipeline_cache_hash(header.format, data.data(), data.size());
        if (header.hash == file_hash.load())
        {
            return;     // only cache hits since the last save
        }

        if (!LLVKShader::writeCacheFile(path, { { &header, sizeof(header) }, { data.data(), data.size() } }))
        {
            LL_WARNS("Vulkan") << "pipeline cache could not be written to " << path << LL_ENDL;
            return;
        }
        file_hash = header.hash;
        LL_INFOS("Vulkan") << "pipeline cache: saved " << data.size() << " bytes, written in "
                           << std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - start).count()
                           << " ms (" << where << ")"
                           << (data.size() > PIPELINE_CACHE_MAX_BYTES ? "; over the limit, the next start begins empty" : "")
                           << LL_ENDL;
    }

    bool has_layer(const char* name)
    {
        U32 count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layers(count);
        vkEnumerateInstanceLayerProperties(&count, layers.data());
        for (const VkLayerProperties& layer : layers)
        {
            if (!strcmp(layer.layerName, name))
            {
                return true;
            }
        }
        return false;
    }

    // An instance extension the loader or a driver offers without any layer.
    bool has_instance_extension(const char* name)
    {
        U32 count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> extensions(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
        for (const VkExtensionProperties& extension : extensions)
        {
            if (!strcmp(extension.extensionName, name))
            {
                return true;
            }
        }
        return false;
    }

    // What the renderer is built around and has no other way for, checked for
    // each Vulkan 1.3 GPU before one is chosen (pickPhysicalDevice), so a PC
    // with two GPUs uses one that has everything. research/82: the bindless
    // table is update-after-bind, reflection probes are cube arrays, shadow
    // passes clamp depth, anisotropic filtering. research/124 V1 (required
    // since 2026-10-21): VK_EXT_depth_clip_control. The translated GL draws
    // keep GL's -1..1 clip depth (negativeOneToOne) and the official deferred
    // lighting reads the depth back as GL's (deferredUtil.glsl), so without it
    // every depth is off, and no shader rewrite stands in for it.
    // The missing ones, ", " between them, each as "<name> (<what has it>)";
    // empty when nothing is missing.
    std::string missing_required(VkPhysicalDevice device)
    {
        U32 ext_count = 0;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> available(ext_count);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &ext_count, available.data());
        available.resize(ext_count);
        auto has_extension = [&available](const char* name)
        {
            return std::any_of(available.begin(), available.end(),
                               [name](const VkExtensionProperties& ext) { return !strcmp(ext.extensionName, name); });
        };
        const bool has_swapchain = has_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        const bool has_clip_control = has_extension(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);

        VkPhysicalDeviceVulkan12Features have12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features have13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceDepthClipControlFeaturesEXT clip_control = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT };
        have12.pNext = &have13;
        if (has_clip_control)
        {
            have13.pNext = &clip_control;   // an extension's structure only where the GPU offers it
        }
        VkPhysicalDeviceFeatures2 have = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        have.pNext = &have12;
        vkGetPhysicalDeviceFeatures2(device, &have);

        const struct
        {
            bool have;
            const char* name;
        } required[] = {
            { has_swapchain, "swapchain (VK_KHR_swapchain)" },
            { has_clip_control && clip_control.depthClipControl, "depth clip control (VK_EXT_depth_clip_control)" },
            { have13.dynamicRendering == VK_TRUE, "dynamicRendering (Vulkan 1.3)" },
            { have13.synchronization2 == VK_TRUE, "synchronization2 (Vulkan 1.3)" },
            { have12.descriptorIndexing == VK_TRUE, "descriptorIndexing (Vulkan 1.2)" },
            { have12.runtimeDescriptorArray == VK_TRUE, "runtimeDescriptorArray (Vulkan 1.2)" },
            { have12.descriptorBindingPartiallyBound == VK_TRUE, "descriptorBindingPartiallyBound (Vulkan 1.2)" },
            { have12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE, "shaderSampledImageArrayNonUniformIndexing (Vulkan 1.2)" },
            { have12.bufferDeviceAddress == VK_TRUE, "bufferDeviceAddress (Vulkan 1.2)" },
            { have12.timelineSemaphore == VK_TRUE, "timelineSemaphore (Vulkan 1.2)" },
            { have12.descriptorBindingSampledImageUpdateAfterBind == VK_TRUE, "descriptorBindingSampledImageUpdateAfterBind (Vulkan 1.2)" },
            { have.features.imageCubeArray == VK_TRUE, "imageCubeArray (Vulkan 1.0)" },
            { have.features.depthClamp == VK_TRUE, "depthClamp (Vulkan 1.0)" },
            { have.features.samplerAnisotropy == VK_TRUE, "samplerAnisotropy (Vulkan 1.0)" },
        };
        std::string missing;
        for (const auto& feature : required)
        {
            if (!feature.have)
            {
                missing += (missing.empty() ? "" : ", ");
                missing += feature.name;
            }
        }
        // fix-round6 2: the bindless texture table (LLVKBindless) needs room
        // for MIN_TABLE_SIZE update-after-bind textures. Conformant drivers
        // with descriptor indexing allow far more (500000); a lower limit is
        // treated as a missing feature, never a table larger than allowed.
        VkPhysicalDeviceDescriptorIndexingProperties indexing = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES };
        VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props.pNext = &indexing;
        vkGetPhysicalDeviceProperties2(device, &props);
        const U32 table_limit = LLVKBindless::tableLimit(indexing);
        if (table_limit < LLVKBindless::MIN_TABLE_SIZE)
        {
            missing += (missing.empty() ? "" : ", ");
            // No ", " inside: plus/vkcheck splits the list at it.
            missing += std::to_string(LLVKBindless::MIN_TABLE_SIZE) + " update-after-bind textures (descriptor indexing limits: this GPU allows " +
                       std::to_string(table_limit) + ")";
        }
        // research/197 2.4 (J3): the retained scene's GPU-driven draws, required
        // whenever LLRHI_SCENE is not off (its default turns auto with the
        // scene's drawing, research/198 5; the old ways stay until stage 7).
        if (LLRHI::sceneMode() != LLRHI::SceneMode::Off)
        {
            const std::string scene = LLVKScene::deviceGaps(have.features, have12.drawIndirectCount == VK_TRUE, props.properties.limits);
            if (!scene.empty())
            {
                missing += (missing.empty() ? "" : ", ");
                missing += scene;
            }
        }
        return missing;
    }
}

LLVKDevice& LLVKDevice::instance()
{
    static LLVKDevice device;
    return device;
}

bool LLVKDevice::init(bool validation)
{
    if (isReady())
    {
        return true;
    }

    mUnsupported.clear();
    mLacksRequiredFeatures = false;
    mPassedOverNotes.clear();
    if (!createInstance(validation) || !pickPhysicalDevice() || !createDevice() || !createAllocator())
    {
        // A GPU passed over for its features keeps mLacksRequiredFeatures
        // even when the other GPU then failed: its report lists both.
        mPassedOverNotes.clear();
        shutdown();
        if (mUnsupported.empty())
        {
            addUnsupported("Vulkan initialization failed (see the Vulkan lines above)");
        }
        LL_WARNS("Vulkan") << "Vulkan renderer unavailable on this PC:\n" << mUnsupported << LL_ENDL;
        return false;
    }
    mUnsupported.clear();   // e.g. a second, older GPU that was passed over
    mLacksRequiredFeatures = false;     // a GPU passed over does not count when another is used
    // Not shown on screen (the viewer runs, on the other GPU): the log says
    // which GPU was skipped, for what, and which one is used.
    for (const std::string& note : mPassedOverNotes)
    {
        LL_WARNS("Vulkan") << note << LL_ENDL;
    }
    mPassedOverNotes.clear();
    createPipelineCache();

    LL_INFOS("Vulkan") << "Vulkan device ready: " << mDeviceName << (mMessenger ? " (validation on)" : "") << LL_ENDL;
    return true;
}

void LLVKDevice::addUnsupported(const std::string& reason)
{
    if (!mUnsupported.empty())
    {
        mUnsupported += "\n";
    }
    mUnsupported += reason;
}

bool LLVKDevice::createInstance(bool validation)
{
#if LL_WINDOWS
    // vulkan-1.dll comes with the GPU driver and is delay loaded, so a PC
    // without a Vulkan driver gets the viewer's message instead of a missing
    // DLL error. Before the first vk call: a missing DLL would raise an
    // exception there. From System32 only, where the driver installs it (not
    // a vulkan-1.dll in the working folder); the delay load helper then uses
    // the module already loaded.
    if (!LoadLibraryExA("vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
    {
        LL_WARNS("Vulkan") << "vulkan-1.dll not found: no Vulkan driver installed" << LL_ENDL;
        addUnsupported("No Vulkan driver (vulkan-1.dll not found)");
        return false;
    }
#endif
    // Through vkGetInstanceProcAddr: a Vulkan 1.0 loader has no vkEnumerateInstanceVersion
    // (and the delay loaded import would raise an exception).
    U32 api_version = VK_API_VERSION_1_0;
    auto enumerate_version = (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    if (enumerate_version && enumerate_version(&api_version) != VK_SUCCESS)
    {
        api_version = VK_API_VERSION_1_0;
    }
    if (api_version < VK_API_VERSION_1_3)
    {
        LL_WARNS("Vulkan") << "Vulkan 1.3 loader not available" << LL_ENDL;
        std::ostringstream reason;
        reason << "Vulkan runtime is " << VK_API_VERSION_MAJOR(api_version) << "." << VK_API_VERSION_MINOR(api_version) << "."
               << VK_API_VERSION_PATCH(api_version) << ", 1.3 or later is needed (update the graphics driver)";
        addUnsupported(reason.str());
        return false;
    }

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Charis Viewer";
    app.pEngineName = "llvulkan";
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> layers;
    // Surface extensions are needed for VK_KHR_swapchain on the device.
    std::vector<const char*> extensions = { VK_KHR_SURFACE_EXTENSION_NAME };
#if LL_WINDOWS
    extensions.push_back("VK_KHR_win32_surface");
#endif
    // On by default now (research/109): only where the extension is offered,
    // so an old loader never fails the instance over a diagnostic.
    const bool address_log = address_log_wanted() && has_instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (address_log)
    {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    if (validation)
    {
        if (has_layer(VALIDATION_LAYER))
        {
            layers.push_back(VALIDATION_LAYER);
            if (!address_log)
            {
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            }
        }
        else
        {
            LL_WARNS("Vulkan") << "Validation requested but " << VALIDATION_LAYER << " is not installed" << LL_ENDL;
        }
    }

    VkInstanceCreateInfo info = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    info.pApplicationInfo = &app;
    info.enabledLayerCount = (U32)layers.size();
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = (U32)extensions.size();
    info.ppEnabledExtensionNames = extensions.data();

    VkResult result = vkCreateInstance(&info, nullptr, &mInstance);
    if (result != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "vkCreateInstance failed: " << result << LL_ENDL;
        addUnsupported("Vulkan instance could not be created (error " + std::to_string((S32)result) + ")");
        mInstance = VK_NULL_HANDLE;
        return false;
    }

    if (!layers.empty())
    {
        auto create_messenger = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(mInstance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT messenger = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger.pfnUserCallback = debug_callback;
        if (create_messenger)
        {
            create_messenger(mInstance, &messenger, nullptr, &mMessenger);
        }
    }
    if (address_log)
    {
        auto create_messenger = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(mInstance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT messenger = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_DEVICE_ADDRESS_BINDING_BIT_EXT;
        messenger.pfnUserCallback = address_callback;
        if (create_messenger)
        {
            // Lives as long as the instance: shutdown() destroys it before vkDestroyInstance.
            create_messenger(mInstance, &messenger, nullptr, &mAddressMessengerHandle);
        }
        mAddressMessenger = mAddressMessengerHandle != VK_NULL_HANDLE;
    }
    return true;
}

bool LLVKDevice::pickPhysicalDevice()
{
    U32 count = 0;
    vkEnumeratePhysicalDevices(mInstance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(mInstance, &count, devices.data());

    // Prefer a discrete GPU with Vulkan 1.3; fall back to any 1.3 device. A
    // GPU without everything required (missing_required) is passed over, so
    // on a PC with two GPUs the other one is used (research/124 V1, L4); among
    // the GPUs that have everything the choice is as before.
    S32 best_score = -1;
    // A discrete GPU first, then the others; CPU rendering (lavapipe,
    // SwiftShader) last (fix-round11 4): at the same score the one listed
    // first stays, and CPU rendering listed before an integrated GPU was
    // chosen over it (and then dropped below for a GPU passed over, leaving
    // none although the integrated GPU works).
    auto device_score = [](VkPhysicalDeviceType type) -> S32
    {
        return type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : type == VK_PHYSICAL_DEVICE_TYPE_CPU ? 0 : 1;
    };
    // The real GPUs (not CPU rendering) passed over for lacking required
    // features, for the log line once another GPU is chosen.
    struct PassedOver
    {
        VkPhysicalDeviceType type;
        std::string name;
        std::string missing;
    };
    std::vector<PassedOver> gpus_passed_over;
    VkPhysicalDeviceType chosen_type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    // LLRHI_VK_DEVICE=<part of a GPU's name, any case>: only a GPU with that in its name is considered
    // (to try the integrated GPU on a PC that also has a discrete one). The others are listed in the log.
    std::string only_device;
    if (const char* only_env = std::getenv("LLRHI_VK_DEVICE"))
    {
        only_device = only_env;
        std::transform(only_device.begin(), only_device.end(), only_device.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    }
    for (VkPhysicalDevice device : devices)
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);
        if (!only_device.empty())
        {
            std::string lower = props.deviceName;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            LL_INFOS("Vulkan") << "GPU " << props.deviceName << (lower.find(only_device) == std::string::npos ? ": skipped" : ": considered")
                               << " (LLRHI_VK_DEVICE=" << only_device << ")" << LL_ENDL;
            if (lower.find(only_device) == std::string::npos)
            {
                continue;
            }
        }
        if (props.apiVersion < VK_API_VERSION_1_3)
        {
            std::ostringstream reason;
            reason << props.deviceName << ": Vulkan " << VK_API_VERSION_MAJOR(props.apiVersion) << "."
                   << VK_API_VERSION_MINOR(props.apiVersion) << "." << VK_API_VERSION_PATCH(props.apiVersion)
                   << ", 1.3 or later is needed";
            addUnsupported(reason.str());
            continue;
        }
        const std::string missing = missing_required(device);
        if (!missing.empty())
        {
            LL_WARNS("Vulkan") << "GPU " << props.deviceName << " passed over, it lacks required Vulkan features: " << missing << LL_ENDL;
            addUnsupported(std::string(props.deviceName) + ": missing " + missing);
            // CPU rendering lacking a feature is not what the user is told
            // about: with no real GPU passed over, the reason is elsewhere
            // (no GPU with Vulkan 1.3), and that message is the right one.
            if (props.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
            {
                mLacksRequiredFeatures = true;
                gpus_passed_over.push_back({ props.deviceType, props.deviceName, missing });
            }
            continue;
        }
        const S32 score = device_score(props.deviceType);
        if (score > best_score)
        {
            best_score = score;
            mPhysicalDevice = device;
            mDeviceName = props.deviceName;
            mLimits = props.limits;
            chosen_type = props.deviceType;
        }
    }
    // CPU rendering (lavapipe, SwiftShader, on a developer's PC) does not
    // stand in for a GPU that was passed over only for its features: the user
    // is told what the GPU lacks instead of getting a few frames a second. As
    // before, it is used when no GPU has Vulkan 1.3 at all.
    if (mPhysicalDevice != VK_NULL_HANDLE && chosen_type == VK_PHYSICAL_DEVICE_TYPE_CPU && !gpus_passed_over.empty())
    {
        LL_WARNS("Vulkan") << "CPU rendering (" << mDeviceName << ") not used in place of a GPU that lacks required features" << LL_ENDL;
        addUnsupported(mDeviceName + ": CPU (software) rendering, not used in place of the GPU");
        mPhysicalDevice = VK_NULL_HANDLE;
        mDeviceName.clear();
        mLimits = {};
    }

    if (mPhysicalDevice == VK_NULL_HANDLE)
    {
        LL_WARNS("Vulkan") << "No GPU with Vulkan 1.3 and the required features found" << LL_ENDL;
        if (devices.empty())
        {
            addUnsupported("No GPU with a Vulkan driver found");
        }
        return false;
    }
    // A GPU passed over that would have been chosen otherwise (its score is
    // not below the chosen one's: a discrete GPU skipped for an integrated
    // one) gets a line for the log once init() succeeds, as in
    // "The discrete GPU "<name>" lacks Vulkan features the viewer requires:
    // depth clip control (VK_EXT_depth_clip_control). The integrated GPU
    // "<name>" is used instead". An integrated GPU skipped beside a discrete
    // one changes nothing and has only the "passed over" line above.
    auto gpu_kind = [](VkPhysicalDeviceType type)
    {
        return type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete GPU"
            : type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated GPU"
            : type == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? "virtual GPU"
            : "GPU";
    };
    for (const PassedOver& gpu : gpus_passed_over)
    {
        if (device_score(gpu.type) >= best_score)
        {
            mPassedOverNotes.push_back(std::string("The ") + gpu_kind(gpu.type) + " \"" + gpu.name +
                                       "\" lacks Vulkan features the viewer requires: " + gpu.missing + ". The " +
                                       gpu_kind(chosen_type) + " \"" + mDeviceName + "\" is used instead (research/124 V1)");
        }
    }
    // If the chosen GPU then fails (no graphics queue, createDevice), the
    // user is told about the missing features only when the GPU passed over
    // for them would have been the one; init() clears this on success.
    mLacksRequiredFeatures = !mPassedOverNotes.empty();

    U32 family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &family_count, families.data());
    for (U32 i = 0; i < family_count; ++i)
    {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            mGraphicsQueueFamily = i;
            return true;
        }
    }

    LL_WARNS("Vulkan") << "GPU " << mDeviceName << " has no graphics queue" << LL_ENDL;
    addUnsupported(mDeviceName + ": no graphics queue");
    return false;
}

bool LLVKDevice::createDevice()
{
    // The required features were checked when the GPU was chosen
    // (missing_required); this asks again for the optional ones.
    VkPhysicalDeviceVulkan12Features have12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan13Features have13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    have12.pNext = &have13;
    VkPhysicalDeviceFeatures2 have = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    have.pNext = &have12;
    vkGetPhysicalDeviceFeatures2(mPhysicalDevice, &have);

    VkPhysicalDeviceVulkan12Features want12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    want12.descriptorIndexing = VK_TRUE;
    want12.runtimeDescriptorArray = VK_TRUE;
    want12.descriptorBindingPartiallyBound = VK_TRUE;
    want12.descriptorBindingVariableDescriptorCount = have12.descriptorBindingVariableDescriptorCount;
    want12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    want12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    want12.bufferDeviceAddress = VK_TRUE;
    want12.timelineSemaphore = VK_TRUE;
    want12.drawIndirectCount = have12.drawIndirectCount;
    VkPhysicalDeviceVulkan13Features want13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    want13.dynamicRendering = VK_TRUE;
    want13.synchronization2 = VK_TRUE;
    // GLSL discard compiles to OpDemoteToHelperInvocation under Vulkan rules.
    want13.shaderDemoteToHelperInvocation = have13.shaderDemoteToHelperInvocation;
    // terminateInvocation: the alpha shader's early out (research/54).
    want13.shaderTerminateInvocation = have13.shaderTerminateInvocation;
    want12.pNext = &want13;
    VkPhysicalDeviceFeatures2 want = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    want.pNext = &want12;
    want.features.multiDrawIndirect = have.features.multiDrawIndirect;
    want.features.drawIndirectFirstInstance = have.features.drawIndirectFirstInstance;     // GPU-driven draws (research/46)
    want.features.geometryShader = have.features.geometryShader;
    want.features.samplerAnisotropy = have.features.samplerAnisotropy;
    want.features.fillModeNonSolid = have.features.fillModeNonSolid;
    want.features.wideLines = have.features.wideLines;
    want.features.depthClamp = have.features.depthClamp;
    // The sun shadow casters' slope-scaled bias is clamped where it can be
    // (LLRHI::sShadowDepthBias, plus/shadowdist); without it the clamp stays 0.
    want.features.depthBiasClamp = have.features.depthBiasClamp;
    LLRHI::sShadowDepthBiasClamp = have.features.depthBiasClamp == VK_TRUE;
    want.features.textureCompressionBC = have.features.textureCompressionBC;
    want.features.imageCubeArray = have.features.imageCubeArray;   // reflection probes
    want.features.shaderClipDistance = have.features.shaderClipDistance;
    want.features.independentBlend = have.features.independentBlend;
    want.features.dualSrcBlend = have.features.dualSrcBlend;
    want.features.fragmentStoresAndAtomics = have.features.fragmentStoresAndAtomics;   // occlusion marks
    // The alpha census's fragment counts (research/54): only when asked for at startup.
    want.features.pipelineStatisticsQuery = LLRHI::sAlphaCensus && have.features.pipelineStatisticsQuery;
    // LLRHI_VK_ROBUST=1: out-of-bounds buffer reads return zeros instead of faulting
    // (for finding faults: GL drivers tolerate what Vulkan does not).
    want.features.robustBufferAccess = LLRHI::envFirstChar("LLRHI_VK_ROBUST") == '1' && have.features.robustBufferAccess;
    if (want.features.robustBufferAccess)
    {
        LL_INFOS("Vulkan") << "robust buffer access on" << LL_ENDL;
    }
    mFragmentStores = have.features.fragmentStoresAndAtomics;
    mFillModeNonSolid = have.features.fillModeNonSolid == VK_TRUE;
    mPipelineStatistics = want.features.pipelineStatisticsQuery;
    mMultiDrawIndirect = have.features.multiDrawIndirect;
    mDrawIndirectFirstInstance = have.features.drawIndirectFirstInstance;
    mDrawIndirectCount = have12.drawIndirectCount;
    {
        VkPhysicalDeviceVulkan11Properties props11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES };
        VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props.pNext = &props11;
        vkGetPhysicalDeviceProperties2(mPhysicalDevice, &props);
        mAlphaEarlyOut = have13.shaderTerminateInvocation && have13.shaderDemoteToHelperInvocation &&
                         (props11.subgroupSupportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) &&
                         (props11.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT);
        mFragmentSubgroupBasic = (props11.subgroupSupportedStages & VK_SHADER_STAGE_FRAGMENT_BIT) &&
                                 (props11.subgroupSupportedOperations & VK_SUBGROUP_FEATURE_BASIC_BIT);
    }

    float priority = 1.f;
    VkDeviceQueueCreateInfo queue = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    queue.queueFamilyIndex = mGraphicsQueueFamily;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;

    std::vector<const char*> extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

    U32 ext_count = 0;
    vkEnumerateDeviceExtensionProperties(mPhysicalDevice, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> available(ext_count);
    vkEnumerateDeviceExtensionProperties(mPhysicalDevice, nullptr, &ext_count, available.data());
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip_control = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT };
    VkPhysicalDeviceFaultFeaturesEXT fault = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
    VkPhysicalDeviceAddressBindingReportFeaturesEXT address_report = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ADDRESS_BINDING_REPORT_FEATURES_EXT };
    const bool want_address_log = mAddressMessenger;   // research/109: only with the instance's messenger
    void** chain = &want13.pNext;
    const bool want_checkpoints = LLRHI::envFirstChar("LLRHI_VK_CHECKPOINTS") == '1';
    // bc7-fault2: the driver's figures for chosen pipelines (pipelineStatistics).
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pipeline_stats = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR };
    const bool want_pipeline_stats = LLRHI::envFirstChar("LLRHI_VK_PIPELINE_STATS") == '1';

    // Required (missing_required, research/124 V1): GL's -1..1 clip depth in
    // every pipeline (VkPipelineViewportDepthClipControlCreateInfoEXT).
    extensions.push_back(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    clip_control.depthClipControl = VK_TRUE;
    *chain = &clip_control;
    chain = &clip_control.pNext;

    // Device generated commands: the extension, VK_KHR_maintenance5 (its
    // VkPipelineCreateFlags2 / VkBufferUsageFlags2) and both features, only
    // when all are there and they are wanted (generated_commands_chosen).
    // Decided once here, for the chosen GPU (on a PC with two, the one that
    // renders). LLRHI::sGPUDrivenDGC takes the choice, as LLRHI_GPU_DRIVEN_DGC=0
    // set it before, so an off choice leaves the probe faces on G5 the same
    // way. One "DGC: on/off (why)" line in the log.
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT dgc_want = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT };
    VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5_want = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as_want = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
    VkPhysicalDeviceRayQueryFeaturesKHR rq_want = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
    std::string dgc_why;
    {
        VkPhysicalDeviceProperties gpu;
        vkGetPhysicalDeviceProperties(mPhysicalDevice, &gpu);
        LLRHI::sGPUDrivenDGC = generated_commands_chosen(gpu.vendorID, dgc_why);
    }
    if (!LLRHI::sGPUDrivenDGC)
    {
        LL_INFOS("Vulkan") << "DGC: off (" << dgc_why << ")" << LL_ENDL;
    }
    else if (!LLRHI::sGPUDriven)
    {
        // Off for this start: the switch says what runs (gpudgc.toggle can still turn it
        // on, but nothing generates commands without the extension).
        LLRHI::sGPUDrivenDGC = false;
        LL_INFOS("Vulkan") << "DGC: off (GPU-driven drawing off: LLRHI_GPU_DRIVEN=0)" << LL_ENDL;
    }
    else
    {
        bool has_dgc = false, has_maintenance5 = false;
        for (const VkExtensionProperties& ext : available)
        {
            has_dgc |= !strcmp(ext.extensionName, VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME);
            has_maintenance5 |= !strcmp(ext.extensionName, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
        }
        bool usable = false;
        if (has_dgc && has_maintenance5)
        {
            VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT dgc_have = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT };
            VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5_have = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR };
            dgc_have.pNext = &maintenance5_have;
            VkPhysicalDeviceFeatures2 query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            query.pNext = &dgc_have;
            vkGetPhysicalDeviceFeatures2(mPhysicalDevice, &query);
            mDGC.properties = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_PROPERTIES_EXT };
            VkPhysicalDeviceProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
            props.pNext = &mDGC.properties;
            vkGetPhysicalDeviceProperties2(mPhysicalDevice, &props);
            const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            usable = dgc_have.deviceGeneratedCommands && maintenance5_have.maintenance5 && mMultiDrawIndirect &&
                     mDrawIndirectFirstInstance && (mDGC.properties.supportedIndirectCommandsShaderStages & stages) == stages &&
                     mDGC.properties.maxIndirectCommandsTokenCount >= 2 + GeneratedCommands::MAX_VERTEX_TOKENS &&
                     mDGC.properties.maxIndirectSequenceCount >= 1;
        }
        if (usable)
        {
            extensions.push_back(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME);
            extensions.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
            dgc_want.deviceGeneratedCommands = VK_TRUE;
            maintenance5_want.maintenance5 = VK_TRUE;
            *chain = &dgc_want;
            dgc_want.pNext = &maintenance5_want;
            chain = &maintenance5_want.pNext;
            mDGCEnabled = true;
            mMaintenance5 = true;
            LL_INFOS("Vulkan") << "DGC: on (" << dgc_why << ")" << LL_ENDL;
        }
        else
        {
            LLRHI::sGPUDrivenDGC = false;   // as above
            LL_INFOS("Vulkan") << "DGC: off (the GPU does not support it: extension " << (has_dgc ? "present" : "missing")
                               << ", maintenance5 " << (has_maintenance5 ? "present" : "missing") << "; wanted by " << dgc_why
                               << ")" << LL_ENDL;
        }
    }

    // Hardware ray tracing (research/230 R1): an optional capability, as DGC
    // above. Enabled whenever the GPU has all three extensions and the
    // features (the extensions cost nothing until an acceleration structure is
    // built); LLRHI_VK_RAYTRACING=0 leaves it out. bufferDeviceAddress is
    // already required. One "RayTracing: on/off (why)" line in the log.
    {
        mRayTracing = false;   // a second createDevice starts from off
        const auto has_ext = [&](const char* name)
        {
            return std::any_of(available.begin(), available.end(), [name](const VkExtensionProperties& ext)
                               { return !strcmp(ext.extensionName, name); });
        };
        if (LLRHI::envFirstChar("LLRHI_VK_RAYTRACING") == '0')
        {
            mRayTracingStatus = "off (LLRHI_VK_RAYTRACING=0)";
        }
        else if (!has_ext(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) || !has_ext(VK_KHR_RAY_QUERY_EXTENSION_NAME) ||
                 !has_ext(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME))
        {
            mRayTracingStatus = "off (this GPU does not support ray queries)";
        }
        else
        {
            VkPhysicalDeviceAccelerationStructureFeaturesKHR as_have = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR };
            VkPhysicalDeviceRayQueryFeaturesKHR rq_have = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR };
            as_have.pNext = &rq_have;
            VkPhysicalDeviceFeatures2 query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            query.pNext = &as_have;
            vkGetPhysicalDeviceFeatures2(mPhysicalDevice, &query);
            if (as_have.accelerationStructure && rq_have.rayQuery)
            {
                extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
                extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
                extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
                as_want.accelerationStructure = VK_TRUE;
                rq_want.rayQuery = VK_TRUE;
                *chain = &as_want;
                as_want.pNext = &rq_want;
                chain = &rq_want.pNext;
                mRayTracing = true;
                mRayTracingStatus = "on";
            }
            else
            {
                mRayTracingStatus = "off (the driver does not offer the ray query features)";
            }
        }
        LLRHI::setRayTracingStatus(mRayTracing, mRayTracingStatus);
        LL_INFOS("Vulkan") << "RayTracing: " << mRayTracingStatus << LL_ENDL;
    }

    // VK_KHR_maintenance5 on its own (fix-round15), when DGC did not enable it:
    // a vertex shader that does not write gl_PointSize then draws points of
    // size 1 (GL's default) instead of breaking VUID-...-topology-08773.
    // Without it, point draws are left out (create_pipeline).
    if (!mMaintenance5)
    {
        const bool has_maintenance5 = std::any_of(available.begin(), available.end(), [](const VkExtensionProperties& ext)
                                                  { return !strcmp(ext.extensionName, VK_KHR_MAINTENANCE_5_EXTENSION_NAME); });
        if (has_maintenance5)
        {
            VkPhysicalDeviceMaintenance5FeaturesKHR maintenance5_have = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR };
            VkPhysicalDeviceFeatures2 query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            query.pNext = &maintenance5_have;
            vkGetPhysicalDeviceFeatures2(mPhysicalDevice, &query);
            if (maintenance5_have.maintenance5)
            {
                extensions.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
                maintenance5_want.maintenance5 = VK_TRUE;
                *chain = &maintenance5_want;
                chain = &maintenance5_want.pNext;
                mMaintenance5 = true;
            }
        }
        LL_INFOS("Vulkan") << "maintenance5: " << (mMaintenance5 ? "on" : "off (point draws are left out)") << LL_ENDL;
    }

    // Frame pacing (research/94): VK_KHR_present_id numbers the presents and
    // VK_KHR_present_wait waits until one is on screen. Both or neither.
    VkPhysicalDevicePresentIdFeaturesKHR present_id_want = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR };
    VkPhysicalDevicePresentWaitFeaturesKHR present_wait_want = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR };
    {
        auto has_extension = [&available](const char* name)
        {
            return std::any_of(available.begin(), available.end(),
                               [name](const VkExtensionProperties& ext) { return !strcmp(ext.extensionName, name); });
        };
        bool usable = false;
        if (has_extension(VK_KHR_PRESENT_ID_EXTENSION_NAME) && has_extension(VK_KHR_PRESENT_WAIT_EXTENSION_NAME))
        {
            VkPhysicalDevicePresentIdFeaturesKHR id_have = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR };
            VkPhysicalDevicePresentWaitFeaturesKHR wait_have = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR };
            id_have.pNext = &wait_have;
            VkPhysicalDeviceFeatures2 query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            query.pNext = &id_have;
            vkGetPhysicalDeviceFeatures2(mPhysicalDevice, &query);
            usable = id_have.presentId && wait_have.presentWait;
        }
        if (usable)
        {
            extensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
            extensions.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
            present_id_want.presentId = VK_TRUE;
            present_wait_want.presentWait = VK_TRUE;
            *chain = &present_id_want;
            present_id_want.pNext = &present_wait_want;
            chain = &present_wait_want.pNext;
            mPresentWaitEnabled = true;
        }
    }

    for (const VkExtensionProperties& ext : available)
    {
        if (!strcmp(ext.extensionName, VK_EXT_DEVICE_FAULT_EXTENSION_NAME))
        {
            extensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
            fault.deviceFault = VK_TRUE;
            *chain = &fault;
            chain = &fault.pNext;
            mDeviceFault = true;
        }
        else if (want_address_log && !strcmp(ext.extensionName, VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME))
        {
            extensions.push_back(VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);
            address_report.reportAddressBinding = VK_TRUE;
            *chain = &address_report;
            chain = &address_report.pNext;
            mAddressLog = true;
        }
        else if (want_pipeline_stats && !strcmp(ext.extensionName, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME))
        {
            VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR stats_have = {
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR };
            VkPhysicalDeviceFeatures2 query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
            query.pNext = &stats_have;
            vkGetPhysicalDeviceFeatures2(mPhysicalDevice, &query);
            if (stats_have.pipelineExecutableInfo)
            {
                extensions.push_back(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME);
                pipeline_stats.pipelineExecutableInfo = VK_TRUE;
                *chain = &pipeline_stats;
                chain = &pipeline_stats.pNext;
                mPipelineStats = true;
            }
        }
        else if (want_checkpoints && !strcmp(ext.extensionName, VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME))
        {
            extensions.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
            mCheckpoints = true;
        }
        else if (!strcmp(ext.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME))
        {
            // Queries only: the memory log reads the driver's budget (research/54).
            extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
            mMemoryBudget = true;
        }
    }

    // research/200: a compute-only family's queue as well, when LLRHI_ASYNC_QUEUES asks for it and the GPU has one.
    std::vector<VkDeviceQueueCreateInfo> queues(1, queue);
    LLVKQueues::instance().planQueues(mPhysicalDevice, mGraphicsQueueFamily, queues);

    VkDeviceCreateInfo info = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    info.pNext = &want;
    info.queueCreateInfoCount = (U32)queues.size();
    info.pQueueCreateInfos = queues.data();
    info.enabledExtensionCount = (U32)extensions.size();
    info.ppEnabledExtensionNames = extensions.data();

    VkResult result = vkCreateDevice(mPhysicalDevice, &info, nullptr, &mDevice);
    if (result != VK_SUCCESS && (mRayTracing || mDGCEnabled))
    {
        // Review rt-base low 2: a driver that lists the optional ray tracing or
        // device generated commands but cannot make a device with them. Once
        // more without them (maintenance5 stays: without DGC it is asked for on
        // its own), rather than no Vulkan device at all.
        LL_WARNS("Vulkan") << "vkCreateDevice failed (" << result << ") with " << (mRayTracing ? "ray tracing" : "")
                           << (mRayTracing && mDGCEnabled ? " and " : "") << (mDGCEnabled ? "device generated commands" : "")
                           << ": trying once more without them" << LL_ENDL;
        const auto unlink = [&want](const void* node)
        {
            for (VkBaseOutStructure* at = reinterpret_cast<VkBaseOutStructure*>(&want); at && at->pNext; at = at->pNext)
            {
                if (at->pNext == node)
                {
                    at->pNext = static_cast<const VkBaseOutStructure*>(node)->pNext;
                    return;
                }
            }
        };
        const auto drop_extension = [&extensions](const char* name)
        {
            extensions.erase(std::remove_if(extensions.begin(), extensions.end(), [name](const char* ext) { return !strcmp(ext, name); }),
                             extensions.end());
        };
        if (mRayTracing)
        {
            unlink(&as_want);
            unlink(&rq_want);
            drop_extension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            drop_extension(VK_KHR_RAY_QUERY_EXTENSION_NAME);
            drop_extension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            mRayTracing = false;
            mRayTracingStatus = "off (the device could not be made with it)";
            LLRHI::setRayTracingStatus(mRayTracing, mRayTracingStatus);
            LL_INFOS("Vulkan") << "RayTracing: " << mRayTracingStatus << LL_ENDL;
        }
        if (mDGCEnabled)
        {
            unlink(&dgc_want);
            drop_extension(VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME);
            mDGCEnabled = false;
            LLRHI::sGPUDrivenDGC = false;
            LL_INFOS("Vulkan") << "DGC: off (the device could not be made with it)" << LL_ENDL;
        }
        info.enabledExtensionCount = (U32)extensions.size();
        info.ppEnabledExtensionNames = extensions.data();
        mDevice = VK_NULL_HANDLE;
        result = vkCreateDevice(mPhysicalDevice, &info, nullptr, &mDevice);
    }
    if (result != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "vkCreateDevice failed: " << result << LL_ENDL;
        addUnsupported(mDeviceName + ": Vulkan device could not be created (error " + std::to_string((S32)result) + ")");
        mDevice = VK_NULL_HANDLE;
        return false;
    }
    vkGetDeviceQueue(mDevice, mGraphicsQueueFamily, 0, &mGraphicsQueue);
    LLVKQueues::instance().init(mDevice);   // research/200 (false: folded, logged)
    if (mDeviceFault)
    {
        mGetDeviceFaultInfo = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(mDevice, "vkGetDeviceFaultInfoEXT");
        mDeviceFault = mGetDeviceFaultInfo != nullptr;
    }
    if (mCheckpoints)
    {
        mCmdSetCheckpoint = (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(mDevice, "vkCmdSetCheckpointNV");
        mGetQueueCheckpointData = (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(mDevice, "vkGetQueueCheckpointDataNV");
        mCheckpoints = mCmdSetCheckpoint && mGetQueueCheckpointData;
        LL_INFOS("Vulkan") << "draw checkpoints " << (mCheckpoints ? "on" : "unavailable") << LL_ENDL;
    }
    if (mPipelineStats)
    {
        mGetPipelineExecutableProperties =
            (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(mDevice, "vkGetPipelineExecutablePropertiesKHR");
        mGetPipelineExecutableStatistics =
            (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(mDevice, "vkGetPipelineExecutableStatisticsKHR");
        mPipelineStats = mGetPipelineExecutableProperties && mGetPipelineExecutableStatistics;
    }
    if (want_pipeline_stats)
    {
        LL_INFOS("Vulkan") << "pipeline statistics (LLRHI_VK_PIPELINE_STATS) " << (mPipelineStats ? "on" : "unavailable") << LL_ENDL;
    }
    if (want_address_log)
    {
        LL_INFOS("Vulkan") << "address binding log " << (mAddressLog ? "on" : "unavailable") << LL_ENDL;
    }
    if (mPresentWaitEnabled)
    {
        mWaitForPresent = (PFN_vkWaitForPresentKHR)vkGetDeviceProcAddr(mDevice, "vkWaitForPresentKHR");
    }
    LL_INFOS("Vulkan") << "present wait (VSync frame pacing) " << (mWaitForPresent ? "available" : "unavailable") << LL_ENDL;
    if (mDGCEnabled)
    {
        mDGC.getMemoryRequirements = (PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)vkGetDeviceProcAddr(mDevice, "vkGetGeneratedCommandsMemoryRequirementsEXT");
        mDGC.cmdExecute = (PFN_vkCmdExecuteGeneratedCommandsEXT)vkGetDeviceProcAddr(mDevice, "vkCmdExecuteGeneratedCommandsEXT");
        mDGC.createLayout = (PFN_vkCreateIndirectCommandsLayoutEXT)vkGetDeviceProcAddr(mDevice, "vkCreateIndirectCommandsLayoutEXT");
        mDGC.destroyLayout = (PFN_vkDestroyIndirectCommandsLayoutEXT)vkGetDeviceProcAddr(mDevice, "vkDestroyIndirectCommandsLayoutEXT");
        mDGCReady = mDGC.getMemoryRequirements && mDGC.cmdExecute && mDGC.createLayout && mDGC.destroyLayout;
        if (!mDGCReady)
        {
            LL_WARNS("Vulkan") << "device generated commands: entry points missing, not used" << LL_ENDL;
        }
    }
    return true;
}

void LLVKDevice::setFrame(U32 frame)
{
    mFrame.store(frame, std::memory_order_relaxed);
    if (mAllocator)     // none before init() or after shutdown()
    {
        // VMA fetches the driver's budget again on a new frame index: without
        // this a quiet scene (few allocations) left vramBudget() on an old one.
        vmaSetCurrentFrameIndex(mAllocator, frame);
    }
}

std::string LLVKDevice::memoryReport()
{
    if (!mAllocator)
    {
        return std::string();
    }
    // Fetches the driver's numbers again (VMA does on a new frame index).
    vmaSetCurrentFrameIndex(mAllocator, mFrame.load(std::memory_order_relaxed));
    const VkPhysicalDeviceMemoryProperties* properties = nullptr;
    vmaGetMemoryProperties(mAllocator, &properties);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(mAllocator, budgets);
    U64 usage = 0, budget = 0, block_bytes = 0, allocation_bytes = 0;
    U32 allocations = 0, memory_objects = 0;
    for (U32 heap = 0; properties && heap < properties->memoryHeapCount; ++heap)
    {
        if (properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        {
            usage += budgets[heap].usage;
            budget += budgets[heap].budget;
            block_bytes += budgets[heap].statistics.blockBytes;
            allocation_bytes += budgets[heap].statistics.allocationBytes;
            allocations += budgets[heap].statistics.allocationCount;
            memory_objects += budgets[heap].statistics.blockCount;     // blocks and dedicated allocations
        }
    }
    std::ostringstream out;
    out << "VRAM " << (usage >> 20) << " of " << (budget >> 20) << " MB (" << (mMemoryBudget ? "driver budget" : "estimate")
        << "), Vulkan allocations " << (allocation_bytes >> 20) << " MB (" << allocations << ") in " << (block_bytes >> 20)
        << " MB of blocks (" << memory_objects << " device memory objects)";
    // research/285 X3: how often memory is had from the driver and given back,
    // and where the images over 8 MB went.
    out << ", since the start " << mDeviceAllocations.load(std::memory_order_relaxed) << " vkAllocateMemory and "
        << mDeviceFrees.load(std::memory_order_relaxed) << " vkFreeMemory, images over 8 MB ";
    if (mBigBlocks && mSizedPools)
    {
        // research/285 (compact-hysteresis): textures and other images, not render targets (memory of their own,
        // not counted) nor what the driver wants alone.
        out << mBigOwn.load(std::memory_order_relaxed) << " in memory of their own, " << mBigHeld.load(std::memory_order_relaxed)
            << " in default blocks for the limits, " << mBigInterim.load(std::memory_order_relaxed) << " in default blocks until BC7, "
            << mBigMoved.load(std::memory_order_relaxed) << " moved out of default blocks into memory of their own later";
    }
    else
    {
        out << (mBigBlocks ? "not counted (LLRHI_VK_POOLS=0)" : "not counted (LLRHI_VK_BIG_BLOCKS=0)");
    }
    // igpu-safe: what a GPU without VRAM of its own takes from the PC (every heap) against what it may.
    U64 shared_images = 0, shared_usage = 0, shared_budget = 0;
    if (sharedMemory() && vramBudget(shared_images, shared_usage, shared_budget))
    {
        out << ", GPU memory from the PC " << (shared_usage >> 20) << " of " << (shared_budget >> 20) << " MB (every heap)";
    }
    if (mSizedPools)
    {
        out << ", " << poolReport();
    }
    out << ", " << blockReport();      // research/285 X0
    if (mAddressLog)
    {
        // What the address log costs (research/125 #13): one callback per bind or unbind.
        std::lock_guard<std::mutex> lock(mAddressMutex);
        out << ", address log " << (mAddressEventCount - mAddressEventsReported) << " binds and unbinds since the last line ("
            << mLiveBindings.size() << " ranges bound)";
        mAddressEventsReported = mAddressEventCount;
    }
    return out.str();
}

void LLVKDevice::dumpAllocations(const std::string& path)
{
    if (!mAllocator)
    {
        return;
    }
    char* json = nullptr;
    vmaBuildStatsString(mAllocator, &json, VK_TRUE);
    if (json)
    {
        llofstream out(path.c_str());
        if (out.is_open())
        {
            out << json;
        }
        vmaFreeStatsString(mAllocator, json);
    }
}

void LLVKDevice::reportDeviceLost()
{
    noteDeviceLost();
    if (mLostReported.exchange(true))
    {
        return;
    }
    std::ostringstream out;
    out << "GPU fault report:";
    if (mDeviceFault)
    {
        VkDeviceFaultCountsEXT counts = { VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
        mGetDeviceFaultInfo(mDevice, &counts, nullptr);
        std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
        std::vector<VkDeviceFaultVendorInfoEXT> vendor(counts.vendorInfoCount);
        counts.vendorBinarySize = 0;
        VkDeviceFaultInfoEXT info = { VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT };
        info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
        info.pVendorInfos = vendor.empty() ? nullptr : vendor.data();
        VkResult result = mGetDeviceFaultInfo(mDevice, &counts, &info);
        out << "\n  device fault (" << (S32)result << "): " << info.description;
        for (const VkDeviceFaultAddressInfoEXT& a : addresses)
        {
            out << "\n    address type " << (S32)a.addressType << " at 0x" << std::hex << a.reportedAddress
                << " precision 0x" << a.addressPrecision << std::dec;
        }
        for (const VkDeviceFaultVendorInfoEXT& v : vendor)
        {
            out << "\n    vendor: " << v.description << " code 0x" << std::hex << v.vendorFaultCode << " data 0x" << v.vendorFaultData << std::dec;
        }
    }
    else
    {
        out << "\n  (VK_EXT_device_fault unavailable)";
    }
    if (mAddressLog && mDeviceFault)
    {
        // Again (the first call above filled the counts): the addresses to look up.
        VkDeviceFaultCountsEXT counts = { VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
        mGetDeviceFaultInfo(mDevice, &counts, nullptr);
        std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
        counts.vendorInfoCount = 0;
        counts.vendorBinarySize = 0;
        VkDeviceFaultInfoEXT info = { VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT };
        info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
        mGetDeviceFaultInfo(mDevice, &counts, &info);
        std::lock_guard<std::mutex> lock(mAddressMutex);
        out << "\n  address log (" << mAddressEvents.size() << " events, current frame " << mFrame.load(std::memory_order_relaxed) << "):";
        for (const VkDeviceFaultAddressInfoEXT& a : addresses)
        {
            const U64 precision = a.addressPrecision ? a.addressPrecision : 1;
            const U64 low = a.reportedAddress & ~(precision - 1);
            const U64 high = low + precision;
            out << "\n    0x" << std::hex << a.reportedAddress << std::dec << " (type " << (S32)a.addressType << "):";
            describeLiveBindings(out, low, high);   // research/109: what is bound there now, or the nearest
            U32 shown = 0;
            for (auto it = mAddressEvents.rbegin(); it != mAddressEvents.rend() && shown < 8; ++it)
            {
                if (it->base < high && it->base + it->size > low)
                {
                    out << "\n      frame " << it->frame << (it->bound ? " bound " : " UNBOUND ") << "object type " << (S32)it->type
                        << " handle 0x" << std::hex << it->handle << " at 0x" << it->base << std::dec << " size " << it->size;
                    std::string described = mDescribeHook ? mDescribeHook(it->handle, it->type) : std::string();
                    if (described.empty())
                    {
                        described = describeFromHistory(it->handle);    // research/109: also what is gone
                    }
                    if (!described.empty())
                    {
                        out << " " << described;
                    }
                    ++shown;
                }
            }
            if (!shown)
            {
                out << "\n      no bind or unbind there in the last " << mAddressEvents.size() << " events";
            }
        }
    }
    else if (!mAddressLog)
    {
        out << "\n  (no address log: LLRHI_VK_ADDRESS_LOG=0, or the driver lacks VK_EXT_device_address_binding_report)";
    }
    out << reportHistory();     // research/109
    std::vector<U64> markers;
    std::vector<U32> stages;    // bc7-fault: the stage of each marker
    if (!mCheckpoints)
    {
        out << "\n  (draws at the fault: start with LLRHI_VK_CHECKPOINTS=1)";
    }
    if (mCheckpoints)
    {
        U32 count = 0;
        mGetQueueCheckpointData(mGraphicsQueue, &count, nullptr);
        std::vector<VkCheckpointDataNV> data(count, { VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV });
        mGetQueueCheckpointData(mGraphicsQueue, &count, data.data());
        for (const VkCheckpointDataNV& c : data)
        {
            const U64 marker = (U64)(uintptr_t)c.pCheckpointMarker;
            markers.push_back(marker);
            stages.push_back((U32)c.stage);
            out << "\n  checkpoint at stage 0x" << std::hex << (U32)c.stage << std::dec << ": program " << (marker >> 32)
                << " draw " << (marker & 0xffffffffu);
        }
    }
    LL_WARNS("Vulkan") << out.str() << LL_ENDL;
    if (mDeviceLostHook)
    {
        mDeviceLostHook(markers, stages);
    }
}

std::string LLVKDevice::pipelineStatistics(VkPipeline pipeline) const
{
    if (!mPipelineStats || !pipeline)
    {
        return std::string();
    }
    VkPipelineInfoKHR info = { VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR };
    info.pipeline = pipeline;
    U32 count = 0;
    if (mGetPipelineExecutableProperties(mDevice, &info, &count, nullptr) != VK_SUCCESS || !count)
    {
        return "no figures from the driver";
    }
    std::vector<VkPipelineExecutablePropertiesKHR> executables(count, { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR });
    mGetPipelineExecutableProperties(mDevice, &info, &count, executables.data());
    std::ostringstream out;
    for (U32 i = 0; i < count; ++i)
    {
        out << (i ? "; " : "") << executables[i].name << " (subgroup " << executables[i].subgroupSize << "):";
        VkPipelineExecutableInfoKHR which = { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR };
        which.pipeline = pipeline;
        which.executableIndex = i;
        U32 stats = 0;
        mGetPipelineExecutableStatistics(mDevice, &which, &stats, nullptr);
        std::vector<VkPipelineExecutableStatisticKHR> values(stats, { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR });
        mGetPipelineExecutableStatistics(mDevice, &which, &stats, values.data());
        for (U32 v = 0; v < stats; ++v)
        {
            const VkPipelineExecutableStatisticKHR& value = values[v];
            out << (v ? "," : "") << " " << value.name << " ";
            switch (value.format)
            {
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR:
                out << (value.value.b32 ? "yes" : "no");
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR:
                out << value.value.i64;
                break;
            case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR:
                out << value.value.u64;
                break;
            default:
                out << value.value.f64;
                break;
            }
        }
    }
    return out.str();
}

void LLVKDevice::addAddressEvent(const AddressEvent& event)
{
    std::lock_guard<std::mutex> lock(mAddressMutex);
    ++mAddressEventCount;
    if (mAddressEvents.size() >= 131072)
    {
        mAddressEvents.erase(mAddressEvents.begin(), mAddressEvents.begin() + 65536);    // keep the recent half
    }
    mAddressEvents.push_back(event);
    mAddressEvents.back().frame = mFrame.load(std::memory_order_relaxed);
    // research/109: what is bound stays known however old (the ring above
    // forgets the binds made early on, e.g. the pools' and tables' buffers).
    if (event.bound)
    {
        mLiveBindings[event.base] = mAddressEvents.back();
    }
    else
    {
        auto it = mLiveBindings.find(event.base);
        if (it != mLiveBindings.end() && (it->second.handle == event.handle || !event.handle))
        {
            mLiveBindings.erase(it);
        }
    }
}

// research/109: the live bindings holding [low, high), else the nearest ones
// below and above (a write past the end of a buffer lands just above it).
// Under mAddressMutex.
void LLVKDevice::describeLiveBindings(std::ostringstream& out, U64 low, U64 high)
{
    auto type_name = [](VkObjectType type)
    {
        return type == VK_OBJECT_TYPE_BUFFER ? "buffer" : type == VK_OBJECT_TYPE_IMAGE ? "image" :
               type == VK_OBJECT_TYPE_DEVICE_MEMORY ? "memory" : "object";
    };
    auto line = [&](const char* what, const AddressEvent& e)
    {
        out << "\n      " << what << ": " << type_name(e.type) << " (type " << (S32)e.type << ") handle 0x" << std::hex << e.handle
            << " at 0x" << e.base << "-0x" << e.base + e.size << std::dec << " (" << (e.size >> 10) << " KB), bound in frame " << e.frame;
        std::string described = mDescribeHook ? mDescribeHook(e.handle, e.type) : std::string();
        if (described.empty())
        {
            described = describeFromHistory(e.handle);
        }
        if (!described.empty())
        {
            out << " " << described;
        }
    };
    out << " " << mLiveBindings.size() << " ranges bound now";
    auto above = mLiveBindings.lower_bound(high);
    U32 inside = 0;
    // The ranges starting below `high` that reach past `low` (a memory object
    // holds the buffers and images bound in it); none is larger than 4 GB.
    for (auto it = above; it != mLiveBindings.begin(); )
    {
        --it;
        if (it->second.base + (1ull << 32) < low || inside >= 8)
        {
            break;
        }
        if (it->second.base + it->second.size > low)
        {
            line("bound there now", it->second);
            ++inside;
        }
    }
    if (!inside)
    {
        if (above != mLiveBindings.begin())
        {
            const AddressEvent& below = std::prev(above)->second;
            line("nearest below", below);
            out << ", its end " << ((low - (below.base + below.size)) >> 10) << " KB before the fault";
        }
        if (above != mLiveBindings.end())
        {
            line("nearest above", above->second);
        }
    }
}

// research/109: the history the fault report prints.
void LLVKDevice::noteResource(ResourceStep step, U64 handle, VmaAllocation allocation, const char* what)
{
    mStepCounts[step].fetch_add(1, std::memory_order_relaxed);
    ResourceEvent event;
    event.handle = handle;
    event.step = step;
    if (allocation && mAllocator)
    {
        VmaAllocationInfo info = {};
        vmaGetAllocationInfo(mAllocator, allocation, &info);
        event.memory = (U64)info.deviceMemory;
        event.offset = info.offset;
        event.bytes = info.size;
    }
    if (what)
    {
        strncpy(event.what, what, sizeof(event.what) - 1);
    }
    std::lock_guard<std::mutex> lock(mHistoryMutex);
    event.frame = mFrame.load(std::memory_order_relaxed);
    if (mResources.empty())
    {
        mResources.resize(RESOURCE_RING);
        mLargeResources.resize(LARGE_RING);
    }
    mResources[mResourceCount++ % RESOURCE_RING] = event;
    if (event.bytes >= LARGE_BYTES)
    {
        mLargeResources[mLargeCount++ % LARGE_RING] = event;
    }
}

void LLVKDevice::noteEvent(const char* text)
{
    std::lock_guard<std::mutex> lock(mHistoryMutex);
    if (mEvents.empty())
    {
        mEvents.resize(EVENT_RING);
    }
    FrameEvent& event = mEvents[mEventCount++ % EVENT_RING];
    event.frame = mFrame.load(std::memory_order_relaxed);
    strncpy(event.text, text ? text : "", sizeof(event.text) - 1);
    event.text[sizeof(event.text) - 1] = 0;
}

std::string LLVKDevice::describeFromHistory(U64 handle)
{
    std::lock_guard<std::mutex> lock(mHistoryMutex);
    std::ostringstream out;
    // Newest first, in both rings (a large one may have left the other).
    for (const std::vector<ResourceEvent>* ring : { &mResources, &mLargeResources })
    {
        const U64 count = ring == &mResources ? mResourceCount : mLargeCount;
        const U64 size = ring->size();
        for (U64 i = 0; size && i < llmin(count, size); ++i)
        {
            const ResourceEvent& e = (*ring)[(count - 1 - i) % size];
            if (e.handle == handle)
            {
                static const char* const STEPS[] = { "made", "released", "freed" };
                out << "(" << e.what << ", " << STEPS[e.step] << " in frame " << e.frame << ")";
                return out.str();
            }
        }
    }
    return std::string();
}

std::string LLVKDevice::reportHistory()
{
    static const char* const STEPS[] = { "made    ", "released", "freed   " };
    std::lock_guard<std::mutex> lock(mHistoryMutex);
    std::ostringstream out;
    auto print = [&](const std::vector<ResourceEvent>& ring, U64 count, U64 shown)
    {
        const U64 size = ring.size();
        const U64 first = count > shown ? count - shown : 0;
        for (U64 i = first; size && i < count; ++i)
        {
            if (count - i > size)
            {
                continue;   // overwritten
            }
            const ResourceEvent& e = ring[i % size];
            out << "\n    frame " << e.frame << " " << STEPS[e.step] << " 0x" << std::hex << e.handle << " memory 0x" << e.memory
                << " +0x" << e.offset << std::dec << " " << (e.bytes >> 10) << " KB " << e.what;
        }
    };
    out << "\n  frame events (current frame " << mFrame.load(std::memory_order_relaxed) << "):";
    const U64 events = llmin(mEventCount, (U64)mEvents.size());
    for (U64 i = mEventCount - events; i < mEventCount; ++i)
    {
        const FrameEvent& e = mEvents[i % mEvents.size()];
        out << "\n    frame " << e.frame << ": " << e.text;
    }
    out << "\n  large resources (8 MB and up), the last 64 of " << mLargeCount << ":";
    print(mLargeResources, mLargeCount, 64);
    out << "\n  every resource, the last 64 of " << mResourceCount << ":";
    print(mResources, mResourceCount, 64);
    // The last ones actually freed (a release only queues; the free follows the GPU).
    out << "\n  freed (" << mStepCounts[RESOURCE_FREED].load(std::memory_order_relaxed) << " in all), the last 32 the ring holds:";
    const U64 held = llmin(mResourceCount, (U64)mResources.size());
    U32 freed_shown = 0;
    std::vector<const ResourceEvent*> freed;
    for (U64 i = 0; i < held && freed_shown < 32; ++i)
    {
        const ResourceEvent& e = mResources[(mResourceCount - 1 - i) % mResources.size()];
        if (e.step == RESOURCE_FREED)
        {
            freed.push_back(&e);
            ++freed_shown;
        }
    }
    for (auto it = freed.rbegin(); it != freed.rend(); ++it)
    {
        const ResourceEvent& e = **it;
        out << "\n    frame " << e.frame << " freed 0x" << std::hex << e.handle << " memory 0x" << e.memory << " +0x" << e.offset << std::dec
            << " " << (e.bytes >> 10) << " KB " << e.what;
    }
    return out.str();
}

bool LLVKDevice::createAllocator()
{
    VmaAllocatorCreateInfo info = {};
    info.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (mMemoryBudget)
    {
        info.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
    }
    info.vulkanApiVersion = VK_API_VERSION_1_3;
    info.instance = mInstance;
    info.physicalDevice = mPhysicalDevice;
    info.device = mDevice;
    // research/285 X3: VMA's default blocks were 256 MB. After a peak (a
    // teleport, a crowded event) a few allocations left in each kept whole
    // blocks out of the driver's budget (1.9 GB free inside them at an
    // event). Smaller blocks empty sooner; allocations over half a block get
    // memory of their own (VMA's rule).
    mBigBlocks = LLRHI::envDefaultOn("LLRHI_VK_BIG_BLOCKS");
    U64 default_block_mb = 64;
    if (mBigBlocks)
    {
        LLRHI::envNumber("LLRHI_VK_DEFAULT_BLOCK_MB", 16, 256, default_block_mb);
        info.preferredLargeHeapBlockSize = default_block_mb << 20;
        const U32 max_objects = mLimits.maxMemoryAllocationCount;
        mOwnMemoryCap = llmin(max_objects / 2, 2048u);
    }
    // vkAllocateMemory / vkFreeMemory counted (memoryReport).
    VmaDeviceMemoryCallbacks memory_callbacks = {};
    memory_callbacks.pfnAllocate = &LLVKDevice::onDeviceAllocate;
    memory_callbacks.pfnFree = &LLVKDevice::onDeviceFree;
    memory_callbacks.pUserData = this;
    info.pDeviceMemoryCallbacks = &memory_callbacks;

    VkResult result = vmaCreateAllocator(&info, &mAllocator);
    if (result != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "vmaCreateAllocator failed: " << result << LL_ENDL;
        addUnsupported("Vulkan memory allocator could not be created (error " + std::to_string((S32)result) + ")");
        mAllocator = nullptr;
        return false;
    }
    mSizedPools = LLRHI::envDefaultOn("LLRHI_VK_POOLS");
    LL_INFOS("Vulkan") << "VMA pools by size class " << (mSizedPools ? "on" : "off (LLRHI_VK_POOLS=0)") << " (research/77)" << LL_ENDL;
    if (mBigBlocks)
    {
        LL_INFOS("Vulkan") << "VMA default blocks " << default_block_mb << " MB; images over 8 MB in memory of their own while under "
                           << mOwnMemoryCap << " device memory objects (the device allows " << mLimits.maxMemoryAllocationCount
                           << ") (research/285 X3)" << LL_ENDL;
    }
    else
    {
        LL_INFOS("Vulkan") << "VMA default blocks 256 MB, images over 8 MB in them (LLRHI_VK_BIG_BLOCKS=0)" << LL_ENDL;
    }
    return true;
}

void VKAPI_PTR LLVKDevice::onDeviceAllocate(VmaAllocator, uint32_t, VkDeviceMemory, VkDeviceSize, void* self)
{
    static_cast<LLVKDevice*>(self)->mDeviceAllocations.fetch_add(1, std::memory_order_relaxed);
}

void VKAPI_PTR LLVKDevice::onDeviceFree(VmaAllocator, uint32_t, VkDeviceMemory, VkDeviceSize, void* self)
{
    static_cast<LLVKDevice*>(self)->mDeviceFrees.fetch_add(1, std::memory_order_relaxed);
}

bool LLVKDevice::bigImageOwnMemory(bool frame_limit)
{
    if (!mBigBlocks || !mAllocator)
    {
        return false;
    }
    // A driver call each (0.1 ms to a few ms, on the thread making the image,
    // under the tracker's lock): a few a frame. A crowd of large textures
    // arriving at once spreads over frames; the rest go to default blocks.
    // research/285 (compact-hysteresis): not for render targets (frame_limit
    // false: a few, made again when the window is resized, all at once).
    constexpr U32 OWN_MEMORY_PER_FRAME = 4;
    const U32 frame = mFrame.load(std::memory_order_relaxed);
    if (mOwnMemoryFrame.exchange(frame, std::memory_order_relaxed) != frame)
    {
        mOwnMemoryInFrame.store(0, std::memory_order_relaxed);
    }
    if (frame_limit && mOwnMemoryInFrame.load(std::memory_order_relaxed) >= OWN_MEMORY_PER_FRAME)
    {
        return false;
    }
    // The device memory objects now, every heap (VMA's counts: blocks and
    // allocations of their own).
    const VkPhysicalDeviceMemoryProperties* properties = nullptr;
    vmaGetMemoryProperties(mAllocator, &properties);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(mAllocator, budgets);
    U64 objects = 0;
    for (U32 heap = 0; properties && heap < properties->memoryHeapCount; ++heap)
    {
        objects += budgets[heap].statistics.blockCount;
    }
    if (objects < mOwnMemoryCap)
    {
        if (frame_limit)
        {
            mOwnMemoryInFrame.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }
    if (!mOwnMemoryCapLogged.exchange(true, std::memory_order_relaxed))
    {
        LL_WARNS("Vulkan") << objects << " device memory objects (cap " << mOwnMemoryCap
                           << "): images over 8 MB go into VMA's default blocks from now on while that many (research/285 X3)" << LL_ENDL;
    }
    return false;
}

namespace
{
    // pool_group's groups: size classes 2^12 (4 KB and under, both kinds) to
    // 2^23 (8 MB, the texture pools' largest).
    constexpr U32 FIRST_GROUP = 12;
    constexpr U32 TEXTURE_POOL_LARGEST_CLASS = 23;
    constexpr U32 GROUP_COUNT = TEXTURE_POOL_LARGEST_CLASS - FIRST_GROUP + 1;
    // The GL buffer pools' groups past the first: up to 64 KB and up to 4 MB.
    constexpr U32 GL_BUFFER_MIDDLE_GROUP = 16;
    constexpr U32 GL_BUFFER_LARGEST_CLASS = 22;

    // Allocation sizes by power of two: 2^class is the first at least `size`.
    U32 size_class(U64 size)
    {
        U32 c = 0;
        while (c < 63 && ((U64)1 << c) < size)
        {
            ++c;
        }
        return c;
    }

    // The pool of a kind that takes `size` bytes (its group) and the pool's
    // block size; false: too large for the pools of that kind.
    bool pool_group(LLVKDevice::PoolKind kind, U64 size, U32& group, U64& block)
    {
        const U32 c = size_class(size);
        if (kind == LLVKDevice::POOL_TEXTURE)
        {
            // A pool per power of two up to 8 MB (a 1024x1024 RGBA texture with
            // its mips is 5.3 MB), 4 KB and under together. Textures come in a
            // few sizes (powers of two, full mip chains), so the hole a freed
            // texture leaves takes the next one of its class. Blocks of 16 MB
            // (small classes) to 128 MB (16 or more of the largest class).
            if (c > TEXTURE_POOL_LARGEST_CLASS)
            {
                return false;
            }
            group = llmax(c, FIRST_GROUP);
            block = llclamp((U64)1 << (group + 4), (U64)16 << 20, (U64)128 << 20);
            // research/79: the large classes in blocks of about 8 textures
            // (48 MB for 1024x1024 RGBA, was 128 MB: 22), so a block empties
            // (and VMA gives it back while VRAM is over the driver's budget)
            // once its few textures went down a class or away. With the
            // discard bias up, textures move to smaller classes and the large
            // blocks were left part full (850 MB free in Le'eaf).
            // LLRHI_VK_TEX_SMALL_BLOCKS=0: the sizes before.
            static const bool small_blocks = LLRHI::envDefaultOn("LLRHI_VK_TEX_SMALL_BLOCKS");
            if (small_blocks)
            {
                block = llclamp(((U64)1 << group) * 6, (U64)16 << 20, (U64)48 << 20);
            }
            return true;
        }
        // GL buffers: up to 4 KB (most of them), up to 64 KB, up to 4 MB; in
        // blocks of 16, 32 and 64 MB.
        if (c > GL_BUFFER_LARGEST_CLASS)
        {
            return false;
        }
        group = c <= FIRST_GROUP ? FIRST_GROUP : c <= GL_BUFFER_MIDDLE_GROUP ? GL_BUFFER_MIDDLE_GROUP : GL_BUFFER_LARGEST_CLASS;
        block = (U64)(group == FIRST_GROUP ? 16 : group == GL_BUFFER_MIDDLE_GROUP ? 32 : 64) << 20;
        return true;
    }

    const char* const POOL_KIND_NAMES[LLVKDevice::POOL_KIND_COUNT] = { "texture", "GL buffer" };

    // mPools' key: kind, memory type, group.
    U64 pool_key(U32 kind, U32 memory_type, U32 group)
    {
        return ((U64)kind << 48) | ((U64)memory_type << 8) | group;
    }

    // A pool group's largest size: "4 KB", "8 MB".
    void print_group(std::ostream& out, U32 group)
    {
        out << ((U64)1 << (group >= 20 ? group - 20 : group - 10)) << (group >= 20 ? " MB" : " KB");
    }

    // igpu-safe: the budget of a GPU sharing the PC's memory leaves the texture images at least this
    // much over all the rest it takes (vramBudget).
    constexpr U64 SHARED_TEXTURE_ROOM = 1024ull << 20;

    // igpu-safe: make(create) with the memory types in `spent` (spentDeviceLocalTypes: device-local
    // heaps past their budget on a GPU that shares the PC's memory) left out first, then as asked
    // when that fails: never fails where it did not before. spent 0 (a discrete GPU, or room):
    // make(create) alone.
    template <typename Make>
    VkResult create_with_spill(U32 spent, const VmaAllocationCreateInfo& create, const Make& make)
    {
        if (spent)
        {
            VmaAllocationCreateInfo elsewhere = create;
            elsewhere.memoryTypeBits = (create.memoryTypeBits ? create.memoryTypeBits : ~0u) & ~spent;
            if (elsewhere.memoryTypeBits && make(elsewhere) == VK_SUCCESS)
            {
                return VK_SUCCESS;
            }
        }
        return make(create);
    }
}

VmaPool LLVKDevice::sizedPool(PoolKind kind, U32 memory_type, U64 size)
{
    U32 group = 0;
    U64 block = 0;
    if (!mAllocator || !mSizedPools || !pool_group(kind, size, group, block))
    {
        return VK_NULL_HANDLE;
    }
    const U64 key = pool_key(kind, memory_type, group);
    std::lock_guard<std::mutex> lock(mPoolMutex);
    auto it = mPools.find(key);
    if (it != mPools.end())
    {
        return it->second.pool;
    }
    VmaPoolCreateInfo info = {};
    info.memoryTypeIndex = memory_type;
    // Each pool holds one kind of resource: only buffers, or only optimal images.
    info.flags = VMA_POOL_CREATE_IGNORE_BUFFER_IMAGE_GRANULARITY_BIT;
    info.blockSize = block;
    VmaPool pool = VK_NULL_HANDLE;
    const VkResult result = vmaCreatePool(mAllocator, &info, &pool);
    std::ostringstream name;
    name << POOL_KIND_NAMES[kind] << "s up to ";
    print_group(name, group);
    name << ", memory type " << memory_type;
    if (result != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "VMA pool for " << name.str() << " could not be made (" << result << "); the default pools take them" << LL_ENDL;
        pool = VK_NULL_HANDLE;
    }
    else
    {
        vmaSetPoolName(mAllocator, pool, name.str().c_str());   // shows in LLRHI_VMA_DUMP's files
    }
    mPools[key] = { kind, pool };
    return pool;
}

U32 LLVKDevice::imageMemoryType(const VkImageCreateInfo& info, U32 type_bits)
{
    {
        std::lock_guard<std::mutex> lock(mPoolMutex);
        auto it = mImageMemoryTypes.find(type_bits);
        if (it != mImageMemoryTypes.end())
        {
            return it->second;
        }
    }
    // What vmaCreateImage would try first for an image the GPU reads (the
    // same for every image with these memory type bits: no host access).
    VmaAllocationCreateInfo gpu = {};
    gpu.usage = VMA_MEMORY_USAGE_AUTO;
    U32 type = U32_MAX;
    if (vmaFindMemoryTypeIndexForImageInfo(mAllocator, &info, &gpu, &type) != VK_SUCCESS || type >= 32 || !(type_bits & (1u << type)))
    {
        type = U32_MAX;
    }
    std::lock_guard<std::mutex> lock(mPoolMutex);
    mImageMemoryTypes[type_bits] = type;
    return type;
}

VkResult LLVKDevice::createImage(const VkImageCreateInfo& info, VkImage& image, VmaAllocation& memory, ImageMemory where)
{
    const VkResult result = createImageInPool(info, image, memory, where);
    if (result == VK_SUCCESS)
    {
        // research/109: for the fault report.
        char what[56];
        snprintf(what, sizeof(what), "image %ux%ux%u format %d, %u levels%s", info.extent.width, info.extent.height, info.arrayLayers,
                 (S32)info.format, info.mipLevels,
                 (info.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) ? ", attachable" : "");
        noteResource(RESOURCE_MADE, (U64)image, memory, what);
    }
    return result;
}

VkResult LLVKDevice::createImageInPool(const VkImageCreateInfo& info, VkImage& image, VmaAllocation& memory, ImageMemory where)
{
    const bool interim = where == ImageMemory::Interim;
    const bool existing_blocks = where == ImageMemory::ExistingPoolBlocks;
    image = VK_NULL_HANDLE;
    memory = nullptr;
    VkDeviceSize size = 0;      // igpu-safe: the memory it takes, when asked below (0: not known)
    bool big = false;           // research/285 X3: too large for the texture pools
    if (mSizedPools && info.tiling == VK_IMAGE_TILING_OPTIMAL)
    {
        VkImage made = VK_NULL_HANDLE;
        if (vkCreateImage(mDevice, &info, nullptr, &made) == VK_SUCCESS)
        {
            VkMemoryDedicatedRequirements dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
            VkMemoryRequirements2 requirements = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &dedicated };
            VkImageMemoryRequirementsInfo2 query = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 };
            query.image = made;
            vkGetImageMemoryRequirements2(mDevice, &query, &requirements);
            size = requirements.memoryRequirements.size;
            VmaPool pool = VK_NULL_HANDLE;
            bool pool_spent = false;    // igpu-safe: the pool's heap has no room in its budget
            // An image the driver wants in memory of its own (NVIDIA: attachable
            // ones of 1024x1024 and up) keeps getting it, as before.
            if (!dedicated.prefersDedicatedAllocation && !dedicated.requiresDedicatedAllocation)
            {
                const U32 type = imageMemoryType(info, requirements.memoryRequirements.memoryTypeBits);
                big = size_class(size) > TEXTURE_POOL_LARGEST_CLASS;
                if (type != U32_MAX)
                {
                    pool = sizedPool(POOL_TEXTURE, type, size);
                    pool_spent = (spentDeviceLocalTypes(size) & (1u << type)) != 0;
                }
            }
            if (pool)
            {
                VmaAllocationCreateInfo pooled = {};
                pooled.pool = pool;
                if (pool_spent || existing_blocks)
                {
                    // igpu-safe: a hole in the pool's blocks, but no new block past the budget
                    // (pools ignore it: explicit block size); else the default pools below.
                    // research/285 X2 (ExistingPoolBlocks): a hole in the pool's blocks only.
                    pooled.flags |= VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;
                }
                if (vmaAllocateMemoryForImage(mAllocator, made, &pooled, &memory, nullptr) == VK_SUCCESS)
                {
                    if (vmaBindImageMemory(mAllocator, memory, made) == VK_SUCCESS)
                    {
                        image = made;
                        countImage(memory);
                        return VK_SUCCESS;
                    }
                    vmaFreeMemory(mAllocator, memory);
                    memory = nullptr;
                }
            }
            // Not for a pool, or the pool's memory could not be had: as before.
            vkDestroyImage(mDevice, made, nullptr);
        }
    }
    if (existing_blocks)
    {
        // research/285 X2: not in a block a texture pool had (none for it,
        // no room, or the driver wants it in memory of its own).
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    VmaAllocationCreateInfo gpu = {};
    gpu.usage = VMA_MEMORY_USAGE_AUTO;
    const auto make = [&](const VmaAllocationCreateInfo& create) { return vmaCreateImage(mAllocator, &info, &create, &image, &memory, nullptr); };
    // igpu-safe: heaps without room for it (its size asked above; else heaps already past their budget).
    const U32 spent = spentDeviceLocalTypes(size);
    // research/285 (compact-hysteresis): a render target (AMD and Intel do not
    // ask for memory of its own) gets it too, but takes none of the frame's few
    // for large textures and is not counted with them.
    const bool attachment = (info.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0;
    const bool own_only = where == ImageMemory::OwnOnly;    // research/285 compact-hysteresis: a move, no default block
    if (big && !interim && bigImageOwnMemory(!attachment))
    {
        // research/285 X3: memory of its own, back to the driver when freed;
        // failing that, a default block as before.
        VmaAllocationCreateInfo own = gpu;
        own.flags |= VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (create_with_spill(spent, own, make) == VK_SUCCESS)
        {
            if (!attachment && !own_only)   // a move is counted by its caller, once it is sure (noteBigMoved)
            {
                mBigOwn.fetch_add(1, std::memory_order_relaxed);
            }
            countImage(memory);
            return VK_SUCCESS;
        }
        image = VK_NULL_HANDLE;
        memory = nullptr;
    }
    if (own_only)
    {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;   // no memory of its own now (the frame's few, the cap): stays where it is
    }
    const VkResult result = create_with_spill(spent, gpu, make);
    if (result == VK_SUCCESS)
    {
        countImage(memory);
        if (big && mBigBlocks && !attachment)
        {
            // Where it went in the end: VMA gives memory of its own to what is
            // over half a default block (64 MB: 32 MB) whatever was asked.
            const bool own = memory->GetType() == VmaAllocation_T::ALLOCATION_TYPE_DEDICATED;
            (own ? mBigOwn : interim ? mBigInterim : mBigHeld).fetch_add(1, std::memory_order_relaxed);
        }
    }
    return result;
}

void LLVKDevice::countImage(VmaAllocation memory)
{
    VmaAllocationInfo info = {};
    vmaGetAllocationInfo(mAllocator, memory, &info);
    mImageBytes.fetch_add(info.size, std::memory_order_relaxed);
}

void LLVKDevice::forgetImage(VmaAllocation memory)
{
    if (!memory || !mAllocator)
    {
        return;
    }
    VmaAllocationInfo info = {};
    vmaGetAllocationInfo(mAllocator, memory, &info);
    mImageBytes.fetch_sub(info.size, std::memory_order_relaxed);
}

bool LLVKDevice::vramBudget(U64& image_bytes, U64& usage, U64& budget, bool* floored)
{
    image_bytes = usage = budget = 0;
    if (floored)
    {
        *floored = false;
    }
    if (!mAllocator || !mMemoryBudget)
    {
        return false;
    }
    const VkPhysicalDeviceMemoryProperties* properties = nullptr;
    vmaGetMemoryProperties(mAllocator, &properties);
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(mAllocator, budgets);
    // igpu-safe: a GPU that shares the PC's memory takes it from every heap (an AMD APU's
    // device-local heap is only the firmware's UMA frame buffer, 512 MB on many: the texture
    // limit sat at its floor of 256 MB with the rest of its memory not counted at all), and
    // all of it is the PC's: held to the share set.
    const U64 shared = mSharedBudget.load(std::memory_order_relaxed);
    for (U32 heap = 0; properties && heap < properties->memoryHeapCount; ++heap)
    {
        if (shared || (properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
        {
            usage += budgets[heap].usage;
            budget += budgets[heap].budget;
        }
    }
    image_bytes = mImageBytes.load(std::memory_order_relaxed);
    if (shared)
    {
        // At least SHARED_TEXTURE_ROOM over what is not a texture image (the rings, the pools, the
        // tables: about 1 GB from the start, the whole share of an 8 GB PC): the texture limit
        // (budget less the rest less a margin) is not held at its floor by memory the textures
        // cannot give back. What runs the PC short past that is plus/memguard's business.
        const U64 others = usage > image_bytes ? usage - image_bytes : 0;
        const U64 lowest = others + SHARED_TEXTURE_ROOM;
        if (floored)
        {
            *floored = lowest > shared && lowest < budget;  // the budget follows what is not a texture image
        }
        budget = llmin(budget, llmax(shared, lowest));
    }
    return budget > 0;
}

void LLVKDevice::setSharedMemoryBudget(U64 bytes)
{
    static const bool allowed = LLRHI::envDefaultOn("LLRHI_VK_SHARED_MEMORY");
    if (!allowed || !bytes)
    {
        LL_INFOS("Vulkan") << "GPU memory from the PC's: device-local heap alone ("
                           << (!allowed ? "LLRHI_VK_SHARED_MEMORY=0" : "no share of the PC's memory known") << ")" << LL_ENDL;
        return;
    }
    mSharedBudget.store(bytes, std::memory_order_relaxed);
    LL_INFOS("Vulkan") << "GPU memory from the PC's: every heap counted against " << (bytes >> 20)
                       << " MB; new memory leaves a device-local heap whose budget is spent (igpu-safe)" << LL_ENDL;
}

U32 LLVKDevice::spentDeviceLocalTypes(U64 bytes)
{
    if (!mAllocator || !mMemoryBudget || !mSharedBudget.load(std::memory_order_relaxed))
    {
        return 0;
    }
    const VkPhysicalDeviceMemoryProperties* properties = nullptr;
    vmaGetMemoryProperties(mAllocator, &properties);
    if (!properties)
    {
        return 0;
    }
    VmaBudget budgets[VK_MAX_MEMORY_HEAPS] = {};
    vmaGetHeapBudgets(mAllocator, budgets);
    U32 spent_heaps = 0;
    bool elsewhere = false;     // a heap that is not device local: where the memory can go instead
    for (U32 heap = 0; heap < properties->memoryHeapCount; ++heap)
    {
        if (!(properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
        {
            elsewhere = true;
        }
        else if (budgets[heap].usage + bytes > budgets[heap].budget)
        {
            spent_heaps |= 1u << heap;
        }
    }
    if (!spent_heaps || !elsewhere)
    {
        return 0;   // room, or one heap for all (Intel's): nowhere else to go
    }
    U32 types = 0;
    for (U32 type = 0; type < properties->memoryTypeCount; ++type)
    {
        if (spent_heaps & (1u << properties->memoryTypes[type].heapIndex))
        {
            types |= 1u << type;
        }
    }
    return types;
}

VkResult LLVKDevice::createBuffer(PoolKind kind, const VkBufferCreateInfo& info, U32 usage, VkBuffer& buffer, VmaAllocation& memory)
{
    buffer = VK_NULL_HANDLE;
    memory = nullptr;
    VmaAllocationCreateInfo gpu = {};
    gpu.usage = (VmaMemoryUsage)usage;
    const U32 spent = spentDeviceLocalTypes(info.size);     // igpu-safe: 0 on a discrete GPU
    if (mSizedPools && kind < POOL_KIND_COUNT)
    {
        U32 type = U32_MAX;
        bool known = false;
        {
            std::lock_guard<std::mutex> lock(mPoolMutex);
            if (mBufferMemoryUsage[kind] == info.usage)
            {
                type = mBufferMemoryTypes[kind];
                known = true;
            }
        }
        if (!known)
        {
            // What vmaCreateBuffer would try first for buffers of this usage.
            if (vmaFindMemoryTypeIndexForBufferInfo(mAllocator, &info, &gpu, &type) != VK_SUCCESS || type >= 32)
            {
                type = U32_MAX;
            }
            std::lock_guard<std::mutex> lock(mPoolMutex);
            mBufferMemoryUsage[kind] = info.usage;
            mBufferMemoryTypes[kind] = type;
        }
        VmaAllocationCreateInfo pooled = gpu;
        pooled.pool = type != U32_MAX ? sizedPool(kind, type, info.size) : VK_NULL_HANDLE;
        if (pooled.pool && (spent & (1u << type)))
        {
            pooled.flags |= VMA_ALLOCATION_CREATE_NEVER_ALLOCATE_BIT;     // igpu-safe: as createImageInPool
        }
        if (pooled.pool && vmaCreateBuffer(mAllocator, &info, &pooled, &buffer, &memory, nullptr) == VK_SUCCESS)
        {
            return VK_SUCCESS;
        }
        buffer = VK_NULL_HANDLE;
        memory = nullptr;
    }
    return create_with_spill(spent, gpu, [&](const VmaAllocationCreateInfo& create)
                             { return vmaCreateBuffer(mAllocator, &info, &create, &buffer, &memory, nullptr); });
}

void LLVKDevice::destroyPools()
{
    std::lock_guard<std::mutex> lock(mPoolMutex);
    for (auto& entry : mPools)
    {
        if (entry.second.pool)
        {
            vmaDestroyPool(mAllocator, entry.second.pool);
        }
    }
    mPools.clear();
    mImageMemoryTypes.clear();
    for (U32 kind = 0; kind < POOL_KIND_COUNT; ++kind)
    {
        mBufferMemoryUsage[kind] = 0;
        mBufferMemoryTypes[kind] = 0;
    }
}

std::string LLVKDevice::poolReport()
{
    U64 used[POOL_KIND_COUNT] = {}, blocks[POOL_KIND_COUNT] = {};
    U32 block_count[POOL_KIND_COUNT] = {}, pools[POOL_KIND_COUNT] = {};
    {
        std::lock_guard<std::mutex> lock(mPoolMutex);
        for (auto& entry : mPools)
        {
            if (entry.second.pool)
            {
                VmaStatistics stats = {};
                vmaGetPoolStatistics(mAllocator, entry.second.pool, &stats);
                used[entry.second.kind] += stats.allocationBytes;
                blocks[entry.second.kind] += stats.blockBytes;
                block_count[entry.second.kind] += stats.blockCount;
                ++pools[entry.second.kind];
            }
        }
    }
    std::ostringstream out;
    for (U32 kind = 0; kind < POOL_KIND_COUNT; ++kind)
    {
        out << (kind ? ", " : "") << POOL_KIND_NAMES[kind] << " pools " << (used[kind] >> 20) << " MB in " << (blocks[kind] >> 20)
            << " MB (" << block_count[kind] << " blocks, " << pools[kind] << " pools)";
    }
    return out.str();
}

namespace
{
    // The blocks of one of VMA's block lists into out, as PoolBlocks of `pool`.
    // VMA has no call that gives a list's blocks one by one: they are read
    // here (this file builds VMA) as its own statistics read them
    // (VmaBlockVector::AddStatistics), under the block list's lock. VMA 3.2.1's
    // insides (m_BlockVector, GetBlock, m_pMetadata): to be checked against
    // vmaCalculatePoolStatistics when VMA is updated.
    void append_blocks(VmaBlockVector& blocks, bool use_mutex, U32 pool, std::vector<LLVKDevice::PoolBlock>& out)
    {
        VmaMutexLockRead read(blocks.GetMutex(), use_mutex);
        for (size_t i = 0; i < blocks.GetBlockCount(); ++i)
        {
            const VmaDeviceMemoryBlock* block = blocks.GetBlock(i);
            const VmaBlockMetadata* metadata = block->m_pMetadata;
            LLVKDevice::PoolBlock b;
            b.memory = block->GetDeviceMemory();
            b.size = metadata->GetSize();
            b.used = b.size - llmin(b.size, (U64)metadata->GetSumFreeSize());
            b.allocations = (U32)metadata->GetAllocationCount();
            b.pool = pool;
            out.push_back(b);
        }
    }

    void sort_blocks(std::vector<LLVKDevice::PoolBlock>& out)
    {
        std::sort(out.begin(), out.end(), [](const LLVKDevice::PoolBlock& a, const LLVKDevice::PoolBlock& b) { return a.memory < b.memory; });
    }
}

bool LLVKDevice::poolBlocks(PoolKind kind, std::vector<PoolBlock>& out)
{
    out.clear();
    if (!mAllocator || !mSizedPools || kind >= POOL_KIND_COUNT)
    {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mPoolMutex);
        U32 ordinal = 0;
        for (auto& entry : mPools)
        {
            if (entry.second.pool && entry.second.kind == kind)
            {
                append_blocks(entry.second.pool->m_BlockVector, mAllocator->m_UseMutex, ++ordinal, out);
            }
        }
    }
    sort_blocks(out);
    return true;
}

void LLVKDevice::defaultBlocks(std::vector<PoolBlock>& out)
{
    out.clear();
    if (!mAllocator)
    {
        return;
    }
    const VkPhysicalDeviceMemoryProperties* properties = nullptr;
    vmaGetMemoryProperties(mAllocator, &properties);
    const bool shared = mSharedBudget.load(std::memory_order_relaxed) != 0;
    for (U32 type = 0; properties && type < properties->memoryTypeCount && type < VK_MAX_MEMORY_TYPES; ++type)
    {
        const U32 heap = properties->memoryTypes[type].heapIndex;
        VmaBlockVector* blocks = mAllocator->m_pBlockVectors[type];
        if (blocks && heap < properties->memoryHeapCount &&
            (shared || (properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)))
        {
            append_blocks(*blocks, mAllocator->m_UseMutex, type + 1, out);
        }
    }
    sort_blocks(out);
}

bool LLVKDevice::poolBytes(PoolKind kind, U64& used, U64& blocks)
{
    used = blocks = 0;
    if (!mAllocator || !mSizedPools || kind >= POOL_KIND_COUNT)
    {
        return false;
    }
    std::lock_guard<std::mutex> lock(mPoolMutex);
    for (auto& entry : mPools)
    {
        if (entry.second.pool && entry.second.kind == kind)
        {
            VmaStatistics stats = {};
            vmaGetPoolStatistics(mAllocator, entry.second.pool, &stats);
            used += stats.allocationBytes;
            blocks += stats.blockBytes;
        }
    }
    return true;
}

bool LLVKDevice::inSizedPool(VmaAllocation memory) const
{
    // VMA's own record of the allocation (no public call tells the pool):
    // the sized pools are the only custom pools this allocator has.
    return memory && memory->GetType() == VmaAllocation_T::ALLOCATION_TYPE_BLOCK && memory->GetParentPool() != VK_NULL_HANDLE;
}

void LLVKDevice::setTrimEmptyBlocks(bool on)
{
    if (!mAllocator)
    {
        return;
    }
    // The memory types of the heaps vramBudget counts (device-local ones; every
    // heap on a GPU that shares the PC's memory) that the CPU does not map:
    // the default blocks of the large images the texture compaction moves.
    // Not the staging and readback memory (host visible, also a GPU's that
    // shares the PC's memory): emptied and made again for nothing. And the
    // sized pools' own types, whatever they are (the compactions' textures and
    // GL buffers; a GPU that shares the PC's memory may have no device-local
    // type the CPU cannot map). Without the sized pools the compactions do not
    // run: nothing.
    U32 types = 0;
    if (on && mSizedPools)
    {
        const VkPhysicalDeviceMemoryProperties* properties = nullptr;
        vmaGetMemoryProperties(mAllocator, &properties);
        const bool shared = mSharedBudget.load(std::memory_order_relaxed) != 0;
        for (U32 type = 0; properties && type < properties->memoryTypeCount && type < 32; ++type)
        {
            const VkMemoryType& memory_type = properties->memoryTypes[type];
            if (!(memory_type.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && memory_type.heapIndex < properties->memoryHeapCount &&
                (shared || (properties->memoryHeaps[memory_type.heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)))
            {
                types |= 1u << type;
            }
        }
        std::lock_guard<std::mutex> lock(mPoolMutex);
        for (auto& entry : mPools)
        {
            const U32 type = entry.second.pool ? entry.second.pool->m_BlockVector.GetMemoryTypeIndex() : 32;
            types |= type < 32 ? 1u << type : 0u;
        }
    }
    mAllocator->m_TrimEmptyBlockTypes.store(types, std::memory_order_relaxed);   // the local change in vk_mem_alloc.h
}

namespace
{
    // The empty blocks of one of VMA's block lists (under its lock for reading).
    void count_empty_blocks(VmaBlockVector& vector, bool use_mutex, U32& blocks, U64& bytes)
    {
        VmaMutexLockRead read(vector.GetMutex(), use_mutex);
        for (size_t i = 0; i < vector.GetBlockCount(); ++i)
        {
            const VmaBlockMetadata* metadata = vector.GetBlock(i)->m_pMetadata;
            if (metadata->IsEmpty())
            {
                ++blocks;
                bytes += metadata->GetSize();
            }
        }
    }
}

void LLVKDevice::emptyBlocks(U32& blocks, U64& bytes)
{
    blocks = 0;
    bytes = 0;
    if (!mAllocator)
    {
        return;
    }
    // VMA 3.2.1's insides (as poolBlocks and blockReport): its default block
    // lists by memory type, then the sized pools'.
    for (U32 type = 0; type < VK_MAX_MEMORY_TYPES; ++type)
    {
        if (VmaBlockVector* vector = mAllocator->m_pBlockVectors[type])
        {
            count_empty_blocks(*vector, mAllocator->m_UseMutex, blocks, bytes);
        }
    }
    std::lock_guard<std::mutex> lock(mPoolMutex);
    for (auto& entry : mPools)
    {
        if (entry.second.pool)
        {
            count_empty_blocks(entry.second.pool->m_BlockVector, mAllocator->m_UseMutex, blocks, bytes);
        }
    }
}

std::string LLVKDevice::poolDetail()
{
    struct Line
    {
        U64 key;
        PoolKind kind;
        VmaStatistics stats;
    };
    std::vector<Line> lines;
    {
        std::lock_guard<std::mutex> lock(mPoolMutex);
        for (auto& entry : mPools)
        {
            if (entry.second.pool)
            {
                Line line = { entry.first, entry.second.kind, {} };
                vmaGetPoolStatistics(mAllocator, entry.second.pool, &line.stats);
                lines.push_back(line);
            }
        }
    }
    std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.key < b.key; });
    std::ostringstream out;
    for (const Line& line : lines)
    {
        const U32 group = (U32)(line.key & 0xff);
        const U32 type = (U32)((line.key >> 8) & 0xff);
        const U64 free_bytes = line.stats.blockBytes - line.stats.allocationBytes;
        out << (out.tellp() > 0 ? "; " : "") << POOL_KIND_NAMES[line.kind] << "s up to ";
        print_group(out, group);
        out << " (type " << type << "): "
            << (line.stats.allocationBytes >> 20) << " MB in " << (line.stats.blockBytes >> 20) << " MB, free "
            << (free_bytes >> 20) << " MB, " << line.stats.blockCount << " blocks, " << line.stats.allocationCount << " allocations";
    }
    return out.str();
}

namespace
{
    void add_statistics(VmaStatistics& into, const VmaStatistics& more)
    {
        into.blockCount += more.blockCount;
        into.allocationCount += more.allocationCount;
        into.blockBytes += more.blockBytes;
        into.allocationBytes += more.allocationBytes;
    }

    // "blocks/MB of blocks/MB allocated/MB free".
    void print_statistics(std::ostream& out, const VmaStatistics& s)
    {
        const U64 free_bytes = s.blockBytes > s.allocationBytes ? s.blockBytes - s.allocationBytes : 0;
        out << s.blockCount << "/" << (s.blockBytes >> 20) << "/" << (s.allocationBytes >> 20) << "/" << (free_bytes >> 20);
    }
}

std::string LLVKDevice::blockReport()
{
    const VkPhysicalDeviceMemoryProperties* properties = nullptr;
    if (mAllocator)
    {
        vmaGetMemoryProperties(mAllocator, &properties);
    }
    if (!properties)
    {
        return std::string();
    }
    std::ostringstream out;
    out << "memory blocks by type (blocks/MB of blocks/MB allocated/MB free):";
    VmaStatistics total = {};
    std::lock_guard<std::mutex> lock(mPoolMutex);
    for (U32 type = 0; type < properties->memoryTypeCount && type < VK_MAX_MEMORY_TYPES; ++type)
    {
        const U32 heap = properties->memoryTypes[type].heapIndex;
        if (heap >= properties->memoryHeapCount || !(properties->memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
        {
            continue;   // as memoryReport's totals: device-local heaps only
        }
        // The sized pools of this type, by kind and size class (vmaGetPoolStatistics:
        // a sum each block keeps, and the pool's own dedicated allocations).
        VmaStatistics pools[POOL_KIND_COUNT][GROUP_COUNT] = {};
        VmaStatistics pool_totals[POOL_KIND_COUNT] = {};
        bool has_pool[POOL_KIND_COUNT] = {};
        for (U32 kind = 0; kind < POOL_KIND_COUNT; ++kind)
        {
            for (U32 g = 0; g < GROUP_COUNT; ++g)
            {
                auto it = mPools.find(pool_key(kind, type, FIRST_GROUP + g));
                if (it != mPools.end() && it->second.pool)
                {
                    vmaGetPoolStatistics(mAllocator, it->second.pool, &pools[kind][g]);
                    add_statistics(pool_totals[kind], pools[kind][g]);
                    has_pool[kind] = true;
                }
            }
        }
        // VMA's default blocks of this type (what the pools do not take) and the
        // allocations in memory of their own. VMA's own sums, read under its
        // locks as vmaCalculateStatistics would. VMA 3.2.1's insides (no public
        // call gives these apart): when VMA is updated, check them against
        // vmaCalculateStatistics.
        VmaStatistics in_default = {}, dedicated = {};
        if (VmaBlockVector* blocks = mAllocator->m_pBlockVectors[type])
        {
            blocks->AddStatistics(in_default);
        }
        mAllocator->m_DedicatedAllocations[type].AddStatistics(dedicated);

        VmaStatistics of_type = in_default;
        add_statistics(of_type, dedicated);
        for (U32 kind = 0; kind < POOL_KIND_COUNT; ++kind)
        {
            add_statistics(of_type, pool_totals[kind]);
        }
        if (!of_type.blockCount)
        {
            continue;
        }
        add_statistics(total, of_type);
        out << " type " << type << " (heap " << heap << "):";
        for (U32 kind = 0; kind < POOL_KIND_COUNT; ++kind)
        {
            if (!has_pool[kind])
            {
                continue;
            }
            out << " " << POOL_KIND_NAMES[kind] << " pools ";
            print_statistics(out, pool_totals[kind]);
            out << " [";
            bool first = true;
            for (U32 g = 0; g < GROUP_COUNT; ++g)
            {
                if (pools[kind][g].blockCount)
                {
                    out << (first ? "" : ", ");
                    print_group(out, FIRST_GROUP + g);
                    out << " ";
                    print_statistics(out, pools[kind][g]);
                    first = false;
                }
            }
            out << "],";
        }
        out << " default blocks ";
        print_statistics(out, in_default);
        out << ", dedicated ";
        print_statistics(out, dedicated);
        out << ";";
    }
    out << " all ";
    print_statistics(out, total);
    return out.str();
}

void LLVKDevice::createPipelineCache()
{
    if (!pipeline_cache_wanted())
    {
        LL_INFOS("Vulkan") << "pipeline cache off (LLRHI_PIPELINE_CACHE=0)" << LL_ENDL;
        return;
    }
    const std::string& dir = LLVKShader::cacheDir();
    mPipelineCachePath = dir.empty() ? std::string() : dir + "/pipelines.vkpc";

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(mPhysicalDevice, &props);
    U64 file_hash = 0;
    std::string file;   // the whole file; the driver data follow its header
    const bool have = !mPipelineCachePath.empty() && read_pipeline_cache(mPipelineCachePath, props, file, file_hash);
    mPipelineCacheHash = have ? file_hash : 0;

    // No VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT: the driver
    // synchronises, since pipelines are created on more than one thread.
    VkPipelineCacheCreateInfo info = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    info.initialDataSize = have ? file.size() - sizeof(PipelineCacheFileHeader) : 0;
    info.pInitialData = have ? file.data() + sizeof(PipelineCacheFileHeader) : nullptr;
    VkResult result = vkCreatePipelineCache(mDevice, &info, nullptr, &mPipelineCache);
    if (result != VK_SUCCESS && have)
    {
        LL_WARNS("Vulkan") << "pipeline cache data rejected (" << (S32)result << "); starting empty" << LL_ENDL;
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        mPipelineCacheHash = 0;
        result = vkCreatePipelineCache(mDevice, &info, nullptr, &mPipelineCache);
    }
    if (result != VK_SUCCESS)
    {
        LL_WARNS("Vulkan") << "pipeline cache could not be created (" << (S32)result << ")" << LL_ENDL;
        mPipelineCache = VK_NULL_HANDLE;
        return;
    }
    mPipelineCacheDirty = false;
    mPipelineCacheReadSize = info.initialDataSize;
    mPipelineCacheSaved = std::chrono::steady_clock::now();
    sPipelineCacheLastFullRead = mPipelineCacheSaved;
    LL_INFOS("Vulkan") << "pipeline cache: loaded " << info.initialDataSize << " bytes from "
                       << (mPipelineCachePath.empty() ? std::string("(no cache folder)") : mPipelineCachePath) << LL_ENDL;
}

bool LLVKDevice::readPipelineCacheData(std::vector<U8>& data)
{
    size_t size = 0;
    VkResult result = vkGetPipelineCacheData(mDevice, mPipelineCache, &size, nullptr);
    if (result == VK_SUCCESS && size)
    {
        data.resize(size);
        result = vkGetPipelineCacheData(mDevice, mPipelineCache, &size, data.data());
    }
    if (result == VK_INCOMPLETE)
    {
        // Grew between the two calls, so the data is cut short. Try again next time.
        mPipelineCacheDirty = true;
        return false;
    }
    if (result != VK_SUCCESS || !size)
    {
        LL_WARNS("Vulkan") << "pipeline cache data unavailable (" << (S32)result << ")" << LL_ENDL;
        return false;
    }
    data.resize(size);
    return true;
}

void LLVKDevice::savePipelineCacheIfDue()
{
    // A lost device: no background read of its cache (the driver may fail or
    // hang in it); the viewer is about to stop (fix-round14 5).
    if (!mPipelineCache || mPipelineCachePath.empty() || isLost() || !mPipelineCacheDirty.load(std::memory_order_relaxed))
    {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    if (std::chrono::duration<F64>(start - mPipelineCacheSaved).count() < PIPELINE_CACHE_SAVE_SECONDS)
    {
        return;
    }
    bool idle = false;
    if (!mPipelineCacheSaving.compare_exchange_strong(idle, true))
    {
        return;     // the previous save is still being written
    }
    mPipelineCacheSaved = start;
    mPipelineCacheDirty = false;

    // Read as well as written on a background thread (research/125 #11: the
    // read alone took 10-20 ms for 31.8 MB on the render thread, every minute,
    // mostly to find nothing new). The cache is internally synchronised (made
    // without VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT), and
    // vkGetPipelineCacheData only reads it, so other threads may go on
    // creating pipelines with it meanwhile. destroyPipelineCache waits for this
    // thread before the cache goes. Normal priority: the driver may hold the
    // cache's lock while it copies, and a starved holder would stall them.
    // Detached: a write cut short by the process exiting leaves only the
    // temporary file behind (removed by a later start, LLVKShader::cacheDir).
    std::thread([this, path = mPipelineCachePath]()
    {
        const auto read_start = std::chrono::steady_clock::now();
        // The size first: the same size as the last read means nothing was
        // added (pipelines were only found in the cache), so the copy and the
        // hash are skipped; so are additions under PIPELINE_CACHE_SAVE_GROWTH
        // (fix-logcache: a few new pipelines rewrote the whole 50 MB file).
        // Should the contents have changed at the same size (a driver evicting
        // entries), or grown by less, the save at shutdown still reads it
        // whole (mPipelineCacheSizeSkipped). A smaller cache is saved now.
        // Smaller additions wait at most PIPELINE_CACHE_SAVE_LONGEST, so a
        // crash does not lose a whole session's few new pipelines.
        size_t size = 0;
        const size_t last_read = mPipelineCacheReadSize.load();
        const bool waited_long = read_start - sPipelineCacheLastFullRead.load() >= PIPELINE_CACHE_SAVE_LONGEST;
        if (vkGetPipelineCacheData(mDevice, mPipelineCache, &size, nullptr) == VK_SUCCESS && size &&
            size >= last_read && (size == last_read || (size - last_read < PIPELINE_CACHE_SAVE_GROWTH && !waited_long)))
        {
            mPipelineCacheSizeSkipped = true;
            if (size > last_read)
            {
                // Grown, but not saved yet: looked at again at the next periodic
                // save (fix-round11 5). Dirty is set again only when a pipeline
                // is made, which may not happen again this session; without
                // this the addition waited for the shutdown, and a crash lost it.
                mPipelineCacheDirty = true;
            }
            mPipelineCacheSaving = false;
            return;
        }
        // Detached, so it may outlive the shutdown's log (fix-round71 5b, as
        // the shader cache upkeep in LLVKShader): once the viewer stops
        // running, nothing more is read, written or logged here. The save in
        // destroyPipelineCache (shutdown) reads the cache whole instead.
        if (!LLApp::isRunning())
        {
            mPipelineCacheSaving = false;
            return;
        }
        std::vector<U8> data;
        if (readPipelineCacheData(data) && LLApp::isRunning())
        {
            mPipelineCacheReadSize = data.size();
            sPipelineCacheLastFullRead = std::chrono::steady_clock::now();
            mPipelineCacheSizeSkipped = false;      // read whole now: an earlier size-only pass is covered
            LL_INFOS("Vulkan") << "pipeline cache: read " << data.size() << " bytes in the background in "
                               << std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - read_start).count()
                               << " ms" << LL_ENDL;
            write_pipeline_cache(path, data, mPipelineCacheHash, "background");
        }
        mPipelineCacheSaving = false;
    }).detach();
}

bool LLVKDevice::claimPipelineCacheSave()
{
    // Up to 10 s for a background save in flight (one save at a time: the
    // hash of the file and the read size are shared).
    bool idle = false;
    for (U32 tries = 0; !mPipelineCacheSaving.compare_exchange_strong(idle, true); ++tries)
    {
        if (tries >= 1000)
        {
            return false;
        }
        idle = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

void LLVKDevice::saveClaimedPipelineCache()
{
    // Also when a periodic save only compared the size (and the data may
    // still differ from the file): the hash then skips an unchanged write.
    const bool dirty = mPipelineCacheDirty.exchange(false);
    const bool size_skipped = mPipelineCacheSizeSkipped.exchange(false);
    if (isLost())
    {
        // A lost device: no vkGetPipelineCacheData (the data may be bad, and the
        // call may fail or hang); the flags are dropped and the old file stays.
        return;
    }
    // Neither flag: the size still tells whether pipelines went in after the
    // last read (fix-round14 2). usePipelineCache marks the cache before the
    // vkCreate*Pipelines call, so a background save running in between clears
    // the mark and reads the cache before the new pipeline is in it.
    bool grown = false;
    if (!dirty && !size_skipped)
    {
        size_t size = 0;
        grown = vkGetPipelineCacheData(mDevice, mPipelineCache, &size, nullptr) == VK_SUCCESS && size &&
                size != mPipelineCacheReadSize.load();
    }
    if (dirty || size_skipped || grown)
    {
        mPipelineCacheSaved = std::chrono::steady_clock::now();
        std::vector<U8> data;
        if (readPipelineCacheData(data))
        {
            mPipelineCacheReadSize = data.size();
            write_pipeline_cache(mPipelineCachePath, data, mPipelineCacheHash, "at shutdown");
        }
    }
}

void LLVKDevice::savePipelineCache()
{
    if (!mPipelineCache || mPipelineCachePath.empty())
    {
        return;
    }
    if (!claimPipelineCacheSave())
    {
        LL_WARNS("Vulkan") << "pipeline cache: a background save is still running; not saved" << LL_ENDL;
        return;
    }
    saveClaimedPipelineCache();
    mPipelineCacheSaving = false;
}

bool LLVKDevice::destroyPipelineCache()
{
    if (!mPipelineCache)
    {
        return true;
    }
    // Claimed and kept: no background save can start after this one, so the
    // cache is not pulled from under a thread still reading it.
    if (!claimPipelineCacheSave())
    {
        LL_WARNS("Vulkan") << "pipeline cache: a background save is still running; the cache and the device are left to the process exit"
                           << LL_ENDL;
        return false;
    }
    if (!mPipelineCachePath.empty())
    {
        saveClaimedPipelineCache();
    }
    vkDestroyPipelineCache(mDevice, mPipelineCache, nullptr);
    mPipelineCache = VK_NULL_HANDLE;
    mPipelineCacheSaving = false;
    return true;
}

VkResult LLVKDevice::submit(const VkSubmitInfo& info, VkFence fence)
{
    return submit(&info, 1, fence);
}

VkResult LLVKDevice::submit(const VkSubmitInfo* infos, U32 count, VkFence fence)
{
    std::lock_guard<std::mutex> lock(mQueueMutex);
    // research/200: with the compute queue in use, every submission signals
    // the graphics timeline and waits for the compute work it depends on.
    LLVKQueues& queues = LLVKQueues::instance();
    return queues.active() ? queues.submitGraphics(mGraphicsQueue, infos, count, fence)
                           : vkQueueSubmit(mGraphicsQueue, count, infos, fence);
}

VkResult LLVKDevice::present(const VkPresentInfoKHR& info)
{
    std::lock_guard<std::mutex> lock(mQueueMutex);
    return vkQueuePresentKHR(mGraphicsQueue, &info);
}

void LLVKDevice::waitIdle()
{
    std::lock_guard<std::mutex> lock(mQueueMutex);
    // Every queue of the device is held (research/200: the compute queue's after the graphics queue's).
    std::lock_guard<std::mutex> compute_lock(LLVKQueues::instance().computeMutex());
    vkDeviceWaitIdle(mDevice);
}

// Called only by a failed init(), before anything else has used the device.
// At exit the device is never destroyed (llvkmirror.h): the mirror's targets,
// the pipelines, the GPU shadow and probe lists and the vertex pools
// (llvkgpulist, llvkgpudriven, llvkattribpool) keep their objects until the
// process ends. A caller that destroys the device after they have used it
// must have each of them free its objects first (fix-round92).
void LLVKDevice::shutdown()
{
    if (mDevice)
    {
        // The queue is held only for the wait itself (a submit from a texture
        // upload thread waits for it to end, not for the whole shutdown):
        // those threads must have stopped submitting before this is called.
        waitIdle();
        LLVKRtAs::shutdown();               // research/230 R2: its acceleration structures (the device idle)
        LLVKScene::shutdown();              // research/197 stage 2: its tables, outputs and pipeline (the device idle)
        LLVKQueues::instance().shutdown();  // research/200
    }
    if (!destroyPipelineCache())
    {
        // A detached background save still calls vkGetPipelineCacheData:
        // destroying the device (or its instance) now would pull both from
        // under it. The process is exiting; the driver frees them then.
        return;
    }
    if (mAllocator)
    {
        destroyPools();
        vmaDestroyAllocator(mAllocator);
        mAllocator = nullptr;
    }
    if (mDevice)
    {
        vkDestroyDevice(mDevice, nullptr);
        mDevice = VK_NULL_HANDLE;
    }
    mDGCEnabled = mDGCReady = false;
    mMaintenance5 = false;
    mFillModeNonSolid = false;
    mDGC = GeneratedCommands();
    mPresentWaitEnabled = false;
    mWaitForPresent = nullptr;
    if (mMessenger || mAddressMessengerHandle)
    {
        auto destroy_messenger = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(mInstance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy_messenger)
        {
            if (mMessenger)
            {
                destroy_messenger(mInstance, mMessenger, nullptr);
            }
            if (mAddressMessengerHandle)
            {
                destroy_messenger(mInstance, mAddressMessengerHandle, nullptr);
            }
        }
        mMessenger = VK_NULL_HANDLE;
        mAddressMessengerHandle = VK_NULL_HANDLE;
    }
    mAddressMessenger = false;
    if (mInstance)
    {
        vkDestroyInstance(mInstance, nullptr);
        mInstance = VK_NULL_HANDLE;
    }
    mPhysicalDevice = VK_NULL_HANDLE;
}
