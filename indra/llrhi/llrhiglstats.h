/**
 * @file llrhiglstats.h
 * @brief Opt-in GL call counter (LLRHI_TRACE=1), built on GL function pointer redirection.
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

#ifndef LL_LLRHIGLSTATS_H
#define LL_LLRHIGLSTATS_H

// Replaces the draw / bind / upload / uniform GL function pointers with thin
// counting wrappers that forward to the driver. Logs per-frame averages and
// peaks to SecondLife.log every few seconds (tag "RHIStats").
//
// This is both the first user of the interception mechanism the Vulkan
// backend will use, and the measurement tool for picking what to rebuild.
namespace LLRHIGLStats
{
    void install();
}

// Where draws come from (diagnostics): the viewer marks the current pass and
// LLVertexBuffer reports each draw. Every 5 s logs, per frame, how many draws
// each pass issued and how many distinct pieces of geometry they were, so the
// gain of drawing several views at once (shadow cascades, probe faces) can be
// estimated. Off unless LLRHI_DRAW_SOURCES is set.
namespace LLRHIDrawSources
{
    extern bool sEnabled;
    extern int sProbeFace;      // -1: main view, 0..5: reflection probe face being rendered
    extern int sShadowCascade;  // -1: not a sun shadow pass, else the cascade
    void countDraw(const void* geometry, unsigned int offset);
}

#endif // LL_LLRHIGLSTATS_H
