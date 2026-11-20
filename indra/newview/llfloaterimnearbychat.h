/**
 * @file llfloaterimnearbychat.h
 * @brief LLFloaterIMNearbyChat class definition
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLFLOATERIMNEARBYCHAT_H
#define LL_LLFLOATERIMNEARBYCHAT_H

#include "llfloaterimsessiontab.h"
#include "llcombobox.h"
#include "llgesturemgr.h"
#include "llchat.h"
#include "llvoiceclient.h"
#include "lloutputmonitorctrl.h"
#include "llspeakers.h"
#include "llscrollbar.h"
#include "llviewerchat.h"
#include "llpanel.h"

class LLResizeBar;
// <plus> chat-channel-select
class LLSpinCtrl;
class LLButton;
// </plus>

class LLFloaterIMNearbyChat
    :   public LLFloaterIMSessionTab
{
public:
    // constructor for inline chat-bars (e.g. hosted in chat history window)
    LLFloaterIMNearbyChat(const LLSD& key = LLSD(LLUUID()));
    ~LLFloaterIMNearbyChat() {}

    static LLFloaterIMNearbyChat* buildFloater(const LLSD& key);

    /*virtual*/ bool postBuild();
    /*virtual*/ void onOpen(const LLSD& key);
    /*virtual*/ void onClose(bool app_quitting);
    /*virtual*/ void setVisible(bool visible);
    /*virtual*/ void setVisibleAndFrontmost(bool take_focus=true, const LLSD& key = LLSD());
    /*virtual*/ void closeHostedFloater();

    void loadHistory();
    void reloadMessages(bool clean_messages = false);
    void removeScreenChat();

    void show();
    bool isChatVisible() const;

    /** @param archive true - to save a message to the chat history log */
    void    addMessage          (const LLChat& message,bool archive = true, const LLSD &args = LLSD());

    LLChatEntry* getChatBox() { return mInputEditor; }

    std::string getCurrentChat();
    S32 getMessageArchiveLength() { return static_cast<S32>(mMessageArchive.size()); }

    virtual bool handleKeyHere( KEY key, MASK mask );

    static void startChat(const char* line);
    static void stopChat();

    static void sendChatFromViewer(const std::string &utf8text, EChatType type, bool animate);
    static void sendChatFromViewer(const LLWString &wtext, EChatType type, bool animate);

    static bool isWordsName(const std::string& name);

    // <plus> chat-bar (plus/chatbar, research/334): what typing in and sending from a nearby chat field does, for
    // any such field: this floater's (mInputEditor) and the chat bar beside the bottom toolbar share them.
    // A keystroke: the typing animation, the gesture completion. Sending: the text (with "/number", a gesture,
    // "/whisper" "/shout") is said, the field emptied, its focus let go when CloseChatOnReturn is on.
    // selected_channel: the channel of a text without "/number" and where no typing animation is shown when not 0:
    // this floater's selector (getSelectedChannel(), chat-channel-select); 0 for the chat bar, which always says on
    // 0 as in Firestorm, whatever the floater's selectors.
    static void onChatEntryKeystroke(LLChatEntry* editor, S32 selected_channel);
    static void sendChatFromEntry(LLChatEntry* editor, EChatType type, S32 selected_channel);
    // </plus>

    void showHistory();

protected:
    static bool matchChatTypeTrigger(const std::string& in_str, std::string* out_str);
    void onChatBoxKeystroke();
    void onChatBoxFocusLost();
    void onChatBoxFocusReceived();

    void sendChat( EChatType type );
    void onChatBoxCommit();
    void onChatFontChange(LLFontGL* fontp);

    /*virtual*/ void onTearOffClicked();
    /*virtual*/ void onClickCloseBtn(bool app_qutting = false);

    static LLWString stripChannelNumber(const LLWString &mesg, S32* channel, bool* is_set = nullptr); // <plus> is_set: chat-channel-select </plus>
    // <plus> chat-channel-select / chat-type button (from Firestorm)
    // sendChatFromViewer() with the channel decided by the caller (the window's own send path uses the selector; other callers do not)
    static void sendChatToChannel(const LLWString &wtext, const LLWString &out_text, S32 channel, EChatType type, bool animate);
    EChatType getSelectedChatType() const;
    S32 getSelectedChannel() const;
    void updateInputExtras();
    void onChatTypeChanged();
    // </plus>
    static EChatType processChatTypeTriggers(EChatType type, std::string &str); // <plus> chat-bar: static (it reads no member; sendChatFromEntry() is static) </plus>

    void displaySpeakingIndicator();

    // Which non-zero channel did we last chat on?
    static S32 sLastSpecialChatChannel;

    LLOutputMonitorCtrl*    mOutputMonitor;
    LLLocalSpeakerMgr*      mSpeakerMgr;

    S32 mExpandedHeight;

    // <plus> chat-channel-select / chat-type button
    LLPanel*    mChatChannelPanel = nullptr;
    LLSpinCtrl* mChatChannelSpinner = nullptr;
    LLPanel*    mChatTypePanel = nullptr;
    LLComboBox* mChatTypeCombo = nullptr;
    LLButton*   mSendChatButton = nullptr;
    boost::signals2::scoped_connection mShowChannelConn;
    boost::signals2::scoped_connection mShowTypeConn;
    boost::signals2::scoped_connection mTimeLocalConn;    // chat-time-local: stamp the kept lines again
    // </plus>

private:
    /*virtual*/ void refresh();
    void restampArchive();

    std::vector<LLChat> mMessageArchive;
};

#endif // LL_LLFLOATERIMNEARBYCHAT_H
