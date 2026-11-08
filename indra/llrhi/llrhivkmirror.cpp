/**
 * @file llrhivkmirror.cpp
 * @brief The Vulkan renderer's frame loop: GL calls go into the Vulkan frame, Vulkan presents it.
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

#include "llrhivkmirror.h"

#if LL_VULKAN && LL_WINDOWS

#include "llglheaders.h"
#include "llfile.h"
#include "llimagepng.h"
#include "llrhi.h"
#include "llrhidrawlist.h"
#include "llrhipassstate.h"   // research/204: the pass states' VKPerf line
#include "llrhiframe.h"       // research/208 S0: the render thread census
#include "replay/llrhireplaycapture.h"   // research/271 P1b stage 2
#include "llstring.h"
#include "llthread.h"   // on_main_thread (research/180 9 3)
#include "llvkbenchmark.h"
#include "llvkbindless.h"
#include "llvkdevice.h"
#include "llvkdrawlist.h"
#include "llvkgl.h"
#include "llvkimmediate.h"
#include "llvkmirror.h"
#include "llvkocclusion.h"
#include "llvkpass.h"
#include "llvkpassstate.h"
#include "llvkrecorder.h"
#include "llvkresource.h"
#include "llvkswapchain.h"
#include "llvktracker.h"
#include "llvkwindows.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

extern bool (*gLLRHIPresentHook)();
// research/117 G5, G6 (llwindowwin32.cpp): set here before the window exists:
// the viewer runs without a GL context.
extern PROC (*gLLRHIGetProcAddress)(const char* name);
extern bool (*gLLRHIAttachWindow)(HWND window);

namespace
{
    U32 sPresents = 0;

    // Frame pacing, logged every 5 s: where a frame's time goes outside the draw translation.
    using Clock = std::chrono::steady_clock;
    Clock::time_point sPeriodStart = Clock::now();
    U32 sPeriodFrames = 0;
    F64 sEndFrameMs = 0.0, sBeginFrameMs = 0.0;

    F64 ms_since(Clock::time_point start)
    {
        return std::chrono::duration<F64, std::milli>(Clock::now() - start).count();
    }

    F64 ms_between(Clock::time_point start, Clock::time_point end)
    {
        return std::chrono::duration<F64, std::milli>(end - start).count();
    }

    // ------------------------------------------------------------------
    // Frame cost (LLRHI::sFrameCost, research/83). The main thread's frame is
    // cut into segments at every gpuMark and cpuMark; present_frame closes the
    // open one, times its own steps (LLVKMirror::takeFrameCost) and opens
    // "after present". Main thread only: marks from other threads are ignored.
    struct CostSegment
    {
        const char* scope;
        const char* name;
        F64 ms;
        U64 draws;      // through the GL translator's draw() (direct draws that take it included)
        U64 direct;     // direct draws (templates and runs included)
        U64 scopes;     // rendering scopes begun
        U64 sets;       // per-frame descriptor sets allocated
    };

    struct CostCounters
    {
        U32 draws = 0;
        U32 direct = 0;
        U64 scopes = 0;
        U64 sets = 0;
    };

    struct FrameCostState
    {
        bool on = false;                    // as at the last present
        std::thread::id main;
        std::vector<CostSegment> segments;  // since the last report, in order of first appearance
        S32 open = -1;                      // the open segment, -1: none
        Clock::time_point open_start;
        CostCounters open_counters;
        bool have_present = false;
        Clock::time_point last_present;
        std::vector<F32> frame_ms;          // present to present
        U32 frames = 0;                     // presents measured
        F64 lock_wait = 0.0;                // present_frame waiting for the tracker lock
        F64 present_ms = 0.0;               // all of present_frame
        F64 report_ms = 0.0;                // the 5 s report lines (in present_ms)
        F64 pacing_ms = 0.0;                // waiting for the display (frame pacing, research/94)
        // present_frame's own steps around endFrame and beginFrame (research/95).
        F64 cue_ms = 0.0;                   // capture cue, swap interval and frame pacing setting
        F64 frame_stats_ms = 0.0;           // LLVKGL::resetFrameStats and the window size
        F64 bindless_ms = 0.0;              // LLVKBindless::beginFrame
        F64 compress_ms = 0.0;              // LLVKGL::compressTextures
        F64 compact_ms = 0.0;               // LLVKGL::compactTextures and compactBuffers (research/245, 285)
    };

    // Adds the time since the last call (or construction) to a step; no clock while off.
    struct StepLap
    {
        explicit StepLap(bool on) : mOn(on), mMark(on ? Clock::now() : Clock::time_point()) {}
        void operator()(F64& into)
        {
            if (mOn)
            {
                const Clock::time_point now = Clock::now();
                into += ms_between(mMark, now);
                mMark = now;
            }
        }
        // Starts over without counting (the time went to another step).
        void restart()
        {
            if (mOn)
            {
                mMark = Clock::now();
            }
        }
        const bool mOn;
        Clock::time_point mMark;
    };

    // The GL swap interval (RenderVSyncEnable) is read this often, not at every
    // present (research/95: wglGetSwapIntervalEXT was a call into the GL driver,
    // which may wait for its own thread; it is the translation's own now,
    // research/117). LLRHI_SWAP_INTERVAL_EVERY_FRAME=1 reads it at every present.
    constexpr std::chrono::milliseconds SWAP_INTERVAL_POLL_PERIOD(250);
    Clock::time_point sSwapIntervalPolled;
    const bool sSwapIntervalEveryFrame = []()
    {
        const char* env = std::getenv("LLRHI_SWAP_INTERVAL_EVERY_FRAME");
        return env && *env && strcmp(env, "0");
    }();
    FrameCostState sCost;

    bool cost_same(const char* a, const char* b)
    {
        return a == b || (a && b && !strcmp(a, b));
    }

    CostCounters cost_counters()
    {
        CostCounters c;
        LLVKGL::drawCounts(c.draws, c.direct);
        const LLVKMirror& mirror = LLVKMirror::instance();
        c.scopes = mirror.renderScopes();
        c.sets = mirror.setsAllocated();
        return c;
    }

    void cost_close(Clock::time_point now)
    {
        if (sCost.open < 0)
        {
            return;
        }
        CostSegment& s = sCost.segments[sCost.open];
        s.ms += llmax(ms_between(sCost.open_start, now), 0.0);
        const CostCounters c = cost_counters();
        const CostCounters& o = sCost.open_counters;
        // The draw counts start over only in present_frame, never inside a segment.
        s.draws += c.draws >= o.draws ? c.draws - o.draws : 0;
        s.direct += c.direct >= o.direct ? c.direct - o.direct : 0;
        s.scopes += c.scopes - o.scopes;
        s.sets += c.sets - o.sets;
        sCost.open = -1;
    }

    void cost_open(const char* scope, const char* name, Clock::time_point now)
    {
        S32 index = -1;
        for (size_t i = 0; i < sCost.segments.size(); ++i)
        {
            if (cost_same(sCost.segments[i].scope, scope) && cost_same(sCost.segments[i].name, name))
            {
                index = (S32)i;
                break;
            }
        }
        if (index < 0)
        {
            sCost.segments.push_back({ scope, name, 0.0, 0, 0, 0, 0 });
            index = (S32)sCost.segments.size() - 1;
        }
        sCost.open = index;
        sCost.open_start = now;
        sCost.open_counters = cost_counters();
    }

    // A gpuMark or cpuMark (under the tracker lock).
    void cost_mark(const char* scope, const char* name)
    {
        if (!sCost.on || !name || std::this_thread::get_id() != sCost.main)
        {
            return;
        }
        const Clock::time_point now = Clock::now();
        cost_close(now);
        cost_open(scope, name, now);
    }

    // Accounting started or stopped (present_frame, under the tracker lock).
    void cost_restart(bool on)
    {
        sCost.segments.clear();
        sCost.open = -1;
        sCost.have_present = false;
        sCost.frame_ms.clear();
        sCost.frames = 0;
        sCost.lock_wait = sCost.present_ms = sCost.report_ms = sCost.pacing_ms = 0.0;
        sCost.cue_ms = sCost.frame_stats_ms = sCost.bindless_ms = sCost.compress_ms = sCost.compact_ms = 0.0;
        LLRHI::sImmediate = LLRHI::ImmediateCounts();
        sCost.on = on;
        sCost.main = std::this_thread::get_id();
        LLVKMirror::instance().takeFrameCost();    // what was measured before is from another period
        LL_INFOS("FrameCost") << "frame cost accounting " << (on ? "on" : "off")
                              << (on ? (LLRHI::sGPUTiming ? " (GPU timestamps on)" : " (no GPU timestamps)") : "") << LL_ENDL;
    }

    F32 percentile(std::vector<F32>& values, F32 fraction)
    {
        if (values.empty())
        {
            return 0.f;
        }
        const size_t at = llmin((size_t)(fraction * (values.size() - 1) + 0.5f), values.size() - 1);
        std::nth_element(values.begin(), values.begin() + at, values.end());
        return values[at];
    }

    // research/94: "stalls" counts the frames this much longer than the
    // median (with VSync on, frames that took two refreshes or more); "sd" is
    // the standard deviation of the frame times.
    constexpr F32 STALL_OVER_MEDIAN = 1.5f;

    // The FrameCost line: per frame averages since the last one.
    std::string cost_report(F64 period_ms, const char* pacing, U32 pacing_skips)
    {
        const LLVKMirror::FrameCost m = LLVKMirror::instance().takeFrameCost();
        const U32 frames = llmax(sCost.frames, 1u);
        const F64 n = (F64)frames;
        std::ostringstream out;
        out << std::fixed << std::setprecision(2);
        out << "frames " << sCost.frames << ", fps " << sCost.frames * 1000.0 / llmax(period_ms, 1.0) << "; frame ms";
        if (!sCost.frame_ms.empty())
        {
            F64 sum = 0.0, sum_sq = 0.0;
            for (F32 ms : sCost.frame_ms)
            {
                sum += ms;
                sum_sq += (F64)ms * ms;
            }
            const F64 count = (F64)sCost.frame_ms.size();
            const F64 mean = sum / count;
            const F32 max = *std::max_element(sCost.frame_ms.begin(), sCost.frame_ms.end());
            const F32 sd = (F32)std::sqrt(llmax(sum_sq / count - mean * mean, 0.0));
            const F32 p95 = percentile(sCost.frame_ms, 0.95f);
            const F32 p50 = percentile(sCost.frame_ms, 0.5f);
            const auto stalls = std::count_if(sCost.frame_ms.begin(), sCost.frame_ms.end(),
                                              [p50](F32 ms) { return ms > STALL_OVER_MEDIAN * p50; });
            out << " avg " << (F32)mean << " p50 " << p50 << " p95 " << p95 << " max " << max << " sd " << sd
                << " stalls " << stalls;
        }
        out << "; frame pacing " << pacing << " (waits skipped as GPU-bound " << pacing_skips << ")";

        // Where the main thread's frame goes (ms per frame; [draws, direct draws, rendering scopes, descriptor sets]).
        F64 main_total = sCost.present_ms + sCost.pacing_ms;
        out << "; main thread ms/frame:";
        for (const CostSegment& s : sCost.segments)
        {
            main_total += s.ms;
            out << " " << (s.scope ? s.scope : "") << (s.scope ? " " : "") << s.name << " " << s.ms / n;
            if (s.draws || s.direct || s.scopes || s.sets)
            {
                out << " [" << (F64)s.draws / n << ", " << (F64)s.direct / n << ", " << (F64)s.scopes / n << ", " << (F64)s.sets / n << "]";
            }
            out << ",";
        }
        out << " present " << sCost.present_ms / n << ", pacing wait " << sCost.pacing_ms / n << ", sum " << main_total / n;

        // The present: frame end and start (in present_frame).
        const F64 steps = sCost.lock_wait + m.before_submit + m.recorder_wait + m.record_end + m.acquire + m.submit + m.present +
                          m.other_windows + m.other_presents + m.main_present_wait +
                          m.fence_wait + m.deferred + m.reset + m.resize + m.begin + sCost.report_ms +
                          sCost.cue_ms + sCost.frame_stats_ms + sCost.bindless_ms + sCost.compress_ms + sCost.compact_ms;
        out << "; present ms/frame: lock wait " << sCost.lock_wait / n << ", cue " << sCost.cue_ms / n
            << ", before submit " << m.before_submit / n
            << ", recorder wait " << m.recorder_wait / n << ", record end " << m.record_end / n << ", acquire " << m.acquire / n
            << ", submit " << m.submit / n << ", present " << m.present / n;
        if (m.other_windows > 0.0 || m.other_presents > 0.0)
        {
            out << ", detached windows " << m.other_windows / n << " (presents " << m.other_presents / n << ")";
        }
        if (m.main_presents || m.main_present_wait > 0.0)
        {
            // research/179 6.6 4 (LLRHI_PRESENT_THREAD): "present" above is the hand-over then.
            out << ", main present thread: wait " << m.main_present_wait / n << ", its vkQueuePresentKHR "
                << m.main_present_call / llmax(m.main_presents, 1u) << " ms each (" << m.main_presents << " presents)";
        }
        out << ", GPU wait " << m.fence_wait / n
            << ", frame stats " << sCost.frame_stats_ms / n << ", deferred " << m.deferred / n << ", reset " << m.reset / n
            << ", begin " << m.begin / n << ", bindless " << sCost.bindless_ms / n << ", compress " << sCost.compress_ms / n
            << ", compact " << sCost.compact_ms / n;
        if (m.resizes)
        {
            out << ", resize " << m.resize / n << " (" << m.resizes << " times)";
        }
        out << ", report " << sCost.report_ms / n << ", rest " << llmax(sCost.present_ms - steps, 0.0) / n;

        // Immediate mode (the UI's draws mostly): what ends its batches.
        const LLRHI::ImmediateCounts& im = LLRHI::sImmediate;
        out << "; immediate mode per frame: flushes " << im.flushes / n << ", texture changes " << im.texture_flushes / n
            << ", same texture " << im.same_texture_flushes / n << ", VB cache misses " << im.cache_misses / n;
        LLRHI::sImmediate = LLRHI::ImmediateCounts();

        // Waits inside the frame (whole GPU syncs).
        out << "; in-frame syncs (whole period): submitAndWait " << m.sync_submits << ", submitAndWait ms " << m.sync_ms
            << ", one-off submissions " << m.immediates << ", one-off ms " << m.immediate_ms;

        // The GPU (needs the timestamps).
        if (m.gpu_frames)
        {
            const F64 span = m.gpu_span_ms / m.gpu_frames;
            const F64 idle = m.gpu_gaps ? m.gpu_idle_ms / m.gpu_gaps : 0.0;
            out << "; GPU ms/frame: busy " << span << ", idle between frames " << idle << " ("
                << (span + idle > 0.0 ? 100.0 * span / (span + idle) : 0.0) << "% busy, " << m.gpu_frames << " frames)";
        }
        else
        {
            out << "; GPU: no timestamps (LLRHI_FRAME_COST=1 or gputiming.toggle)";
        }

        // Which side sets the pace: the GPU (the CPU waits for its fence), the
        // swapchain (no image free: vsync), the display (frame pacing waits
        // for it: vsync), or the CPU. The longest wait above PACING_WAIT_MS.
        constexpr F64 PACING_WAIT_MS = 0.5;  // a wait per frame above this sets the pace
        const struct { F64 ms; const char* name; } waits[] =
        {
            { m.fence_wait / n, "GPU" },
            { m.acquire / n, "present (swapchain)" },
            { m.main_present_wait / n, "present (the present thread, research/179)" },
            { sCost.pacing_ms / n, "display (frame pacing)" },
        };
        const char* paced_by = "CPU";
        F64 longest = PACING_WAIT_MS;
        for (const auto& wait : waits)
        {
            if (wait.ms > longest)
            {
                longest = wait.ms;
                paced_by = wait.name;
            }
        }
        out << "; paced by " << paced_by;
        return out.str();
    }
    // For the GPU timing line: the GPU's temperature, SM clock and why its
    // clocks are held down right now, from NVML (nvml.dll, which the NVIDIA
    // driver puts in System32). Loaded on first use; empty when it is not
    // there (other GPUs). Reads the first NVIDIA GPU.
    std::string gpu_sensors()
    {
        typedef int (*NvmlInit)();
        typedef int (*NvmlHandleByIndex)(unsigned int index, void** device);
        typedef int (*NvmlTemperature)(void* device, int sensor, unsigned int* celsius);
        typedef int (*NvmlClockInfo)(void* device, int type, unsigned int* mhz);
        typedef int (*NvmlReasons)(void* device, unsigned long long* reasons);
        static bool loaded = false;
        static void* device = nullptr;
        static NvmlTemperature temperature = nullptr;
        static NvmlClockInfo clock_info = nullptr;
        static NvmlReasons reasons = nullptr;
        if (!loaded)
        {
            loaded = true;
            HMODULE nvml = LoadLibraryExA("nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (nvml)
            {
                NvmlInit init = (NvmlInit)GetProcAddress(nvml, "nvmlInit_v2");
                NvmlHandleByIndex handle = (NvmlHandleByIndex)GetProcAddress(nvml, "nvmlDeviceGetHandleByIndex_v2");
                temperature = (NvmlTemperature)GetProcAddress(nvml, "nvmlDeviceGetTemperature");
                clock_info = (NvmlClockInfo)GetProcAddress(nvml, "nvmlDeviceGetClockInfo");
                reasons = (NvmlReasons)GetProcAddress(nvml, "nvmlDeviceGetCurrentClocksEventReasons");
                if (!reasons)
                {
                    reasons = (NvmlReasons)GetProcAddress(nvml, "nvmlDeviceGetCurrentClocksThrottleReasons");     // older drivers
                }
                if (!init || !handle || init() != 0 || handle(0, &device) != 0)
                {
                    device = nullptr;
                }
            }
        }
        if (!device)
        {
            return std::string();
        }
        std::ostringstream out;
        unsigned int value = 0;
        if (temperature && temperature(device, 0 /* NVML_TEMPERATURE_GPU */, &value) == 0)
        {
            out << "; GPU " << value << " C";
        }
        if (clock_info && clock_info(device, 1 /* NVML_CLOCK_SM */, &value) == 0)
        {
            out << ", SM " << value << " MHz";
        }
        if (clock_info && clock_info(device, 2 /* NVML_CLOCK_MEM */, &value) == 0)
        {
            out << ", memory " << value << " MHz";
        }
        unsigned long long bits = 0;
        if (reasons && reasons(device, &bits) == 0)
        {
            // nvmlClocksEventReasons / nvmlClocksThrottleReasons bits.
            static const struct { unsigned long long bit; const char* name; } names[] =
            {
                { 0x1, "idle" }, { 0x2, "app clocks" }, { 0x4, "power cap" }, { 0x8, "hw slowdown" },
                { 0x10, "sync boost" }, { 0x20, "sw thermal" }, { 0x40, "hw thermal" },
                { 0x80, "power brake" }, { 0x100, "display clocks" },
            };
            out << ", clocks held by:";
            bool any = false;
            for (const auto& reason : names)
            {
                if (bits & reason.bit)
                {
                    out << (any ? ", " : " ") << reason.name;
                    any = true;
                }
            }
            if (!any)
            {
                out << " nothing";
            }
        }
        return out.str();
    }

    U32 sVulkanPresented = 0;
    LLVKSwapchain sSwapchain;   // the window shows the Vulkan frame
    // The viewer's window (research/117 G5: it has no GL context).
    HWND sWindow = nullptr;

    HWND frame_window()
    {
        return sWindow;
    }
    std::set<U32> sCaptureFrames;   // LLRHI_VK_CAPTURE
    U32 sCaptureEvery = 0;          // LLRHI_VK_CAPTURE_EVERY: also capture every N presents
    std::string sDir;               // where captures go; empty: no captures

    void window_size(U32& width, U32& height)
    {
        width = height = 0;
        HWND hwnd = frame_window();
        RECT rect;
        if (hwnd && GetClientRect(hwnd, &rect))
        {
            width = rect.right - rect.left;
            height = rect.bottom - rect.top;
        }
    }

    void save_png(const std::string& path, U32 width, U32 height, std::vector<U8>& pixels)
    {
        LLPointer<LLImageRaw> raw = new LLImageRaw(width, height, 4);
        U8* dst = raw->getData();
        for (size_t i = 0; i < pixels.size(); i += 4)
        {
            dst[i + 0] = pixels[i + 0];
            dst[i + 1] = pixels[i + 1];
            dst[i + 2] = pixels[i + 2];
            dst[i + 3] = 255;   // back buffer alpha is not meaningful
        }
        // LLImageRaw rows are bottom-up.
        raw->verticalFlip();
        LLPointer<LLImagePNG> png = new LLImagePNG();
        if (png->encode(raw, 0.f))
        {
            png->save(path);
        }
    }

    bool present_frame()
    {
        LLRHI::renderThreadCensusJoin();    // research/208 S0: the join point (before the lock: its take is the present's)
        const Clock::time_point entered = Clock::now();
        // Other threads (LLImageGLThread's texture uploads) record into this
        // frame's command buffers and ring under the tracker lock: submitting and
        // starting the next frame must not interleave with them.
        std::lock_guard<LLVKTrackerMutex> frame_lock(LLVKTracker::lock());
        // Frame cost (research/83): the frame's last segment ends here.
        const bool cost = LLRHI::sFrameCost;
        Clock::time_point present_from = entered;   // present_ms counts from here
        if (cost != sCost.on)
        {
            cost_restart(cost);
        }
        if (cost)
        {
            sCost.lock_wait += ms_since(entered);
            cost_close(entered);
            if (sCost.have_present)
            {
                sCost.frame_ms.push_back((F32)ms_between(sCost.last_present, entered));
            }
            sCost.last_present = entered;
            sCost.have_present = true;
            ++sCost.frames;
        }
        StepLap lap(cost);  // present_frame's own steps (endFrame and beginFrame time theirs)
        ++sPresents;
        LLVKMirror& mirror = LLVKMirror::instance();
        bool presented = false;
        bool capture = !sDir.empty() && mirror.getWidth() &&
                       (sCaptureFrames.count(sPresents) || (sCaptureEvery && sPresents % sCaptureEvery == 0));
        // On cue: <dir>/capture.now present -> capture this frame and remove it.
        if (!capture && !sDir.empty() && sPresents % 10 == 0 && mirror.getWidth())
        {
            std::string cue = sDir + "/capture.now";
            if (LLFile::isfile(cue))
            {
                LLFile::remove(cue);
                capture = true;
            }
        }

        // research/179 6.6 4: the last frame's present on the present thread
        // returns before the swapchain is touched here (its wait is timed apart).
        lap(sCost.cue_ms);
        mirror.waitMainPresent();
        lap.restart();
        // Read now, while nothing presents to it (the FrameCost line, below).
        const char* const pacing_state = sSwapchain.pacingState();

        // Present mode follows the GL swap interval (RenderVSyncEnable sets it
        // through LLWindowWin32::toggleVSync); the GL swap itself is skipped.
        LLVKSwapchain* swapchain = sSwapchain.hasSurface() ? &sSwapchain : nullptr;
        if (swapchain && wglGetSwapIntervalEXT &&
            (sSwapIntervalEveryFrame || entered - sSwapIntervalPolled >= SWAP_INTERVAL_POLL_PERIOD))
        {
            sSwapIntervalPolled = entered;
            swapchain->setSwapInterval(wglGetSwapIntervalEXT());
        }
        if (swapchain)
        {
            swapchain->setPacing(LLRHI::sFramePacing);
            swapchain->setNoTearing(LLRHI::sPresentMailbox);    // research/179 6.6 4 (a change: a new swapchain)
        }
        lap(sCost.cue_ms);

        if (capture)
        {
            U32 width = mirror.getWidth(), height = mirror.getHeight();
            std::vector<U8> vk_pixels;
            presented = mirror.endFrame(&vk_pixels, swapchain);
            if (!vk_pixels.empty())
            {
                std::ostringstream vk_path;
                vk_path << sDir << "/vk_" << sPresents << ".png";
                save_png(vk_path.str(), width, height, vk_pixels);
            }
            LL_INFOS("VKMirror") << "frame " << sPresents << ": " << LLVKGL::frameReport()
                                 << (swapchain ? ", presented by Vulkan so far: " + std::to_string(sVulkanPresented + (presented ? 1 : 0)) : std::string()) << LL_ENDL;
        }
        else
        {
            Clock::time_point start = Clock::now();
            presented = mirror.endFrame(nullptr, swapchain);
            sEndFrameMs += ms_since(start);
        }

        sVulkanPresented += presented ? 1 : 0;
        LLRHIReplayCapture::onFramePresented();   // research/271 3.2-4: a frame boundary record
        lap.restart();      // endFrame timed its own steps
        LLVKGL::resetFrameStats();
        U32 width, height;
        window_size(width, height);
        if (!width || !height)
        {
            // Minimized (a client area of size 0): the next frame keeps the
            // size this one had (fix-pipeleak). beginFrame would start none,
            // and without an open frame every defer waits for the GPU.
            width = mirror.getWidth();
            height = mirror.getHeight();
        }
        lap(sCost.frame_stats_ms);
        Clock::time_point start = Clock::now();
        mirror.beginFrame(width, height);    // waits for the GPU to finish the frame before last
        lap.restart();      // beginFrame timed its own steps
        if (mirror.isRecording())
        {
            LLVKBindless::beginFrame();     // nothing of the new frame is recorded yet
            lap(sCost.bindless_ms);
            // research/285 X2: while VRAM is short, before the BC7 swaps (off: flag tests).
            const bool textures_scanned = LLVKGL::compactTextures();
            lap(sCost.compact_ms);
            LLVKGL::compressTextures();     // research/88: into the new frame's upload commands
            lap(sCost.compress_ms);
            if (!textures_scanned)
            {
                // research/245: while VRAM is short (off: a flag test). After a
                // texture check that looked over the textures, in the next
                // frame: not two such scans in one frame.
                LLVKGL::compactBuffers();
            }
            lap(sCost.compact_ms);
        }
        sBeginFrameMs += ms_since(start);

        ++sPeriodFrames;
        F64 period = ms_since(sPeriodStart);
        // fix-logcache: without LLRHI::sPeriodicLogs (a plain start) nothing
        // goes to the log, and the reports run once a minute only to start
        // their counters over (they also fold counts other code reads,
        // LLVKRecorder::takeStreamCounts), so the backend's lock is taken for
        // them a twelfth as often. Frame cost and GPU timing are development
        // switches: every 5 s while one is on, as before.
        const bool log = LLRHI::sPeriodicLogs;
        // research/204: verify and auto are development switches too (their line is written either way).
        const bool pass_state_log = LLRHI::passStateMode() == LLRHI::PassStateMode::Verify ||
                                    LLRHI::passStateMode() == LLRHI::PassStateMode::Auto ||
                                    LLRHI::passUnitsMode() == LLRHI::PassUnitsMode::Verify ||   // research/204 10 (vk-s4c2)
                                    LLRHI::passUnitsMode() == LLRHI::PassUnitsMode::Auto;
        const F64 report_period = (log || cost || LLRHI::sGPUTiming || pass_state_log) ? 5000.0 : 60000.0;
        if (period >= report_period)
        {
            const Clock::time_point report_start = Clock::now();
            // Each report is taken whether or not it is written: taking it starts its counters over.
            // research/155 T0b: the counters first (perfReport starts the pass miss reasons over).
            const std::string counters = LLVKGL::counterReport(sPeriodFrames);
            // research/218 vk-s7f: before perfReport (it starts the publication's hits and the refusals over).
            const std::string old_paths = LLVKGL::oldPathReport(sPeriodFrames);
            const std::string perf = LLVKGL::perfReport(sPeriodFrames);
            // research/137 N8, research/179 A: empty while LLRHI_OCCLUSION_BATCH is off and nothing was counted.
            const std::string occlusion_batch = LLVKOcclusion::report(sPeriodFrames);
            // research/137 N12: empty while LLRHI_NATIVE_PASSES is 0 and nothing was counted.
            const std::string passes = LLVKPass::report(sPeriodFrames);
            // research/204: empty while LLRHI_PASS_STATE is off or on and nothing was counted.
            const std::string pass_states = LLRHI::passStateReport(sPeriodFrames);
            // GPU pass timing (LLRHI_GPU_TIMING=1 or gputiming.toggle): empty while off.
            const std::string gpu_times = mirror.takeGPUTimes();
            if (log)
            {
                LL_INFOS("VKPerf") << sPeriodFrames * 1000.0 / period << " fps, " << period / sPeriodFrames << " ms/frame; submit+present "
                                   << sEndFrameMs / sPeriodFrames << " ms, GPU wait " << sBeginFrameMs / sPeriodFrames << " ms; "
                                   << perf << "; descriptor pools " << mirror.getDescriptorPoolCount() << counters << LL_ENDL;
                if (!old_paths.empty())
                {
                    LL_INFOS("VKPerf") << old_paths << LL_ENDL;
                }
                if (!occlusion_batch.empty())
                {
                    LL_INFOS("VKPerf") << "occlusion batch: " << occlusion_batch << LL_ENDL;
                }
                if (!passes.empty())
                {
                    LL_INFOS("VKPerf") << "full-screen passes: " << passes << LL_ENDL;
                }
            }
            if ((log || pass_state_log) && !pass_states.empty())
            {
                LL_INFOS("VKPerf") << "pass state: " << pass_states << LL_ENDL;
            }
            // GPU timing and frame cost are asked for (env or toggle): written either way.
            if (!gpu_times.empty())
            {
                LL_INFOS("VKPerf") << gpu_times << gpu_sensors() << LL_ENDL;
            }
            if (cost)
            {
                // This frame's present up to the reports counts in this period
                // (its steps are in takeFrameCost), the reports in the next.
                sCost.present_ms += ms_between(present_from, report_start);
                LL_INFOS("FrameCost") << cost_report(period, pacing_state, sSwapchain.takeGPUBoundSkips()) << LL_ENDL;
                sCost.segments.clear();     // none is open: "after present" opens below
                sCost.frame_ms.clear();
                sCost.frames = 0;
                sCost.lock_wait = sCost.present_ms = sCost.pacing_ms = 0.0;
                sCost.cue_ms = sCost.frame_stats_ms = sCost.bindless_ms = sCost.compress_ms = sCost.compact_ms = 0.0;
                present_from = report_start;
                sCost.report_ms = ms_since(report_start);
            }
            sPeriodStart = Clock::now();
            sPeriodFrames = 0;
            sEndFrameMs = sBeginFrameMs = 0.0;
        }
        if (cost)
        {
            // From here to the next mark: what runs after the present.
            const Clock::time_point left = Clock::now();
            sCost.present_ms += ms_between(present_from, left);
            cost_open(nullptr, "after present", left);
        }
        // true: skip the GL swap. While Vulkan presents to the window, a single
        // GL swap (a frame Vulkan could not present, e.g. mid-resize) hands the
        // window to GL's empty surface and later Vulkan presents stay hidden.
        return presented || swapchain != nullptr;
    }

    // research/94: with VSync on, wait until the frame before the one just
    // presented is on screen before the next frame starts. Outside the tracker
    // lock, so texture upload threads go on recording meanwhile; the swapchain
    // itself is only used on this thread (and, without VSync, by the present
    // thread between endFrame and the next waitMainPresent: research/179 6.6 4).
    void wait_for_display()
    {
        // research/179 6.6 4: a present still on the present thread is
        // without VSync, where there is no pacing (and the swapchain is its).
        if (!sSwapchain.hasSurface() || LLVKMirror::instance().mainPresentQueued())
        {
            return;
        }
        const Clock::time_point start = Clock::now();
        sSwapchain.waitForDisplay(LLRHI::sFramePacingGPUBound && LLVKMirror::instance().gpuWasBehind());
        if (LLRHI::sFrameCost)
        {
            // Its own item on the FrameCost line, not part of "after present".
            const Clock::time_point end = Clock::now();
            std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
            if (sCost.on)
            {
                cost_close(start);
                sCost.pacing_ms += ms_between(start, end);
                cost_open(nullptr, "after present", end);
            }
        }
    }

    bool present()
    {
        const bool skip_gl_swap = present_frame();
        wait_for_display();
        // Outside the tracker lock: the pipeline cache synchronises itself.
        LLVKDevice::instance().savePipelineCacheIfDue();
        LLVKGL::prewarmPipelines();     // research/111 B-PL
        return skip_gl_swap;
    }

    void exit_if_lost();

    bool on_present()
    {
        const bool skip_gl_swap = present();
        exit_if_lost();
        return skip_gl_swap;
    }

    // fix-pipeleak: a main loop that draws nothing (the window minimized or
    // hidden: display() returns before its swap) still ends its Vulkan frame,
    // without showing it, and starts the next one at the same size. Left open,
    // the frame ran on for as long as the window stayed minimized: nothing
    // deferred to it was freed (deleted textures kept their memory while the
    // texture budget counted it free), and the uploads, command buffers and
    // bindless queue only grew. The empty frame records nothing: one
    // submission, and a fence wait on the frame before last (already done).
    void close_frame_unshown()
    {
        std::lock_guard<LLVKTrackerMutex> frame_lock(LLVKTracker::lock());
        LLVKMirror& mirror = LLVKMirror::instance();
        if (!mirror.isRecording())
        {
            return;     // none open: not started yet, or its target could not be made
        }
        mirror.endFrame(nullptr, nullptr);  // no swapchain: nothing is acquired or presented
        LLVKGL::resetFrameStats();
        mirror.beginFrame(mirror.getWidth(), mirror.getHeight());
        if (mirror.isRecording())
        {
            LLVKBindless::beginFrame();
            LLVKGL::compressTextures();     // the textures uploaded meanwhile (research/88)
        }
    }

    void on_frame_unshown()
    {
        if (!LLVKMirror::instance().isReady())
        {
            return;
        }
        close_frame_unshown();
        exit_if_lost();
    }

    // research/82, 85: no OpenGL renderer to go back to. Tells the user why
    // and ends the viewer (LLRHI::sVulkanUnavailableExit does not return).
    [[noreturn]] void vulkan_unavailable(LLRHI::VulkanUnavailable why, std::string reasons)
    {
        const bool stopped = why == LLRHI::VulkanUnavailable::DeviceLost || why == LLRHI::VulkanUnavailable::SurfaceLost ||
                             why == LLRHI::VulkanUnavailable::SubmitFailed;
        if (reasons.empty())
        {
            reasons = "Vulkan renderer could not be set up (see the log)";
        }
        if (stopped)
        {
            LL_WARNS("VKMirror") << "Vulkan stopped, the viewer ends:\n" << reasons
                                 << (why == LLRHI::VulkanUnavailable::DeviceLost
                                     ? "\nThis usually follows a graphics driver reset or update (Windows TDR), a GPU fault, "
                                       "overheating or running out of GPU memory."
                                     : why == LLRHI::VulkanUnavailable::SubmitFailed
                                     ? "\nThis usually follows running out of GPU or system memory."
                                     : "\nThis usually follows a graphics driver reset or update, or displays being reconnected.")
                                 << " Start the viewer again; if it keeps happening, update the graphics driver or restart the computer."
                                 << LL_ENDL;
        }
        else
        {
            LL_WARNS("VKMirror") << "Vulkan unavailable, the viewer cannot start:\n" << reasons << LL_ENDL;
        }
        if (LLRHI::sVulkanUnavailableExit)
        {
            LLRHI::sVulkanUnavailableExit(why, reasons);
        }
        // Only without the viewer's own message (plus/vkcheck).
        const std::string intro = stopped ? "The viewer has to close: Vulkan stopped.\n\n"
            : why == LLRHI::VulkanUnavailable::Unsupported
            ? "This viewer needs Vulkan 1.3, which this computer cannot run.\n\n"
            : why == LLRHI::VulkanUnavailable::MissingFeatures
            ? "This computer's GPU, with its current graphics driver, lacks Vulkan features the viewer requires. "
              "A driver that supports them is needed; if this still appears after updating the driver from the GPU "
              "maker's website, the GPU does not support this viewer.\n\n"
            : "Vulkan could not be set up on this computer.\n\n";
        const std::wstring text = ll_convert_string_to_wide(intro + reasons);
        MessageBoxW(nullptr, text.c_str(), L"Vulkan", MB_OK | MB_ICONERROR);
        _exit(1);
    }

    // fix-submitfail C: once the device or the window's surface is lost,
    // nothing renders or shows again; the viewer would run on behind a window
    // that looks frozen. The device and swapchain are not recreated (a large
    // job): the user is told why and the viewer ends, as when Vulkan cannot
    // start. On the main thread, after the present.
    // fix-surfacelost: a lost surface counts only once new ones kept failing
    // for 5 s (LLVKSwapchain::surfaceLost); a short loss passes. A lost
    // device ends at once: a VkDevice once lost stays lost (the Vulkan spec),
    // also after Windows has reset the GPU and the driver runs again, and
    // its fences may still read as signaled, so waiting would not bring it
    // back; only a new device would, which is not made here.
    void exit_if_lost()
    {
        const bool device_lost = LLVKDevice::instance().isLost();
        const VkResult stuck = LLVKMirror::instance().stuckSubmitResult();
        if (!device_lost && stuck == VK_SUCCESS && !sSwapchain.surfaceLost())
        {
            return;
        }
        // The GPU fault report (LLVKDevice::reportDeviceLost) came with the
        // fence wait or submission that saw the loss.
        if (device_lost)
        {
            vulkan_unavailable(LLRHI::VulkanUnavailable::DeviceLost, "The GPU stopped responding (VK_ERROR_DEVICE_LOST)");
        }
        if (stuck != VK_SUCCESS)
        {
            vulkan_unavailable(LLRHI::VulkanUnavailable::SubmitFailed,
                               "The GPU did not take a submission, and what it carried is lost (VkResult " + std::to_string((S32)stuck) +
                                   (stuck == VK_ERROR_OUT_OF_DEVICE_MEMORY ? ", out of GPU memory"
                                    : stuck == VK_ERROR_OUT_OF_HOST_MEMORY ? ", out of memory" : "") + ")");
        }
        vulkan_unavailable(LLRHI::VulkanUnavailable::SurfaceLost,
                           "The window can no longer show Vulkan frames (VK_ERROR_SURFACE_LOST_KHR for 5 s, new surfaces did not help)");
    }

    // After LLVKDevice::init failed: a GPU with Vulkan 1.3 that lacks required
    // features (research/124 V1) has its own message; anything else (no
    // driver, an older Vulkan) is Unsupported.
    [[noreturn]] void device_unavailable()
    {
        const LLVKDevice& device = LLVKDevice::instance();
        vulkan_unavailable(device.lacksRequiredFeatures() ? LLRHI::VulkanUnavailable::MissingFeatures
                                                          : LLRHI::VulkanUnavailable::Unsupported,
                           device.getUnsupportedReport());
    }

    // UTF-8 (the folders below are given to LLFile), not the ANSI code page std::getenv gives.
    std::string env_string(const char* name)
    {
        return LLStringUtil::getenv(name);
    }

    // research/117 G5: the window is made and has no GL context. The Vulkan
    // device comes first: LLGLManager::initGL asks the GL strings and limits
    // (the translation's table answers them from the device) before
    // onGLFunctionsLoaded installs the renderer (install then finds the
    // device ready). Does not return when Vulkan cannot run here.
    bool attach_window(HWND window)
    {
        sWindow = window;
        const char* validation = std::getenv("LLRHI_VK_VALIDATION");
        if (!LLVKDevice::instance().init(validation && *validation && strcmp(validation, "0")))
        {
            device_unavailable();
        }
        LL_INFOS("VKMirror") << "No GL context (research/117 G5): the window goes to Vulkan on " << LLVKDevice::instance().getDeviceName() << LL_ENDL;
        return true;
    }

    PROC get_proc_address(const char* name)
    {
        return (PROC)LLVKGL::getProcAddress(name);
    }
}

