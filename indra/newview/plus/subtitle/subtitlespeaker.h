/**
 * @file subtitlespeaker.h
 * @brief Subtitles (plus/subtitle, research/137 section 5): who was speaking while a sentence was heard.
 *
 * The viewer knows, for every voice participant, whether they are speaking and how loud (from the
 * voice server). The window samples that ten times a second; when the recognizer returns a sentence
 * with its time span, the participant who was speaking longest (and loudest) in that span is the
 * speaker. No viewer dependencies: subtitle_test.cpp builds this on its own.
 *
 * Times are seconds of audio since the subtitles started (plus::subtitle::tap::producedSamples()),
 * the same clock the recognizer's t0 / t1 use.
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

#ifndef PLUS_SUBTITLE_SPEAKER_H
#define PLUS_SUBTITLE_SPEAKER_H

#include <algorithm>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace plus
{
    namespace subtitle
    {
        class SpeakerTracker
        {
        public:
            struct Voice
            {
                std::string id;       // the participant (an agent id as text)
                bool speaking = false;
                float power = 0.0f;   // 0..1, how loud
            };

            struct Attribution
            {
                std::string id;           // empty: nobody was seen speaking
                bool others = false;      // somebody else spoke a good part of the time too
                double share = 0.0;       // the speaker's part of all the speaking seen (0..1)
            };

            // Add what the voice server says at time `t` (seconds, never going back).
            void add(double t, const std::vector<Voice>& voices)
            {
                if (!mEntries.empty())
                {
                    if (t < mEntries.back().t)
                    {
                        return;
                    }
                    if (t == mEntries.back().t)
                    {
                        // The clock stands still while no voice is played (it counts the sound heard), but the
                        // window keeps looking: keep only the newest look. An entry that lasts no time counts
                        // for nothing in attribute(), so this changes no answer and the list stops growing.
                        mEntries.back().voices = voices;
                        return;
                    }
                }
                mEntries.push_back({ t, voices });
                while (mEntries.size() > MAX_ENTRIES)   // a last guard: forget() keeps it far below this
                {
                    mEntries.pop_front();
                }
            }

            // Who spoke the sentence heard from t0 to t1? What the server says reaches us `latency`
            // seconds after the sound (it can be negative), and `margin` seconds are added on both sides
            // to catch a word that starts or ends late.
            Attribution attribute(double t0, double t1, double latency, double margin) const
            {
                const double a = t0 - margin + latency;
                const double b = t1 + margin + latency;
                std::map<std::string, double> score;
                for (size_t i = 0; i < mEntries.size(); ++i)
                {
                    const Entry& e = mEntries[i];
                    const double until = (i + 1 < mEntries.size()) ? mEntries[i + 1].t : e.t + DEFAULT_STEP;
                    const double overlap = std::min(until, b) - std::max(e.t, a);
                    if (overlap <= 0.0)
                    {
                        continue;
                    }
                    for (const Voice& v : e.voices)
                    {
                        if (v.speaking)
                        {
                            score[v.id] += overlap * (0.5 + std::min(1.0f, std::max(0.0f, v.power)));
                        }
                    }
                }
                Attribution result;
                double best = 0.0, second = 0.0, total = 0.0;
                for (const auto& s : score)
                {
                    total += s.second;
                    if (s.second > best)
                    {
                        second = best;
                        best = s.second;
                        result.id = s.first;
                    }
                    else if (s.second > second)
                    {
                        second = s.second;
                    }
                }
                if (best <= 0.0)
                {
                    result.id.clear();
                    return result;
                }
                result.share = best / total;
                result.others = second >= OTHERS_RATIO * best;
                return result;
            }

            // Drop what is older than `t` (the sentences being heard are seconds long: a minute is plenty).
            void forget(double t)
            {
                while (mEntries.size() > 1 && mEntries[1].t < t)
                {
                    mEntries.pop_front();
                }
            }

            void clear() { mEntries.clear(); }
            size_t size() const { return mEntries.size(); }

        private:
            static constexpr double DEFAULT_STEP = 0.1;   // how long the newest entry counts for
            static constexpr double OTHERS_RATIO = 0.6;   // a second voice this much of the first counts as "others"
            static constexpr size_t MAX_ENTRIES  = 6000;  // 10 minutes of looks ten times a second

            struct Entry
            {
                double t;
                std::vector<Voice> voices;
            };
            std::deque<Entry> mEntries;
        };
    }
}

#endif // PLUS_SUBTITLE_SPEAKER_H
