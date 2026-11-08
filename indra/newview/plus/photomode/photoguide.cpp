/**
 * @file photoguide.cpp
 * @brief Photo tools: the composition guide drawn over the screen.
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

// The frame is the picture a one-press photo would take (photoshot.h): the largest rectangle of the
// photo's shape that fits in the window, in the middle (that is how LLViewerWindow::rawSnapshot crops a
// picture whose shape differs from the window's), and the outside is dimmed. The photo is taken of the
// whole window (without UI), not of the area between the toolbars the 3D view is drawn in: with
// toolbars on the screen the frame is the photo's, and the toolbars cover a part of it.
//
// No official file is touched: the view is a child of the window's root view (LLViewerWindow::getRootView())
// sent to the back, which is under every window, menu and toolbar and above the 3D picture (the world is
// drawn before the UI).

#include "llviewerprecompiledheaders.h"

#include "photoguide.h"
#include "photoshot.h"

#include "plus/camroll/camroll.h"
#include "plus/photoscreen/photoscreen.h"
#include "plus/photoscreen/psui/psstyle.h"
#include "plus/presets/presets.h"

#include <cmath>

#include "llfontgl.h"
#include "llframetimer.h"
#include "llrender2dutils.h"
#include "lluictrlfactory.h"
#include "llview.h"
#include "llviewercontrol.h"

namespace plus::photomode
{
    namespace
    {
        constexpr char SETTING_ON[]      = "PlusPhotoGuideOn";
        constexpr char SETTING_KIND[]    = "PlusPhotoGuideKind";
        constexpr char SETTING_COLOR[]   = "PlusPhotoGuideColor";
        constexpr char SETTING_OPACITY[] = "PlusPhotoGuideOpacity";
        constexpr char SETTING_THICK[]   = "PlusPhotoGuideThickness";
        constexpr char SETTING_ASPECT[]  = "PlusPhotoShotAspect";

        constexpr F32 DIM_ALPHA = 0.55f;
        constexpr F32 PHOTO_SCREEN_ALPHA = 0.76f;   // in the photo screen's frame: 50 % of the setting shows as 38 %

        // showGuideMessage(): the line and how long ago it was set
        constexpr F32 MESSAGE_SECONDS = 1.5f;
        constexpr F32 MESSAGE_FADE    = 0.4f; // the last part of it
        std::string   sMessage;
        LLFrameTimer  sMessageTimer;
        std::string   sBadge; // setGuideBadge(): stays until it is emptied

        LLColor4 colorOf(int index)
        {
            switch (index)
            {
            case 1:  return LLColor4(0.f, 0.f, 0.f, 1.f);
            case 2:  return LLColor4(1.f, 0.9f, 0.1f, 1.f);
            case 3:  return LLColor4(1.f, 0.15f, 0.1f, 1.f);
            default: return LLColor4(1.f, 1.f, 1.f, 1.f);
            }
        }

        class PhotoGuideView : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit PhotoGuideView(const Params& p) : LLView(p) {}
            friend class LLUICtrlFactory;

        public:
            void draw() override
            {
                if (plus::presets::snapshotInProgress())
                {
                    return;
                }
                LLRect window = getLocalRect();
                // the photo screen's detailed mode: the lines go in the world's rectangle, thin and plain (research/221 10.)
                mInPhotoScreen = plus::photoscreen::shownWorldRect(window);
                if (window.getWidth() < 8 || window.getHeight() < 8)
                {
                    return;
                }
                static LLCachedControl<bool> guide_on(gSavedSettings, SETTING_ON, false);   // every frame (research/125 #25)
                if (guide_on)
                {
                    drawGuide(window);
                }
                drawMessage(window);
                drawBadge(window);
            }

        private:
            void drawGuide(const LLRect& window)
            {
                const LLRect frame = frameIn(window);

                // the outside of the frame, dimmed
                const bool framed = frame != window;
                if (framed)
                {
                    const LLColor4 dim(0.f, 0.f, 0.f, DIM_ALPHA);
                    gl_rect_2d(window.mLeft, window.mTop, frame.mLeft, window.mBottom, dim);
                    gl_rect_2d(frame.mRight, window.mTop, window.mRight, window.mBottom, dim);
                    gl_rect_2d(frame.mLeft, window.mTop, frame.mRight, frame.mTop, dim);
                    gl_rect_2d(frame.mLeft, frame.mBottom, frame.mRight, window.mBottom, dim);
                }

                // Every frame while the guide shows: no lookup by name (fix-pipeleak).
                static LLCachedControl<S32> opacity(gSavedSettings, SETTING_OPACITY, 50);
                static LLCachedControl<S32> color(gSavedSettings, SETTING_COLOR, 0);
                static LLCachedControl<S32> thickness(gSavedSettings, SETTING_THICK, 0);
                static LLCachedControl<S32> guide_kind(gSavedSettings, SETTING_KIND, 1);
                mAlpha = llclamp((S32)opacity, 0, 100) / 100.f;
                mColor = colorOf(color);
                mThick = thickness > 0 ? 2 : 1;
                if (mInPhotoScreen)
                {
                    // research/221 10.: 1 px lines, white 38 % at the default opacity (50), no dark edge
                    mAlpha *= PHOTO_SCREEN_ALPHA;
                    mThick = 1;
                }

                if (framed)
                {
                    rectangle(frame);
                }

                const GuideKind kind = static_cast<GuideKind>(llclamp((S32)guide_kind, 0, (S32)GuideKind::Count - 1));
                const S32 w = frame.getWidth();
                const S32 h = frame.getHeight();
                switch (kind)
                {
                case GuideKind::Thirds:
                    gridLines(frame, { 1.0 / 3.0, 2.0 / 3.0 });
                    break;
                case GuideKind::Golden:
                    gridLines(frame, { 0.381966, 0.618034 });
                    break;
                case GuideKind::Cross:
                    gridLines(frame, { 0.5 });
                    break;
                case GuideKind::Diagonals:
                    line(frame.mLeft, frame.mBottom, frame.mRight, frame.mTop);
                    line(frame.mLeft, frame.mTop, frame.mRight, frame.mBottom);
                    break;
                case GuideKind::Grid4:
                    gridLines(frame, { 0.25, 0.5, 0.75 });
                    break;
                case GuideKind::Grid6:
                    gridLines(frame, { 1.0 / 6.0, 2.0 / 6.0, 0.5, 4.0 / 6.0, 5.0 / 6.0 });
                    break;
                case GuideKind::Horizon:
                    horizon(frame, w, h);
                    break;
                default:
                    break;
                }
            }

            // The line of showGuideMessage(), a little above the middle (clear of the subject in the
            // middle and of the thirds' lines), on a dark band; it fades out at the end.
            static void drawMessage(const LLRect& window)
            {
                const F32 age = sMessageTimer.getElapsedTimeF32();
                if (sMessage.empty() || age > MESSAGE_SECONDS)
                {
                    return;
                }
                const F32 fade = llclamp((MESSAGE_SECONDS - age) / MESSAGE_FADE, 0.f, 1.f);
                const LLFontGL* font = LLFontGL::getFontSansSerif();
                const S32 width  = font->getWidth(sMessage) + 24;
                const S32 height = font->getLineHeight() + 10;
                const S32 cx     = window.getCenterX();
                const S32 cy     = window.mBottom + window.getHeight() * 3 / 5;
                gl_rect_2d(cx - width / 2, cy + height / 2, cx + width / 2, cy - height / 2, LLColor4(0.f, 0.f, 0.f, 0.55f * fade));
                font->renderUTF8(sMessage, 0, (F32)cx, (F32)cy, LLColor4(1.f, 1.f, 1.f, fade), LLFontGL::HCENTER, LLFontGL::VCENTER,
                                 LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
            }

            // The badge of setGuideBadge(): at the top, in the middle, on a dark band.
            static void drawBadge(const LLRect& window)
            {
                if (sBadge.empty())
                {
                    return;
                }
                const LLFontGL* font = LLFontGL::getFontSansSerif();
                const S32 width  = font->getWidth(sBadge) + 28;
                const S32 height = font->getLineHeight() + 12;
                const S32 cx     = window.getCenterX();
                const S32 top    = window.mTop - 40;
                gl_rect_2d(cx - width / 2, top, cx + width / 2, top - height, LLColor4(0.f, 0.f, 0.f, 0.6f));
                font->renderUTF8(sBadge, 0, (F32)cx, (F32)(top - height / 2), LLColor4(1.f, 0.9f, 0.3f, 1.f), LLFontGL::HCENTER,
                                 LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
            }

            // The photo's frame in the window.
            static LLRect frameIn(const LLRect& window)
            {
                static LLCachedControl<S32> aspect(gSavedSettings, SETTING_ASPECT, 0);     // every frame (fix-pipeleak)
                const double ratio = shotAspectRatio(aspect);
                if (ratio <= 0.0)
                {
                    return window; // the window's own shape: no frame
                }
                const double window_ratio = (double)window.getWidth() / window.getHeight();
                S32          w = window.getWidth(), h = window.getHeight();
                if (window_ratio > ratio)
                {
                    w = (S32)std::lround(h * ratio);
                }
                else
                {
                    h = (S32)std::lround(w / ratio);
                }
                const S32 left   = window.mLeft + (window.getWidth() - w) / 2;
                const S32 bottom = window.mBottom + (window.getHeight() - h) / 2;
                return LLRect(left, bottom + h, left + w, bottom);
            }

            // A line with a thin dark edge, so it shows on a bright sky and a dark night alike.
            void line(S32 x1, S32 y1, S32 x2, S32 y2)
            {
                const LLColor4 shadow(0.f, 0.f, 0.f, mInPhotoScreen ? 0.f : mAlpha * 0.7f);
                const LLColor4 color(mColor.mV[VRED], mColor.mV[VGREEN], mColor.mV[VBLUE], mAlpha);
                if (y1 == y2) // across
                {
                    gl_rect_2d(llmin(x1, x2), y1 - 1 - mThick, llmax(x1, x2), y1 - 1 - mThick + mThick, shadow);
                    gl_rect_2d(llmin(x1, x2), y1 + mThick, llmax(x1, x2), y1, color);
                    return;
                }
                if (x1 == x2) // up
                {
                    gl_rect_2d(x1 + 1, llmax(y1, y2), x1 + 1 + mThick, llmin(y1, y2), shadow);
                    gl_rect_2d(x1, llmax(y1, y2), x1 + mThick, llmin(y1, y2), color);
                    return;
                }
                gl_line_2d(x1 + 1, y1 - 1, x2 + 1, y2 - 1, shadow);
                gl_line_2d(x1, y1, x2, y2, color);
                if (mThick > 1)
                {
                    gl_line_2d(x1, y1 + 1, x2, y2 + 1, color);
                }
            }

            void rectangle(const LLRect& r)
            {
                line(r.mLeft, r.mTop, r.mRight, r.mTop);
                line(r.mLeft, r.mBottom, r.mRight, r.mBottom);
                line(r.mLeft, r.mTop, r.mLeft, r.mBottom);
                line(r.mRight - mThick, r.mTop, r.mRight - mThick, r.mBottom);
            }

            // Lines across and up the frame at the given shares of its width and height.
            void gridLines(const LLRect& r, std::initializer_list<double> shares)
            {
                for (const double share : shares)
                {
                    const S32 x = r.mLeft + (S32)std::lround(r.getWidth() * share);
                    const S32 y = r.mBottom + (S32)std::lround(r.getHeight() * share);
                    line(x, r.mTop, x, r.mBottom);
                    line(r.mLeft, y, r.mRight, y);
                }
            }

            // A level for the camera's tilt: the line follows the world's horizontal (it turns the other
            // way from the camera, so it lies along the horizon of a flat scene), the marks at the sides
            // stay level with the screen. Green when the camera is level.
            void horizon(const LLRect& r, S32 w, S32 h)
            {
                const F32 degrees = plus::camroll::currentDegrees();
                const S32 cx      = r.mLeft + w / 2;
                const S32 cy      = r.mBottom + h / 2;
                const F32 half    = w * 0.36f;
                const F32 radians = degrees * DEG_TO_RAD;
                const bool level  = fabsf(degrees) < 0.3f;

                const LLColor4 keep = mColor;
                // the screen-level marks
                line(cx - (S32)(half) - 24, cy, cx - (S32)(half) - 6, cy);
                line(cx + (S32)(half) + 6, cy, cx + (S32)(half) + 24, cy);
                if (level)
                {
                    mColor = LLColor4(0.2f, 1.f, 0.3f, 1.f);
                }
                // the tilted line: a positive tilt turns the camera's up toward its left (camroll.h), so a flat
                // scene's horizon turns clockwise on the screen (y up here: dy goes down on the right)
                const S32 dx = (S32)std::lround(half * cosf(radians));
                const S32 dy = (S32)std::lround(-half * sinf(radians));
                line(cx - dx, cy - dy, cx + dx, cy + dy);
                mColor = keep;
            }

            F32      mAlpha = 0.5f;
            LLColor4 mColor = LLColor4::white;
            S32      mThick = 1;
            bool     mInPhotoScreen = false;
        };

        LLHandle<LLView> sGuide;
    }

    void attachGuide()
    {
        psui::attachWorldOverlay<PhotoGuideView>(sGuide, "plus_photo_guide");
    }

    void showGuideMessage(const std::string& text)
    {
        sMessage = text;
        sMessageTimer.reset();
    }

    void setGuideBadge(const std::string& text)
    {
        sBadge = text;
    }
}
