/**
 * @file camerapathoverlay.cpp
 * @brief Camera path: the route and the points drawn over the 3D picture while the path is edited.
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

// Like the composition guide of plus/photomode (photoguide.cpp): a view that is a child of the window's root
// view, sent to the back, so it is under every window and toolbar and above the 3D picture (the world is drawn
// before the UI). Nothing of the renderer is touched: the points are put on the screen with the camera's own
// projection and drawn as 2D lines. It draws only while the path window is open, the switch is on and nothing
// plays; never while a picture is taken, so it is not in a photo or a frame of a video.

#include "llviewerprecompiledheaders.h"

#include "camerapath.h"

#include "plus/photoscreen/psui/psstyle.h"
#include "plus/presets/presets.h"

#include "llagent.h"
#include "llfontgl.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "lluictrlfactory.h"
#include "llui.h"
#include "llview.h"
#include "llviewercamera.h"

namespace plus::camerapath
{
    namespace
    {
        constexpr S32 POINT_RADIUS     = 6;
        constexpr S32 SELECTED_RADIUS  = 9;
        constexpr double LOOK_LENGTH   = 3.0;   // metres of the little line that shows where a point looks

        class RouteView : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit RouteView(const Params& p) : LLView(p) {}
            friend class LLUICtrlFactory;

        public:
            void draw() override
            {
                if (!routeVisible() || plus::presets::snapshotInProgress())
                {
                    return;
                }
                const std::vector<std::vector<LLVector3d>>& lines = route();
                const LLColor4 line_color(0.35f, 0.85f, 1.f, 0.85f);
                const LLColor4 shadow(0.f, 0.f, 0.f, 0.6f);

                for (const std::vector<LLVector3d>& line : lines)
                {
                    S32  x0 = 0, y0 = 0;
                    bool have_last = false;
                    for (const LLVector3d& place : line)
                    {
                        S32 x = 0, y = 0;
                        const bool visible = project(place, x, y);
                        if (visible && have_last)
                        {
                            gl_line_2d(x0 + 1, y0 - 1, x + 1, y - 1, shadow);
                            gl_line_2d(x0, y0, x, y, line_color);
                        }
                        have_last = visible;
                        x0 = x;
                        y0 = y;
                    }
                }

                const Path& open = path();
                const int   chosen = selectedPoint();
                for (size_t i = 0; i < open.points.size(); ++i)
                {
                    const camerapath::Point& point = open.points[i];
                    S32 x = 0, y = 0;
                    if (!project(point.position, x, y))
                    {
                        continue;
                    }
                    const bool selected = (int)i == chosen;

                    // where the point looks
                    S32 lx = 0, ly = 0;
                    const LLVector3d to_focus = point.focus - point.position;
                    const double     length   = to_focus.magVec();
                    if (length > 0.01 && project(point.position + to_focus * (LOOK_LENGTH / length), lx, ly))
                    {
                        gl_line_2d(x, y, lx, ly, selected ? LLColor4(1.f, 0.9f, 0.2f, 0.9f) : LLColor4(1.f, 1.f, 1.f, 0.5f));
                    }

                    const S32 radius = selected ? SELECTED_RADIUS : POINT_RADIUS;
                    gGL.color4fv(LLColor4(0.f, 0.f, 0.f, 0.7f).mV);   // a dark edge under, so it shows on a bright sky
                    gl_circle_2d((F32)x, (F32)y, (F32)radius + 1.f, 16, true);
                    gGL.color4fv((selected ? LLColor4(1.f, 0.9f, 0.2f, 1.f) : LLColor4(0.35f, 0.85f, 1.f, 1.f)).mV);
                    gl_circle_2d((F32)x, (F32)y, (F32)radius, 16, true);
                    mLabel = std::to_string(i + 1);
                    LLFontGL::getFontSansSerifSmall()->renderUTF8(mLabel, 0, x, y, LLColor4::black, LLFontGL::HCENTER,
                                                                  LLFontGL::VCENTER, LLFontGL::NORMAL);
                }
            }

        private:
            // The place on the screen in this view's coordinates; false when it is behind the camera or off the screen.
            static bool project(const LLVector3d& global, S32& x, S32& y)
            {
                LLCoordGL screen;
                if (!LLViewerCamera::getInstance()->projectPosAgentToScreen(gAgent.getPosAgentFromGlobal(global), screen, false))
                {
                    return false;
                }
                // already in UI coordinates: projectPosAgentToScreen() divides by the display scale
                x = screen.mX;
                y = screen.mY;
                return true;
            }

            std::string mLabel;
        };

        LLHandle<LLView> sOverlay;
    }

    void attachOverlay()
    {
        psui::attachWorldOverlay<RouteView>(sOverlay, "plus_camera_path_route");
    }
}
