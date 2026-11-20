/**
 * @file rmldemo.cpp
 * @brief Plus.RmlDemo, the RmlUi test window (development), and its XUI twin (research/364 4, 3.5).
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

#include "rmldemo.h"

#include "rmlhost.h"
#include "rmlsettings.h"

#include "plus.h"
#include "plus/photoscreen/psui/psstyle.h"

#include "llfloaterreg.h"
#include "llscrolllistctrl.h"

#include <cmath>
#include <cstdio>

namespace plus::rmlui
{
    namespace
    {
        constexpr char FLOATER_NAME[] = "plus_rml_demo";
        constexpr char XUI_FLOATER_NAME[] = "plus_rml_demo_xui";
        constexpr S32 ROW_TOTAL = 1000;
        constexpr S32 ROW_MARGIN = 3;           // rows made above and below the visible ones
        constexpr S32 FIRST_VIEW_ROWS = 12;     // before the first layout tells the list's height
        // The rows' height: tokens.txt's LIST_ROW_HEIGHT, which charis_tokens.rcss gives the RCSS as --list-row-height.
        constexpr F32 ROW_HEIGHT_DP = (F32)plus::psui::style::LIST_ROW_HEIGHT;

        std::string dpText(F32 dp)
        {
            return std::to_string(ll_round(dp)) + "dp";
        }

        // The slider's range: demo.rml's min and max of #s_amount, which the number box beside it must keep.
        constexpr int AMOUNT_MIN = 0;
        constexpr int AMOUNT_MAX = 100;

        // The number box's text to a value in the range. Full-width digits (what a Japanese IME types, UTF-8
        // EF BC 90..99) count as digits; spaces and tabs, half-width and the full-width space (U+3000, E3 80 80),
        // are skipped; anything else (a letter, a sign, a point, bytes that are no UTF-8) refuses the whole text, as
        // does a text without a digit, so the box goes back to the value it had. The value stops at AMOUNT_MAX while
        // more digits are read ("1000" and "００１００" give 100), so it never overflows; a letter after them still refuses.
        bool parseAmount(const std::string& text, int& out)
        {
            int value = 0;
            bool any = false;
            for (size_t i = 0; i < text.size(); ++i)
            {
                const unsigned char c = (unsigned char)text[i];
                int digit = -1;
                if (c >= '0' && c <= '9')
                {
                    digit = c - '0';
                }
                else if (c == 0xEF && i + 2 < text.size() && (unsigned char)text[i + 1] == 0xBC
                         && (unsigned char)text[i + 2] >= 0x90 && (unsigned char)text[i + 2] <= 0x99)
                {
                    digit = (unsigned char)text[i + 2] - 0x90;
                    i += 2;
                }
                else if (c == ' ' || c == '\t')
                {
                    continue;
                }
                else if (c == 0xE3 && i + 2 < text.size() && (unsigned char)text[i + 1] == 0x80
                         && (unsigned char)text[i + 2] == 0x80)
                {
                    i += 2;     // the full-width space
                    continue;
                }
                else
                {
                    return false;
                }
                any = true;
                value = llmin(value * 10 + digit, AMOUNT_MAX);
            }
            if (!any)
            {
                return false;
            }
            out = llclamp(value, AMOUNT_MIN, AMOUNT_MAX);
            return true;
        }

        std::string rowText(const std::string& format, S32 row)
        {
            std::string text = format;
            LLStringUtil::replaceString(text, "[N]", std::to_string(row + 1));
            return text;
        }

        void init()
        {
            LLFloaterReg::add(FLOATER_NAME, "floater_plus_rml_demo.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<RmlDemoFloater>);
            LLFloaterReg::add(XUI_FLOATER_NAME, "floater_plus_rml_demo_xui.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<RmlDemoXuiFloater>);
        }

        plus::FeatureRegistration sRegistration({ "rmlui", &init, {} });
    }

    // ---- the RmlUi window -----------------------------------------------------------------------------

    RmlDemoFloater::RmlDemoFloater(const LLSD& key)
    :   LLFloater(key)
    {
    }

    RmlDemoFloater::~RmlDemoFloater()
    {
        // The document goes before this floater's members: its data model points at them, its hooks call us.
        if (LLRmlView* host = mHost.get())
        {
            host->unload();
        }
    }

    bool RmlDemoFloater::postBuild()
    {
        LLRmlView* host = findChild<LLRmlView>("body");
        if (!host)
        {
            return true;
        }
        mHost = host->getRmlHandle();
        mRowFormat = getString("list_item");
        LLRmlView::Hooks hooks;
        hooks.before_load = [this](Rml::Context& context) { beforeLoad(context); };
        hooks.after_load = [this](Rml::ElementDocument& document) { afterLoad(document); };
        hooks.before_update = [this]() { beforeUpdate(); };
        hooks.after_update = [this]() { afterUpdate(); };
        hooks.on_event = [this](Rml::Event& event) { onEvent(event); };
        host->setHooks(std::move(hooks));
        host->load();
        return true;
    }

    void RmlDemoFloater::beforeLoad(Rml::Context& context)
    {
        Rml::DataModelConstructor model = context.CreateDataModel("demo");
        if (!model)
        {
            return;
        }
        model.Bind("clicks", &mClicks);
        model.Bind("option", &mOption);     // data-checked: the check box writes it
        model.Bind("amount", &mAmount);     // data-value: the slider writes it
        mModel = model.GetModelHandle();
    }

    void RmlDemoFloater::afterLoad(Rml::ElementDocument& document)
    {
        LLRmlView* host = mHost.get();
        if (!host)
        {
            return;
        }
        for (const char* id : { "b_count", "b_colour", "b_size" })
        {
            host->listen(document.GetElementById(id), "click");
        }
        host->listen(document.GetElementById("list"), "scroll");
        host->listen(document.GetElementById("t_amount"), "change");
        host->listen(document.GetElementById("t_amount"), "blur");
        // (A "resize" event goes to documents only and does not bubble: the list's height is looked at after each
        // update instead, afterUpdate.)
        mRowsDirty = true;
    }

    void RmlDemoFloater::beforeUpdate()
    {
        if (mRowsDirty)
        {
            mRowsDirty = false;
            fillRows();
        }
    }

    void RmlDemoFloater::afterUpdate()
    {
        // The window resized (or the text size switched): the list's new height asks for other rows, made before
        // the next update (fillRows reads the layout this update made).
        LLRmlView* host = mHost.get();
        Rml::ElementDocument* document = host ? host->getDocument() : nullptr;
        Rml::Element* list = document ? document->GetElementById("list") : nullptr;
        if (!list)
        {
            return;
        }
        const F32 height_px = list->GetClientHeight();
        if (height_px != mListHeightPx)
        {
            mListHeightPx = height_px;
            mRowsDirty = true;
            host->markDirty();
        }
    }

    void RmlDemoFloater::onEvent(Rml::Event& event)
    {
        // In RmlUi's event: nothing is removed from the document here (the rows wait for beforeUpdate).
        const Rml::Element* element = event.GetCurrentElement();
        LLRmlView* host = mHost.get();
        if (!element || !host)
        {
            return;
        }
        const Rml::String& id = element->GetId();
        if (id == "b_count")
        {
            ++mClicks;
            if (mModel)
            {
                mModel.DirtyVariable("clicks");
            }
        }
        else if (id == "b_colour")
        {
            switchColour();
        }
        else if (id == "b_size")
        {
            switchTextSize();
        }
        else if (id == "list")
        {
            mRowsDirty = true;
        }
        else if (id == "t_amount")
        {
            // "change" comes after every key; the text counts when Enter was pressed (linebreak) or the focus left
            // (blur), not while it is typed (an empty box in between must stay empty).
            if (event.GetId() == Rml::EventId::Blur || event.GetParameter<bool>("linebreak", false))
            {
                commitAmount(event);
            }
        }
        host->markDirty();
    }

    void RmlDemoFloater::commitAmount(Rml::Event& event)
    {
        Rml::ElementFormControl* box = rmlui_dynamic_cast<Rml::ElementFormControl*>(event.GetCurrentElement());
        if (!box)
        {
            return;
        }
        int amount = mAmount;
        if (parseAmount(box->GetValue(), amount))
        {
            mAmount = amount;
        }
        // The model refreshes the slider and the box (data-value, data-attr-value) at the next update; the box is also
        // set now, so a refused or clamped text ("abc", "500") is replaced even when mAmount did not change.
        box->SetValue(std::to_string(mAmount));
        if (mModel)
        {
            mModel.DirtyVariable("amount");
        }
    }

    void RmlDemoFloater::switchColour()
    {
        LLRmlView* host = mHost.get();
        Rml::ElementDocument* document = host ? host->getDocument() : nullptr;
        if (!document)
        {
            return;
        }
        mAltColour = !mAltColour;
        // The variable set on the body from C++, as a user's colour theme will be (research/364 2.6, research/320);
        // removed, the style sheet's (charis_tokens.rcss) comes back.
        if (mAltColour)
        {
            document->SetProperty("--accent", hexColour(plus::psui::style::LIST_STAR_ON));
        }
        else
        {
            document->RemoveProperty("--accent");
        }
    }

    void RmlDemoFloater::switchTextSize()
    {
        LLRmlView* host = mHost.get();
        Rml::ElementDocument* document = host ? host->getDocument() : nullptr;
        if (!document)
        {
            return;
        }
        mLargeText = !mLargeText;
        document->SetClass("large-text", mLargeText);   // charis.rcss: body.large-text sets --font-body to --font-head
        mRowsDirty = true;
    }

    void RmlDemoFloater::fillRows()
    {
        LLRmlView* host = mHost.get();
        Rml::ElementDocument* document = host ? host->getDocument() : nullptr;
        if (!document || !host->getContext())
        {
            return;
        }
        Rml::Element* list = document->GetElementById("list");
        Rml::Element* top = document->GetElementById("list_top");
        Rml::Element* rows = document->GetElementById("list_rows");
        Rml::Element* bottom = document->GetElementById("list_bottom");
        if (!list || !top || !rows || !bottom)
        {
            return;
        }
        const F32 row_px = ROW_HEIGHT_DP * host->getContext()->GetDensityIndependentPixelRatio();
        if (row_px <= 0.f)
        {
            return;
        }
        const F32 scroll_px = list->GetScrollTop();
        const F32 view_px = list->GetClientHeight() > 0.f ? list->GetClientHeight() : FIRST_VIEW_ROWS * row_px;
        const S32 first = llclamp((S32)(scroll_px / row_px) - ROW_MARGIN, 0, ROW_TOTAL);
        const S32 last = llclamp((S32)std::ceil((scroll_px + view_px) / row_px) + ROW_MARGIN, first, ROW_TOTAL);
        if (first == mFirstRow && last - first == mRowCount && rows->GetNumChildren() == mRowCount)
        {
            return;     // the same rows (a scroll within them)
        }
        mFirstRow = first;
        mRowCount = last - first;
        top->SetProperty("height", dpText((F32)first * ROW_HEIGHT_DP));
        bottom->SetProperty("height", dpText((F32)(ROW_TOTAL - last) * ROW_HEIGHT_DP));
        while (Rml::Element* child = rows->GetFirstChild())
        {
            rows->RemoveChild(child);
        }
        for (S32 row = first; row < last; ++row)
        {
            Rml::ElementPtr element = document->CreateElement("div");
            element->SetClass("list-row", true);
            element->AppendChild(document->CreateTextNode(rowText(mRowFormat, row)));   // text, never RML
            rows->AppendChild(std::move(element));
        }
    }

    // ---- the XUI twin ---------------------------------------------------------------------------------

    RmlDemoXuiFloater::RmlDemoXuiFloater(const LLSD& key)
    :   LLFloater(key)
    {
    }

    bool RmlDemoXuiFloater::postBuild()
    {
        LLUICtrl* count = findChild<LLUICtrl>("clicks_value");
        LLUICtrl* status = findChild<LLUICtrl>("status");
        if (LLUICtrl* button = findChild<LLUICtrl>("b_count"))
        {
            button->setCommitCallback([this, count](LLUICtrl*, const LLSD&) {
                ++mClicks;
                if (count)
                {
                    count->setValue(LLSD(std::to_string(mClicks)));
                }
            });
        }
        // The check box and the slider show their values themselves (the slider's value box). The XUI has no CSS variables: the two buttons only say what they would switch (for the same clicks).
        auto show_state = [this, status]() {
            if (status)
            {
                std::string text = mAltColour ? getString("colour_switched") : std::string();
                if (mLargeText)
                {
                    text += (text.empty() ? "" : " ") + getString("size_switched");
                }
                status->setValue(LLSD(text));
            }
        };
        if (LLUICtrl* button = findChild<LLUICtrl>("b_colour"))
        {
            button->setCommitCallback([this, show_state](LLUICtrl*, const LLSD&) { mAltColour = !mAltColour; show_state(); });
        }
        if (LLUICtrl* button = findChild<LLUICtrl>("b_size"))
        {
            button->setCommitCallback([this, show_state](LLUICtrl*, const LLSD&) { mLargeText = !mLargeText; show_state(); });
        }
        if (LLScrollListCtrl* list = findChild<LLScrollListCtrl>("list"))
        {
            const std::string format = getString("list_item");
            for (S32 row = 0; row < ROW_TOTAL; ++row)
            {
                list->addSimpleElement(rowText(format, row));
            }
        }
        return true;
    }
}
