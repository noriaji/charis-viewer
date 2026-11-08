/**
 * @file fpslimit.h
 * @brief Frame rate limit (plus/fpslimit)
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

#ifndef PLUS_FPSLIMIT_H
#define PLUS_FPSLIMIT_H

namespace plus::fpslimit
{
    // Once per frame in the main loop: waits until 1/PlusFpsLimit seconds have passed since the last
    // call. Does nothing (at once) when the limit is 0. A frame that was late is not made up for.
    void wait();

    // The refresh rate (Hz) of the monitor the viewer window is on; 0 when it cannot be told.
    int monitorRefreshRate();
}

#endif // PLUS_FPSLIMIT_H
