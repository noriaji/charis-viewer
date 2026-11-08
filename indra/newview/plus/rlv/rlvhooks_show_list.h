/**
 * @file rlvhooks_show_list.h
 * @brief rlv-show hooks: E6-E14 agentui, appviewer, avatar lists, chat history/toast, avatar picker, avatar textures, and the nearby chat participants and mention picker (S-H1) (research/248). Included by rlvhooks.h.
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

// Declarations only, inside namespace plus::rlv (rlvhooks.h). Definitions: rlvhooks_show_list.cpp.
// Every hook is called as  "if (plus::rlv::enabled() && plus::rlv::xxx(...))".
// These files also use canShowMenuName() (wear, E49: @shownames does not hide this agent) and canStartIM() (comm).

// LLAgentUI::buildLocationString() (llagentui.cpp, E6): @showloc - true when the parcel and region names were replaced by
// the "hidden" texts (the caller then drops the coordinates from the format, as FS).
bool hideAgentLocation(std::string& strParcel, std::string& strRegion);

// LLAppViewer::getViewerInfo() (llappviewer.cpp, E7): the RLVa version ("RLV_VERSION"), and under @showloc the hidden region
// instead of the region, the position, the host and the SLURL. LLAppViewer::getViewerInfoString() then shows the position
// part without the coordinates (the missing "POSITION" tells it).
void filterViewerInfo(LLSD& sdInfo);

// @shownames is set (FS gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMES)): llavatarlist.cpp (E8) - no context menu and no
// double click in a list that hides names, llfloateravatarpicker.cpp (E13) - no "Near me" tab, llfloateravatartextures.cpp
// (E14) - the title is anonymous.
bool hasShowNames();
// The anonym of an avatar (FS RlvStrings::getAnonym(av_name)): the avatar lists (E8, E9), the avatar textures title (E14)
std::string anonymName(const LLAvatarName& avName);
// Changes whenever @shownames is set or cleared or its exceptions change. LLAvatarList::draw() (E8) compares it with what it
// saw last to update the names and the info buttons of a list that hides names (FS: a behaviour callback per list).
unsigned int showNamesGeneration();

// The line is an object's and chat.mURL is an "objectim" link. Two places make one: filterChat() (process_chat_from_simulator(),
// rlvhooks_core.cpp) only while @shownames or @showloc is on, and llimprocessing.cpp for every IM_FROM_TASK (official; with RLVa
// on its query also carries the name, the owner, the place and "rlv_shownames", filterObjectIM() having hidden the name and a
// nearby region first under @shownames / @showloc; the objectim inspector hides the owner when "rlv_shownames" is set, E22).
// LLChatHistory::appendMessage() (E10) and processChat()
// (E12) link the object's name to it instead of to the link LLViewerChat::getSenderSLURL() builds (which has the region name).
bool hasObjectIMURL(const LLChat& chat);

// LLFloaterIMNearbyChatHandler::processChat() (llfloaterimnearbychathandler.cpp, E12): @showloc / @shownames in the text of
// the lines process_chat_from_simulator() did not filter (objects' IMs, system lines; not agents' chat) and the mentions.
// Sets chat.mRlvLocFiltered / mRlvNamesFiltered.
void filterNearbyChatText(LLChat& chat, const LLSD& sdArgs);

// ---- S-H1 the participants of the nearby chat session (Conversations floater, nearby chat floater) and the mention picker.
// FS shows them in its own voice controls list with the names anonymized (setRlvCheckShowNames(is_local_chat)); Charis' list is
// LLConversationItemParticipant (llconversationmodel.cpp), which uses canShowMenuName() / anonymName() and anonymousNearbyMenuItems().
// Only the rows of the nearby chat session are changed: a one-on-one IM, a group or a conference shows its participants as usual
// (as FS, which only anonymizes its own nearby list).
// idSession is the session whose participant list the agents were picked from (the nearby chat session's id is null): true when it is
// the nearby chat session and the name of one of them is hidden by @shownames (LLConversationViewParticipant, llconversationview.cpp).
bool hidesNearbyParticipantNames(const LLUUID& idSession, const std::vector<LLUUID>& idAgents);
// LLFloaterIMContainer::doToParticipants() / enableContextMenuItem(item, uuids) (llfloaterimcontainer.cpp) are not told the session:
// their callers (the container's and LLFloaterIMSessionTab's doToSelected() / enableContextMenuItem(), llfloaterimsessiontab.cpp) say
// with this scope whether the selection is in the nearby chat session's list. Outside such a scope (the one-on-one IM floater) it is
// not. Scopes nest (the previous value comes back on every path).
class NearbyParticipantsScope
{
public:
    explicit NearbyParticipantsScope(bool fNearbySession);
    ~NearbyParticipantsScope();
    NearbyParticipantsScope(const NearbyParticipantsScope&) = delete;
    NearbyParticipantsScope& operator=(const NearbyParticipantsScope&) = delete;
private:
    bool mPrevious;
};
// Inside a NearbyParticipantsScope for the nearby chat session: the name of one of the agents is hidden by @shownames (only the
// nearby people menu's commands are left for them).
bool hidesNearbyParticipantNames(const std::vector<LLUUID>& idAgents);
// rlvui_show.cpp, when @shownames is set or cleared or its exceptions change: the names of the nearby chat session's participants
// are looked up again (anonym or name). One bool with RLVa off.
void refreshConversationNames();
// LLFloaterChatMentionPicker::updateSessionID() (llfloaterchatmentionpicker.cpp): the session the mention helper works for. For the
// nearby chat session it puts in LLChatMentionHelper's gate (gPlusChatMentionGate, llchatmentionhelper.cpp), which keeps the
// picker closed while @shownames is on; for other sessions it takes the gate out.
void setMentionSession(const LLUUID& idSession);
