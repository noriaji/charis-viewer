/**
 * @file llrhishaderdump.h
 * @brief Opt-in dump of every final GLSL source the viewer compiles (LLRHI_DUMP_SHADERS=<dir>).
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

#ifndef LL_LLRHISHADERDUMP_H
#define LL_LLRHISHADERDUMP_H

#include <string>

// LLShaderMgr assembles each shader at runtime (version line, defines,
// feature headers, then the file). The Vulkan backend has to translate that
// assembled text, not the files on disk, so this captures it at glShaderSource
// as <dir>/<stage>_<hash>.glsl, one file per distinct source.
namespace LLRHIShaderDump
{
    // dir: where to write the sources.
    void install(const std::string& dir);
}

#endif // LL_LLRHISHADERDUMP_H
