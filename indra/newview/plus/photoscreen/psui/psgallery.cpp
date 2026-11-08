/**
 * @file psgallery.cpp
 * @brief The photo screen's parts sample (a development window) and the parts' registration (research/221).
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

// The parts sample (Preferences > Charis > Development > "Photo Screen Parts Sample...")
// shows every widget of pswidgets.h at the target picture's sizes and in its places (1080 pixel
// high, x 1.18 of research/photoscreen-design-target-2026-10-30.webp), so a screenshot of it can
// be laid next to the picture. Hover, press and focus are tried with the mouse and Tab; "Show
// disabled" greys every part. It is not connected to any setting: the values only move.
// Under them (research/265): the parts the other windows use (a choice of three and seven, the
// star), a column made by psstack.h as the photo tools make theirs, and sub-tabs over their pages;
// at the bottom the compatible parts (pscompat.h, research/266): the official widgets in this look.

#include "llviewerprecompiledheaders.h"

#include "pscompat.h"
#include "psstack.h"
#include "psstyle.h"
#include "pswidgets.h"

#include "plus.h"

#include "llfloater.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llscrollcontainer.h"

#include <cmath>
#include <tuple>

namespace plus::psui
{
    namespace
    {
        constexpr char FLOATER_NAME[] = "plus_ps_gallery";

        // The values of the column of the sample (research/265): its own, behind no setting. Kept out of the
        // floater, as a row's widget going away may still write its value.
        struct StackValues
        {
            float       exposure  = 0.2f;
            float       softness  = 0.5f;
            float       particles = 1.f;
            float       shadows   = 1.f;
            float       gaze      = 1.f;
            float       fog       = 0.3f;
            float       glow      = 0.f;
            std::string time;
            S32         look      = 1;
        };
        StackValues sValues;
        std::vector<Stack::Row> sRows;
        LLHandle<LLUICtrl> sLookCombo;      // the look row's drop-down (its arrows show what they chose)

        // The marks the photo tools put before a row (the next row's: changed, rebuilds).
        enum Marks { MARK_CHANGED = 1, MARK_REBUILD = 2 };

        class GalleryFloater : public LLFloater
        {
        public:
            explicit GalleryFloater(const LLSD& key) : LLFloater(key) {}

            bool postBuild() override
            {
                fillHistogram();
                fillStack();
                bindTabs();
                if (PsSlider* time = findChild<PsSlider>("s_time"))
                {
                    // The four official times of day as the value box's text (setDisplayText()).
                    auto show = [](PsSlider* slider)
                    {
                        static constexpr const char* TIMES[] = { "6:00", "12:00", "18:30", "0:00" };
                        slider->setDisplayText(TIMES[llclamp(S32(slider->getValue().asReal() + 0.5), 0, 3)]);
                    };
                    show(time);
                    time->setCommitCallback([show](LLUICtrl* ctrl, const LLSD&) { show(static_cast<PsSlider*>(ctrl)); });
                }
                if (PsCheck* disabled = findChild<PsCheck>("show_disabled"))
                {
                    disabled->setCommitCallback([this](LLUICtrl* ctrl, const LLSD&)
                                                { setPartsEnabled(getChild<LLView>("sample"), !ctrl->getValue().asBoolean()); });
                }
                return true;
            }

        private:
            // The column of psstack.h as the photo tools will make it: the labels beside the controls, the star
            // and the marks before each row, a fold that opens without the rows being made again.
            void fillStack()
            {
                LLScrollContainer* scroll = findChild<LLScrollContainer>("stack_scroll");
                LLPanel* content = findChild<LLPanel>("stack_content");
                if (!scroll || !content)
                {
                    return;
                }
                static LLUICachedControl<S32> scrollbar_size("UIScrollbarSize", 0);
                const S32 width = llmax(1, scroll->getRect().getWidth() - (S32)scrollbar_size);
                Stack stack(content, width, sRows);
                stack.setLabelsBeside(true);

                int marks = 0;  // the next row's
                const std::string star_tip = getString("StackStarTip");
                const std::string changed_tip = getString("StackChangedTip");
                const std::string rebuild_tip = getString("StackRebuildTip");
                stack.setDecorator(
                    [&marks, star_tip, changed_tip, rebuild_tip](const plus::photomode::Item*, S32,
                                                                 std::vector<LLView*>& out) {
                        out.push_back(stack::makeStar(star_tip, false));
                        for (const auto& [text, tip, mark] : { std::make_tuple("*", &changed_tip, MARK_CHANGED),
                                                               std::make_tuple("!", &rebuild_tip, MARK_REBUILD) })
                        {
                            if (PsLabel* label = stack::makeMark(text, *tip))
                            {
                                label->setVisible((marks & mark) != 0);
                                out.push_back(label);
                            }
                        }
                        marks = 0;
                    },
                    stack::DECOR_STAR_W + 2 * stack::DECOR_MARK_W);

                StackValues& v = sValues;
                stack.subHeader(getString("StackHead"));
                marks = MARK_CHANGED;
                // With a "D" button (research/272): off its default (lit) and at it (dim).
                stack.slider(getString("StackExposure"), std::string(), -1.f, 1.f, 0.1f, 1, 1.f,
                             [&v]() { return v.exposure; }, [&v](float x) { v.exposure = x; }, 0, 0.f);
                stack.slider(getString("StackSoftness"), std::string(), 0.f, 1.f, 0.05f, 2, 1.f,
                             [&v]() { return v.softness; }, [&v](float x) { v.softness = x; }, 0, 0.5f);
                stack.check(getString("StackParticles"), std::string(), [&v]() { return v.particles; },
                            [&v](float x) { v.particles = x; });
                marks = MARK_REBUILD;
                stack.combo(getString("StackShadows"), std::string(),
                            { { 0.f, getString("StackShadows0") }, { 1.f, getString("StackShadows1") },
                              { 2.f, getString("StackShadows2") } },
                            [&v]() { return v.shadows; }, [&v](float x) { v.shadows = x; });
                std::vector<std::string> gaze;
                LLStringUtil::getTokens(getString("StackGazeChoices"), gaze, "|");
                stack.segment(getString("StackGaze"), std::string(), gaze, [&v]() { return v.gaze; },
                              [&v](float x) { v.gaze = x; });
                stack.text(getString("StackTime"), std::string(), getString("StackTimeHint"),
                           [&v]() { return v.time; }, [&v](const std::string& typed) { v.time = typed; return true; });
                if (PsCombo* look = stack.stepCombo(getString("StackLook"), std::string(), getString("StackLookPrev"),
                                                    getString("StackLookNext"), [](int step) {
                                                        sValues.look = llclamp(sValues.look + step, 0, 2);
                                                        if (LLUICtrl* combo = sLookCombo.get())
                                                        {
                                                            setComboIfChanged(static_cast<LLComboBox*>(combo), LLSD(sValues.look));
                                                        }
                                                    }))
                {
                    sLookCombo = look->getHandle();
                    for (S32 i = 0; i < 3; ++i)
                    {
                        look->add(getString("StackLook" + std::to_string(i)), LLSD(i));
                    }
                    look->setValue(LLSD(v.look));
                    look->setCommitCallback([](LLUICtrl* ctrl, const LLSD&) { sValues.look = ctrl->getValue().asInteger(); });
                }
                stack.foldHeader(getString("StackDetails"), false);
                stack.slider(getString("StackFog"), std::string(), 0.f, 1.f, 0.05f, 2, 1.f,
                             [&v]() { return v.fog; }, [&v](float x) { v.fog = x; });
                stack.check(getString("StackGlow"), std::string(), [&v]() { return v.glow; },
                            [&v](float x) { v.glow = x; });
                stack.endFold();
                stack.endSection();
                stack.finish();
                Stack::refresh(sRows);
            }

            // The sub-tabs over their pages (TabPages).
            void bindTabs()
            {
                LLView* box = findChildView("tabs_box");
                for (const char* name : { "display", "graphics", "other" })
                {
                    mTabs.add(box, std::string("tab_") + name, std::string("page_") + name);
                }
                mTabs.show(0);
            }

            TabPages mTabs;

            // A made-up evening picture's levels: a dark peak, a warm middle and a few highlights.
            void fillHistogram()
            {
                PsHistogram* histogram = findChild<PsHistogram>("histogram");
                if (!histogram)
                {
                    return;
                }
                constexpr int N = PsHistogram::BINS;
                F32 red[N], green[N], blue[N];
                auto bump = [](F32 x, F32 centre, F32 width) { const F32 d = (x - centre) / width; return std::exp(-d * d); };
                for (int i = 0; i < N; ++i)
                {
                    const F32 x = F32(i) / F32(N - 1);
                    red[i]   = 0.55f * bump(x, 0.18f, 0.10f) + 0.70f * bump(x, 0.52f, 0.12f) + 0.25f * bump(x, 0.86f, 0.05f);
                    green[i] = 0.70f * bump(x, 0.16f, 0.09f) + 0.45f * bump(x, 0.42f, 0.10f) + 0.15f * bump(x, 0.80f, 0.06f);
                    blue[i]  = 0.95f * bump(x, 0.22f, 0.11f) + 0.35f * bump(x, 0.38f, 0.08f) + 0.10f * bump(x, 0.70f, 0.08f);
                }
                histogram->setData(red, green, blue, N);
            }

            // All parts under view, but the "Show disabled" check itself (it is outside "sample").
            static void setPartsEnabled(LLView* view, bool enabled)
            {
                for (LLView* child : *view->getChildList())
                {
                    if (dynamic_cast<PsCtrl*>(child) || dynamic_cast<PsCombo*>(child))
                    {
                        child->setEnabled(enabled);
                    }
                    else if (isCompatWidget(child))
                    {
                        child->setEnabled(enabled);
                        continue;   // its official parts follow it (LLSliderCtrl::setEnabled ...)
                    }
                    setPartsEnabled(child, enabled);
                }
            }
        };

        void init()
        {
            style::applyColorTable();   // the colour table is read by now (LLAppViewer::initConfiguration)
            declareImages();
            LLFloaterReg::add(FLOATER_NAME, "floater_plus_ps_gallery.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<GalleryFloater>);
        }

        plus::FeatureRegistration sRegistration({ "psui", &init, {} });
    }
}
