/**
 * @file llrhiprofiler.h
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

#ifndef LL_LLRHIPROFILER_H
#define LL_LLRHIPROFILER_H

// About a thousand times a second, a background thread briefly suspends the
// thread that called install() (the main/render thread), records its call
// stack, and resumes it. Every `period` seconds the functions that were on
// the stack most often (inclusive) and executing most often (self) are
// written to SecondLife.log (tag "RHIProfile"), with symbols from the PDBs.
//
// Needs no external tools, so it can run on the machine where the slowdown
// happens. Windows only; a no-op elsewhere.
namespace LLRHIProfiler
{
    void install(int period_seconds);

    // The sampling used by the profiler, for other tools (plus/hitchlog).
    // init() on the thread to be sampled; sample() on any other thread.
    class StackSampler
    {
    public:
        static constexpr int MAX_FRAMES = 64;

        bool init();
        // Suspends the thread for a copy of its stack only, then walks the copy:
        // the start addresses of the functions on its stack, innermost first, in
        // functions[MAX_FRAMES]. Returns their number, 0 when the stack could not
        // be read, -1 when the thread is gone. Does not allocate or lock while the
        // thread is suspended.
        int sample(U64* functions);

    private:
        void* mThread = nullptr;
        U8* mCopy = nullptr;
    };

    // "module!function" of a code address (the start of a function from
    // sample()), "module!<no symbols>" without a PDB. Slow; any thread, one
    // call at a time per process of this function and the profiler's own naming.
    std::string symbolName(U64 address);
}

#endif // LL_LLRHIPROFILER_H
