/**
 * @file rmlsystem.cpp
 * @brief RmlUi's start and end, its system and file interfaces, and the "RmlUi:" log line (research/364).
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

#include "rmlsystem.h"

#include "rmlfonts.h"
#include "rmlhost.h"
#include "rmlime.h"
#include "rmlrender.h"

#include "plus.h"
#include "plus/safemode/safemode.h"

#include "llapp.h"
#include "llcallbacklist.h"
#include "llclipboard.h"
#include "lldir.h"
#include "llfile.h"
#include "llrhi.h"
#include "lltimer.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace plus::rmlui
{
    namespace
    {
        constexpr U32 MAX_LOG_MESSAGES = 100;   // RmlUi's warnings in the log per session
        constexpr size_t MAX_LOG_CHARS = 300;
        constexpr size_t MAX_KEY_CHARS = 64;    // a "[[key]]"'s key

        // Where RmlUi is (research/364 6, sState).
        enum class State
        {
            Uninit,         // not started, or shut down
            Ready,          // started; hosts hold it
            Failed,         // the start failed: not tried again this session
            ReleasePending, // no host: shut down at sReleaseAt
        };

        bool plainChar(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        }

        class System final : public Rml::SystemInterface
        {
        public:
            double GetElapsedTime() override
            {
                return LLTimer::getTotalSeconds() - mStart;
            }

            // "[[key]]" in a text node: the floater's <string name="key"> (LLRmlView::translate). Called for the text
            // of the document when it is made, and again for a data view's text with its values in.
            int TranslateString(Rml::String& translated, const Rml::String& input) override
            {
                translated = input;
                LLRmlView* host = LLRmlView::current();
                if (!host || input.find("[[") == Rml::String::npos)
                {
                    return 0;
                }
                Rml::String out;
                out.reserve(input.size());
                int count = 0;
                size_t from = 0;
                for (size_t open = input.find("[[", from); open != Rml::String::npos; open = input.find("[[", from))
                {
                    const size_t close = input.find("]]", open + 2);
                    if (close == Rml::String::npos)
                    {
                        break;
                    }
                    out.append(input, from, open - from);
                    const std::string key = input.substr(open + 2, close - open - 2);
                    std::string value;
                    if (isPlainName(key, MAX_KEY_CHARS) && host->translate(key, value))
                    {
                        out += value;
                        ++count;
                    }
                    else
                    {
                        out.append(input, open, close + 2 - open);
                    }
                    from = close + 2;
                }
                out.append(input, from, Rml::String::npos);
                translated.swap(out);
                return count;
            }

            // Style sheets and documents are found next to the document naming them; a UI image source stays as it is.
            void JoinPath(Rml::String& translated_path, const Rml::String& document_path, const Rml::String& path) override
            {
                if (path.compare(0, sizeof(UI_IMAGE_PREFIX) - 1, UI_IMAGE_PREFIX) == 0)
                {
                    translated_path = path;
                    return;
                }
                const size_t slash = document_path.find_last_of('/');
                translated_path = (slash == Rml::String::npos ? Rml::String() : document_path.substr(0, slash + 1)) + path;
            }

            bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
            {
                switch (type)
                {
                case Rml::Log::LT_ALWAYS:
                case Rml::Log::LT_ERROR:
                case Rml::Log::LT_ASSERT:
                case Rml::Log::LT_WARNING:
                    break;
                default:
                    return true;
                }
                if (sLogged >= MAX_LOG_MESSAGES)
                {
                    return true;
                }
                if (++sLogged == MAX_LOG_MESSAGES)
                {
                    LL_WARNS("RmlUi") << "RmlUi: further messages of RmlUi are not logged" << LL_ENDL;
                }
                else
                {
                    LL_WARNS("RmlUi") << "RmlUi: " << message.substr(0, MAX_LOG_CHARS) << LL_ENDL;
                }
                return true;    // false would break into a debugger
            }

            void SetMouseCursor(const Rml::String& cursor_name) override
            {
                if (LLRmlView* host = LLRmlView::current())
                {
                    host->setCursorName(cursor_name);
                }
            }

            void SetClipboardText(const Rml::String& text) override
            {
                // RmlUi's copy and cut hand over a password field's real value (WidgetTextInput::CopySelection).
                LLRmlView* host = LLRmlView::current();
                if (host && host->passwordFocused())
                {
                    LL_WARNS_ONCE("RmlUi") << "RmlUi: a copy out of a password field was refused" << LL_ENDL;
                    return;
                }
                const LLWString wide = utf8str_to_wstring(text);
                LLClipboard::instance().copyToClipboard(wide, 0, (S32)wide.length());
            }

            void GetClipboardText(Rml::String& text) override
            {
                LLWString wide;
                text = LLClipboard::instance().pasteFromClipboard(wide) ? wstring_to_utf8str(wide) : Rml::String();
            }

            void ActivateKeyboard(Rml::Vector2f caret_position, float line_height) override
            {
                RmlPreeditor::instance().setCaret(LLRmlView::current(), caret_position, line_height);
            }

        private:
            F64 mStart = LLTimer::getTotalSeconds();
            static U32 sLogged;
        };
        U32 System::sLogged = 0;

        // Files under app_settings/plus/rmlui/ only, read whole when opened (they are small: MAX_FILE_BYTES).
        class Files final : public Rml::FileInterface
        {
        public:
            Rml::FileHandle Open(const Rml::String& path) override
            {
                if (!isDocumentPath(path))
                {
                    LL_WARNS_ONCE("RmlUi") << "RmlUi: a file outside app_settings/" << DOCUMENT_FOLDER << " was refused" << LL_ENDL;
                    return 0;
                }
                const std::string full = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, DOCUMENT_FOLDER, path);
                const S64 size = LLFile::size(full);
                if (size < 0 || size > MAX_FILE_BYTES)
                {
                    LL_WARNS("RmlUi") << "RmlUi: " << path << (size < 0 ? " is not there" : " is too large") << LL_ENDL;
                    return 0;
                }
                OpenFile* file = new OpenFile;
                file->data = LLFile::getContents(full);
                return reinterpret_cast<Rml::FileHandle>(file);
            }

            void Close(Rml::FileHandle handle) override
            {
                delete reinterpret_cast<OpenFile*>(handle);
            }

            size_t Read(void* buffer, size_t size, Rml::FileHandle handle) override
            {
                OpenFile* file = reinterpret_cast<OpenFile*>(handle);
                if (!file || !buffer)
                {
                    return 0;
                }
                const size_t count = std::min(size, file->data.size() - file->position);
                std::memcpy(buffer, file->data.data() + file->position, count);
                file->position += count;
                return count;
            }

            bool Seek(Rml::FileHandle handle, long offset, int origin) override
            {
                OpenFile* file = reinterpret_cast<OpenFile*>(handle);
                if (!file)
                {
                    return false;
                }
                const S64 base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? (S64)file->position : (S64)file->data.size();
                const S64 position = base + (S64)offset;
                if (position < 0 || position > (S64)file->data.size())
                {
                    return false;
                }
                file->position = (size_t)position;
                return true;
            }

            size_t Tell(Rml::FileHandle handle) override
            {
                const OpenFile* file = reinterpret_cast<const OpenFile*>(handle);
                return file ? file->position : 0;
            }

            size_t Length(Rml::FileHandle handle) override
            {
                const OpenFile* file = reinterpret_cast<const OpenFile*>(handle);
                return file ? file->data.size() : 0;
            }

        private:
            struct OpenFile
            {
                std::string data;
                size_t position = 0;
            };
        };

        // Made at the start and deleted after Rml::Shutdown (RmlUi uses them until then). Not static objects: a
        // renderer left to static destruction would free GL resources after shutdownGL.
        struct Interfaces
        {
            Renderer renderer;
            System system;
            Files files;
        };

        State sState = State::Uninit;
        S32 sHostCount = 0;
        F64 sReleaseAt = 0.0;       // LLTimer::getTotalSeconds(); 0: no release pending
        Interfaces* sInterfaces = nullptr;
        Stats sStats;
        F64 sStatsUntil = 0.0;      // the end of the counting period; 0: not counting for a line

        bool quitting()
        {
            return !LLApp::isRunning() || LLRHI::sQuitting.load();
        }

        void clearInterfaces()
        {
            Rml::SetRenderInterface(nullptr);
            Rml::SetSystemInterface(nullptr);
            Rml::SetFileInterface(nullptr);
            Rml::SetTextInputHandler(nullptr);
            delete sInterfaces;
            sInterfaces = nullptr;
        }

        void shutdown()
        {
            if (!sInterfaces)
            {
                return;
            }
            Rml::Shutdown();    // contexts (none left), then every geometry and texture through the renderer
            clearInterfaces();
            sState = State::Uninit;
            sReleaseAt = 0.0;
            LL_INFOS("RmlUi") << "RmlUi shut down" << LL_ENDL;
        }

        void releaseTick(void*)
        {
            if (sState != State::ReleasePending)
            {
                gIdleCallbacks.deleteFunction(&releaseTick);
                return;
            }
            if (quitting() || LLTimer::getTotalSeconds() >= sReleaseAt)
            {
                gIdleCallbacks.deleteFunction(&releaseTick);
                shutdown();
            }
        }

        bool initialise(StartTimes& times)
        {
            const F64 start = LLTimer::getTotalSeconds();
            sInterfaces = new Interfaces;
            Rml::SetRenderInterface(&sInterfaces->renderer);
            Rml::SetSystemInterface(&sInterfaces->system);
            Rml::SetFileInterface(&sInterfaces->files);
            Rml::SetTextInputHandler(&RmlPreeditor::instance());
            if (!Rml::Initialise())
            {
                LL_WARNS("RmlUi") << "RmlUi could not be started" << LL_ENDL;
                clearInterfaces();
                return false;
            }
            const F64 fonts_start = LLTimer::getTotalSeconds();
            times.init_ms = (fonts_start - start) * 1000.0;
            times.font_faces = fonts::registerFonts();
            times.fonts_ms = (LLTimer::getTotalSeconds() - fonts_start) * 1000.0;
            if (times.font_faces <= 0)
            {
                LL_WARNS("RmlUi") << "RmlUi: no font could be given to it: not used" << LL_ENDL;
                shutdown();
                return false;
            }
            LL_INFOS("RmlUi") << "RmlUi " << Rml::GetVersion() << " started (" << times.font_faces << " font faces)" << LL_ENDL;
            return true;
        }
    }

    Start acquire(StartTimes& times)
    {
        if (plus::safemode::active())
        {
            return Start::SafeMode;
        }
        if (sState == State::Failed)
        {
            return Start::Failed;
        }
        if (sState == State::ReleasePending)
        {
            gIdleCallbacks.deleteFunction(&releaseTick);
            sReleaseAt = 0.0;
            sState = State::Ready;
        }
        if (sState == State::Uninit)
        {
            if (!initialise(times))
            {
                sState = State::Failed;
                return Start::Failed;
            }
            sState = State::Ready;
        }
        ++sHostCount;
        return Start::Ready;
    }

    void release()
    {
        llassert(sHostCount > 0);
        if (sHostCount <= 0)
        {
            return;
        }
        if (--sHostCount > 0 || sState != State::Ready)
        {
            return;
        }
        if (quitting())
        {
            shutdown();     // shutdownViews: before shutdownGL
            return;
        }
        sReleaseAt = LLTimer::getTotalSeconds() + RELEASE_DELAY_SECONDS;
        sState = State::ReleasePending;
        gIdleCallbacks.addFunction(&releaseTick);
    }

    Renderer& renderer()
    {
        llassert(sInterfaces);
        return sInterfaces->renderer;
    }

    bool isDocumentPath(const std::string& path)
    {
        if (path.empty() || path.size() > MAX_PATH_CHARS)
        {
            return false;
        }
        size_t start = 0;
        while (true)
        {
            const size_t slash = path.find('/', start);
            const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
            if (part.empty() || part == "." || part == ".." || plus::isWindowsDeviceName(part) ||
                !std::all_of(part.begin(), part.end(), [](char c) { return plainChar(c) || c == '.' || c == '-'; }))
            {
                return false;   // also an empty part: a leading, trailing or doubled '/'
            }
            if (slash == std::string::npos)
            {
                return true;
            }
            start = slash + 1;
        }
    }

    bool isPlainName(const std::string& name, size_t max_chars)
    {
        return !name.empty() && name.size() <= max_chars && std::all_of(name.begin(), name.end(), plainChar);
    }

    Stats& stats()
    {
        return sStats;
    }

    bool statsOn()
    {
        return LLRHI::sPeriodicLogs || LLRHI::sFrameCost;
    }

    void logStatsIfDue(F64 now_seconds)
    {
        if (!statsOn())
        {
            sStatsUntil = 0.0;
            return;
        }
        if (sStatsUntil == 0.0)
        {
            sStats = Stats();   // a period starts with the line on: every frame of it is timed
            sStatsUntil = now_seconds + STATS_PERIOD_SECONDS;
            return;
        }
        if (now_seconds < sStatsUntil || !sInterfaces)
        {
            return;
        }
        const Stats& s = sStats;
        const Renderer& render = sInterfaces->renderer;
        std::ostringstream times;
        if (s.draws && s.timed_draws == s.draws)
        {
            times << "update " << s.update_ms / s.draws << " ms/frame, render " << s.render_ms / s.draws << " ms/frame";
        }
        else
        {
            times << "times not measured (" << s.timed_draws << " of " << s.draws << " frames)";
        }
        LL_INFOS("RmlUi") << "RmlUi: contexts " << Rml::GetNumContexts() << ", hosts " << sHostCount << ", frames " << s.draws
                          << " (updates " << s.updates << ", renders " << s.renders << ", replays " << s.replays << "), "
                          << times.str() << ", RenderGeometry " << s.render_geometry << ", drawRange " << s.runs
                          << ", run builds " << s.run_builds << " (packed runs copied " << s.run_packs << ", reused "
                          << s.run_reuses << "), compiles " << s.compiles << " (" << s.compile_bytes / 1024
                          << " KB), geometries " << render.geometryCount() << ", textures " << render.textureCount() << " ("
                          << render.textureBytes() / 1024 << " KB)" << LL_ENDL;
        sStats = Stats();
        sStatsUntil = now_seconds + STATS_PERIOD_SECONDS;
    }
}
