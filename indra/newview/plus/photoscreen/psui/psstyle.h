/**
 * @file psstyle.h
 * @brief The photo screen's look in one place: colours, sizes and drawing helpers (research/221).
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

// The values are read from the target picture (research/photoscreen-design-target-2026-10-30.webp,
// 1536x912) and scaled to a 1080 pixel high window (x 1.18): the numbers of research/221's
// "design specification", part by part (the part's number is in the comments). The widgets
// (pswidgets.h) read only this table. The table itself is tokens.txt (next to this file): it makes
// pstokens.h (included below) and the Charis skin's colours (skins/charis/colors.xml), so a colour or a
// size is changed there and nowhere else (research/239 (12)). States the picture does not show come
// from fixed steps (style::STATE_*): hovered = 6 % towards white, pressed = 6 % towards black,
// disabled = 40 % opacity.
//
// Drawing: everything goes through the official immediate mode (gGL, the UI shader, the UI
// translation and scale), like gl_rect_2d(). Rounded shapes are a white disc mask (ps_round.png)
// drawn as nine pieces whose corners are as large as the radius asked for; the same texture for
// all of them, so the UI's batches are not broken between shapes (only text breaks them, as for
// every official widget).

#ifndef PLUS_PSSTYLE_H
#define PLUS_PSSTYLE_H

#include "v4color.h"
#include "llfontgl.h"
#include "llpointer.h"
#include "llrect.h"
#include "llstring.h"
#include "lluictrlfactory.h"
#include "llview.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

class LLPanel;
class LLTexture;
class LLUICtrl;
class LLUIImage;
class LLView;

namespace plus::psui
{
    // 0xRRGGBB to a colour.
    inline LLColor4 rgb(U32 hex, F32 alpha = 1.f)
    {
        return LLColor4(F32((hex >> 16) & 0xff) / 255.f, F32((hex >> 8) & 0xff) / 255.f, F32(hex & 0xff) / 255.f, alpha);
    }

    namespace style
    {
        // A size name of fonts.xml and a style (LLFontGL::NORMAL / BOLD).
        struct FontSpec { const char* size; U8 style; };
    }
}

// The table: colours, sizes and fonts, made from tokens.txt by make_ui_tokens.py (research/239 (12)).
#include "pstokens.h"

namespace plus::psui
{
    const LLFontGL* font(const style::FontSpec& spec);

    // Which corners are round.
    enum Corners : U8
    {
        CORNER_TL = 1, CORNER_TR = 2, CORNER_BL = 4, CORNER_BR = 8,
        CORNERS_NONE = 0, CORNERS_ALL = 15,
        CORNERS_TOP = CORNER_TL | CORNER_TR, CORNERS_BOTTOM = CORNER_BL | CORNER_BR,
        CORNERS_LEFT = CORNER_TL | CORNER_BL, CORNERS_RIGHT = CORNER_TR | CORNER_BR,
    };
    // "all", "none", "top", "bottom", "left", "right" or a list of "tl", "tr", "bl", "br" joined by ','.
    U8 parseCorners(const std::string& text);

    // The colours of a shape's four corners (in between: linear).
    struct Fill
    {
        LLColor4 tl, tr, bl, br;
        static Fill solid(const LLColor4& c)                                   { return { c, c, c, c }; }
        static Fill vertical(const LLColor4& top, const LLColor4& bottom)      { return { top, top, bottom, bottom }; }
        static Fill horizontal(const LLColor4& left, const LLColor4& right)    { return { left, right, left, right }; }
        Fill faded(F32 alpha) const;
        Fill hovered() const;
        Fill pressed() const;
    };

    LLColor4 hovered(const LLColor4& c);        // 6 % towards white
    LLColor4 pressed(const LLColor4& c);        // 6 % towards black
    LLColor4 faded(const LLColor4& c, F32 alpha);

    // ---- shapes (rectangles are in the view's local coordinates, as for gl_rect_2d) ---------
    void fillRect(const LLRectf& r, const Fill& fill);
    void drawRound(const LLRectf& r, F32 radius, const Fill& fill, U8 corners = CORNERS_ALL);
    // A rounded shape with a border line. An opaque fill draws the border shape and the fill on
    // top; a see-through fill gets its border as a ring, so the border does not show through it.
    void drawRoundBordered(const LLRectf& r, F32 radius, const Fill& fill, const LLColor4& border,
                           F32 border_width = 1.f, U8 corners = CORNERS_ALL);
    // A border line alone, anti-aliased on both sides.
    void drawRing(const LLRectf& r, F32 radius, F32 width, const LLColor4& color, U8 corners = CORNERS_ALL);
    void drawCircle(F32 cx, F32 cy, F32 radius, const Fill& fill);
    // The soft shadow under a round thing: black, drop pixels lower.
    void drawShadow(F32 cx, F32 cy, F32 radius, F32 alpha = 0.35f, F32 drop = 2.f);
    // Covers the corners of r outside a rounded rectangle with color (a picture's corners on a
    // known background, the world's corners on the ground).
    void drawCornerCuts(const LLRectf& r, F32 radius, const LLColor4& color, U8 corners = CORNERS_ALL);
    // A button's box (16.): its fill and border, pressed or hovered (hovered: only for an enabled button).
    void drawButtonBox(const LLRectf& r, bool pressed, bool hovered, F32 alpha);

    // ---- what every drawn widget needs (PsCtrl's, and widgets on official bases: plus/photoscreen/photocamctl.cpp) ----
    // The view's rectangle in its own coordinates.
    LLRectf localRectOf(const LLView& view);
    // The opacity to draw with: the floater's fading and STATE_DISABLED when disabled.
    F32 drawAlphaOf(const LLView& view);
    // The keyboard focus ring (2 px outside r) while ctrl has the focus and the keys are in use
    // (focusRingVisible()); a box that takes typing shows it however the focus came.
    void drawFocusRingOf(const LLUICtrl& ctrl, const LLRectf& r, F32 radius);
    // "focus-visible": true from a key that moves the focus or presses what has it (Tab, the arrows, Home / End,
    // Page Up / Down, Space, Return) until a mouse button goes down, so a click shows no ring. Looked at once a frame.
    bool focusRingVisible();
    // A tab of an underlined strip (17.: plus_ps_tab, plus_ps_tabs on top): its text centred in r, and while
    // chosen the line along r's bottom with the glow under it. hovered: only for an enabled tab.
    void drawUnderlineTab(const LLFontGL* font, const LLWString& label, const LLRectf& r, bool chosen, bool hovered,
                          F32 alpha);

    // ---- pictures and icons -----------------------------------------------------------------
    // A white icon, coloured, centred on (cx, cy), size pixels square (on whole pixels).
    void drawIcon(const LLUIImage* icon, F32 cx, F32 cy, S32 size, const LLColor4& color);
    // A picture in r with rounded corners (how it fits r: PictureFit below). Nothing for a null
    // texture or one without a size yet.
    // Crop (default): the picture is cut to the rectangle's shape, keeping the middle. Stretch: the whole
    // picture is squeezed into the rectangle (for a rectangle that already has the picture's aspect).
    enum class PictureFit { Crop, Stretch };
    void drawPicture(LLTexture* texture, const LLRectf& r, F32 radius, const LLColor4& color, U8 corners = CORNERS_ALL, PictureFit fit = PictureFit::Crop);

    // ---- text -------------------------------------------------------------------------------
    void drawText(const LLFontGL* font, const LLWString& text, F32 x, F32 y, const LLColor4& color,
                  LLFontGL::HAlign halign = LLFontGL::LEFT, LLFontGL::VAlign valign = LLFontGL::VCENTER,
                  S32 max_pixels = S32_MAX, bool ellipses = false);
    // Cuts text into lines of at most max_pixels (at word boundaries where it can), added to out; at most
    // max_lines of them. Returns how many were added.
    S32 wrapText(const LLFontGL* font, const LLWString& text, F32 max_pixels, std::vector<LLWString>& out,
                 S32 max_lines = S32_MAX);

    // ---- badges -------------------------------------------------------------------------------
    // The text of a count's badge: empty for none, "1" .. "9", "9+".
    LLWString badgeText(S32 count);
    // A round badge (a pill for "9+") centred on (cx, cy): style::BADGE_FILL, or BADGE_QUIET when quiet.
    // Nothing for an empty text.
    void drawBadge(const LLWString& text, F32 cx, F32 cy, bool quiet, F32 alpha);

    // ---- over the world -------------------------------------------------------------------------
    // For a view of its own over the world that draws a chip (plus/shaderreload's notice, plus/safemode's
    // badge): the toolbars' snap region under root (floater_snap_region, panel_toolbar_view.xml), or null.
    LLView* findSnapRegion(LLView* root);
    // Where such a chip may go, in view's coordinates: the world view, within snap_region (the area
    // floaters snap to, between the toolbars) while that is shown. snap_region may be null. top_bars: bars along
    // the window's top (the chiclet, top info and shortcut bars) the area stays under while they are shown (a null
    // one is skipped).
    LLRect worldChipArea(const LLView& view, const LLView* snap_region, std::initializer_list<const LLView*> top_bars = {});

    // A view of its own over the world (a notice, a chip, a guide drawn over the 3D picture): a child of the window's
    // root view, as large as it and following it, the mouse going through it to the world (mouse_opaque false).
    // Where among the root's children: at the back (over the 3D picture, under every floater, menu and panel), or at
    // the front (over the login panel: plus/safemode's badge until the world is shown).
    enum class OverlayOrder { Back, Front };

    // The window's root view, or null while there is none (before the UI is made).
    LLView* overlayRoot();
    // The parameters every such view has (the root's whole rectangle, FOLLOWS_ALL, mouse_opaque false); visible
    // false: made hidden (true leaves the default, shown).
    void setOverlayParams(LLView::Params& params, const LLView& root, const char* name, bool visible);
    // Adds view to root at order and keeps its handle in handle.
    void addOverlay(LLView& root, LLView& view, OverlayOrder order, LLHandle<LLView>& handle);

    // Makes a T (an LLView with its own Params) over the world and keeps it in handle; prepare(T&, LLView* root) runs
    // after it is made and before it is added (finding the official views it places itself by, its first texts).
    // Null when handle holds a view already, when the UI is not there yet, or when the view could not be made.
    template <class T, class Prepare>
    T* attachWorldOverlay(LLHandle<LLView>& handle, const char* name, bool visible, OverlayOrder order, Prepare&& prepare)
    {
        LLView* root = overlayRoot();
        if (handle.get() || !root)
        {
            return nullptr;
        }
        typename T::Params params;
        setOverlayParams(params, *root, name, visible);
        T* view = LLUICtrlFactory::create<T>(params);
        if (!view)
        {
            return nullptr;
        }
        prepare(*view, root);
        addOverlay(*root, *view, order, handle);
        return view;
    }

    // The same with nothing to do between making and adding.
    template <class T>
    T* attachWorldOverlay(LLHandle<LLView>& handle, const char* name, bool visible = true,
                          OverlayOrder order = OverlayOrder::Back)
    {
        return attachWorldOverlay<T>(handle, name, visible, order, [](T&, LLView*) {});
    }

    // ---- texts of a panel never shown ------------------------------------------------------------
    // A feature's localized texts kept as <string>s of a panel's XML (the viewer reads its strings; the panel is
    // never shown nor added anywhere). The panel is made at the first text asked for and kept for the session; while
    // it cannot be made, every text is its name. Main thread only. constexpr: a file-scope one is made before any
    // code runs (no order of static construction to mind).
    class StringsPanel
    {
    public:
        constexpr explicit StringsPanel(const char* file) : mFile(file) {}

        // The text as written (LLPanel::getString(name): no [ARG] is replaced).
        std::string get(std::string_view name);
        // The text with its [ARGS] (LLPanel::getString(name, args): formatted, even for no args).
        std::string get(std::string_view name, const LLStringUtil::format_map_t& args);
        // Whether the panel has a text named name (false while it cannot be made).
        bool has(std::string_view name);

    private:
        LLPanel* panel();

        const char* mFile;
        LLPanel*    mPanel = nullptr;
    };

    // The icon named name ("camera", "chevron_down" ...: the ps_icon_<name>.png files), or null.
    LLPointer<LLUIImage> icon(const std::string& name);

    // Declares the masks and icons (UI images Plus_PS_*). Called once from the feature's init.
    void declareImages();
}

#endif // PLUS_PSSTYLE_H
