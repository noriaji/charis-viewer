/**
 * @file llvkwindows.cpp
 * @brief The viewer's other windows (detached UI, research/87): one swapchain
 *        each, presented with the main window's frame.
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

#include "llvkwindows.h"

#include "llrhi.h"
#include "llvkdevice.h"
#include "llvkshader.h"
#include "llvkfullscreen.h"
#include "llvkmirror.h"
#include "llvkswapchain.h"
#include "llvktracker.h"

#include "vk_mem_alloc.h"
#include "llvkdispatch.h"   // after the Vulkan headers: the probes' commands go to the frame's recorder in order

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <vector>

namespace
{
    // The frame's own color format: the window's image is blitted like the frame.
    constexpr VkFormat IMAGE_FORMAT = LLVKMirror::COLOR_FORMAT;

    // The UI texture, texel for texel (both stored GL-style), opaque.
    const char* const COPY_FRAGMENT = R"GLSL(#version 450
layout(std140, set = 0, binding = 0) uniform Copy
{
    ivec4 size;     // xy: the window's image in pixels; z: 1 = green instead (LLRHI_DETACH_COPY_TEST)
};
layout(set = 0, binding = 1) uniform sampler2D ui_image;
layout(location = 0) out vec4 frag_color;
void main()
{
    if (size.z != 0)
    {
        frag_color = vec4(0.0, 1.0, 0.0, 1.0);
        return;
    }
    ivec2 texel = min(ivec2(gl_FragCoord.xy), size.xy - 1);
    frag_color = vec4(texelFetch(ui_image, texel, 0).rgb, 1.0);
}
)GLSL";

    // LLRHI_DETACH_COPY_TEST (research/105, tests):
    //   1: the copy writes green without reading the UI's texture;
    //   2: no copy: the window's image is cleared to cyan in the copy's
    //      rendering scope (vkCmdClearAttachments, no pipeline).
    int copyTest()
    {
        static const int test = []()
        {
            U64 value = 0;
            LLRHI::envNumber("LLRHI_DETACH_COPY_TEST", 0, S32_MAX, value);
            return (int)value;
        }();
        return test;
    }

    struct CopyUniforms
    {
        S32 size[4];
    };

    // LLRHI_DETACH_LOG=1: every LOG_PERIOD, a line per window on what
    // presentWindow did and what its swapchain's acquires and presents
    // returned (research/87: a window that stays black, research/96).
    constexpr char ENV_LOG[] = "LLRHI_DETACH_LOG";
    constexpr auto LOG_PERIOD = std::chrono::seconds(5);

    int logLevel()
    {
        static const int level = []()
        {
            U64 value = 0;
            LLRHI::envNumber(ENV_LOG, 0, S32_MAX, value);
            return (int)value;
        }();
        return level;
    }

    // 1, 2 (plus/detach also shows a test colour) or 3.
    bool logging()
    {
        return logLevel() >= 1;
    }

    // LLRHI_DETACH_LOG=3 (research/105): the log line also gives the middle
    // pixel of the window's image and of its swapchain image after the blit,
    // read back from the frame: where the picture is lost on the way to the
    // screen (plus/detach adds what the screen shows there).
    bool probing()
    {
        const int level = logLevel();
        return level >= 3 && level <= 4;  // 5 measures without them
    }

    // LLRHI_DETACH_LOG=4: also PROBE_POINT_COUNT points of the UI's texture
    // (after the UI was drawn into it) and of the window's image right after
    // the copy, read in the frame's own commands, and the texture's image in
    // words.
    bool deepProbing()
    {
        return logLevel() == 4;
    }

    // LLRHI_DETACH_LOG=5: each window's line also gives the CPU time of its
    // copy being recorded, its acquires and its presents (the frame's own
    // FrameCost line, LLRHI_FRAME_COST=1, has them for all windows together).
    bool timing()
    {
        return logLevel() >= 5;
    }

    // Where deepProbing() reads, in GL rows (bottom up) of a w x h image.
    struct ProbePoint
    {
        const char* name = "";
        S32 x = 0;
        S32 y = 0;
    };
    constexpr U32 PROBE_POINT_COUNT = 7;

    void probePoints(S32 w, S32 h, ProbePoint (&out)[PROBE_POINT_COUNT])
    {
        out[0] = { "bottom left", 4, 4 };
        out[1] = { "bottom right", w - 5, 4 };
        out[2] = { "top left", 4, h - 5 };
        out[3] = { "top right", w - 5, h - 5 };
        out[4] = { "middle", w / 2, h / 2 };
        out[5] = { "title bar", w / 2, h - 10 };
        out[6] = { "close button", w - 12, h - 10 };
    }

    // LLRHI_DETACH_PRESENT: the present mode of the windows' swapchains.
    // Unset or "immediate": as the main window without vsync (IMMEDIATE, else
    // MAILBOX); "mailbox": MAILBOX first (research/87's first choice; the
    // windows stayed black with it, research/105); "fifo": waits for the
    // display (may slow the frame down).
    constexpr char ENV_PRESENT[] = "LLRHI_DETACH_PRESENT";

    void choosePresentMode(LLVKSwapchain& swapchain)
    {
        const char* env = getenv(ENV_PRESENT);
        const std::string mode = env ? env : "";
        swapchain.setSwapInterval(mode == "fifo" ? 1 : 0);
        swapchain.setNoTearing(mode == "mailbox");
    }

    // LLRHI_DETACH_ALPHA (research/105, a test): the swapchains' compositeAlpha.
    // Unset or "opaque": OPAQUE (as the main window); "premul", "postmul",
    // "inherit": that one where the surface offers it, else OPAQUE (the log
    // line says which). The copy writes alpha 1 either way.
    constexpr char ENV_ALPHA[] = "LLRHI_DETACH_ALPHA";

    void chooseCompositeAlpha(LLVKSwapchain& swapchain)
    {
        const char* env = getenv(ENV_ALPHA);
        const std::string alpha = env ? env : "";
        swapchain.setCompositeAlpha(alpha == "premul"  ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR :
                                    alpha == "postmul" ? VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR :
                                    alpha == "inherit" ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR :
                                                         VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR);
    }

    // Why presentWindow did or did not show the window, counted.
    struct Outcomes
    {
        U32 shown = 0;
        U32 not_recording = 0;      // no frame being recorded
        U32 no_copy = 0;            // the copy pass could not be set up
        U32 no_swapchain = 0;
        U32 no_image = 0;
        U32 no_pass = 0;            // LLVKTracker::beginPass refused
        U32 copy_failed = 0;        // the copy recorded nothing
    };

    // probing(): the 8 bytes LLVKMirror::presentExtraWindow fills, and the
    // last values read back from them.
    constexpr U8 PROBE_UNWRITTEN = 0xcd;   // 205,205,205,205 in the log: the frame wrote nothing there

    // Its layout: 0 the window's image at the end of the frame (middle), 4 the
    // swapchain image (middle), then deepProbing()'s PROBE_POINT_COUNT texels
    // of the UI's texture, and as many of the window's image after the copy.
    struct Probe
    {
        static constexpr VkDeviceSize TEXTURE_OFFSET = 8;
        static constexpr VkDeviceSize COPIED_OFFSET = TEXTURE_OFFSET + 4 * PROBE_POINT_COUNT;
        static constexpr VkDeviceSize SIZE = COPIED_OFFSET + 4 * PROBE_POINT_COUNT;
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        void* data = nullptr;       // mapped for good
        U64 recorded_at = 0;        // LLVKMirror::frameBegins() of the frame that fills it; 0: none waiting
        bool read = false;          // last_* hold a reading
        U8 last_image[4] = {};      // RGBA
        U8 last_swapchain[4] = {};  // in the swapchain's format
        U8 last_texture[PROBE_POINT_COUNT][4] = {};     // deepProbing(), RGBA
        U8 last_copied[PROBE_POINT_COUNT][4] = {};      // deepProbing(), RGBA
        std::string texture;        // deepProbing(): the UI's texture in words, when the probe was recorded
    };

    // A window's copy of its UI, one per frame in flight: a frame writes the
    // one of its slot, which the slot's last frame (waited for in beginFrame)
    // was the last to read. Its write then waits for nothing earlier on the
    // queue (research/105: one image waited for every earlier command, the
    // blit of the frame before included).
    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };

    // LLRHI_DETACH_LOG=5: CPU time spent per window, since the last log line.
    struct Times
    {
        U32 frames = 0;             // presentWindow calls that recorded a copy
        F64 record_ms = 0.0;        // presentWindow (copy recorded, probes)
    };

    struct Window
    {
        void* handle = nullptr;     // HWND
        LLVKSwapchain swapchain;
        Image images[LLVKMirror::FRAMES_IN_FLIGHT];
        U32 width = 0;
        U32 height = 0;
        bool failed = false;        // no swapchain on this window: not tried again
        Outcomes outcomes;          // since the last log line
        Times times;
        Probe probe;
        // presentWindow's prepareSampled list (fix-round71 5e): kept, so a
        // frame allocates nothing for it.
        std::vector<LLVKTracker::GLName> sampled;
    };

    std::vector<std::unique_ptr<Window>> sWindows;
    LLVKFullscreen::Pass sCopy;
    bool sCopyTried = false;
    bool sCopyReady = false;

    Window* findWindow(void* handle)
    {
        for (const std::unique_ptr<Window>& window : sWindows)
        {
            if (window->handle == handle)
            {
                return window.get();
            }
        }
        return nullptr;
    }

    bool copyReady()
    {
        if (!sCopyTried)
        {
            sCopyTried = true;
            sCopyReady = sCopy.init("detached window copy", COPY_FRAGMENT, 1);
            if (!sCopyReady)
            {
                LL_WARNS("VKWindows") << "The copy for detached windows could not be set up; they stay blank" << LL_ENDL;
            }
        }
        return sCopyReady;
    }

    // Frees the window's images: now (the GPU is idle), or once the frames
    // recorded so far are done with them.
    void releaseImages(Window& window, bool gpu_idle)
    {
        for (Image& slot_image : window.images)
        {
            if (!slot_image.image)
            {
                continue;
            }
            const Image image = slot_image;
            auto destroy = [image]()
            {
                if (image.view)
                {
                    vkDestroyImageView(LLVKDevice::instance().getDevice(), image.view, nullptr);
                }
                vmaDestroyImage(LLVKDevice::instance().getAllocator(), image.image, image.memory);
            };
            if (gpu_idle)
            {
                destroy();
            }
            else
            {
                LLVKMirror::instance().defer(destroy);
            }
            slot_image = Image();
        }
        window.width = window.height = 0;
    }

    bool makeImage(Image& out, U32 width, U32 height)
    {
        VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = IMAGE_FORMAT;
        info.extent = { width, height, 1 };
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo gpu = {};
        gpu.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(LLVKDevice::instance().getAllocator(), &info, &gpu, &out.image, &out.memory, nullptr) != VK_SUCCESS)
        {
            out = Image();
            return false;
        }
        VkImageViewCreateInfo view = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = out.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = IMAGE_FORMAT;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        if (vkCreateImageView(LLVKDevice::instance().getDevice(), &view, nullptr, &out.view) != VK_SUCCESS)
        {
            out.view = VK_NULL_HANDLE;
            return false;   // the image is released with the others
        }
        return true;
    }

    bool ensureImages(Window& window, U32 width, U32 height)
    {
        if (window.images[0].view && window.width == width && window.height == height)
        {
            return true;
        }
        releaseImages(window, false);
        for (Image& image : window.images)
        {
            if (!makeImage(image, width, height))
            {
                LL_WARNS_ONCE("VKWindows") << "No image for a detached window of " << width << "x" << height << LL_ENDL;
                releaseImages(window, false);
                return false;
            }
        }
        window.width = width;
        window.height = height;
        return true;
    }

    // The slot's image, whatever it held, to be rendered into: its last
    // reader was the slot's previous frame, which has finished (no wait on
    // the queue's earlier commands).
    void toAttachment(VkCommandBuffer cmd, VkImage image)
    {
        VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.srcAccessMask = 0;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo dependency = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    // probing(): the window's probe buffer, made once (host-visible, mapped).
    bool ensureProbe(Window& window)
    {
        Probe& probe = window.probe;
        if (probe.buffer)
        {
            return true;
        }
        VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = Probe::SIZE;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo host = {};
        host.usage = VMA_MEMORY_USAGE_AUTO;
        host.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo allocated = {};
        if (vmaCreateBuffer(LLVKDevice::instance().getAllocator(), &info, &host, &probe.buffer, &probe.memory, &allocated) != VK_SUCCESS)
        {
            probe.buffer = VK_NULL_HANDLE;
            probe.memory = nullptr;
            LL_WARNS_ONCE("VKWindows") << "No readback buffer for the detached windows' probe" << LL_ENDL;
            return false;
        }
        probe.data = allocated.pMappedData;
        return true;
    }

    // Takes the values of the frame that filled the probe, once the GPU has
    // done it: a slot's frame is done when the slot is begun again.
    void readProbe(Window& window)
    {
        Probe& probe = window.probe;
        if (!probe.recorded_at || LLVKMirror::instance().frameBegins() < probe.recorded_at + LLVKMirror::FRAMES_IN_FLIGHT)
        {
            return;
        }
        vmaInvalidateAllocation(LLVKDevice::instance().getAllocator(), probe.memory, 0, Probe::SIZE);
        const U8* data = (const U8*)probe.data;
        memcpy(probe.last_image, data, 4);
        memcpy(probe.last_swapchain, data + 4, 4);
        memcpy(probe.last_texture, data + Probe::TEXTURE_OFFSET, sizeof(probe.last_texture));
        memcpy(probe.last_copied, data + Probe::COPIED_OFFSET, sizeof(probe.last_copied));
        probe.read = true;
        probe.recorded_at = 0;
    }

    void releaseProbe(Window& window)
    {
        Probe& probe = window.probe;
        if (probe.buffer)
        {
            // Only from forgetWindow; the frame being recorded may copy into it (research/109).
            const VkBuffer buffer = probe.buffer;
            const VmaAllocation memory = probe.memory;
            auto destroy = [buffer, memory]() { vmaDestroyBuffer(LLVKDevice::instance().getAllocator(), buffer, memory); };
            if (LLVKMirror::instance().isRecording())
            {
                LLVKMirror::instance().defer(destroy);
            }
            else
            {
                destroy();  // the device is idle (the swapchain's shutdown waited)
            }
        }
        probe = Probe();
    }

    std::string pixelText(const U8 (&pixel)[4])
    {
        std::ostringstream text;
        text << (U32)pixel[0] << "," << (U32)pixel[1] << "," << (U32)pixel[2] << "," << (U32)pixel[3];
        return text.str();
    }

    bool initSwapchain(Window& window)
    {
        if (window.swapchain.hasSurface())
        {
            return true;
        }
        // Only the main window waits for the display: a second window that
        // waited too would halve the frame rate (research/87, Dear ImGui #2970).
        choosePresentMode(window.swapchain);
        chooseCompositeAlpha(window.swapchain);
        window.swapchain.setReadable(probing());
        window.swapchain.setTimed(timing());
        if (!window.swapchain.init(window.handle))
        {
            window.failed = true;
            LL_WARNS("VKWindows") << "A detached window cannot show Vulkan frames: " << window.swapchain.failure() << LL_ENDL;
            window.swapchain.shutdown();
            return false;
        }
        return true;
    }

    std::string resultText(VkResult result)
    {
        return std::to_string((S32)result);
    }

    // LLRHI_DETACH_LOG: one line per window, then the counts start again.
    void logWindows()
    {
        static auto last = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        if (!logging() || now - last < LOG_PERIOD)
        {
            return;
        }
        last = now;
        LLVKMirror::instance().waitExtraPresents();     // their results are written by the present thread
        for (const std::unique_ptr<Window>& window : sWindows)
        {
            const Outcomes& o = window->outcomes;
            const LLVKSwapchain::Results r = window->swapchain.takeResults();
            std::ostringstream line;
            line << "Detached window " << window->handle << ": shown " << o.shown << ", not recording " << o.not_recording
                 << ", no copy " << o.no_copy << ", no swapchain " << o.no_swapchain << ", no image " << o.no_image
                 << ", no pass " << o.no_pass << ", copy failed " << o.copy_failed << "; image " << window->width << "x"
                 << window->height << ", swapchain " << window->swapchain.getWidth() << "x" << window->swapchain.getHeight()
                 << "; acquired " << r.acquired << ", no size " << r.no_size << ", out of date " << r.out_of_date
                 << ", recreate failed " << r.recreate_failed << ", acquire failed " << r.acquire_failed;
            if (r.acquire_failed)
            {
                line << " (last " << resultText(r.last_acquire_error) << ")";
            }
            line << "; presented " << r.presented << " (suboptimal " << r.suboptimal << "), present failed " << r.present_failed;
            if (r.present_failed)
            {
                line << " (last " << resultText(r.last_present_error) << ")";
            }
            line << "; present mode " << (S32)window->swapchain.getPresentMode() << ", format "
                 << (S32)window->swapchain.getFormat() << ", composite alpha " << (U32)window->swapchain.getCompositeAlpha()
                 << " (offered 0x" << std::hex << (U32)window->swapchain.getSupportedCompositeAlpha() << std::dec << ")";
            if (probing())
            {
                const Probe& p = window->probe;
                line << "; middle pixel: image RGBA " << (p.read ? pixelText(p.last_image) : std::string("(none yet)"))
                     << ", swapchain image ";
                if (!window->swapchain.isReadable())
                {
                    line << "(not readable)";
                }
                else
                {
                    line << (p.read ? pixelText(p.last_swapchain) : std::string("(none yet)"));
                }
                if (deepProbing() && p.read)
                {
                    ProbePoint points[PROBE_POINT_COUNT];
                    probePoints((S32)window->width, (S32)window->height, points);
                    line << "; images " << (void*)window->images[0].image << ", " << (void*)window->images[1 % LLVKMirror::FRAMES_IN_FLIGHT].image
                         << "; RGBA at";
                    for (U32 i = 0; i < PROBE_POINT_COUNT; ++i)
                    {
                        line << " [" << points[i].name << " " << points[i].x << "," << points[i].y << ": UI texture "
                             << pixelText(p.last_texture[i]) << ", after the copy " << pixelText(p.last_copied[i]) << "]";
                    }
                    line << "; " << p.texture;
                }
            }
            if (timing())
            {
                // CPU ms per frame that showed the window, and per acquire or present.
                const Times& t = window->times;
                const U32 frames = llmax(t.frames, 1u);
                line << "; CPU ms: copy recorded " << t.record_ms / frames << " (" << t.frames << " frames), acquire "
                     << r.acquire_ms / llmax(r.acquired + r.not_ready + r.no_size, 1u) << " (no free image " << r.not_ready
                     << " times), present " << r.present_ms / llmax(r.presented + r.present_failed, 1u);
            }
            LL_INFOS("VKWindows") << line.str() << LL_ENDL;
            window->outcomes = Outcomes();
            window->times = Times();
        }
    }

    Window* findOrAddWindow(void* handle)
    {
        Window* window = findWindow(handle);
        if (!window)
        {
            sWindows.push_back(std::make_unique<Window>());
            window = sWindows.back().get();
            window->handle = handle;
        }
        return window;
    }

    // LLRHI::sPresentWindow.
    bool presentWindow(void* handle, U32 gl_texture, U32 width, U32 height)
    {
        const auto start = std::chrono::steady_clock::now();
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        LLVKMirror& mirror = LLVKMirror::instance();
        if (!handle || !gl_texture || !width || !height)
        {
            return false;
        }
        Window* window = findOrAddWindow(handle);
        logWindows();
        Outcomes& outcomes = window->outcomes;
        if (!mirror.isRecording())
        {
            ++outcomes.not_recording;
            return false;
        }
        if (!copyReady())
        {
            ++outcomes.no_copy;
            return false;
        }
        if (window->failed || !initSwapchain(*window))
        {
            ++outcomes.no_swapchain;
            return false;
        }
        if (!ensureImages(*window, width, height))
        {
            ++outcomes.no_image;
            return false;
        }

        // This frame fills the window's probe (one frame at a time uses it).
        VkBuffer probe = VK_NULL_HANDLE;
        if (probing() && ensureProbe(*window))
        {
            readProbe(*window);
            if (!window->probe.recorded_at)
            {
                // Marked first: what the frame does not get to write shows as
                // PROBE_UNWRITTEN in the log.
                memset(window->probe.data, PROBE_UNWRITTEN, Probe::SIZE);
                vmaFlushAllocation(LLVKDevice::instance().getAllocator(), window->probe.memory, 0, Probe::SIZE);
                probe = window->probe.buffer;
                window->probe.recorded_at = mirror.frameBegins();
            }
        }
        std::vector<std::pair<S32, S32>> texels;
        if (probe && deepProbing())
        {
            ProbePoint points[PROBE_POINT_COUNT];
            probePoints((S32)width, (S32)height, points);
            for (const ProbePoint& point : points)
            {
                texels.emplace_back(point.x, point.y);
            }
            window->probe.texture = LLVKTracker::describeTexture(gl_texture);
            LLVKTracker::copyTexels(gl_texture, texels, probe, Probe::TEXTURE_OFFSET);
        }

        // The tracker keeps the UI texture's layout: it moves it to shader-read.
        // The copy goes to the window's own image, not the bound framebuffer.
        window->sampled.assign(1, gl_texture);
        if (!LLVKTracker::prepareSampled(window->sampled))
        {
            ++outcomes.no_pass;
            return false;
        }
        // The slot's image: what it held (the slot's last frame, done) is discarded.
        const Image& slot_image = window->images[mirror.getSlot() % LLVKMirror::FRAMES_IN_FLIGHT];
        toAttachment(mirror.transferCommands(), slot_image.image);
        LLVKMirror::Target target;
        target.colors[0] = slot_image.view;
        target.color_formats[0] = IMAGE_FORMAT;
        target.color_count = 1;
        target.width = width;
        target.height = height;
        VkCommandBuffer cmd = mirror.beginRendering(target);
        bool drawn = false;
        if (cmd && copyTest() == 2)
        {
            VkClearAttachment clear = {};
            clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            clear.colorAttachment = 0;
            clear.clearValue.color = { { 0.f, 1.f, 1.f, 1.f } };
            VkClearRect rect = {};
            rect.rect = { { 0, 0 }, { width, height } };
            rect.layerCount = 1;
            vkCmdClearAttachments(cmd, 1, &clear, 1, &rect);
            drawn = true;
        }
        else if (cmd)
        {
            const CopyUniforms uniforms = { { (S32)width, (S32)height, copyTest() == 1 ? 1 : 0, 0 } };
            const LLVKFullscreen::Image image = { gl_texture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            drawn = sCopy.draw(cmd, target, &uniforms, sizeof(uniforms), &image, 1);
        }
        mirror.endRendering();
        if (drawn && !texels.empty())
        {
            // The window's image as the copy left it, in the frame's commands.
            VkCommandBuffer transfer = mirror.transferCommands();
            LLVKMirror::imageBarrier(transfer, slot_image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 1, 1);
            std::vector<VkBufferImageCopy> regions;
            for (const std::pair<S32, S32>& texel : texels)
            {
                VkBufferImageCopy region = {};
                region.bufferOffset = Probe::COPIED_OFFSET + regions.size() * 4;
                region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.imageOffset = { llclamp(texel.first, 0, (S32)width - 1), llclamp(texel.second, 0, (S32)height - 1), 0 };
                region.imageExtent = { 1, 1, 1 };
                regions.push_back(region);
            }
            vkCmdCopyImageToBuffer(transfer, slot_image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, probe, (U32)regions.size(),
                                   regions.data());
            LLVKMirror::imageBarrier(transfer, slot_image.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 1, 1);
        }
        LLVKTracker::endPass();
        if (drawn)
        {
            ++outcomes.shown;
            mirror.presentExtraWindow(&window->swapchain, slot_image.image, width, height, probe);
            if (timing())
            {
                ++window->times.frames;
                window->times.record_ms += std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - start).count();
            }
        }
        else
        {
            ++outcomes.copy_failed;
        }
        return drawn;
    }

    // LLRHI::sForgetWindow.
    void forgetWindow(void* handle)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        auto it = std::find_if(sWindows.begin(), sWindows.end(),
                               [handle](const std::unique_ptr<Window>& window) { return window->handle == handle; });
        if (it == sWindows.end())
        {
            return;
        }
        Window& window = **it;
        LLVKMirror::instance().forgetExtraWindow(&window.swapchain);
        window.swapchain.shutdown();    // waits for the device to be idle
        // research/109: the device being idle covers what was submitted, not
        // what this frame has recorded so far (the window drawn into its
        // image, the probe copy): those go once the frame is done.
        releaseImages(window, !LLVKMirror::instance().isRecording());
        releaseProbe(window);
        sWindows.erase(it);
    }
}

void LLVKWindows::install()
{
    LLRHI::sPresentWindow = &presentWindow;
    LLRHI::sForgetWindow = &forgetWindow;
    LLRHI::sDescribeDrawTarget = []()
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        return LLVKTracker::describeDrawTarget();
    };
    LLVKShader::addBakeStep("detached window copy", []() { std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock()); return copyReady(); });   // research/159 3.1
}
