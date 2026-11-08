/**
 * @file reloadnotice.h
 * @brief research/250: the notice at the bottom of the world while the
 *        shaders are made in the background ("Preparing shaders... 37 %").
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#ifndef PLUS_SHADERRELOAD_RELOADNOTICE_H
#define PLUS_SHADERRELOAD_RELOADNOTICE_H

#include "stdtypes.h"

// Main thread. The view is made (hidden) once the UI is there, at login, and
// only drawn while shown; not in a snapshot, not before login.
namespace plus::shaderreload::notice
{
    enum class Kind
    {
        Preparing,  // the build: the text, the share done and its bar
        Switching,  // the commit comes at the next frame
        Failed,     // the reload made after a failed build gave up on a program too
    };

    // fraction: of the programs built (Preparing, Switching), 0 to 1.
    void show(Kind kind, F32 fraction = 0.f);
    void hide();
}

#endif // PLUS_SHADERRELOAD_RELOADNOTICE_H
