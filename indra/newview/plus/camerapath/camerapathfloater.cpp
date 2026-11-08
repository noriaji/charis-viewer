/**
 * @file camerapathfloater.cpp
 * @brief Camera path: the window (the timeline, the points, the chosen point's settings, the effects, play,
 *        files).
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

// The window redraws its list and fields only when camerapath::revision() moved (the path, the files or
// what plays changed) or the chosen point moved, and the time text only when what it says changes.
// The chosen point lives in camerapath (camerapath::selectedPoint()), so the list, the timeline and the route on the
// screen agree. Menu: "Camera Path" in menu_plus.xml (Floater.Toggle plus_camera_path).

#include "llviewerprecompiledheaders.h"

#include "camerapath.h"
#include "plus.h"
#include "plus/rlv/rlvhooks.h"

#include <cmath>
#include <limits>

#include "llbutton.h"
#include "llcheckboxctrl.h"
#include "llcombobox.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "lllineeditor.h"
#include "llpanel.h"
#include "llagent.h"
#include "llrand.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llscrolllistctrl.h"
#include "llsliderctrl.h"
#include "llspinctrl.h"
#include "lltextbox.h"
#include "lluictrlfactory.h"
#include "llview.h"

#include "plus/camroll/camroll.h"

namespace plus::camerapath
{
    namespace
    {
        constexpr char FLOATER_NAME[] = "plus_camera_path";
        constexpr F32  REGION_CHECK_SECONDS = 0.5f;
        // Avatars farther from the camera than this are not offered to look at.
        constexpr float SUBJECT_RADIUS = 60.f;

        // --- the timeline ---------------------------------------------------------
        //
        // The path along a line: a ball for each point where the camera is there, a thick bar for a stay, a blue bar for a
        // move, a red stroke for a cut. A click on a ball chooses the point and a drag moves it in time (the time of
        // its move changes; the points after it follow). A click or drag on the empty line shows the camera at that
        // time (the shake too). While a path plays a red line runs along it.
        class TimelineView : public LLView
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLView::Params>
            {
                Params() {}
            };

        protected:
            explicit TimelineView(const Params& p) : LLView(p) {}
            friend class LLUICtrlFactory;

        public:
            void draw() override
            {
                const LLRect rect = getLocalRect();
                const S32 bar_y = bar(rect);
                gl_rect_2d(rect.mLeft, rect.mTop, rect.mRight, rect.mBottom, LLColor4(0.f, 0.f, 0.f, 0.28f));

                const double axis = axisSeconds();
                const S32    x0   = rect.mLeft + PAD;
                const S32    x1   = rect.mRight - PAD;
                gl_rect_2d(x0, bar_y + 1, x1, bar_y - 1, LLColor4(0.6f, 0.6f, 0.6f, 0.7f));

                // seconds along the bottom
                const double step = tickStep(axis);
                const LLFontGL* font = LLFontGL::getFontSansSerifSmall();
                for (double t = 0.0; t <= axis + 1e-6; t += step)
                {
                    const S32 x = xOf(t, axis, x0, x1);
                    gl_line_2d(x, rect.mBottom + 12, x, rect.mBottom + 16, LLColor4(0.7f, 0.7f, 0.7f, 0.8f));
                    mLabel = std::to_string((int)std::lround(t));
                    font->renderUTF8(mLabel, 0, x, rect.mBottom + 2, LLColor4(0.8f, 0.8f, 0.8f, 0.9f), LLFontGL::HCENTER,
                                     LLFontGL::BOTTOM, LLFontGL::NORMAL);
                }

                const Path& open = camerapath::path();
                // stays and moves
                for (size_t i = 0; i < open.points.size(); ++i)
                {
                    const double arrive = camerapath::arrivalSeconds(i);
                    const camerapath::Point& point = open.points[i];
                    if (point.hold > 0.0)
                    {
                        gl_rect_2d(xOf(arrive, axis, x0, x1), bar_y + 3, xOf(arrive + point.hold, axis, x0, x1), bar_y - 3,
                                   LLColor4(0.35f, 0.85f, 1.f, 0.55f));
                    }
                    if (i > 0)
                    {
                        if (point.seconds <= 0.0)
                        {
                            const S32 x = xOf(arrive, axis, x0, x1);
                            gl_rect_2d(x - 1, bar_y + 9, x + 1, bar_y - 9, LLColor4(1.f, 0.35f, 0.3f, 0.95f));
                        }
                        else
                        {
                            const double leave = camerapath::arrivalSeconds(i - 1) + open.points[i - 1].hold;
                            gl_rect_2d(xOf(leave, axis, x0, x1), bar_y + 2, xOf(arrive, axis, x0, x1), bar_y - 2,
                                       LLColor4(0.3f, 0.5f, 1.f, 0.85f));
                        }
                    }
                }
                // the balls
                const int chosen = camerapath::selectedPoint();
                for (size_t i = 0; i < open.points.size(); ++i)
                {
                    const S32  x = xOf(camerapath::arrivalSeconds(i), axis, x0, x1);
                    const bool selected = (int)i == chosen;
                    gGL.color4fv(LLColor4(0.f, 0.f, 0.f, 0.7f).mV);
                    gl_circle_2d((F32)x, (F32)bar_y, (F32)BALL + 1.f, 14, true);
                    gGL.color4fv((selected ? LLColor4(1.f, 0.9f, 0.2f, 1.f) : LLColor4(0.35f, 0.85f, 1.f, 1.f)).mV);
                    gl_circle_2d((F32)x, (F32)bar_y, (F32)BALL, 14, true);
                    mLabel = std::to_string(i + 1);
                    font->renderUTF8(mLabel, 0, x, bar_y, LLColor4::black, LLFontGL::HCENTER, LLFontGL::VCENTER, LLFontGL::NORMAL);
                }
                // where the camera is
                double now = -1.0;
                if (camerapath::state() == State::Playing)
                {
                    now = camerapath::time();
                }
                else if (mScrubbing)
                {
                    now = mScrubSeconds;
                }
                if (now >= 0.0)
                {
                    const S32 x = xOf(now, axis, x0, x1);
                    gl_rect_2d(x - 1, rect.mTop - 2, x + 1, rect.mBottom + 2, LLColor4(1.f, 0.25f, 0.2f, 0.95f));
                }
            }

            bool handleMouseDown(S32 x, S32 y, MASK mask) override
            {
                const LLRect rect = getLocalRect();
                mAxis = axisSeconds();
                mX0   = rect.mLeft + PAD;
                mX1   = rect.mRight - PAD;
                const Path& open = camerapath::path();

                // the nearest ball in reach
                int    hit = -1;
                double best = (BALL + 4) * (BALL + 4);
                if (std::abs(y - bar(rect)) <= BALL + 4)
                {
                    for (size_t i = 0; i < open.points.size(); ++i)
                    {
                        const double d = xOf(camerapath::arrivalSeconds(i), mAxis, mX0, mX1) - x;
                        if (d * d <= best)
                        {
                            best = d * d;
                            hit  = (int)i;
                        }
                    }
                }
                if (hit >= 0)
                {
                    // A name being typed (the field commits when it loses the focus) goes into the point it was typed
                    // for: the focus is taken from it before another point is chosen. This view takes no focus itself.
                    gFocusMgr.setKeyboardFocus(nullptr);
                    camerapath::selectPoint(hit);
                    if (hit > 0 && camerapath::state() != State::Exporting)   // the times stay while frames are taken
                    {
                        mDrag        = hit;
                        mDragStartX  = x;
                        mDragSeconds = open.points[hit].seconds;
                        gFocusMgr.setMouseCapture(this);
                    }
                    return true;
                }
                if (camerapath::state() == State::Idle && !open.points.empty() && x >= mX0 - BALL && x <= mX1 + BALL)
                {
                    mScrubbing = true;
                    gFocusMgr.setMouseCapture(this);
                    scrubTo(x);
                    return true;
                }
                return LLView::handleMouseDown(x, y, mask);
            }

            bool handleHover(S32 x, S32 y, MASK mask) override
            {
                if (hasMouseCapture())
                {
                    if (mDrag > 0 && mDrag < (int)camerapath::path().points.size())
                    {
                        const double per_pixel = mAxis / std::max(1, mX1 - mX0);
                        double seconds = mDragSeconds + (x - mDragStartX) * per_pixel;
                        seconds = std::max(0.0, std::round(seconds * 10.0) / 10.0);   // to a tenth of a second
                        seconds = std::min(seconds, MAX_DRAG_SECONDS);
                        camerapath::Point& point = camerapath::path().points[mDrag];
                        if (seconds != point.seconds)
                        {
                            point.seconds = seconds;
                            camerapath::changed();
                        }
                    }
                    else if (mScrubbing)
                    {
                        scrubTo(x);
                    }
                    return true;
                }
                return LLView::handleHover(x, y, mask);
            }

            bool handleMouseUp(S32 x, S32 y, MASK mask) override
            {
                if (hasMouseCapture())
                {
                    gFocusMgr.setMouseCapture(nullptr);   // onMouseCaptureLost() ends the drag
                    return true;
                }
                return LLView::handleMouseUp(x, y, mask);
            }

            void onMouseCaptureLost() override
            {
                mDrag      = -1;
                mScrubbing = false;
            }

        private:
            static constexpr double MAX_DRAG_SECONDS = 600.0;   // what the path takes for one move
            static constexpr S32 PAD  = 16;
            static constexpr S32 BALL = 8;

            // The line sits above the numbers.
            static S32 bar(const LLRect& rect) { return rect.mBottom + 28; }

            // The seconds the line covers: the path's, at least a few so a short path is not a dot.
            double axisSeconds() const
            {
                return mDrag > 0 ? mAxis : std::max(camerapath::totalSeconds(), 2.0);
            }

            static S32 xOf(double t, double axis, S32 x0, S32 x1)
            {
                return x0 + (S32)std::lround((x1 - x0) * std::clamp(t / axis, 0.0, 1.0));
            }

            static double tickStep(double axis)
            {
                static const double steps[] = { 1, 2, 5, 10, 15, 30, 60, 120, 300 };
                for (double s : steps)
                {
                    if (axis / s <= 10.0)
                    {
                        return s;
                    }
                }
                return 600.0;
            }

            void scrubTo(S32 x)
            {
                const double seconds = std::clamp((double)(x - mX0) / std::max(1, mX1 - mX0), 0.0, 1.0) * mAxis;
                mScrubSeconds = std::min(seconds, camerapath::totalSeconds());
                std::string error;
                camerapath::previewAt(mScrubSeconds, error);
            }

            int         mDrag = -1;
            S32         mDragStartX = 0;
            double      mDragSeconds = 0.0;
            bool        mScrubbing = false;
            double      mScrubSeconds = 0.0;
            double      mAxis = 2.0;    // the seconds the line covers, fixed while something is dragged
            S32         mX0 = 0, mX1 = 1;
            std::string mLabel;
        };

        // --- the window -----------------------------------------------------------

        class CameraPathFloater : public LLFloater
        {
            friend class ::LLFloaterReg;

        public:
            bool postBuild() override
            {
                mSaved   = getChild<LLComboBox>("saved");
                mName    = getChild<LLLineEditor>("path_name");
                mLoop    = getChild<LLCheckBoxCtrl>("loop");
                mList    = getChild<LLScrollListCtrl>("points");
                mPointName = getChild<LLLineEditor>("name");
                mMotion  = getChild<LLComboBox>("motion");
                mSeconds = getChild<LLSpinCtrl>("seconds");
                mHold    = getChild<LLSpinCtrl>("hold");
                mSpin    = getChild<LLSpinCtrl>("spin");
                mOrbit   = getChild<LLSpinCtrl>("orbit");
                mSpinSeconds = getChild<LLSpinCtrl>("spin_seconds");
                mSubject = getChild<LLComboBox>("subject");
                mPart    = getChild<LLComboBox>("part");
                mShakeKind   = getChild<LLComboBox>("shake_kind");
                mShakeAmount = getChild<LLSliderCtrl>("shake_amount");
                mShakeSpeed  = getChild<LLSliderCtrl>("shake_speed");
                mShakeSeed   = getChild<LLSpinCtrl>("shake_seed");
                mCountdown   = getChild<LLSpinCtrl>("countdown");
                mShowRoute   = getChild<LLCheckBoxCtrl>("show_route");
                mFps     = getChild<LLComboBox>("export_fps");
                mTilt    = getChild<LLSliderCtrl>("tilt");
                mLevel   = getChild<LLButton>("btn_level");
                mChosenLabel = getChild<LLTextBox>("lbl_point");
                mPlay    = getChild<LLButton>("btn_play");
                mStop    = getChild<LLButton>("btn_stop");
                mTime    = getChild<LLTextBox>("time");
                mStatus  = getChild<LLTextBox>("status");
                mWarning = getChild<LLTextBox>("warning");

                // the timeline, in the panel the XUI keeps for it
                LLPanel* holder = getChild<LLPanel>("timeline_holder");
                TimelineView::Params timeline_params;
                timeline_params.name          = "timeline";
                timeline_params.rect          = holder->getLocalRect();
                timeline_params.follows.flags = FOLLOWS_ALL;
                if (TimelineView* timeline = LLUICtrlFactory::create<TimelineView>(timeline_params))
                {
                    holder->addChild(timeline);
                }

                // files
                getChild<LLButton>("btn_load")->setCommitCallback([this](LLUICtrl*, const LLSD&) { loadChosen(); });
                getChild<LLButton>("btn_delete_file")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
                    camerapath::removeSaved(mSaved->getValue().asString());
                    mFilesDirty = true;
                });
                getChild<LLButton>("btn_save")->setCommitCallback([this](LLUICtrl*, const LLSD&) { saveNamed(); });
                mName->setCommitCallback([this](LLUICtrl*, const LLSD&) { saveNamed(); });
                getChild<LLButton>("btn_new")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
                    camerapath::clearPath();
                    mFilesDirty = true;
                    setStatus(std::string());
                });
                mLoop->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    if (!mRefreshing)
                    {
                        camerapath::path().loop = ctrl->getValue().asBoolean();
                        camerapath::changed();
                    }
                });

                // points
                mList->setCommitCallback([this](LLUICtrl*, const LLSD&) {
                    camerapath::selectPoint(mList->getFirstSelectedIndex());
                });
                getChild<LLButton>("btn_add")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
                    std::string error;
                    setStatus(camerapath::addPoint(error) ? std::string() : error);
                });
                getChild<LLButton>("btn_overwrite")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
                    std::string error;
                    if (valid() && !camerapath::overwritePoint((size_t)camerapath::selectedPoint(), error))
                    {
                        setStatus(error);
                    }
                });
                getChild<LLButton>("btn_goto")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
                    std::string error;
                    if (valid() && !camerapath::goToPoint((size_t)camerapath::selectedPoint(), error))
                    {
                        setStatus(error);
                    }
                });
                getChild<LLButton>("btn_remove")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
                    if (valid())
                    {
                        camerapath::removePoint((size_t)camerapath::selectedPoint());
                    }
                });
                getChild<LLButton>("btn_up")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
                    if (valid())
                    {
                        camerapath::movePoint((size_t)camerapath::selectedPoint(), -1);
                    }
                });
                getChild<LLButton>("btn_down")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
                    if (valid())
                    {
                        camerapath::movePoint((size_t)camerapath::selectedPoint(), 1);
                    }
                });
                getChild<LLButton>("btn_copy")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
                    if (valid())
                    {
                        camerapath::duplicatePoint((size_t)camerapath::selectedPoint());
                    }
                });

                // the chosen point
                mPointName->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    if (valid() && !mRefreshing)
                    {
                        chosenPoint().name = ctrl->getValue().asString();
                        camerapath::changed();
                    }
                });
                mMotion->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    if (valid() && !mRefreshing)
                    {
                        chosenPoint().motion = static_cast<Motion>(llclamp(ctrl->getValue().asInteger(), 0, 3));
                        camerapath::changed();
                    }
                });
                mSeconds->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    if (valid() && !mRefreshing)
                    {
                        chosenPoint().seconds = ctrl->getValue().asReal();
                        camerapath::changed();
                    }
                });
                mHold->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    if (valid() && !mRefreshing)
                    {
                        chosenPoint().hold = ctrl->getValue().asReal();
                        camerapath::changed();
                    }
                });
                mOrbit->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    if (valid() && !mRefreshing)
                    {
                        chosenPoint().orbit = std::round(llclamp((float)ctrl->getValue().asReal(), -MAX_ORBIT, MAX_ORBIT));   // whole laps
                        camerapath::changed();
                    }
                });
                mSpin->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    if (valid() && !mRefreshing)
                    {
                        chosenPoint().spin = std::round(llclamp((float)ctrl->getValue().asReal(), -MAX_SPIN, MAX_SPIN));   // whole turns
                        camerapath::changed();
                    }
                });
                mSubject->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    if (valid() && !mRefreshing)
                    {
                        chosenPoint().subject = ctrl->getValue().asUUID();
                        camerapath::changed();
                    }
                });
                mPart->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    if (valid() && !mRefreshing)
                    {
                        chosenPoint().part = static_cast<plus::camerashot::Part>(llclamp(ctrl->getValue().asInteger(), 0, 2));
                        camerapath::changed();
                    }
                });
                getChild<LLButton>("btn_refresh_subjects")->setCommitCallback([this](LLUICtrl*, const LLSD&) { refreshSubjects(true); });

                // effects
                mShakeKind->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    if (!mRefreshing)
                    {
                        camerapath::path().shake.kind = static_cast<ShakeKind>(llclamp(ctrl->getValue().asInteger(), 0, 4));
                        camerapath::changed();
                    }
                });
                mShakeAmount->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    if (!mRefreshing)
                    {
                        camerapath::path().shake.amount = llclamp(ctrl->getValue().asReal(), 0.0, 1.0);
                        camerapath::changed();
                    }
                });
                mShakeSpeed->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    if (!mRefreshing)
                    {
                        camerapath::path().shake.speed = llclamp(ctrl->getValue().asReal(), 0.0, 1.0);
                        camerapath::changed();
                    }
                });
                mShakeSeed->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    if (!mRefreshing)
                    {
                        camerapath::path().shake.seed = (unsigned)llclamp(ctrl->getValue().asInteger(), 0, (S32)MAX_SEED);
                        camerapath::changed();
                    }
                });
                getChild<LLButton>("btn_shake_seed")->setCommitCallback([](LLUICtrl*, const LLSD&) {
                    camerapath::path().shake.seed = (unsigned)ll_rand((S32)MAX_SEED + 1);   // 0..MAX_SEED
                    camerapath::changed();
                });
                struct Strength { const char* name; double amount; };
                for (const Strength& strength : { Strength{ "btn_shake_weak", 0.25 }, Strength{ "btn_shake_medium", 0.5 },
                                                  Strength{ "btn_shake_strong", 1.0 } })
                {
                    const double amount = strength.amount;
                    getChild<LLButton>(strength.name)->setCommitCallback([amount](LLUICtrl*, const LLSD&) {
                        ShakeParams& shake = camerapath::path().shake;
                        shake.amount = amount;
                        if (shake.kind == ShakeKind::None)
                        {
                            shake.kind = ShakeKind::Handheld;   // a strength asked for means a shake is wanted
                        }
                        camerapath::changed();
                    });
                }
                for (const bool right : { true, false })
                {
                    getChild<LLButton>(right ? "btn_spin_right" : "btn_spin_left")->setCommitCallback([this, right](LLUICtrl*, const LLSD&) {
                        std::string error;
                        setStatus(camerapath::spinOnce(mSpinSeconds->getValue().asReal(), right, error) ? std::string() : error);
                    });
                }
                getChild<LLButton>("btn_spin_point")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
                    // The points stay as they are while the frames are taken.
                    if (camerapath::state() == State::Exporting)
                    {
                        return;
                    }
                    // The turn happens while the camera moves into the point: the first point has no move, and a cut
                    // (0 seconds) has no time to show it in.
                    if (!valid() || camerapath::selectedPoint() == 0)
                    {
                        setStatus("path_err_spin_point");
                        return;
                    }
                    camerapath::Point& point = chosenPoint();
                    if (point.spin >= MAX_SPIN)
                    {
                        setStatus("path_err_spin_max");
                        return;
                    }
                    point.spin = llclamp(point.spin + 1.f, -MAX_SPIN, MAX_SPIN);
                    if (point.seconds <= 0.0)
                    {
                        point.seconds = 3.0;
                    }
                    camerapath::changed();
                    setStatus(std::string());
                });
                getChild<LLButton>("btn_inplace")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
                    std::string error;
                    setStatus(camerapath::playInPlace(error) ? std::string("path_in_place") : error);
                });
                mCountdown->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&) {
                    if (!mRefreshing)
                    {
                        camerapath::path().countdown = llclamp(ctrl->getValue().asInteger(), 0, MAX_COUNTDOWN);
                        camerapath::changed();
                    }
                });
                mShowRoute->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) { camerapath::setShowRoute(ctrl->getValue().asBoolean()); });
                getChild<LLButton>("btn_export")->setCommitCallback([this](LLUICtrl*, const LLSD&) {
                    std::string error;
                    if (camerapath::startExport(mFps->getValue().asInteger(), error))
                    {
                        setStatus(std::string());
                    }
                    else
                    {
                        setStatus(error);
                    }
                });

                // the tilt of the camera now: the setting the photo tools and the keys write too (plus/camroll)
                mTilt->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) {
                    plus::camroll::setDegrees((F32)ctrl->getValue().asReal());
                });
                mLevel->setCommitCallback([this](LLUICtrl*, const LLSD&) {
                    plus::camroll::setDegrees(0.f);
                    mTiltKnown = false;   // the slider follows at the next frame
                });

                // play
                mPlay->setCommitCallback([this](LLUICtrl*, const LLSD&) {
                    std::string error;
                    setStatus(camerapath::play(error) ? std::string() : error);
                });
                mStop->setCommitCallback([](LLUICtrl*, const LLSD&) { camerapath::stop(); });

                mShowRoute->setValue(camerapath::showRoute());
                mFps->selectNthItem(1);   // 30 frames/s
                refresh();
                return true;
            }

            void onOpen(const LLSD& key) override
            {
                LLFloater::onOpen(key);
                camerapath::setEditorOpen(true);
                mFilesDirty = true;
                refresh();
            }

            void onClose(bool app_quitting) override
            {
                camerapath::setEditorOpen(false);
                LLFloater::onClose(app_quitting);
            }

            void draw() override
            {
                if (mShownRevision != camerapath::revision())
                {
                    refresh();
                }
                else if (mShownSelected != camerapath::selectedPoint())
                {
                    // only the chosen point moved (a click on the timeline): the list is left as it is
                    mShownSelected = camerapath::selectedPoint();
                    mRefreshing = true;
                    if (valid())
                    {
                        mList->selectNthItem(mShownSelected);
                    }
                    else
                    {
                        mList->deselectAllItems();
                    }
                    mRefreshing = false;
                    showSelected();
                    refreshSubjects(true);
                }
                updatePlayState();
                updateTilt();
                if (mRegionTimer.getElapsedTimeF32() > REGION_CHECK_SECONDS)
                {
                    mRegionTimer.reset();
                    updateWarning();
                }
                LLFloater::draw();
            }

        private:
            explicit CameraPathFloater(const LLSD& key) : LLFloater(key) {}

            bool valid() const
            {
                const int chosen = camerapath::selectedPoint();
                return chosen >= 0 && chosen < (int)camerapath::path().points.size();
            }

            camerapath::Point& chosenPoint()
            {
                return camerapath::path().points[(size_t)camerapath::selectedPoint()];
            }

            // The text of a refusal (a key of this window's strings), or the ready text for an empty key.
            void setStatus(const std::string& key)
            {
                const std::string text = getString(key.empty() ? "path_idle" : key);
                if (text != mStatusShown)
                {
                    mStatus->setText(text);
                    mStatusShown = text;
                }
            }

            std::string motionLabel(Motion motion)
            {
                switch (motion)
                {
                case Motion::Linear: return getString("list_motion_linear");
                case Motion::Ease:   return getString("list_motion_ease");
                case Motion::Orbit:  return getString("list_motion_orbit");
                default:             return getString("list_motion_smooth");
                }
            }

            static std::string tenths(double seconds)
            {
                return llformat("%.1f", seconds);
            }

            void loadChosen()
            {
                const std::string name = mSaved->getValue().asString();
                if (name.empty())
                {
                    return;
                }
                std::string error;
                mFilesDirty = true;
                setStatus(camerapath::load(name, error) ? std::string() : error);
            }

            void saveNamed()
            {
                std::string error;
                mFilesDirty = true;
                setStatus(camerapath::save(mName->getText(), error) ? std::string() : error);
            }

            // The avatars to look at: the one chosen, yours and those near the camera. The search of the world is
            // made only when asked (a new chosen point, the Find button, the window opening); an edit of the path
            // shows the list that was found.
            void refreshSubjects(bool search)
            {
                const bool was = mRefreshing;
                mRefreshing = true;
                if (search)
                {
                    mNearby = plus::camerashot::nearbySubjects(SUBJECT_RADIUS);
                }
                const LLUUID chosen = valid() ? chosenPoint().subject : LLUUID::null;
                mSubject->clearRows();
                mSubject->add(getString("subject_none"), LLSD(LLUUID::null));
                bool found = chosen.isNull();
                for (const plus::camerashot::Subject& subject : mNearby)
                {
                    std::string label = subject.name;
                    if (subject.id == gAgentID)
                    {
                        LLStringUtil::format_map_t args;
                        args["[NAME]"] = subject.name;
                        label          = getString("subject_me", args);
                    }
                    mSubject->add(label, LLSD(subject.id));
                    found = found || subject.id == chosen;
                }
                if (!found)
                {
                    mSubject->add(getString("subject_far"), LLSD(chosen));   // kept, so choosing another point does not lose it
                }
                if (!mSubject->setCurrentByID(chosen))
                {
                    mSubject->selectFirstItem();
                }
                mRefreshing = was;
            }

            // The list, the file list and the fields from the path.
            void refresh()
            {
                mRefreshing = true;
                mShownRevision = camerapath::revision();
                const bool selection_moved = mShownSelected != camerapath::selectedPoint();
                mShownSelected = camerapath::selectedPoint();
                const Path& open = camerapath::path();

                // files (a folder is read only after something that changes it, not at every edit of the path)
                if (mFilesDirty)
                {
                    mFilesDirty = false;
                    const std::string chosen = mSaved->getValue().asString();
                    mSaved->clearRows();
                    for (const std::string& name : camerapath::savedNames())
                    {
                        mSaved->add(name, LLSD(name));
                    }
                    if (!mSaved->setSimple(open.name) && !mSaved->setSimple(chosen))
                    {
                        mSaved->selectFirstItem();
                    }
                }
                if (open.name != mNameShown)
                {
                    // the name of the path opened or saved; what is typed meanwhile is left alone
                    mNameShown = open.name;
                    mName->setText(open.name);
                }
                mLoop->setValue(open.loop);

                // effects
                mShakeKind->setValue(LLSD((S32)open.shake.kind));
                mShakeAmount->setValue((F32)open.shake.amount);
                mShakeSpeed->setValue((F32)open.shake.speed);
                mShakeSeed->setValue((S32)open.shake.seed);
                mCountdown->setValue(open.countdown);

                // points
                const int count = (int)open.points.size();
                mList->deleteAllItems();
                for (int i = 0; i < count; ++i)
                {
                    const camerapath::Point& point = open.points[i];
                    LLStringUtil::format_map_t args;
                    args["[NUMBER]"] = std::to_string(i + 1);
                    LLSD row;
                    row["columns"][0]["column"] = "num";
                    row["columns"][0]["value"]  = std::to_string(i + 1);
                    row["columns"][1]["column"] = "point_name";
                    row["columns"][1]["value"]  = point.name.empty() ? getString("path_point_default", args) : point.name;
                    row["columns"][2]["column"] = "seconds";
                    row["columns"][3]["column"] = "motion";
                    row["columns"][4]["column"] = "hold";
                    if (i == 0)
                    {
                        row["columns"][2]["value"] = getString("path_start");
                    }
                    else if (point.seconds <= 0.0)
                    {
                        row["columns"][2]["value"] = getString("path_cut");
                    }
                    else
                    {
                        LLStringUtil::format_map_t seconds_args;
                        seconds_args["[SECONDS]"] = tenths(point.seconds);
                        row["columns"][2]["value"] = getString("path_seconds", seconds_args);
                        std::string move = motionLabel(point.motion);
                        if (point.motion == Motion::Orbit && point.orbit != 0.f)
                        {
                            LLStringUtil::format_map_t orbit_args;
                            orbit_args["[LAPS]"] = llformat("%+g", point.orbit);
                            move += " " + getString(std::fabs(point.orbit) == 1.f ? "list_orbit" : "list_orbits", orbit_args);
                        }
                        if (point.spin != 0.f)
                        {
                            LLStringUtil::format_map_t spin_args;
                            spin_args["[TURNS]"] = llformat("%+g", point.spin);
                            move += " " + getString(std::fabs(point.spin) == 1.f ? "list_spin" : "list_spins", spin_args);
                        }
                        row["columns"][3]["value"] = move;
                    }
                    if (point.hold > 0.0)
                    {
                        LLStringUtil::format_map_t hold_args;
                        hold_args["[SECONDS]"] = tenths(point.hold);
                        row["columns"][4]["value"] = getString("path_seconds", hold_args);
                    }
                    mList->addElement(row, ADD_BOTTOM);
                }
                if (valid())
                {
                    mList->selectNthItem(camerapath::selectedPoint());
                }
                mRefreshing = false;
                showSelected();
                refreshSubjects(selection_moved || mNearby.empty());

                if (mStatusShown.empty())
                {
                    setStatus(std::string());
                }
                mTimeShown = -1;   // the time text is made again
                mWarningShown.clear();
                updateWarning();
            }

            // The fields and buttons of the chosen point.
            void showSelected()
            {
                const bool was = mRefreshing;
                mRefreshing = true;
                const bool have  = valid();
                const bool first = have && camerapath::selectedPoint() == 0;
                const int  chosen = camerapath::selectedPoint();
                // the points stay as they are while the frames are taken (camerapath.cpp editLocked())
                const bool editable = have && camerapath::state() != State::Exporting;
                if (have)
                {
                    const camerapath::Point& point = chosenPoint();
                    mPointName->setText(point.name);
                    mMotion->setValue(LLSD((S32)point.motion));
                    mSeconds->setValue(point.seconds);
                    mHold->setValue(point.hold);
                    mSpin->setValue((F32)point.spin);
                    mOrbit->setValue((F32)point.orbit);
                    mPart->setValue(LLSD((S32)point.part));
                }
                else
                {
                    mPointName->setText(std::string());
                }
                mPointName->setEnabled(editable);
                mMotion->setEnabled(editable && !first);   // the first point has no move into it
                mSeconds->setEnabled(editable && !first);
                mHold->setEnabled(editable);
                mSpin->setEnabled(editable && !first);   // the move into the first point does not exist
                mOrbit->setEnabled(editable && !first && have && chosenPoint().motion == Motion::Orbit);   // the laps of an orbit move
                mSubject->setEnabled(editable);
                mPart->setEnabled(editable);
                // the title of the chosen point's part shows its number and the tilt it saved
                std::string title;
                if (have)
                {
                    LLStringUtil::format_map_t args;
                    args["[NUMBER]"]  = std::to_string(chosen + 1);
                    args["[DEGREES]"] = tenths(chosenPoint().roll);
                    title = getString("path_chosen", args);
                }
                else
                {
                    title = getString("path_chosen_none");
                }
                if (title != mChosenShown)
                {
                    mChosenShown = title;
                    mChosenLabel->setText(title);
                }
                const size_t points = camerapath::path().points.size();
                const bool exporting = camerapath::state() == State::Exporting;
                getChild<LLButton>("btn_overwrite")->setEnabled(editable);
                getChild<LLButton>("btn_goto")->setEnabled(editable);   // it would stop the frames
                getChild<LLButton>("btn_remove")->setEnabled(editable);
                getChild<LLButton>("btn_up")->setEnabled(editable && chosen > 0);
                getChild<LLButton>("btn_down")->setEnabled(editable && chosen + 1 < (int)points);
                getChild<LLButton>("btn_copy")->setEnabled(editable && points < MAX_POINTS);
                getChild<LLButton>("btn_spin_point")->setEnabled(editable && !first);   // a turn needs a move into the point
                getChild<LLButton>("btn_add")->setEnabled(!exporting && points < MAX_POINTS);
                // what shapes the frames stays as it is while they are taken
                for (LLUICtrl* ctrl : std::initializer_list<LLUICtrl*>{ mLoop, mShakeKind, mShakeAmount, mShakeSpeed, mShakeSeed, mCountdown,
                                                                        getChild<LLUICtrl>("btn_shake_weak"), getChild<LLUICtrl>("btn_shake_medium"),
                                                                        getChild<LLUICtrl>("btn_shake_strong"), getChild<LLUICtrl>("btn_shake_seed"),
                                                                        getChild<LLUICtrl>("btn_load"), getChild<LLUICtrl>("btn_new") })
                {
                    ctrl->setEnabled(!exporting);
                }
                const bool busy = camerapath::playing();
                const bool spinning = camerapath::spinning();
                mPlay->setEnabled(!busy && !spinning && points > 0);
                mStop->setEnabled(busy || spinning);
                for (const char* name : { "btn_spin_right", "btn_spin_left" })
                {
                    getChild<LLButton>(name)->setEnabled(!busy && !spinning);
                }
                mRefreshing = was;
            }

            // The time text and the Play / Stop buttons, when what plays changed.
            void updatePlayState()
            {
                const State now_state = camerapath::state();
                if (now_state != mStateShown)
                {
                    mStateShown = now_state;
                    showSelected();
                    mTimeShown = -1;
                    if (now_state == State::Idle || now_state == State::InPlace)
                    {
                        setStatus(now_state == State::InPlace ? "path_in_place" : std::string());
                    }
                }
                // one number says what the text shows: tenths of a second, the countdown, or the frame
                long long key = 0;
                int       frame = 0, frames = 0;
                switch (now_state)
                {
                case State::Playing:   key = (long long)(camerapath::time() * 10.0); break;
                case State::Counting:  key = camerapath::countdownLeft(); break;
                case State::Exporting: camerapath::exportProgress(frame, frames); key = frame; break;
                default: break;
                }
                key = key * 8 + (long long)now_state;
                if (key != mTimeShown)
                {
                    mTimeShown = key;
                    LLStringUtil::format_map_t args;
                    args["[TOTAL]"] = tenths(camerapath::totalSeconds());
                    const char* name = "path_total";
                    switch (now_state)
                    {
                    case State::Playing:
                        args["[TIME]"] = tenths(camerapath::time());
                        name = "path_playing";
                        break;
                    case State::Counting:
                        args["[NUMBER]"] = std::to_string(camerapath::countdownLeft());
                        name = "path_counting";
                        break;
                    case State::Exporting:
                        args["[FRAME]"]  = std::to_string(frame);
                        args["[FRAMES]"] = std::to_string(frames);
                        name = "path_exporting";
                        break;
                    default: break;
                    }
                    mTime->setText(getString(name, args));
                }
            }

            // The slider and the Level button follow the camera's tilt (it also changes by the photo tools and the keys),
            // and are off while a path holds the camera.
            void updateTilt()
            {
                const float tilt = std::remainder(plus::camroll::currentDegrees(), 360.f);   // 360 shows as 0 (the same picture)
                if (!mTiltKnown || std::fabs(tilt - mTiltShown) > 0.04f)
                {
                    mTiltKnown = true;
                    mTiltShown = tilt;
                    mTilt->setValue(tilt);
                    mLevel->setEnabled(mTiltEnabled && std::fabs(tilt) > 0.04f);
                }
                const bool enabled = !camerapath::playing() && !camerapath::spinning();
                if (enabled != mTiltEnabled)
                {
                    mTiltEnabled = enabled;
                    mTilt->setEnabled(enabled);
                    mLevel->setEnabled(enabled && std::fabs(tilt) > 0.04f);
                }
            }

            void updateWarning()
            {
                std::string text;
                if (camerapath::otherRegion())
                {
                    LLStringUtil::format_map_t args;
                    args["[REGION]"] = camerapath::path().region;
                    if (plus::rlv::enabled())
                    {
                        // RLVa @showloc: the region is not named (plus/rlv, research/248 3.4)
                        args["[REGION]"] = plus::rlv::regionNameForPlus(camerapath::path().region);
                    }
                    text = getString("path_other_region", args);
                }
                if (text != mWarningShown)
                {
                    mWarning->setText(text);
                    mWarningShown = text;
                }
            }

            LLComboBox*       mSaved = nullptr;
            LLLineEditor*     mName = nullptr;
            LLCheckBoxCtrl*   mLoop = nullptr;
            LLScrollListCtrl* mList = nullptr;
            LLLineEditor*     mPointName = nullptr;
            LLComboBox*       mMotion = nullptr;
            LLSpinCtrl*       mSeconds = nullptr;
            LLSpinCtrl*       mHold = nullptr;
            LLSpinCtrl*       mSpin = nullptr;
            LLSpinCtrl*       mOrbit = nullptr;
            LLSpinCtrl*       mSpinSeconds = nullptr;
            std::vector<plus::camerashot::Subject> mNearby;   // the avatars near the camera when last looked for
            LLComboBox*       mSubject = nullptr;
            LLComboBox*       mPart = nullptr;
            LLComboBox*       mShakeKind = nullptr;
            LLSliderCtrl*     mShakeAmount = nullptr;
            LLSliderCtrl*     mShakeSpeed = nullptr;
            LLSpinCtrl*       mShakeSeed = nullptr;
            LLSpinCtrl*       mCountdown = nullptr;
            LLCheckBoxCtrl*   mShowRoute = nullptr;
            LLComboBox*       mFps = nullptr;
            LLSliderCtrl*     mTilt = nullptr;
            LLButton*         mLevel = nullptr;
            LLTextBox*        mChosenLabel = nullptr;
            LLButton*         mPlay = nullptr;
            LLButton*         mStop = nullptr;
            LLTextBox*        mTime = nullptr;
            LLTextBox*        mStatus = nullptr;
            LLTextBox*        mWarning = nullptr;
            bool              mFilesDirty = true;
            bool              mRefreshing = false;   // the fields are being set from the path: their commits are not edits
            unsigned          mShownRevision = 0;
            int               mShownSelected = -2;
            State             mStateShown = State::Idle;
            long long         mTimeShown = -1;       // what the time text shows (see updatePlayState)
            // Not a NaN for "nothing shown yet": the build has /fp:fast (cmake/00-Common.cmake), where
            // comparisons with a NaN are not kept.
            bool              mTiltKnown = false;    // mTiltShown is what the slider shows
            float             mTiltShown = 0.f;      // the tilt the slider shows
            bool              mTiltEnabled = true;
            std::string       mChosenShown;
            std::string       mNameShown;
            std::string       mStatusShown;
            std::string       mWarningShown;
            LLFrameTimer      mRegionTimer;
        };

        void init()
        {
            LLFloaterReg::add(FLOATER_NAME, "floater_plus_camera_path.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<CameraPathFloater>);
        }

        plus::FeatureRegistration sRegistration({ "camerapath", &init, []()
                                                  {
                                                      attachOverlay();
                                                      attachBench();
                                                  } });
    }
}
