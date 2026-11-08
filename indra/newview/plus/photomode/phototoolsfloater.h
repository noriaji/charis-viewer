/**
 * @file phototoolsfloater.h
 * @brief The photo tools card (a standard floater).
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

#ifndef PLUS_PHOTOMODE_PHOTOMODEFLOATER_H
#define PLUS_PHOTOMODE_PHOTOMODEFLOATER_H

namespace plus::photomode
{
    // The photo tools window's registered name (LLFloaterReg): other features open or look for the window by it.
    inline constexpr char PHOTO_TOOLS_FLOATER[] = "plus_photo_tools";

    // Registers the floater PHOTO_TOOLS_FLOATER (floater_plus_photo_tools.xml). From the feature's init. The
    // widgets the window uses (the sky list, the Compare button) register themselves.
    void registerFloater();

    // The photo tools window is open and in use: shown and not minimized (in its detached window too). Part
    // of inPhotoMode, which is what features ask.
    bool photoToolsShown();

    // The photo mode: the photo screen is on, or the photo tools are shown (photoToolsShown). The photo tools
    // minimized are not the photo mode. One definition for everything that behaves differently while taking
    // photos (the camera's region limits, the upscale drawn at the full size: what is seen is what is taken).
    bool inPhotoMode();
}

#endif // PLUS_PHOTOMODE_PHOTOMODEFLOATER_H
