/**
 * @file subtitlefloater.cpp
 * @brief Subtitles (plus/subtitle, research/137): the window. While it is open, the voice you hear
 *        is listened to and written out, each sentence with the name of who spoke it; closing it
 *        stops everything and frees the helper's memory.
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

#include "plus.h"
#include "plus/rlv/rlvhooks.h"
#include "streamtap.h"
#include "subtitleclient.h"
#include "subtitledialect.h"
#include "subtitledownload.h"
#include "translateclient.h"
#include "translatefetch.h"
#include "subtitlering.h"
#include "subtitlespeaker.h"
#include "subtitletranslate.h"

#include "llagent.h"
#include "llaudioengine.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llbutton.h"
#include "lldir.h"
#include "llfile.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "lltextbox.h"
#include "lltranslate.h"
#include "lltexteditor.h"
#include "llviewercontrol.h"
#include "llviewermedia_streamingaudio.h"
#include "llvoiceclient.h"

#include <deque>
#include <set>
#include <vector>

namespace plus
{
    namespace subtitle
    {
        namespace
        {
            constexpr char FLOATER_NAME[]       = "plus_subtitle";
            constexpr char SETTING_THREADS[]    = "PlusSubtitleThreads";
            constexpr char SETTING_LANGUAGE[]   = "PlusSubtitleLanguage";
            constexpr char SETTING_LINES[]      = "PlusSubtitleLines";
            constexpr char SETTING_LATENCY[]    = "PlusSubtitleLatency";
            constexpr char SETTING_FLAGS[]      = "PlusSubtitleUseFlags";
            constexpr char SETTING_TRANSLATE[]  = "PlusSubtitleTranslate";
            constexpr char SETTING_TRANSLATE_TO[] = "PlusSubtitleTranslateTo";
            constexpr char SETTING_STREAM[]     = "PlusSubtitleStream";
            constexpr char SETTING_TRANSLATE_SERVICE[]   = "PlusSubtitleTranslateService";
            constexpr char SETTING_DIALECT[]             = "PlusTranslateDialect";   // shared with the chat translation (research/228)
            constexpr char HELPER_TRANSLATE[]            = "charis-translate.exe";
            constexpr int  LOCAL_TRANSLATE_THREADS       = 2;
            constexpr int  LOCAL_TRANSLATE_RESTARTS      = 3;   // a helper that keeps exiting is let go after this many starts
            constexpr char MODEL_FILE[]         = "ggml-small.bin";

            constexpr F32 SAMPLE_INTERVAL = 0.1f;    // seconds between looks at who is speaking
            constexpr F32 NAME_RETRY      = 1.0f;    // seconds between tries to show names that were not known yet
            constexpr double HISTORY      = 60.0;    // seconds of "who was speaking" kept
            constexpr double MARGIN       = 0.3;     // seconds added on both sides of a sentence when looking for its speaker

            struct Line
            {
                uint64_t serial = 0;  // the translation's answer finds its line by this
                LLUUID id;            // null: nobody was seen speaking
                bool others = false;  // somebody else spoke a good part of it too
                bool radio = false;   // heard from the parcel's internet radio, not the voice (research/192)
                std::string station;  // radio: the station's host name
                std::string lang;
                std::string text;
                std::string translation;   // empty: none (yet, or not wanted, or it failed)
            };

            class SubtitleFloater : public LLFloater
            {
                friend class ::LLFloaterReg;

            public:
                ~SubtitleFloater() override
                {
                    mClient.stop();
                    mTranslator.stop();
                    streamtap::stop();
                }

                bool postBuild() override
                {
                    mLog    = getChild<LLTextEditor>("log");
                    mStatus = getChild<LLTextBox>("status");
                    mDownloadButton = getChild<LLButton>("download_model");
                    mDownloadButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onDownloadButton(); });
                    mDownloadButton->setVisible(false);
                    mFetchButton = getChild<LLButton>("download_translate_model");
                    mFetchButton->setCommitCallback([this](LLUICtrl*, const LLSD&) { onFetchButton(); });
                    mFetchButton->setVisible(false);
                    mFetchTip = mFetchButton->getToolTip();
                    getChild<LLUICtrl>("translate_service")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTranslateServiceChanged(); });
                    getChild<LLUICtrl>("translate")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTranslateServiceChanged(); });
                    getChild<LLUICtrl>("translate_to")->setCommitCallback([this](LLUICtrl*, const LLSD&) { onTranslateServiceChanged(); });
                    return true;
                }

                void onOpen(const LLSD& key) override
                {
                    LLFloater::onOpen(key);
                    mLines.clear();
                    mTracker.clear();
                    mSampleClock = 0.0;
                    mDroppedSeconds = 0.0;
                    // mNextSerial only grows: a translation asked for before the window was closed can
                    // still come back, and must not land on a new line that reused its number
                    mListening = false;
                    mUnresolved = false;
                    mLocalStarts = 0;
                    mLocalNoticeShown = false;
                    refreshLog();
                    const TranslateModelFetch::State fs = mFetch.state();
                    mWatchFetch = (fs == TranslateModelFetch::State::Running || fs == TranslateModelFetch::State::Unpacking);
                    updateFetchButton();
                    const ModelDownload::State dl = mDownload.state();
                    if (dl == ModelDownload::State::Running || dl == ModelDownload::State::Verifying)
                    {
                        // a download that was started before this window was closed goes on
                        mWatchDownload = true;
                        mVerifyingShown = false;
                        showDownloadButton("cancel_label");
                    }
                    else
                    {
                        // a download that ended while the window was closed: begin() below starts the
                        // helper, so draw() must not start it a second time when it sees State::Done
                        mWatchDownload = false;
                        begin();
                    }
                }

                void onClose(bool app_quitting) override
                {
                    mClient.stop();
                    mTranslator.stop();
                    streamtap::stop();
                    mStreamFailedPid = 0;
                    mListening = false;
                    LLFloater::onClose(app_quitting);
                }

                void draw() override
                {
                    watchDownload();
                    watchFetch();
                    pollLocalTranslation();
                    Event e;
                    bool changed = false;
                    while (mClient.poll(e))
                    {
                        changed |= handle(e);
                    }
                    if (mListening && mSampleTimer.getElapsedTimeF32() >= SAMPLE_INTERVAL)
                    {
                        mSampleTimer.reset();
                        updateStream();
                        sampleSpeakers();
                    }
                    if (mUnresolved && mNameTimer.getElapsedTimeF32() >= NAME_RETRY)
                    {
                        mNameTimer.reset();
                        changed = true;   // some names were not known yet: try again
                    }
                    if (changed)
                    {
                        refreshLog();
                    }
                    LLFloater::draw();
                }

            private:
                SubtitleFloater(const LLSD& key) : LLFloater(key) {}

                std::string text(const std::string& name) { return getString(name); }

                void setStatus(const std::string& name)
                {
                    mStatus->setText(text(name));
                }

                void showDownloadButton(const std::string& label_name)
                {
                    mDownloadButton->setLabel(text(label_name));
                    mDownloadButton->setVisible(true);
                }

                // The button is the user's go-ahead for the download; pressing it again cancels.
                void onDownloadButton()
                {
                    const ModelDownload::State st = mDownload.state();
                    if (st == ModelDownload::State::Running || st == ModelDownload::State::Verifying)
                    {
                        mDownload.cancel();
                        mWatchDownload = false;
                        setStatus("download_cancelled");
                        showDownloadButton("download_label");
                        return;
                    }
                    std::string error;
                    if (!mDownload.start(smallModel(), modelFolder(), error))
                    {
                        LL_WARNS("Subtitle") << error << LL_ENDL;
                        setStatus("download_cannot_start");
                        return;
                    }
                    mWatchDownload = true;
                    mVerifyingShown = false;
                    mDownloadTimer.reset();
                    showDownloadButton("cancel_label");
                    showDownloadProgress();
                }

                // The local translation models (research/217). The button shows while they are wanted ("this PC" chosen,
                // translation on) and not installed, or while they are being fetched. Pressing it is the user's go-ahead
                // for the download; pressing it again cancels.
                void updateFetchButton()
                {
                    const TranslateModelFetch::State st = mFetch.state();
                    const bool busy = (st == TranslateModelFetch::State::Running || st == TranslateModelFetch::State::Unpacking);
                    const bool wanted = gSavedSettings.getBOOL(SETTING_TRANSLATE) &&
                                        gSavedSettings.getString(SETTING_TRANSLATE_SERVICE) == "local";
                    // wanted: the models of the language chosen to translate into, when there are some and they are not here
                    const std::string target = primaryLanguage(translateTarget());
                    mFetchButton->setVisible(busy || (wanted && !translatePairsFor(target).empty() && !translateModelsInstalledFor(target)));
                    mFetchButton->setLabel(text(busy ? "cancel_label" : "translate_fetch_label"));
                    // The sources and licenses of the models it fetches (some ask for the credit to be shown).
                    if (target != mFetchTipTarget)
                    {
                        mFetchTipTarget = target;
                        const std::string credits = translateModelCredits(target);
                        mFetchButton->setToolTip(credits.empty() ? mFetchTip : mFetchTip + "\n\n" + credits);
                    }
                }

                void onFetchButton()
                {
                    const TranslateModelFetch::State st = mFetch.state();
                    if (st == TranslateModelFetch::State::Running || st == TranslateModelFetch::State::Unpacking)
                    {
                        mFetch.cancel();
                        mWatchFetch = false;
                        setStatus("download_cancelled");
                        updateFetchButton();
                        return;
                    }
                    const std::string base = translateModelBaseUrl();
                    std::string error;
                    const std::string target = primaryLanguage(translateTarget());
                    if (!mFetch.start(base, target, error))
                    {
                        LL_WARNS("Subtitle") << error << LL_ENDL;
                        setStatus(base.empty() ? "translate_fetch_not_published" : "download_cannot_start");
                        return;
                    }
                    LL_INFOS("Subtitle") << "fetching the translation models: " << translateModelCredits(target) << LL_ENDL;
                    mWatchFetch = true;
                    mUnpackingShown = false;
                    mFetchTimer.reset();
                    updateFetchButton();
                    showFetchProgress();
                }

                void showFetchProgress()
                {
                    LLStringUtil::format_map_t args;
                    args["[PERCENT]"] = llformat("%.0f", mFetch.fraction() * 100.0);
                    std::string msg = text("translate_fetching");
                    LLStringUtil::format(msg, args);
                    mStatus->setText(msg);
                }

                // Called every frame while the window is drawn: follows the fetch to its end.
                void watchFetch()
                {
                    if (!mWatchFetch)
                    {
                        return;
                    }
                    mFetch.poll();
                    switch (mFetch.state())
                    {
                        case TranslateModelFetch::State::Running:
                            if (mFetchTimer.getElapsedTimeF32() >= 1.0f)
                            {
                                mFetchTimer.reset();
                                showFetchProgress();
                            }
                            break;
                        case TranslateModelFetch::State::Unpacking:
                            if (!mUnpackingShown)   // set once, not every frame
                            {
                                mUnpackingShown = true;
                                setStatus("translate_fetch_unpacking");
                            }
                            break;
                        case TranslateModelFetch::State::Done:
                            mWatchFetch = false;
                            setStatus("translate_fetch_done");
                            updateFetchButton();
                            break;
                        case TranslateModelFetch::State::Failed:
                            mWatchFetch = false;
                            LL_WARNS("Subtitle") << mFetch.failure() << LL_ENDL;
                            setStatus("download_failed");
                            updateFetchButton();
                            break;
                        case TranslateModelFetch::State::Idle:
                            mWatchFetch = false;
                            updateFetchButton();
                            break;
                    }
                }

                void showDownloadProgress()
                {
                    LLStringUtil::format_map_t args;
                    args["[PERCENT]"] = llformat("%.0f", mDownload.fraction() * 100.0);
                    std::string msg = text("downloading");
                    LLStringUtil::format(msg, args);
                    mStatus->setText(msg);
                }

                // Called every frame while the window is drawn: follows a download to its end.
                void watchDownload()
                {
                    if (!mWatchDownload)
                    {
                        return;
                    }
                    switch (mDownload.state())
                    {
                        case ModelDownload::State::Running:
                            if (mDownloadTimer.getElapsedTimeF32() >= 1.0f)
                            {
                                mDownloadTimer.reset();
                                showDownloadProgress();
                            }
                            break;
                        case ModelDownload::State::Verifying:
                            if (!mVerifyingShown)   // the check takes seconds: the text is set once, not every frame
                            {
                                mVerifyingShown = true;
                                setStatus("verifying");
                            }
                            break;
                        case ModelDownload::State::Done:
                            mWatchDownload = false;
                            mDownloadButton->setVisible(false);
                            begin();
                            break;
                        case ModelDownload::State::Failed:
                        {
                            mWatchDownload = false;
                            LL_WARNS("Subtitle") << mDownload.failure() << LL_ENDL;
                            setStatus("download_failed");
                            showDownloadButton("download_label");
                            break;
                        }
                        case ModelDownload::State::Idle:
                            mWatchDownload = false;
                            break;
                    }
                }

                void begin()
                {
                    mDownloadButton->setVisible(false);
                    const std::string model = modelFolder() + gDirUtilp->getDirDelimiter() + MODEL_FILE;
                    const std::string exe   = helperPath();
                    if (!LLFile::isfile(exe))
                    {
                        setStatus("no_helper");
                        return;
                    }
                    if (!LLFile::isfile(model))
                    {
                        // The place goes in the log field, which is empty now (no helper, no lines) and big
                        // enough for a long path; the status line has room for two lines only.
                        LLStringUtil::format_map_t args;
                        args["[PATH]"] = model;
                        std::string place = text("model_place");
                        LLStringUtil::format(place, args);
                        mLog->setText(place);
                        setStatus("no_model");
                        showDownloadButton("download_label");
                        return;
                    }
                    refreshLog();   // the model's place, if it was shown before the download, goes
                    std::string error;
                    const int threads = llclamp((int)gSavedSettings.getS32(SETTING_THREADS), 1, 16);
                    // The voice server's "speaking" flags say nothing about the radio: the client cuts the
                    // radio's sentences by loudness and the voice's by the flags, so the radio can be turned
                    // on and off while the window is open (research/192, fix-r5).
                    const bool use_flags = gSavedSettings.getBOOL(SETTING_FLAGS);
                    if (!mClient.start(exe, model, threads, gSavedSettings.getString(SETTING_LANGUAGE), use_flags, error))
                    {
                        LL_WARNS("Subtitle") << error << LL_ENDL;
                        setStatus("start_failed");
                        return;
                    }
                    setStatus("loading");
                }

                // The language sentences are translated into: the setting, else the one the chat translation uses.
                std::string translateTarget()
                {
                    const std::string to = gSavedSettings.getString(SETTING_TRANSLATE_TO);
                    return to.empty() ? LLTranslate::getTranslateLanguage() : to;
                }

                bool localTranslation()
                {
                    return gSavedSettings.getString(SETTING_TRANSLATE_SERVICE) == "local";
                }

                // Sends a sentence to be translated: on this PC with the local helper (research/217), else to the
                // translation service the chat translation uses. The answer comes back on the main thread, maybe
                // after this window is gone: the service's answer finds the window by handle.
                void translate(const Line& line)
                {
                    if (!gSavedSettings.getBOOL(SETTING_TRANSLATE))
                    {
                        return;
                    }
                    const std::string to = translateTarget();
                    if (!needsTranslation(line.lang, to))
                    {
                        return;
                    }
                    if (localTranslation())
                    {
                        translateLocal(line, to);
                        return;
                    }
                    // Only the value "service" sends text to the translation service: a setting that is neither "local" nor
                    // "service" (a slip in the settings file) is no reason to send what was said out of this PC (audit C, I4).
                    if (gSavedSettings.getString(SETTING_TRANSLATE_SERVICE) != "service" || !LLTranslate::isTranslationConfigured())
                    {
                        return;
                    }
                    LLHandle<LLFloater> handle = getHandle();
                    const uint64_t serial = line.serial;
                    LLTranslate::translateMessage(
                        line.lang, to, line.text,
                        [handle, serial](std::string translation, std::string) {
                            if (SubtitleFloater* self = dynamic_cast<SubtitleFloater*>(handle.get()))
                            {
                                self->setTranslation(serial, translation);
                            }
                        },
                        [](int status, std::string reason) {
                            LL_INFOS("Subtitle") << "translation failed: " << status << " " << reason << LL_ENDL;
                        });
                }

                // Says something about the local translation once per opening of the window (a status that comes back
                // with every sentence would drown the log's view).
                void localNotice(const char* status)
                {
                    if (!mLocalNoticeShown)
                    {
                        mLocalNoticeShown = true;
                        setStatus(status);
                    }
                }

                // Nothing leaves this PC here, and nothing falls back to the service when the local way cannot be used:
                // the user chose "this PC".
                void translateLocal(const Line& line, const std::string& to)
                {
                    if (!localPairSupported(line.lang, to))
                    {
                        localNotice("translate_local_unsupported");
                        return;
                    }
                    if (!translatePairInstalled(primaryLanguage(line.lang) + "-" + primaryLanguage(to)))
                    {
                        localNotice("translate_local_missing");
                        updateFetchButton();
                        return;
                    }
                    if (!mTranslator.running())
                    {
                        const std::string exe = helperPath(HELPER_TRANSLATE);
                        if (mLocalStarts >= LOCAL_TRANSLATE_RESTARTS || !LLFile::isfile(exe))
                        {
                            localNotice(LLFile::isfile(exe) ? "translate_local_failed" : "translate_local_no_helper");
                            return;
                        }
                        ++mLocalStarts;
                        std::string error;
                        if (!mTranslator.start(exe, translateModelRoot(), LOCAL_TRANSLATE_THREADS, error))
                        {
                            LL_WARNS("Subtitle") << error << LL_ENDL;
                            localNotice("translate_local_failed");
                            return;
                        }
                    }
                    mTranslator.request(line.serial, primaryLanguage(line.lang), primaryLanguage(to), line.text);
                }

                // Called every frame while the window is drawn: the answers of the local helper.
                void pollLocalTranslation()
                {
                    TranslateClient::Result r;
                    while (mTranslator.poll(r))
                    {
                        if (r.ok)
                        {
                            setTranslation(r.id, r.text);
                        }
                        else
                        {
                            LL_INFOS("Subtitle") << "local translation failed: " << r.message << LL_ENDL;
                        }
                    }
                }

                // The way of translating was chosen again: what it needs is not the same.
                void onTranslateServiceChanged()
                {
                    mLocalNoticeShown = false;
                    mLocalStarts = 0;
                    if (!localTranslation())
                    {
                        mTranslator.stop();   // nothing is kept running for a way that is not chosen
                    }
                    updateFetchButton();
                    if (mListening && gSavedSettings.getBOOL(SETTING_TRANSLATE))
                    {
                        setStatus(!localTranslation() && !LLTranslate::isTranslationConfigured() ? "translate_unconfigured" : "ready");
                    }
                }

                void setTranslation(uint64_t serial, const std::string& translation)
                {
                    // A translation into Japanese can be written in a dialect (research/222); only that, never the spoken text.
                    // The translations of the services come back through LLTranslate, which has written the dialect already (plus/translate);
                    // the local ones of this window do not, so only they get it here (not twice).
                    std::string shown = translation;
                    if (localTranslation() && primaryLanguage(translateTarget()) == "ja")
                    {
                        shown = applyDialect(translation, gSavedSettings.getString(SETTING_DIALECT));
                    }
                    for (Line& line : mLines)
                    {
                        if (line.serial == serial)
                        {
                            if (shown != line.text)
                            {
                                line.translation = shown;
                                refreshLog();
                            }
                            return;
                        }
                    }
                }

                // The host name of the station now playing ("" when none), for the label of its sentences.
                std::string stationName()
                {
                    std::string url = gAudiop ? gAudiop->getInternetStreamURL() : std::string();
                    const size_t scheme = url.find("://");
                    if (scheme != std::string::npos)
                    {
                        url.erase(0, scheme + 3);
                    }
                    const size_t end = url.find_first_of("/:? ");
                    if (end != std::string::npos)
                    {
                        url.erase(end);
                    }
                    return url;
                }

                // Keeps the radio's capture in step with the setting and with the stream player (research/192):
                // listening to the plugin process that plays the stream, while it plays. The capture is the
                // player's own sound, so there is no second connection to the station. Ten times a second at
                // most, and only while this window is open.
                void updateStream()
                {
                    static LLCachedControl<bool> sRadio(gSavedSettings, SETTING_STREAM, false);
                    uint32_t pid = 0;
                    if (mListening && sRadio && gAudiop)
                    {
                        auto* media = dynamic_cast<LLStreamingAudio_MediaPlugins*>(gAudiop->getStreamingAudioImpl());
                        if (media && media->isPlaying() == 1)
                        {
                            pid = (uint32_t)media->getPluginProcessID();
                        }
                    }
                    if (pid == 0)
                    {
                        if (streamtap::pid() != 0)
                        {
                            streamtap::stop();
                        }
                        mClient.setStreamEnabled(false);
                        mStreamFailedPid = 0;
                        return;
                    }
                    if (streamtap::pid() == pid)
                    {
                        mClient.setStreamEnabled(true);
                        return;
                    }
                    if (mStreamFailedPid == pid)
                    {
                        return;   // it did not work for this player: not tried again every tenth of a second
                    }
                    std::string error;
                    if (streamtap::start(pid, error))
                    {
                        mStreamFailedPid = 0;
                        mClient.setStreamEnabled(true);
                        setStatus("radio_listening");
                    }
                    else
                    {
                        LL_WARNS("Subtitle") << "radio: " << error << LL_ENDL;
                        mStreamFailedPid = pid;
                        mClient.setStreamEnabled(false);
                        setStatus("radio_failed");
                    }
                }

                // Looks at the voice server's news about every participant and files it on the clock of
                // the subtitles (the sound heard so far). Cheap: a few participants, ten times a second.
                void sampleSpeakers()
                {
                    LLVoiceClient* voice = LLVoiceClient::getInstance();
                    if (!voice || !voice->voiceEnabled())
                    {
                        mClient.setVoiced(false);
                        return;
                    }
                    std::set<LLUUID> ids;
                    voice->getParticipantList(ids);
                    std::vector<SpeakerTracker::Voice> voices;
                    bool anyone = false;
                    for (const LLUUID& id : ids)
                    {
                        if (id == gAgentID)
                        {
                            continue;   // our own voice is not in what we hear
                        }
                        SpeakerTracker::Voice v;
                        v.id       = id.asString();
                        v.speaking = voice->getIsSpeaking(id);
                        v.power    = voice->getCurrentPower(id);
                        anyone |= v.speaking;
                        voices.push_back(std::move(v));
                    }
                    const double now = mClient.sentSeconds();   // voice and radio together: the helper's clock
                    if (now < mSampleClock)
                    {
                        // The clock went back: the writer thread started the tap (and set the clock to 0)
                        // after the first looks were filed on the last session's clock. Those would make
                        // add() refuse every newer look, so they go.
                        mTracker.clear();
                    }
                    mSampleClock = now;
                    mTracker.add(now, voices);
                    mTracker.forget(now - HISTORY);
                    mClient.setVoiced(anyone);
                }

                // Returns true when the log changed.
                bool handle(const Event& e)
                {
                    switch (e.type)
                    {
                        case Event::Type::Loading:
                            setStatus("loading");
                            return false;
                        case Event::Type::Ready:
                            mListening = true;   // the tap starts now: the clock of the subtitles starts with it
                            mSampleTimer.reset();
                            setStatus("ready");
                            if (gSavedSettings.getBOOL(SETTING_TRANSLATE) && !localTranslation() && !LLTranslate::isTranslationConfigured())
                            {
                                setStatus("translate_unconfigured");
                            }
                            return false;
                        case Event::Type::Text:
                        {
                            const size_t keep = (size_t)llclamp((int)gSavedSettings.getS32(SETTING_LINES), 1, 100);
                            Line line;
                            if (e.source == Event::Source::Stream)
                            {
                                line.radio = true;   // nobody speaks on the radio to be named
                                line.station = stationName();
                            }
                            const SpeakerTracker::Attribution who = line.radio
                                ? SpeakerTracker::Attribution()
                                : mTracker.attribute(e.t0, e.t1, (double)gSavedSettings.getF32(SETTING_LATENCY), MARGIN);
                            if (!who.id.empty())
                            {
                                line.id.set(who.id);
                                line.others = who.others;
                                LLAvatarNameCache::get(line.id, [](const LLUUID&, const LLAvatarName&) {});   // have the name ready
                            }
                            line.serial = mNextSerial++;
                            line.lang = e.lang;
                            line.text = e.text;
                            translate(line);
                            mLines.push_back(std::move(line));
                            while (mLines.size() > keep)
                            {
                                mLines.pop_front();
                            }
                            return true;
                        }
                        case Event::Type::Dropped:
                        {
                            mDroppedSeconds += e.seconds;
                            LLStringUtil::format_map_t args;
                            args["[SECONDS]"] = llformat("%.0f", mDroppedSeconds);
                            std::string msg = text("dropped");
                            LLStringUtil::format(msg, args);
                            mStatus->setText(msg);
                            return false;
                        }
                        case Event::Type::Error:
                            LL_WARNS("Subtitle") << e.message << LL_ENDL;
                            setStatus("error");
                            return false;
                        case Event::Type::Exited:
                            mListening = false;
                            setStatus("exited");   // after our own stop() the queue is emptied, so this is a crash
                            return false;
                    }
                    return false;
                }

                std::string speakerName(const Line& line)
                {
                    if (line.radio)
                    {
                        return line.station.empty() ? text("radio_label") : text("radio_label") + " " + line.station;
                    }
                    if (line.id.isNull())
                    {
                        return text("unknown_speaker");
                    }
                    LLAvatarName name;
                    if (!LLAvatarNameCache::get(line.id, &name))
                    {
                        mUnresolved = true;
                        return text("unknown_speaker");
                    }
                    // RLVa @shownames: a nearby speaker's anonym (plus/rlv, research/248 3.4)
                    return plus::rlv::enabled() ? plus::rlv::nearbyAvatarNameForPlus(line.id, name.getDisplayName())
                                                : name.getDisplayName();
                }

                void refreshLog()
                {
                    mUnresolved = false;
                    std::vector<std::string> parts;
                    parts.reserve(mLines.size());
                    for (const Line& line : mLines)
                    {
                        std::string part = speakerName(line);
                        if (line.others)
                        {
                            part += text("and_others");
                        }
                        if (!line.lang.empty())
                        {
                            part += " [" + line.lang + "]";
                        }
                        part += ": " + line.text;
                        if (!line.translation.empty())
                        {
                            part += "\n    " + text("translated_mark") + " " + line.translation;
                        }
                        if (plus::rlv::enabled())
                        {
                            // RLVa @shownames / @showloc: the names and places said aloud too, as in chat
                            plus::rlv::filterTextForPlus(part);
                        }
                        parts.push_back(std::move(part));
                    }

                    // The field cuts what goes past its max_length at the end, which is where the newest
                    // line is: the oldest lines are left out instead, so that the newest ones always show.
                    const S32 max_bytes = mLog->getMaxTextLength();
                    const size_t limit = max_bytes > 0 ? static_cast<size_t>(max_bytes) : std::string::npos;
                    size_t first = parts.size();
                    size_t bytes = 0;
                    while (first > 0)
                    {
                        const size_t add = parts[first - 1].size() + (bytes ? 1 : 0);
                        if (bytes + add > limit)
                        {
                            break;
                        }
                        bytes += add;
                        --first;
                    }
                    std::string all;
                    if (first == parts.size() && !parts.empty())
                    {
                        all = utf8str_truncate(parts.back(), max_bytes);   // the newest line alone is too long
                    }
                    for (size_t i = first; i < parts.size(); ++i)
                    {
                        if (!all.empty())
                        {
                            all += "\n";
                        }
                        all += parts[i];
                    }
                    mLog->setText(all);
                    mLog->endOfDoc();
                }

                Client          mClient;
                ModelDownload   mDownload;
                TranslateModelFetch& mFetch = sharedTranslateFetch();   // shared with the translation settings window
                TranslateClient mTranslator;       // the local translation helper (started on the first sentence)
                SpeakerTracker  mTracker;
                LLTextEditor*   mLog    = nullptr;
                LLTextBox*      mStatus = nullptr;
                LLButton*       mDownloadButton = nullptr;
                LLButton*       mFetchButton = nullptr;
                std::string     mFetchTip;         // the button's tool tip from the XML; the credits go after it
                std::string     mFetchTipTarget = "-";
                std::deque<Line> mLines;
                LLFrameTimer    mSampleTimer;
                LLFrameTimer    mNameTimer;
                LLFrameTimer    mDownloadTimer;
                LLFrameTimer    mFetchTimer;
                double          mDroppedSeconds = 0.0;
                uint64_t        mNextSerial = 1;
                uint32_t        mStreamFailedPid = 0;     // the stream player whose capture could not be started
                bool            mWatchDownload = false;   // a download is being followed in draw()
                int             mLocalStarts = 0;         // how many times the local helper was started since the window opened
                bool            mLocalNoticeShown = false;   // the status already said what is wrong with the local translation
                bool            mWatchFetch = false;      // the translation models are being fetched
                bool            mUnpackingShown = false;  // the status already says they are being unpacked
                bool            mVerifyingShown = false;  // the status already says the download is being checked
                double          mSampleClock = 0.0;       // the subtitles' clock at the last sampleSpeakers()
                bool            mListening  = false;   // the helper is ready and the tap is running
                bool            mUnresolved = false;   // some name in the log was not known yet
            };

            void init()
            {
                LLFloaterReg::add(FLOATER_NAME, "floater_plus_subtitle.xml",
                                  (LLFloaterBuildFunc)&LLFloaterReg::build<SubtitleFloater>);
            }

            plus::FeatureRegistration sRegistration({ "subtitle", &init, {} });
        }
    }
}
