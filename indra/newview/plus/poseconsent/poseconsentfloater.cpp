/**
 * @file poseconsentfloater.cpp
 * @brief The reminder of who may pose our avatar (research/336 4.1: the people we allowed, with Withdraw, while
 *        the poser is closed too). floater_plus_pose_consent.xml.
 *
 * The list is made again only when the ledger's list changed (grantedGeneration()), a name arrived, or RLVa's
 * @shownames changed (the names hidden or shown again): draw() compares three numbers otherwise.
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

#include "poseconsent.h"
#include "poseconsentui.h"
#include "posereflect.h"

#include "plus/poseshare/poseshare.h"

#include "plus/rlv/rlvhooks.h"

#include "llfloater.h"
#include "llfloaterreg.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "lluictrl.h"

#include <vector>

namespace plus::poseconsent
{
    namespace
    {
        unsigned sNamesArrived = 0;     // bumped by a name that arrived (the list shows it then)

        void onNameArrived()
        {
            ++sNamesArrived;
        }

        // RLVa's @shownames and its exceptions as they were when the list was made (0 with RLVa off).
        unsigned showNamesGeneration()
        {
            return plus::rlv::enabled() ? plus::rlv::showNamesGeneration() : 0;
        }

        class PoseConsentFloater : public LLFloater
        {
        public:
            explicit PoseConsentFloater(const LLSD& key) : LLFloater(key) {}

            bool postBuild() override
            {
                mList = getChild<LLScrollListCtrl>("granted_list");
                mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateButtons(); });
                mWithdraw = getChild<LLUICtrl>("withdraw_btn");
                mWithdrawAll = getChild<LLUICtrl>("withdraw_all_btn");
                mReflectStop = getChild<LLUICtrl>("reflect_stop_btn");     // research/374b
                mReflectStop->setCommitCallback([this](LLUICtrl*, const LLSD&) { stopReflectSelected(); });
                mWithdraw->setCommitCallback([this](LLUICtrl*, const LLSD&) { withdrawSelected(); });
                mWithdrawAll->setCommitCallback([](LLUICtrl*, const LLSD&) { withdrawAll(); });
                return true;
            }

            void onOpen(const LLSD& key) override
            {
                LLFloater::onOpen(key);
                rebuild();
            }

            void draw() override
            {
                if (mGeneration != grantedGeneration() || mReflectGeneration != reflect::grantedGeneration() ||
                    mPushedGeneration != plus::poseshare::pushedGeneration() || mNames != sNamesArrived || mShowNames != showNamesGeneration())
                {
                    rebuild();
                }
                LLFloater::draw();
            }

        private:
            void rebuild()
            {
                mGeneration = grantedGeneration();
                mReflectGeneration = reflect::grantedGeneration();
                mPushedGeneration = plus::poseshare::pushedGeneration();
                mNames = sNamesArrived;
                mShowNames = showNamesGeneration();

                uuid_vec_t selected;
                for (LLScrollListItem* item : mList->getAllSelected())
                {
                    selected.push_back(item->getUUID());
                }
                mList->deleteAllItems();
                for (const Granted& granted : grantedList())
                {
                    std::string name;
                    if (!avatarName(granted.peer, name, &onNameArrived))
                    {
                        name = getString("loading");
                    }
                    if (granted.withdrawPending)
                    {
                        // Withdrawn, the withdraw not sent yet (the bridge busy or not there). The whole row is the
                        // language's string, so that each places its own space and brackets.
                        LLStringUtil::format_map_t args;
                        args["[NAME]"] = name;
                        name = getString("withdraw_waiting_row", args);
                    }
                    else if (plus::poseshare::isPushedBy(granted.peer))
                    {
                        LLStringUtil::format_map_t args;     // their pose is on our avatar now
                        args["[NAME]"] = name;
                        name = getString("reflect_now_row", args);
                    }
                    else if (reflect::mayReflect(granted.peer))
                    {
                        LLStringUtil::format_map_t args;     // allowed to show their poses on our screen
                        args["[NAME]"] = name;
                        name = getString("reflect_row", args);
                    }
                    LLSD row;
                    row["value"] = granted.peer;
                    row["columns"][0]["column"] = "name";
                    row["columns"][0]["value"] = name;
                    mList->addElement(row);
                }
                if (mList->isEmpty())
                {
                    mList->setCommentText(getString("nobody"));
                }
                else if (!selected.empty())
                {
                    mList->selectMultiple(selected);
                }
                updateButtons();
            }

            void updateButtons()
            {
                mWithdraw->setEnabled(mList->getFirstSelected() != nullptr);
                mWithdrawAll->setEnabled(!mList->isEmpty());
                bool reflected = false;
                for (LLScrollListItem* item : mList->getAllSelected())
                {
                    reflected |= reflect::mayReflect(item->getUUID());
                }
                mReflectStop->setEnabled(reflected);
            }

            // Only what shows their poses on our screen (research/374b); what they may do on their own screen stays
            void stopReflectSelected()
            {
                uuid_vec_t peers;   // copied first: withdrawing changes the list this reads
                for (LLScrollListItem* item : mList->getAllSelected())
                {
                    peers.push_back(item->getUUID());
                }
                for (const LLUUID& peer : peers)
                {
                    reflect::withdraw(peer);
                }
            }

            void withdrawSelected()
            {
                uuid_vec_t peers;   // copied first: withdrawing changes the list this reads
                for (LLScrollListItem* item : mList->getAllSelected())
                {
                    peers.push_back(item->getUUID());
                }
                withdrawPeers(peers);
            }

            static void withdrawAll()
            {
                uuid_vec_t peers;
                for (const Granted& granted : grantedList())
                {
                    peers.push_back(granted.peer);
                }
                withdrawPeers(peers);   // one notice for all who could not be told
            }

            LLScrollListCtrl* mList = nullptr;
            LLUICtrl* mWithdraw = nullptr;
            LLUICtrl* mWithdrawAll = nullptr;
            LLUICtrl* mReflectStop = nullptr;
            unsigned mReflectGeneration = ~0u;
            unsigned mPushedGeneration = ~0u;
            unsigned mGeneration = ~0u;     // never a generation at first: the first draw builds the list
            unsigned mNames = ~0u;
            unsigned mShowNames = ~0u;
        };
    }

    void registerFloater()
    {
        LLFloaterReg::add(FLOATER_NAME, "floater_plus_pose_consent.xml", (LLFloaterBuildFunc)&LLFloaterReg::build<PoseConsentFloater>);
    }

    void showGrantedFloater(bool take_focus)
    {
        LLFloaterReg::showInstance(FLOATER_NAME, LLSD(), take_focus);
    }
}
