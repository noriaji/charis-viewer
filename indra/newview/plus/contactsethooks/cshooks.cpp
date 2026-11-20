/**
 * @file cshooks.cpp
 * @brief Contact sets where they show (cshooks.h, research/333).
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

#include "cshooks.h"
#include "plus/contactsets/lggcontactsets.h"
#include "plus/rlv/rlvhooks.h"

#include <array>
#include <unordered_map>

#include "llapp.h"
#include "llcontrol.h"
#include "llframetimer.h"
#include "llviewercontrol.h"

namespace plus::cshooks
{
    namespace
    {
        // How long an answer of colorFor() is kept, in seconds (frame time, F64): a set colour changed without
        // LGGContactSets' change signal, a friendship begun or ended, show within this.
        constexpr F64 CACHE_SECONDS = 1.0;
        // Answers kept per place at most; a full table is emptied (the avatars around come back within a frame).
        constexpr size_t CACHE_MAX_ENTRIES = 2048;
        // The places (ContactSetType's values, CHAT .. FRIENDS).
        constexpr size_t PLACE_COUNT = static_cast<size_t>(ContactSetType::FRIENDS) + 1;

        struct Answer
        {
            F64      checkedSeconds = 0.0;  // LLFrameTimer::getElapsedSeconds() when asked
            U32      generation = 0;        // State::generation when asked
            U32      rlvGeneration = 0;     // plus::rlv::showNamesGeneration() when asked (0 with RLVa off)
            bool     has = false;
            LLColor4 color;
        };

        struct State
        {
            // Whether any contact set exists, as of the frame frame (the first ask of each frame looks again).
            bool  frameKnown = false;
            U32   frame = 0;
            bool  setsExist = false;
            // Counts LGGContactSets' change signals: an answer of an earlier generation is asked again.
            U32   generation = 0;
            boost::signals2::scoped_connection setsChanged;    // to LGGContactSets::mChangedSignal
            std::array<std::unordered_map<LLUUID, Answer>, PLACE_COUNT> answers;
            boost::signals2::signal<void()> changed;            // onChanged()
        };

        State& state()
        {
            static State s;
            return s;
        }

        // LGGContactSets exists and may be used (not before it is made at login, not while the viewer quits:
        // asking LLSingleton then would make it again).
        bool contactSetsUsable()
        {
            return LLApp::isRunning() && LGGContactSets::instanceExists();
        }

        void onContactSetsChanged()
        {
            State& s = state();
            ++s.generation;
            s.frameKnown = false;
            s.changed();
        }

        // Connected to LGGContactSets' change signal once it exists (again if it was made anew). A
        // scoped_connection: when LGGContactSets goes first (LLSingleton's cleanup at quit), disconnecting is a no-op.
        void connectToContactSets(LGGContactSets& sets)
        {
            State& s = state();
            if (!s.setsChanged.connected())
            {
                s.setsChanged = sets.setContactSetChangeCallback([](LGGContactSets::EContactSetUpdate) { onContactSetsChanged(); });
                ++s.generation; // what was kept before the connection may be stale
            }
        }

        // Any contact set exists: looked up on the first ask of each frame, the same answer for the frame after.
        bool setsExist()
        {
            State& s = state();
            const U32 frame = LLFrameTimer::getFrameCount();
            if (!s.frameKnown || s.frame != frame)
            {
                s.frameKnown = true;
                s.frame = frame;
                s.setsExist = false;
                if (contactSetsUsable())
                {
                    LGGContactSets& sets = LGGContactSets::instance();
                    connectToContactSets(sets);
                    s.setsExist = sets.hasSets();
                }
            }
            return s.setsExist;
        }

        // The place's FSContactSetsColorize* (cs-core defines them in settings_plus.xml; Firestorm's names and defaults).
        bool colorizeOn(ContactSetType type)
        {
            static LLCachedControl<bool> chat(gSavedSettings, "FSContactSetsColorizeChat", false);
            static LLCachedControl<bool> tag(gSavedSettings, "FSContactSetsColorizeNameTag", false);
            static LLCachedControl<bool> radar(gSavedSettings, "FSContactSetsColorizeRadar", false);
            static LLCachedControl<bool> minimap(gSavedSettings, "FSContactSetsColorizeMiniMap", false);
            static LLCachedControl<bool> friends(gSavedSettings, "FSContactSetsColorizeFriends", false);
            switch (type)
            {
                case ContactSetType::CHAT:
                case ContactSetType::IM:
                    return chat;
                case ContactSetType::TAG:
                    return tag;
                case ContactSetType::RADAR:
                    return radar;
                case ContactSetType::MINIMAP:
                    return minimap;
                case ContactSetType::FRIENDS:
                    return friends;
            }
            return false;
        }

        // LGGContactSets to ask about id: loaded, with at least one set, and id not null; else null (for the callers
        // that run on events, not on every frame: no frame cache).
        const LGGContactSets* setsToAsk(const LLUUID& id)
        {
            if (id.isNull() || !contactSetsUsable())
            {
                return nullptr;
            }
            const LGGContactSets& sets = LGGContactSets::instance();
            return sets.hasSets() ? &sets : nullptr;
        }

        bool notifies(const LLUUID& id)
        {
            const LGGContactSets* sets = setsToAsk(id);
            return sets && sets->notifyForFriend(id);
        }

        // The answer of LGGContactSets::hasFriendColorThatShouldShow() for id at the place type, kept (Answer) or
        // asked again when older than CACHE_SECONDS or of an earlier generation. LGGContactSets must be usable.
        const Answer& answerFor(const LLUUID& id, ContactSetType type, U32 rlv_generation)
        {
            State& s = state();
            auto& answers = s.answers[static_cast<size_t>(type)];
            const F64 now = LLFrameTimer::getElapsedSeconds().value();
            auto found = answers.find(id);
            if (found != answers.end()
                && found->second.generation == s.generation
                && found->second.rlvGeneration == rlv_generation
                && now - found->second.checkedSeconds < CACHE_SECONDS)
            {
                return found->second;
            }
            if (found == answers.end() && answers.size() >= CACHE_MAX_ENTRIES)
            {
                answers.clear();
            }
            Answer answer;
            answer.checkedSeconds = now;
            answer.generation = s.generation;
            answer.rlvGeneration = rlv_generation;
            answer.has = LGGContactSets::instance().hasFriendColorThatShouldShow(id, type, answer.color);
            return answers.insert_or_assign(id, answer).first->second;
        }
    }

    bool colorFor(const LLUUID& id, ContactSetType type, LLColor4& color)
    {
        if (type == ContactSetType::IM)
        {
            type = ContactSetType::CHAT;
        }
        if (!colorizeOn(type) || id.isNull() || !setsExist())
        {
            return false;
        }
        // RLVa @shownames: the colour would tell who it is (LGGContactSets checks it too; asked here as the places
        // that anonymize the name do).
        const bool rlv = plus::rlv::enabled();
        if (rlv && !plus::rlv::canShowMenuName(id))
        {
            return false;
        }

        const Answer& answer = answerFor(id, type, rlv ? plus::rlv::showNamesGeneration() : 0); // setsExist(): usable
        if (!answer.has)
        {
            return false;
        }
        color = answer.color;
        return true;
    }

    bool notifyToast(const LLUUID& id)
    {
        static LLCachedControl<bool> toast(gSavedSettings, "FSContactSetsNotificationToast", false);
        return toast && notifies(id);
    }

    bool notifyNearbyChat(const LLUUID& id)
    {
        static LLCachedControl<bool> nearby_chat(gSavedSettings, "FSContactSetsNotificationNearbyChat", true);
        return nearby_chat && notifies(id);
    }

    bool busyResponse(const LLUUID& id, std::string& response)
    {
        const LGGContactSets* sets = setsToAsk(id);
        std::string set_response;
        if (!sets || !sets->getAutoresponseForFriend(id, ContactSetAutoresponseMode::BUSY, set_response))
        {
            return false;
        }
        response = set_response;
        return true;
    }

    bool hasPseudonym(const LLUUID& id)
    {
        // The aliases are kept apart from the sets: an alias may exist with no set.
        return id.notNull() && contactSetsUsable() && LGGContactSets::instance().hasPseudonym(id);
    }

    boost::signals2::connection onChanged(const std::function<void()>& callback)
    {
        if (contactSetsUsable())
        {
            connectToContactSets(LGGContactSets::instance());
        }
        return state().changed.connect(callback);
    }
}