// research/207 T3 (llrhi.h).
bool LLRHI::deviceInfo(DeviceInfo& out)
{
    return LLVKGL::deviceInfo(out);
}

namespace
{

    // Before the window exists (the viewer makes it after the static initializers).
    const bool sNoGLContextHooks = []()
    {
        gLLRHIGetProcAddress = &get_proc_address;
        gLLRHIAttachWindow = &attach_window;
        return true;
    }();
}


void LLRHIVKMirror::install(bool validation)
{
    if (!LLVKDevice::instance().init(validation))
    {
        device_unavailable();
    }
    if (!LLVKMirror::instance().init())
    {
        vulkan_unavailable(LLRHI::VulkanUnavailable::SetupFailed, "The Vulkan frame could not be set up (see the log)");
    }

    sDir = env_string("LLRHI_VK_CAPTURE_DIR");
    if (sDir.empty())
    {
        sDir = env_string("LLRHI_CUE_DIR");
    }
    U64 capture_every = 0;
    LLRHI::envNumber("LLRHI_VK_CAPTURE_EVERY", 0, U32_MAX, capture_every);
    sCaptureEvery = (U32)capture_every;
    const std::string frames = env_string("LLRHI_VK_CAPTURE");
    std::stringstream list(frames);
    std::string item;
    while (std::getline(list, item, ','))
    {
        const S32 frame = atoi(item.c_str());
        if (frame > 0)
        {
            sCaptureFrames.insert((U32)frame);
        }
    }

    LLVKGL::install();
    LLVKWindows::install();     // research/87: detached UI windows, presented with this frame
    // research/117 G6: no GL to time; the GPU's clocks go into its log lines.
    LLRHI::sGPUBenchmark = []() { return LLVKBenchmark::memoryBandwidth(&gpu_sensors); };
    // GPU pass timing: marks go into the frame like the translated GL calls
    // (under the tracker lock, so they stay in order with them).
    // Frame cost (research/83): the same marks cut the main thread's frame.
    LLRHI::sGPUMark = [](const char* scope, const char* name)
    {
        // research/208 S0: the census first, outside the lock (its take is not the frame's).
        if (LLRHI::renderThreadCensusOn())
        {
            LLRHI::renderThreadCensusMark(name);
            if (!LLRHI::sGPUTiming && !LLRHI::sFrameCost)
            {
                return;
            }
        }
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        if (LLRHI::sGPUTiming)
        {
            LLVKMirror::instance().gpuMark(scope, name);
        }
        cost_mark(scope, name);
    };
    LLRHI::sCPUMark = [](const char* name)
    {
        if (LLRHI::renderThreadCensusOn())     // research/208 S0, as above
        {
            LLRHI::renderThreadCensusMark(name);
            if (!LLRHI::sFrameCost)
            {
                return;
            }
        }
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        cost_mark(nullptr, name);
    };
    if (LLRHI::sFrameCost)
    {
        // =1: with GPU timestamps (one per mark), without the alpha pool's
        // split by kind unless LLRHI_GPU_TIMING_ALPHA asks for it (a timestamp
        // at every switch between kinds). =2: CPU only.
        const char* cost = std::getenv("LLRHI_FRAME_COST");
        if (cost && strcmp(cost, "2"))
        {
            LLRHI::sGPUTiming = true;
            if (!std::getenv("LLRHI_GPU_TIMING_ALPHA"))
            {
                LLRHI::sGPUTimingAlpha = false;
            }
        }
    }
    // Alpha census (research/54): the counts read back, taken by LLDrawPoolAlpha's report.
    LLRHI::sCensusTake = [](std::vector<LLRHI::CensusCount>& counts, U32& frames, U32& dropped)
    {
        std::lock_guard<LLVKTrackerMutex> lock(LLVKTracker::lock());
        return LLVKMirror::instance().takeCensus(counts, frames, dropped);
    };

    // The interval here is the driver's default: the viewer's own comes after
    // (LLWindowWin32::toggleVSync), and present_frame follows it, recreating
    // the swapchain on the first frame if it differs.
    if (wglGetSwapIntervalEXT)
    {
        sSwapchain.setSwapInterval(wglGetSwapIntervalEXT());
    }
    // research/179 6.6 4: the swapchain's image count (0 or unset: as before).
    U64 images = 0;
    if (LLRHI::envNumber("LLRHI_PRESENT_IMAGES", 0, ~(U64)0, images))
    {
        sSwapchain.setImageCount((U32)llmin(images, (U64)8));
    }
    sSwapchain.setNoTearing(LLRHI::sPresentMailbox);
    if (!sSwapchain.init(frame_window()))
    {
        // research/90 G1: the GPU has what the renderer needs, but the window
        // cannot show its frames (e.g. the driver refuses a swapchain on a
        // window GL already set up): not a GPU to replace.
        vulkan_unavailable(LLRHI::VulkanUnavailable::SetupFailed, "The window cannot show Vulkan frames (" + sSwapchain.failure() + ")");
    }
    gLLRHIPresentHook = on_present;
    LLVKDrawList::install();    // research/137 N1: draw lists
    LLVKImmediate::install();   // research/137 N9: the immediate mode ring
    LLVKOcclusion::install();   // research/137 N8: occlusion boxes in one batch
    LLVKPass::install();        // research/137 N12: full-screen passes as units
    LLVKPassState::install();   // research/204: the world passes' declared states
    LLVKResource::install();    // research/137 N14: resources by handle (LLRHI_NATIVE_RESOURCES)
    // fix-threads4: the draw list guard wraps its hooks now, once every hook
    // is set and before other threads call them.
    LLRHI::DrawList::installGuard();
    // research/271 3.1: capture wraps go in last, the same way (before other threads call the
    // hooks), composing as the outermost layer around the draw list guard's own wraps.
    LLRHIReplayCapture::install();

    const std::string captures = sDir.empty() ? std::string(", no captures (no LLRHI_VK_CAPTURE_DIR or LLRHI_CUE_DIR)") :
        ", captures (frames '" + frames + "', every " + std::to_string(sCaptureEvery) + ", capture.now) to " + sDir;
    LL_INFOS("VKMirror") << "Vulkan renderer on " << LLVKDevice::instance().getDeviceName() << captures << LL_ENDL;
}


