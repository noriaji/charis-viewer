/**
 * @file llvkwindows.h
 * @brief The viewer's other windows (detached UI, research/87): one swapchain
 *        each, presented with the main window's frame.
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

#ifndef LL_LLVKWINDOWS_H
#define LL_LLVKWINDOWS_H

// LLRHI::sPresentWindow and sForgetWindow (llrhi.h). Each window gets an
// LLVKSwapchain of its own (no vsync: MAILBOX, else IMMEDIATE; only the main
// window waits for the display) and an image of its own: the GL texture the
// viewer drew the window's UI into is copied there at once (a fullscreen
// pass, llvkfullscreen), and LLVKMirror::endFrame blits it into the window's
// swapchain image in the frame's own submission.
namespace LLVKWindows
{
    void install();
}

#endif // LL_LLVKWINDOWS_H
