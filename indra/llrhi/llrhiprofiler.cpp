/**
 * @file llrhiprofiler.cpp
 * @brief Opt-in sampling profiler for the main thread (LLRHI_PROFILE=<seconds>).
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

#include "linden_common.h"

#include "llrhiprofiler.h"
#include "llrhi.h"

#if LL_WINDOWS

#include "llwin32headers.h"
#include <dbghelp.h>
#include <tlhelp32.h>
#include <timeapi.h>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <sstream>
#include <atomic>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace
{
    constexpr int MAX_FRAMES = 64;

    LLRHIProfiler::StackSampler sStack;
    std::atomic<bool> sStop{ false };
    std::thread sThread;
    int sPeriod = 10;

    // DbgHelp is not thread safe: every session of this file (the profiler's
    // and symbolName()'s) calls it under this lock.
    std::mutex sSymbolLock;

    // The target's stack is copied while it is suspended and walked after it
    // resumes: the unwinder takes loader locks, and the suspended thread may be
    // holding them (DLL loads and unloads, e.g. during shutdown).
    constexpr size_t STACK_COPY = 256 * 1024;
    constexpr size_t COPY_PAD = 4096;   // zeros after the copy: the last frame may be read past its end

    // A register that points into the stack that was copied (stack_begin..stack_end)
    // now points into the copy.
    void relocate(DWORD64& reg, DWORD64 stack_begin, DWORD64 stack_end, DWORD64 delta)
    {
        if (reg >= stack_begin && reg < stack_end)
        {
            reg += delta;
        }
    }

    void relocate_saved(CONTEXT& ctx, DWORD64 stack_begin, DWORD64 stack_end, DWORD64 delta)
    {
        relocate(ctx.Rbp, stack_begin, stack_end, delta);
        relocate(ctx.Rbx, stack_begin, stack_end, delta);
        relocate(ctx.Rsi, stack_begin, stack_end, delta);
        relocate(ctx.Rdi, stack_begin, stack_end, delta);
        relocate(ctx.R12, stack_begin, stack_end, delta);
        relocate(ctx.R13, stack_begin, stack_end, delta);
        relocate(ctx.R14, stack_begin, stack_end, delta);
        relocate(ctx.R15, stack_begin, stack_end, delta);
    }

    // Walks the copied stack. ctx has been relocated into the copy (Rsp and the saved
    // registers); the registers the unwinder restores from the copy are relocated again
    // after each step (a frame register saved in the copy is an address of the original
    // stack, which may be gone). functions[i] is the start address of the function at
    // depth i (or the pc itself when the code has no unwind data).
    int walk(CONTEXT& ctx, DWORD64* functions, DWORD64 copy_begin, DWORD64 copy_end, DWORD64 stack_begin, DWORD64 stack_end)
    {
        const DWORD64 delta = copy_begin - stack_begin;
        int count = 0;
        __try
        {
            while (count < MAX_FRAMES && ctx.Rip && ctx.Rsp >= copy_begin && ctx.Rsp + 8 <= copy_end)
            {
                DWORD64 base = 0;
                PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
                functions[count++] = entry ? base + entry->BeginAddress : ctx.Rip;
                if (!entry)
                {
                    // Leaf function: the return address is at the top of the stack.
                    ctx.Rip = *(DWORD64*)ctx.Rsp;
                    ctx.Rsp += 8;
                }
                else
                {
                    void* handler_data = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, entry, &ctx, &handler_data, &establisher, nullptr);
                    relocate_saved(ctx, stack_begin, stack_end, delta);
                    relocate(ctx.Rsp, stack_begin, stack_end, delta);   // UWOP_PUSH_MACHFRAME reads the original Rsp from the stack
                }
            }
        }
        // A read past the copy (a frame larger than the padding after a copy cut at STACK_COPY) ends the
        // walk here, after the target has resumed; in the release build the viewer's own vectored handler
        // runs first (it gives up the emergency memory reserve). Rare: only a stack deeper than STACK_COPY.
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        return count;
    }

    using SymInitializeFn = BOOL (WINAPI*)(HANDLE, PCSTR, BOOL);
    using SymSetOptionsFn = DWORD (WINAPI*)(DWORD);
    using SymGetOptionsFn = DWORD (WINAPI*)();
    using SymFromAddrFn = BOOL (WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
    using SymFromNameFn = BOOL (WINAPI*)(HANDLE, PCSTR, PSYMBOL_INFO);
    using SymGetModuleInfoFn = BOOL (WINAPI*)(HANDLE, DWORD64, PIMAGEHLP_MODULE64);

    struct Symbols
    {
        HANDLE process = nullptr;
        SymFromAddrFn from_addr = nullptr;
        SymFromNameFn from_name = nullptr;
        SymGetModuleInfoFn module_info = nullptr;
        const DWORD64 exe = (DWORD64)GetModuleHandleA(nullptr);

        // One code address of a stack (a function start, or the pc in code
        // without unwind data), named once. Names are counted, not addresses:
        // several addresses can carry one name (overloads, and the code of a
        // module without symbols), and a name on the stack twice is still one
        // sample.
        struct Frame
        {
            U32 name = 0;       // index into names
            U32 module = 0;     // index into names: the module's file name
            bool viewer = false;    // in the viewer's own executable
        };
        std::unordered_map<DWORD64, Frame> frames;
        std::unordered_map<std::string, U32> ids;
        std::vector<std::string> names;

        U32 id(const std::string& name)
        {
            auto it = ids.find(name);
            if (it != ids.end())
            {
                return it->second;
            }
            names.push_back(name);
            return ids[name] = (U32)(names.size() - 1);
        }

        bool init(const char* search_path = nullptr)
        {
            std::lock_guard<std::mutex> lock(sSymbolLock);
            // System32's only: not one planted beside the exe or in the current folder
            HMODULE dbghelp = LoadLibraryExA("dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!dbghelp)
            {
                return false;
            }
            auto initialize = (SymInitializeFn)GetProcAddress(dbghelp, "SymInitialize");
            auto set_options = (SymSetOptionsFn)GetProcAddress(dbghelp, "SymSetOptions");
            from_addr = (SymFromAddrFn)GetProcAddress(dbghelp, "SymFromAddr");
            from_name = (SymFromNameFn)GetProcAddress(dbghelp, "SymFromName");
            module_info = (SymGetModuleInfoFn)GetProcAddress(dbghelp, "SymGetModuleInfo64");
            if (!initialize || !set_options || !from_addr)
            {
                return false;
            }
            // A handle of our own: the crash reporter may run its own DbgHelp session on the pseudo-handle.
            DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &process, 0, FALSE, DUPLICATE_SAME_ACCESS);
            // The options are the process's, shared with every other DbgHelp user: added to,
            // not replaced. A session with a search path of its own (symbolName) adds
            // FAIL_CRITICAL_ERRORS, which then holds for every user of DbgHelp: a missing disk
            // is an error, not a dialog.
            auto get_options = (SymGetOptionsFn)GetProcAddress(dbghelp, "SymGetOptions");
            DWORD options = (get_options ? get_options() : 0) | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS;
            if (search_path)
            {
                options |= SYMOPT_FAIL_CRITICAL_ERRORS;
            }
            set_options(options);
            return initialize(process, search_path, TRUE);
        }

        static std::string module_of(DWORD64 address, DWORD64* base = nullptr)
        {
            HMODULE module = nullptr;
            char path[MAX_PATH] = {};
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)address, &module) && GetModuleFileNameA(module, path, MAX_PATH))
            {
                if (base)
                {
                    *base = (DWORD64)module;
                }
                const char* name = strrchr(path, '\\');
                return name ? name + 1 : path;
            }
            return "?";
        }

        // A module known only by its export table (no PDB: display drivers,
        // and system DLLs without a symbol path).
        bool exports_only(DWORD64 address, const SYMBOL_INFO* symbol)
        {
            if (symbol->Flags & SYMFLAG_EXPORT)
            {
                return true;
            }
            IMAGEHLP_MODULE64 info = {};
            info.SizeOfStruct = sizeof(info);
            return module_info && module_info(process, address, &info) && info.SymType == SymExport;
        }

        // Start address of a function by (undecorated) name, 0 when unknown.
        DWORD64 address_of(const std::string& name)
        {
            std::lock_guard<std::mutex> lock(sSymbolLock);
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
            SYMBOL_INFO* symbol = (SYMBOL_INFO*)buffer;
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            // DbgHelp names the exe's module after its file name (CharisViewer, or
            // SecondLifeViewer for a build before research/227 F4, or secondlife-bin).
            static const std::string exe_module = []()
            {
                // The file name as module_of() takes it, without ".exe".
                char path[MAX_PATH] = {};
                if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
                {
                    return std::string();
                }
                const char* slash = strrchr(path, '\\');
                const std::string file = slash ? slash + 1 : path;
                return file.substr(0, file.find_last_of('.'));
            }();
            std::string qualified = exe_module + "!" + name;
            if (from_name && (from_name(process, qualified.c_str(), symbol) || from_name(process, name.c_str(), symbol)))
            {
                return symbol->Address;
            }
            return 0;
        }

        // Runs on the sampler thread after the target has resumed (DbgHelp and
        // the function table lookup take loader locks).
        const Frame& frame(DWORD64 address)
        {
            std::lock_guard<std::mutex> lock(sSymbolLock);
            return frame_locked(address);
        }

        // With sSymbolLock held.
        const Frame& frame_locked(DWORD64 address)
        {
            auto it = frames.find(address);
            if (it != frames.end())
            {
                return it->second;
            }
            DWORD64 base = 0;
            const std::string module = module_of(address, &base);
            std::string result = module;
            alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
            SYMBOL_INFO* symbol = (SYMBOL_INFO*)buffer;
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 511;
            DWORD64 displacement = 0;
            if (from_addr && from_addr(process, address, &displacement, symbol))
            {
                // Without a PDB, DbgHelp names any address after the nearest
                // export, however far: nvoglv64.dll's Vulkan and GL internals
                // came out as "DrvPresentBuffers" and "vkGetInstanceProcAddr"
                // (research/71). An address that is a function start of its
                // own (or far from the export, in code without unwind data)
                // is another function: say so instead of naming the export.
                bool another_function = false;
                if (displacement && exports_only(address, symbol))
                {
                    DWORD64 image_base = 0;
                    another_function = RtlLookupFunctionEntry(address, &image_base, nullptr) ? true : displacement > 0x400;
                }
                if (another_function)
                {
                    result += "!<no symbols, after ";
                    result += symbol->Name;
                    result += ">";
                }
                else
                {
                    result += "!";
                    result += symbol->Name;
                }
            }
            else if (base && base != exe)
            {
                result += "!<no symbols>";
            }
            Frame entry;
            entry.name = id(result);
            entry.module = id(module);
            entry.viewer = base && base == exe;
            return frames[address] = entry;
        }

        const std::string& name(U32 name_id) const
        {
            return names[name_id];
        }
    };

    // LLRHI_PROFILE_FOCUS=name,name,...: what runs underneath these functions.
    struct Focus
    {
        std::string name;
        DWORD64 address = 0;
        U32 samples = 0;
        std::unordered_map<U32, U32> callees;  // by name id, inclusive, within samples that contain the focus
    };
    std::vector<Focus> sFocus;

    struct Profile
    {
        U32 samples = 0;
        std::unordered_map<DWORD64, U32> self;
        std::unordered_map<U32, U32> inclusive;     // by name id
        // Code outside the viewer's executable entered from the viewer: samples
        // per module, and per (module, the viewer function that called in).
        std::unordered_map<U32, U32> outside;
        std::unordered_map<U64, U32> outside_callers;
    };

    // CPU time per thread name over the last period (all threads of the process),
    // e.g. how busy the image decode pool is while textures load.
    std::string thread_cpu(double period_ms)
    {
        typedef HRESULT (WINAPI *GetThreadDescriptionFn)(HANDLE, PWSTR*);
        static GetThreadDescriptionFn get_description =
            (GetThreadDescriptionFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
        static std::unordered_map<DWORD, U64> last;     // thread id -> kernel + user time (100 ns)
        std::unordered_map<DWORD, U64> now;
        std::unordered_map<std::string, std::pair<double, U32>> groups;    // name -> ms, threads
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return std::string();
        }
        THREADENTRY32 entry = { sizeof(entry) };
        const DWORD pid = GetCurrentProcessId();
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID != pid)
            {
                continue;
            }
            HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
            if (!thread)
            {
                continue;
            }
            FILETIME created, exited, kernel, user;
            if (GetThreadTimes(thread, &created, &exited, &kernel, &user))
            {
                const U64 total = ((U64)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
                                  ((U64)user.dwHighDateTime << 32 | user.dwLowDateTime);
                now[entry.th32ThreadID] = total;
                auto before = last.find(entry.th32ThreadID);
                if (before != last.end() && total >= before->second)
                {
                    std::string name = "(unnamed)";
                    PWSTR description = nullptr;
                    if (get_description && SUCCEEDED(get_description(thread, &description)) && description && *description)
                    {
                        name = ll_convert_wide_to_string(description);
                    }
                    if (description)
                    {
                        LocalFree(description);
                    }
                    // Pool threads are numbered ("ImageDecode:3"): group them.
                    size_t colon = name.find_last_of(':');
                    if (colon != std::string::npos && colon + 1 < name.size() &&
                        name.find_first_not_of("0123456789", colon + 1) == std::string::npos)
                    {
                        name.erase(colon);
                    }
                    auto& group = groups[name];
                    group.first += (total - before->second) / 10000.0;
                    ++group.second;
                }
            }
            CloseHandle(thread);
        }
        CloseHandle(snapshot);
        last.swap(now);

        std::vector<std::pair<double, std::string>> sorted;
        for (const auto& g : groups)
        {
            std::ostringstream label;
            label << g.first;
            if (g.second.second > 1)
            {
                label << " x" << g.second.second;
            }
            sorted.emplace_back(g.second.first, label.str());
        }
        std::sort(sorted.rbegin(), sorted.rend());
        std::ostringstream out;
        out.precision(0);
        out << std::fixed << "  CPU by thread (100% = one core):";
        for (size_t i = 0; i < sorted.size() && i < 12; ++i)
        {
            if (sorted[i].first < period_ms * 0.01)
            {
                break;
            }
            out << " " << sorted[i].second << " " << 100.0 * sorted[i].first / period_ms << "%";
        }
        return out.str();
    }

    void report(Profile& profile, Symbols& symbols)
    {
        if (!profile.samples)
        {
            return;
        }
        auto sorted_names = [&](const std::unordered_map<U32, U32>& counts)
        {
            std::vector<std::pair<U32, std::string>> sorted;
            for (const auto& n : counts)
            {
                sorted.emplace_back(n.second, symbols.name(n.first));
            }
            std::sort(sorted.rbegin(), sorted.rend());
            return sorted;
        };
        // Sum per name (several addresses can resolve to one function).
        auto by_name = [&](const std::unordered_map<DWORD64, U32>& counts)
        {
            std::unordered_map<U32, U32> named;
            for (const auto& c : counts)
            {
                named[symbols.frame(c.first).name] += c.second;
            }
            return sorted_names(named);
        };
        auto percent = [&](U32 count)
        {
            std::ostringstream out;
            out.precision(1);
            out << std::fixed << 100.0 * count / profile.samples << "%";
            return out.str();
        };

        std::ostringstream out;
        out << profile.samples << " samples over " << sPeriod << " s\n  inclusive (on the stack):\n";
        auto inclusive = sorted_names(profile.inclusive);
        for (size_t i = 0; i < inclusive.size() && i < 60; ++i)
        {
            out << "    " << percent(inclusive[i].first) << "  " << inclusive[i].second << "\n";
        }
        out << "  self (executing):\n";
        auto self = by_name(profile.self);
        for (size_t i = 0; i < self.size() && i < 40; ++i)
        {
            out << "    " << percent(self[i].first) << "  " << self[i].second << "\n";
        }
        std::unordered_map<std::string, U32> modules;
        for (const auto& c : profile.self)
        {
            modules[symbols.name(symbols.frame(c.first).module)] += c.second;
        }
        out << "  self by module:";
        for (const auto& m : modules)
        {
            out << " " << m.first << " " << percent(m.second);
        }
        // Time in other modules (drivers, the C runtime, the OS) entered from
        // the viewer, by the viewer function that made the call: which of our
        // calls a driver's time without symbols belongs to.
        out << "\n  outside the viewer, by the viewer function that called in:";
        auto outside = sorted_names(profile.outside);
        for (size_t i = 0; i < outside.size() && i < 10 && outside[i].first * 100 >= profile.samples; ++i)
        {
            const U32 module = symbols.ids[outside[i].second];
            out << "\n    " << percent(outside[i].first) << "  " << outside[i].second << ":";
            std::vector<std::pair<U32, std::string>> callers;
            for (const auto& c : profile.outside_callers)
            {
                if ((U32)(c.first >> 32) == module)
                {
                    std::string caller = symbols.name((U32)c.first);
                    const size_t bang = caller.find('!');
                    callers.emplace_back(c.second, bang == std::string::npos ? caller : caller.substr(bang + 1));
                }
            }
            std::sort(callers.rbegin(), callers.rend());
            for (size_t j = 0; j < callers.size() && j < 6 && callers[j].first * 500 >= profile.samples; ++j)
            {
                out << (j ? ", " : " ") << percent(callers[j].first) << " " << callers[j].second;
            }
        }
        out << "\n" << thread_cpu(sPeriod * 1000.0);
        for (Focus& focus : sFocus)
        {
            if (!focus.address)
            {
                continue;
            }
            out << "\n  under " << focus.name << " (" << percent(focus.samples) << " of all samples):\n";
            auto callees = sorted_names(focus.callees);
            for (size_t i = 0; i < callees.size() && i < 30; ++i)
            {
                out << "    " << percent(callees[i].first) << "  " << callees[i].second << "\n";
            }
            focus.samples = 0;
            focus.callees.clear();
        }
        LL_INFOS("RHIProfile") << out.str() << LL_ENDL;
        profile = Profile();
    }

    void sampler()
    {
        timeBeginPeriod(1);
        Symbols symbols;
        if (!symbols.init())
        {
            LL_WARNS("RHIProfile") << "DbgHelp unavailable: reporting addresses by module only" << LL_ENDL;
        }
        if (const char* focus = std::getenv("LLRHI_PROFILE_FOCUS"))
        {
            std::stringstream names(focus);
            std::string name;
            while (std::getline(names, name, ','))
            {
                Focus f;
                f.name = name;
                f.address = symbols.address_of(name);
                if (!f.address)
                {
                    LL_WARNS("RHIProfile") << "focus function not found: " << name << LL_ENDL;
                }
                sFocus.push_back(f);
            }
        }
        Profile profile;
        DWORD64 functions[MAX_FRAMES];
        auto period_start = std::chrono::steady_clock::now();
        while (!sStop)
        {
            Sleep(1);
            const int count = sStack.sample((U64*)functions);
            if (count < 0)
            {
                break;  // thread gone
            }

            if (count)
            {
                ++profile.samples;
                ++profile.self[functions[0]];
                // Named here, after the target resumed (DbgHelp takes loader locks).
                Symbols::Frame frames[MAX_FRAMES];
                for (int i = 0; i < count; ++i)
                {
                    frames[i] = symbols.frame(functions[i]);
                }
                // Whether the name of frame i is already counted in this sample
                // (recursion, overloads, nested functions of one module without
                // symbols): each name counts once per sample.
                auto named_before = [&](int i)
                {
                    for (int j = 0; j < i; ++j)
                    {
                        if (frames[j].name == frames[i].name) return true;
                    }
                    return false;
                };
                for (Focus& focus : sFocus)
                {
                    int depth = -1;
                    for (int i = 0; i < count && depth < 0; ++i)
                    {
                        if (functions[i] == focus.address) depth = i;
                    }
                    if (depth < 0)
                    {
                        continue;
                    }
                    ++focus.samples;
                    for (int i = 0; i < depth; ++i)
                    {
                        if (!named_before(i)) ++focus.callees[frames[i].name];
                    }
                }
                for (int i = 0; i < count; ++i)
                {
                    if (!named_before(i))
                    {
                        ++profile.inclusive[frames[i].name];
                    }
                }
                // Code of other modules entered from the viewer: from the
                // outermost frame inward, `caller` is the nearest viewer frame
                // outside frame i (none for the thread start code).
                U64 counted[2 * MAX_FRAMES];    // up to two keys per frame
                int counted_count = 0;
                auto count_once = [&](U64 key)
                {
                    for (int j = 0; j < counted_count; ++j)
                    {
                        if (counted[j] == key) return false;
                    }
                    counted[counted_count++] = key;
                    return true;
                };
                int caller = -1;
                for (int i = count - 1; i >= 0; --i)
                {
                    if (frames[i].viewer)
                    {
                        caller = i;
                        continue;
                    }
                    if (caller < 0)
                    {
                        continue;
                    }
                    // Module keys have U32_MAX in the high half: never a (module, caller) key.
                    const U64 module_key = ((U64)U32_MAX << 32) | frames[i].module;
                    if (count_once(module_key))
                    {
                        ++profile.outside[frames[i].module];
                    }
                    const U64 pair_key = ((U64)frames[i].module << 32) | frames[caller].name;
                    if (count_once(pair_key))
                    {
                        ++profile.outside_callers[pair_key];
                    }
                }
            }
            if (std::chrono::steady_clock::now() - period_start >= std::chrono::seconds(sPeriod))
            {
                report(profile, symbols);
                period_start = std::chrono::steady_clock::now();
            }
        }
    }
}

namespace
{
    // Suspends the thread, copies its stack, resumes it. Returns false when the thread is gone.
    // Suspended: only GetThreadContext, VirtualQuery (a system call, no user-side lock)
    // and ReadProcessMemory; no heap, no locks, no logging. The thread may be running on a
    // stack other than the one it started with (LLCoros' coroutines): the range to copy is
    // the committed, readable memory from rsp on, not a limit taken at start. The copy is
    // not a memcpy in a __try: a failed read is an error code from the kernel, with no
    // exception, so none of the exception handlers (the viewer's own takes locks that the
    // suspended thread may hold) run on this thread while the other is stopped.
    bool copy_suspended(HANDLE target, U8* copy, CONTEXT& ctx, DWORD64& rsp, size_t& copied)
    {
        if (SuspendThread(target) == (DWORD)-1)
        {
            return false;
        }
        ctx = CONTEXT();
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        MEMORY_BASIC_INFORMATION info = {};
        if (GetThreadContext(target, &ctx) && VirtualQuery((const void*)ctx.Rsp, &info, sizeof(info)) &&
            info.State == MEM_COMMIT && info.Protect && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        {
            rsp = ctx.Rsp;
            const DWORD64 end = (DWORD64)info.BaseAddress + info.RegionSize;
            if (end > rsp)
            {
                SIZE_T got = 0;
                ReadProcessMemory(GetCurrentProcess(), (const void*)rsp, copy, (SIZE_T)llmin((DWORD64)STACK_COPY, end - rsp), &got);
                copied = (size_t)got;
            }
        }
        ResumeThread(target);
        return true;
    }
}

bool LLRHIProfiler::StackSampler::init()
{
    mCopy = new U8[STACK_COPY + COPY_PAD]();
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), (HANDLE*)&mThread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    return mThread != nullptr;
}

int LLRHIProfiler::StackSampler::sample(U64* functions)
{
    static_assert(MAX_FRAMES == ::MAX_FRAMES, "one frame count");
    HANDLE target = (HANDLE)mThread;
    CONTEXT ctx = {};
    size_t copied = 0;
    DWORD64 rsp = 0;
    if (!copy_suspended(target, mCopy, ctx, rsp, copied))
    {
        return -1;
    }
    if (!copied)
    {
        return 0;
    }
    // Relocate the stack pointers into the copy, then unwind there.
    const DWORD64 delta = (DWORD64)mCopy - rsp;
    relocate_saved(ctx, rsp, rsp + copied, delta);
    ctx.Rsp += delta;
    return walk(ctx, (DWORD64*)functions, (DWORD64)mCopy, (DWORD64)mCopy + copied, rsp, rsp + copied);
}

std::string LLRHIProfiler::symbolName(U64 address)
{
    // A session of its own (kept for the life of the process) that looks first in the exe's folder
    // (and where the PE file says its PDB was built): no symbol server, whatever _NT_SYMBOL_PATH says.
    static Symbols* symbols = []()
    {
        Symbols* s = new Symbols();
        char path[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, path, MAX_PATH))
        {
            if (char* slash = strrchr(path, '\\'))
            {
                *slash = 0;
            }
        }
        s->init(path);
        return s;
    }();
    // Everything under the lock: the names are shared with other threads' calls.
    std::lock_guard<std::mutex> lock(sSymbolLock);
    std::string result = symbols->name(symbols->frame_locked(address).name);
    // Without a PDB (a release build ships none) a name says only the module: add where
    // in it, so that the function can be found afterwards (the PDB of the build, dumpbin).
    if (result.find('!') == std::string::npos || result.find("<no symbols") != std::string::npos)
    {
        DWORD64 base = 0;
        Symbols::module_of(address, &base);
        if (base)
        {
            char rva[32];
            snprintf(rva, sizeof(rva), "+0x%llx", (unsigned long long)(address - base));
            result += rva;
        }
    }
    return result;
}

void LLRHIProfiler::install(int period_seconds)
{
    if (sThread.joinable())
    {
        return;
    }
    sPeriod = llmax(1, period_seconds);
    // The sampled (main) thread, named for the per-thread CPU report.
    typedef HRESULT (WINAPI *SetThreadDescriptionFn)(HANDLE, PCWSTR);
    if (auto set_description = (SetThreadDescriptionFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription"))
    {
        set_description(GetCurrentThread(), L"Main");
    }
    if (!sStack.init())
    {
        LL_WARNS("RHIProfile") << "Could not open the main thread for sampling" << LL_ENDL;
        return;
    }
    sThread = std::thread(sampler);
    // Stop and join before the process exits: a thread killed by ExitProcess while
    // holding the heap or log lock deadlocks whatever runs after it.
    std::atexit([]()
    {
        sStop = true;
        if (sThread.joinable())
        {
            sThread.join();
        }
    });
    LL_INFOS("RHIProfile") << "Sampling the main thread, report every " << sPeriod << " s" << LL_ENDL;
}

#else // !LL_WINDOWS

void LLRHIProfiler::install(int) {}
bool LLRHIProfiler::StackSampler::init() { return false; }
int LLRHIProfiler::StackSampler::sample(U64*) { return -1; }
std::string LLRHIProfiler::symbolName(U64) { return std::string(); }

#endif // LL_WINDOWS
