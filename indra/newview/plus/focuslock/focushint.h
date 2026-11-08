/**
 * @file focushint.h
 * @brief The hint over the world while the focus is picked by clicking (mode 4 of plus/focuslock,
 *        research/264): "Click the place to focus on · Esc to stop".
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

#ifndef PLUS_FOCUSLOCK_FOCUSHINT_H
#define PLUS_FOCUSLOCK_FOCUSHINT_H

// Main thread. The view is made at the first picking (not before), and is drawn only while it is shown;
// not in a snapshot, not in mouselook. plus::focuslock::setPicking() shows and hides it.
namespace plus::focuslock::hint
{
    void setShown(bool shown);
}

#endif // PLUS_FOCUSLOCK_FOCUSHINT_H
