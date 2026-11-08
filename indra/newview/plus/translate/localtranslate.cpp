/**
 * @file localtranslate.cpp
 * @brief Translation (plus/translate, research/228): "this PC" as a translation service of the chat translation.
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

#include "localtranslate.h"
#include "langdetect.h"
#include "textsplit.h"

#include "plus/subtitle/subtitleclient.h"
#include "plus/subtitle/subtitledialect.h"
#include "plus/subtitle/subtitletranslate.h"
#include "plus/subtitle/translateclient.h"
#include "plus/subtitle/translatefetch.h"

#include "llfile.h"
#include "lleventtimer.h"
#include "lltimer.h"
#include "llviewercontrol.h"

#include <map>
#include <memory>
#include <vector>

namespace plus
{
    namespace translate
    {
        namespace
        {
            constexpr int    HELPER_THREADS    = 2;       // processor threads of the helper
            constexpr double IDLE_STOP_SECONDS = 300.0;   // the helper is stopped when nothing was translated for this long
            constexpr double ANSWER_SECONDS    = 12.0;    // a sentence not answered in this long is given up (the lines behind it wait for it)
            constexpr double FIRST_ANSWER_SECONDS = 60.0;   // ... until the helper has answered once: it reads the model of a direction when
                                                          // the first sentence of it comes (a slow disk and 500 MB take their time)
            constexpr int    START_TRIES       = 3;       // starts in a row that failed before the helper is left alone for a while
            constexpr double RETRY_SECONDS     = 60.0;    // ... this long
            constexpr double EARLY_EXIT_SECONDS = 15.0;   // a helper that ends this soon after its start without one answer is a failed start
            constexpr F32    TICK_BUSY         = 0.2f;    // seconds between looks at the helper's answers while sentences wait
            constexpr F32    TICK_IDLE         = 5.0f;    // ... and while the helper only waits for its idle stop
            constexpr double LOG_QUIET_SECONDS = 30.0;    // a kind of log line is written again after this long at the earliest

            std::string sProblem;   // what last kept the local translation from working

            // The log of the local translation says, for each chat line, which way it went (left as it is and why, translated
            // and how long it took, no answer). No chat text and no names go into it: languages, counts and times only. A kind of
            // line is written the first time, then at most once in LOG_QUIET_SECONDS with the number held back in between, so a
            // busy place does not fill the log.
            struct LogGate
            {
                double last = 0.0;
                bool written = false;
                unsigned held = 0;
            };
            std::map<std::string, LogGate> sLogGates;   // by kind (a reason with its languages: a few dozen at most)

            void logGated(const std::string& kind, const std::string& line)
            {
                LogGate& g = sLogGates[kind];
                const double now = LLTimer::getTotalSeconds();
                if (g.written && now - g.last < LOG_QUIET_SECONDS)
                {
                    ++g.held;
                    return;
                }
                if (g.held > 0)
                {
                    LL_INFOS("Translate") << "local translation: " << line << " (and " << g.held << " like it since the last such line)"
                                          << LL_ENDL;
                }
                else
                {
                    LL_INFOS("Translate") << "local translation: " << line << LL_ENDL;
                }
                g.written = true;
                g.last = now;
                g.held = 0;
            }
            // Failed starts in a row, and when the last was. Kept here, not in the timer: the timer is deleted when the helper is gone,
            // and a broken helper must not be started again for every line of chat.
            int sStartFailures = 0;
            double sLastStartFailure = 0.0;

            // One chat line being translated: its pieces, and what became of each.
            struct Job
            {
                LLTranslate::TranslationSuccess_fn success;
                std::vector<TextPiece> pieces;
                std::vector<std::string> answer;   // per piece: the answer, or the original when it was not translated
                size_t remaining = 0;              // sentences sent and not answered yet
                bool translatedAny = false;
                std::string detected;              // the language the first translated piece was in
                std::string target;                // the language translated into
                // for the log
                double started = 0.0;              // when the line came
                size_t sent = 0;                   // sentences handed to the helper
                size_t givenUp = 0;                // ... not answered in time, or the helper went away
                size_t failed = 0;                 // ... answered with an error
            };

            struct Waiting
            {
                std::shared_ptr<Job> job;
                size_t piece = 0;
                double sent = 0.0;
            };

            // What became of a line that was handed to the helper.
            void logOutcome(const Job& job)
            {
                const std::string pair = job.detected + "-" + job.target;
                const int ms = int((LLTimer::getTotalSeconds() - job.started) * 1000.0);
                if (job.translatedAny)
                {
                    std::string line = "translated a line (" + pair + ", " + std::to_string(ms) + " ms)";
                    if (job.givenUp > 0 || job.failed > 0)
                    {
                        line += ", " + std::to_string(job.givenUp + job.failed) + " of " + std::to_string(job.sent) + " pieces left as they are";
                    }
                    logGated("translated " + pair, line);
                }
                else if (job.givenUp > 0)
                {
                    logGated("no answer", "the translator did not answer (" + pair + ", gave up after " + std::to_string(ms) + " ms)");
                }
                else if (job.failed > 0)
                {
                    logGated("failed " + pair, "the translator could not translate the line (" + pair + ")");
                }
                else
                {
                    logGated("empty " + pair, "the translator answered with no text (" + pair + ")");
                }
            }

            void finish(const std::shared_ptr<Job>& job)
            {
                if (job->sent > 0)
                {
                    logOutcome(*job);
                }
                if (!job->translatedAny)
                {
                    job->success(std::string(), job->detected);   // nothing translated: the line stays as it is, with no note
                    return;
                }
                std::string joined;
                for (const std::string& s : job->answer)
                {
                    joined += s;
                }
                job->success(joined, job->detected);
            }

            // Watches the helper's answers (every 0.2 s while sentences wait), and ends the helper after the idle time. The timer
            // deletes itself then; the next sentence makes a new one.
            class LocalTranslator : public LLEventTimer
            {
            public:
                static LocalTranslator& get()
                {
                    if (!sSelf)
                    {
                        sSelf = new LocalTranslator;
                    }
                    return *sSelf;
                }

                ~LocalTranslator() override
                {
                    mClient.stop();
                    sSelf = nullptr;
                    // sentences still waiting: their lines are shown as they are
                    std::map<uint64_t, Waiting> left;
                    left.swap(mWaiting);
                    for (auto& w : left)
                    {
                        ++w.second.job->givenUp;
                        if (--w.second.job->remaining == 0)
                        {
                            finish(w.second.job);
                        }
                    }
                }

                // The helper is running, or is started now. False (and sProblem says why) when it cannot be.
                bool ensureRunning()
                {
                    if (mClient.running())
                    {
                        return true;
                    }
                    const double now = LLTimer::getTotalSeconds();
                    if (sStartFailures >= START_TRIES && now - sLastStartFailure < RETRY_SECONDS)
                    {
                        logGated("resting", "the translator is not started: " + std::to_string(sStartFailures) +
                                                " starts failed in a row (tried again a minute after the last)");
                        return false;
                    }
                    const std::string exe = subtitle::helperPath("charis-translate.exe");
                    std::string error;
                    if (!LLFile::isfile(exe))
                    {
                        error = "charis-translate.exe is missing next to the viewer";
                    }
                    else if (mClient.start(exe, subtitle::translateModelRoot(), HELPER_THREADS, error))
                    {
                        // a start that works is counted when the helper answers (see tick())
                        mStartedAt = now;
                        mAnswered = false;
                        sProblem.clear();
                        LL_INFOS("Translate") << "local translation: started charis-translate.exe (" << HELPER_THREADS << " threads)" << LL_ENDL;
                        return true;
                    }
                    ++sStartFailures;
                    sLastStartFailure = now;
                    sProblem = error;
                    LL_WARNS("Translate") << "local translation: " << error << LL_ENDL;
                    return false;
                }

                // Sends one sentence of a job. False when it could not be sent (the piece stays as it is).
                bool send(const std::shared_ptr<Job>& job, size_t piece, const std::string& from, const std::string& to)
                {
                    const uint64_t id = ++mNextId;
                    if (!mClient.request(id, from, to, job->pieces[piece].text))
                    {
                        return false;
                    }
                    Waiting w;
                    w.job = job;
                    w.piece = piece;
                    w.sent = LLTimer::getTotalSeconds();
                    mWaiting[id] = w;
                    ++job->remaining;
                    ++job->sent;
                    mLastActive = w.sent;
                    mPeriod = TICK_BUSY;
                    mEventTimer.reset();
                    return true;
                }

                bool tick() override
                {
                    subtitle::TranslateClient::Result r;
                    while (mClient.poll(r))
                    {
                        if (r.id == 0)
                        {
                            LL_INFOS("Translate") << "local translation: " << r.message << LL_ENDL;   // a message of the helper that belongs to no sentence
                            continue;
                        }
                        auto it = mWaiting.find(r.id);
                        if (it == mWaiting.end())
                        {
                            continue;
                        }
                        Waiting w = it->second;
                        mWaiting.erase(it);
                        mAnswered = true;
                        sStartFailures = 0;   // it works
                        if (r.ok && !r.text.empty())
                        {
                            w.job->answer[w.piece] = r.text;
                            w.job->translatedAny = true;
                        }
                        else if (!r.ok)
                        {
                            ++w.job->failed;
                            LL_INFOS("Translate") << "local translation failed: " << r.message << LL_ENDL;
                        }
                        mLastActive = LLTimer::getTotalSeconds();
                        if (--w.job->remaining == 0)
                        {
                            finish(w.job);
                        }
                    }

                    const double now = LLTimer::getTotalSeconds();
                    // a helper that ended soon after its start without one answer (a broken helper or model) counts as a failed start
                    if (mStartedAt > 0.0 && !mClient.running())
                    {
                        if (!mAnswered && now - mStartedAt < EARLY_EXIT_SECONDS)
                        {
                            ++sStartFailures;
                            sLastStartFailure = now;
                            sProblem = "the translator stopped right after it started";
                            LL_WARNS("Translate") << "local translation: " << sProblem << LL_ENDL;
                        }
                        mStartedAt = 0.0;
                    }
                    if (!mWaiting.empty())
                    {
                        // the helper went away, or does not answer: the sentences waiting are given up (the lines stay as they are)
                        const bool gone = !mClient.running();
                        std::vector<uint64_t> expired;
                        for (const auto& w : mWaiting)
                        {
                            if (gone || now - w.second.sent > (mAnswered ? ANSWER_SECONDS : FIRST_ANSWER_SECONDS))
                            {
                                expired.push_back(w.first);
                            }
                        }
                        for (const uint64_t id : expired)
                        {
                            Waiting w = mWaiting[id];
                            mWaiting.erase(id);
                            ++w.job->givenUp;
                            if (--w.job->remaining == 0)
                            {
                                finish(w.job);
                            }
                        }
                        if (gone)
                        {
                            sProblem = "the translator stopped";
                        }
                    }
                    if (mWaiting.empty())
                    {
                        mPeriod = TICK_IDLE;
                        if (now - mLastActive > IDLE_STOP_SECONDS || !mClient.running())
                        {
                            return true;   // deleted: stops the helper in the destructor
                        }
                    }
                    return false;
                }

            private:
                LocalTranslator() : LLEventTimer(TICK_BUSY) {}

                static LocalTranslator* sSelf;

                subtitle::TranslateClient mClient;
                std::map<uint64_t, Waiting> mWaiting;
                uint64_t mNextId = 0;
                double mLastActive = 0.0;
                double mStartedAt = 0.0;   // when the helper was started (0: not running)
                bool mAnswered = false;    // it has answered a sentence since
            };

            LocalTranslator* LocalTranslator::sSelf = nullptr;

            // The setting TranslationService: asked several times for every line of chat (LLTranslate's hooks), so the cached
            // value, not a look-up by name.
            const std::string& translationService()
            {
                static LLCachedControl<std::string> sService(gSavedSettings, "TranslationService", "");
                return sService();
            }

            std::string orNone(const std::string& value)
            {
                return value.empty() ? std::string("(none)") : value;
            }
        }

        bool isLocalService()
        {
            return translationService() == "local";
        }

        void noteChatLine()
        {
            static LLCachedControl<bool> sChat(gSavedSettings, "TranslateChat", false);
            static LLCachedControl<std::string> sLanguage(gSavedSettings, "TranslateLanguage", "");
            static bool sLogged = false;
            static bool sLastChat = false;
            static std::string sLastService, sLastLanguage;
            const bool chat = sChat;
            const std::string& service = translationService();
            const std::string& language = sLanguage();
            if (sLogged && chat == sLastChat && service == sLastService && language == sLastLanguage)
            {
                return;   // the usual case: nothing changed since the last line
            }
            sLogged = true;
            sLastChat = chat;
            sLastService = service;
            sLastLanguage = language;
            logState("nearby chat", chat, service, language, LLTranslate::getTranslateLanguage());
        }

        void logState(const char* when, bool chat, const std::string& service, const std::string& language, const std::string& target)
        {
            std::string line = std::string("chat translation (") + when + "): TranslateChat " + (chat ? "on" : "off") + ", service " +
                               orNone(service) + ", TranslateLanguage " + orNone(language) + " -> " + orNone(target);
            if (service == "local")
            {
                line += localReady(target) ? ", a model for it is installed" : ", no model for it is installed";
            }
            LL_INFOS("Translate") << line << LL_ENDL;
        }

        bool localReady(const std::string& to)
        {
            for (const std::string& pair : subtitle::translatePairsFor(subtitle::primaryLanguage(to)))
            {
                if (subtitle::translatePairInstalled(pair))
                {
                    return true;
                }
            }
            // the Korean model alone reads Korean into Japanese or English
            const std::string target = subtitle::primaryLanguage(to);
            return (target == "ja" || target == "en") && subtitle::translateKoreanInstalled();
        }

        void translateLocal(const std::string& from_lang, const std::string& to, const std::string& text,
                            LLTranslate::TranslationSuccess_fn success, LLTranslate::TranslationFailure_fn failure)
        {
            const std::string target = subtitle::primaryLanguage(to);
            if (target.empty())
            {
                logGated("no target", "no language to translate into");
                failure(0, "no language to translate into");
                return;
            }
            std::shared_ptr<Job> job(new Job);
            job->success = success;
            job->target = target;
            job->started = LLTimer::getTotalSeconds();
            job->pieces = splitForTranslation(text);
            for (const TextPiece& p : job->pieces)
            {
                job->answer.push_back(p.text);
            }

            // Which pieces can be translated: a language known (told, or found out), one that a model reads into the target,
            // and the model there. Why the first piece left out was left out goes to the log when nothing is translated.
            struct Todo
            {
                size_t piece;
                std::string from;
            };
            std::vector<Todo> todo;
            const std::string told = subtitle::primaryLanguage(from_lang);
            std::string skipKind;   // "" while no piece was left out
            std::string skipLine;
            auto skip = [&](const std::string& kind, const std::string& line) {
                if (skipKind.empty())
                {
                    skipKind = kind;
                    skipLine = line;
                }
            };
            bool missingModel = false;
            for (size_t i = 0; i < job->pieces.size(); ++i)
            {
                if (!job->pieces[i].translate)
                {
                    continue;
                }
                const std::string lang = told.empty() ? detectLanguage(job->pieces[i].text) : told;
                if (lang.empty())
                {
                    skip("unknown " + target, "line left as it is: its language is not known (target " + target + ")");
                    continue;
                }
                if (lang == target)
                {
                    skip("same " + target, "line left as it is: it is in the target language already (" + target + ")");
                    continue;
                }
                const std::string pair = lang + "-" + target;
                if (!subtitle::localPairSupported(lang, target))
                {
                    skip("unsupported " + pair, "line left as it is: no model translates " + pair);
                    continue;
                }
                if (!subtitle::translatePairInstalled(pair))
                {
                    missingModel = true;
                    skip("no model " + pair, "line left as it is: the model for " + pair + " is not installed (subtitle/translate/" +
                                                 subtitle::translateArchiveOfPair(pair) + ")");
                    continue;
                }
                if (job->detected.empty())
                {
                    job->detected = lang;
                }
                todo.push_back({ i, lang });
            }
            if (missingModel && todo.empty())
            {
                sProblem = "the translation models for " + target + " are not fetched yet";
            }
            if (todo.empty())
            {
                if (skipKind.empty())
                {
                    logGated("nothing", "line left as it is: nothing in it to translate (no letters)");
                }
                else
                {
                    logGated(skipKind, skipLine);
                }
                finish(job);   // nothing to translate: the line stays as it is
                return;
            }

            LocalTranslator& translator = LocalTranslator::get();
            if (!translator.ensureRunning())
            {
                logGated("not running", "line left as it is: the translator is not running (" + sProblem + ")");
                finish(job);
                return;
            }
            for (const Todo& t : todo)
            {
                translator.send(job, t.piece, t.from, target);
            }
            if (job->remaining == 0)
            {
                logGated("not sent", "line left as it is: the translator did not take it (its queue is full or it has stopped)");
                finish(job);   // none could be sent
            }
        }

        LLTranslate::TranslationSuccess_fn withDialect(const std::string& to, LLTranslate::TranslationSuccess_fn success)
        {
            if (subtitle::primaryLanguage(to) != "ja")
            {
                return success;
            }
            static LLCachedControl<std::string> sDialect(gSavedSettings, "PlusTranslateDialect", "");
            const std::string id = sDialect();
            if (!subtitle::knownDialect(id))
            {
                return success;
            }
            return [success, id](std::string text, std::string lang) { success(subtitle::applyDialect(text, id), lang); };
        }

        std::string localProblem()
        {
            return sProblem;
        }
    }
}
