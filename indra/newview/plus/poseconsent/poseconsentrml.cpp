/**
 * @file poseconsentrml.cpp
 * @brief Who may pose my avatar: the window made with RmlUi (research/386).
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

// The same window as poseconsentfloater.cpp's (the XUI one, frozen: safe mode and PlusRmlUiWindows off), with its content
// app_settings/plus/rmlui/pose_consent.rml. The rows and the "list is out of date" stamp are the shared ones of
// poseconsentui.h (grantedRowText, grantedStamp), so that the two windows cannot differ. The list is a
// plus::rmlui::RmlList; the names in it come from the name service (poseconsentui.h avatarName) and are put in as text
// (escaped), never as RML.

#include "llviewerprecompiledheaders.h"

#include "poseconsent.h"
#include "poseconsentui.h"
#include "posereflect.h"

#include "plus/rmlui/rmlfloater.h"
#include "plus/rmlui/rmlhost.h"
#include "plus/rmlui/rmllist.h"

#include "llfloaterreg.h"

#include <vector>

namespace plus::poseconsent
{
    namespace
    {
        class PoseConsentRmlFloater : public plus::rmlui::RmlFloater
        {
            friend class ::LLFloaterReg;

        public:
            ~PoseConsentRmlFloater() override
            {
                closeDocument();    // before the list goes
            }

            void onOpen(const LLSD& key) override
            {
                RmlFloater::onOpen(key);
                mStamp = GrantedStamp();    // made again at the next frame
            }

            void draw() override
            {
                if (mStamp != grantedStamp())
                {
                    mRebuild = true;
                    if (LLRmlView* view = host())
                    {
                        view->markDirty();
                    }
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
                mList.attach(document, "list", true);
                mList.setEmptyText(getString("nobody"));
                for (const char* event : { "click", "keydown", "mousedown" })
                {
                    view->listen(document.GetElementById("list"), event);
                }
                for (const char* id : { "withdraw", "withdraw_all", "reflect_stop" })
                {
                    view->listen(document.GetElementById(id), "click");
                }
                mRebuild = true;
                mButtons = true;
            }

            void beforeUpdate() override
            {
                if (mRebuild)
                {
                    mRebuild = false;
                    rebuild();
                }
                mList.apply();
                if (mButtons || mButtonVersion != mList.selectionVersion())
                {
                    mButtons = false;
                    mButtonVersion = mList.selectionVersion();
                    updateButtons();
                }
            }

            void afterUpdate() override
            {
                mList.afterUpdate();
            }

            // In RmlUi's event: no element is removed here (the rows change in beforeUpdate).
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
                if (id == "list")
                {
                    mList.handleEvent(event);
                }
                else if (id == "withdraw")
                {
                    withdrawPeers(selectedPeers());
                }
                else if (id == "reflect_stop")
                {
                    // Only what shows their poses on our screen (research/374b); what they may do on their own screen stays
                    for (const LLUUID& peer : selectedPeers())     // copied first: withdrawing changes the list
                    {
                        reflect::withdraw(peer);
                    }
                }
                else if (id == "withdraw_all")
                {
                    std::vector<LLUUID> peers;
                    for (const Granted& granted : grantedList())
                    {
                        peers.push_back(granted.peer);
                    }
                    withdrawPeers(peers);   // one notice for all who could not be told
                }
                // The enabling of the buttons follows at the next update (the selection, the list, who may reflect).
                mButtons = true;
            }

            void unbind() override
            {
                mList.detach();
            }

        private:
            explicit PoseConsentRmlFloater(const LLSD& key)
            :   RmlFloater(key)
            {
            }

            std::vector<LLUUID> selectedPeers() const
            {
                std::vector<LLUUID> peers;
                for (const std::string& key : mList.selectedKeys())
                {
                    peers.emplace_back(key);
                }
                return peers;
            }

            void rebuild()
            {
                mStamp = grantedStamp();
                GrantedRowStrings strings = { getString("loading"), getString("withdraw_waiting_row"),
                                              getString("reflect_now_row"), getString("reflect_row") };
                std::vector<plus::rmlui::RmlList::Row> rows;
                for (const Granted& granted : grantedList())
                {
                    rows.push_back({ granted.peer.asString(), grantedRowText(granted, strings), false });
                }
                mList.setRows(std::move(rows));
                mButtons = true;
            }

            void updateButtons()
            {
                LLRmlView* view = host();
                Rml::ElementDocument* document = view ? view->getDocument() : nullptr;
                if (!document)
                {
                    return;
                }
                bool reflected = false;
                for (const LLUUID& peer : selectedPeers())
                {
                    reflected |= reflect::mayReflect(peer);
                }
                plus::rmlui::setElementDisabled(document->GetElementById("withdraw"), mList.selectedCount() == 0);
                plus::rmlui::setElementDisabled(document->GetElementById("withdraw_all"), grantedList().empty());
                plus::rmlui::setElementDisabled(document->GetElementById("reflect_stop"), !reflected);
            }

            plus::rmlui::RmlList mList;
            GrantedStamp mStamp;            // never a stamp at first (all ~0): the first draw builds the list
            bool mRebuild = false;
            bool mButtons = false;
            unsigned mButtonVersion = 0;
        };
    }

    void addRmlFloater()
    {
        LLFloaterReg::add(FLOATER_NAME, "floater_plus_pose_consent_rml.xml",
                          (LLFloaterBuildFunc)&LLFloaterReg::build<PoseConsentRmlFloater>);
    }
}
