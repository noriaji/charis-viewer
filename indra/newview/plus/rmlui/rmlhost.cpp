/**
 * @file rmlhost.cpp
 * @brief LLRmlView (XUI tag plus_rml_view): an RmlUi document as a floater's content (research/364 2.2, 2.4).
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

#include "rmlhost.h"

#include "rmlime.h"
#include "rmlsystem.h"

#include "plus/photoscreen/psui/psstyle.h"

#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llglslshader.h"
#include "lllocalcliprect.h"
#include "llpanel.h"
#include "llrender.h"
#include "llrhi.h"
#include "lltimer.h"
#include "lltooltip.h"
#include "llui.h"
#include "llvertexbuffer.h"
#include "llwindow.h"

#include <limits>

using namespace plus::rmlui;

LLRmlView* LLRmlView::sCurrent = nullptr;

namespace
{
    LLDefaultChildRegistry::Register<LLRmlView> sRmlViewRegistration("plus_rml_view");

    constexpr F64 NO_DEADLINE = std::numeric_limits<F64>::max();
    constexpr F64 MAX_DELAY_SECONDS = 3600.0;   // a longer GetNextUpdateDelay (infinity: wait for input) is no deadline
    constexpr S32 MESSAGE_PAD = 10;

    F64 seconds()
    {
        return LLTimer::getTotalSeconds();
    }

    int modifiers(MASK mask)
    {
        int state = 0;
        if (mask & MASK_CONTROL)
        {
            state |= Rml::Input::KM_CTRL;
        }
        if (mask & MASK_SHIFT)
        {
            state |= Rml::Input::KM_SHIFT;
        }
        if (mask & MASK_ALT)
        {
            state |= Rml::Input::KM_ALT;
        }
        return state;
    }

    Rml::Input::KeyIdentifier keyIdentifier(KEY key)
    {
        using namespace Rml::Input;
        if (key >= 'A' && key <= 'Z')
        {
            return (KeyIdentifier)(KI_A + (key - 'A'));
        }
        if (key >= '0' && key <= '9')
        {
            return (KeyIdentifier)(KI_0 + (key - '0'));
        }
        if (key >= KEY_F1 && key <= KEY_F12)
        {
            return (KeyIdentifier)(KI_F1 + (key - KEY_F1));
        }
        switch (key)
        {
        case ' ':               return KI_SPACE;
        case KEY_RETURN:        return KI_RETURN;
        case KEY_PAD_RETURN:    return KI_NUMPADENTER;
        case KEY_LEFT:
        case KEY_PAD_LEFT:      return KI_LEFT;
        case KEY_RIGHT:
        case KEY_PAD_RIGHT:     return KI_RIGHT;
        case KEY_UP:
        case KEY_PAD_UP:        return KI_UP;
        case KEY_DOWN:
        case KEY_PAD_DOWN:      return KI_DOWN;
        case KEY_HOME:
        case KEY_PAD_HOME:      return KI_HOME;
        case KEY_END:
        case KEY_PAD_END:       return KI_END;
        case KEY_PAGE_UP:
        case KEY_PAD_PGUP:      return KI_PRIOR;
        case KEY_PAGE_DOWN:
        case KEY_PAD_PGDN:      return KI_NEXT;
        case KEY_INSERT:
        case KEY_PAD_INS:       return KI_INSERT;
        case KEY_DELETE:
        case KEY_PAD_DEL:       return KI_DELETE;
        case KEY_BACKSPACE:     return KI_BACK;
        case KEY_TAB:           return KI_TAB;
        case KEY_ESCAPE:        return KI_ESCAPE;
        case KEY_SHIFT:         return KI_LSHIFT;
        case KEY_CONTROL:       return KI_LCONTROL;
        case KEY_ALT:           return KI_LMENU;
        default:                return KI_UNKNOWN;
        }
    }

    // The keys a focused part of the document uses (moving the focus, pressing, sliding, editing).
    bool navigationKey(KEY key)
    {
        switch (key)
        {
        case KEY_TAB: case KEY_RETURN: case KEY_PAD_RETURN: case ' ':
        case KEY_LEFT: case KEY_RIGHT: case KEY_UP: case KEY_DOWN:
        case KEY_PAD_LEFT: case KEY_PAD_RIGHT: case KEY_PAD_UP: case KEY_PAD_DOWN:
        case KEY_HOME: case KEY_END: case KEY_PAGE_UP: case KEY_PAGE_DOWN:
        case KEY_PAD_HOME: case KEY_PAD_END: case KEY_PAD_PGUP: case KEY_PAD_PGDN:
        case KEY_BACKSPACE: case KEY_DELETE: case KEY_PAD_DEL: case KEY_INSERT: case KEY_PAD_INS:
            return true;
        default:
            return false;
        }
    }

    S32 countElements(Rml::Element* element)
    {
        S32 count = 1;
        for (int i = 0; i < element->GetNumChildren(); ++i)
        {
            count += countElements(element->GetChild(i));
        }
        return count;
    }

    // The host's drawing state for one frame, put back on every way out (research/149 (A)): the UI shader,
    // the model view (context pixels, y down, at the host's corner), the host's clip, the XUI's blend and an
    // unbound texture unit.
    class DrawScope
    {
    public:
        explicit DrawScope(const LLRmlView& view)
        :   mClip(view.getLocalRect()),
            mPreviousShader(LLGLSLShader::sCurBoundShaderPtr)
        {
            gGL.flush();    // what the XUI drew before goes first (as LLFontVertexBuffer::renderBuffers)
            if (mPreviousShader != &gUIProgram)
            {
                gUIProgram.bind();
            }
            gGL.getTexUnit(0)->enable(LLTexUnit::TT_TEXTURE);

            // The XUI's vertices land at (offset + local) * scale (LLRender::vertex3f): the context's pixels are
            // local * scale, so the model view moves them by offset * scale and turns y down from the top.
            const LLVector3 offset = gGL.getUITranslation();
            const LLVector3 ui_scale = gGL.getUIScale();
            mScale.set(ui_scale.mV[VX], ui_scale.mV[VY]);
            const LLRect rect = view.getRect();
            const F32 height_px = (F32)ll_round((F32)rect.getHeight() * mScale.mV[VY]);
            gGL.matrixMode(LLRender::MM_MODELVIEW);
            gGL.pushMatrix();
            gGL.translatef((F32)ll_round(offset.mV[VX] * mScale.mV[VX]), (F32)ll_round(offset.mV[VY] * mScale.mV[VY]) + height_px, 0.f);
            gGL.scalef(1.f, -1.f, 1.f);

            // The scissor's frame, as LLScreenClipRect::updateScissorRegion makes the host's clip (lllocalcliprect.cpp).
            const LLVector2& k = LLUI::getScaleFactor();
            const F32 left_ui = (F32)LLFontGL::sCurOrigin.mX;
            const F32 bottom_ui = (F32)LLFontGL::sCurOrigin.mY;
            mFrame.left_px = left_ui * k.mV[VX];
            mFrame.top_px = (bottom_ui + (F32)rect.getHeight()) * k.mV[VY];
            mFrame.zoom_x = mScale.mV[VX] > 0.f ? k.mV[VX] / mScale.mV[VX] : 1.f;
            mFrame.zoom_y = mScale.mV[VY] > 0.f ? k.mV[VY] / mScale.mV[VY] : 1.f;
            mFrame.clip_x = llfloor(left_ui * k.mV[VX]);
            mFrame.clip_y = llfloor(bottom_ui * k.mV[VY]);
            mFrame.clip_w = llmax(0, llceil((F32)rect.getWidth() * k.mV[VX])) + 1;
            mFrame.clip_h = llmax(0, llceil((F32)rect.getHeight() * k.mV[VY])) + 1;
        }

        ~DrawScope()
        {
            gGL.flush();
            LLVertexBuffer::unbind();
            gGL.matrixMode(LLRender::MM_MODELVIEW);
            gGL.popMatrix();
            gGL.setSceneBlendType(LLRender::BT_ALPHA);  // the XUI's blend (LLGLSUIDefault)
            gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
            if (mPreviousShader != &gUIProgram)
            {
                if (mPreviousShader)
                {
                    mPreviousShader->bind();
                }
                else
                {
                    gUIProgram.unbind();
                }
            }
            // mClip goes last: the scissor before this host comes back
        }

        DrawScope(const DrawScope&) = delete;
        DrawScope& operator=(const DrawScope&) = delete;

        const LLVector2& scale() const { return mScale; }
        DrawFrame frame(F32 alpha) const
        {
            DrawFrame frame = mFrame;
            frame.alpha = alpha;
            return frame;
        }

    private:
        LLLocalClipRect mClip;
        LLGLSLShader* mPreviousShader;
        LLVector2 mScale;
        DrawFrame mFrame;
    };
}

LLRmlView::Params::Params()
:   document("document")    // named: the XUI attribute document="..." (an unnamed param is never read from the XML)
{
    mouse_opaque(true);
    tab_stop(true);
}

LLRmlView::LLRmlView(const Params& p)
:   LLUICtrl(p),
    mDocumentPath(p.document)
{
    mListener.setHost(this);
}

LLRmlView::~LLRmlView()
{
    if (mBusy > 0)
    {
        // Deleted from inside its own RmlUi work (an event): RmlUi still runs on the context. Floaters are deleted
        // later (LLMortician), so this only comes from a direct delete; nothing can wait here.
        LL_WARNS("RmlUi") << "RmlUi: a host was deleted while RmlUi worked for it" << LL_ENDL;
        llassert(false);
    }
    unloadNow();
}

LLRmlView::Scope::Scope(LLRmlView* host)
:   mHost(host),
    mPrevious(sCurrent)
{
    sCurrent = host;
    if (mHost)
    {
        ++mHost->mBusy;
    }
}

LLRmlView::Scope::~Scope()
{
    sCurrent = mPrevious;
    if (mHost && --mHost->mBusy == 0 && mHost->mUnloadPending)
    {
        mHost->unloadNow();     // an unload() asked during the work (research/364 6: "unload の保留")
    }
}

void LLRmlView::Listener::ProcessEvent(Rml::Event& event)
{
    if (mHost && mHost->mHooks.on_event)
    {
        mHost->mHooks.on_event(event);
    }
}

void LLRmlView::fail(Status status, const char* message_name)
{
    mStatus = status;
    std::string text;
    if (translate(message_name, text))
    {
        mMessage = utf8str_to_wstring(text);
    }
    else
    {
        mMessage.clear();
    }
}

void LLRmlView::load()
{
    if (mStatus != Status::NotLoaded)
    {
        return;
    }
    if (!isDocumentPath(mDocumentPath))
    {
        LL_WARNS("RmlUi") << "RmlUi: the document of " << getName() << " is not a file name under app_settings/"
                          << DOCUMENT_FOLDER << LL_ENDL;
        fail(Status::Failed, "rml_failed");
        return;
    }
    const F64 start = seconds();
    StartTimes times;
    switch (acquire(times))
    {
    case Start::SafeMode:
        fail(Status::SafeMode, "rml_unavailable");
        return;
    case Start::Failed:
        fail(Status::Failed, "rml_failed");
        return;
    case Start::Ready:
        break;
    }
    mHolding = true;

    static U32 sContextSerial = 0;  // a new name for every host: two windows of the same document never share one
    mContextName = "plus_rml_" + std::to_string(++sContextSerial);
    const LLVector2 scale = LLUI::getScaleFactor();
    mPixelScale = scale;
    mWidthPx = llmax(1, ll_round((F32)getRect().getWidth() * scale.mV[VX]));
    mHeightPx = llmax(1, ll_round((F32)getRect().getHeight() * scale.mV[VY]));

    Scope host(this);
    mContext = Rml::CreateContext(mContextName, Rml::Vector2i(mWidthPx, mHeightPx), &renderer(), &RmlPreeditor::instance());
    if (mContext)
    {
        mContext->SetDensityIndependentPixelRatio(scale.mV[VY]);
        mContext->EnableMouseCursor(true);
        if (mHooks.before_load)
        {
            mHooks.before_load(*mContext);
        }
        mDocument = mContext->LoadDocument(mDocumentPath);
    }
    if (!mDocument)
    {
        LL_WARNS("RmlUi") << "RmlUi: " << mDocumentPath << " could not be loaded" << LL_ENDL;
        if (mContext)
        {
            Rml::RemoveContext(mContextName);
            mContext = nullptr;
        }
        mHolding = false;
        release();
        fail(Status::Failed, "rml_failed");
        return;
    }
    if (mHooks.after_load)
    {
        mHooks.after_load(*mDocument);
    }
    // No focus: a text field focused now would ask the window for the IME without the keyboard focus here.
    mDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    mStatus = Status::Loaded;
    mDirty = DIRTY_ALL;
    mNextUpdateAt = 0.0;

    mOpenLogPending = true;
    mOpenStart = start;
    mLoadMs = (seconds() - start) * 1000.0;
    mInitMs = times.init_ms;
    mFontsMs = times.fonts_ms;
    mFontFaces = times.font_faces;
}

void LLRmlView::unload()
{
    if (mBusy > 0)
    {
        mUnloadPending = true;  // RmlUi is in the middle of this context's work: the last Scope out unloads
        return;
    }
    unloadNow();
}

void LLRmlView::unloadNow()
{
    mUnloadPending = false;
    mBlurredField.reset();
    mBlurPlace.reset();
    releaseEditMenu();
    RmlPreeditor::instance().forgetHost(this);
    if (mMouseCaptured)
    {
        mMouseCaptured = false;
        gFocusMgr.removeMouseCaptureWithoutCallback(this);
    }
    if (mContext)
    {
        Scope host(this);
        renderer().forget(mDrawList);
        Rml::RemoveContext(mContextName);   // its document and elements go now (a text field's OnDestroy finds the bridge cleared)
        mContext = nullptr;
        mDocument = nullptr;
    }
    if (mHolding)
    {
        mHolding = false;
        release();      // the last host: RmlUi ends in 60 s, or now while the viewer quits
    }
    mStatus = Status::Unloaded;
    mMessage.clear();
    mOpenLogPending = false;
}

void LLRmlView::listen(Rml::Element* element, const char* event)
{
    if (element && event)
    {
        element->AddEventListener(event, &mListener);
    }
}

void LLRmlView::listenCapture(Rml::Element* element, const char* event)
{
    if (element && event)
    {
        element->AddEventListener(event, &mListener, true);
    }
}

void LLRmlView::markDirty()
{
    mDirty |= DIRTY_DATA;
}

bool LLRmlView::translate(const std::string& key, std::string& out) const
{
    for (LLView* view = getParent(); view; view = view->getParent())
    {
        LLPanel* panel = dynamic_cast<LLPanel*>(view);
        if (panel && panel->hasString(key))
        {
            std::string value = panel->getString(key);
            if (value.find_first_of("<&{}") != std::string::npos)
            {
                // a text node holding them would be read again as RML or as a data expression (Factory::InstanceElementText)
                LL_WARNS_ONCE("RmlUi") << "RmlUi: the string " << key << " holds < & { or }: not used" << LL_ENDL;
                return false;
            }
            out.swap(value);
            return true;
        }
    }
    return false;
}

void LLRmlView::setCursorName(const std::string& name)
{
    if (name == "text")
    {
        mCursor = UI_CURSOR_IBEAM;
    }
    else if (name == "pointer")
    {
        mCursor = UI_CURSOR_HAND;
    }
    else if (name == "move")
    {
        mCursor = UI_CURSOR_SIZEALL;
    }
    else if (name == "ew-resize" || name == "col-resize")
    {
        mCursor = UI_CURSOR_SIZEWE;
    }
    else if (name == "ns-resize" || name == "row-resize")
    {
        mCursor = UI_CURSOR_SIZENS;
    }
    else if (name == "not-allowed")
    {
        mCursor = UI_CURSOR_NO;
    }
    else if (name == "wait" || name == "progress")
    {
        mCursor = UI_CURSOR_WORKING;
    }
    else if (name == "crosshair")
    {
        mCursor = UI_CURSOR_CROSS;
    }
    else
    {
        mCursor = UI_CURSOR_ARROW;
    }
}

LLCoordGL LLRmlView::contextToGL(Rml::Vector2f point) const
{
    S32 screen_x = 0;
    S32 screen_y = 0;
    localPointToScreen(0, 0, &screen_x, &screen_y);
    S32 gl_x = 0;
    S32 gl_y = 0;
    LLUI::getInstance()->screenPointToGL(screen_x, screen_y, &gl_x, &gl_y);
    return LLCoordGL(gl_x + ll_round(point.x), gl_y + mHeightPx - ll_round(point.y));
}

LLRect LLRmlView::contextRectToGL(const Rml::Rectanglef& rect) const
{
    const LLCoordGL top_left = contextToGL(Rml::Vector2f(rect.Left(), rect.Top()));
    const LLCoordGL bottom_right = contextToGL(Rml::Vector2f(rect.Right(), rect.Bottom()));
    return LLRect(top_left.mX, top_left.mY, bottom_right.mX, bottom_right.mY);
}

void LLRmlView::syncSize(const LLVector2& scale)
{
    if (scale.mV[VX] <= 0.f || scale.mV[VY] <= 0.f)
    {
        return;
    }
    if (scale != mPixelScale)
    {
        mPixelScale = scale;
        mContext->SetDensityIndependentPixelRatio(scale.mV[VY]);    // 1 dp = 1 UI pixel of a 1080 high window
        mDirty |= DIRTY_SIZE;
    }
    const S32 width = llmax(1, ll_round((F32)getRect().getWidth() * scale.mV[VX]));
    const S32 height = llmax(1, ll_round((F32)getRect().getHeight() * scale.mV[VY]));
    if (width != mWidthPx || height != mHeightPx)
    {
        mWidthPx = width;
        mHeightPx = height;
        mContext->SetDimensions(Rml::Vector2i(width, height));
        mDirty |= DIRTY_SIZE;
    }
}

void LLRmlView::drawMessage() const
{
    if (mMessage.empty())
    {
        return;
    }
    const S32 width = getRect().getWidth() - 2 * MESSAGE_PAD;
    LLFontGL::getFontSansSerif()->render(mMessage, 0, (F32)MESSAGE_PAD, (F32)(getRect().getHeight() - MESSAGE_PAD),
                                         plus::psui::style::TEXT_LABEL, LLFontGL::LEFT, LLFontGL::TOP, LLFontGL::NORMAL,
                                         LLFontGL::NO_SHADOW, S32_MAX, llmax(0, width));
}

void LLRmlView::draw()
{
    if (mStatus == Status::NotLoaded)
    {
        load();
    }
    if (!ready())
    {
        drawMessage();
        return;
    }

    const F64 now = seconds();
    Stats& counts = stats();
    const bool timed = statsOn();
    LLRHI::cpuDetailMark("ui rml");
    {
        DrawScope scope(*this);
        Scope host(this);   // before syncSize: SetDimensions fires a resize event into the document's listeners
        syncSize(scope.scale());
        bool render = !mDrawList.valid();
        if (mDirty || now >= mNextUpdateAt)
        {
            mDirty = 0;     // first: what dirties it during the update is for the next frame
            if (mHooks.before_update)
            {
                mHooks.before_update();
            }
            const F64 update_start = timed ? seconds() : 0.0;
            mContext->Update();
            if (timed)
            {
                counts.update_ms += (seconds() - update_start) * 1000.0;
            }
            ++counts.updates;
            const double delay = mContext->GetNextUpdateDelay();
            mNextUpdateAt = (delay >= 0.0 && delay < MAX_DELAY_SECONDS) ? now + delay : NO_DEADLINE;
            if (mHooks.after_update)
            {
                mHooks.after_update();  // the new layout's sizes (what changes there is for the next update)
            }
            RmlPreeditor::instance().refresh(*this);
            render = true;
        }
        if (render)
        {
            const F64 render_start = timed ? seconds() : 0.0;
            // A second try when a glyph texture was made again during the first (rmlrender.h endRecording); still
            // invalid after it, nothing is drawn this frame and the next one renders again.
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                renderer().beginRecording(mDrawList);
                mContext->Render();
                if (renderer().endRecording())
                {
                    break;
                }
            }
            if (timed)
            {
                counts.render_ms += (seconds() - render_start) * 1000.0;
            }
            ++counts.renders;
        }
        else
        {
            ++counts.replays;
        }
        renderer().draw(mDrawList, scope.frame(getDrawContext().mAlpha));
        ++counts.draws;
        if (timed)
        {
            ++counts.timed_draws;
        }
    }
    LLRHI::cpuDetailMark("ui 2d");
    if (mOpenLogPending)
    {
        logOpened(seconds());
    }
    logStatsIfDue(now);
}

void LLRmlView::logOpened(F64 now)
{
    mOpenLogPending = false;
    LL_INFOS("RmlUi") << "RmlUi: opened " << mDocumentPath << " in " << (S32)((now - mOpenStart) * 1000.0) << " ms (load "
                      << (S32)mLoadMs << " ms, init " << (S32)mInitMs << " ms, fonts " << (S32)mFontsMs << " ms, "
                      << mFontFaces << " faces, elements " << (mDocument ? countElements(mDocument) : 0) << ")" << LL_ENDL;
}

void LLRmlView::reshape(S32 width, S32 height, bool called_from_parent)
{
    LLUICtrl::reshape(width, height, called_from_parent);
    mDirty |= DIRTY_SIZE;
}

void LLRmlView::onVisibilityChange(bool new_visibility)
{
    LLUICtrl::onVisibilityChange(new_visibility);
    if (!new_visibility && mMouseCaptured)
    {
        mMouseCaptured = false;
        gFocusMgr.removeMouseCaptureWithoutCallback(this);
        leaveMouse();
    }
    mDirty |= DIRTY_ALL;    // shown again: the first frame updates
}

LLVector2 LLRmlView::inputScale() const
{
    return (mPixelScale.mV[VX] > 0.f && mPixelScale.mV[VY] > 0.f) ? mPixelScale : LLUI::getScaleFactor();
}

Rml::Vector2i LLRmlView::toContext(S32 x, S32 y) const
{
    const LLVector2 scale = inputScale();
    const S32 height = mHeightPx > 0 ? mHeightPx : ll_round((F32)getRect().getHeight() * scale.mV[VY]);
    return Rml::Vector2i(ll_round((F32)x * scale.mV[VX]), height - ll_round((F32)y * scale.mV[VY]));
}

void LLRmlView::moveMouse(S32 x, S32 y, MASK mask)
{
    const Rml::Vector2i point = toContext(x, y);
    if (mMouseInside && point == mMousePx)
    {
        return;     // the hover of every frame with the mouse still: nothing to update
    }
    mMouseInside = true;
    mMousePx = point;
    mContext->ProcessMouseMove(point.x, point.y, modifiers(mask));
    mDirty |= DIRTY_INPUT;
}

bool LLRmlView::handleHover(S32 x, S32 y, MASK mask)
{
    if (!ready())
    {
        return LLUICtrl::handleHover(x, y, mask);
    }
    {
        Scope host(this);
        moveMouse(x, y, mask);
    }
    if (LLWindow* window = getWindow())
    {
        window->setCursor(mCursor);     // every hover: the viewer puts the arrow back each frame
    }
    return true;
}

bool LLRmlView::pressMouse(S32 x, S32 y, MASK mask, int button, bool capture)
{
    if (!ready())
    {
        return false;
    }
    RmlPreeditor::instance().dropReconvert();
    mBlurredField.reset();      // the pressed element decides what has the focus now
    mBlurPlace.reset();
    if (!hasFocus())
    {
        mPressing = true;
        setFocus(true);     // keys and typing come here (and a text field's IME needs it)
        mPressing = false;
    }
    if (capture && !mMouseCaptured)
    {
        gFocusMgr.setMouseCapture(this);
        mMouseCaptured = true;
    }
    {
        Scope host(this);
        moveMouse(x, y, mask);
        mContext->ProcessMouseButtonDown(button, modifiers(mask));
    }
    mDirty |= DIRTY_INPUT;
    return true;
}

bool LLRmlView::releaseMouse(S32 x, S32 y, MASK mask, int button)
{
    if (!ready())
    {
        return false;
    }
    {
        Scope host(this);
        moveMouse(x, y, mask);
        mContext->ProcessMouseButtonUp(button, modifiers(mask));
    }
    if (button == 0)
    {
        releaseCapture();
    }
    mDirty |= DIRTY_INPUT;
    return true;
}

void LLRmlView::releaseCapture()
{
    if (mMouseCaptured)
    {
        mMouseCaptured = false;     // before: setMouseCapture(nullptr) calls onMouseCaptureLost, which then does nothing
        if (hasMouseCapture())
        {
            gFocusMgr.setMouseCapture(nullptr);
        }
    }
}

void LLRmlView::leaveMouse()
{
    mMouseInside = false;
    if (!ready())
    {
        return;
    }
    Scope host(this);
    mContext->ProcessMouseLeave();
    // A press still held ends without a click: no element is hovered now (Context::ProcessMouseButtonUp clicks
    // only the hovered one), and a drag (a slider) stops.
    mContext->ProcessMouseButtonUp(0, 0);
    mDirty |= DIRTY_INPUT;
}

bool LLRmlView::handleMouseDown(S32 x, S32 y, MASK mask)
{
    return pressMouse(x, y, mask, 0, true) || LLUICtrl::handleMouseDown(x, y, mask);
}

bool LLRmlView::handleMouseUp(S32 x, S32 y, MASK mask)
{
    return releaseMouse(x, y, mask, 0) || LLUICtrl::handleMouseUp(x, y, mask);
}

bool LLRmlView::handleRightMouseDown(S32 x, S32 y, MASK mask)
{
    return pressMouse(x, y, mask, 1, false) || LLUICtrl::handleRightMouseDown(x, y, mask);
}

bool LLRmlView::handleRightMouseUp(S32 x, S32 y, MASK mask)
{
    return releaseMouse(x, y, mask, 1) || LLUICtrl::handleRightMouseUp(x, y, mask);
}

bool LLRmlView::handleMiddleMouseDown(S32 x, S32 y, MASK mask)
{
    return pressMouse(x, y, mask, 2, false) || LLUICtrl::handleMiddleMouseDown(x, y, mask);
}

bool LLRmlView::handleMiddleMouseUp(S32 x, S32 y, MASK mask)
{
    return releaseMouse(x, y, mask, 2) || LLUICtrl::handleMiddleMouseUp(x, y, mask);
}

bool LLRmlView::handleDoubleClick(S32 x, S32 y, MASK mask)
{
    // The second press of a double click (the viewer sends down, up, double click, up): RmlUi tells a double
    // click by itself. Taken here so the floater's blockMouseEvent does not swallow it (research/358 P26).
    return pressMouse(x, y, mask, 0, true) || LLUICtrl::handleDoubleClick(x, y, mask);
}

void LLRmlView::onMouseCaptureLost()
{
    if (!mMouseCaptured)
    {
        return;     // released by releaseCapture(), or never taken
    }
    mMouseCaptured = false;
    leaveMouse();   // Alt-Tab, a modal window: the press ends without a click
}

void LLRmlView::onMouseLeave(S32 x, S32 y, MASK mask)
{
    LLUICtrl::onMouseLeave(x, y, mask);
    if (!mMouseCaptured && mMouseInside && ready())
    {
        mMouseInside = false;
        Scope host(this);
        mContext->ProcessMouseLeave();
        mDirty |= DIRTY_INPUT;
    }
}

bool LLRmlView::handleScrollWheel(S32 x, S32 y, S32 clicks)
{
    if (!ready())
    {
        return false;
    }
    bool not_consumed = true;
    {
        Scope host(this);
        moveMouse(x, y, 0);
        not_consumed = mContext->ProcessMouseWheel(Rml::Vector2f(0.f, (F32)clicks), 0);
    }
    mDirty |= DIRTY_INPUT;
    return !not_consumed;   // nothing scrolled: the parent's scroll gets the wheel
}

bool LLRmlView::handleToolTip(S32 x, S32 y, MASK mask)
{
    if (!ready())
    {
        return LLUICtrl::handleToolTip(x, y, mask);
    }
    const Rml::Vector2i point = toContext(x, y);
    for (Rml::Element* element = mContext->GetElementAtPoint(Rml::Vector2f((F32)point.x, (F32)point.y)); element;
         element = element->GetParentNode())
    {
        const Rml::Variant* title = element->GetAttribute("title");
        if (!title)
        {
            continue;
        }
        std::string text = title->Get<Rml::String>();
        std::string translated;
        // title="[[name]]": the floater's string (attributes are not text nodes: TranslateString does not see them)
        if (text.size() > 4 && text.compare(0, 2, "[[") == 0 && text.compare(text.size() - 2, 2, "]]") == 0 &&
            translate(text.substr(2, text.size() - 4), translated))
        {
            text.swap(translated);
        }
        if (text.empty())
        {
            break;
        }
        static LLCachedControl<bool> allow_ui_tooltips(*LLUI::getInstance()->mSettingGroups["config"], "BasicUITooltips", true);
        if (allow_ui_tooltips)
        {
            // The element's box in this view's UI units, for the tool tip's sticky rectangle.
            const LLVector2 scale = inputScale();
            const Rml::Vector2f corner = element->GetAbsoluteOffset(Rml::BoxArea::Border);
            const Rml::Vector2f size = element->GetBox().GetSize(Rml::BoxArea::Border);
            const S32 height = mHeightPx;
            const LLRect local(ll_round(corner.x / scale.mV[VX]), ll_round(((F32)height - corner.y) / scale.mV[VY]),
                               ll_round((corner.x + size.x) / scale.mV[VX]), ll_round(((F32)height - corner.y - size.y) / scale.mV[VY]));
            LLRect screen;
            localRectToScreen(local, &screen);
            LLToolTipMgr::instance().show(LLToolTip::Params().message(text).sticky_rect(screen).delay_time(getTooltipTimeout()));
        }
        return true;
    }
    return LLUICtrl::handleToolTip(x, y, mask);
}

bool LLRmlView::textFieldFocused() const
{
    if (!mContext)
    {
        return false;
    }
    Rml::Element* focus = mContext->GetFocusElement();
    if (!focus)
    {
        return false;
    }
    const Rml::String& tag = focus->GetTagName();
    if (tag == "textarea")
    {
        return true;
    }
    if (tag != "input")
    {
        return false;
    }
    const Rml::String type = focus->GetAttribute<Rml::String>("type", "text");
    return type == "text" || type == "password";
}

bool LLRmlView::keyForDocument(KEY key, MASK mask, bool not_consumed) const
{
    if (!not_consumed)
    {
        return true;    // an element of the document stopped it
    }
    if (key >= KEY_F1 && key <= KEY_F12)
    {
        return false;   // the viewer's function keys (F12: the photo screen ...)
    }
    Rml::Element* focus = mContext->GetFocusElement();
    const bool inner = focus && focus != focus->GetOwnerDocument() && focus != mContext->GetRootElement();
    if (!inner || key == KEY_ESCAPE)
    {
        return false;   // nothing of the document has the focus; Esc is the floater's
    }
    if (textFieldFocused())
    {
        if (mask & MASK_ALT)
        {
            return false;
        }
        if (mask & MASK_CONTROL)
        {
            // the field's own shortcuts (select all, copy, cut, paste, undo, redo); the menus' others go on
            return navigationKey(key) || key == 'A' || key == 'C' || key == 'X' || key == 'V' || key == 'Z' || key == 'Y';
        }
        return true;    // its editing keys (characters come as typed characters, not here)
    }
    // Not a text field: only what an element took (it stopped the key: Tab moving the focus, Space / Return
    // pressing, a slider's arrows; ElementDocument.cpp, WidgetSlider.cpp) is the document's. RmlUi focuses any
    // element clicked, so a key it did not use (the arrows, Page Up / Down, Home / End to walk and fly, Enter
    // for the chat) goes on to the viewer, as after a click on an XUI button.
    return false;
}

bool LLRmlView::processKey(KEY key, MASK mask, KeyMove move)
{
    if (!ready())
    {
        return false;
    }
    const Rml::Input::KeyIdentifier id = keyIdentifier(key);
    if (id == Rml::Input::KI_UNKNOWN)
    {
        return false;
    }
    if (move == KeyMove::Down && key != KEY_SHIFT && key != KEY_CONTROL && key != KEY_ALT)
    {
        RmlPreeditor::instance().dropReconvert();   // (a modifier moves neither the caret nor a character)
    }
    if ((mask & MASK_CONTROL) && (key == 'X' || key == 'C') && passwordFocused())
    {
        // No copy out of a password field; RmlUi would still delete the selection on Ctrl+X (any Alt: RmlUi's
        // WidgetTextInput looks at Ctrl only).
        return true;
    }
    bool not_consumed = true;
    {
        Scope host(this);
        not_consumed = move == KeyMove::Down ? mContext->ProcessKeyDown(id, modifiers(mask))
                                             : mContext->ProcessKeyUp(id, modifiers(mask));
    }
    mDirty |= DIRTY_INPUT;
    if (!mContext)
    {
        return true;    // the key closed the document (an unload asked in its event ran as the Scope ended): it was used
    }
    return keyForDocument(key, mask, not_consumed);
}

bool LLRmlView::handleKeyHere(KEY key, MASK mask)
{
    return processKey(key, mask, KeyMove::Down);
}

bool LLRmlView::handleKeyUpHere(KEY key, MASK mask)
{
    return processKey(key, mask, KeyMove::Up);
}

bool LLRmlView::handleUnicodeCharHere(llwchar uni_char)
{
    return typeCharacter(uni_char);
}

bool LLRmlView::typeCharacter(llwchar uni_char)
{
    if (!ready() || uni_char < 0x20 || uni_char == 0x7f || !textFieldFocused())
    {
        return false;
    }
    RmlPreeditor::instance().dropReconvert();
    {
        Scope host(this);
        mContext->ProcessTextInput(static_cast<Rml::Character>(uni_char));  // the field's limits (maxlength) apply
    }
    mDirty |= DIRTY_INPUT;
    return true;
}

void LLRmlView::onFocusReceived()
{
    LLUICtrl::onFocusReceived();
    if (!ready())
    {
        return;
    }
    if (!mPressing && mBlurredField.get())
    {
        // The focus came back: the field it left has it again, as an XUI field does (LLFloater::setFocus: only an
        // enabled, visible one), so keys do not go to the world with a field on show. Not when the focus was moved
        // meanwhile (to anything but where Blur put it: the field, if the Blur did not reach it, or the place the
        // Blur gave it, mBlurPlace). A press that gives the view
        // the focus sets mPressing; a click on a window without the focus first brings it to the front, which
        // focuses its first item before the press arrives, so the old field is focused for a moment and the press
        // then moves the focus to the pressed element (the end state is the XUI's too).
        Scope host(this);
        Rml::Element* field = mBlurredField.get();
        Rml::Element* now = mContext->GetFocusElement();
        auto* control = rmlui_dynamic_cast<Rml::ElementFormControl*>(field);
        if (field && control && !control->IsDisabled() && field->IsVisible(true) &&
            now && (now == field || now == mBlurPlace.get()))
        {
            field->Focus();
        }
        mDirty |= DIRTY_INPUT;
    }
    mBlurredField.reset();
    mBlurPlace.reset();
    // A text field kept RmlUi's focus while the view had none (or got it without a Focus event: the same element
    // is not focused twice): the edit menu and the IME are the view's again.
    if (ready() && textFieldFocused())
    {
        takeEditMenu();
        RmlPreeditor::instance().hostFocusChanged(*this);
    }
}

void LLRmlView::onFocusLost()
{
    LLUICtrl::onFocusLost();
    if (ready())
    {
        Scope host(this);
        // (taken back by onFocusReceived; assigned every time, so an older one does not stay)
        mBlurredField = textFieldFocused() ? mContext->GetFocusElement()->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
        if (Rml::Element* focus = mContext->GetFocusElement())
        {
            focus->Blur();  // a text field lets the IME and the edit menu go (RmlPreeditor::OnDeactivate)
        }
        Rml::Element* placed = mContext->GetFocusElement();
        mBlurPlace = placed ? placed->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>();
        mDirty |= DIRTY_INPUT;
        // A blur that did not reach the field (an ancestor with `focus: none`: no OnDeactivate) leaves it active in
        // the bridge: the window's IME is let go now, as the view has no keyboard focus.
        RmlPreeditor::instance().hostFocusChanged(*this);
    }
    releaseEditMenu();      // and in any case (a field that did not blur, a view not ready)
}

bool LLRmlView::passwordFocused() const
{
    return RmlPreeditor::isPassword(mContext ? mContext->GetFocusElement() : nullptr);
}

void LLRmlView::takeEditMenu()
{
    gEditMenuHandler = this;
}

void LLRmlView::releaseEditMenu()
{
    if (gEditMenuHandler == this)
    {
        gEditMenuHandler = nullptr;
    }
}

void LLRmlView::sendEditKey(Rml::Input::KeyIdentifier key, int modifiers)
{
    if (!ready() || !textFieldFocused())
    {
        return;
    }
    RmlPreeditor::instance().dropReconvert();   // (the edit menu took Del and Ctrl+X/V/A before processKey saw them)
    {
        Scope host(this);
        mContext->ProcessKeyDown(key, modifiers);
        if (mContext)   // (an unload asked in it waits for the Scope: still there)
        {
            mContext->ProcessKeyUp(key, modifiers);
        }
    }
    mDirty |= DIRTY_INPUT;
}

void LLRmlView::cut()
{
    sendEditKey(Rml::Input::KI_X, Rml::Input::KM_CTRL);
}

bool LLRmlView::canCut() const
{
    return ready() && textFieldFocused() && !passwordFocused();
}

void LLRmlView::copy()
{
    sendEditKey(Rml::Input::KI_C, Rml::Input::KM_CTRL);
}

bool LLRmlView::canCopy() const
{
    return ready() && textFieldFocused() && !passwordFocused();
}

void LLRmlView::paste()
{
    sendEditKey(Rml::Input::KI_V, Rml::Input::KM_CTRL);
}

bool LLRmlView::canPaste() const
{
    return ready() && textFieldFocused();
}

void LLRmlView::doDelete()
{
    sendEditKey(Rml::Input::KI_DELETE, 0);
}

bool LLRmlView::canDoDelete() const
{
    return ready() && textFieldFocused();
}

void LLRmlView::selectAll()
{
    sendEditKey(Rml::Input::KI_A, Rml::Input::KM_CTRL);
}

bool LLRmlView::canSelectAll() const
{
    return ready() && textFieldFocused();
}

bool LLRmlView::acceptsTextInput() const
{
    return ready() && textFieldFocused();
}
