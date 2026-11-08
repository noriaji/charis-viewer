/**
 * @file llrhiuploadtrace.h
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

#ifndef LL_LLRHIUPLOADTRACE_H
#define LL_LLRHIUPLOADTRACE_H

#include "stdtypes.h"

// Every vertex/index upload ends in a few glBufferData/glBufferSubData calls
// inside LLVertexBuffer, so the GL call alone says nothing about *who* is
// streaming data. This samples the call stack on each upload, skips the
// generic buffer/render plumbing, and reports bytes per frame by the first
// viewer function above it (flexi prims, particles, avatars, UI, ...).
// Windows only; needs the RelWithDebInfo symbols next to the executable.
namespace LLRHIUploadTrace
{
    enum Channel
    {
        UPLOADS,    // bytes, from the GL upload wrappers
        REBUILDS,   // calls, from diagnostic hooks in LLPipeline::markRebuild
        NAME_READS, // calls, from LLRHIVBCensus::nameRead (research/293 branch 0); reported by reportChannel only
        CHANNEL_COUNT
    };

    bool enabled();

    void record(U64 amount, Channel channel = UPLOADS);

    // From the stats report: log the top sources per channel for the window just closed.
    void report(U32 frames);

    // One channel's report for the window just closed (and clears it), whether
    // or not LLRHI_TRACE_UPLOADS is set: the callers of LLRHIVBCensus.
    void reportChannel(Channel channel, U32 frames);
}

#endif // LL_LLRHIUPLOADTRACE_H
