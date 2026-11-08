/**
 * @file starfield.h
 * @brief The night sky's stars, and the Milky Way as an option (research/176-star-field.md).
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

#ifndef PLUS_STARFIELD_H
#define PLUS_STARFIELD_H

#include "stdtypes.h"
#include "v3math.h"
#include "v4color.h"

#include <string>
#include <vector>

// The official sky draws 1000 stars scattered at random, all of about one
// brightness and one tint, as textured squares. This makes the catalogue of a
// real-looking sky: thousands of stars, mostly faint and a few bright, each of
// the colour of its temperature; and, as an option, the Milky Way as a band of
// soft, faint patches with a dark rift. LLVOWLSky builds the vertex buffer
// from it; starsV.glsl / starsF.glsl draw it (star_field).
namespace plus::starfield
{
    // PlusSkyStars
    constexpr S32 MODE_OFFICIAL = 0;    // the official stars, as they were
    constexpr S32 MODE_FIELD = 1;       // the star field
    constexpr S32 MODE_MILKY_WAY = 2;   // the star field and the Milky Way

    // PlusSkyStars, clamped to MODE_OFFICIAL .. MODE_MILKY_WAY. Cached; main thread.
    S32 mode();

    // PlusSkyStarGain (0.25 .. 4), the brightness of the stars and the Milky Way. Cached.
    F32 gain();

    // PlusSkyStarsMilkyWayGain (0 .. 2, 0.25 by default), the Milky Way's patches alone, on top
    // of gain() (2026-10-26: the band was too bright beside the stars; fix-r4: 0.6 -> 0.25). 0: the patches are not drawn.
    // Cached; main thread.
    F32 milkyWayGain();

    // PlusSkyStarsDense (research/224): about 40,000 faint stars more (generateDenseOn). Cached;
    // main thread.
    bool dense();

    // PlusSkyStarsMilkyWayHQ (research/224): the Milky Way baked into a picture of the sky (skymilky.h)
    // instead of the soft patches, for PlusSkyStars 2. Cached; main thread.
    bool milkyWayHQ();

    // Whether the Milky Way of PlusSkyStars starMode is the baked picture (the patches are then
    // not made).
    bool bakedMilkyWay(S32 starMode);

    // The made-up sky's Milky Way plane in the star field's frame (z up): the galactic pole and the
    // direction of the centre, unit vectors (starfieldmath.h madeUpPlane()). The real sky's are
    // plus::realstars::galacticPlane().
    void madeUpPlane(LLVector3& pole, LLVector3& centre);

    // The twinkle (research/176 section 11, starsV.glsl twinkleOf()). The shader's waves are
    // whole turns of this period, so its clock wraps without a jump.
    constexpr F32 TWINKLE_PERIOD = 1024.f;

    // PlusSkyStarsTwinkle (0 .. 2), how much the stars twinkle; 0 while LLRHI_STAR_TWINKLE is
    // "0" or the cue startwinkle.toggle has turned it off (LLRHI_CUE_DIR, not saved). Cached.
    F32 twinkle();

    // The twinkle's clock: seconds of the world's time (plus::freeze: it stops with the world)
    // wrapped to TWINKLE_PERIOD. Held while a snapshot is drawn (the stars stay as they were at
    // the shutter, every tile of a large photo alike) and while PlusSkyStarsTwinkleHold is on.
    // Main thread, once or more a frame (the same value within a frame).
    F32 twinkleTime();

    // PlusSkyStarsConstellationBoost (0 .. 2): how much brighter the constellation stars are
    // drawn (research/177 section 2.7), whether or not their lines are. Cached.
    F32 constellationBoost();

    // The colours to draw field's stars with: star_color, the constellation stars (star_mark)
    // raised by boost (its light times 1 + CONSTELLATION_LIGHT_STEP * boost; the level is a log
    // scale, so the brightest stay at the top). colors is filled again, its size that of star_color.
    constexpr F32 CONSTELLATION_LIGHT_STEP = 0.3f;
    struct Field;
    void boostedColors(const Field& field, F32 boost, std::vector<LLColor4>& colors);

    // The cues of LLRHI_CUE_DIR (startwinkle.toggle, LLRHI::takeCue); plus::realstars::update()
    // calls it once a second while the cue directory is set.
    void pollCues();

    struct Field
    {
        // Stars: unit directions (z up, above the horizon) and colours (rgb: the tint, its
        // brightest channel 1; alpha: the level, the light on a log scale: starsV.glsl).
        std::vector<LLVector3> star_dir;
        std::vector<LLColor4> star_color;
        // The real sky's constellation stars (plus/realstars): 1 for a star on any of the
        // constellation lines, by the star's index. Empty for the made-up sky.
        std::vector<U8> star_mark;

        // Milky Way patches: unit directions, colours (alpha: the level of its light) and
        // angular radii (radians).
        std::vector<LLVector3> glow_dir;
        std::vector<LLColor4> glow_color;
        std::vector<F32> glow_radius;

        // The real sky's constellation lines (plus/realstars): a segment from line_a[i] to
        // line_b[i], unit directions. Empty for the made-up sky.
        std::vector<LLVector3> line_a;
        std::vector<LLVector3> line_b;

        void clear();
    };

    // Fills field for the mode (MODE_FIELD or MODE_MILKY_WAY; the official mode has nothing
    // to fill). The same sky every time (starfieldmath.h, a fixed seed).
    void generate(S32 mode, Field& field);
}

#endif // PLUS_STARFIELD_H
