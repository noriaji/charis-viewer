/**
 * @file fastexit.cpp
 * @brief Fast exit (research/276 stage 2): see fastexit.h.
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

#include "plus/fastexit/fastexit.h"

#include "llaudioengine.h"
#include "llviewercontrol.h"
#include "lltimer.h"
#include "llviewerwindow.h"
#include "llvoiceclient.h"
#include "llwindow.h"

#include <cstdlib>

#if LL_WINDOWS
extern bool gGPUBenchmarkMode;
#endif

#if LL_WINDOWS
#include "llwin32headers.h"
#endif

namespace plus::fastexit
{
namespace
{
    // State: sWanted is read once (-1 not read yet); sHidden is set by commit() and never cleared
    // (the exit has no way back after it). Both are written and read on the main thread only.
    int sWanted = -1;
    bool sHidden = false;
    U64 sStartMs = 0;       // when commit() ran (LLTimer total time, ms)
#if LL_WINDOWS
    // Named mutex owned by the closing process (commit). No user name in it; Local\ = this Windows session only.
    const wchar_t* const CLOSING_MUTEX = L"Local\\CharisViewerClosing";
    HANDLE sClosingMutex = nullptr;
#endif
}

bool wanted()
{
    if (sWanted < 0)
    {
        bool on = gSavedSettings.getBOOL("PlusFastExit");
        // LLRHI_FAST_EXIT: "0" off, "1" on, anything else (also empty): the setting (as the other LLRHI_ switches)
        if (const char* env = std::getenv("LLRHI_FAST_EXIT"))
        {
            if (env[0] == '0' && env[1] == '\0')
            {
                on = false;
            }
            else if (env[0] == '1' && env[1] == '\0')
            {
                on = true;
            }
        }
        sWanted = on ? 1 : 0;
    }
    return sWanted == 1;
}

void commit()
{
    if (sHidden)
    {
        return;
    }
    sHidden = true;
    if (gViewerWindow && gViewerWindow->getWindow())
    {
        gViewerWindow->getWindow()->hide();
    }
    // The sounds, the media and the voice are silenced by audio_update_volume (hidden() mutes them every
    // frame; the MuteAudio setting is not written, the next start would be muted). The stream stops here.
    // Silenced here once as well: an exit that does not run audio_update_volume again (a disconnect, the login screen,
    // WM_ENDSESSION) is silent too.
    if (gAudiop)
    {
        gAudiop->stopInternetStream();
        gAudiop->setMasterGain(0.f);
        gAudiop->setMuted(true);
    }
    if (LLVoiceClient::instanceExists())
    {
        LLVoiceClient::getInstance()->setVoiceVolume(0.f);
        LLVoiceClient::getInstance()->setMuteMic(true);
    }
    sStartMs = LLTimer::getTotalTime() / 1000;
    // A second viewer started now waits for this process to end (waitForClosingInstance).
#if LL_WINDOWS
    sClosingMutex = CreateMutexW(nullptr, TRUE, CLOSING_MUTEX);   // owned: abandoned when the process ends
    if (sClosingMutex && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // Another viewer is closing too (several Charis viewers at once): this one takes the mutex over when it
        // ends, so a new start waits for the last of them.
        CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD
        {
            WaitForSingleObject(sClosingMutex, INFINITE);   // the thread owns it from here, until the process ends
            return 0;
        }, nullptr, 0, nullptr));
    }
#endif
}

bool hidden()
{
    return sHidden;
}

bool overdue()
{
    // The logout of the official idleShutdown ends within 5 + 5 + 6 s; past this the exit is stuck behind
    // something that needs the window (a modal dialog, a disconnect, a teleport): save and end.
    constexpr U64 OVERDUE_MS = 25000;
    return sHidden && (LLTimer::getTotalTime() / 1000) - sStartMs > OVERDUE_MS;
}

void waitForClosingInstance()
{
#if LL_WINDOWS
    // The closing viewer owns the mutex until it ends (the system then marks it abandoned and the wait
    // returns). 10 s at most; no mutex: nothing to wait for.
    if (gGPUBenchmarkMode)
    {
        return;     // the benchmark's child process is started by a running viewer: it never waits
    }
    if (HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, CLOSING_MUTEX))
    {
        const DWORD r = WaitForSingleObject(m, 10000);
        if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED)
        {
            ReleaseMutex(m);    // taken only to know the other has ended: let go, so the next start does not wait
        }
        CloseHandle(m);
    }
#endif
}

void terminate()
{
#if LL_WINDOWS
    TerminateProcess(GetCurrentProcess(), 0);
#endif
    _exit(0);   // not reached on Windows unless TerminateProcess failed
}

} // namespace plus::fastexit
