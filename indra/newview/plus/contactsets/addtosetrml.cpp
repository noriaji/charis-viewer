/**
 * @file addtosetrml.cpp
 * @brief Adding avatars to a contact set: the window made with RmlUi (research/386).
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

// The same window as fsfloateraddtocontactset.cpp's (the XUI one, frozen: safe mode and PlusRmlUiWindows off), with its content
// app_settings/plus/rmlui/contact_set_add.rml. The key, the choice of sets and the adding are plus::contactsets::
// AddToSetTarget (addtoset.h), shared with the XUI window. The avatar of a single target is shown as the XUI text's link
// was: the label the URL registry gives (it follows the name cache and RLVa's hiding), and a click opens the same
// link (LLUrlAction::clickAction) -- the link is made here from the id, never taken from outside.
// The drop-down list is the document's <select>; its list is shown outside the window by RmlFloater's SelectPopup (rmlpopup.h).

#include "llviewerprecompiledheaders.h"

#include "addtoset.h"
#include "addtosetrml.h"
#include "contactsets.h"
#include "lggcontactsets.h"

#include "plus/rmlui/rmlfloater.h"
#include "plus/rmlui/rmlhost.h"
#include "plus/rmlui/rmllist.h"
#include "plus/rmlui/rmlmenu.h"

#include "llfloaterreg.h"
#include "lluictrl.h"
#include "llurlaction.h"
#include "llurlmatch.h"
#include "llurlregistry.h"

#include <boost/bind.hpp>

namespace plus::contactsets
{
    namespace
    {
        class AddToSetRmlFloater : public plus::rmlui::RmlFloater
        {
            friend class ::LLFloaterReg;

        public:
            ~AddToSetRmlFloater() override
            {
                closeDocument();    // before the connection and the target go
                mSetsConnection.disconnect();
            }

            bool postBuild() override
            {
                mSetsConnection = LGGContactSets::getInstance()->setContactSetChangeCallback(
                    [this](LGGContactSets::EContactSetUpdate type)
                    {
                        if (type == LGGContactSets::UPDATED_LISTS)
                        {
                            mSetsDirty = true;
                            if (LLRmlView* view = host())
                            {
                                view->markDirty();
                            }
                        }
                    });
                const bool built = RmlFloater::postBuild();
                if (!mTarget.multiple())
                {
                    findLabel();
                }
                return built;
            }

            void draw() override
            {
                if (mClose)
                {
                    mClose = false;
                    closeFloater();     // not inside RmlUi's event (the buttons only ask)
                    return;
                }
                RmlFloater::draw();
            }

        protected:
            void afterLoad(Rml::ElementDocument& document) override
            {
                LLRmlView* view = host();
                if (!view)
                {
                    return;
                }
                for (const char* id : { "add_set", "add", "cancel", "text" })
                {
                    view->listen(document.GetElementById(id), "click");
                }
                view->listen(document.GetElementById("text"), "mousedown");     // the right press on the name: its menu
                mSetsDirty = true;
                mTextDirty = true;
            }

            void beforeUpdate() override
            {
                LLRmlView* view = host();
                Rml::ElementDocument* document = view ? view->getDocument() : nullptr;
                if (!document)
                {
                    return;
                }
                if (mSetsDirty)
                {
                    mSetsDirty = false;
                    fillSets(*document);
                }
                if (mTextDirty)
                {
                    mTextDirty = false;
                    showText(*document);
                }
            }

            // In RmlUi's event: nothing is removed from the document here.
            void onEvent(Rml::Event& event) override
            {
                const Rml::Element* element = event.GetCurrentElement();
                if (!element)
                {
                    return;
                }
                if (event.GetId() == Rml::EventId::Click && plus::rmlui::isElementDisabled(element))
                {
                    return;     // a disabled button takes no click (RmlUi 6.3 does not stop it)
                }
                const std::string id = element->GetId();
                if (id == "add_set")
                {
                    askAddSet();
                }
                else if (id == "cancel")
                {
                    mClose = true;
                }
                else if (id == "add")
                {
                    LLRmlView* view = host();
                    Rml::ElementDocument* document = view ? view->getDocument() : nullptr;
                    Rml::Element* sets = document ? document->GetElementById("sets") : nullptr;
                    auto* select = sets ? rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(sets) : nullptr;
                    if (select && mTarget.add(select->GetValue()))
                    {
                        mClose = true;
                    }
                }
                else if (id == "text")
                {
                    // the avatar's name in the sentence
                    const Rml::Element* target = event.GetTargetElement();
                    if (target && target->GetId() == "who" && !mTarget.multiple())
                    {
                        if (event.GetId() == Rml::EventId::Click)
                        {
                            LLUrlAction::clickAction(mTarget.agentLink(), true);
                        }
                        else if (event.GetId() == Rml::EventId::Mousedown && event.GetParameter<int>("button", 0) == 1)
                        {
                            event.StopPropagation();    // the host has nothing for a name: this menu is the window's
                            showNameMenu(event);
                        }
                    }
                }
            }

            void unbind() override
            {
                mMenu.hide();
                mSetsConnection.disconnect();
            }

        private:
            explicit AddToSetRmlFloater(const LLSD& target)
            :   RmlFloater(target),
                mTarget(target)
            {
            }

            // The right press on the name: the menu the XUI text gave a link (LLTextBase::createUrlContextMenu: the file the URL
            // registry names, "View Profile", "Send IM...", "Add Friend...", "Remove Friend...", "Report Abuse", "Copy Name" and
            // "Copy SLurl"; the XUI text of this window had no friend check, so both friend items are on). The URL is made here
            // from the id (agentLink()), never taken from the document.
            void showNameMenu(Rml::Event& event)
            {
                LLRmlView* view = host();
                if (!view)
                {
                    return;
                }
                const std::string url = mTarget.agentLink();
                LLUrlMatch match;
                if (!LLUrlRegistry::instance().findUrl(url, match) || match.getMenuName().empty())
                {
                    return;
                }
                LLUICtrl::CommitCallbackRegistry::ScopedRegistrar registrar;     // alive while the menu is made
                registrar.add("Url.ShowProfile", boost::bind(&LLUrlAction::showProfile, url));
                registrar.add("Url.SendIM", boost::bind(&LLUrlAction::sendIM, url));
                registrar.add("Url.AddFriend", boost::bind(&LLUrlAction::addFriend, url));
                registrar.add("Url.RemoveFriend", boost::bind(&LLUrlAction::removeFriend, url));
                registrar.add("Url.ReportAbuse", boost::bind(&LLUrlAction::reportAbuse, url));
                registrar.add("Url.CopyLabel", boost::bind(&LLUrlAction::copyLabelToClipboard, url));
                registrar.add("Url.CopyUrl", boost::bind(&LLUrlAction::copyURLToClipboard, url));
                if (mMenu.create(match.getMenuName()))
                {
                    mMenu.open(*view, Rml::Vector2f(event.GetParameter<float>("mouse_x", 0.f),
                                                    event.GetParameter<float>("mouse_y", 0.f)));
                }
            }

            // The avatar's label as the link of the XUI text would show it (the name cache answers later: the callback).
            void findLabel()
            {
                LLHandle<LLView> handle = getHandle();
                LLUrlMatch match;
                LLUrlRegistry::instance().findUrl(
                    mTarget.agentLink(), match,
                    [handle](const std::string&, const std::string& label, const std::string&)
                    {
                        if (auto* floater = dynamic_cast<AddToSetRmlFloater*>(handle.get()))
                        {
                            floater->mLabel = label;
                            floater->mTextDirty = true;
                            if (LLRmlView* view = floater->host())
                            {
                                view->markDirty();
                            }
                        }
                    });
                mLabel = match.getLabel();
            }

            void showText(Rml::ElementDocument& document)
            {
                Rml::Element* text = document.GetElementById("text");
                if (!text)
                {
                    return;
                }
                std::string rml;
                if (mTarget.multiple())
                {
                    LLStringUtil::format_map_t args;
                    args["COUNT"] = llformat("%d", mTarget.count());
                    rml = plus::rmlui::escapeRml(getString("text_add_multiple", args));
                }
                else
                {
                    // the sentence with the label where [NAME] stands (each language places it)
                    const std::string sentence = getString("text_add_single");
                    const size_t at = sentence.find("[NAME]");
                    if (at == std::string::npos)
                    {
                        rml = plus::rmlui::escapeRml(sentence);
                    }
                    else
                    {
                        rml = plus::rmlui::escapeRml(sentence.substr(0, at)) + "<span id=\"who\" class=\"ps-link\">" +
                              plus::rmlui::escapeRml(mLabel) + "</span>" +
                              plus::rmlui::escapeRml(sentence.substr(at + 6));
                    }
                }
                if (rml != mShownText)
                {
                    mShownText = rml;
                    text->SetInnerRML(rml);
                }
            }

            void fillSets(Rml::ElementDocument& document)
            {
                Rml::Element* sets = document.GetElementById("sets");
                auto* select = sets ? rmlui_dynamic_cast<Rml::ElementFormControlSelect*>(sets) : nullptr;
                if (!select)
                {
                    return;
                }
                const std::string selected = select->GetValue();
                std::vector<plus::rmlui::SelectOption> options;
                for (const std::string& name : mTarget.setNames())
                {
                    options.push_back({ name, name });
                }
                const bool none = options.empty();
                if (none)
                {
                    options.push_back({ std::string(), getString("no_sets") });
                }
                plus::rmlui::fillSelect(*select, options, selected);
                plus::rmlui::setElementDisabled(document.GetElementById("add"), none);
            }

            plus::contactsets::AddToSetTarget mTarget;
            plus::rmlui::ContextMenu mMenu;     // the name's menu (closed by its own destructor)
            boost::signals2::connection mSetsConnection;
            std::string mLabel;             // the avatar's label of a single target
            std::string mShownText;
            bool mSetsDirty = false;
            bool mTextDirty = false;
            bool mClose = false;
        };
    }

    void addRmlAddToSetFloater(const char* floater_name)
    {
        LLFloaterReg::add(floater_name, "floater_plus_contact_set_add_rml.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<AddToSetRmlFloater>);
    }
}
