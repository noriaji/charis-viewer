/**
 * @file uiframe.cpp
 * @brief "UI frames": while the user works the UI over a slow world, most
 *        frames draw only the UI over the last world image. Registers with
 *        the plus foundation (research/86-ui-thread.md, E1).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

// On by default (PlusUiFrame; LLRHI_UI_FRAME=0 turns it off).
//
// The UI waits for the world: input, UI updates and UI drawing all run once
// per main loop, and a loop takes as long as the world's frame. When that is
// slow (a loop over PlusUiFrameThresholdMs, 50 ms = under 20 fps) and the user
// is working the UI - dragging on a UI view (slider, scroll bar, floater),
// a menu or a combo box list open, the mouse busy over a graphics settings
// floater - most loops draw a UI-only frame instead of the world:
//
//   the last world image (kept by onWorldDrawn(), with the HUD and name tags)
//   + the 2D UI over it, presented.
//
// Input, coroutines and idle() (network, object updates, texture fetches) run
// every loop as always, and the UI-only frame keeps texture creation and the
// UI's preview textures going: only the drawing of the world waits. The world
// is still redrawn now and then, by a time budget: it gets PlusUiFrameWorldShare
// of the time (a world frame of W is followed by UI-only frames for
// W * (1 - share) / share), and, except during a drag, at least once every
// PlusUiFrameMaxPauseMs. Chat and other typing never pause the world.
// With PlusUIFrameNotice (off by default) a small notice in the world view says the
// picture is held.
//
// Statistics (Advanced > Performance Tools > Statistics): FPS counts only the
// frames that draw the world (LLRHI_UI_FRAME_STAT=0 counts every loop, as
// before), and "UI frames (Charis)" shows the UI-only frames per second.
//
// Measuring (PlusUiFrameLog or LLRHI_UI_FRAME_LOG=1): a "UIFrame" log line
// every 5 s with the frames of each kind and, on Windows, how long input
// waited for the screen. LLRHI_SLOW_WORLD_MS=<ms> makes every world frame
// that much longer, to try this in a light place.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "uiframe.h"

#include "llagentcamera.h"
#include "llappviewer.h"
#include "llcombobox.h"
#include "lldrawpoolbump.h"
#include "lldynamictexture.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llgl.h"
#include "llgltfmateriallist.h"
#include "lljoystickbutton.h"
#include "llmenugl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhigl.h"
#include "llscrolllistctrl.h"
#include "llstartup.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llviewerstats.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "pipeline.h"

#include "detach/detach.h"
#include "uishowbtn/uishowbtn.h"
#include "poser/fsvirtualtrackpad.h"

#if LL_WINDOWS
#include "llwin32headers.h"
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

// llviewerdisplay.cpp: the 2D UI (gViewerWindow->draw()) as render_ui() draws
// it, and whether the frame being drawn is a snapshot's.
void render_ui_2d();
extern bool gSnapshot;

namespace
{
    constexpr char SETTING_ENABLED[]      = "PlusUiFrame";
    constexpr char SETTING_THRESHOLD_MS[] = "PlusUiFrameThresholdMs";
    constexpr char SETTING_WORLD_SHARE[]  = "PlusUiFrameWorldShare";
    constexpr char SETTING_MAX_PAUSE_MS[] = "PlusUiFrameMaxPauseMs";
    constexpr char SETTING_LOG[]          = "PlusUiFrameLog";

    constexpr char ENV_ENABLED[]       = "LLRHI_UI_FRAME";       // 0: off, whatever PlusUiFrame says
    constexpr char ENV_LOG[]           = "LLRHI_UI_FRAME_LOG";   // 1: log, whatever PlusUiFrameLog says
    constexpr char ENV_SLOW_WORLD_MS[] = "LLRHI_SLOW_WORLD_MS";  // test: each world frame sleeps this long
    constexpr char ENV_STAT[]          = "LLRHI_UI_FRAME_STAT";  // 0: the FPS stat counts every loop, as before

    // Floaters whose settings change what the world looks like: the mouse busy
    // over one counts as working the UI.
    const char* const GRAPHICS_FLOATERS[] = { "preferences", "prefs_graphics_advanced", "performance" };
    constexpr F64 RECENT_INPUT_SECONDS = 0.5;

    constexpr F32 MIN_WORLD_SHARE = 0.02f;      // below: the world waits for the end of the work (or the pause limit)
    constexpr F32 MAX_WORLD_SHARE = 1.f;

    // Seconds of texture creation per UI-only frame (display() allows 2-5 ms
    // per world frame; UI-only frames come many times as often).
    constexpr F32 UI_FRAME_TEXTURE_TIME = 0.002f;

    // The world image is freed once the world has been fast this long.
    constexpr F64 RELEASE_IMAGE_SECONDS = 10.0;

    constexpr F64 LOG_PERIOD_SECONDS = 5.0;

    // "UI frames (Charis)" in the Statistics floater (floater_stats.xml):
    // UI-only frames over the last second.
    LLTrace::SampleStatHandle<> UI_FRAMES_PER_SECOND("plus_ui_fps", "UI-only frames per second (plus/uiframe)");
    constexpr F64 UI_RATE_WINDOW_SECONDS = 1.0;

    // The FPS displays' world rate (plus::uiframe::worldFps()): world frames
    // over the last second, while one of the last STAT_MEDIAN_LOOPS loops was
    // UI-only (the loops of the displays' median, LLFloaterPerformance's
    // NUM_PERIODS); with none, the FPS stat's median as it is.
    constexpr F64 WORLD_RATE_WINDOW_SECONDS = 1.0;
    constexpr U32 STAT_MEDIAN_LOOPS = 50;

    // What the user is doing with the UI, worst first in uiActivity().
    enum class Activity { None, Drag, Menu, List, Graphics, Count };
    const char* const ACTIVITY_NAMES[] = { "none", "drag", "menu", "list", "graphics" };

    bool enabled()
    {
        static const bool env_off = LLRHI::envIs(ENV_ENABLED, "0");
        static LLCachedControl<bool> setting(gSavedSettings, SETTING_ENABLED, true);
        return !env_off && setting();
    }

    bool logging()
    {
        static const bool env_on = LLRHI::envIs(ENV_LOG, "1");
        static LLCachedControl<bool> setting(gSavedSettings, SETTING_LOG, false);
        return env_on || setting();
    }

    bool fpsCountsWorldFrames()
    {
        static const bool env_off = LLRHI::envIs(ENV_STAT, "0");
        return !env_off;
    }

    U32 slowWorldTestMs()
    {
        static const U32 ms = []()
        {
            const char* env = getenv(ENV_SLOW_WORLD_MS);
            return env ? (U32)llclamp(atoi(env), 0, 2000) : 0u;
        }();
        return ms;
    }

    // ------------------------------------------------------------------
    // The world image: the default framebuffer after the world, HUD and name
    // tags, before the 2D UI.

    class WorldImage
    {
    public:
        bool matches(U32 width, U32 height) const
        {
            return mValid && mTarget && mTarget->getWidth() == width && mTarget->getHeight() == height;
        }

        // Whether the backend can keep the image (Vulkan; not the GL-only
        // benchmark process).
        static bool canCapture()
        {
            return LLRHI::sGLDrawsDisabled && LLRHI::sCopyTexSubImage2D;
        }

        // Copies the default framebuffer (bound, width x height) into the image.
        void capture(U32 width, U32 height)
        {
            if (!mTarget)
            {
                mTarget = new LLRenderTarget();     // never deleted: a static destructor would run without a context
            }
            if (mTarget->getWidth() != width || mTarget->getHeight() != height || !mTarget->isComplete())
            {
                mValid = false;
                if (!mTarget->allocate(width, height, GL_RGBA8))
                {
                    LL_WARNS("Plus") << "UI frames: cannot make a " << width << "x" << height << " world image" << LL_ENDL;
                    mTarget->release();
                    return;
                }
            }

            gGL.flush();    // the name tags may still be in LLRender's batch
            // The Vulkan backend copies its default framebuffer into the texture (one image copy).
            mValid = LLRHI::sCopyTexSubImage2D(mTarget->getTexture(), GL_TEXTURE_2D, 0, 0, 0, 0, 0, (S32)width, (S32)height);
        }

        // Over the whole window, replacing what is there. Needs the 2D setup
        // (setup2DRender(): window pixels) and a matching image.
        void draw() const
        {
            const S32 width = (S32)mTarget->getWidth();
            const S32 height = (S32)mTarget->getHeight();
            LLGLDisable blend(GL_BLEND);
            LLGLDisable cull(GL_CULL_FACE);
            LLGLDepthTest no_depth(GL_FALSE, GL_FALSE);
            gUIProgram.bind();
            gGL.getTexUnit(0)->bind(mTarget);
            gGL.color4f(1.f, 1.f, 1.f, 1.f);
            gGL.begin(LLRender::TRIANGLE_STRIP);
            gGL.texCoord2f(0.f, 0.f); gGL.vertex2i(0, 0);
            gGL.texCoord2f(1.f, 0.f); gGL.vertex2i(width, 0);
            gGL.texCoord2f(0.f, 1.f); gGL.vertex2i(0, height);
            gGL.texCoord2f(1.f, 1.f); gGL.vertex2i(width, height);
            gGL.end();
            gGL.flush();
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            gUIProgram.unbind();
        }

        // The world changed in a way the image does not show (a world frame
        // that was not kept).
        void invalidate() { mValid = false; }

        void release()
        {
            mValid = false;
            if (mTarget && mTarget->isComplete())
            {
                mTarget->release();
            }
        }

    private:
        LLRenderTarget* mTarget = nullptr;
        bool mValid = false;
    };

    // ------------------------------------------------------------------
    // Measuring: how long input waits for the screen, and the frames drawn.

    class Meter
    {
    public:
        // At the decision, after this loop's input was handled.
        void onInputHandled()
        {
#if LL_WINDOWS
            LASTINPUTINFO info = { sizeof(info) };
            if (GetLastInputInfo(&info) && info.dwTime != mLastInputTick)
            {
                mLastInputTick = info.dwTime;
                mInputPending = true;
            }
#endif
        }

        // When this loop's frame reaches the screen. `held`: the world was
        // held for the UI (UI-only frames and the world frames between them).
        void onPresent(bool held, F64 now)
        {
#if LL_WINDOWS
            if (mInputPending)
            {
                Samples& samples = mSamples[held ? 1 : 0];
                // GetTickCount counts in steps of 10-16 ms.
                samples.latency_ms.push_back((F32)(DWORD)(GetTickCount() - mLastInputTick));
                if (mLastPresent > 0.0)
                {
                    samples.interval_ms.push_back((F32)((now - mLastPresent) * 1000.0));
                }
                mInputPending = false;
            }
#endif
            mLastPresent = now;
        }

        void countWorldFrame() { ++mWorldFrames; }
        void countUIFrame(F64 cpu_seconds) { ++mUIFrames; mUISeconds += cpu_seconds; }
        void countActivity(Activity activity) { ++mActivity[(size_t)activity]; }

        void report(F64 now, F64 world_frame_seconds)
        {
            if (mPeriodStart <= 0.0)
            {
                mPeriodStart = now;
                return;
            }
            const F64 period = now - mPeriodStart;
            if (period < LOG_PERIOD_SECONDS)
            {
                return;
            }
            std::string activity;
            for (size_t i = (size_t)Activity::Drag; i < (size_t)Activity::Count; ++i)
            {
                activity += llformat(" %s %u", ACTIVITY_NAMES[i], mActivity[i]);
            }
            LL_INFOS("UIFrame") << llformat("%.1fs: world frames %u (last %.0f ms), UI-only frames %u (%.2f ms CPU each); loops with UI work:%s",
                                            period, mWorldFrames, world_frame_seconds * 1000.0, mUIFrames,
                                            mUIFrames ? mUISeconds * 1000.0 / mUIFrames : 0.0, activity.c_str())
                                << LL_ENDL;
#if LL_WINDOWS
            const char* const kinds[2] = { "world live", "world held" };
            for (size_t i = 0; i < 2; ++i)
            {
                Samples& samples = mSamples[i];
                if (!samples.latency_ms.empty())
                {
                    LL_INFOS("UIFrame") << kinds[i] << ": input to screen ms " << summary(samples.latency_ms)
                                        << "; screen updates with input, ms apart " << summary(samples.interval_ms) << LL_ENDL;
                }
                samples.latency_ms.clear();
                samples.interval_ms.clear();
            }
#endif
            mPeriodStart = now;
            mWorldFrames = mUIFrames = 0;
            mUISeconds = 0.0;
            std::fill(std::begin(mActivity), std::end(mActivity), 0u);
        }

    private:
        struct Samples
        {
            std::vector<F32> latency_ms;
            std::vector<F32> interval_ms;
        };

        static std::string summary(std::vector<F32>& values)
        {
            if (values.empty())
            {
                return "-";
            }
            F64 sum = 0.0;
            for (F32 v : values)
            {
                sum += v;
            }
            const size_t p95 = (values.size() * 95) / 100;
            std::nth_element(values.begin(), values.begin() + p95, values.end());
            const F32 p95_value = values[p95];
            const F32 max_value = *std::max_element(values.begin(), values.end());
            return llformat("n %u avg %.0f p95 %.0f max %.0f", (U32)values.size(), sum / values.size(), p95_value, max_value);
        }

        Samples mSamples[2];    // [0] world live, [1] world held
        F64 mLastPresent = 0.0;
        F64 mPeriodStart = 0.0;
        F64 mUISeconds = 0.0;
        U32 mWorldFrames = 0;
        U32 mUIFrames = 0;
        U32 mActivity[(size_t)Activity::Count] = {};
#if LL_WINDOWS
        DWORD mLastInputTick = 0;
        bool mInputPending = false;
#endif
    };

    // ------------------------------------------------------------------
    // The official statistics: FPS counts the world's frames, and the UI-only
    // frames get a rate of their own.
    //
    // A loop that draws the world counts 1 in FPS, exactly as the official
    // update_statistics() does; a UI-only loop counts nothing. With no UI-only
    // frames the FPS stat is the official one. While they run, the per-loop
    // readings of the statistics (mean or median over loops of count / loop
    // time) include those loops at 0; the FPS displays (plus/fpscorner, the
    // performance floater) read worldFps() instead, the world frames over the
    // time they took.

    class FrameStats
    {
    public:
        // From update_statistics(), in idle(), before the loop decides what it draws.
        bool countFrameLater()
        {
            if (!fpsCountsWorldFrames())
            {
                return false;
            }
            if (mCountPending)
            {
                LLTrace::add(LLStatViewer::FPS, 1);     // the last loop drew nothing (no display() at all)
            }
            mCountPending = true;
            return true;
        }

        // Once per loop, where it decides what it draws.
        void onFrame(F64 now, bool world)
        {
            if (mCountPending && world)
            {
                LLTrace::add(LLStatViewer::FPS, 1);
            }
            mCountPending = false;

            if (world)
            {
                mWorldFrameTimes.push_back(now);
                mLoopsSinceUIFrame = llmin(mLoopsSinceUIFrame + 1, STAT_MEDIAN_LOOPS);
            }
            else
            {
                mUIFrameTimes.push_back(now);
                mLoopsSinceUIFrame = 0;
            }
            while (mWorldFrameTimes.size() > 2 && now - mWorldFrameTimes.front() > WORLD_RATE_WINDOW_SECONDS)
            {
                mWorldFrameTimes.pop_front();
            }
            while (!mUIFrameTimes.empty() && now - mUIFrameTimes.front() > UI_RATE_WINDOW_SECONDS)
            {
                mUIFrameTimes.pop_front();
            }
            LLTrace::sample(UI_FRAMES_PER_SECOND, (F64)mUIFrameTimes.size() / UI_RATE_WINDOW_SECONDS);
        }

        // plus::uiframe::worldFps().
        F64 worldFps(F64 stat_fps) const
        {
            if (!fpsCountsWorldFrames() || mLoopsSinceUIFrame >= STAT_MEDIAN_LOOPS || mWorldFrameTimes.size() < 2)
            {
                return stat_fps;
            }
            const F64 span = mWorldFrameTimes.back() - mWorldFrameTimes.front();
            return span > 0.0 ? (F64)(mWorldFrameTimes.size() - 1) / span : stat_fps;
        }

    private:
        std::deque<F64> mWorldFrameTimes;   // the last second's world frames, and always the last two
        std::deque<F64> mUIFrameTimes;      // the last second's UI-only frames
        U32 mLoopsSinceUIFrame = STAT_MEDIAN_LOOPS; // world loops since the last UI-only frame, up to STAT_MEDIAN_LOOPS
        bool mCountPending = false;         // update_statistics() left this loop's count to onFrame()
    };

    FrameStats& frameStats()
    {
        static FrameStats stats;
        return stats;
    }

    // ------------------------------------------------------------------
    // When the world can wait.

    // Things that need the world drawn whatever the UI does.
    bool worldRequired()
    {
        LLWindow* window = gViewerWindow->getWindow();
        return LLStartUp::getStartupState() < STATE_STARTED
            || gViewerWindow->getShowProgress()         // login, teleport and logout screens
            || gTeleportDisplay || gRestoreGL || gDisconnected
            || LLApp::isExiting() || LLAppViewer::instance()->logoutRequestSent()
            || gWindowResized || gResizeScreenTexture   // display() handles these first
            || !gViewerWindow->getActive() || !window->getVisible() || window->getMinimized()
            || !gFocusMgr.getAppHasFocus()
            || gAgentCamera.cameraMouselook()
            || !gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI);   // no UI to work
    }

    // Drags on these move the camera or a pose: the world has to follow.
    bool movesTheWorld(LLView* view)
    {
        return dynamic_cast<LLJoystick*>(view) || dynamic_cast<FSVirtualTrackpad*>(view);
    }

    F64 secondsSinceInput()
    {
#if LL_WINDOWS
        LASTINPUTINFO info = { sizeof(info) };
        if (GetLastInputInfo(&info))
        {
            return (DWORD)(GetTickCount() - info.dwTime) / 1000.0;
        }
#endif
        return LLUI::getInstance()->getMouseIdleTime();
    }

    bool mouseBusyOverGraphicsFloater()
    {
        if (secondsSinceInput() > RECENT_INPUT_SECONDS)
        {
            return false;
        }
        const LLCoordGL mouse = gViewerWindow->getCurrentMouse();
        for (const char* name : GRAPHICS_FLOATERS)
        {
            LLFloater* floater = LLFloaterReg::findInstance(name);
            if (floater && floater->isInVisibleChain() && !floater->isMinimized() &&
                floater->calcScreenRect().pointInRect(mouse.mX, mouse.mY))
            {
                return true;
            }
        }
        return false;
    }

    // What the user is doing with the UI right now. A click that reaches the
    // world (and schedules a pick) is none of these: the world is drawn then.
    Activity uiActivity()
    {
        if (LLMenuGL::sMenuContainer && LLMenuGL::sMenuContainer->hasVisibleMenu())
        {
            return Activity::Menu;
        }

        // An open combo box list has the keyboard focus.
        LLView* focus = dynamic_cast<LLView*>(gFocusMgr.getKeyboardFocus());
        if (dynamic_cast<LLScrollListCtrl*>(focus) && focus->getVisible() && dynamic_cast<LLComboBox*>(focus->getParent()))
        {
            return Activity::List;
        }

        const bool button_down = gViewerWindow->getLeftMouseDown() || gViewerWindow->getRightMouseDown() ||
                                 gViewerWindow->getMiddleMouseDown();
        LLView* captor = dynamic_cast<LLView*>(gFocusMgr.getMouseCapture());    // tools are no views
        if (button_down && captor && !movesTheWorld(captor))
        {
            return Activity::Drag;
        }

        if (mouseBusyOverGraphicsFloater())
        {
            return Activity::Graphics;
        }
        return Activity::None;
    }

    // ------------------------------------------------------------------

    class UIFrames
    {
    public:
        bool drawInsteadOfWorld()
        {
            const F64 now = LLTimer::getTotalSeconds();
            if (mLastWasWorld && mLastDecision > 0.0)
            {
                mWorldFrameSeconds = now - mLastDecision;   // the whole loop: what the UI waits for
            }
            mLastDecision = now;

            const bool on = enabled();
            mLogging = on && logging();
            if (mLogging)
            {
                mMeter.onInputHandled();
                mMeter.report(now, mWorldFrameSeconds);
            }

            static LLCachedControl<F32> threshold_ms(gSavedSettings, SETTING_THRESHOLD_MS, 50.f);
            mWorldSlow = on && mWorldFrameSeconds * 1000.0 > threshold_ms();
            const Activity activity = mWorldSlow && !worldRequired() ? uiActivity() : Activity::None;
            mHolding = activity != Activity::None;
            if (mLogging && mHolding)
            {
                mMeter.countActivity(activity);
            }

            const U32 width = (U32)gViewerWindow->getWindowWidthRaw();
            const U32 height = (U32)gViewerWindow->getWindowHeightRaw();
            if (mHolding && mImage.matches(width, height) && !worldDue(activity, now))
            {
                drawUIOnlyFrame();
                mLastWasWorld = false;
                return true;
            }

            if (!on)
            {
                mImage.release();
            }
            else if (!mWorldSlow && now - mLastCapture > RELEASE_IMAGE_SECONDS)
            {
                mImage.release();
            }
            mLastWasWorld = true;
            mLastWorldStart = now;
            if (mLogging)
            {
                mMeter.countWorldFrame();
            }
            if (const U32 slow_ms = slowWorldTestMs(); slow_ms && LLStartUp::getStartupState() >= STATE_STARTED)
            {
                ms_sleep(slow_ms);
            }
            return false;
        }

        void onWorldDrawn()
        {
            if (gSnapshot || gDisconnected)
            {
                return;     // not the frame on the screen
            }
            const F64 now = LLTimer::getTotalSeconds();
            if (mWorldSlow && LLRenderTarget::sCurFBO == 0 && WorldImage::canCapture())
            {
                mImage.capture((U32)gViewerWindow->getWindowWidthRaw(), (U32)gViewerWindow->getWindowHeightRaw());
                mLastCapture = now;
            }
            else
            {
                mImage.invalidate();
            }
            if (mHolding)
            {
                gViewerWindow->setup2DRender();     // render_ui_2d() sets it up again
                drawNotice();
            }
            if (mLogging)
            {
                mMeter.onPresent(mHolding, now);    // the 2D UI and the present follow at once
            }
        }

    private:
        // Whether the world gets its next frame now, by its share of the time
        // and the longest pause.
        bool worldDue(Activity activity, F64 now) const
        {
            static LLCachedControl<F32> world_share(gSavedSettings, SETTING_WORLD_SHARE, 0.25f);
            static LLCachedControl<F32> max_pause_ms(gSavedSettings, SETTING_MAX_PAUSE_MS, 1000.f);
            const F64 since = now - mLastWorldStart;
            if (activity != Activity::Drag && since * 1000.0 >= max_pause_ms())
            {
                return true;
            }
            const F32 share = world_share();
            if (share < MIN_WORLD_SHARE)
            {
                return false;
            }
            // A world frame of W and the UI-only frames after it: W / share in all.
            return since >= mWorldFrameSeconds / llmin(share, MAX_WORLD_SHARE);
        }

        // What display() does besides drawing the world, then the last world
        // image and the 2D UI, presented.
        void drawUIOnlyFrame()
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("UI-only frame");
            const F64 start = LLTimer::getTotalSeconds();
            LLRHI::cpuMark("uiframe");
            LLRHI::gpuMark("uiframe");

            gViewerWindow->performPick();   // CPU ray tests; normally none is waiting (see uiActivity())
            updateTextures();

            LLGLSDefault gls_default;
            if (gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_DYNAMIC_TEXTURES) &&
                LLViewerDynamicTexture::updateAllInstances())     // previews in floaters
            {
                gGL.setColorMask(true, true);
            }

            {
                LLGLSUIDefault gls_ui;
                gPipeline.disableLights();
                gViewerWindow->setup2DRender();
                LLRHI::clear(GL_DEPTH_BUFFER_BIT);
                mImage.draw();
                drawNotice();
                render_ui_2d();
                plus::detach::drawWindows();    // as render_ui() does after render_ui_2d()
                gViewerWindow->setup2DRender();
                plus::uishowbtn::draw();        // and the "Show UI" button
                gViewerWindow->updateDebugText();
                gViewerWindow->drawDebugText();
                gGL.flush();
            }
            LLVertexBuffer::unbind();
            LLGLState::checkStates();
            gViewerWindow->getWindow()->swapBuffers();

            if (mLogging)
            {
                const F64 end = LLTimer::getTotalSeconds();
                mMeter.onPresent(true, end);
                mMeter.countUIFrame(end - start);
            }
        }

        // Texture creation and fetch priorities, as display() keeps them going
        // while the window is minimized.
        static void updateTextures()
        {
            LL_PROFILE_ZONE_NAMED_CATEGORY_DISPLAY("UI-only frame textures");
            LLViewerTexture::updateClass();
            gBumpImageList.updateImages();  // before gTextureList, as in display()
            gTextureList.updateImages(UI_FRAME_TEXTURE_TIME);
            gGLTFMaterialList.flushMaterials();
        }

        // "The world picture is held": so a held picture is not taken for a slow
        // world. Needs the 2D setup.
        static void drawNotice()
        {
            static LLCachedControl<bool> show_notice(gSavedSettings, "PlusUIFrameNotice", false);
            if (!show_notice)
            {
                return;     // off by default: the user does not want the message
            }
            static const std::string text = LLUI::getLanguage().rfind("ja", 0) == 0
                ? "UI の操作中は世界の描画を間引いています"
                : "World view held while you use the UI";
            const LLRect world = gViewerWindow->getWorldViewRectScaled();
            constexpr S32 TOP_MARGIN = 48;      // below the status and navigation bars

            gUIProgram.bind();
            gGL.pushMatrix();
            gGL.pushUIMatrix();
            gGL.scaleUI(LLUI::getScaleFactor().mV[VX], LLUI::getScaleFactor().mV[VY], 1.f);
            LLFontGL::getFontSansSerifSmall()->renderUTF8(text, 0, (F32)world.getCenterX(), (F32)(world.mTop - TOP_MARGIN),
                                                          LLColor4(1.f, 0.85f, 0.3f, 1.f), LLFontGL::HCENTER, LLFontGL::TOP,
                                                          LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
            gGL.popUIMatrix();
            gGL.popMatrix();
            gGL.flush();
            gUIProgram.unbind();
        }

        WorldImage mImage;
        Meter mMeter;
        F64 mLastDecision = 0.0;
        F64 mLastWorldStart = 0.0;
        F64 mLastCapture = 0.0;
        F64 mWorldFrameSeconds = 0.0;   // the last world loop, input to input
        bool mLastWasWorld = true;
        bool mWorldSlow = false;        // this loop: the world is slow (keep its image)
        bool mHolding = false;          // this loop: the user works the UI over a slow world
        bool mLogging = false;          // this loop: measuring (PlusUiFrameLog)
    };

    UIFrames& uiFrames()
    {
        static UIFrames frames;
        return frames;
    }

    void init()
    {
        // Settings: app_settings/settings_plus.xml (PlusUiFrame*).
        // Called from the <plus> hooks in LLAppViewer::doFrame() and render_ui().
    }

    plus::FeatureRegistration sRegistration({ "uiframe", &init, {} });
}

namespace plus::uiframe
{
    bool drawInsteadOfWorld()
    {
        const F64 now = LLTimer::getTotalSeconds();
        const bool ui_only = uiFrames().drawInsteadOfWorld();
        frameStats().onFrame(now, !ui_only);
        return ui_only;
    }

    void onWorldDrawn()
    {
        uiFrames().onWorldDrawn();
    }

    bool countFrameLater()
    {
        return frameStats().countFrameLater();
    }

    F64 worldFps(F64 stat_fps)
    {
        return frameStats().worldFps(stat_fps);
    }
}
