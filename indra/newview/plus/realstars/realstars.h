/**
 * @file realstars.h
 * @brief The real night sky (research/177-real-sky-stars.md): the stars of the Yale Bright Star
 *        Catalogue where they stand for a place and a time, and the constellation lines.
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

#ifndef PLUS_REALSTARS_H
#define PLUS_REALSTARS_H

#include "realskyplaces.h"
#include "stdtypes.h"
#include "v3math.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace plus::starfield
{
    struct Field;
}

namespace plus::realstars
{
    struct Art;     // realstarart.h
}

// The star field of plus/starfield (research/176) with the real stars in it: LLVOWLSky fills its
// buffer from the catalogue in J2000 (x to the vernal equinox, z to the north celestial pole) once,
// and lldrawpoolwlsky.cpp turns the whole sky with one matrix (frame()) for the place and the time.
// starsV.glsl takes the zenith of that frame to leave out what is below the horizon. Main thread.
namespace plus::realstars
{
    // PlusSkyStarsReal is 1 (and LLRHI_REAL_STARS is not "0"). Cached.
    bool wanted();

    // Once a frame, before wanted(): the cues of LLRHI_CUE_DIR (realstars.toggle flips
    // PlusSkyStarsReal, realstarlines.toggle PlusSkyStarsLines, realstarart.toggle
    // PlusSkyStarsArt; none is saved), and plus::starfield's (startwinkle.toggle).
    void update();

    // The star field for PlusSkyStars starMode (plus::starfield::MODE_FIELD or MODE_MILKY_WAY):
    // the catalogue's stars, the Milky Way on its real plane (MODE_MILKY_WAY) and the constellation
    // lines (when their file is there), all in J2000 over the whole sphere. false when the catalogue
    // cannot be read (field is left empty: the made-up sky is drawn instead, with one log line).
    bool fill(S32 starMode, plus::starfield::Field& field);

    // The real Milky Way's plane in the field's frame (J2000): the galactic north pole and the
    // direction of the galactic centre, unit vectors (research/224: the baked picture is made on
    // it, and the faint stars are gathered along it).
    void galacticPlane(LLVector3& pole, LLVector3& centre);

    struct Frame
    {
        F32 gl[16];         // J2000 to the agent frame (x east, y north, z up), for gGL.multMatrix()
        LLVector3 zenith;   // the zenith in J2000 (the matrix's third row): starsV.glsl's star_zenith
    };

    // The turn of the sky for now (PlusSkyStarsTimeMode: with SL's sun sunDir, the real time, a
    // given time; PlusSkyStarsLat / Lon), worked out again only when something it depends on
    // changed (the time: by a second; the date for the sun's own place: by an hour).
    const Frame& frame(const LLVector3& sunDir);

    // PlusSkyStarsLines on: the lines are drawn (the real sky only).
    bool linesWanted();
    // PlusSkyStarsLineAlpha (0 .. 1) and PlusSkyStarsLineWidth (pixels, 0.5 .. 3). Cached.
    F32 lineAlpha();
    F32 lineWidth();
    // The lines come and go with the stars: on any sky that shows them, dimmed by its star
    // brightness as they are (starsF.glsl). Not by SL's sun: a sky may keep its sun up at night
    // (Nacon's Dawn: 18.7 degrees, 2026-10-25), and the lines were left out there.

    // ---- the constellation pictures (research/288) ----------------------------------------
    // PlusSkyStarsArt on: the pictures are drawn over the real sky. Cached.
    bool artWanted();
    // PlusSkyStarsArtAlpha (0 .. 1). Cached.
    F32 artAlpha();
    // The plates (app_settings/plus/starart.bin, realstarart.h), read with the real sky's stars.
    // A missing file is no error (the sky without pictures); a bad one leaves art empty, with one
    // log line.
    void fillArt(Art& art);
    // The pictures (app_settings/plus/starart-tex.bin, about 15 MB), read and checked on a thread
    // of their own once they are first wanted, so the frame that first draws them does not wait
    // for the disk (as plus::starfield::MilkyBaker bakes); only the upload is left for the frame.
    // Made, polled and destroyed on the main thread.
    class ArtPicturesReader
    {
    public:
        enum class State { Reading, Ready, Failed };

        explicit ArtPicturesReader(U32 layersNeeded);   // the read starts here
        ~ArtPicturesReader();                           // waits for a read in flight
        ArtPicturesReader(const ArtPicturesReader&) = delete;
        ArtPicturesReader& operator=(const ArtPicturesReader&) = delete;

        // Reading until the thread is done; then Ready (bytes() the file, layers() its layers, at
        // least layersNeeded) or Failed (missing or not right: one log line here).
        State poll();
        const std::vector<U8>& bytes() const { return mBytes; }
        U32 layers() const { return mLayers; }

    private:
        std::thread mThread;
        std::atomic<bool> mDone{ false };
        // written by the thread before mDone, read after
        std::vector<U8> mBytes;
        U32 mLayers = 0;
        S32 mResult = 0;        // 0 read and right; -1 not read; else the ArtTexResult; -2 too few layers
        U32 mNeeded = 0;
        bool mReported = false;
    };

    // The places of the photo tools' list (research/177 §2.2): realskyplaces.h (PLACES, REGIONS,
    // placeIndexOf()).
    const Place* places(size_t& count);

    // PlusSkyStarsTime (UTC, "YYYY-MM-DDTHH:MM:SSZ") as the photo tools show it, in the PC's time
    // zone ("YYYY-MM-DD HH:MM"; empty for an empty or unreadable setting) ...
    std::string localTextOf(const std::string& utc);
    // ... and back: false when text is not a date (the setting is then left as it is). An empty
    // text is "not given" (utc empty).
    bool utcOfLocalText(const std::string& text, std::string& utc);
}

#endif // PLUS_REALSTARS_H
