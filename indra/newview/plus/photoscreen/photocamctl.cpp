/**
 * @file photocamctl.cpp
 * @brief The camera controls of the photo screen's camera tab (photo-camera, research/260).
 *
 * Speed: nothing runs while the camera tab is not shown. Shown, the parts read what they show while they are
 * drawn: the zoom (LLAgentCamera::getCameraZoomFraction(), as the official camera controls read it every frame)
 * and the tilt (plus::camroll::currentDegrees(), a cached setting). The angle's text is made only when the whole
 * degrees change, the tilt buttons are enabled or disabled only when mouselook comes or goes.
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

#include "llviewerprecompiledheaders.h"

#include "photocamctl.h"

#include "plus/camroll/camroll.h"
#include "plus/photoscreen/psui/psstyle.h"
#include "plus/photoscreen/psui/pswidgets.h"

#include <cmath>
#include <functional>

#include "llagent.h"
#include "llagentcamera.h"
#include "llbutton.h"
#include "llfocusmgr.h"
#include "lljoystickbutton.h"
#include "llpanel.h"
#include "lluictrlfactory.h"

namespace plus::photoscreen::camctl
{
    namespace
    {
        using namespace plus::psui;

        // The pads and the zoom column (UI pixels at 1080; research/260, the layout part)
        constexpr S32 PAD_MIN     = 64;     // a pad is square: its side
        constexpr S32 PAD_MAX     = 104;
        constexpr S32 ZOOM_W      = 28;     // the zoom column: + and - buttons and the bar between them
        constexpr S32 ZOOM_BUTTON = 24;
        constexpr S32 ZOOM_BAR_GAP = 2;     // between the buttons and the bar
        constexpr S32 COLUMN_GAP  = 12;     // pad - zoom - pad
        constexpr S32 CAPTION_GAP = 4;
        constexpr S32 CAPTION_H   = 22;     // as a small label of the column (psui/psstack.h stack::LABEL_H)
        // The tilt row
        constexpr S32 TILT_BUTTON_W = 32;
        constexpr S32 TILT_VALUE_W  = 56;
        constexpr S32 TILT_GAP      = 8;
        constexpr S32 LEVEL_MIN_W   = 48;

        // As the official pads (lljoystickbutton.cpp, file constants there): the dot in the middle that puts the
        // camera back, and the dead zone around it where a held pad does not move it.
        constexpr S32 CENTER_DOT = 7;
        constexpr S32 DEAD_ZONE  = CENTER_DOT + 3;
        // As the official zoom buttons (llfloatercamera.cpp): a short press nudges, a held one speeds up.
        constexpr F32 NUDGE_TIME       = 0.25f;
        constexpr F32 ORBIT_NUDGE_RATE = 0.05f;
        // A key on the zoom bar or its buttons: this much of the zoom's range.
        constexpr F32 ZOOM_KEY_STEP = 0.05f;

        // The pads' drawing, inside a circle, relative to it
        constexpr F32 ARROW_DISTANCE = 0.7f;    // the arrows' centres from the middle
        constexpr F32 ORBIT_TRACK    = 0.5f;    // the orbit pad's inner ring
        constexpr F32 CROSS_LENGTH   = 0.5f;    // the pan pad's cross, each arm
        constexpr F32 MARK_WIDTH     = 1.5f;    // the inner ring and the cross's lines

        F32 orbitRate(F32 held_seconds)
        {
            return held_seconds < NUDGE_TIME ? ORBIT_NUDGE_RATE + held_seconds * (1.f - ORBIT_NUDGE_RATE) / NUDGE_TIME : 1.f;
        }

        // ---- the pads ---------------------------------------------------------------------------

        // Which ways a held pad is pushed (the arrows lit).
        struct Push
        {
            bool left = false, right = false, up = false, down = false;
        };

        struct PadIcons
        {
            LLPointer<LLUIImage> up, down, left, right;
            PadIcons()
            :   up(icon("chevron_up")), down(icon("chevron_down")), left(icon("chevron_left")), right(icon("chevron_right"))
            {
            }
        };

        // A pad: a round button with four arrows and the dot in the middle; the orbit pad has a ring inside, the
        // pan pad a cross.
        void drawPad(const LLView& view, const PadIcons& icons, bool cross, bool hover, bool centre_held,
                     const Push& push, F32 alpha)
        {
            const LLRectf r = localRectOf(view);
            const F32 radius = llmin(r.getWidth(), r.getHeight()) * 0.5f;
            const F32 cx = r.getCenterX();
            const F32 cy = r.getCenterY();
            const LLRectf disc(cx - radius, cy + radius, cx + radius, cy - radius);
            drawRoundBordered(disc, radius, Fill::solid(faded(hover ? style::BUTTON_FILL_HOVER : style::BUTTON_FILL, alpha)),
                              faded(hover ? style::BUTTON_BORDER_HOVER : style::BUTTON_BORDER, alpha));

            const LLColor4 mark = faded(style::SLIDER_EMPTY, alpha);
            if (cross)
            {
                const F32 arm = radius * CROSS_LENGTH;
                const F32 half = MARK_WIDTH * 0.5f;
                fillRect(LLRectf(cx - arm, cy + half, cx + arm, cy - half), Fill::solid(mark));
                fillRect(LLRectf(cx - half, cy + arm, cx + half, cy - arm), Fill::solid(mark));
            }
            else
            {
                const F32 ring = radius * ORBIT_TRACK;
                drawRing(LLRectf(cx - ring, cy + ring, cx + ring, cy - ring), ring, MARK_WIDTH, mark);
            }

            const F32 d = radius * ARROW_DISTANCE;
            const S32 size = style::HEAD_CHEVRON;
            auto arrow = [&](const LLPointer<LLUIImage>& image, F32 x, F32 y, bool lit) {
                drawIcon(image.get(), x, y, size, faded(lit ? style::ACCENT_LINE : style::TEXT_LABEL, alpha));
            };
            arrow(icons.up, cx, cy + d, push.up);
            arrow(icons.down, cx, cy - d, push.down);
            arrow(icons.left, cx - d, cy, push.left);
            arrow(icons.right, cx + d, cy, push.right);

            drawShadow(cx, cy, F32(CENTER_DOT), 0.35f * alpha);
            const Fill dot = centre_held ? Fill::solid(style::ACCENT) : Fill::vertical(style::KNOB_TOP, style::KNOB_BOTTOM);
            drawCircle(cx, cy, F32(CENTER_DOT), dot.faded(alpha));
        }

        // The official pad (LLJoystickCameraRotate or LLJoystickCameraTrack) with the photo screen's look.
        template <class Base, bool CROSS>
        class PsPad : public Base
        {
        public:
            struct Params : public LLInitParam::Block<Params, typename Base::Params>
            {
                Params()
                {
                    this->mouse_opaque(true);   // the official pads' template says false
                    this->tab_stop(false);      // the mouse only, as the official pads
                }
            };

            ~PsPad() override
            {
                if (this->hasMouseCapture())
                {
                    gAgent.setMovementLocked(false);    // deleted while held (the column made again)
                }
            }

            bool handleMouseDown(S32 x, S32 y, MASK mask) override
            {
                // The official pad locks the avatar's movement before it looks whether the press is on its circle, and
                // does not take a press outside it (so nothing would unlock it): only presses on the circle here.
                if (!this->pointInCircle(x, y))
                {
                    return false;
                }
                return Base::handleMouseDown(x, y, mask);
            }

            void onMouseCaptureLost() override
            {
                Base::onMouseCaptureLost();
                this->mInCenter = false;
                gAgent.setMovementLocked(false);    // released by the mouse up already, or taken away by another view
            }

            void onMouseEnter(S32 x, S32 y, MASK mask) override
            {
                Base::onMouseEnter(x, y, mask);
                mHover = true;
            }

            void onMouseLeave(S32 x, S32 y, MASK mask) override
            {
                Base::onMouseLeave(x, y, mask);
                mHover = false;
            }

            void draw() override
            {
                const bool held = this->hasMouseCapture();
                Push push;
                if (held && !this->mInCenter)
                {
                    // the ways onHeldDown() moves the camera now
                    const S32 dx = this->mLastMouse.mX - this->getRect().getWidth() / 2;
                    const S32 dy = this->mLastMouse.mY - this->getRect().getHeight() / 2;
                    push.left  = dx < -DEAD_ZONE;
                    push.right = dx > DEAD_ZONE;
                    push.up    = dy > DEAD_ZONE;
                    push.down  = dy < -DEAD_ZONE;
                }
                drawPad(*this, mIcons, CROSS, (mHover && this->getEnabled()) || held, held && this->mInCenter, push,
                        drawAlphaOf(*this));
            }

        protected:
            friend class LLUICtrlFactory;
            explicit PsPad(const Params& p) : Base(p) {}

        private:
            PadIcons mIcons;
            bool     mHover = false;
        };

        using PsOrbitPad = PsPad<LLJoystickCameraRotate, false>;
        using PsPanPad   = PsPad<LLJoystickCameraTrack, true>;

        // ---- a button that acts while it is held ----------------------------------------------------

        // The official button (its held-down timing: held_down_delay, then every frame) with an icon in the
        // photo screen's look. press: on the mouse going down; hold(seconds held): every frame after the delay;
        // key: Space or Return with the keyboard focus.
        class PsHoldButton : public LLButton
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLButton::Params>
            {
                Optional<std::string> icon;
                Optional<S32>         icon_size;
                Params()
                :   icon("icon"),
                    icon_size("icon_size", style::CHIP_ICON)
                {
                    tab_stop(true);
                }
            };

            void setActions(std::function<void()> press, std::function<void(F32)> hold, std::function<void()> key)
            {
                mPress = std::move(press);
                mHold  = std::move(hold);
                mKey   = std::move(key);
            }

            bool handleKeyHere(KEY key, MASK mask) override
            {
                if (getEnabled() && mask == MASK_NONE && (key == KEY_RETURN || key == ' '))
                {
                    if (mKey)
                    {
                        mKey();
                    }
                    return true;
                }
                return LLButton::handleKeyHere(key, mask);
            }

            void onMouseEnter(S32 x, S32 y, MASK mask) override
            {
                LLButton::onMouseEnter(x, y, mask);
                mHover = true;
            }

            void onMouseLeave(S32 x, S32 y, MASK mask) override
            {
                LLButton::onMouseLeave(x, y, mask);
                mHover = false;
            }

            void draw() override
            {
                const LLRectf r = localRectOf(*this);
                const F32 a = drawAlphaOf(*this);
                drawButtonBox(r, hasMouseCapture(), mHover && getEnabled(), a);
                drawIcon(mIcon.get(), r.getCenterX(), r.getCenterY(), mIconSize, faded(style::ICON_LIGHT, a));
                drawFocusRingOf(*this, r, style::BUTTON_RADIUS);
            }

        protected:
            friend class LLUICtrlFactory;
            explicit PsHoldButton(const Params& p)
            :   LLButton(p),
                mIcon(psui::icon(p.icon())),
                mIconSize(p.icon_size())
            {
                setMouseDownCallback([this](LLUICtrl*, const LLSD&) {
                    if (mPress)
                    {
                        mPress();
                    }
                });
                setHeldDownCallback([this](LLUICtrl*, const LLSD&) {
                    if (mHold)
                    {
                        mHold(getHeldDownTime());
                    }
                });
            }

        private:
            LLPointer<LLUIImage> mIcon;
            S32 mIconSize;
            bool mHover = false;
            std::function<void()>    mPress;
            std::function<void(F32)> mHold;
            std::function<void()>    mKey;
        };

        // ---- the zoom bar -----------------------------------------------------------------------

        // A vertical slider of the camera's zoom: the top is all the way in (getCameraZoomFraction() 1), the bottom
        // all the way out. It shows the zoom however it was changed (the wheel, the buttons, a camera position).
        class PsZoomBar : public PsCtrl
        {
        public:
            struct Params : public LLInitParam::Block<Params, PsCtrl::Params>
            {
                Params() { tab_stop(true); }
            };

            bool handleMouseDown(S32 x, S32 y, MASK mask) override
            {
                if (getEnabled())
                {
                    setFocus(true);
                    mDragging = true;
                    gFocusMgr.setMouseCapture(this);
                    zoomTo(fractionAt(y));
                }
                return true;
            }

            bool handleHover(S32 x, S32 y, MASK mask) override
            {
                if (mDragging && hasMouseCapture())
                {
                    zoomTo(fractionAt(y));
                    return true;
                }
                return PsCtrl::handleHover(x, y, mask);
            }

            bool handleMouseUp(S32 x, S32 y, MASK mask) override
            {
                if (hasMouseCapture())
                {
                    gFocusMgr.setMouseCapture(nullptr);
                }
                mDragging = false;
                return true;
            }

            void onMouseCaptureLost() override
            {
                PsCtrl::onMouseCaptureLost();
                mDragging = false;
            }

            bool handleKeyHere(KEY key, MASK mask) override
            {
                if (!getEnabled())
                {
                    return false;
                }
                switch (key)
                {
                    case KEY_UP:   case KEY_RIGHT: zoomBy(ZOOM_KEY_STEP);  return true;
                    case KEY_DOWN: case KEY_LEFT:  zoomBy(-ZOOM_KEY_STEP); return true;
                    case KEY_HOME: zoomTo(0.f); return true;
                    case KEY_END:  zoomTo(1.f); return true;
                    default:       return PsCtrl::handleKeyHere(key, mask);
                }
            }

            void draw() override
            {
                const F32 a = drawAlpha();
                const F32 cx = std::floor(F32(getRect().getWidth()) * 0.5f);
                const F32 bottom = trackBottom();
                const F32 top = trackTop();
                const F32 value = mDragging ? mDragValue : llclamp(gAgentCamera.getCameraZoomFraction(), 0.f, 1.f);
                const F32 ky = bottom + (top - bottom) * value;
                const F32 t2 = style::SLIDER_TRACK * 0.5f;
                drawRound(LLRectf(cx - t2, top, cx + t2, bottom), t2, Fill::solid(faded(style::SLIDER_EMPTY, a)));
                if (ky > bottom)
                {
                    drawRound(LLRectf(cx - t2, ky, cx + t2, bottom), t2,
                              Fill::vertical(faded(style::SLIDER_FILL_RIGHT, a), faded(style::SLIDER_FILL_LEFT, a)),
                              CORNERS_BOTTOM);
                }
                const F32 kr = (mDragging ? style::SLIDER_KNOB_PRESSED : style::SLIDER_KNOB) * 0.5f;
                drawShadow(cx, ky, kr, 0.35f * a);
                Fill knob = Fill::vertical(style::KNOB_TOP, style::KNOB_BOTTOM);
                if (mHover && getEnabled() && !mDragging)
                {
                    knob = knob.hovered();
                }
                drawCircle(cx, ky, kr, knob.faded(a));
                drawFocusRing(LLRectf(cx - kr, ky + kr, cx + kr, ky - kr), kr);
            }

        protected:
            friend class LLUICtrlFactory;
            explicit PsZoomBar(const Params& p) : PsCtrl(p) {}

        private:
            F32 trackBottom() const { return style::SLIDER_KNOB * 0.5f; }
            F32 trackTop() const { return F32(getRect().getHeight()) - style::SLIDER_KNOB * 0.5f; }

            F32 fractionAt(S32 y) const
            {
                const F32 span = trackTop() - trackBottom();
                return span > 0.f ? llclamp((F32(y) - trackBottom()) / span, 0.f, 1.f) : 0.f;
            }

            void zoomTo(F32 fraction)
            {
                mDragValue = llclamp(fraction, 0.f, 1.f);
                gAgentCamera.setCameraZoomFraction(mDragValue);     // as the official zoom slider
                if (mDragging)
                {
                    // setCameraZoomFraction() starts the camera's easing each time (the zoom time): called every frame of a
                    // drag, the camera is kept at the start of its ease and moves only once the button is let go. Followed at
                    // once while it is dragged; a key step (not dragging) keeps the ease.
                    gAgentCamera.stopCameraAnimation();
                }
            }

            void zoomBy(F32 delta)
            {
                zoomTo(gAgentCamera.getCameraZoomFraction() + delta);
            }

            bool mDragging = false;
            F32  mDragValue = 0.f;
        };

        // ---- the tilt row -------------------------------------------------------------------------

        // The tilt in whole degrees in a value box (as a slider's), read while it is drawn.
        class PsTiltValue : public PsCtrl
        {
        public:
            struct Params : public LLInitParam::Block<Params, PsCtrl::Params>
            {
                Params() { tab_stop(false); }
            };

            void draw() override
            {
                const S32 degrees = (S32)lroundf(plus::camroll::currentDegrees());
                if (degrees != mShown)
                {
                    mShown = degrees;
                    mText = utf8str_to_wstring(llformat("%d\xC2\xB0", degrees));    // the degree sign, UTF-8
                }
                const LLRectf r = localRectf();
                const F32 a = drawAlpha();
                drawRound(r, style::VALUE_RADIUS, Fill::solid(faded(style::VALUE_FILL, a)));
                drawText(mFont, mText, r.getCenterX(), r.getCenterY(), faded(style::TEXT_WHITE, a), LLFontGL::HCENTER,
                         LLFontGL::VCENTER, S32(r.getWidth()) - 2 * style::VALUE_PAD, true);
            }

        protected:
            friend class LLUICtrlFactory;
            explicit PsTiltValue(const Params& p) : PsCtrl(p), mFont(font(style::FONT_BODY)) {}

        private:
            const LLFontGL* mFont;
            LLWString mText;
            S32 mShown = S32_MIN;
        };

        // The row's panel: the tilt buttons are off in mouselook (as the official camera controls' tilt row,
        // camroll drawCameraControls()), switched only when that changes.
        class TiltRow : public LLPanel
        {
        public:
            struct Params : public LLInitParam::Block<Params, LLPanel::Params> {};

            void setButtons(LLView* left, LLView* right)
            {
                mLeft  = left ? left->getHandle() : LLHandle<LLView>();
                mRight = right ? right->getHandle() : LLHandle<LLView>();
            }

            void draw() override
            {
                const S32 enabled = gAgentCamera.cameraMouselook() ? 0 : 1;
                if (enabled != mEnabled)
                {
                    mEnabled = enabled;
                    for (const LLHandle<LLView>& handle : { mLeft, mRight })
                    {
                        if (LLView* button = handle.get())
                        {
                            button->setEnabled(enabled != 0);
                        }
                    }
                }
                LLPanel::draw();
            }

        protected:
            friend class LLUICtrlFactory;
            explicit TiltRow(const Params& p) : LLPanel(p) {}

        private:
            LLHandle<LLView> mLeft, mRight;
            S32 mEnabled = -1;
        };

        // ---- making them ------------------------------------------------------------------------

        // A widget with its name, rectangle and tip (its other params from its defaults).
        template <class T>
        typename T::Params paramsOf(const char* name, const LLRect& rect, const std::string& tip)
        {
            typename T::Params p;
            p.name = name;
            p.rect = rect;
            p.follows.flags = FOLLOWS_LEFT | FOLLOWS_TOP;
            if (!tip.empty())
            {
                p.tool_tip = tip;
            }
            return p;
        }

        template <class T>
        T* add(LLView* parent, typename T::Params& p)
        {
            T* view = LLUICtrlFactory::create<T>(p);
            if (view)
            {
                parent->addChild(view);
            }
            return view;
        }

        // The see-through panel the parts are put on (the clicks between them go through to the column).
        template <class T>
        T* makeHolder(const char* name, S32 width, S32 height)
        {
            typename T::Params p;
            p.name = name;
            p.rect = LLRect(0, height, width, 0);
            p.follows.flags = FOLLOWS_LEFT | FOLLOWS_TOP;
            p.background_visible = false;
            p.mouse_opaque = false;
            return LLUICtrlFactory::create<T>(p);
        }

        void addCaption(LLView* parent, const char* name, const std::string& text, S32 left, S32 width)
        {
            PsLabel::Params p = paramsOf<PsLabel>(name, LLRect(left, CAPTION_H, left + width, 0), std::string());
            p.label = text;
            p.kind  = "sub";
            p.align = "center";
            p.mouse_opaque = false;
            add<PsLabel>(parent, p);
        }

        // at_once: held from the first frame (the zoom buttons); otherwise after the official buttons' delay
        // (button.xml held_down_delay, as the official tilt buttons).
        PsHoldButton* addHoldButton(LLView* parent, const char* name, const char* icon_name, S32 icon_size,
                                    const LLRect& rect, const std::string& tip, bool at_once)
        {
            PsHoldButton::Params p = paramsOf<PsHoldButton>(name, rect, tip);
            p.icon = icon_name;
            p.icon_size = icon_size;
            if (at_once)
            {
                p.held_down_delay.seconds(0.f);
            }
            return add<PsHoldButton>(parent, p);
        }

        S32 padSide(S32 width)
        {
            return llclamp((width - ZOOM_W - 2 * COLUMN_GAP) / 2, PAD_MIN, PAD_MAX);
        }

        void zoomHeld(bool closer, F32 held_seconds)
        {
            // as the official zoom buttons (LLPanelCameraZoom::onZoomPlusHeldDown / onZoomMinusHeldDown)
            gAgentCamera.unlockView();
            if (closer)
            {
                gAgentCamera.setOrbitInKey(orbitRate(held_seconds));
            }
            else
            {
                gAgentCamera.setOrbitOutKey(orbitRate(held_seconds));
            }
        }

        void zoomKey(bool closer)
        {
            gAgentCamera.setCameraZoomFraction(
                llclamp(gAgentCamera.getCameraZoomFraction() + (closer ? ZOOM_KEY_STEP : -ZOOM_KEY_STEP), 0.f, 1.f));
        }

        void tiltStep(F32 direction)
        {
            plus::camroll::nudge(direction * plus::camroll::STEP_DEGREES);
        }

        void tiltHeld(F32 direction)
        {
            plus::camroll::nudge(direction * plus::camroll::HOLD_DEGREES_PER_SECOND * LLFrameTimer::getFrameDeltaTimeF32());
        }
    }

    S32 padsHeight(S32 width)
    {
        return padSide(width) + CAPTION_GAP + CAPTION_H;
    }

    LLView* makePads(S32 width, const Words& words)
    {
        const S32 side   = padSide(width);
        const S32 height = padsHeight(width);
        LLPanel* holder = makeHolder<LLPanel>("camera_pads", width, height);
        if (!holder)
        {
            return nullptr;
        }
        const S32 total = 2 * side + ZOOM_W + 2 * COLUMN_GAP;
        const S32 orbit_x = llmax(0, (width - total) / 2);
        const S32 zoom_x  = orbit_x + side + COLUMN_GAP;
        const S32 pan_x   = zoom_x + ZOOM_W + COLUMN_GAP;
        const S32 top     = height;
        const S32 bottom  = height - side;      // the pads' bottom; the captions under it

        PsOrbitPad::Params orbit = paramsOf<PsOrbitPad>("cam_rotate_stick", LLRect(orbit_x, top, orbit_x + side, bottom),
                                                        words.orbit_tip);
        add<PsOrbitPad>(holder, orbit);
        PsPanPad::Params pan = paramsOf<PsPanPad>("cam_track_stick", LLRect(pan_x, top, pan_x + side, bottom), words.pan_tip);
        add<PsPanPad>(holder, pan);

        // the zoom: + over the bar, - under it (held: on and on at once, as the official buttons)
        const S32 icon_size = style::HEAD_CHEVRON;
        if (PsHoldButton* in = addHoldButton(holder, "zoom_plus_btn", "plus", icon_size,
                                             LLRect(zoom_x, top, zoom_x + ZOOM_W, top - ZOOM_BUTTON), words.zoom_in_tip, true))
        {
            in->setActions(nullptr, [](F32 held) { zoomHeld(true, held); }, []() { zoomKey(true); });
        }
        if (PsHoldButton* out = addHoldButton(holder, "zoom_minus_btn", "minus", icon_size,
                                              LLRect(zoom_x, bottom + ZOOM_BUTTON, zoom_x + ZOOM_W, bottom), words.zoom_out_tip, true))
        {
            out->setActions(nullptr, [](F32 held) { zoomHeld(false, held); }, []() { zoomKey(false); });
        }
        PsZoomBar::Params bar = paramsOf<PsZoomBar>("zoom_slider",
                                                    LLRect(zoom_x, top - ZOOM_BUTTON - ZOOM_BAR_GAP, zoom_x + ZOOM_W,
                                                           bottom + ZOOM_BUTTON + ZOOM_BAR_GAP),
                                                    words.zoom_tip);
        add<PsZoomBar>(holder, bar);

        addCaption(holder, "orbit_caption", words.orbit, orbit_x, side);
        addCaption(holder, "zoom_caption", words.zoom, zoom_x - COLUMN_GAP, ZOOM_W + 2 * COLUMN_GAP);
        addCaption(holder, "pan_caption", words.pan, pan_x, side);
        return holder;
    }

    LLView* makeTiltRow(S32 width, const Words& words)
    {
        TiltRow* row = makeHolder<TiltRow>("camera_tilt", width, TILT_H);
        if (!row)
        {
            return nullptr;
        }
        S32 x = 0;
        // the official tilt buttons' timing: a step when pressed, then on and on after the button's delay
        PsHoldButton* left = addHoldButton(row, "roll_left_btn", "rotate_ccw", style::CHIP_ICON,
                                           LLRect(x, TILT_H, x + TILT_BUTTON_W, 0), words.tilt_left_tip, false);
        x += TILT_BUTTON_W + TILT_GAP;
        PsTiltValue::Params value = paramsOf<PsTiltValue>("roll_value", LLRect(x, TILT_H, x + TILT_VALUE_W, 0),
                                                          words.tilt_value_tip);
        add<PsTiltValue>(row, value);
        x += TILT_VALUE_W + TILT_GAP;
        PsHoldButton* right = addHoldButton(row, "roll_right_btn", "rotate_cw", style::CHIP_ICON,
                                            LLRect(x, TILT_H, x + TILT_BUTTON_W, 0), words.tilt_right_tip, false);
        x += TILT_BUTTON_W + TILT_GAP;
        if (left)
        {
            left->setActions([]() { tiltStep(-1.f); }, [](F32) { tiltHeld(-1.f); }, []() { tiltStep(-1.f); });
        }
        if (right)
        {
            right->setActions([]() { tiltStep(1.f); }, [](F32) { tiltHeld(1.f); }, []() { tiltStep(1.f); });
        }
        row->setButtons(left, right);

        PsButton::Params level = paramsOf<PsButton>("roll_level_btn", LLRect(x, TILT_H, llmax(x + LEVEL_MIN_W, width), 0),
                                                    words.level_tip);
        level.label = words.level;
        if (PsButton* button = add<PsButton>(row, level))
        {
            button->setCommitCallback([](LLUICtrl*, const LLSD&) { plus::camroll::setDegrees(0.f); });
        }
        return row;
    }
}
