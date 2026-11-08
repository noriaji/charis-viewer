/**
 * @file llrhiframe.cpp
 * @brief The frame as a render thread would take it (research/208, stage 4a): the census (S0) and the pass head's switch (S1).
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

#include "llrhiframe.h"
#include "llrhi.h"          // sRenderThreadCensus
#include "llrhiscene.h"     // the scene's state in the census line

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <thread>

namespace LLRHI
{
    std::atomic<bool> sRenderThreadCensusOn{ false };

namespace
{
    // The main thread's frame between two marks (research/208 1.2): before
    // the hand-off, the world after it (what a render thread would take, or
    // the main thread would record before handing off: the batches), and
    // what stays the main thread's after it (the HUD and the UI).
    enum Segment : U32
    {
        SEG_AFTER_PRESENT,      // the present to the next mark (the probes' faces, the snapshot floater)
        SEG_LOOP,               // "loop", "background yield": texture threads, mesh, input
        SEG_IDLE,               // "idle" and its detail marks
        SEG_DISPLAY,            // display() up to the hand-off but images and state sort (cull, shadows, geometry, ...)
        SEG_IMAGES,             // "pre-gbuffer images" (research/208 K1: could move after the hand-off)
        SEG_STATE_SORT,         // "pre-gbuffer state sort"
        SEG_BATCHES,            // after the hand-off: the G-buffer, lights, water, alpha (the pools' batches)
        SEG_FULLSCREEN,         // after the hand-off: the full-screen passes (lighting, post)
        SEG_HUD,                // "ui": the HUD's elements and attachments
        SEG_UI_3D,
        SEG_UI_HUD_OBJECTS,
        SEG_UI_2D,
        SEG_UI_DEBUG,           // the detached windows, the debug text, on to the present
        SEG_COUNT
    };
    const char* const SEG_NAMES[SEG_COUNT] = { "after present", "loop", "idle", "display", "images", "state sort", "batches",
                                               "full-screen", "hud", "ui 3d", "hud objects", "ui 2d", "debug text" };
    const char* const PART_NAMES[CENSUS_PARTS] = { "begin frame", "views", "pass entries", "choose", "binder", "expand", "record" };

    enum Phase : U8
    {
        BEFORE_HANDOFF,
        WORLD,
        UI,
    };

    constexpr U32 PART_DEPTH = 8;
    constexpr F64 REPORT_MS = 5000.0;
    constexpr F64 HANDOFF_MS = 0.002;   // research/208 3.3: what a hand-off and its join cost (taken off the overlap)
    constexpr F64 VERDICT_MS = 0.3;     // research/208 6.2 S0: alpha (owned tables) worth a render thread

    struct Totals
    {
        U64 ns = 0;
        U64 lock_ns = 0;        // of it, the main thread held the backend lock
        U64 takes = 0;          // the main thread's takes of the lock (depth 0)
    };

    // Main thread only (every entry tests on_main), but `main`.
    struct Census
    {
        Phase phase = BEFORE_HANDOFF;
        Segment seg = SEG_AFTER_PRESENT;
        U64 seg_start = 0, seg_lock = 0, seg_takes = 0;
        bool held = false;              // the main thread holds the lock (taken while counting)
        U64 held_since = 0;
        U64 lock_ns = 0, takes = 0;     // running
        U64 waits = 0, wait_ns = 0, waits_after = 0, wait_ns_after = 0;
        Totals segs[SEG_COUNT];
        U32 part_stack[PART_DEPTH] = {};
        U64 part_since[PART_DEPTH] = {};
        U32 depth = 0;
        U64 part_ns[CENSUS_PARTS] = {};
        U64 part_ns_after[CENSUS_PARTS] = {};   // after the hand-off
        U64 part_calls[CENSUS_PARTS] = {};
        U64 rescues = 0, rescued = 0;
        U32 handoffs = 0, frames = 0, odd = 0;
        U64 period_start = 0;
    };
    Census sCensus;
    std::atomic<std::thread::id> sMain{ std::thread::id() };

    U64 now_ns()
    {
        return (U64)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool on_main()
    {
        return std::this_thread::get_id() == sMain.load(std::memory_order_relaxed);
    }

    U64 lock_total(U64 now)
    {
        const Census& c = sCensus;
        return c.lock_ns + (c.held ? now - c.held_since : 0);
    }

    void close_segment(U64 now)
    {
        Census& c = sCensus;
        Totals& t = c.segs[c.seg];
        const U64 lock = lock_total(now);
        t.ns += now - c.seg_start;
        t.lock_ns += lock - c.seg_lock;
        t.takes += c.takes - c.seg_takes;
        c.seg_start = now;
        c.seg_lock = lock;
        c.seg_takes = c.takes;
    }

    bool is(const char* a, const char* b)
    {
        return !strcmp(a, b);
    }

    // The full-screen passes after the hand-off (pipeline.cpp's marks): a
    // render thread's own work also while the batches stay the main thread's.
    bool fullscreen_mark(const char* name)
    {
        return is(name, "lighting") || is(name, "sun light") || is(name, "light maps") || is(name, "fullscreen lights") ||
               is(name, "tonemap") || is(name, "bloom") || is(name, "dof") || is(name, "aa") || is(name, "post");
    }

    // The marks that only come before the hand-off (the main loop's).
    bool loop_mark(const char* name)
    {
        return is(name, "idle") || is(name, "display") || is(name, "loop") || is(name, "background yield") || is(name, "snapshot");
    }

    Segment classify(const char* name)
    {
        Census& c = sCensus;
        if (is(name, "pre-gbuffer sky"))
        {
            c.phase = WORLD;    // the hand-off point: the main view's stateSort and rebuildPools are done
            ++c.handoffs;
            return SEG_BATCHES;
        }
        if (c.phase != BEFORE_HANDOFF && loop_mark(name))
        {
            c.phase = BEFORE_HANDOFF;   // a frame without its present (a snapshot's display): counted, not split
            ++c.odd;
        }
        if (c.phase == UI)
        {
            return is(name, "ui 3d") ? SEG_UI_3D : is(name, "ui hud objects") ? SEG_UI_HUD_OBJECTS : is(name, "ui 2d") ? SEG_UI_2D :
                   is(name, "ui debug text") ? SEG_UI_DEBUG : SEG_HUD;
        }
        if (c.phase == WORLD)
        {
            if (is(name, "ui"))
            {
                c.phase = UI;
                return SEG_HUD;
            }
            return fullscreen_mark(name) ? SEG_FULLSCREEN : SEG_BATCHES;
        }
        if (is(name, "idle") || !strncmp(name, "idle ", 5))
        {
            return SEG_IDLE;
        }
        if (is(name, "loop") || is(name, "background yield"))
        {
            return SEG_LOOP;
        }
        return is(name, "pre-gbuffer images") ? SEG_IMAGES : is(name, "pre-gbuffer state sort") ? SEG_STATE_SORT : SEG_DISPLAY;
    }

    void add_part(U32 part, U64 ns)
    {
        Census& c = sCensus;
        c.part_ns[part] += ns;
        c.part_ns_after[part] += c.phase != BEFORE_HANDOFF ? ns : 0;
    }

    void start(U64 now)
    {
        Census& c = sCensus;
        c = Census();
        c.period_start = now;
        c.seg_start = now;
    }

    F64 per(U64 ns, U32 frames)
    {
        return frames ? ns * 1e-6 / frames : 0.0;
    }

    std::string report()
    {
        const Census& c = sCensus;
        const U32 f = c.frames;
        F64 seg[SEG_COUNT], lock[SEG_COUNT];
        F64 takes_after = 0.0;
        for (U32 i = 0; i < SEG_COUNT; ++i)
        {
            seg[i] = per(c.segs[i].ns, f);
            lock[i] = per(c.segs[i].lock_ns, f);
            takes_after += i >= SEG_BATCHES ? (F64)c.segs[i].takes / llmax(f, 1u) : 0.0;
        }
        F64 parts[CENSUS_PARTS], parts_after[CENSUS_PARTS];
        for (U32 i = 0; i < CENSUS_PARTS; ++i)
        {
            parts[i] = per(c.part_ns[i], f);
            parts_after[i] = per(c.part_ns_after[i], f);
        }
        const F64 world = seg[SEG_BATCHES] + seg[SEG_FULLSCREEN];
        const F64 world_lock = lock[SEG_BATCHES] + lock[SEG_FULLSCREEN];
        F64 ui = 0.0, ui_lock = 0.0;
        for (U32 i = SEG_HUD; i < SEG_COUNT; ++i)
        {
            ui += seg[i];
            ui_lock += lock[i];
        }
        // What a render thread takes after the hand-off (research/208 3.1 alpha1:
        // the batches the usual way are recorded by the main thread before it):
        // the full-screen passes and the retained scene's backend parts.
        const F64 scene_rt = parts_after[CENSUS_SCENE_CHOOSE] + parts_after[CENSUS_SCENE_BINDER] + parts_after[CENSUS_SCENE_EXPAND] +
                             parts_after[CENSUS_SCENE_RECORD];
        const F64 rt_work = seg[SEG_FULLSCREEN] + scene_rt;
        auto overlap = [rt_work](F64 main_side) { return llmax(0.0, llmin(main_side, rt_work) - HANDOFF_MS); };
        const F64 alpha_single = overlap(ui - ui_lock);
        const F64 alpha_owned = overlap(ui);
        const F64 alpha_owned_images = overlap(ui + seg[SEG_IMAGES]);
        const F64 beta = overlap(seg[SEG_IDLE] + seg[SEG_LOOP]);
        const bool scene_drawing = sScene && sScene->drawing();

        std::ostringstream out;
        out << std::fixed << std::setprecision(3);
        out << "rt census (research/208 S0, " << f << " frames, hand-offs " << (F64)c.handoffs / llmax(f, 1u) << " a frame";
        if (c.odd)
        {
            out << ", frames without their present " << c.odd;
        }
        out << "; scene " << sceneModeName(sceneMode()) << (scene_drawing ? " drawing" : " not drawing") << ", frame snapshot "
            << passHeadModeName(passHeadMode()) << "): after the hand-off the main thread " << world + ui << " ms [lock " << world_lock + ui_lock
            << ", takes " << takes_after << ", waits " << (F64)c.waits_after / llmax(f, 1u) << " (" << per(c.wait_ns_after, f) << " ms)]"
            << " = world " << world << " [lock " << world_lock << "] (full-screen " << seg[SEG_FULLSCREEN] << ", batches "
            << seg[SEG_BATCHES] << ") + main-side " << ui << " [lock " << ui_lock << "] (";
        for (U32 i = SEG_HUD; i < SEG_COUNT; ++i)
        {
            out << (i != SEG_HUD ? ", " : "") << SEG_NAMES[i] << " " << seg[i];
        }
        out << "); before it";
        for (U32 i = SEG_AFTER_PRESENT; i < SEG_BATCHES; ++i)
        {
            out << (i ? ", " : " ") << SEG_NAMES[i] << " " << seg[i] << " [lock " << lock[i] << "]";
        }
        out << "; lock takes " << (F64)c.takes / llmax(f, 1u) << " a frame, waits " << (F64)c.waits / llmax(f, 1u) << " ("
            << per(c.wait_ns, f) << " ms)";
        out << "; scene: main (";
        for (U32 i = CENSUS_SCENE_BEGIN_FRAME; i <= CENSUS_SCENE_PASS_ENTRY; ++i)
        {
            out << (i ? ", " : "") << PART_NAMES[i] << " " << parts[i];
        }
        out << ") / render thread (";
        for (U32 i = CENSUS_SCENE_CHOOSE; i < CENSUS_PARTS; ++i)
        {
            out << (i != CENSUS_SCENE_CHOOSE ? ", " : "") << PART_NAMES[i] << " " << parts[i] << " (after the hand-off " << parts_after[i]
                << ", calls " << (F64)c.part_calls[i] / llmax(f, 1u) << ")";
        }
        out << ")";
        out << "; answers after the hand-off: rescues " << (F64)c.rescues / llmax(f, 1u) << " a frame (" << (F64)c.rescued / llmax(f, 1u)
            << " batches)";
        out << "; render thread work " << rt_work << " ms; predicted alpha (single lock) " << alpha_single << " ms, alpha (owned tables) "
            << alpha_owned << " ms (images after the hand-off " << alpha_owned_images << "), beta " << beta << " ms";
        out << "; S0 verdict: alpha (owned tables) " << (alpha_owned >= VERDICT_MS ? ">= 0.3 ms: S1 then S3 (research/208 6.2)"
                                                                                    : "< 0.3 ms: S1 only, the thread waits");
        if (!scene_drawing)
        {
            out << " (judge it with LLRHI_SCENE=on and LLRHI_SCENE_ALPHA=on)";
        }
        return out.str();
    }
} // namespace

    void renderThreadCensusMark(const char* name)
    {
        if (!name || !renderThreadCensusOn() || !on_main())
        {
            return;
        }
        Census& c = sCensus;
        const U64 now = now_ns();
        close_segment(now);
        c.seg = classify(name);
    }

    void renderThreadCensusJoin()
    {
        const bool want = sRenderThreadCensus;
        if (want != renderThreadCensusOn())
        {
            if (want)
            {
                sMain.store(std::this_thread::get_id(), std::memory_order_relaxed);
                start(now_ns());
                LL_INFOS("VKPerf") << "rt census on (research/208 S0): the main thread's frame split at the hand-off, every 5 s" << LL_ENDL;
            }
            sRenderThreadCensusOn.store(want, std::memory_order_relaxed);
            return;
        }
        if (!want || !on_main())
        {
            return;
        }
        Census& c = sCensus;
        const U64 now = now_ns();
        close_segment(now);
        ++c.frames;
        c.phase = BEFORE_HANDOFF;
        c.seg = SEG_AFTER_PRESENT;
        if ((now - c.period_start) * 1e-6 >= REPORT_MS && c.frames)
        {
            LL_INFOS("VKPerf") << report() << LL_ENDL;
            // A new period; an open hold goes on from here (close_segment counted it up to now).
            const bool held = c.held;
            start(now);
            c.held = held;
            c.held_since = now;
        }
    }

    void renderThreadCensusLockTaken()
    {
        if (!on_main())
        {
            return;
        }
        Census& c = sCensus;
        c.held = true;
        c.held_since = now_ns();
        ++c.takes;
    }

    void renderThreadCensusLockLeft()
    {
        Census& c = sCensus;
        if (!on_main() || !c.held)
        {
            return;     // taken before the census began
        }
        c.lock_ns += now_ns() - c.held_since;
        c.held = false;
    }

    U64 renderThreadCensusTicks()
    {
        return now_ns();
    }

    void renderThreadCensusLockWaited(U64 start)
    {
        if (!on_main())
        {
            return;
        }
        Census& c = sCensus;
        const U64 ns = now_ns() - start;
        ++c.waits;
        c.wait_ns += ns;
        if (c.phase != BEFORE_HANDOFF)
        {
            ++c.waits_after;
            c.wait_ns_after += ns;
        }
    }

    bool renderThreadCensusPartBegin(CensusPart part)
    {
        Census& c = sCensus;
        if (part >= CENSUS_PARTS || !on_main() || c.depth >= PART_DEPTH)
        {
            return false;
        }
        const U64 now = now_ns();
        if (c.depth)
        {
            add_part(c.part_stack[c.depth - 1], now - c.part_since[c.depth - 1]);
        }
        c.part_stack[c.depth] = part;
        c.part_since[c.depth] = now;
        ++c.depth;
        ++c.part_calls[part];
        return true;
    }

    void renderThreadCensusPartEnd()
    {
        Census& c = sCensus;
        if (!c.depth)
        {
            return;     // begun before the census started over
        }
        const U64 now = now_ns();
        --c.depth;
        add_part(c.part_stack[c.depth], now - c.part_since[c.depth]);
        if (c.depth)
        {
            c.part_since[c.depth - 1] = now;
        }
    }

    void renderThreadCensusRescue(U32 batches)
    {
        if (!renderThreadCensusOn() || !on_main())
        {
            return;
        }
        Census& c = sCensus;
        if (c.phase != BEFORE_HANDOFF)
        {
            ++c.rescues;
            c.rescued += batches;
        }
    }

    // Read once: the mode asked for does not change while the viewer runs
    // (auto moves between its own states in the binder).
    PassHeadMode passHeadMode()
    {
        static const PassHeadMode mode = []()
        {
            const char* env = std::getenv("LLRHI_FRAME_SNAPSHOT");
            if (!env || !*env || !strcmp(env, "auto"))
            {
                return PassHeadMode::Auto;
            }
            if (!strcmp(env, "verify"))
            {
                return PassHeadMode::Verify;
            }
            if (!strcmp(env, "on") || !strcmp(env, "1"))
            {
                return PassHeadMode::On;
            }
            return PassHeadMode::Off;
        }();
        return mode;
    }

    const char* passHeadModeName(PassHeadMode mode)
    {
        switch (mode)
        {
            case PassHeadMode::Verify:
                return "verify";
            case PassHeadMode::Auto:
                return "auto";
            case PassHeadMode::On:
                return "on";
            default:
                return "off";
        }
    }

    // off / verify / auto / on (also 1) of a switch; anything else is off.
    static RenderThreadMode read_four_way(const char* name)
    {
        const char* env = std::getenv(name);
        if (!env || !*env)
        {
            return RenderThreadMode::Off;
        }
        if (!strcmp(env, "verify"))
        {
            return RenderThreadMode::Verify;
        }
        if (!strcmp(env, "auto"))
        {
            return RenderThreadMode::Auto;
        }
        if (!strcmp(env, "on") || !strcmp(env, "1"))
        {
            return RenderThreadMode::On;
        }
        return RenderThreadMode::Off;
    }

    // research/208 S2: read once, off unless asked for.
    RenderThreadMode renderThreadMode()
    {
        static const RenderThreadMode mode = read_four_way("LLRHI_RENDER_THREAD");
        return mode;
    }

    // research/209 E4 (vk-s4a4).
    RenderThreadMode sceneBatchMode()
    {
        static const RenderThreadMode mode = read_four_way("LLRHI_SCENE_BATCH");
        return mode;
    }

    // research/208 13.3 (vk-s4a4).
    RenderThreadMode passSecondaryMode()
    {
        static const RenderThreadMode mode = read_four_way("LLRHI_PASS_SECONDARY");
        return mode;
    }

    // research/208 13.4 (vk-s4a4).
    bool renderThreadAllViews()
    {
        static const bool all = []()
        {
            const char* env = std::getenv("LLRHI_RENDER_THREAD_VIEWS");
            return env && !strcmp(env, "all");
        }();
        return all;
    }

    // research/208 13.6 (vk-s4a4).
    bool sceneShadowProbe()
    {
        static const bool on = read_four_way("LLRHI_SCENE_SHADOW_PROBE") == RenderThreadMode::On;
        return on;
    }

    const char* renderThreadModeName(RenderThreadMode mode)
    {
        switch (mode)
        {
            case RenderThreadMode::Verify:
                return "verify";
            case RenderThreadMode::Auto:
                return "auto";
            case RenderThreadMode::On:
                return "on";
            default:
                return "off";
        }
    }
}
