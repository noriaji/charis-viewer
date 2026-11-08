/**
 * @file slwxfloater.h
 * @brief Parcel weather: the window that writes the [slwx ...] tag into the parcel description.
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

#ifndef PLUS_SLWXFLOATER_H
#define PLUS_SLWXFLOATER_H

namespace plus::slwx
{
    // The window is registered from the feature's init (slwxfloater.cpp) under this name; the menu
    // opens it with Floater.Toggle.
    constexpr const char* PARCEL_FLOATER_NAME = "plus_weather_parcel";
}

#endif // PLUS_SLWXFLOATER_H
