/**
 * @file plus_j2c.h
 * @brief <plus> slj2k (the Charis JPEG 2000 decoder for Second Life textures) in front of OpenJPEG.
 *
 * Settings (PlusJ2CDecoder, PlusJ2CDecoderThreads, PlusJ2CDecoderThreadsMinArea) are pushed in
 * from newview/plus/j2cdecoder; this library cannot see gSavedSettings.
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
#ifndef PLUS_J2C_H
#define PLUS_J2C_H

#include "linden_common.h"
#include "llimage.h"

namespace plus_j2c
{
    // mode: 0 = OpenJPEG only, 1 = slj2k first (OpenJPEG when slj2k cannot do the image).
    // threads: slj2k workers for one big image (1 = decode on the calling thread only; at most
    // MAX_THREADS). minArea: decoded pixels (after discard) from which `threads` is used.
    constexpr U32 MAX_THREADS = 8;
    void setConfig(S32 mode, U32 threads, U32 minArea);

    // Whether a codestream may go to a decoder at all (slj2k or OpenJPEG). False, with why, when
    // its header asks for more than a texture can need: wider or taller than MAX_IMAGE_SIZE, more
    // than 64 quality layers, more than 4096 tiles, so many code blocks and packets that the
    // packet headers alone would take minutes (a few hundred bytes can ask for hours) or the
    // decoders gigabytes, or so many samples times coding passes (from the bit planes its
    // quantisation allows) that tier-1 would take longer than for any real texture. Reads the main header and the tile-part headers in `size`; a header cut
    // short is refused (neither decoder could use it). Any thread.
    bool screen(const U8* data, U32 size, const char*& why);

    // Decodes the codestream into raw_image (same layout and channel selection as the OpenJPEG path
    // in LLImageJ2COJ::decodeImpl). Returns false, leaving raw_image alone, when OpenJPEG has to do it.
    bool decode(const U8* data, U32 size, S32 discard, LLImageRaw& raw_image, S32 first_channel, S32 max_channel_count);

    // Counts and times each LLImageJ2COJ::decodeImpl call; logs the averages of both decoders now
    // and then (LL_INFOS("Plus")), so the two can be compared in a real session.
    class Timer
    {
    public:
        explicit Timer(const LLImageRaw& raw);
        ~Timer();
        bool mSlj2k = false;

    private:
        const LLImageRaw& mRaw;
        U64 mStart;
    };
}

#endif // PLUS_J2C_H
