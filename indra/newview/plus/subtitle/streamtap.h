/**
 * @file streamtap.h
 * @brief Subtitles for the parcel's internet radio (research/192): the sound of one process, as 16 kHz mono.
 *
 * The parcel stream is played by a media plugin process (SLPlugin, LibVLC). Windows can hand over just
 * what one process plays (process loopback capture, Windows 10 2004 or later): the radio exactly as it is
 * heard, with no second connection to the station and nothing else mixed in (not the voice, not the game).
 * The sound goes into a ring that the subtitle client reads, like the voice tap.
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

#ifndef PLUS_SUBTITLE_STREAMTAP_H
#define PLUS_SUBTITLE_STREAMTAP_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace plus
{
    namespace subtitle
    {
        namespace streamtap
        {
            // Starts listening to what process `pid` (and the processes it started) plays. Main thread.
            // On failure returns false and says why in `error` (English, for the log). A capture that is
            // running is ended first.
            bool start(uint32_t pid, std::string& error);
            // Ends the capture and waits for its thread (a few ms). Safe when not running.
            void stop();
            // The process being listened to (0: none).
            uint32_t pid();
            // Grows by one at each successful start(). The reader discards what is left in the ring from the
            // capture before when it sees this change (the ring may only be cleared by its reader).
            uint64_t generation();

            // Reader side of the ring (one thread): up to `max` samples of 16 kHz mono, the number copied.
            size_t read(int16_t* dst, size_t max);
            // Throws away what the ring holds (the reader does not want it).
            void discard();
        }
    }
}

#endif // PLUS_SUBTITLE_STREAMTAP_H
