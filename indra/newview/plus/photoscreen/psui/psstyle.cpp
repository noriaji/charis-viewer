/**
 * @file psstyle.cpp
 * @brief The photo screen's drawing helpers (research/221).
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

#include "psstyle.h"

#include "plus.h"

#include "llfontregistry.h"
#include "llframetimer.h"
#include "llkeyboard.h"
#include "llpanel.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llrootview.h"
#include "lltexture.h"
#include "lltimer.h"
#include "llui.h"
#include "lluictrl.h"
#include "lluiimage.h"
#include "llview.h"
#include "llviewerwindow.h"

#include <cmath>

namespace plus::psui
{
    namespace
    {
        // The masks (make_ps_images.py): a disc filling the image; its outside; a soft disc.
        constexpr char IMAGE_ROUND[]  = "Plus_PS_Round";
        constexpr char IMAGE_CIRCLE[] = "Plus_PS_Circle";
        constexpr char IMAGE_CUT[]    = "Plus_PS_Cut";
        constexpr char IMAGE_SHADOW[] = "Plus_PS_Shadow";
        constexpr char ICON_PREFIX[]  = "Plus_PS_Icon_";

        // From this radius on, circles and round corners take the finer 128 pixel disc.
        constexpr F32 LARGE_RADIUS = 20.f;

        // Points on each corner of an outline (drawRing, drawPicture).
        constexpr int ARC_SEGMENTS = 8;
        constexpr int OUTLINE_POINTS = 4 * (ARC_SEGMENTS + 1);

        // The icons of make_ps_images.py (ps_icon_<name>.png).
        constexpr const char* ICON_NAMES[] = {
            "scene", "avatar", "environment", "lighting", "pose", "camera", "effects", "presets",
            "clapperboard", "sliders", "user", "aperture", "sun", "palette", "output",
            "film", "resolution", "ratio", "grid",
            "focus", "frame", "maximize", "scan_face", "flycam",
            "chevron_down", "chevron_up", "more", "plus", "settings", "check", "close",
            "star", "star_filled", "water", "quality",
            "message", "send",
            "chevron_left", "chevron_right", "rotate_ccw", "rotate_cw", "minus",
        };

        // The masks are held by the UI image list (which lives until the UI is torn down, after
        // the last draw): kept here as plain pointers, looked up until all are there. The widgets
        // are made after login (the photo screen, the parts sample), when plus has preloaded them.
        struct Masks
        {
            const LLUIImage* round  = nullptr;
            const LLUIImage* circle = nullptr;
            const LLUIImage* cut    = nullptr;
            const LLUIImage* shadow = nullptr;
        };

        const Masks& masks()
        {
            static Masks m;
            if (!m.round || !m.circle || !m.cut || !m.shadow)
            {
                m.round  = LLUI::getUIImage(IMAGE_ROUND).get();
                m.circle = LLUI::getUIImage(IMAGE_CIRCLE).get();
                m.cut    = LLUI::getUIImage(IMAGE_CUT).get();
                m.shadow = LLUI::getUIImage(IMAGE_SHADOW).get();
            }
            return m;
        }

        // Keys that move the keyboard focus or press what has it: they show the focus ring (focusRingVisible()).
        bool movesFocus(KEY key)
        {
            switch (key)
            {
            case KEY_TAB:
            case KEY_LEFT:
            case KEY_RIGHT:
            case KEY_UP:
            case KEY_DOWN:
            case KEY_HOME:
            case KEY_END:
            case KEY_PAGE_UP:
            case KEY_PAGE_DOWN:
            case KEY_RETURN:
            case KEY_PAD_RETURN:
            case ' ':
                return true;
            default:
                return false;
            }
        }

        // focusRingVisible()'s state: one for the whole UI, as the keyboard focus is.
        struct FocusRing
        {
            bool   visible = false;
            U32    frame = U32_MAX;     // the frame it was last looked at in
            LLTimer sinceLook;          // made at the first look: keys pressed before it do not count
        };
        FocusRing& focusRing()
        {
            static FocusRing ring;
            return ring;
        }

        bool bindImage(const LLUIImage* image)
        {
            if (!image || image->getImage().isNull())
            {
                return false;
            }
            gGL.getTexUnit(0)->bind(image->getImage().get());
            return true;
        }

        // The colour of fill at (s, t): s from the left, t from the bottom, 0..1.
        LLColor4 colorAt(const Fill& f, F32 s, F32 t)
        {
            s = llclamp(s, 0.f, 1.f);
            t = llclamp(t, 0.f, 1.f);
            const LLColor4 bottom = lerp(f.bl, f.br, s);
            const LLColor4 top    = lerp(f.tl, f.tr, s);
            return lerp(bottom, top, t);
        }

        inline void vertex(F32 x, F32 y, F32 u, F32 v, const LLColor4& c)
        {
            gGL.color4fv(c.mV);
            gGL.texCoord2f(u, v);
            gGL.vertex2f(x, y);
        }

        // One quad: corners in the order bottom-left, bottom-right, top-right, top-left.
        inline void quad(const F32 (&x)[4], const F32 (&y)[4], const F32 (&u)[4], const F32 (&v)[4], const LLColor4 (&c)[4])
        {
            vertex(x[0], y[0], u[0], v[0], c[0]);
            vertex(x[1], y[1], u[1], v[1], c[1]);
            vertex(x[2], y[2], u[2], v[2], c[2]);
            vertex(x[0], y[0], u[0], v[0], c[0]);
            vertex(x[2], y[2], u[2], v[2], c[2]);
            vertex(x[3], y[3], u[3], v[3], c[3]);
        }

        // A rounded rectangle from a disc mask drawn in nine pieces: the corners are the disc's
        // quarters at the size of the radius, the edges its middle lines, the inside its centre.
        void ninePatch(const LLUIImage* mask, const LLRectf& r, F32 radius, const Fill& fill, U8 corners)
        {
            const F32 w = r.getWidth();
            const F32 h = r.getHeight();
            if (w <= 0.f || h <= 0.f)
            {
                return;
            }
            radius = llclamp(radius, 0.f, llmin(w, h) * 0.5f);
            if (radius < 0.5f || corners == CORNERS_NONE || !bindImage(mask))
            {
                fillRect(r, fill);
                return;
            }

            const F32 xs[4] = { r.mLeft, r.mLeft + radius, r.mRight - radius, r.mRight };
            const F32 ys[4] = { r.mBottom, r.mBottom + radius, r.mTop - radius, r.mTop };
            constexpr F32 UV[4] = { 0.f, 0.5f, 0.5f, 1.f };
            LLColor4 col[4][4];     // [x][y]
            for (int i = 0; i < 4; ++i)
            {
                for (int j = 0; j < 4; ++j)
                {
                    col[i][j] = colorAt(fill, (xs[i] - r.mLeft) / w, (ys[j] - r.mBottom) / h);
                }
            }

            gGL.begin(LLRender::TRIANGLES);
            for (int j = 0; j < 3; ++j)
            {
                for (int i = 0; i < 3; ++i)
                {
                    if (xs[i + 1] - xs[i] <= 0.f || ys[j + 1] - ys[j] <= 0.f)
                    {
                        continue;   // a pill has no middle row (or column)
                    }
                    F32 u0 = UV[i], u1 = UV[i + 1], v0 = UV[j], v1 = UV[j + 1];
                    if (i != 1 && j != 1)
                    {
                        const U8 corner = (j == 2) ? (i == 0 ? CORNER_TL : CORNER_TR) : (i == 0 ? CORNER_BL : CORNER_BR);
                        if (!(corners & corner))
                        {
                            u0 = u1 = v0 = v1 = 0.5f;   // a square corner: the inside of the disc
                        }
                    }
                    const F32 x[4] = { xs[i], xs[i + 1], xs[i + 1], xs[i] };
                    const F32 y[4] = { ys[j], ys[j], ys[j + 1], ys[j + 1] };
                    const F32 u[4] = { u0, u1, u1, u0 };
                    const F32 v[4] = { v0, v0, v1, v1 };
                    const LLColor4 c[4] = { col[i][j], col[i + 1][j], col[i + 1][j + 1], col[i][j + 1] };
                    quad(x, y, u, v, c);
                }
            }
            gGL.end();
        }

        // The outline of a rounded rectangle, OUTLINE_POINTS points counter-clockwise from the
        // bottom-right corner; a square corner repeats its point, so outlines of different sizes
        // have matching points (drawRing joins them as strips).
        void outline(const LLRectf& r, F32 radius, U8 corners, F32 (&px)[OUTLINE_POINTS], F32 (&py)[OUTLINE_POINTS])
        {
            radius = llclamp(radius, 0.f, llmax(0.f, llmin(r.getWidth(), r.getHeight()) * 0.5f));
            struct Corner { U8 flag; F32 cx, cy, sx, sy; F32 start; };
            const Corner list[4] = {
                { CORNER_BR, r.mRight - radius, r.mBottom + radius, r.mRight, r.mBottom, -F_PI_BY_TWO },
                { CORNER_TR, r.mRight - radius, r.mTop - radius,    r.mRight, r.mTop,    0.f },
                { CORNER_TL, r.mLeft + radius,  r.mTop - radius,    r.mLeft,  r.mTop,    F_PI_BY_TWO },
                { CORNER_BL, r.mLeft + radius,  r.mBottom + radius, r.mLeft,  r.mBottom, F_PI },
            };
            int n = 0;
            for (const Corner& c : list)
            {
                const bool round = (corners & c.flag) && radius >= 0.5f;
                for (int k = 0; k <= ARC_SEGMENTS; ++k)
                {
                    if (round)
                    {
                        const F32 a = c.start + F_PI_BY_TWO * F32(k) / F32(ARC_SEGMENTS);
                        px[n] = c.cx + radius * cosf(a);
                        py[n] = c.cy + radius * sinf(a);
                    }
                    else
                    {
                        px[n] = c.sx;
                        py[n] = c.sy;
                    }
                    ++n;
                }
            }
        }

        LLRectf inset(const LLRectf& r, F32 d)
        {
            return LLRectf(r.mLeft + d, r.mTop - d, r.mRight - d, r.mBottom + d);
        }

        // Rounded corner shapes with radius r grown or shrunk by d keep their centres.
        F32 insetRadius(F32 radius, F32 d)
        {
            return llmax(0.f, radius - d);
        }
    }

    const LLFontGL* font(const style::FontSpec& spec)
    {
        const LLFontGL* f = LLFontGL::getFont(LLFontDescriptor("SansSerif", spec.size, spec.style));
        return f ? f : LLFontGL::getFontSansSerif();
    }

    U8 parseCorners(const std::string& text)
    {
        if (text.empty() || text == "all")  return CORNERS_ALL;
        if (text == "none")                 return CORNERS_NONE;
        if (text == "top")                  return CORNERS_TOP;
        if (text == "bottom")               return CORNERS_BOTTOM;
        if (text == "left")                 return CORNERS_LEFT;
        if (text == "right")                return CORNERS_RIGHT;
        U8 out = CORNERS_NONE;
        if (text.find("tl") != std::string::npos) out |= CORNER_TL;
        if (text.find("tr") != std::string::npos) out |= CORNER_TR;
        if (text.find("bl") != std::string::npos) out |= CORNER_BL;
        if (text.find("br") != std::string::npos) out |= CORNER_BR;
        return out;
    }

    LLColor4 hovered(const LLColor4& c)
    {
        LLColor4 out = lerp(c, LLColor4::white, style::STATE_HOVER);
        out.mV[VALPHA] = c.mV[VALPHA];
        return out;
    }

    LLColor4 pressed(const LLColor4& c)
    {
        LLColor4 out = lerp(c, LLColor4::black, style::STATE_PRESS);
        out.mV[VALPHA] = c.mV[VALPHA];
        return out;
    }

    LLColor4 faded(const LLColor4& c, F32 alpha)
    {
        return LLColor4(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], c.mV[VALPHA] * alpha);
    }

    Fill Fill::faded(F32 alpha) const
    {
        return { psui::faded(tl, alpha), psui::faded(tr, alpha), psui::faded(bl, alpha), psui::faded(br, alpha) };
    }

    Fill Fill::hovered() const
    {
        return { psui::hovered(tl), psui::hovered(tr), psui::hovered(bl), psui::hovered(br) };
    }

    Fill Fill::pressed() const
    {
        return { psui::pressed(tl), psui::pressed(tr), psui::pressed(bl), psui::pressed(br) };
    }

    void fillRect(const LLRectf& r, const Fill& fill)
    {
        if (r.getWidth() <= 0.f || r.getHeight() <= 0.f)
        {
            return;
        }
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        const F32 x[4] = { r.mLeft, r.mRight, r.mRight, r.mLeft };
        const F32 y[4] = { r.mBottom, r.mBottom, r.mTop, r.mTop };
        const F32 u[4] = { 0.f, 0.f, 0.f, 0.f };
        const LLColor4 c[4] = { fill.bl, fill.br, fill.tr, fill.tl };
        gGL.begin(LLRender::TRIANGLES);
        quad(x, y, u, u, c);
        gGL.end();
    }

    void drawRound(const LLRectf& r, F32 radius, const Fill& fill, U8 corners)
    {
        ninePatch(radius >= LARGE_RADIUS ? masks().circle : masks().round, r, radius, fill, corners);
    }

    void drawRoundBordered(const LLRectf& r, F32 radius, const Fill& fill, const LLColor4& border, F32 border_width, U8 corners)
    {
        if (border.mV[VALPHA] <= 0.f || border_width <= 0.f)
        {
            drawRound(r, radius, fill, corners);
            return;
        }
        const bool opaque = fill.tl.mV[VALPHA] >= 0.999f && fill.tr.mV[VALPHA] >= 0.999f
                            && fill.bl.mV[VALPHA] >= 0.999f && fill.br.mV[VALPHA] >= 0.999f;
        const LLRectf inner = inset(r, border_width);
        const F32 inner_radius = insetRadius(radius, border_width);
        if (opaque)
        {
            drawRound(r, radius, Fill::solid(border), corners);
            drawRound(inner, inner_radius, fill, corners);
        }
        else
        {
            drawRound(inner, inner_radius, fill, corners);
            drawRing(r, radius, border_width, border, corners);
        }
    }

    void drawRing(const LLRectf& r, F32 radius, F32 width, const LLColor4& color, U8 corners)
    {
        if (width <= 0.f || color.mV[VALPHA] <= 0.f || r.getWidth() <= 0.f || r.getHeight() <= 0.f)
        {
            return;
        }
        // Four outlines: half a pixel outside the edge (clear), half a pixel inside it (full), half
        // a pixel inside the inner edge (full) and half a pixel beyond it (clear). A line thinner
        // than a pixel has its two middle outlines at its middle and a weaker colour.
        const F32 peak = llmin(width, 1.f);
        const F32 in1 = width >= 1.f ? 0.5f : width * 0.5f;
        const F32 in2 = width >= 1.f ? width - 0.5f : width * 0.5f;
        const F32 offsets[4] = { -0.5f, in1, in2, width + 0.5f };
        const F32 alphas[4] = { 0.f, peak, peak, 0.f };

        static thread_local F32 px[4][OUTLINE_POINTS];
        static thread_local F32 py[4][OUTLINE_POINTS];
        for (int l = 0; l < 4; ++l)
        {
            outline(inset(r, offsets[l]), insetRadius(radius, offsets[l]), corners, px[l], py[l]);
        }

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin(LLRender::TRIANGLES);
        for (int l = 0; l < 3; ++l)
        {
            const LLColor4 ca = faded(color, alphas[l]);
            const LLColor4 cb = faded(color, alphas[l + 1]);
            for (int k = 0; k < OUTLINE_POINTS; ++k)
            {
                const int k1 = (k + 1) % OUTLINE_POINTS;
                const F32 x[4] = { px[l][k], px[l][k1], px[l + 1][k1], px[l + 1][k] };
                const F32 y[4] = { py[l][k], py[l][k1], py[l + 1][k1], py[l + 1][k] };
                const F32 u[4] = { 0.f, 0.f, 0.f, 0.f };
                const LLColor4 c[4] = { ca, ca, cb, cb };
                quad(x, y, u, u, c);
            }
        }
        gGL.end();
    }

    void drawCircle(F32 cx, F32 cy, F32 radius, const Fill& fill)
    {
        drawRound(LLRectf(cx - radius, cy + radius, cx + radius, cy - radius), radius, fill, CORNERS_ALL);
    }

    void drawButtonBox(const LLRectf& r, bool pressed, bool hovered, F32 alpha)
    {
        LLColor4 fill = style::BUTTON_FILL;
        LLColor4 border = style::BUTTON_BORDER;
        if (pressed)
        {
            fill = style::BUTTON_FILL_PRESS;
        }
        else if (hovered)
        {
            fill = style::BUTTON_FILL_HOVER;
            border = style::BUTTON_BORDER_HOVER;
        }
        drawRoundBordered(r, style::BUTTON_RADIUS, Fill::solid(faded(fill, alpha)), faded(border, alpha));
    }

    LLRectf localRectOf(const LLView& view)
    {
        return LLRectf(0.f, F32(view.getRect().getHeight()), F32(view.getRect().getWidth()), 0.f);
    }

    F32 drawAlphaOf(const LLView& view)
    {
        return view.getDrawContext().mAlpha * (view.getEnabled() ? 1.f : style::STATE_DISABLED);
    }

    bool focusRingVisible()
    {
        FocusRing& ring = focusRing();
        const U32 frame = LLFrameTimer::getFrameCount();
        if (frame == ring.frame)
        {
            return ring.visible;
        }
        ring.frame = frame;
        const F64 since_look = ring.sinceLook.getElapsedTimeAndResetF64();
        if (gViewerWindow && (gViewerWindow->getLeftMouseDown() || gViewerWindow->getRightMouseDown() ||
                              gViewerWindow->getMiddleMouseDown()))
        {
            ring.visible = false;
        }
        else if (gKeyboard)
        {
            // The key last pressed, if that was since the last look (other keys, typing, leave it as it is).
            const KEY key = gKeyboard->currentKey();
            if (movesFocus(key) && gKeyboard->getKeyElapsedTime(key) <= since_look)
            {
                ring.visible = true;
            }
        }
        return ring.visible;
    }

    void drawFocusRingOf(const LLUICtrl& ctrl, const LLRectf& r, F32 radius)
    {
        const bool keys = focusRingVisible();   // first: it watches the mouse every frame the widgets are drawn
        if (ctrl.hasFocus() && ctrl.getEnabled() && (keys || ctrl.acceptsTextInput()))
        {
            const LLRectf ring(r.mLeft - 2.f, r.mTop + 2.f, r.mRight + 2.f, r.mBottom - 2.f);
            drawRing(ring, radius + 2.f, 1.f, faded(style::FOCUS_RING, ctrl.getDrawContext().mAlpha));
        }
    }

    void drawUnderlineTab(const LLFontGL* f, const LLWString& label, const LLRectf& r, bool chosen, bool hovered,
                          F32 alpha)
    {
        const LLColor4 text = chosen ? style::TAB_TEXT_ON : (hovered ? style::TEXT_LABEL : style::TEXT_DIM);
        drawText(f, label, r.getCenterX(), r.getCenterY() + 2.f, faded(text, alpha), LLFontGL::HCENTER, LLFontGL::VCENTER,
                 S32(r.getWidth()), true);
        if (!chosen)
        {
            return;
        }
        const F32 line_y = r.mBottom;
        const F32 x0 = r.mLeft + style::TAB_LINE_INSET;
        const F32 x1 = r.mRight - style::TAB_LINE_INSET;
        const F32 xm = (x0 + x1) * 0.5f;
        const LLColor4 edge = faded(style::TAB_LINE_EDGE, alpha);
        const LLColor4 middle = faded(style::TAB_LINE_MID, alpha);
        // The glow under the line, then the line (bright in the middle).
        fillRect(LLRectf(x0, line_y, x1, line_y - style::TAB_GLOW),
                 Fill::vertical(faded(style::TAB_GLOW_COLOR, alpha), LLColor4(style::TAB_GLOW_COLOR.mV[VRED],
                                style::TAB_GLOW_COLOR.mV[VGREEN], style::TAB_GLOW_COLOR.mV[VBLUE], 0.f)));
        fillRect(LLRectf(x0, line_y + style::TAB_LINE, xm, line_y), Fill::horizontal(edge, middle));
        fillRect(LLRectf(xm, line_y + style::TAB_LINE, x1, line_y), Fill::horizontal(middle, edge));
    }

    void drawShadow(F32 cx, F32 cy, F32 radius, F32 alpha, F32 drop)
    {
        if (!bindImage(masks().shadow))
        {
            return;
        }
        // The soft disc is half covered at 3/4 of the image's half size.
        const F32 half = radius / 0.75f;
        const LLRectf r(cx - half, cy - drop + half, cx + half, cy - drop - half);
        const F32 x[4] = { r.mLeft, r.mRight, r.mRight, r.mLeft };
        const F32 y[4] = { r.mBottom, r.mBottom, r.mTop, r.mTop };
        const F32 u[4] = { 0.f, 1.f, 1.f, 0.f };
        const F32 v[4] = { 0.f, 0.f, 1.f, 1.f };
        const LLColor4 black(0.f, 0.f, 0.f, alpha);
        const LLColor4 c[4] = { black, black, black, black };
        gGL.begin(LLRender::TRIANGLES);
        quad(x, y, u, v, c);
        gGL.end();
    }

    void drawCornerCuts(const LLRectf& r, F32 radius, const LLColor4& color, U8 corners)
    {
        radius = llclamp(radius, 0.f, llmin(r.getWidth(), r.getHeight()) * 0.5f);
        if (radius < 0.5f || corners == CORNERS_NONE || !bindImage(masks().cut))
        {
            return;
        }
        struct Cell { U8 flag; F32 x0, y0; F32 u0, v0; };
        // Each corner's cell: its bottom-left point and the mask's quarter (the same quarters as
        // ninePatch()).
        const Cell cells[4] = {
            { CORNER_BL, r.mLeft,           r.mBottom,         0.f,  0.f },
            { CORNER_BR, r.mRight - radius, r.mBottom,         0.5f, 0.f },
            { CORNER_TR, r.mRight - radius, r.mTop - radius,   0.5f, 0.5f },
            { CORNER_TL, r.mLeft,           r.mTop - radius,   0.f,  0.5f },
        };
        const LLColor4 c[4] = { color, color, color, color };
        gGL.begin(LLRender::TRIANGLES);
        for (const Cell& cell : cells)
        {
            if (!(corners & cell.flag))
            {
                continue;
            }
            const F32 x[4] = { cell.x0, cell.x0 + radius, cell.x0 + radius, cell.x0 };
            const F32 y[4] = { cell.y0, cell.y0, cell.y0 + radius, cell.y0 + radius };
            const F32 u[4] = { cell.u0, cell.u0 + 0.5f, cell.u0 + 0.5f, cell.u0 };
            const F32 v[4] = { cell.v0, cell.v0, cell.v0 + 0.5f, cell.v0 + 0.5f };
            quad(x, y, u, v, c);
        }
        gGL.end();
    }

    void drawIcon(const LLUIImage* image, F32 cx, F32 cy, S32 size, const LLColor4& color)
    {
        if (!image || size <= 0)
        {
            return;
        }
        const S32 x = ll_round(cx - F32(size) * 0.5f);
        const S32 y = ll_round(cy - F32(size) * 0.5f);
        image->draw(x, y, size, size, color);
    }

    void drawPicture(LLTexture* texture, const LLRectf& r, F32 radius, const LLColor4& color, U8 corners, PictureFit fit)
    {
        if (!texture || r.getWidth() <= 0.f || r.getHeight() <= 0.f)
        {
            return;
        }
        const S32 tw = texture->getWidth(0);
        const S32 th = texture->getHeight(0);
        if (tw <= 0 || th <= 0)
        {
            return;
        }
        // Crop: cut the picture to the rectangle's shape, keeping its middle. Stretch: the whole picture.
        const F32 tex_aspect = F32(tw) / F32(th);
        const F32 rect_aspect = r.getWidth() / r.getHeight();
        F32 u0 = 0.f, u1 = 1.f, v0 = 0.f, v1 = 1.f;
        if (fit == PictureFit::Crop)
        {
            if (tex_aspect > rect_aspect)
            {
                const F32 keep = rect_aspect / tex_aspect;
                u0 = (1.f - keep) * 0.5f;
                u1 = u0 + keep;
            }
            else
            {
                const F32 keep = tex_aspect / rect_aspect;
                v0 = (1.f - keep) * 0.5f;
                v1 = v0 + keep;
            }
        }

        static thread_local F32 px[OUTLINE_POINTS];
        static thread_local F32 py[OUTLINE_POINTS];
        outline(r, radius, corners, px, py);

        auto uOf = [&](F32 x) { return u0 + (u1 - u0) * (x - r.mLeft) / r.getWidth(); };
        auto vOf = [&](F32 y) { return v0 + (v1 - v0) * (y - r.mBottom) / r.getHeight(); };
        const F32 cx = r.getCenterX();
        const F32 cy = r.getCenterY();

        gGL.getTexUnit(0)->bind(texture);
        gGL.begin(LLRender::TRIANGLES);
        for (int k = 0; k < OUTLINE_POINTS; ++k)
        {
            const int k1 = (k + 1) % OUTLINE_POINTS;
            if (px[k] == px[k1] && py[k] == py[k1])
            {
                continue;   // the repeated point of a square corner
            }
            vertex(cx, cy, uOf(cx), vOf(cy), color);
            vertex(px[k], py[k], uOf(px[k]), vOf(py[k]), color);
            vertex(px[k1], py[k1], uOf(px[k1]), vOf(py[k1]), color);
        }
        gGL.end();
    }

    void drawText(const LLFontGL* f, const LLWString& text, F32 x, F32 y, const LLColor4& color,
                  LLFontGL::HAlign halign, LLFontGL::VAlign valign, S32 max_pixels, bool ellipses)
    {
        if (!f || text.empty())
        {
            return;
        }
        f->render(text, 0, x, y, color, halign, valign, LLFontGL::NORMAL, LLFontGL::NO_SHADOW,
                  S32_MAX, max_pixels, nullptr, ellipses);
    }

    S32 wrapText(const LLFontGL* f, const LLWString& text, F32 max_pixels, std::vector<LLWString>& out, S32 max_lines)
    {
        if (!f)
        {
            return 0;
        }
        const S32 length = (S32)text.length();
        S32 offset = 0;
        S32 count = 0;
        while (offset < length && count < max_lines)
        {
            const S32 drawable = (S32)f->maxDrawableChars(text.c_str() + offset, max_pixels, length - offset,
                                                          LLFontGL::WORD_BOUNDARY_IF_POSSIBLE);
            if (drawable <= 0)
            {
                break;
            }
            out.push_back(text.substr(offset, drawable));
            offset += drawable;
            ++count;
        }
        return count;
    }

    LLWString badgeText(S32 count)
    {
        if (count <= 0)
        {
            return LLWString();
        }
        return utf8str_to_wstring(count > style::BADGE_MAX ? llformat("%d+", style::BADGE_MAX) : llformat("%d", count));
    }

    void drawBadge(const LLWString& text, F32 cx, F32 cy, bool quiet, F32 alpha)
    {
        const LLFontGL* f = font(style::FONT_SMALL);
        if (text.empty() || !f)
        {
            return;
        }
        const F32 h = F32(style::BADGE_HEIGHT);
        const F32 w = llmax(h, F32(f->getWidth(text.c_str())) + 2.f * F32(style::BADGE_PAD));
        const F32 left = std::round(cx - w * 0.5f);
        const F32 bottom = std::round(cy - h * 0.5f);
        const LLRectf r(left, bottom + h, left + w, bottom);
        drawRound(r, h * 0.5f, Fill::solid(faded(quiet ? style::BADGE_QUIET : style::BADGE_FILL, alpha)));
        drawText(f, text, r.getCenterX(), r.getCenterY(), faded(style::BADGE_TEXT, alpha), LLFontGL::HCENTER, LLFontGL::VCENTER);
    }

    LLView* findSnapRegion(LLView* root)
    {
        return root ? root->findChildView("floater_snap_region", true) : nullptr;
    }

    LLRect worldChipArea(const LLView& view, const LLView* snap_region, std::initializer_list<const LLView*> top_bars)
    {
        LLRect screen = gViewerWindow->getWorldViewRectScaled();
        if (snap_region && snap_region->isInVisibleChain())
        {
            screen.intersectWith(snap_region->calcScreenRect());
        }
        for (const LLView* bar : top_bars)
        {
            if (bar && bar->isInVisibleChain())
            {
                const LLRect bar_rect = bar->calcScreenRect();
                if (bar_rect.getHeight() > 0 && bar_rect.mBottom < screen.mTop)
                {
                    screen.mTop = bar_rect.mBottom;
                }
            }
        }
        LLRect local;
        view.screenRectToLocal(screen, &local);
        return local;
    }

    LLView* overlayRoot()
    {
        return gViewerWindow ? gViewerWindow->getRootView() : nullptr;
    }

    void setOverlayParams(LLView::Params& params, const LLView& root, const char* name, bool visible)
    {
        params.name          = name;
        params.rect          = root.getLocalRect();
        params.follows.flags = FOLLOWS_ALL;
        params.mouse_opaque  = false;
        if (!visible)
        {
            params.visible = false;     // only when asked: a shown view keeps the default as it is
        }
    }

    void addOverlay(LLView& root, LLView& view, OverlayOrder order, LLHandle<LLView>& handle)
    {
        root.addChild(&view);
        if (order == OverlayOrder::Back)
        {
            root.sendChildToBack(&view);
        }
        handle = view.getHandle();
    }

    LLPanel* StringsPanel::panel()
    {
        if (!mPanel)
        {
            mPanel = LLUICtrlFactory::getInstance()->createFromFile<LLPanel>(mFile, NULL, LLPanel::child_registry_t::instance());
        }
        return mPanel;
    }

    std::string StringsPanel::get(std::string_view name)
    {
        LLPanel* strings = panel();
        return strings ? strings->getString(name) : std::string(name);
    }

    std::string StringsPanel::get(std::string_view name, const LLStringUtil::format_map_t& args)
    {
        LLPanel* strings = panel();
        return strings ? strings->getString(name, args) : std::string(name);
    }

    bool StringsPanel::has(std::string_view name)
    {
        LLPanel* strings = panel();
        return strings && strings->hasString(name);
    }

    LLPointer<LLUIImage> icon(const std::string& name)
    {
        if (name.empty())
        {
            return nullptr;
        }
        return LLUI::getUIImage(ICON_PREFIX + name);
    }

    void declareImages()
    {
        plus::declareUIImage(IMAGE_ROUND,  "plus/ps_round.png");
        plus::declareUIImage(IMAGE_CIRCLE, "plus/ps_circle.png");
        plus::declareUIImage(IMAGE_CUT,    "plus/ps_cut.png");
        plus::declareUIImage(IMAGE_SHADOW, "plus/ps_shadow.png");
        for (const char* name : ICON_NAMES)
        {
            plus::declareUIImage(std::string(ICON_PREFIX) + name, std::string("plus/ps_icon_") + name + ".png");
        }
    }
}
