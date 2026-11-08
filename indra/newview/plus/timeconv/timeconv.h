/**
 * @file timeconv.h
 * @brief Time converter window: SL time (US Pacific), UTC and the time of places of choice,
 *        one moment shown on every row; editing any field of any row moves the others.
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

#ifndef PLUS_TIMECONV_H
#define PLUS_TIMECONV_H

class LLTextBox;

namespace plus::timeconv
{
    // The window ("plus_time_conv") is registered from the feature's init and has a menu
    // item in menu_plus.xml. The two official hooks are in LLStatusBar::postBuild() and
    // where the status bar sets the clock's tool tip: a click on the SL clock opens it.

    // Makes a click on the status bar's clock open the window (LLStatusBar::postBuild()).
    void setupStatusBarClock(LLTextBox* clock);

    // Adds "click to convert" to the clock's tool tip, which the status bar rewrites
    // every ten seconds (a line below the date).
    void addClockTooltipHint(LLTextBox* clock);
}

#endif // PLUS_TIMECONV_H