void LLRHIVKMirror::onGLShutdown()
{
    // research/276 stage 1: with an exit measured (LLRHI::sExitTimeLog, set by
    // plus/fastexit for PlusExitTimeLog), each step writes one line in the form
    // of the viewer's other ExitTime lines. Off: nothing is read or written.
    const bool exit_time_log = LLRHI::sExitTimeLog;
    Clock::time_point step_start = exit_time_log ? Clock::now() : Clock::time_point();
    const auto step_done = [exit_time_log, &step_start](const char* step)
    {
        if (!exit_time_log)
        {
            return;
        }
        const Clock::time_point now = Clock::now();
        const Clock::time_point quit_start{ std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(LLRHI::sExitTimeStartNs)) };
        LL_INFOS("Plus") << "ExitTime [onGLShutdown / " << step << "] +" << std::fixed << std::setprecision(1)
                         << ms_between(quit_start, now) << " ms (step " << ms_between(step_start, now) << " ms)" << LL_ENDL;
        step_start = now;
    };

    // research/271: a capture that had not reached LLRHI_REPLAY_CAPTURE_FRAMES yet (the viewer
    // closed first) is flushed and closed now, so it is not lost.
    LLRHIReplayCapture::shutdown();
    step_done("replay capture closed");
    // Join the recording worker while the viewer shuts down in order: left
    // running, its std::thread is still joinable when the statics are destroyed
    // at exit, which calls std::terminate (the crash on every close, research/104).
    // fix-threads4: the prewarm threads end before exit() destroys the
    // tables their jobs read (programs, pipelines, the compile cache's).
    LLVKGL::stopPrewarm();
    step_done("pipeline prewarm stopped, its keys saved");
    LLVKRecorder::shutdown();
    step_done("recorder and render thread joined");
    // fix-threads4: the other windows' present thread ends too (detached;
    // the mirror's mutex and queue are statics destroyed at exit).
    LLVKMirror::instance().stopPresentThread();
    step_done("present thread stopped");
    if (LLVKDevice::instance().isReady())
    {
        LLVKDevice::instance().savePipelineCache();
        step_done("pipeline cache saved");
    }
    // The device, the main window's swapchain and the tables are not destroyed
    // at exit (llvkdevice.cpp, LLVKDevice::shutdown): the process exit frees them.
}

