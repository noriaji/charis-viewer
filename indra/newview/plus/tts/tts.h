/**
 * @file tts.h
 * @brief Chat read-aloud (plus/tts): speaks the chat that arrives, with a voice for each speaker.
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

#ifndef PLUS_TTS_H
#define PLUS_TTS_H

#include <string>
#include <vector>

#include "lluuid.h"

class LLChat;

namespace plus::tts
{
    // Official hooks (marked <plus>):
    //   LLFloaterIMNearbyChat::addMessage()  llfloaterimnearbychat.cpp   onNearbyChat(chat)
    //   LLIMMgr::addMessage()                llimview.cpp                onInstantMessage(...)
    //   translateSuccess / translateFailure  llviewermessage.cpp         TranslatedLineScope
    //   menu_avatar_other.xml (en, ja)       the "Read-aloud voice" submenu (Plus.Tts.SetVoice / Plus.Tts.VoiceIs)

    // A line has been added to the nearby chat window: chat of avatars and objects, and the system's
    // lines (money, teleport, region, the radar's comings and goings). Text is as shown, with the
    // translation if the chat translation added one. Does nothing when the read-aloud is off.
    void onNearbyChat(const LLChat& chat);

    // While one is alive (around the onChat() of translateSuccess / translateFailure), the nearby line
    // `line` is known to have been through the chat translation: original_size is its length before
    // " (...)" was added, failed that the brackets hold the viewer's note of a failed translation.
    // onNearbyChat() then reads the translation alone (PlusTtsTranslated) or the original from this, not
    // by guessing from the brackets at the end of a line. Nests (the one before is put back).
    class TranslatedLineScope
    {
    public:
        TranslatedLineScope(const std::string& line, size_t original_size, bool failed);
        ~TranslatedLineScope();
        TranslatedLineScope(const TranslatedLineScope&) = delete;
        TranslatedLineScope& operator=(const TranslatedLineScope&) = delete;

        const std::string&         mLine;
        const size_t               mOriginalSize;
        const bool                 mFailed;
        const TranslatedLineScope* mPrevious;
    };

    // A line of IM or group chat has arrived and is kept (just before it is stored); from_id is the speaker.
    // offline: a message that was saved for you (read at login: not read). system_notice: a notice the
    // simulator or the viewer put into the conversation (from_id is still the other person's): read as a notice.
    void onInstantMessage(const LLUUID& session_id, const LLUUID& from_id, const std::string& from_name,
                          const std::string& text, bool offline, bool system_notice);

    // The hand-set voice of an avatar: 0 = by the avatar's shape (auto), 1 = man, 2 = woman. Saved
    // with the account's settings.
    int  avatarVoiceMode(const LLUUID& avatar_id);
    void setAvatarVoiceMode(const LLUUID& avatar_id, int mode);
    void clearAvatarVoiceModes();

    // For the window's try-out button: says a sample in a man's and a woman's voice, whether or not
    // the read-aloud is on.
    void speakSample();
    // Stops what is spoken and drops what waits.
    void stopSpeaking();

    // For the window: whether the speech engine is still starting, could start, or failed to (then no
    // voice will come; the window stops asking). Starts the engine when it has not.
    enum class EngineStatus
    {
        Starting,
        Ready,
        Failed
    };
    EngineStatus engineStatus();

    // The system's voices (names), for the window's choices. Empty until the engine has started;
    // starts it when it has not.
    struct VoiceName
    {
        std::string name;
        std::string language;
        bool        male = false;
    };
    std::vector<VoiceName> availableVoices();
}

#endif // PLUS_TTS_H
