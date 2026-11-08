/**
 * @file uishowbtn.cpp
 * @brief "Show UI" button: with the UI hidden, the mouse at the top edge of the window brings down a
 *        button that shows it again.
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

// Charis's own. Two ways to hide the UI, both with a shortcut to remember and no way back without it:
//  - View > Hide UI (Ctrl+Shift+U): floaters, toolbars and bars are hidden (HideUIControls);
//  - Advanced > Rendering Types... > UI (Ctrl+Alt+F1): the root view is not drawn and takes no click.
// In either, the mouse at the top edge brings down a small button ("Show UI") that shows it again, as the
// bar of a browser in full screen does. It is not a view (the root view is not drawn in the second case):
// draw() paints it from the end of render_ui() and handleClick() takes its click before the UI gets it,
// both from <plus> hooks.
//
// The button slides down when the mouse touches the top edge (within EDGE_PIXELS) and up again when
// the mouse leaves its area (the button and a margin around it). It is not shown in mouselook (the
// cursor is not a cursor there), in a snapshot, before the world, or while disconnected.
// A click is a press and a release on the button; the release shows the UI again.
// A press and its release go to the same side: the button takes a release only when it took the press
// (wherever the release lands), so a drag begun in the world and let go on the button reaches the world,
// which lets go of its capture and its mouse clipping. Nothing is taken while something holds the
// mouse capture.

#include "llviewerprecompiledheaders.h"

#include "uishowbtn.h"

#include "plus.h"
#include "plus/presets/presets.h"
#include "plus/detach/detach.h"

#include "llagentcamera.h"
#include "llappviewer.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llgl.h"
#include "llmoveview.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llshadermgr.h"
#include "llstartup.h"
#include "llui.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "llviewerwindow.h"
#include "pipeline.h"

namespace plus::uishowbtn
{
    namespace
    {
        constexpr char SETTING_ON[] = "PlusUiShowButton";

        constexpr S32 EDGE_PIXELS   = 3;     // the mouse this close to the top edge brings the button down
        constexpr S32 BUTTON_WIDTH  = 220;
        constexpr S32 BUTTON_HEIGHT = 46;
        constexpr S32 TOP_GAP       = 8;     // button to the window's top edge
        constexpr S32 KEEP_MARGIN   = 24;    // the button stays down while the mouse is this close to it
        constexpr F32 SLIDE_SECONDS = 0.18f; // up to down, and back

        const LLColor4 BOX_COLOR(0.08f, 0.09f, 0.11f, 0.90f);
        const LLColor4 BOX_HOVER_COLOR(0.15f, 0.34f, 0.62f, 0.95f);
        const LLColor4 BORDER_COLOR(1.f, 1.f, 1.f, 0.35f);
        const LLColor4 TEXT_COLOR(1.f, 1.f, 1.f, 1.f);
        const LLColor4 HINT_COLOR(1.f, 1.f, 1.f, 0.65f);

        // 0: up (not shown) .. 1: down. Moved toward its goal each frame; the position follows.
        F32  sSlide   = 0.f;
        U32  sTakenPresses = 0;     // a bit per mouse button (buttonBit): its press was taken by the button,
                                    // so its release is too; the left one's release on the button is the click

        // One bit per physical button; a double click is a press of its button.
        U32 buttonBit(EMouseClickType clicktype)
        {
            switch (clicktype)
            {
            case CLICK_DOUBLELEFT:   clicktype = CLICK_LEFT;   break;
            case CLICK_DOUBLERIGHT:  clicktype = CLICK_RIGHT;  break;
            case CLICK_DOUBLEMIDDLE: clicktype = CLICK_MIDDLE; break;
            default: break;
            }
            return (clicktype >= 0 && clicktype < CLICK_COUNT) ? (1u << clicktype) : 0u;
        }

        bool featureOn()
        {
            static LLCachedControl<bool> on(gSavedSettings, SETTING_ON, true);
            return on;
        }

        bool uiHiddenByControls()
        {
            return !gViewerWindow->getUIVisibility();
        }

        bool uiHiddenByFeature()
        {
            return !gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        }

        // The UI is hidden by one of the two ways and the button has a use here.
        bool wanted()
        {
            if (!featureOn() || gDisconnected || LLStartUp::getStartupState() < STATE_STARTED)
            {
                return false;
            }
            if (!uiHiddenByControls() && !uiHiddenByFeature())
            {
                return false;
            }
            return !gAgentCamera.cameraMouselook() && !plus::presets::snapshotInProgress();
        }

        // The button's rectangle at the slide position (UI-scaled window coordinates). With the
        // button fully up its bottom is at the window's top edge: nothing of it is in the window.
        LLRect buttonRect(F32 slide)
        {
            const S32 width  = gViewerWindow->getWindowWidthScaled();
            const S32 height = gViewerWindow->getWindowHeightScaled();
            const S32 down   = ll_round(slide * (F32)(BUTTON_HEIGHT + TOP_GAP));
            const S32 top    = height + BUTTON_HEIGHT - down;
            const S32 left   = (width - BUTTON_WIDTH) / 2;
            return LLRect(left, top, left + BUTTON_WIDTH, top - BUTTON_HEIGHT);
        }

        // The shortcut that hides (and shows) the UI the way it was hidden.
        const char* shortcutText(bool japanese)
        {
            if (uiHiddenByFeature() && !uiHiddenByControls())
            {
                return japanese ? "Ctrl+Alt+F1 でも戻せます" : "Ctrl+Alt+F1 also shows it";
            }
            return japanese ? "Ctrl+Shift+U でも戻せます" : "Ctrl+Shift+U also shows it";
        }

        void showUI()
        {
            sSlide = 0.f;
            if (uiHiddenByFeature())
            {
                // as Advanced > Rendering Types > UI: the root view and the clicks come back
                LLPipeline::toggleRenderDebugFeature(LLPipeline::RENDER_DEBUG_FEATURE_UI);
            }
            if (uiHiddenByControls())
            {
                // as View > Hide UI (LLViewToggleUI::confirm), the way back
                gViewerWindow->setUIVisibility(true);
                LLPanelStandStopFlying::getInstance()->setVisible(true);
                gSavedSettings.setBOOL("HideUIControls", false);
            }
        }
    }

    void draw()
    {
        if (!wanted())
        {
            sSlide = 0.f;   // a taken press keeps its bit: its release is still the button's
            return;
        }

        // Where the mouse is decides the goal: at the top edge, or still near the button when it is down.
        // Not while it is on a detached window (fix-round86): the point is not kept at the main window's
        // edge there (plus::detach::keepsMousePoint()), so a window over the top band, or on a monitor
        // above, would bring the button down unseen under it, and it would take the clicks on that window.
        const S32 mouse_x = gViewerWindow->getCurrentMouseX();
        const S32 mouse_y = gViewerWindow->getCurrentMouseY();
        const S32 height  = gViewerWindow->getWindowHeightScaled();
        const bool on_window = plus::detach::containsPoint(mouse_x, mouse_y);
        bool goal_down = !on_window && mouse_y >= height - EDGE_PIXELS;
        if (!goal_down && !on_window && sSlide > 0.f)
        {
            LLRect keep = buttonRect(1.f);
            keep.stretch(KEEP_MARGIN);
            goal_down = keep.pointInRect(mouse_x, mouse_y);
        }
        const F32 step = (F32)gFrameIntervalSeconds / SLIDE_SECONDS;
        sSlide = goal_down ? llmin(1.f, sSlide + step) : llmax(0.f, sSlide - step);
        if (sSlide <= 0.f)
        {
            return;
        }

        const LLRect rect  = buttonRect(sSlide);
        const bool hover   = !on_window && rect.pointInRect(mouse_x, mouse_y);
        const bool japanese = LLUI::getLanguage().rfind("ja", 0) == 0;
        const F32 alpha    = sSlide;

        gUIProgram.bind();
        gGL.pushMatrix();
        gGL.pushUIMatrix();
        gGL.scaleUI(LLUI::getScaleFactor().mV[VX], LLUI::getScaleFactor().mV[VY], 1.f);
        {
            LLGLSUIDefault gls_ui;
            LLColor4 box = hover ? BOX_HOVER_COLOR : BOX_COLOR;
            box.mV[VALPHA] *= alpha;
            gl_rect_2d(rect, box, true);
            LLColor4 border = BORDER_COLOR;
            border.mV[VALPHA] *= alpha;
            gl_rect_2d(rect, border, false);

            LLColor4 text = TEXT_COLOR;
            text.mV[VALPHA] *= alpha;
            LLColor4 hint = HINT_COLOR;
            hint.mV[VALPHA] *= alpha;
            LLFontGL::getFontSansSerif()->renderUTF8(japanese ? "UI を表示" : "Show UI", 0, (F32)rect.getCenterX(),
                                                     (F32)(rect.mTop - 7), text, LLFontGL::HCENTER, LLFontGL::TOP,
                                                     LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
            LLFontGL::getFontSansSerifSmall()->renderUTF8(shortcutText(japanese), 0, (F32)rect.getCenterX(),
                                                          (F32)(rect.mBottom + 6), hint, LLFontGL::HCENTER,
                                                          LLFontGL::BOTTOM, LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
        }
        gGL.popUIMatrix();
        gGL.popMatrix();
        gGL.flush();
        gUIProgram.unbind();
    }

    bool handleClick(S32 x, S32 y, EMouseClickType clicktype, bool down)
    {
        const U32 bit = buttonBit(clicktype);
        // A view or a tool holds the mouse (a grab, a drag): its presses and releases are its own,
        // wherever they land. Its release must reach it, or the capture and the clipping stay.
        if (gFocusMgr.getMouseCapture())
        {
            // fix-round100: except the release of a press the button took before that capture began (by
            // another button): the captor never saw that press, so its release is not the captor's either.
            const bool taken = !down && (sTakenPresses & bit);
            sTakenPresses &= ~bit;
            return taken;
        }
        if (!down)
        {
            // Only the release of a press the button took is the button's (the world never saw that press);
            // a release whose press went elsewhere goes there too.
            if (!(sTakenPresses & bit))
            {
                return false;
            }
            sTakenPresses &= ~bit;
            if (bit == buttonBit(CLICK_LEFT) && sSlide > 0.f && wanted() && buttonRect(sSlide).pointInRect(x, y))
            {
                showUI();
            }
            return true;
        }
        // A press on a detached window is that window's, even where the button lies under it (fix-round86).
        if (sSlide <= 0.f || !wanted() || plus::detach::containsPoint(x, y) || !buttonRect(sSlide).pointInRect(x, y))
        {
            sTakenPresses &= ~bit;  // a press elsewhere: an earlier taken press that never got its release is forgotten
            return false;
        }
        sTakenPresses |= bit;       // a press on the button is the button's, whichever button: not the world's
        return true;
    }
}