void LLRHIVKMirror::onFrameNotShown()
{
    on_frame_unshown();
}

bool LLRHIVKMirror::frameRecording()
{
    return LLVKMirror::instance().isRecording();
}

void LLRHIVKMirror::freeDeferred()
{
    if (!LLVKMirror::instance().isReady())
    {
        return;
    }
    // One unshown frame per slot: the last one waits for the fence of the
    // frame that was open here, and frees what it deferred. With no frame
    // open (none started yet) each does nothing: a defer then waits for the
    // GPU and frees at once anyway.
    const std::string before = LLVKDevice::instance().memoryReport();
    for (U32 i = 0; i < LLVKMirror::FRAMES_IN_FLIGHT; ++i)
    {
        on_frame_unshown();
    }
    LL_INFOS("VKMirror") << "Shader reload: the frames in flight finished and what they deferred was freed: " << before << " -> "
                         << LLVKDevice::instance().memoryReport() << LL_ENDL;
}

// research/180 9 3 (LLRHI::TrackerHold). Main thread.
namespace
{
    struct HoldCounts
    {
        U32 frame = 0;              // LLVKTracker::frame() at the last loop
        U64 frames = 0;
        U64 loops = 0;              // loops that asked (held or not)
        U64 holds = 0;
        F64 held_ms = 0.0;
        F64 longest_ms = 0.0;
        U32 depth = 0;              // open holds (a loop inside a held one only counts)
        Clock::time_point since;    // the outer hold's start
        Clock::time_point logged = Clock::now();
    };
    HoldCounts sHold;

