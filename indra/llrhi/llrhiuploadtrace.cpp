/**
 * @file llrhiuploadtrace.cpp
 * @brief Attributes GL buffer uploads to the viewer code that caused them (LLRHI_TRACE_UPLOADS=1).
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

#include "llrhiuploadtrace.h"

#include "llrhi.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <unordered_map>
#include <vector>

#if LL_WINDOWS

#include "llwin32headers.h"
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace
{
    constexpr int MAX_FRAMES = 20;
    constexpr int TOP_SOURCES = 12;

    struct Stack
    {
        void* frames[MAX_FRAMES];
        USHORT count;
        U64 bytes;
        U64 calls;
    };

    std::mutex sMutex;
    std::unordered_map<ULONG, Stack> sStacks[LLRHIUploadTrace::CHANNEL_COUNT]; // keyed by CaptureStackBackTrace hash
    std::unordered_map<DWORD64, std::string> sNames;   // resolved addresses

    const std::string& symbol_name(void* address)
    {
        DWORD64 addr = (DWORD64)address;
        auto it = sNames.find(addr);
        if (it != sNames.end())
        {
            return it->second;
        }

        char buffer[sizeof(SYMBOL_INFO) + 256] = {};
        SYMBOL_INFO* symbol = (SYMBOL_INFO*)buffer;
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        std::string name = SymFromAddr(GetCurrentProcess(), addr, &displacement, symbol)
            ? std::string(symbol->Name)
            : std::string("?");
        return sNames.emplace(addr, name).first->second;
    }

    // The census's reads (NAME_READS): the functions of the read itself, wherever they are (an anonymous
    // namespace's too: the readers of llvknative and llvkgpudriven are in one, and are what we look for).
    bool is_read_plumbing(const std::string& name)
    {
        static const char* const own[] = { "std::", "LLRHIVBCensus::", "LLRHIUploadTrace::" };
        for (const char* prefix : own)
        {
            if (name.compare(0, strlen(prefix), prefix) == 0)
            {
                return true;
            }
        }
        if (name == "?")
        {
            return true;
        }
        // The name itself, whatever namespace it is in (an anonymous one too): not a part of another name
        // (llvkattribpool's on_buffer_changed is a reader we look for).
        static const char* const names[] =
        {
            "gpu_buffer", "gpu_buffer_named", "census_set_owner", "census_clear_owner", "census_owner", "census_changed",
            "buffer_changed", "drawBuffer", "bufferContents", "native_object_versions",
        };
        const size_t colons = name.rfind("::");
        const std::string last = colons == std::string::npos ? name : name.substr(colons + 2);
        for (const char* n : names)
        {
            if (last == n)
            {
                return true;
            }
        }
        return false;
    }

    // Plumbing every upload passes through; attribution starts above these.
    bool is_plumbing(const std::string& name)
    {
        static const char* const prefixes[] =
        {
            "`anonymous namespace'::", "LLRHIUploadTrace", "wrap_",
            "LLVertexBuffer::", "LLRender::", "LLGLSLShader::", "LLTexUnit::", "flush_vbo",
            "LLPipeline::markRebuild",
            "std::", "?",
        };
        for (const char* prefix : prefixes)
        {
            if (name.compare(0, strlen(prefix), prefix) == 0)
            {
                return true;
            }
        }
        return false;
    }

    // "Caller <- its caller" for the first two non-plumbing frames.
    std::string attribute(const Stack& stack, bool reads)
    {
        std::string out;
        int found = 0;
        for (USHORT i = 0; i < stack.count && found < 2; ++i)
        {
            const std::string& name = symbol_name(stack.frames[i]);
            if (reads ? is_read_plumbing(name) : is_plumbing(name))
            {
                continue;
            }
            if (found)
            {
                out += " <- ";
            }
            out += name;
            ++found;
        }
        return out.empty() ? std::string("(unknown)") : out;
    }
}

bool LLRHIUploadTrace::enabled()
{
    static const bool on = LLRHI::envSet("LLRHI_TRACE_UPLOADS");
    return on;
}

void LLRHIUploadTrace::record(U64 amount, Channel channel)
{
    Stack stack;
    ULONG hash = 0;
    stack.count = CaptureStackBackTrace(1, MAX_FRAMES, stack.frames, &hash);

    std::lock_guard<std::mutex> lock(sMutex);
    auto& stacks = sStacks[channel];
    auto it = stacks.find(hash);
    if (it == stacks.end())
    {
        stack.bytes = 0;
        stack.calls = 0;
        it = stacks.emplace(hash, stack).first;
    }
    it->second.bytes += amount;
    it->second.calls += 1;
}

namespace
{
    void ensure_symbols()
    {
        static bool sym_ready = false;
        if (!sym_ready)
        {
            SymSetOptions(SymGetOptions() | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            // May already be initialised by the crash handler; resolving still works then.
            // The search path is the program's own folder: no symbol server from _NT_SYMBOL_PATH is asked for names.
            char folder[MAX_PATH] = {};
            const DWORD length = GetModuleFileNameA(nullptr, folder, MAX_PATH);
            char* slash = length && length < MAX_PATH ? strrchr(folder, '\\') : nullptr;
            if (slash)
            {
                *slash = '\0';
            }
            SymInitialize(GetCurrentProcess(), slash ? folder : nullptr, TRUE);
            sym_ready = true;
        }
    }

    void report_one(LLRHIUploadTrace::Channel channel, U32 frames)
    {
        std::unordered_map<std::string, std::pair<U64, U64>> by_source; // amount, calls
        U64 total = 0;
        {
            std::lock_guard<std::mutex> lock(sMutex);
            for (auto& entry : sStacks[channel])
            {
                auto& slot = by_source[attribute(entry.second, channel == LLRHIUploadTrace::NAME_READS)];
                slot.first += entry.second.bytes;
                slot.second += entry.second.calls;
                total += entry.second.bytes;
            }
            sStacks[channel].clear();
        }
        if (by_source.empty())
        {
            return;
        }

        std::vector<std::pair<std::string, std::pair<U64, U64>>> sorted(by_source.begin(), by_source.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b) { return a.second.first > b.second.first; });

        const bool uploads = channel == LLRHIUploadTrace::UPLOADS;
        const bool reads = channel == LLRHIUploadTrace::NAME_READS;
        if (uploads)
        {
            LL_INFOS("RHIUploads") << "buffer uploads " << (total / frames / 1024) << " KB/frame, top sources (KB/frame, calls/frame):" << LL_ENDL;
        }
        else if (reads)
        {
            // The amount of a sample is its weight (a sample stands for 64 reads): calls over the window, not per frame.
            LL_INFOS("VBCensus") << "reads by GL name of buffers with a number, about " << total << " in the window, top callers (reads, sampled 1 in 64):" << LL_ENDL;
        }
        else
        {
            LL_INFOS("RHIUploads") << "rebuild requests " << (total / frames) << "/frame, top sources (per frame):" << LL_ENDL;
        }
        for (size_t i = 0; i < sorted.size() && i < TOP_SOURCES; ++i)
        {
            if (reads)
            {
                LL_INFOS("VBCensus") << "  [read] " << sorted[i].second.first << "  " << sorted[i].first << LL_ENDL;
                continue;
            }
            U64 amount = sorted[i].second.first / frames;
            LL_INFOS("RHIUploads") << "  [" << (uploads ? "upload" : "rebuild") << "] "
                                   << (uploads ? amount / 1024 : amount) << (uploads ? " KB  " : "  ")
                                   << (sorted[i].second.second / frames) << "x  " << sorted[i].first << LL_ENDL;
        }
    }
}

void LLRHIUploadTrace::report(U32 frames)
{
    if (!frames)
    {
        return;
    }
    ensure_symbols();
    report_one(UPLOADS, frames);
    report_one(REBUILDS, frames);
}

void LLRHIUploadTrace::reportChannel(Channel channel, U32 frames)
{
    ensure_symbols();
    report_one(channel, llmax(frames, 1u));
}

#else // !LL_WINDOWS

bool LLRHIUploadTrace::enabled() { return false; }
void LLRHIUploadTrace::record(U64, Channel) {}
void LLRHIUploadTrace::report(U32) {}
void LLRHIUploadTrace::reportChannel(Channel, U32) {}

#endif // LL_WINDOWS
