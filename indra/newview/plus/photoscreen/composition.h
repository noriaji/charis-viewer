/**
 * @file composition.h
 * @brief Composition presets of the simple photo screen (research/218): full body, knees up, bust,
 *        face, low angle, high angle. The camera is put at a distance and a height worked out from
 *        the person in focus (so it follows who is chosen, and a preset does not mean a place).
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

#ifndef PLUS_PHOTOSCREEN_COMPOSITION_H
#define PLUS_PHOTOSCREEN_COMPOSITION_H

namespace plus::photoscreen::composition
{
    enum class Shot
    {
        Full,   // the whole body
        Knee,   // from the knees up
        Bust,   // from the chest up
        Face,   // the head and the neck
        Low,    // the whole body from below, looking up
        High,   // the whole body from above, looking down
        Count
    };

    // The shot's key ("full", "knee", "bust", "face", "low", "high"), as the UI's strings name it.
    const char* key(Shot shot);

    enum class Result
    {
        Ok,
        NoTarget,   // nobody to frame: not in the world, the person is out of range / not drawn, or has no skeleton to measure
        NoCamera    // no camera to move
    };

    // Puts the camera for the shot, in front of the person the focus lock points at (the focus lock off
    // or on the cursor: yourself), looking at the middle of the part the shot shows, at the distance that
    // fills the picture's height with it. The angle of view is not touched. The camera that was in use is
    // kept for restorePrevious(). Main thread. Does nothing but say why when it cannot.
    Result apply(Shot shot);

    // Whether apply() would find someone to frame now. Cheap: for the UI to grey the buttons.
    bool canApply();

    // The camera before the last apply() (one step; applying again replaces it).
    bool hasPrevious();
    bool restorePrevious();
}

#endif // PLUS_PHOTOSCREEN_COMPOSITION_H
