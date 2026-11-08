/**
 * @file posermcp.cpp
 * @brief The viewer's local MCP endpoint for AI assistants (Claude Code etc.):
 *        posing avatars through the Poser (plus/posermcp) and searching the
 *        inventory (plus/invfinder), reading and changing the viewer's
 *        settings (settingstools.h), and the development test scene
 *        (plus/testscene), as tools of one server.
 *        Registers with the plus foundation; owns the server's life cycle.
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

// Off by default. "Plus > MCP server (for AI assistants)" toggles
// PlusPoserMcpEnabled; the server then listens on
// http://127.0.0.1:<PlusPoserMcpPort>/mcp and writes
// user_settings/plus_poser_mcp.json with the URL and the bearer token. The
// token is kept across restarts (PlusPoserMcpKeepToken) so the client
// registration stays valid; "New MCP token" replaces it. One server, one port
// and one token for every tool set (the settings keep their Poser names so
// that existing registrations and saved settings stay valid). Whoever holds
// the token can pose the avatar AND read the names and folders of the
// inventory.
//
// Threads: the HTTP server's connection threads call the tool handlers; every
// handler queues its viewer work here and waits. The queue is drained on the
// main thread from an idle callback, in order, one job at a time (a job may
// ask to be called again next frame, e.g. a snapshot waiting for the pose to
// settle). Stopping fails all queued jobs first, so no connection thread is
// left waiting when the server joins them.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "debugtools.h"
#include "mcphttpserver.h"
#include "posehistorytools.h"
#include "posertools.h"
#include "settingstools.h"

#include "invfinder/invsource.h"
#include "invfinder/invtools.h"
#include "testscene/testscene.h"
#include "testscene/testscenetools.h"

#include "llcallbacklist.h"
#include "llevents.h"
#include "llfile.h"
#include "llnotificationsutil.h"
#include "llthread.h"
#include "lltrans.h"
#include "lltimer.h"
#include "lluictrl.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"

#if LL_WINDOWS
#include "llwin32headers.h"
#include <sddl.h>
#endif

#include <chrono>
#include <deque>
#include <future>
#include <vector>

namespace json = boost::json;
using plus::mcp::ToolResult;
using plus::posermcp::JobProgress;
using plus::posermcp::MainThreadStep;

namespace
{
    constexpr char SETTING_ENABLED[]    = "PlusPoserMcpEnabled";
    constexpr char SETTING_PORT[]       = "PlusPoserMcpPort";
    constexpr char SETTING_KEEP_TOKEN[] = "PlusPoserMcpKeepToken";
    constexpr char CONNECTION_FILE[]    = "plus_poser_mcp.json"; // in user_settings
    constexpr char ENDPOINT_PATH[]      = "/mcp";
    constexpr char SERVER_NAME[]        = "sl-viewer";
    constexpr int  JOBS_PER_FRAME       = 16;

    // ---- main-thread job queue ----

    struct Job
    {
        MainThreadStep                  step;
        std::promise<ToolResult>        promise;
        std::atomic<bool>               abandoned{ false }; // the caller gave up waiting
        unsigned                        calls         = 0;
        F64                             firstCallTime = 0.0;
    };

    std::mutex                        sJobsMutex;
    std::deque<std::shared_ptr<Job>>  sJobs;
    bool                              sAcceptingJobs = false; // guarded by sJobsMutex

    // Connection threads: hand a step to the main thread and wait for it.
    ToolResult runOnMainThread(MainThreadStep step, double timeout_seconds)
    {
        auto job  = std::make_shared<Job>();
        job->step = std::move(step);
        std::future<ToolResult> result = job->promise.get_future();
        {
            std::lock_guard<std::mutex> lock(sJobsMutex);
            if (!sAcceptingJobs)
                return ToolResult::error("The MCP server is stopping.");
            sJobs.push_back(job);
        }

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout_seconds);
        while (result.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                job->abandoned = true;
                if (result.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                    break; // finished just now after all
                return ToolResult::error("Timed out waiting for the viewer's main thread (is it busy, or still logging in?).");
            }
        }
        return result.get();
    }

    // Main thread, every frame.
    void processJobs()
    {
        for (int count = 0; count < JOBS_PER_FRAME; ++count)
        {
            std::shared_ptr<Job> job;
            {
                std::lock_guard<std::mutex> lock(sJobsMutex);
                if (sJobs.empty())
                    return;
                job = sJobs.front();
            }

            std::optional<ToolResult> result;
            if (job->abandoned)
            {
                result = ToolResult::error("abandoned"); // nobody waits for it any more
            }
            else
            {
                const F64 now = LLTimer::getTotalSeconds();
                if (job->calls == 0)
                    job->firstCallTime = now;

                JobProgress progress;
                progress.calls        = job->calls++;
                progress.firstCallAge = now - job->firstCallTime;
                try
                {
                    result = job->step(progress);
                }
                catch (const std::exception& e)
                {
                    result = ToolResult::error(e.what());
                }
                catch (...)
                {
                    result = ToolResult::error("The tool failed inside the viewer.");
                }
            }

            if (!result)
                return; // the job wants the next frame; keep the order of jobs

            {
                std::lock_guard<std::mutex> lock(sJobsMutex);
                if (!sJobs.empty() && sJobs.front() == job)
                    sJobs.pop_front();
            }
            job->promise.set_value(std::move(*result));
        }
    }

    // ---- server life cycle ----

    std::unique_ptr<plus::mcp::HttpServer> sServer;
    std::string                            sToken;
    bool                                   sChangingSetting = false;

    std::string connectionFilePath()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, CONNECTION_FILE);
    }

    std::string endpointUrl(U32 port)
    {
        return "http://127.0.0.1:" + std::to_string(port) + ENDPOINT_PATH;
    }

    bool isUsableToken(const std::string& token)
    {
        return token.size() >= 32 && token.size() <= 256 &&
               std::all_of(token.begin(), token.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
    }

    std::string readSavedToken()
    {
        llifstream file(connectionFilePath().c_str());
        if (!file.is_open())
            return {};
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        boost::system::error_code ec;
        json::value               value = json::parse(text, ec);
        if (ec || !value.is_object())
            return {};
        const json::value* token = value.get_object().if_contains("token");
        if (!token || !token->is_string())
            return {};
        std::string result(token->get_string());
        return isUsableToken(result) ? result : std::string();
    }

    // The connection file holds the token: it is made readable by this OS user only (security audit A M3;
    // a protected access list with one entry, nothing inherited from the folder, so not the administrators
    // or other accounts either). The file is created here, empty, before the token is written into it:
    // opening it again for writing keeps its access list, and so does the move over the old file.
    // Failing only leaves the folder's access list (the user's profile), as before, with a warning.
    void createPrivateFile(const std::string& path)
    {
#if LL_WINDOWS
        LLFile::remove(path, ENOENT); // the access list is given only to a new file
        bool   made  = false;
        HANDLE token = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        {
            DWORD size = 0;
            GetTokenInformation(token, TokenUser, nullptr, 0, &size);
            std::vector<BYTE> user(size);
            LPWSTR            sid = nullptr;
            if (size && GetTokenInformation(token, TokenUser, user.data(), size, &size) &&
                ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid))
            {
                const std::wstring   sddl = L"D:P(A;;FA;;;" + std::wstring(sid) + L")";
                PSECURITY_DESCRIPTOR sd   = nullptr;
                if (ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
                {
                    SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };
                    HANDLE file = CreateFileW(ll_convert<std::wstring>(path).c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW,
                                              FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (file != INVALID_HANDLE_VALUE)
                    {
                        CloseHandle(file);
                        made = true;
                    }
                    LocalFree(sd);
                }
                LocalFree(sid);
            }
            CloseHandle(token);
        }
        if (!made)
            LL_WARNS("PlusPoserMcp") << "Could not limit " << path << " to this OS user; it keeps the folder's access" << LL_ENDL;
#endif
    }

    bool writeConnectionFile(U32 port, const std::string& token)
    {
        json::object info;
        info["url"]       = endpointUrl(port);
        info["port"]      = port;
        info["token"]     = token;
        info["transport"] = "streamable-http";
        info["header"]    = "Authorization: Bearer <token>";
        info["note"]      = "Written by the viewer (Plus > MCP Server). Keep the token private. The token allows posing the avatar, searching the inventory, changing the viewer's display and debug settings and flipping its render switches.";

        // Written next to the file, then moved over it: a client never reads a half-written file. The
        // temporary name is per process: two viewers of this OS user may write at once.
        const std::string path = connectionFilePath();
        const std::string temp = plus::tempPathFor(path);
        createPrivateFile(temp);   // made private first; the stream below keeps its access
        // A client reading the old file holds it open for a moment (Windows
        // refuses the replace then): a few short retries before giving up.
        constexpr int REPLACE_ATTEMPTS = 3;
        const plus::FileWrite wrote = plus::writeFileReplacing(path, [&](std::ostream& file) { file << json::serialize(info) << "\n"; },
                                                               std::ios::out | std::ios::trunc, REPLACE_ATTEMPTS);
        if (wrote == plus::FileWrite::cannot_open)
        {
            LLFile::remove(temp, ENOENT);   // the empty file createPrivateFile() made (replaceFile() leaves it)
        }
        return wrote == plus::FileWrite::written;
    }

    // ---- "a viewer of this OS user holds this port" ----
    // The connection file is shared by every viewer of the OS user (user_settings),
    // so a viewer that cannot get the port must know whether the port's holder is
    // one of them (a second viewer of the same PC, logged in to another account:
    // the port is the same setting) before it gives the token up (startServer()).
    // While its server holds a port, a viewer keeps plus_poser_mcp.<port>.serving
    // (beside the connection file) open, shared with any other; a viewer asking
    // opens it unshared, which fails only while somebody has it open. Per port: a
    // viewer of ours on another port says nothing about who holds this one.
    // Other OS users cannot open it (their profile is not theirs to read).
    // Elsewhere than Windows nobody is ever found: a failed start gives the
    // token up as before.
#if LL_WINDOWS
    HANDLE sServing = INVALID_HANDLE_VALUE;

    std::wstring servingFilePath(U32 port)
    {
        return ll_convert<std::wstring>(
            gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, llformat("plus_poser_mcp.%u.serving", port)));
    }
#endif

    // From before the port is bound until the server stops. Failing only leaves
    // this viewer unseen by the others.
    void markServing(U32 port)
    {
#if LL_WINDOWS
        if (sServing != INVALID_HANDLE_VALUE)
            return;
        const std::wstring path = servingFilePath(port);
        // A viewer asking (servedElsewhere()) holds it unshared for a moment: try again then.
        // Not FILE_SHARE_DELETE: nobody may remove it while it is held.
        DWORD error = 0;
        for (int tries = 0; tries < 3 && sServing == INVALID_HANDLE_VALUE; ++tries)
        {
            if (tries)
                Sleep(10);
            sServing = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            error = sServing == INVALID_HANDLE_VALUE ? GetLastError() : 0;
            if (error && error != ERROR_SHARING_VIOLATION)
                break;
        }
        if (sServing == INVALID_HANDLE_VALUE)
            LL_WARNS("PlusPoserMcp") << "Cannot open the serving mark of port " << port << " (" << error
                                     << "); other viewers will not see this one serving" << LL_ENDL;
#else
        (void)port;
#endif
    }

    void unmarkServing()
    {
#if LL_WINDOWS
        if (sServing != INVALID_HANDLE_VALUE)
        {
            CloseHandle(sServing);
            sServing = INVALID_HANDLE_VALUE;
        }
#endif
    }

    // Another viewer of this OS user holds the port now (asked while this one holds no mark).
    bool servedElsewhere(U32 port)
    {
#if LL_WINDOWS
        HANDLE probe = CreateFileW(servingFilePath(port).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (probe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(probe);
            return false;   // nobody has it open
        }
        return GetLastError() == ERROR_SHARING_VIOLATION;  // missing or unreadable: nobody known
#else
        (void)port;
        return false;
#endif
    }

    // A string of strings_plus.xml (en/ja) with its [ARGS] filled in.
    std::string text(const char* name, const LLStringUtil::format_map_t& args = {})
    {
        return LLTrans::getString(name, args);
    }

    void notify(const std::string& message)
    {
        LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", message));
    }

    void setEnabledSetting(bool enabled)
    {
        sChangingSetting = true;
        gSavedSettings.setBOOL(SETTING_ENABLED, enabled);
        sChangingSetting = false;
    }

    // quiet: no notifications or logging (static destruction at exit).
    void stopServer(bool quiet)
    {
        std::deque<std::shared_ptr<Job>> pending;
        {
            std::lock_guard<std::mutex> lock(sJobsMutex);
            sAcceptingJobs = false;
            pending.swap(sJobs);
        }
        for (auto& job : pending)
            job->promise.set_value(ToolResult::error("The MCP server was stopped."));

        if (sServer)
        {
            sServer->stop(); // joins the accept and connection threads
            sServer.reset();
            unmarkServing();
            plus::invfinder::InvSource::instance().stop();
            if (!quiet)
                LL_INFOS("PlusPoserMcp") << "MCP server stopped" << LL_ENDL;
        }
    }

    void startServer()
    {
        if (sServer)
            return;

        const U32 port = gSavedSettings.getU32(SETTING_PORT);
        if (port < 1024 || port > 65535)
        {
            notify(text("PlusMcpBadPort", { { "[PORT]", llformat("%u", port) }, { "[SETTING]", SETTING_PORT } }));
            setEnabledSetting(false);
            return;
        }

        // Without PlusPoserMcpKeepToken every start makes a new token, also when this
        // session's server was switched off and on again (sToken still has the old one).
        const bool keep_token = gSavedSettings.getBOOL(SETTING_KEEP_TOKEN);
        if (!keep_token)
            sToken.clear();
        if (sToken.empty() && keep_token)
            sToken = readSavedToken();
        if (sToken.empty())
            sToken = plus::mcp::generateToken();

        plus::mcp::ServerInfo info;
        info.name         = SERVER_NAME;
        info.version      = LLVersionInfo::instance().getVersion();
        info.instructions = plus::posermcp::serverInstructions() + "\n\n" + plus::invfinder::serverInstructions() + "\n\n" +
                            plus::posermcp::settingsInstructions() + "\n\n" + plus::posermcp::debugInstructions() + "\n\n" +
                            plus::testscene::mcpInstructions();

        std::vector<plus::mcp::Tool> tools = plus::posermcp::makePoserTools(&runOnMainThread);
        plus::posermcp::appendSettingsTools(tools, &runOnMainThread);
        plus::posermcp::appendDebugTools(tools, &runOnMainThread);
        plus::testscene::appendTools(tools, &runOnMainThread);
        // The inventory tools do all their work in one step, once.
        const plus::invfinder::MainThreadRunner runOnce = [](std::function<ToolResult()> step, double timeout_seconds)
        {
            return runOnMainThread([step = std::move(step)](const JobProgress&) -> std::optional<ToolResult> { return step(); },
                                   timeout_seconds);
        };
        for (plus::mcp::Tool& tool : plus::invfinder::makeInvTools(runOnce))
            tools.push_back(std::move(tool));
        auto dispatcher = std::make_shared<const plus::mcp::Dispatcher>(info, std::move(tools));

        plus::mcp::HttpConfig config;
        config.port  = static_cast<uint16_t>(port);
        config.token = sToken;
        config.path  = ENDPOINT_PATH;

        {
            std::lock_guard<std::mutex> lock(sJobsMutex);
            sAcceptingJobs = true;
        }

        // Marked before the port is bound: a viewer failing on it meanwhile finds this one.
        markServing(port);
        auto        server = std::make_unique<plus::mcp::HttpServer>(config, dispatcher);
        std::string error;
        if (!server->start(error))
        {
            {
                std::lock_guard<std::mutex> lock(sJobsMutex);
                sAcceptingJobs = false;
            }
            unmarkServing();    // before asking: this viewer's own mark must not answer
            LL_WARNS("PlusPoserMcp") << "Cannot start the MCP server: " << error << LL_ENDL;
            sToken.clear(); // this viewer's copy: read again from the file at the next start
            const std::string failed = text("PlusMcpStartFailed", { { "[ERROR]", error }, { "[SETTING]", SETTING_PORT } });
            if (!server->bindFailed())
            {
                // Winsock, a socket, listen() or a thread failed: nobody else got the port
                // from this, so the token in the file stays (kept for the next start).
                notify(failed);
                setEnabledSetting(false);
                return;
            }
            if (servedElsewhere(port))
            {
                // Another viewer of this OS user holds this port: the MCP client reaches a
                // viewer of ours, and the file is that viewer's (its URL and token stay).
                LL_INFOS("PlusPoserMcp") << "Another viewer of this OS user serves MCP on port " << port
                                         << "; the connection file is left as it is" << LL_ENDL;
                notify(failed + "\n" + text("PlusMcpServedElsewhere", { { "[PATH]", connectionFilePath() } }));
                setEnabledSetting(false);
                return;
            }
            // No viewer of this OS user holds the port: whoever does (another program,
            // another OS user of this PC) is sent the token by the MCP client, which does not
            // know the viewer is not there, and could use it on this viewer's server once it
            // starts with the kept token (127.0.0.1 is open to every OS user): that token is
            // given up. Not replaced with a new one here (a client reading the file would send
            // that one to the same program too): the file keeps no token, and the next start that
            // gets the port makes and writes a new one (PlusPoserMcpKeepToken finds none to keep).
            // Also when a viewer of ours serves on another port with this token: it keeps
            // accepting it (only "New MCP token" there replaces it), but no new client is
            // given it, and the next start here does not take it up again.
            const bool wrote = writeConnectionFile(port, std::string());
            notify(failed + "\n" + text(wrote ? "PlusMcpTokenDropped" : "PlusMcpTokenNotWritten", { { "[PATH]", connectionFilePath() } }));
            setEnabledSetting(false);
            return;
        }
        sServer = std::move(server);
        plus::invfinder::InvSource::instance().start();

        const bool wrote = writeConnectionFile(port, sToken);
        LL_INFOS("PlusPoserMcp") << "MCP server (poser, inventory) listening on " << endpointUrl(port) << LL_ENDL;
        notify(text("PlusMcpListening", { { "[URL]", endpointUrl(port) } }) + "\n"
               + text(wrote ? "PlusMcpConnectionFile" : "PlusMcpCannotWrite", { { "[PATH]", connectionFilePath() } }));
    }

    void onEnabledChanged(const LLSD& value)
    {
        if (sChangingSetting)
            return;
        if (value.asBoolean())
            startServer();
        else
        {
            stopServer(false);
            plus::posermcp::settingsServerStopped(true); // test values of settings_set go back
            notify(text("PlusMcpStopped"));
        }
    }

    void onPortChanged()
    {
        if (sServer)
        {
            stopServer(false);
            plus::posermcp::settingsServerStopped(true);
            startServer();
        }
    }

    void onNewToken()
    {
        const U32 port = gSavedSettings.getU32(SETTING_PORT);
        if (!sServer && servedElsewhere(port))
        {
            // The file is the other viewer's: a token written here would not be its,
            // and its MCP client could no longer reach it.
            notify(text("PlusMcpNewTokenElsewhere", { { "[PATH]", connectionFilePath() } }));
            return;
        }
        sToken = plus::mcp::generateToken();
        if (!sServer)
        {
            // Not written while the server is stopped (fix-round11 2): whoever holds the port
            // now (another program, another OS user of this PC: servedElsewhere() finds only
            // viewers of this OS user) would be sent it by an MCP client reading the file, as
            // startServer() says where it gives a token up. The next start that gets the port
            // writes it (sToken, with PlusPoserMcpKeepToken; a new one without). The file keeps
            // no token meanwhile: the old one is given up, and a later session does not take it
            // up again (PlusPoserMcpKeepToken reads the file).
            const bool wrote = writeConnectionFile(port, std::string());
            notify(text(wrote ? "PlusMcpNewTokenStopped" : "PlusMcpTokenNotWritten", { { "[PATH]", connectionFilePath() } }));
            return;
        }
        sServer->setToken(sToken);
        const bool wrote = writeConnectionFile(port, sToken);
        notify(text(wrote ? "PlusMcpNewToken" : "PlusMcpTokenNotWritten", { { "[PATH]", connectionFilePath() } }));
    }

    // Last resort at exit if the "LLApp" shutdown event was missed: stop the
    // threads before the statics they use go away. Declared after them.
    struct ServerGuard
    {
        ~ServerGuard() { stopServer(true); }
    } sServerGuard;

    void init()
    {
        doOnIdleRepeating(
            []()
            {
                processJobs();
                if (sServer)
                    plus::posermcp::historyIdle(); // edits in the poser window go into the pose history
                return false;
            });

        if (LLControlVariable* control = gSavedSettings.getControl(SETTING_ENABLED))
            control->getSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&) { onEnabledChanged(value); });
        if (LLControlVariable* control = gSavedSettings.getControl(SETTING_PORT))
            control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) { onPortChanged(); });

        LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.PoserMcp.NewToken",
                                                                 [](LLUICtrl*, const LLSD&) { onNewToken(); });

        // Stop (and join the threads) as soon as the viewer starts shutting down.
        LLEventPumps::instance().obtain("LLApp").listen("PlusPoserMcp",
                                                        [](const LLSD& status)
                                                        {
                                                            if (status["status"].asString() != "running")
                                                            {
                                                                stopServer(true);
                                                                // Forgotten, not set back: no rebuild while quitting, and
                                                                // test values are never saved (settingstools.h).
                                                                if (on_main_thread())
                                                                    plus::posermcp::settingsServerStopped(false);
                                                            }
                                                            return false;
                                                        });
    }

    void onUIReady()
    {
        // Settings are loaded and notifications can be shown from here on.
        if (gSavedSettings.getBOOL(SETTING_ENABLED))
            startServer();
    }

    plus::FeatureRegistration sRegistration({ "posermcp", &init, &onUIReady });
}
