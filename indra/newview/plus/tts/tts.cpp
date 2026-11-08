/**
 * @file tts.cpp
 * @brief Chat read-aloud: what is read, how the text is cleaned, which voice a speaker gets (plus/tts).
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

// Design: research/ (the chat read-aloud note). In short:
//  - Two official hooks hand every line over (tts.h). Here the line is sorted into a kind (nearby avatar,
//    yourself, object, system, comings and goings, IM, group) and read only when its kind is switched on.
//  - The text is cleaned (links, URLs, emoji, long runs, length), the speaker gets a voice, and the
//    engine (ttsengine.h) queues it. All waiting is on the engine's own thread.
//  - A voice comes from the shape's sex value of the avatar (LLVOAvatar::getSex(), the "male" slider),
//    kept for a conversation; when the shape is not known yet a neutral voice is used for the rest of
//    that conversation. A hand-set man / woman / auto per avatar wins (saved in a setting). The name is
//    never used to guess a sex; it only decides the pitch, so that two people of one sex differ.

#include "llviewerprecompiledheaders.h"

#include "tts.h"
#include "ttsengine.h"
#include "plus.h"
#include "plus/rlv/rlvhooks.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <sstream>

#include "llagent.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llchat.h"
#include "lleventtimer.h"
#include "llevents.h"
#include "llfloaterreg.h"
#include "llimview.h"
#include "llmutelist.h"
#include "llselectmgr.h"
#include "lltimer.h"
#include "lltrans.h"
#include "llui.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"
#include "llvoavatarself.h"

namespace plus::tts
{
    void logLine(const std::string& line)
    {
        LL_INFOS("PlusTts") << line << LL_ENDL;
    }

    namespace
    {
        // Settings (settings_plus.xml, per account)
        const char* const SET_ON          = "PlusTtsOn";
        const char* const SET_NEARBY      = "PlusTtsNearby";
        const char* const SET_SELF        = "PlusTtsSelf";
        const char* const SET_OBJECTS     = "PlusTtsObjects";
        const char* const SET_SYSTEM      = "PlusTtsSystem";
        const char* const SET_ARRIVALS    = "PlusTtsArrivals";
        const char* const SET_IM          = "PlusTtsIM";
        const char* const SET_GROUP       = "PlusTtsGroup";
        const char* const SET_VOICE_M     = "PlusTtsVoiceMale";
        const char* const SET_VOICE_F     = "PlusTtsVoiceFemale";
        const char* const SET_RATE        = "PlusTtsRate";
        const char* const SET_VOLUME      = "PlusTtsVolume";
        const char* const SET_DISTANCE    = "PlusTtsDistance";
        const char* const SET_MAX_LENGTH  = "PlusTtsMaxLength";
        const char* const SET_MAX_QUEUE   = "PlusTtsMaxQueue";
        const char* const SET_ONLY_ACTIVE = "PlusTtsOnlyActive";
        const char* const SET_TRANSLATED  = "PlusTtsTranslated";
        const char* const SET_SAY_NAME    = "PlusTtsSayName";
        const char* const SET_OVERRIDES   = "PlusTtsOverrides";

        // The same speaker within this many seconds is still in the same conversation: the voice stays.
        const F64 CONVERSATION_GAP = 300.0;
        // At most this many speakers are remembered.
        const size_t MAX_SPEAKERS = 512;
        // The window is checked for being in the background this often.
        const F32 ACTIVE_CHECK_INTERVAL = 0.5f;

        enum class Kind
        {
            Nearby,     // an avatar near you
            Self,       // yourself
            Object,     // an object's say
            System,     // money, teleport, region and the other notices
            Arrivals,   // the radar's comings and goings
            IM,         // a one-to-one or ad-hoc conversation
            Group       // group chat
        };

        const char* settingFor(Kind kind)
        {
            switch (kind)
            {
                case Kind::Nearby:   return SET_NEARBY;
                case Kind::Self:     return SET_SELF;
                case Kind::Object:   return SET_OBJECTS;
                case Kind::System:   return SET_SYSTEM;
                case Kind::Arrivals: return SET_ARRIVALS;
                case Kind::IM:       return SET_IM;
                case Kind::Group:    return SET_GROUP;
            }
            return SET_NEARBY;
        }

        //---------------------------------------------------------------------------------------
        // The engine
        //---------------------------------------------------------------------------------------

        std::unique_ptr<Engine> sEngine;
        bool                    sQuitting = false;

        // Read-aloud off / sound muted stops at once; the volume follows its settings (init()).
        boost::signals2::scoped_connection sOnConnection;
        boost::signals2::scoped_connection sMuteConnection;
        boost::signals2::scoped_connection sVolumeConnection;
        boost::signals2::scoped_connection sMasterConnection;

        // The viewer's mute and master volume times the read-aloud volume (the engine plays straight to
        // the default device, past the viewer's own mixer).
        float currentVolume()
        {
            if (gSavedSettings.getBOOL("MuteAudio"))
            {
                return 0.0f;
            }
            return std::clamp(gSavedPerAccountSettings.getF32(SET_VOLUME), 0.0f, 1.0f)
                 * std::clamp(gSavedSettings.getF32("AudioLevelMaster"), 0.0f, 1.0f);
        }

        class ActiveTimer final : public LLEventTimer
        {
        public:
            ActiveTimer() : LLEventTimer(ACTIVE_CHECK_INTERVAL) {}

            // True: let go of this timer (it is deleted).
            bool tick() override
            {
                if (!sEngine)
                {
                    return true;
                }
                // Busy (do not disturb) set while lines wait or one is spoken: all of it stops, once.
                const bool dnd = gAgent.isDoNotDisturb();
                if (dnd && (!mWasDnd || sEngine->pending() > 0))
                {
                    sEngine->clear();
                }
                mWasDnd = dnd;

                static LLCachedControl<bool> only_active(gSavedPerAccountSettings, SET_ONLY_ACTIVE, true);
                if (only_active() && gViewerWindow && !gViewerWindow->getActive() && sEngine->pending() > 0)
                {
                    sEngine->clear();
                }
                return false;
            }

        private:
            bool mWasDnd = false;
        };

        // A mute made while lines of that avatar or object wait (or one is spoken) drops them; say() only
        // keeps the new lines out.
        class MuteObserver final : public LLMuteListObserver
        {
        public:
            void onChange() override {}

            void onChangeDetailed(const LLMute& mute) override
            {
                if (sEngine && mute.mID.notNull() && LLMuteList::getInstance()->isMuted(mute.mID, LLMute::flagTextChat))
                {
                    sEngine->dropSpeaker(mute.mID.asString());
                }
            }
        };
        MuteObserver sMuteObserver;
        bool         sMuteObserved = false;

        void stopObservingMutes()
        {
            if (sMuteObserved)
            {
                sMuteObserved = false;
                LLMuteList::getInstance()->removeObserver(&sMuteObserver);
            }
        }

        std::unique_ptr<Engine> createEngine()
        {
            // The place for a second engine (VOICEVOX): choose here by a setting.
            return createWinRTEngine();
        }

        Engine* engine()
        {
            if (sQuitting)
            {
                return nullptr;
            }
            if (!sEngine)
            {
                sEngine = createEngine();
                sEngine->setVolume(currentVolume());
                logLine(std::string("engine started: ") + sEngine->name());
                new ActiveTimer();
                if (!sMuteObserved)
                {
                    LLMuteList::getInstance()->addObserver(&sMuteObserver);
                    sMuteObserved = true;
                }
            }
            return sEngine.get();
        }

        // Also from the static guard at exit: no singleton is touched here (the mute list may be gone
        // then; its observer is let go on the "quitting" event, stopObservingMutes()).
        void shutdownEngine()
        {
            sQuitting = true;
            sOnConnection.disconnect();
            sMuteConnection.disconnect();
            sVolumeConnection.disconnect();
            sMasterConnection.disconnect();
            if (sEngine)
            {
                sEngine->shutdown();
                sEngine.reset();
            }
        }

        // Stops the thread even when the viewer leaves without the "quitting" event.
        struct EngineGuard
        {
            ~EngineGuard() { shutdownEngine(); }
        } sEngineGuard;

        //---------------------------------------------------------------------------------------
        // Text
        //---------------------------------------------------------------------------------------

        bool isEmojiOrMark(llwchar c)
        {
            return (c >= 0x1F000 && c <= 0x1FAFF)       // emoji, symbols, flags, pictographs
                || (c >= 0x2600 && c <= 0x27BF)         // misc symbols, dingbats
                || (c >= 0x2B00 && c <= 0x2BFF)         // arrows, stars
                || (c >= 0xFE00 && c <= 0xFE0F)         // variation selectors
                || (c >= 0xE0000 && c <= 0xE007F)       // tags
                || c == 0x200D || c == 0x20E3           // joiner, keycap
                || c == 0x2122 || c == 0x00A9 || c == 0x00AE
                || (c >= 0x2190 && c <= 0x21FF)
                || (c >= 0x2300 && c <= 0x23FF)
                || (c >= 0x25A0 && c <= 0x25FF);
        }

        // Something a voice could say: a letter or a digit of any script (kana and ideographs too).
        bool isSpeakable(llwchar c)
        {
            if (c < 0x80)
            {
                return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
            }
            if (isEmojiOrMark(c))
            {
                return false;
            }
            return (c >= 0xC0 && c <= 0x2FF && c != 0xD7 && c != 0xF7)    // Latin
                || (c >= 0x370 && c <= 0x52F)                              // Greek, Cyrillic
                || (c >= 0x3041 && c <= 0x30FF)                            // kana
                || (c >= 0x3400 && c <= 0x9FFF)                            // ideographs
                || (c >= 0xAC00 && c <= 0xD7AF)                            // Hangul
                || (c >= 0xFF10 && c <= 0xFF19) || (c >= 0xFF21 && c <= 0xFF3A) || (c >= 0xFF41 && c <= 0xFF5A)
                || (c >= 0xFF66 && c <= 0xFF9F);
        }

        std::string agentDisplayName(const LLUUID& id, const std::string& fallback)
        {
            LLAvatarName name;
            if (id.notNull() && LLAvatarNameCache::get(id, &name) && !name.getDisplayName().empty())
            {
                return name.getDisplayName();
            }
            return fallback;
        }

        // agentDisplayName() as RLVa's @shownames lets it be said: the anonym of a hidden avatar, as the viewer
        // shows the links to it (plus/rlv, research/248 3.4). For the links in a text and the radar's reports.
        std::string shownAgentName(const LLUUID& id, const std::string& fallback)
        {
            const std::string name = agentDisplayName(id, fallback);
            return plus::rlv::enabled() ? plus::rlv::avatarNameForPlus(id, name) : name;
        }

        // The name said for an avatar whose name is not known (strings_plus.xml).
        std::string unknownUser()
        {
            return LLTrans::getString("PlusTtsUnknownUser");
        }

        // The spaces of std::regex's \s.
        bool isSpaceByte(char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
        }

        bool isAsciiLetter(char c)
        {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        }

        bool isAsciiAlnum(char c)
        {
            return isAsciiLetter(c) || (c >= '0' && c <= '9');
        }

        // True when text holds prefix (ASCII, lower case) at pos, the case of ASCII letters aside.
        bool hasAt(const std::string& text, size_t pos, const char* prefix)
        {
            for (size_t i = 0; prefix[i]; ++i)
            {
                if (pos + i >= text.size())
                {
                    return false;
                }
                char c = text[pos + i];
                if (c >= 'A' && c <= 'Z')
                {
                    c = static_cast<char>(c - 'A' + 'a');
                }
                if (c != prefix[i])
                {
                    return false;
                }
            }
            return true;
        }

        // The length of the scheme at pos, 0 when none of them is there.
        size_t schemeAt(const std::string& text, size_t pos, std::initializer_list<const char*> schemes)
        {
            for (const char* scheme : schemes)
            {
                if (hasAt(text, pos, scheme))
                {
                    return strlen(scheme);
                }
            }
            return 0;
        }

        size_t webSchemeAt(const std::string& text, size_t pos)
        {
            return schemeAt(text, pos, { "http://", "https://", "ftp://" });
        }

        // The end of an address from pos: the first space or non-ASCII byte. Japanese chat writes the
        // next words right after an address ("https://a.com見て", or after a full-width space); an address
        // with unescaped Japanese in it is read as "URL" and then its Japanese part.
        size_t wordEnd(const std::string& text, size_t pos)
        {
            while (pos < text.size() && !isSpaceByte(text[pos]) && static_cast<unsigned char>(text[pos]) < 0x80)
            {
                ++pos;
            }
            return pos;
        }

        // "[http://example.com label]" -> "label"; the links of the viewer to an avatar's name, the other
        // links of the viewer to "link"; the addresses to "URL". Scanned by hand, once over the text: a
        // std::regex over a long address without spaces recursed deep enough to overflow the stack (MSVC).
        std::string replaceLinks(const std::string& text)
        {
            std::string labels;
            labels.reserve(text.size());
            for (size_t i = 0; i < text.size();)
            {
                if (text[i] == '[')
                {
                    if (const size_t scheme = webSchemeAt(text, i + 1))
                    {
                        size_t p = i + 1 + scheme;
                        const size_t address = p;
                        while (p < text.size() && !isSpaceByte(text[p]) && text[p] != ']')
                        {
                            ++p;
                        }
                        if (p > address && p < text.size() && isSpaceByte(text[p]))
                        {
                            while (p < text.size() && isSpaceByte(text[p]))
                            {
                                ++p;
                            }
                            const size_t close = text.find(']', p);
                            if (close != std::string::npos)
                            {
                                labels.append(text, p, close - p);
                                i = close + 1;
                                continue;
                            }
                        }
                    }
                }
                labels.push_back(text[i]);
                ++i;
            }

            static const char AGENT[] = "secondlife:///app/agent/";
            const size_t agent_length = sizeof(AGENT) - 1;
            std::string out;
            out.reserve(labels.size());
            for (size_t i = 0; i < labels.size();)
            {
                // secondlife:///app/agent/<id>/about and the like: the avatar's name.
                if (hasAt(labels, i, AGENT) && labels.size() > i + agent_length + 36 && labels[i + agent_length + 36] == '/')
                {
                    LLUUID id;
                    size_t p = i + agent_length + 37;
                    const size_t verb = p;
                    while (p < labels.size() && isAsciiLetter(labels[p]))
                    {
                        ++p;
                    }
                    if (p > verb && LLUUID::parseUUID(labels.substr(i + agent_length, 36), &id))
                    {
                        out += shownAgentName(id, unknownUser());
                        i = p;
                        continue;
                    }
                }
                if (const size_t scheme = schemeAt(labels, i, { "secondlife://", "x-grid-location-info://", "hop://" }))
                {
                    out += LLTrans::getString("PlusTtsLink");
                    i = wordEnd(labels, i + scheme);
                    continue;
                }
                const size_t web = webSchemeAt(labels, i);
                if (web && i + web < labels.size() && !isSpaceByte(labels[i + web]))
                {
                    out += "URL";
                    i = wordEnd(labels, i + web);
                    continue;
                }
                // "www.example.com", but not the end of a laugh ("wwww.") nor "www." alone.
                if (hasAt(labels, i, "www.") && (i == 0 || !isAsciiAlnum(labels[i - 1]))
                    && i + 4 < labels.size() && isAsciiAlnum(labels[i + 4]))
                {
                    out += "URL";
                    i = wordEnd(labels, i + 4);
                    continue;
                }
                out.push_back(labels[i]);
                ++i;
            }
            return out;
        }

        // The text of a line as it is to be said, "" when nothing is left to say.
        std::string cleanText(const std::string& raw, S32 max_length)
        {
            const std::string text = replaceLinks(raw);

            LLWString wide = utf8str_to_wstring(text);
            LLWString kept;
            kept.reserve(wide.size());
            S32 run = 0;
            llwchar previous = 0;
            for (llwchar c : wide)
            {
                if (isEmojiOrMark(c))
                {
                    continue;
                }
                if (c == '\n' || c == '\r' || c == '\t' || c == 0x3000)
                {
                    c = ' ';
                }
                if (c < ' ')
                {
                    continue;
                }
                if (c == ' ' && previous == ' ')
                {
                    continue;
                }
                // "wwwwwwww", "!!!!!!!", "ーーーー": three are enough to hear.
                run = (c == previous) ? run + 1 : 1;
                previous = c;
                if (run > 3)
                {
                    continue;
                }
                kept.push_back(c);
            }

            if (std::none_of(kept.begin(), kept.end(), isSpeakable))
            {
                return std::string();
            }

            while (!kept.empty() && kept.front() == ' ')
            {
                kept.erase(kept.begin());
            }
            while (!kept.empty() && kept.back() == ' ')
            {
                kept.pop_back();
            }

            const size_t limit = static_cast<size_t>(std::max<S32>(max_length, 10));
            std::string cut;
            if (kept.size() > limit)
            {
                kept.resize(limit);
                cut = LLTrans::getString("PlusTtsRestLeftOut");
            }
            return wstring_to_utf8str(kept) + cut;
        }

        //---------------------------------------------------------------------------------------
        // Voices
        //---------------------------------------------------------------------------------------

        // Hand-set voices, by avatar: 1 man, 2 woman (auto is not kept).
        std::map<LLUUID, int> sOverrides;
        std::string           sOverridesText;       // the setting's text the map is made from
        bool                  sOverridesRead = false;

        void readOverrides()
        {
            const std::string text = gSavedPerAccountSettings.getString(SET_OVERRIDES);
            if (sOverridesRead && text == sOverridesText)
            {
                return;
            }
            sOverridesRead = true;
            sOverridesText = text;
            sOverrides.clear();
            std::istringstream stream(text);
            std::string item;
            while (std::getline(stream, item, ';'))
            {
                const size_t colon = item.find(':');
                LLUUID id;
                if (colon == std::string::npos || !LLUUID::parseUUID(item.substr(0, colon), &id) || id.isNull())
                {
                    continue;
                }
                const std::string mode = item.substr(colon + 1);
                if (mode == "m")
                {
                    sOverrides[id] = 1;
                }
                else if (mode == "f")
                {
                    sOverrides[id] = 2;
                }
            }
        }

        void writeOverrides()
        {
            std::string text;
            for (const auto& entry : sOverrides)
            {
                text += entry.first.asString() + (entry.second == 1 ? ":m;" : ":f;");
            }
            sOverridesText = text;
            gSavedPerAccountSettings.setString(SET_OVERRIDES, text);
        }

        struct SpeakerState
        {
            bool        decided = false;
            VoiceGender gender  = VoiceGender::Neutral;
            F64         lastSpoke = 0.0;
        };
        std::map<LLUUID, SpeakerState> sSpeakers;

        // The sex of an avatar's shape, or Neutral when it is not known (not near, not loaded yet).
        VoiceGender judgeBySex(const LLUUID& id)
        {
            LLVOAvatar* avatar = nullptr;
            if (id == gAgentID)
            {
                avatar = gAgentAvatarp.get();
            }
            else if (LLViewerObject* object = gObjectList.findObject(id))
            {
                if (object->isAvatar())
                {
                    avatar = static_cast<LLVOAvatar*>(object);
                }
            }
            if (!avatar || avatar->isDead() || !(avatar->isSelf() || avatar->isFullyLoaded()))
            {
                return VoiceGender::Neutral;
            }
            return avatar->getSex() == SEX_MALE ? VoiceGender::Male : VoiceGender::Female;
        }

        VoiceGender genderOf(const LLUUID& id, F64 now)
        {
            readOverrides();
            const auto hand = sOverrides.find(id);
            if (hand != sOverrides.end())
            {
                return hand->second == 1 ? VoiceGender::Male : VoiceGender::Female;
            }

            // Speakers are kept for the conversation rule only; do not let the table grow for ever.
            if (sSpeakers.size() >= MAX_SPEAKERS && sSpeakers.find(id) == sSpeakers.end())
            {
                for (auto it = sSpeakers.begin(); it != sSpeakers.end();)
                {
                    it = (now - it->second.lastSpoke >= CONVERSATION_GAP) ? sSpeakers.erase(it) : std::next(it);
                }
                if (sSpeakers.size() >= MAX_SPEAKERS)
                {
                    sSpeakers.clear();
                }
            }
            SpeakerState& state = sSpeakers[id];
            if (state.decided && now - state.lastSpoke < CONVERSATION_GAP)
            {
                // Same conversation: the voice stays, also a neutral one for a shape not seen yet.
                state.lastSpoke = now;
                return state.gender;
            }
            state.decided   = true;
            state.gender    = judgeBySex(id);
            state.lastSpoke = now;
            return state.gender;
        }

        // 0..99, from the name, the same every time.
        unsigned nameBucket(const std::string& name)
        {
            return static_cast<unsigned>(std::hash<std::string>()(name) % 100u);
        }

        float pitchFor(VoiceGender gender, const std::string& name)
        {
            const float jitter = (static_cast<float>(nameBucket(name)) - 50.0f) / 50.0f;    // -1 .. 1
            switch (gender)
            {
                case VoiceGender::Male:   return 0.95f + 0.07f * jitter;
                case VoiceGender::Female: return 1.05f + 0.07f * jitter;
                case VoiceGender::Neutral:
                default:                  return 0.92f + 0.10f * jitter;    // between the two
            }
        }

        //---------------------------------------------------------------------------------------
        // Saying a line
        //---------------------------------------------------------------------------------------

        bool windowInBackground()
        {
            return gViewerWindow && !gViewerWindow->getActive();
        }

        Utterance makeUtterance(const std::string& text, VoiceGender gender, const std::string& name)
        {
            Utterance utterance;
            utterance.text            = text;
            utterance.gender          = gender;
            utterance.pitch           = pitchFor(gender, name);
            utterance.rate            = std::clamp(gSavedPerAccountSettings.getF32(SET_RATE), 0.5f, 3.0f);
            utterance.preferredMale   = gSavedPerAccountSettings.getString(SET_VOICE_M);
            utterance.preferredFemale = gSavedPerAccountSettings.getString(SET_VOICE_F);
            return utterance;
        }

        // prefix goes before the cleaned text as it is (an emote's name); say_name puts the speaker's
        // name before it in the way of the viewer's language ("Name, text" / "名前、本文").
        void say(Kind kind, const LLUUID& speaker_id, const std::string& speaker_name, const std::string& raw_text,
                 const std::string& prefix = std::string(), bool say_name = false)
        {
            if (!gSavedPerAccountSettings.getBOOL(SET_ON) || !gSavedPerAccountSettings.getBOOL(settingFor(kind)))
            {
                return;
            }
            if (gAgent.isDoNotDisturb())
            {
                return;
            }
            if (gSavedPerAccountSettings.getBOOL(SET_ONLY_ACTIVE) && windowInBackground())
            {
                return;
            }

            if (gSavedSettings.getBOOL("MuteAudio"))
            {
                return;
            }
            std::string text = cleanText(raw_text, std::clamp<S32>(gSavedPerAccountSettings.getS32(SET_MAX_LENGTH), 10, 500));
            if (text.empty())
            {
                return;
            }
            if (!prefix.empty())
            {
                text = prefix + text;
            }
            if (say_name)
            {
                LLStringUtil::format_map_t args;
                args["[NAME]"] = speaker_name;
                args["[TEXT]"] = text;
                text = LLTrans::getString("PlusTtsNameAndText", args);
            }

            // Notices have a voice of their own; the others by the speaker's id (an avatar's shape; neutral for an object).
            VoiceGender gender;
            std::string name_for_pitch = speaker_name;
            if (kind == Kind::System || kind == Kind::Arrivals)
            {
                gender         = VoiceGender::Female;
                name_for_pitch = "system";
            }
            else
            {
                gender = genderOf(speaker_id, LLTimer::getTotalSeconds());
            }

            if (Engine* e = engine())
            {
                const S32 queue = std::clamp<S32>(gSavedPerAccountSettings.getS32(SET_MAX_QUEUE), 1, 50);
                Utterance utterance = makeUtterance(text, gender, name_for_pitch);
                if (speaker_id.notNull())
                {
                    utterance.speaker = speaker_id.asString();      // a mute drops its lines (MuteObserver)
                }
                e->speak(std::move(utterance), static_cast<std::size_t>(queue));
            }
        }

        // The line translateSuccess / translateFailure is handing on now (TranslatedLineScope), or nullptr.
        const TranslatedLineScope* sTranslatedLine = nullptr;

        // The text of a nearby line to read. A line known to have been through the chat translation is
        // read as the translation alone when only that is wanted (PlusTtsTranslated), else as shown; the
        // viewer's note of a failed translation is never read. Any other line is read as it is, also one
        // that ends in "(...)" of its own.
        std::string nearbyText(const LLChat& chat)
        {
            const TranslatedLineScope* line = sTranslatedLine;
            if (!line || line->mLine != chat.mText || line->mOriginalSize > chat.mText.size())
            {
                return chat.mText;
            }
            const size_t added = chat.mText.size() - line->mOriginalSize;
            if (line->mFailed)
            {
                return chat.mText.substr(0, line->mOriginalSize);
            }
            if (added <= 3 || !gSavedPerAccountSettings.getBOOL(SET_TRANSLATED))
            {
                return chat.mText;      // no translation was added (same language), or both are read
            }
            return chat.mText.substr(line->mOriginalSize + 2, added - 3);     // " (" translation ")"
        }

        // "/me waves" is said as "<name> waves".
        bool isEmote(const std::string& text)
        {
            return text.size() > 4 && text.compare(0, 3, "/me") == 0 && (text[3] == ' ' || text[3] == '\'');
        }

        std::string emoteBody(const std::string& text)
        {
            return text[3] == ' ' ? text.substr(4) : text.substr(3);
        }

        // Within the distance limit (0 = none). A line whose place is not known is let through.
        bool withinDistance(const LLChat& chat)
        {
            const F32 limit = gSavedPerAccountSettings.getF32(SET_DISTANCE);
            if (limit <= 0.f || chat.mPosAgent.isExactlyZero())
            {
                return true;
            }
            return dist_vec(gAgent.getPositionAgent(), chat.mPosAgent) <= limit;
        }

        // The radar writes its reports as a link to the avatar followed by the text.
        bool isArrivalReport(const std::string& text, LLUUID& avatar_id, std::string& rest)
        {
            static const std::string head = "secondlife:///app/agent/";
            if (text.compare(0, head.size(), head) != 0 || text.size() < head.size() + 37)
            {
                return false;
            }
            if (!LLUUID::parseUUID(text.substr(head.size(), 36), &avatar_id))
            {
                return false;
            }
            const size_t space = text.find(' ', head.size() + 36);
            if (space == std::string::npos)
            {
                return false;
            }
            rest = text.substr(space + 1);
            return true;
        }

        //---------------------------------------------------------------------------------------
        // The menu of the avatar
        //---------------------------------------------------------------------------------------

        LLUUID selectedAvatarId()
        {
            LLViewerObject* object = LLSelectMgr::getInstance()->getSelection()->getPrimaryObject();
            if (!object)
            {
                return LLUUID::null;
            }
            if (object->isAvatar())
            {
                return object->getID();
            }
            if (LLVOAvatar* avatar = object->getAvatar())
            {
                return avatar->getID();
            }
            return LLUUID::null;
        }

        int modeFromName(const std::string& name)
        {
            return name == "male" ? 1 : (name == "female" ? 2 : 0);
        }

        void init()
        {
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.Tts.SetVoice", [](LLUICtrl*, const LLSD& param)
            {
                const LLUUID id = selectedAvatarId();
                if (id.notNull())
                {
                    setAvatarVoiceMode(id, modeFromName(param.asString()));
                }
            });
            LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.Tts.VoiceIs", [](LLUICtrl*, const LLSD& param)
            {
                const LLUUID id = selectedAvatarId();
                return id.notNull() && avatarVoiceMode(id) == modeFromName(param.asString());
            });

            // Turning read-aloud off or muting the sound also stops the lines that wait and the one spoken
            // (say() only keeps new lines out); a volume change reaches the lines that wait.
            if (LLControlVariable* control = gSavedPerAccountSettings.getControl(SET_ON))
            {
                sOnConnection = control->getSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&)
                {
                    if (!value.asBoolean())
                    {
                        stopSpeaking();
                    }
                });
            }
            if (LLControlVariable* control = gSavedSettings.getControl("MuteAudio"))
            {
                sMuteConnection = control->getSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&)
                {
                    if (value.asBoolean())
                    {
                        stopSpeaking();
                    }
                    if (sEngine)
                    {
                        sEngine->setVolume(currentVolume());
                    }
                });
            }
            const auto follow_volume = [](LLControlVariable*, const LLSD&, const LLSD&)
            {
                if (sEngine)
                {
                    sEngine->setVolume(currentVolume());
                }
            };
            if (LLControlVariable* control = gSavedPerAccountSettings.getControl(SET_VOLUME))
            {
                sVolumeConnection = control->getSignal()->connect(follow_volume);
            }
            if (LLControlVariable* control = gSavedSettings.getControl("AudioLevelMaster"))
            {
                sMasterConnection = control->getSignal()->connect(follow_volume);
            }

            // Stop the speaking thread before the viewer shuts down.
            LLEventPumps::instance().obtain("LLApp").listen("PlusTts", [](const LLSD& event)
            {
                if (event["status"].asString() == "quitting")
                {
                    stopObservingMutes();
                    shutdownEngine();
                }
                return false;
            });
        }

        plus::FeatureRegistration sRegistration({ "tts", &init, {} });
    }

    void onNearbyChat(const LLChat& chat)
    {
        if (!gSavedPerAccountSettings.getBOOL(SET_ON))
        {
            return;
        }
        if (chat.mMuted || chat.mText.empty() || chat.mAudible == CHAT_AUDIBLE_NOT
            || chat.mChatStyle == CHAT_STYLE_HISTORY || chat.mChatStyle == CHAT_STYLE_TELEPORT_SEP
            || chat.mChatType == CHAT_TYPE_DEBUG_MSG || chat.mChatType == CHAT_TYPE_START || chat.mChatType == CHAT_TYPE_STOP)
        {
            return;
        }

        switch (chat.mSourceType)
        {
            case CHAT_SOURCE_AGENT:
            {
                if (LLMuteList::getInstance()->isMuted(chat.mFromID, LLMute::flagTextChat))
                {
                    return;
                }
                const bool mine = chat.mFromID == gAgentID;
                if (!mine && !withinDistance(chat))
                {
                    return;
                }
                // RLVa @shownames: a line whose name was hidden is read with the anonym it came with (research/248 A1)
                const std::string name = chat.mRlvNamesFiltered ? chat.mFromName : agentDisplayName(chat.mFromID, chat.mFromName);
                const std::string text = nearbyText(chat);
                say(mine ? Kind::Self : Kind::Nearby, chat.mFromID, name,
                    isEmote(text) ? emoteBody(text) : text,
                    isEmote(chat.mText) ? name + " " : std::string());
                break;
            }
            case CHAT_SOURCE_OBJECT:
            {
                if (LLMuteList::getInstance()->isMuted(chat.mFromID, chat.mFromName, LLMute::flagTextChat)
                    || !withinDistance(chat))
                {
                    return;
                }
                const std::string text = nearbyText(chat);
                say(Kind::Object, chat.mFromID, chat.mFromName,
                    isEmote(text) ? emoteBody(text) : text,
                    isEmote(chat.mText) ? chat.mFromName + " " : std::string());
                break;
            }
            case CHAT_SOURCE_SYSTEM:
            case CHAT_SOURCE_TELEPORT:
            case CHAT_SOURCE_REGION:
            {
                LLUUID avatar_id;
                std::string rest;
                if (isArrivalReport(chat.mText, avatar_id, rest))
                {
                    say(Kind::Arrivals, LLUUID::null, "system", shownAgentName(avatar_id, unknownUser()) + " " + rest);
                }
                else
                {
                    say(Kind::System, LLUUID::null, "system", chat.mText);
                }
                break;
            }
            default:
                break;
        }
    }

    void onInstantMessage(const LLUUID& session_id, const LLUUID& from_id, const std::string& from_name,
                          const std::string& text, bool offline, bool system_notice)
    {
        if (offline)
        {
            return;     // saved messages arrive all at once at login, with their date in front
        }
        if (system_notice)
        {
            say(Kind::System, LLUUID::null, "system", text);
            return;
        }
        // No speaker (the system's own lines in a session) is not read.
        if (!gSavedPerAccountSettings.getBOOL(SET_ON) || text.empty() || from_id.isNull())
        {
            return;
        }
        const LLIMModel::LLIMSession* session = LLIMModel::getInstance()->findIMSession(session_id);
        const bool group = session && session->isGroupSessionType();
        const bool mine  = from_id == gAgentID;

        const std::string name = agentDisplayName(from_id, from_name);
        const bool emote = isEmote(text);
        const std::string spoken = emote ? emoteBody(text) : text;
        const std::string prefix = emote ? name + " " : std::string();
        const bool say_name = !emote && !mine && gSavedPerAccountSettings.getBOOL(SET_SAY_NAME);
        say(mine ? Kind::Self : (group ? Kind::Group : Kind::IM), from_id, name, spoken, prefix, say_name);
    }

    TranslatedLineScope::TranslatedLineScope(const std::string& line, size_t original_size, bool failed)
    :   mLine(line),
        mOriginalSize(original_size),
        mFailed(failed),
        mPrevious(sTranslatedLine)
    {
        sTranslatedLine = this;
    }

    TranslatedLineScope::~TranslatedLineScope()
    {
        sTranslatedLine = mPrevious;
    }

    EngineStatus engineStatus()
    {
        Engine* e = engine();
        if (!e || e->failed())
        {
            return EngineStatus::Failed;
        }
        return e->ready() ? EngineStatus::Ready : EngineStatus::Starting;
    }

    int avatarVoiceMode(const LLUUID& avatar_id)
    {
        readOverrides();
        const auto found = sOverrides.find(avatar_id);
        return found == sOverrides.end() ? 0 : found->second;
    }

    void setAvatarVoiceMode(const LLUUID& avatar_id, int mode)
    {
        if (avatar_id.isNull())
        {
            return;
        }
        readOverrides();
        if (mode == 1 || mode == 2)
        {
            sOverrides[avatar_id] = mode;
        }
        else
        {
            sOverrides.erase(avatar_id);
        }
        // The next line of this avatar takes the new voice, not the one kept for the conversation.
        sSpeakers.erase(avatar_id);
        writeOverrides();
    }

    void clearAvatarVoiceModes()
    {
        sOverrides.clear();
        sSpeakers.clear();
        writeOverrides();
    }

    void speakSample()
    {
        if (Engine* e = engine())
        {
            const S32 queue = std::clamp<S32>(gSavedPerAccountSettings.getS32(SET_MAX_QUEUE), 4, 50);
            // In the language of the viewer (strings_plus.xml).
            e->speak(makeUtterance(LLTrans::getString("PlusTtsSampleMale"), VoiceGender::Male, "sample"), queue);
            e->speak(makeUtterance(LLTrans::getString("PlusTtsSampleFemale"), VoiceGender::Female, "sample"), queue);
            e->speak(makeUtterance(LLTrans::getString("PlusTtsSampleNeutral"), VoiceGender::Neutral, "sample"), queue);
        }
    }

    void stopSpeaking()
    {
        if (sEngine)
        {
            sEngine->clear();
        }
    }

    std::vector<VoiceName> availableVoices()
    {
        std::vector<VoiceName> out;
        if (Engine* e = engine())
        {
            for (const VoiceInfo& v : e->voices())
            {
                VoiceName voice;
                voice.name     = v.name;
                voice.language = v.language;
                voice.male     = v.gender == VoiceGender::Male;
                out.push_back(std::move(voice));
            }
        }
        return out;
    }
}
