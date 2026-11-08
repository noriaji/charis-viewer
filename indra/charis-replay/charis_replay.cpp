/**
 * @file charis_replay.cpp
 * @brief research/271 P0: the viewer's renderer in an executable of its own.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Charis Viewer Source Code
 * Copyright (C) 2026, Charis Viewer contributors
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

// P0 (research/271 9, 3.7a): a hidden window, the Vulkan device and the
// renderer started the way the viewer starts them (LLWindowWin32 hands the
// window to gLLRHIAttachWindow, LLGLManager::initGL loads the GL entry points
// and installs the backend), a few empty frames, then the viewer's shutdown
// (LLGLManager::shutdownGL). No capture is read yet: this shows that the
// renderer links and runs without newview.
//
//   charis-replay [--width N] [--height N] [--frames N] [--cache <dir>] [--no-present]
//   charis-replay --dump <capture.crp>
//
// --dump reads a research/271 capture file (LLRHI_REPLAY_CAPTURE, indra/llrhi/replay) and
// prints its records (no window, no Vulkan device: this does not yet replay the capture, only
// shows what is in it, to check the capture side against what a session actually drew).
//
// Frames are presented to the hidden window's swapchain (FIFO, the driver's
// swap interval), or with --no-present closed unshown after the first
// (LLRHI::onFrameNotShown, the viewer's minimized path: it needs an open
// frame, and only a present opens one). Empty frames: nothing is drawn
// yet. The swapchain is made either way (the
// backend's install needs it); an install without one is left to a later
// stage. The pipeline and shader caches go to <dir> (default:
// charis-replay-cache next to the executable), never the viewer's; the
// capture folders are cleared too. Other LLRHI_* switches still apply,
// and those read by static initializers (the periodic logs) keep the
// environment the process started with.
//
// Exit status: 0 done, 1 the renderer did not start or stopped (Vulkan
// missing, setup failed, device lost: reported on stderr, no dialog),
// 2 bad arguments.

#include "linden_common.h"

#include "llcommon.h"
#include "fsyspath.h"
#include "lldir.h"
#include "llerrorcontrol.h"
#include "llgl.h"
#include "llrhi.h"
#include "llstring.h"
#include "replay/llrhireplayformat.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

// llwindowwin32.cpp: the window hooks the renderer sets (llrhivkmirror.cpp).
extern bool (*gLLRHIAttachWindow)(HWND window);
extern bool (*gLLRHIPresentHook)();

namespace
{
    struct Options
    {
        U32 width = 1280;
        U32 height = 720;
        U32 frames = 3;
        bool present = true;
        std::string cache;      // UTF-8
        std::string dump;       // UTF-8; non-empty: --dump mode, every other option ignored
    };

    void usage()
    {
        std::cerr << "usage: charis-replay [--width N] [--height N] [--frames N] [--cache <dir>] [--no-present]" << std::endl;
        std::cerr << "       charis-replay --dump <capture.crp>" << std::endl;
    }

    // Digits only (no sign, space or wrap-around), within [low, high].
    bool read_count(const std::string& text, U32 low, U32 high, U32& out)
    {
        if (text.empty() || text.size() > 9 || text.find_first_not_of("0123456789") != std::string::npos)
        {
            return false;
        }
        const U32 value = (U32)std::stoul(text);
        if (value < low || value > high)
        {
            return false;
        }
        out = value;
        return true;
    }

    // UTF-8 arguments (main's char** is the ANSI code page).
    std::vector<std::string> utf8_arguments()
    {
        std::vector<std::string> out;
        int count = 0;
        LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!wide)
        {
            return out;
        }
        for (int i = 1; i < count; ++i)
        {
            out.push_back(ll_convert_wide_to_string(wide[i]));
        }
        LocalFree(wide);
        return out;
    }

    bool parse(const std::vector<std::string>& args, Options& options)
    {
        for (size_t i = 0; i < args.size(); ++i)
        {
            const std::string& arg = args[i];
            if (arg == "--no-present")
            {
                options.present = false;
                continue;
            }
            if (i + 1 >= args.size())
            {
                return false;
            }
            const std::string& value = args[++i];
            bool ok = false;
            if (arg == "--dump")
            {
                options.dump = value;
                ok = !value.empty();
            }
            else if (arg == "--width")
            {
                ok = read_count(value, 64, 16384, options.width);
            }
            else if (arg == "--height")
            {
                ok = read_count(value, 64, 16384, options.height);
            }
            else if (arg == "--frames")
            {
                ok = read_count(value, 1, 100000, options.frames);
            }
            else if (arg == "--cache")
            {
                options.cache = value;
                ok = !value.empty();
            }
            if (!ok)
            {
                return false;
            }
        }
        return true;
    }

    // LLRHI::sVulkanUnavailableExit: stderr and the log instead of the
    // viewer's message box, so an unattended run ends instead of waiting.
    void unavailable_exit(LLRHI::VulkanUnavailable why, const std::string& reasons)
    {
        LL_WARNS("Replay") << "Vulkan unavailable or stopped (" << (S32)why << "): " << reasons << LL_ENDL;
        std::cerr << "charis-replay: Vulkan unavailable or stopped: " << reasons << std::endl;
        std::cerr.flush();
        _exit(1);
    }

    LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
    {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    // Never shown: the swapchain is made on it, nothing appears on screen. A
    // popup has no frame, so its client area is the size asked for (an
    // overlapped window would be clamped to the screen); checked after.
    HWND make_hidden_window(U32 width, U32 height)
    {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_OWNDC;
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = L"CharisReplay";
        if (!RegisterClassExW(&window_class))
        {
            const DWORD error = GetLastError();
            LL_WARNS("Replay") << "No window class (error " << error << ")" << LL_ENDL;
            return nullptr;
        }
        const HWND window = CreateWindowExW(0, window_class.lpszClassName, L"charis-replay", WS_POPUP, 0, 0, (int)width,
                                            (int)height, nullptr, nullptr, instance, nullptr);
        if (!window)
        {
            const DWORD error = GetLastError();
            LL_WARNS("Replay") << "No window (error " << error << ")" << LL_ENDL;
            return nullptr;
        }
        RECT client;
        if (!GetClientRect(window, &client) || (U32)(client.right - client.left) != width || (U32)(client.bottom - client.top) != height)
        {
            LL_WARNS("Replay") << "The window's client area is not " << width << "x" << height << LL_ENDL;
            DestroyWindow(window);
            return nullptr;
        }
        return window;
    }

    void pump_messages()
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    // LLCommon's start and end around every return of main.
    struct CommonScope
    {
        CommonScope() { LLCommon::initClass(); }
        ~CommonScope() { LLCommon::cleanupClass(); }
    };
}

namespace
{
    // --dump: no window, no device, no cache (research/271 P1b stage 2). Returns the process
    // exit status.
    int dump_capture(const std::string& path)
    {
        LLRHIReplay::Reader reader(path);
        if (!reader.ok())
        {
            std::cerr << "charis-replay: " << path << ": not a research/271 capture file (bad magic, version, or not found)"
                      << std::endl;
            return 1;
        }
        U32 records = 0, frames = 0;
        LLRHIReplay::RecordHeader header;
        while (reader.next(header))
        {
            if (header.entry_id == LLRHIReplay::ENTRY_FRAME_BOUNDARY)
            {
                ++frames;
                std::cout << "frame " << frames << " ends (seq " << header.sequence << ")" << std::endl;
            }
            else
            {
                std::cout << "  entry " << header.entry_id << " seq " << header.sequence << " thread " << std::hex
                          << header.thread_hash << std::dec << " payload " << header.payload_bytes << " byte(s)" << std::endl;
            }
            ++records;      // the payload is not read: next() seeks past it
        }
        if (!reader.ok())
        {
            std::cerr << "charis-replay: " << path << ": stopped reading after " << records << " record(s) (truncated or damaged)"
                      << std::endl;
            return 1;
        }
        std::cout << records << " record(s), " << frames << " frame boundary(ies)" << std::endl;
        return 0;
    }
}

int main(int, char**)
{
    Options options;
    if (!parse(utf8_arguments(), options))
    {
        usage();
        return 2;
    }
    if (!options.dump.empty())
    {
        return dump_capture(options.dump);
    }

    CommonScope common;
    if (options.cache.empty())
    {
        options.cache = gDirUtilp->add(gDirUtilp->getExecutableDir(), "charis-replay-cache");
    }
    // No logcontrol.xml is looked for next to the executable: the defaults (and a warning that it is missing).
    LLError::initForApplication(options.cache, gDirUtilp->getExecutableDir(), true);
    std::error_code made;   // setCacheDir makes the last folder only
    std::filesystem::create_directories(fsyspath(options.cache), made);
    if (!gDirUtilp->setCacheDir(options.cache))
    {
        LL_WARNS("Replay") << "The cache folder cannot be used: " << options.cache << (made ? " (" + made.message() + ")" : std::string()) << LL_ENDL;
        return 1;
    }
    // LLVKShader::cacheDir takes this before the cache folder: a value set for
    // the viewer would send the replay's caches to the viewer's.
    // The capture folders likewise (vk_<n>.png, capture.now).
    _putenv_s("LLRHI_VK_SHADER_CACHE", "");
    _putenv_s("LLRHI_VK_CAPTURE_DIR", "");
    _putenv_s("LLRHI_CUE_DIR", "");
    // research/271: a capture set for the viewer would otherwise be overwritten by this run's.
    _putenv_s("LLRHI_REPLAY_CAPTURE", "");
    LLRHI::sVulkanUnavailableExit = &unavailable_exit;
    LL_INFOS("Replay") << "charis-replay P0: " << options.width << "x" << options.height << ", " << options.frames << " frames ("
                       << (options.present ? "presented" : "not shown") << "), cache " << gDirUtilp->getCacheDir() << LL_ENDL;

    const HWND window = make_hidden_window(options.width, options.height);
    if (!window)
    {
        return 1;
    }

    // The viewer's order (LLWindowWin32 then LLGLManager::initGL): the device
    // on the window, then the GL entry points and the backend's install.
    if (!gLLRHIAttachWindow || !gLLRHIAttachWindow(window) || !gGLManager.initGL())
    {
        LL_WARNS("Replay") << "The renderer did not start" << LL_ENDL;
        DestroyWindow(window);
        return 1;
    }

    U32 done = 0;
    for (U32 frame = 0; frame < options.frames; ++frame)
    {
        pump_messages();
        if (!options.present && frame > 0)
        {
            LLRHI::onFrameNotShown();
            ++done;
        }
        else if (gLLRHIPresentHook && gLLRHIPresentHook())
        {
            ++done;
        }
    }
    // A frame is open after the loop when the frames went through the backend.
    const bool recording = LLRHI::frameRecording();
    LL_INFOS("Replay") << "Frames ended: " << done << " of " << options.frames << ", a frame open after them: " << (recording ? "yes" : "no") << LL_ENDL;

    gGLManager.shutdownGL();
    DestroyWindow(window);
    return (done == options.frames && recording) ? 0 : 1;
}