    void hold_new_frame()
    {
        HoldCounts& h = sHold;
        ++h.frames;
        if (!LLRHI::sPeriodicLogs || ms_since(h.logged) < 10000.0)
        {
            return;
        }
        const F64 f = (F64)llmax(h.frames, (U64)1);
        const U32 waits = LLVKTracker::lock().takeWaits();
        LL_INFOS("TrackerHold") << "tracker hold [" << (LLRHI::sTrackerHold ? "on" : "off") << "] over " << h.frames
                                << " frames, per frame: loops " << llformat("%.0f", h.loops / f) << ", held " << llformat("%.0f", h.holds / f)
                                << ", held ms " << llformat("%.2f", h.held_ms / f) << " (longest " << llformat("%.2f", h.longest_ms)
                                << "), lock takes that waited " << llformat("%.1f", waits / f) << LL_ENDL;
        HoldCounts next;
        next.frame = h.frame;
        next.depth = h.depth;
        next.since = h.since;
        next.logged = Clock::now();
        h = next;
    }
}

bool LLRHIVKMirror::holdTracker(bool hold)
{
    HoldCounts& h = sHold;
    const U32 frame = LLVKTracker::frame();
    if (frame != h.frame)
    {
        h.frame = frame;
        hold_new_frame();
    }
    ++h.loops;
    if (!hold || !on_main_thread())
    {
        return false;
    }
    LLVKTracker::lock().lock();
    if (h.depth++ == 0)
    {
        ++h.holds;
        h.since = Clock::now();
    }
    return true;
}

void LLRHIVKMirror::releaseTracker()
{
    HoldCounts& h = sHold;
    if (h.depth && --h.depth == 0)
    {
        const F64 ms = ms_since(h.since);
        h.held_ms += ms;
        h.longest_ms = llmax(h.longest_ms, ms);
    }
    LLVKTracker::lock().unlock();
}


#else // !(LL_VULKAN && LL_WINDOWS)

void LLRHIVKMirror::install(bool)
{
    LL_WARNS("VKMirror") << "The Vulkan renderer needs a Windows build with the Vulkan SDK" << LL_ENDL;
}

void LLRHIVKMirror::onGLShutdown()
{
}

void LLRHIVKMirror::onFrameNotShown()
{
}

bool LLRHIVKMirror::frameRecording()
{
    return true;
}

void LLRHIVKMirror::freeDeferred()
{
}

bool LLRHIVKMirror::holdTracker(bool)
{
    return false;
}

void LLRHIVKMirror::releaseTracker()
{
}

#endif
