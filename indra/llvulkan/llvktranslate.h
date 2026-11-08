/**
 * @file llvktranslate.h
 * @brief What is left of the retired translation thread: its two questions, always "no".
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

#ifndef LL_LLVKTRANSLATE_H
#define LL_LLVKTRANSLATE_H

// The translation thread (LLRHI_TRANSLATE_THREAD, research/118) was retired
// in stage 7 (research/207 T1, 2026-10-27): every call into the backend runs
// on the thread that makes it. Two files of the retained scene
// (llvkscene.cpp, llvkgl_scene.cpp) still ask; they are left alone until
// their stage 7.2 work, and then this header goes.
namespace LLVKTranslate
{
    constexpr int wanted() { return 0; }
    constexpr bool redirect() { return false; }
}

#endif // LL_LLVKTRANSLATE_H
