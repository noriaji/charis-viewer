/**
 * @file objectfx.cpp
 * @brief The "Object appear / disappear effects" group of Advanced Graphics (Charis tab): the Charis
 *        menu's entry that opens it, and the colour swatch of a Color3 setting.
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

// Charis's own (research/256 materialize, research/257 derez). The UI only: the effects read their
// settings themselves (plus/materialize, plus/derez). No official file is touched here.
//
// - Plus.ObjectFx.ShowPrefs (Preferences > Charis, panel_preferences_charis.xml): opens Advanced Graphics on the Charis tab and scrolls the
//   tab to the group's header (ObjectFxText in floater_plus_prefs_graphics_advanced.xml), down to its last row
//   (PlusDerezPalette). At the default size the whole tab fits and nothing scrolls; in a shortened floater the
//   group comes into view.
// - Plus.ObjectFx.InitColor3 / SetColor3 (the swatch's init and commit callbacks; InitColor3 also gives
//   the swatch its select and cancel callbacks, which XUI cannot name): a color_swatch bound by
//   control_name to a Color3 setting reads the three-element value as an LLColor4 whose alpha is sd[3] = 0, so it would draw see-through and the
//   picker would keep alpha 0. The swatch is therefore not bound; these callbacks show the setting with
//   alpha 1 and write the colour back as a Color3. The parameter is "<Color3 setting> [<palette setting>]".
//   - While the picker is open, a change (the picker's "apply now") is written to the colour setting
//     only; the palette is not touched, so there is nothing of it to put back.
//   - Select: the colour is written and, when it differs from the colour before this pick, the palette
//     setting goes to PALETTE_CUSTOM (picking a colour is enough to use it). The same colour: the
//     palette stays as it is. "Before this pick": when this same picker has written a changed colour
//     while open, the colour before its first such write (sChangedBy; the picker's original colour
//     cannot be used then, as clicking the swatch again while the picker is open makes the picker
//     take the colour of then as its original); otherwise the picker's original colour (taken at every
//     opening) and the setting's value (the system's colour dialog, UseDefaultColorPicker, writes
//     nothing while it is open and gives the picker the picked colour as its original), the pick
//     differing when it differs from either. Colours are the same when every channel is less than
//     1/255 apart (the system's dialog truncates to 0-255, the picker does not round at all).
//     sChangedBy holds the picker's handle, so a mark left by a picker closed with its close box does
//     not count for the next picker; select and cancel remove it.
//   - Cancel (the picker's Cancel, the swatch disabled by its check box, the swatch destroyed): the
//     picker has put its colour from before the pick back into the swatch; it is written back to the
//     setting, the palette was never changed. The picker's own close box, and Advanced Graphics being
//     closed (single_instance: it is only hidden, and the picker, its dependent floater, is closed as by
//     its close box), keep the colour applied so far, as the official swatches do, and leave the palette.
//   The swatch follows later changes of the setting (a preset, the debug settings) through the
//   setting's signal; one connection per setting, made again when the floater is built again.
//   Not bound means: not in the graphics presets the user saves and not put back by the floater's
//   Cancel (a matter of taste, as the Look; the palette and the rest of the group are bound and are).

#include "llviewerprecompiledheaders.h"

#include "plus.h"
#include "plus/objectfx/objectfx_palette.h"
#include "plus/photoscreen/psui/psstyle.h"

#include "llcolorswatch.h"
#include "llcontrol.h"
#include "llfloatercolorpicker.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llscrollcontainer.h"
#include "lltabcontainer.h"
#include "lluictrl.h"
#include "llviewercontrol.h"

#include <map>

namespace plus
{
    namespace objectfx
    {
        namespace
        {
            constexpr const char* FLOATER_NAME = "prefs_graphics_advanced";
            constexpr const char* TABS_NAME = "graphics_tabs";
            constexpr const char* TAB_NAME = "Charis panel";
            constexpr const char* GROUP_HEADER_NAME = "ObjectFxText";

            // The group's last row (found anywhere under the header's parent, so a row moved into a container
            // in the XML still counts): the header down to its bottom is brought into view, with the room of a
            // gap between panels (research/275 L077).
            constexpr const char* GROUP_LAST_ROW_NAME = "PlusDerezPalette";
            constexpr S32 GROUP_SHOW_MARGIN = plus::psui::style::PANEL_GAP;

            // One connection per Color3 setting shown by a swatch; replaced when the floater is built again.
            std::map<std::string, boost::signals2::scoped_connection> sSwatchConnections;

            // By Color3 setting: the picker that has written a changed colour to it while open, and the
            // setting's colour before that picker's first changed write.
            struct ChangeMark
            {
                LLHandle<LLFloater> picker;
                LLColor3            before;
            };
            std::map<std::string, ChangeMark> sChangedBy;

            void showPrefs()
            {
                LLFloater* floater = LLFloaterReg::showInstance(FLOATER_NAME);
                if (!floater)
                {
                    return;
                }
                if (LLTabContainer* tabs = floater->findChild<LLTabContainer>(TABS_NAME))
                {
                    tabs->selectTabByName(TAB_NAME);
                }
                LLView* header = floater->findChild<LLView>(GROUP_HEADER_NAME);
                if (!header || !header->getParent())
                {
                    return;
                }
                // The header's parent is the panel inside the tab's scroll_container: scrollToShowRect()
                // takes a rect in that panel's coordinates.
                LLScrollContainer* scroll = header->getParentByType<LLScrollContainer>();
                if (!scroll)
                {
                    return;
                }
                LLView* panel = header->getParent();
                const LLRect& rect = header->getRect();
                S32 bottom = rect.mBottom;
                if (const LLView* last = panel->findChild<LLView>(GROUP_LAST_ROW_NAME))
                {
                    // In the panel's coordinates (the row may sit in a container under it).
                    LLRect last_rect;
                    if (last->localRectToOtherView(last->getLocalRect(), &last_rect, panel))
                    {
                        bottom = llmin(bottom, last_rect.mBottom - GROUP_SHOW_MARGIN);
                    }
                }
                scroll->scrollToShowRect(LLRect(rect.mLeft, rect.mTop, rect.mRight, bottom));
            }

            // The swatch and the settings its callbacks' parameter "<Color3 setting> [<palette setting>]"
            // names; palette is null when none is named or it does not exist.
            struct SwatchTarget
            {
                LLColorSwatchCtrl*  swatch = nullptr;
                LLControlVariable*  color = nullptr;
                LLControlVariable*  palette = nullptr;

                SwatchTarget(LLUICtrl* ctrl, const LLSD& param)
                    : swatch(dynamic_cast<LLColorSwatchCtrl*>(ctrl))
                {
                    const std::string text = param.asString();
                    const size_t space = text.find(' ');
                    color = gSavedSettings.getControl(text.substr(0, space));
                    if (space != std::string::npos)
                    {
                        palette = gSavedSettings.getControl(text.substr(space + 1));
                    }
                }

                bool valid() const { return swatch && color; }
            };

            LLColor4 opaqueColor(const LLControlVariable& control)
            {
                return LLColor4(LLColor3(control.getValue()), 1.f);
            }

            void selectColor3(LLUICtrl* ctrl, const LLSD& param);
            void cancelColor3(LLUICtrl* ctrl, const LLSD& param);

            void initColor3(LLUICtrl* ctrl, const LLSD& param)
            {
                const SwatchTarget target(ctrl, param);
                if (!target.valid())
                {
                    LL_WARNS("Plus") << "objectfx: no colour swatch or no setting for " << param.asString() << LL_ENDL;
                    return;
                }
                target.swatch->setOriginal(opaqueColor(*target.color));
                target.swatch->setOnSelectCallback([param](LLUICtrl* swatch, const LLSD&) { selectColor3(swatch, param); });
                target.swatch->setOnCancelCallback([param](LLUICtrl* swatch, const LLSD&) { cancelColor3(swatch, param); });

                LLHandle<LLUICtrl> handle = target.swatch->getHandle();
                sSwatchConnections[target.color->getName()] = target.color->getSignal()->connect(
                    [handle](LLControlVariable* changed, const LLSD&, const LLSD&)
                    {
                        if (LLColorSwatchCtrl* shown = dynamic_cast<LLColorSwatchCtrl*>(handle.get()))
                        {
                            const LLColor4 color = opaqueColor(*changed);
                            if (shown->get() != color)
                            {
                                // from_event: shown only, the setting has it; an open picker keeps what is
                                // being picked in it (update_picker false)
                                shown->set(color, false, true);
                            }
                        }
                    });
            }

            // The swatch's picker (a dependent floater of the swatch's floater, a child of gFloaterView), or null.
            LLFloaterColorPicker* findPicker(const LLColorSwatchCtrl* swatch)
            {
                for (LLView* view : *gFloaterView->getChildList())
                {
                    LLFloaterColorPicker* picker = dynamic_cast<LLFloaterColorPicker*>(view);
                    if (picker && picker->getSwatch() == swatch)
                    {
                        return picker;
                    }
                }
                return nullptr;
            }

            // The same colour as the colour dialogs give it: every channel less than 1/255 apart (a dialog
            // that truncates to 0-255 and one that rounds both count as the same colour).
            bool sameColor8(const LLColor3& a, const LLColor3& b)
            {
                for (S32 i = 0; i < 3; ++i)
                {
                    if (fabsf(a.mV[i] * 255.f - b.mV[i] * 255.f) >= 1.f)
                    {
                        return false;
                    }
                }
                return true;
            }

            void writeColor(const SwatchTarget& target)
            {
                target.color->setValue(LLColor3(target.swatch->get()).getValue());
            }

            void setColor3(LLUICtrl* ctrl, const LLSD& param)
            {
                const SwatchTarget target(ctrl, param);
                if (!target.valid())
                {
                    return;
                }
                const LLColor3 current(target.color->getValue());
                if (!sameColor8(LLColor3(target.swatch->get()), current))
                {
                    if (LLFloaterColorPicker* picker = findPicker(target.swatch))
                    {
                        ChangeMark& mark = sChangedBy[target.color->getName()];
                        if (mark.picker != picker->getHandle()) // the first changed write of this picker
                        {
                            mark.picker = picker->getHandle();
                            mark.before = current;
                        }
                    }
                }
                writeColor(target);
            }

            // Whether this pick changed the colour (the comment at the top), and the pick's mark removed.
            bool takePickChanged(const SwatchTarget& target)
            {
                const LLColor3 picked(target.swatch->get());
                LLFloaterColorPicker* picker = findPicker(target.swatch);
                const auto mark = sChangedBy.find(target.color->getName());
                bool changed;
                if (mark != sChangedBy.end() && picker && mark->second.picker == picker->getHandle())
                {
                    changed = !sameColor8(picked, mark->second.before);
                }
                else
                {
                    changed = !sameColor8(picked, LLColor3(target.color->getValue())); // the system's dialog
                    if (picker)
                    {
                        F32 r, g, b;
                        picker->getOrigRgb(r, g, b);
                        changed = changed || !sameColor8(picked, LLColor3(r, g, b)); // applied while open
                    }
                }
                if (mark != sChangedBy.end())
                {
                    sChangedBy.erase(mark);
                }
                return changed;
            }

            void selectColor3(LLUICtrl* ctrl, const LLSD& param)
            {
                const SwatchTarget target(ctrl, param);
                if (!target.valid())
                {
                    return;
                }
                const bool changed = takePickChanged(target);
                writeColor(target);
                if (changed && target.palette)
                {
                    target.palette->setValue(LLSD(PALETTE_CUSTOM));
                }
            }

            void cancelColor3(LLUICtrl* ctrl, const LLSD& param)
            {
                const SwatchTarget target(ctrl, param);
                if (!target.valid())
                {
                    return;
                }
                // the swatch holds the picker's colour from before the pick again
                sChangedBy.erase(target.color->getName());
                writeColor(target);
            }

            void init()
            {
                // Button: skins/default/xui/<lang>/panel_preferences_charis.xml (b_objectfx).
                // Swatches: skins/default/xui/<lang>/floater_plus_prefs_graphics_advanced.xml
                // (PlusMaterializeColor, PlusDerezColor, each in a glass box).
                LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.ObjectFx.ShowPrefs",
                    [](LLUICtrl*, const LLSD&) { showPrefs(); });
                LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.ObjectFx.InitColor3", &initColor3);
                LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.ObjectFx.SetColor3", &setColor3);
            }

            plus::FeatureRegistration sRegistration({ "objectfx", &init });
        }
    }
}
