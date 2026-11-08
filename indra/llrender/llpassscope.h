/**
 * @file llpassscope.h
 * @brief Full-screen passes as units (research/137 N12): the viewer's side,
 *        held by LLPipeline's post chain and lighting passes.
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

#ifndef LL_LLPASSSCOPE_H
#define LL_LLPASSSCOPE_H

#include "stdtypes.h"

#include <cstddef>
#include <initializer_list>

class LLRenderTarget;

// One full-screen pass (llrhipass.h), opened right after the pass's target is
// bound (LLRenderTarget::bindTarget) and closed when it goes out of scope
// (after the pass's last draw; the target's flush may come first). Moved from
// plus/nativepass (research/207 6.3): rendering lives in the official files.
//
// target: the bound one, or null for the frame's own target. Does nothing
// while LLRHI_NATIVE_PASSES is 0 or the backend did not set the hooks. Also
// reads the cue files nativepasses.toggle and nativepassesref.toggle in
// LLRHI_CUE_DIR, at most once a second.
//
// A block with two passes one after the other closes the first with close()
// right after its target's flush, before the second opens: one open while
// another is would be counted as replacing it (fix-round6 3).
class LLPassScope
{
public:
    // One texture a pass samples. A render target stands for its first
    // colour attachment; attachment() and depth() name others, texture() a
    // GL name the viewer holds itself. A null target or a name 0 names
    // nothing (left out). No numbers: { src, 0 } would read as two inputs,
    // the second a null target, so plain numbers do not compile.
    class Input
    {
    public:
        Input(const LLRenderTarget* target);
        Input(int) = delete;
        Input(std::nullptr_t) = delete;

        U32 name() const { return mName; }

    private:
        explicit Input(U32 name) : mName(name) {}
        friend class LLPassScope;
        U32 mName = 0;
    };
    static Input attachment(const LLRenderTarget* target, U32 index);  // colour attachment `index` (none: nothing)
    static Input depth(const LLRenderTarget* target);                  // its own depth texture (none or shared: nothing)
    static Input texture(U32 name);

    LLPassScope(const char* name, const LLRenderTarget* target, std::initializer_list<Input> inputs = {});
    ~LLPassScope() { close(); }

    // The pass's end, before the scope's (again: nothing). Const: the scopes
    // are declared const (one line each in pipeline.cpp).
    void close() const;

    LLPassScope(const LLPassScope&) = delete;
    LLPassScope& operator=(const LLPassScope&) = delete;

private:
    mutable bool mOpen = false;
};

#endif // LL_LLPASSSCOPE_H
