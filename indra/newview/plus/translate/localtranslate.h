/**
 * @file localtranslate.h
 * @brief Translation (plus/translate, research/228): "this PC" as a translation service of the chat translation.
 *
 * LLTranslate (the official chat translation) is told by the setting TranslationService which service to use. The value
 * "local" makes it hand the text to translateLocal() here: the text is cut around addresses (textsplit.h), the language is
 * found out when the caller gave none (langdetect.h), and each piece goes to the local helper charis-translate.exe
 * (plus/subtitle/translateclient.h) with the models that were fetched. Nothing is sent anywhere. Nearby chat, instant
 * messages, group chat and conferences all come through LLTranslate::translateMessage(), so they all get it.
 *
 * The helper is started by the first sentence and stopped when nothing was translated for five minutes (it starts again by the
 * next sentence). A line that cannot be translated (a language no model reads, a model not fetched yet, no helper) comes back
 * as a success with an empty translation: the chat shows the line as it is, without an error note. Which way each line went
 * (and why it was left as it is) goes to the log, a kind of line at most once in 30 seconds (see noteChatLine()).
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

#ifndef PLUS_TRANSLATE_LOCALTRANSLATE_H
#define PLUS_TRANSLATE_LOCALTRANSLATE_H

#include "lltranslate.h"

#include <string>

namespace plus
{
    namespace translate
    {
        // The setting TranslationService is "local" (translate on this PC).
        bool isLocalService();

        // Models for translating into `to` are installed (at least one direction). For LLTranslate::isTranslationConfigured().
        bool localReady(const std::string& to);

        // Translates `text` (from_lang may be empty: found out here) into `to`. The answer comes to success() on the main thread,
        // later; an empty translation means "left as it is". failure() is for a real error only.
        void translateLocal(const std::string& from_lang, const std::string& to, const std::string& text,
                            LLTranslate::TranslationSuccess_fn success, LLTranslate::TranslationFailure_fn failure);

        // For a translation into Japanese the success function is wrapped so that the dialect chosen in the settings
        // (PlusTranslateDialect) is written; for anything else it is returned as it is. Used for every service.
        LLTranslate::TranslationSuccess_fn withDialect(const std::string& to, LLTranslate::TranslationSuccess_fn success);

        // What last kept the local translation from working ("" when nothing): for the settings window.
        std::string localProblem();

        // The log (tag "Translate"; languages, counts and times only, never chat text or names):
        // For every nearby chat line (LLTranslate::logCharsSeen()): the state of the chat translation (TranslateChat,
        // TranslationService, TranslateLanguage and the language it stands for, whether a model is installed for it) is written
        // when it is not the one written last, so the log tells whether a line was meant to be translated at all.
        void noteChatLine();
        // One line with that state. when: where it comes from ("nearby chat", "settings saved"); target: the language
        // `language` stands for (two letters).
        void logState(const char* when, bool chat, const std::string& service, const std::string& language, const std::string& target);
    }
}

#endif // PLUS_TRANSLATE_LOCALTRANSLATE_H
