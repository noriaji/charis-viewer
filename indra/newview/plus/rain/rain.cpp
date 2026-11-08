/**
 * @file rain.cpp
 * @brief Rain: falling streaks and splashes laid over the scene before tone
 *        mapping, kept off what lies under a roof by the overhead map.
 *        Registers with the plus foundation (research/120-rain-idea.md,
 *        stages 2 and 3).
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

// The viewer's own rain: a setting, not the region's weather (a sunny EEP
// sky can have it, a rainy one does not bring it). Off by default.
//
// PlusRain: 0 off, 1 weak, 2 medium, 3 strong.
// PlusRainWind: the wind's speed (m/s); PlusRainWindDirection: where it
// blows from (degrees, 0 north, 90 east).
// PlusRainSplashes: at most this many splashes per square metre and second
// (0: none).
// PlusRainDropScale, PlusRainDropLength: heavy rain's large drops (a drop's
// width and its streak's length at the full downpour, times; "Heavy rain" below).
// Vulkan only: the pass is recorded by the backend (llvulkan/llvkrain.cpp).
// Not part of the graphics presets.
//
// Each frame, after the exposure is measured and before the glare and tone
// mapping (research/120 sections 7 and 12), one pass over the scene's HDR
// target:
//  - streaks: drops fixed in the world, about 0.7 to 28 m from the camera.
//    They fall at the level's speed and drift with the wind, never with the
//    camera: turning the camera leaves them where they are, moving it makes
//    the near ones sweep past faster than the far ones, as real rain does.
//    A streak is the drop's path over STREAK_TIME, a straight line in the
//    world (down and downwind) projected like any other. The drops lie in
//    "rain space" (sheared by the wind, moving down with the fall), where
//    they stand still; there they sit on vertical sheets in five bands of
//    distance, each twice as far as the one before, with sheets and cells
//    growing with the distance (llrhirain.h RainBand). Their width in
//    metres is the drop's plus the lens's blur (at least a pixel, then as
//    much fainter), so near drops are wide and faint, far ones hairlines,
//    and a large snapshot shows the rain as the screen does;
//  - splashes on surfaces facing up within SPLASH_RANGE: a ring and a crown
//    of droplets, in world cells (they stay put). On avatars (the stencil's
//    avatar bit, plus/skinsss: their rigged meshes and system bodies) and on
//    an umbrella's canopy the crown only (fix-round77);
//  - a faint haze of the far rain.
// A drop shows its surroundings: about the sky's light (its colour; dimmer
// under a dark sky), so a streak brightens what is darker than that and
// barely changes the sky; and the sun's light when it lies in front of it.
// The rain shows more under a cloudy sky.
// Under a roof nothing falls (the overhead map, overhead.cpp, section 8);
// the wet look (plus/wetlook) reads the same map, so the ground under a roof
// stays dry, and while it rains its puddles ripple more. With PlusRainWetAll
// (on by default) the wet look wets everything the rain falls on by the
// level (rainLevel()), and lets it dry slowly after.
// The rain is drawn only with HDR on (LLPipeline::renderFinalize() calls
// renderStreaks() in its HDR branch): with HDR off it neither wets nor
// ripples, and the overhead map is not drawn for it (rainShown()).
// While the world is stopped (PlusWorldFreeze) the rain's clock stands: the
// drops hang where they are, the splashes hold, nothing gets wetter or drier.

#include "llviewerprecompiledheaders.h"

#include "plus.h"

#include "rain.h"

#include "overhead.h"

#include "plus/freeze/freeze.h"
#include "skinsss/skinsss.h"
#include "slwx/slwx.h"
#include "wetlook/wetlook.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llcallbacklist.h"
#include "llenvironment.h"
#include "llevents.h"
#include "llframetimer.h"
#include "llgl.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llrhi.h"
#include "llrhiwetlook.h"
#include "llthread.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llvoavatarself.h"
#include "pipeline.h"
#include "v3color.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

extern bool gCubeSnapshot;      // llviewerdisplay.cpp: a reflection probe face or mirror is being drawn
extern bool gSnapshot;         // llviewerdisplay.cpp: a snapshot is being drawn

namespace
{
    constexpr char SETTING_LEVEL[] = "PlusRain";
    constexpr char SETTING_WIND[] = "PlusRainWind";
    constexpr char SETTING_WIND_DIRECTION[] = "PlusRainWindDirection";
    constexpr char SETTING_SPLASHES[] = "PlusRainSplashes";
    constexpr char SETTING_SNOW[] = "PlusSnow";
    constexpr char SETTING_EAVES[] = "PlusRainEaves";
    constexpr char SETTING_UMBRELLA[] = "PlusRainUmbrella";
    constexpr char SETTING_UMBRELLA_NAMES[] = "PlusRainUmbrellaNames";
    constexpr char SETTING_DROP_SCALE[] = "PlusRainDropScale";
    constexpr char SETTING_DROP_LENGTH[] = "PlusRainDropLength";

    enum Level : S32
    {
        LEVEL_OFF = 0,
        LEVEL_WEAK = 1,
        LEVEL_MEDIUM = 2,
        LEVEL_STRONG = 3,
        LEVEL_COUNT = 4,
    };

    // Per level: visible drops per cubic metre, how fast they fall (m/s), a
    // streak's opacity where it covers a pixel, splashes per square metre and
    // second, the far haze, and the wet look's puddle ripples while it rains
    // (their tilt, and rings per cell and second: times
    // LLRHI::WET_LOOK_TIME_PERIOD whole).
    struct Strength
    {
        F32 density;
        F32 fall_speed;
        F32 opacity;
        F32 splashes;
        F32 veil;
        F32 ripples;
        F32 ripple_rate;
    };
    constexpr Strength STRENGTHS[LEVEL_COUNT] =
    {
        { 0.f,  0.f, 0.f,   0.f,  0.f,   0.f,   0.f },     // off
        { 4.f,  6.f, 0.5f,  15.f, 0.04f, 0.25f, 1.6f },    // weak: a drizzle
        { 10.f, 8.f, 0.6f,  40.f, 0.08f, 0.35f, 2.4f },    // medium
        { 24.f, 9.f, 0.7f,  90.f, 0.15f, 0.45f, 4.f },     // strong: a downpour
    };
    // Eaves (PlusRainEaves): the share of a roof edge's columns (one every
    // EAVE_COLUMN in the shader, 12.5 cm) that drip, per rain level; a step
    // in the overhead map's height of at least EAVE_STEP m is a roof's edge;
    // drips are drawn out to EAVE_RANGE m.
    constexpr F32 EAVE_DENSITY[LEVEL_COUNT] = { 0.f, 0.2f, 0.35f, 0.5f };
    constexpr F32 EAVE_STEP = 1.8f;
    constexpr F32 EAVE_RANGE = 10.f;

    // Snow, per level, in the same terms (the veil is the far flakes' haze;
    // ripples and ripple_rate are unused: it makes no puddle ripples). Flakes
    // are few, large and slow: they fall at about a metre a second.
    constexpr Strength SNOW_STRENGTHS[LEVEL_COUNT] =
    {
        { 0.f,  0.f,  0.f,  0.f, 0.f,   0.f, 0.f },    // off
        { 3.f,  0.9f, 0.8f, 0.f, 0.03f, 0.f, 0.f },    // light snow
        { 8.f,  1.2f, 0.85f, 0.f, 0.06f, 0.f, 0.f },   // snow
        { 20.f, 1.6f, 0.9f, 0.f, 0.12f, 0.f, 0.f },    // heavy snow
    };
    // Flakes: this wide (m; the rain's drops 2 mm), blurred by the lens this
    // much (m), fluttering sideways this far (m), drifting with the wind this
    // share as fast as rain would (they are light, but slow: a slope of
    // wind / fall speed would be extreme). They show the sky's light more
    // (a flake is white and opaque), and the sun catches them more.
    constexpr F32 SNOW_DROP_WIDTH = 0.008f;
    constexpr F32 SNOW_DEFOCUS = 0.006f;
    constexpr F32 SNOW_SWAY = 0.06f;
    constexpr F32 SNOW_WIND_SHARE = 0.35f;
    constexpr F32 SNOW_LIGHT = 0.85f;
    constexpr F32 SNOW_HIDE = 0.8f;
    constexpr F32 SNOW_GLINT_SHARE = 0.15f;
    // The flakes' look (fix-round79; llvkrain.cpp flakeShape). Real snow is not a streak: a flake
    // falls about a metre a second, so over the shutter it moves less than its own size and shows
    // as a small soft round dot (brightest at its centre), the near ones as faint blurred discs,
    // the far ones as small faint dots. Drawn as the rain's streaks the flakes were lines three to
    // eight times as long as wide, a meteor's head and tail ("流星みたい"). One place to tune:
    //  - SNOW_FLAKE_STRETCH: a flake's motion blur at most this share of its drawn width (0.01: a
    //    round dot; 0.35: at most 1.35 times as long as wide, down its fall and the wind's slant;
    //    keep it above 0: 0 tells the shader to draw the rain's streaks);
    //  - SNOW_NEAR_FADE (m): flakes nearer than this fade out, none at half of it (the nearest
    //    flakes, a metre off, were large blurred blobs across the view);
    //  - SNOW_GLINT_LIMIT (exposed): the sun's (moon's) light on a flake at most this (rain: 2):
    //    a flake towards the sun gets a little brighter, round as it is, not a blown-out spot.
    // The flake's size: SNOW_DROP_WIDTH times 0.5 to 1.6 (the shader's SIZE_MIN, SIZE_MAX, shared
    // with the rain), blurred by SNOW_DEFOCUS; once a pixel is wider than that, the flake is as
    // much fainter by the pixel's area (far flakes small and faint).
    constexpr F32 SNOW_FLAKE_STRETCH = 0.35f;
    constexpr F32 SNOW_NEAR_FADE = 1.5f;        // m
    constexpr F32 SNOW_GLINT_LIMIT = 0.5f;
    // The fall speed the overhead map's upwind look uses while the rain is off
    // (the wet look's dry spots under the eaves still lean with the wind).
    constexpr F32 RESTING_FALL_SPEED = 8.f;

    // The streaks' bands (llrhirain.h RainBand): the first starts this far
    // from the camera (m; nearer drops are too large and blurred to show),
    // each next twice as far; neighbours cross-fade over twice the overlap
    // (octaves). In each, the sheets stand this share of the band's near
    // distance apart (so a ray crosses two or three in a band); a sheet's
    // cells: half of them hold a drop, CELL_ASPECT times as high as wide;
    // none narrower than CELL_MIN_ANGLE seen from the band's near distance
    // (far bands: fewer, brighter drops, at most BRIGHTNESS_LIMIT times: the
    // rest of the far rain is its haze) nor than four of the widest drops.
    constexpr F32 FIRST_BAND = 0.75f;           // m
    constexpr F32 BAND_OVERLAP = 0.25f;         // octaves
    constexpr F32 SHEET_SPACING = 0.6f;
    // Slanted rain: a ray crosses up to sqrt(1 + slope^2) times as many
    // sheets in rain space, so they stand that much farther apart (rounded
    // up to quarter octaves: the drops change only at those steps as the
    // wind changes).
    constexpr F32 SHEET_STEPS_PER_OCTAVE = 4.f;
    constexpr F32 CELL_FILL = 0.5f;
    constexpr F32 CELL_ASPECT = 4.f;
    constexpr F32 CELL_MIN_ANGLE = 0.006f;      // rad
    constexpr F32 BRIGHTNESS_LIMIT = 2.5f;
    // The most a drop is brightened in any case (research/275 L153). The heavy rain's larger cells
    // (bandFor) let it go past BRIGHTNESS_LIMIT: 3.5 at most with the default drop scale and length
    // (2 and 1.4, band 4 at a region's 10 mm/h), but 8.4 with the most (4 and 2.5, band 3). A far drop
    // narrower than a pixel is drawn at shape ~0.2 there (its width / the pixel's at 6 m), and the
    // pixel's cover stops at 1: past 5 the near end of the band is cut anyway, and the drops look
    // sparse and dark. At 5 that end just reaches 1; the default settings stay under it (as before).
    constexpr F32 BRIGHTNESS_CAP = 2.f * BRIGHTNESS_LIMIT;
    // A streak is the drop's path over this long (a camera's shutter), or over
    // the frame time when that is longer (up to STREAK_TIME_MAX, followed over
    // STREAK_TIME_SMOOTHING s), so that at a low frame rate the drops do not
    // strobe (the cells are sized for the longest); a drop is this
    // wide (the shader scales it by 0.5 to 1.6, most drops small); the lens
    // blurs every drop by this much (m: a camera focused far, so the near
    // drops come out wide and faint). The shader varies a streak's length by
    // LENGTH_SPREAD either way (llvkrain.cpp).
    constexpr F32 STREAK_TIME = 0.035f;         // s
    constexpr F32 STREAK_TIME_MAX = 0.07f;      // s
    constexpr F32 STREAK_TIME_SMOOTHING = 0.5f; // s
    constexpr F32 DROP_WIDTH = 0.002f;          // m
    constexpr F32 DROP_SIZE_MAX = 1.6f;
    constexpr F32 DEFOCUS = 0.004f;             // m
    constexpr F32 LENGTH_SPREAD = 0.15f;
    // What a drop shows under a bright sky (exposed): its surroundings, about
    // a mid grey; of a streak's opacity, this share hides what lies behind it
    // (a drop is clear: through it the surroundings show, inverted).
    constexpr F32 DROP_LIGHT = 0.4f;
    constexpr F32 DROP_HIDE = 0.35f;

    // The rain's tint: the sky's light (its colour over its luminance), this much towards white.
    constexpr F32 TINT_WHITE = 0.6f;
    // ...and no channel of it farther than this from white (the spread scaled down, so the hue and
    // the luminance are kept). Drops and flakes are clear or white: a strongly coloured sky shows in
    // them only faintly. Over the luminance, a blue ambient's blue is large (blue weighs 0.07 in it):
    // a deep blue or purple sky took the drops to that colour itself (about twice as blue as grey).
    // A plain sky's tint stays inside this (a classic day of 0.27, 0.33, 0.44: 0.14 at most) and is
    // left as it was. (fix-round75)
    constexpr F32 TINT_MAX_SPREAD = 0.15f;
    // Under a cloudless sky the rain shows this share of what it does under a
    // fully clouded one; the sun through the drops fades by this share of the
    // cloud cover.
    constexpr F32 CLEAR_SKY_SHARE = 0.55f;
    constexpr F32 CLOUD_GLINT_FADE = 0.8f;
    // The sun's (moon's) light through a drop in front of it, times.
    constexpr F32 GLINT_SHARE = 0.05f;
    constexpr F32 MOON_GLINT_SHARE = 0.005f;
    // Under the "no post" tone mapping (plus::isNoPost) nothing scales the picture
    // down and every channel is clamped to 1 (the shader's GLINT_LIMIT, 2, is for an
    // exposed picture): the sun through a drop (its brightest channel, hue kept) is
    // held so that it and the drop's own light (at full opacity) stay under 1, as the
    // ground fog's sun glow is (atmosphere.cpp FOG_NO_POST_*). (fix-round48 2)
    constexpr F32 GLINT_NO_POST_CEILING = 1.f;
    constexpr F32 GLINT_NO_POST_MIN = 0.1f;

    // Heavy rain: large drops (heavy-rain). Within the strong level the drops grow with the rain:
    // PlusRain 3 is the full downpour; the parcel's weather (plus/slwx) grows them from its rate's
    // HEAVY_MM_FROM mm/h (the strong level's start: slwxtag.cpp rainLevelOfMm; there they are as
    // they were) to HEAVY_MM_FULL (30 mm/h: a cloudburst), smoothly. Weak and medium rain and snow
    // are left as they were. At the full downpour:
    //  - a drop is PlusRainDropScale times as wide and its streak PlusRainDropLength times as long
    //    (both 1: the strong rain as it was); in between by the heaviness;
    //  - fewer of them, so that the streaks cover about HEAVY_COVERAGE times what they did (a drop
    //    covers width times length as much, near or far: below a pixel it is as much brighter);
    //    the cells (bandFor) are sized for the full downpour's drops at any heaviness, and only
    //    their share holding a drop goes down: as the rain grows, drops go out, none jump;
    //  - a large drop shows more: its opacity HEAVY_OPACITY_GAIN more per unit of width scale above
    //    1 (at most HEAVY_OPACITY_MAX), the sun through it HEAVY_GLINT_GAIN more (still under the
    //    shader's GLINT_LIMIT and the no-post room below: the light's bounds are the old ones);
    //  - the splashes sqrt(width scale) times as large (the shader's splashScale) and the puddles'
    //    ripples HEAVY_RIPPLE_GAIN more tilted per unit of that above 1 (at most HEAVY_RIPPLE_MAX).
    // The fall speed and the splashes' rate stay the level's (a rate that followed the heaviness
    // would reshuffle the splashes while the parcel's weather blends).
    constexpr F32 HEAVY_MM_FROM = 10.f;         // mm/h
    constexpr F32 HEAVY_MM_FULL = 30.f;         // mm/h
    constexpr F32 DROP_SCALE_DEFAULT = 2.f;
    constexpr F32 DROP_SCALE_MAX = 4.f;
    constexpr F32 DROP_LENGTH_DEFAULT = 1.4f;
    constexpr F32 DROP_LENGTH_MAX = 2.5f;
    constexpr F32 HEAVY_COVERAGE = 1.15f;
    constexpr F32 HEAVY_OPACITY_GAIN = 0.15f;
    constexpr F32 HEAVY_OPACITY_MAX = 0.9f;
    constexpr F32 HEAVY_GLINT_GAIN = 0.25f;
    constexpr F32 HEAVY_RIPPLE_GAIN = 0.5f;
    constexpr F32 HEAVY_RIPPLE_MAX = 0.6f;

    // Splashes: within this range (m), this bright; the far haze builds up over this depth (m).
    constexpr F32 SPLASH_RANGE = 30.f;
    constexpr F32 SPLASH_BRIGHTNESS = 0.7f;
    constexpr F32 VEIL_DISTANCE = 120.f;

    S32 levelIndex(S32 level)
    {
        return llclamp(level, (S32)LEVEL_OFF, (S32)LEVEL_COUNT - 1);
    }

    // What the drops (flakes, splashes, the far haze) take of the sky's colour: a factor per channel
    // of luminance 1, TINT_WHITE towards white, no channel farther than TINT_MAX_SPREAD from 1.
    LLColor3 rainTint(const LLColor3& ambient, F32 luma)
    {
        LLColor3 tint(1.f, 1.f, 1.f);
        if (luma <= 1e-4f)
        {
            return tint;
        }
        F32 spread = 0.f;
        for (U32 c = 0; c < 3; ++c)
        {
            tint.mV[c] = lerp(ambient.mV[c] / luma, 1.f, TINT_WHITE);
            spread = llmax(spread, fabsf(tint.mV[c] - 1.f));
        }
        if (spread > TINT_MAX_SPREAD)
        {
            // The channels' differences from 1 sum to 0 weighted by luminance; scaled alike, they still do.
            const F32 scale = TINT_MAX_SPREAD / spread;
            for (U32 c = 0; c < 3; ++c)
            {
                tint.mV[c] = 1.f + (tint.mV[c] - 1.f) * scale;
            }
        }
        return tint;
    }

    // The rain's setting, and the snow's: while it snows (PlusSnow above 0)
    // there is no rain, whatever PlusRain says.
    // The parcel's weather (plus/slwx) stands in only while both settings are 0, so
    // the viewer's own weather wins; effective() is false then.
    S32 snowSetting()
    {
        static LLCachedControl<S32> level(gSavedSettings, SETTING_SNOW, LEVEL_OFF);
        plus::slwx::Effective parcel;
        if (plus::slwx::effective(parcel))
        {
            return levelIndex(parcel.snowLevel);
        }
        return levelIndex(level);
    }

    S32 currentLevel()
    {
        static LLCachedControl<S32> level(gSavedSettings, SETTING_LEVEL, LEVEL_OFF);
        if (snowSetting() > LEVEL_OFF)
        {
            return LEVEL_OFF;
        }
        plus::slwx::Effective parcel;
        if (plus::slwx::effective(parcel))
        {
            return levelIndex(parcel.rainLevel);
        }
        return levelIndex(level);
    }

    // The level of whatever falls (rain or snow), and its table.
    S32 fallingLevel()
    {
        return snowSetting() > LEVEL_OFF ? snowSetting() : currentLevel();
    }

    const Strength& fallingStrength(S32 level)
    {
        return snowSetting() > LEVEL_OFF ? SNOW_STRENGTHS[level] : STRENGTHS[level];
    }

    // How heavy the rain is within the strong level, 0..1 (Heavy rain above): 1 for PlusRain 3,
    // the parcel's rate between HEAVY_MM_FROM and HEAVY_MM_FULL, 0 below the strong level and in snow.
    F32 heaviness()
    {
        if (currentLevel() != LEVEL_STRONG)
        {
            return 0.f;
        }
        plus::slwx::Effective parcel;
        if (!plus::slwx::effective(parcel))
        {
            return 1.f;     // the viewer's own downpour
        }
        const F32 x = llfinite(parcel.rainMm) ? llclamp((parcel.rainMm - HEAVY_MM_FROM) / (HEAVY_MM_FULL - HEAVY_MM_FROM), 0.f, 1.f) : 0.f;
        return x * x * (3.f - 2.f * x);
    }

    // What the heaviness makes of the drops: times the level's (all 1: as they were).
    struct DropLook
    {
        F32 width = 1.f;        // a drop's width
        F32 length = 1.f;       // its streak's length
        F32 count = 1.f;        // the share of the cells' drops shown
        F32 opacity = 1.f;
        F32 glint = 1.f;
        F32 splash = 1.f;       // the splashes' size
        F32 ripples = 1.f;      // the puddles' ripples' tilt
    };

    // A setting's scale: 1 to `most`; not a number: the default.
    F32 cleanScale(F32 value, F32 fallback, F32 most)
    {
        return llfinite(value) ? llclamp(value, 1.f, most) : fallback;
    }

    DropLook dropLook(F32 heavy)
    {
        static LLCachedControl<F32> scale(gSavedSettings, SETTING_DROP_SCALE, DROP_SCALE_DEFAULT);
        static LLCachedControl<F32> length(gSavedSettings, SETTING_DROP_LENGTH, DROP_LENGTH_DEFAULT);
        DropLook look;
        if (!(heavy > 0.f))
        {
            return look;
        }
        heavy = llmin(heavy, 1.f);
        look.width = lerp(1.f, cleanScale(scale, DROP_SCALE_DEFAULT, DROP_SCALE_MAX), heavy);
        look.length = lerp(1.f, cleanScale(length, DROP_LENGTH_DEFAULT, DROP_LENGTH_MAX), heavy);
        look.count = llmin(1.f, lerp(1.f, HEAVY_COVERAGE, heavy) / (look.width * look.length));
        look.opacity = 1.f + HEAVY_OPACITY_GAIN * (look.width - 1.f);
        look.glint = 1.f + HEAVY_GLINT_GAIN * (look.width - 1.f);
        look.splash = sqrtf(look.width);
        look.ripples = 1.f + HEAVY_RIPPLE_GAIN * (look.splash - 1.f);
        return look;
    }

    // The wind (m/s, world x east, y north).
    // steady: the tag's own speed, without the parcel's gusts (what must not jump with them).
    LLVector2 wind(bool steady = false)
    {
        static LLCachedControl<F32> speed(gSavedSettings, SETTING_WIND, 2.f);
        static LLCachedControl<F32> direction(gSavedSettings, SETTING_WIND_DIRECTION, 45.f);
        F32 s = llfinite((F32)speed) ? llclamp((F32)speed, 0.f, 40.f) : 0.f;
        F32 from = (llfinite((F32)direction) ? (F32)direction : 0.f) * DEG_TO_RAD;
        plus::slwx::Effective parcel;
        if (plus::slwx::effective(parcel))
        {
            s = llclamp(steady ? parcel.windBaseSpeed : parcel.windSpeed, 0.f, 40.f);
            from = parcel.windFromDeg * DEG_TO_RAD;
        }
        // Blowing from `from`: towards the opposite way.
        return LLVector2(-sinf(from) * s, -cosf(from) * s);
    }

    bool eavesOn()
    {
        static LLCachedControl<bool> eaves(gSavedSettings, SETTING_EAVES, false);
        return eaves;
    }

    F32 splashLimit()
    {
        static LLCachedControl<F32> limit(gSavedSettings, SETTING_SPLASHES, 60.f);
        return llfinite((F32)limit) ? llmax((F32)limit, 0.f) : 0.f;
    }

    bool backendReady()
    {
        return LLRHI::sRain && LLRHI::sRainStreaks;
    }

    // Whether the rain can be seen: the backend draws it and HDR is on, as
    // LLPipeline::renderFinalize() decides whether to call renderStreaks().
    bool rainShown()
    {
        static LLCachedControl<bool> has_hdr(gSavedSettings, "RenderHDREnabled", true);
        return backendReady() && gGLManager.mGLVersion > 4.05f && has_hdr();
    }

    // For plus::skinsss::addAvatarMarkUser: while the rain splashes, avatars are
    // told from the rest so that they get the crowns but not the rings (the
    // shader draws them on the stencil's avatar bit apart). fix-round77.
    bool avatarMarkWanted()
    {
        return rainShown() && currentLevel() > LEVEL_OFF && splashLimit() > 0.f;
    }

    // The sky's cloud cover, 0..1.
    F32 cloudCover()
    {
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        return sky ? llclamp(sky->getCloudShadow(), 0.f, 1.f) : 1.f;
    }

    // x modulo period, in [0, period).
    F64 wrapped(F64 x, F64 period)
    {
        const F64 r = fmod(x, period);
        return r < 0.0 ? r + period : r;
    }

    // ------------------------------------------------------------------
    // The rain's clock: advanced once a frame, held while a snapshot draws
    // (its tiles then show the same rain) and while the world is stopped
    // (plus/freeze: it goes on from there). The camera's motion plays no
    // part: the drops are the world's.

    struct Clock
    {
        F64 fallen = 0.0;               // m the rain has fallen (not wrapped: rain space's height grows with it)
        F64 time = 0.0;                 // s, wrapped at LLRHI::RAIN_TIME_PERIOD
        F32 streak_time = STREAK_TIME;  // s, following the frame time
        // Rain space's x, y are the world's plus slope times the world height
        // plus this (m): when the wind changes, it takes up the change at the
        // camera's height, so the drops around the camera stay where they are.
        bool has_slope = false;
        LLVector2 slope;
        F64 shift[2] = { 0.0, 0.0 };
    };
    Clock sClock;

    void advance(F32 fall_speed)
    {
        const F32 dt = llclamp(gFrameIntervalSeconds.value(), 0.f, 0.25f);
        sClock.fallen += (F64)(fall_speed * dt);
        sClock.time = wrapped(sClock.time + (F64)dt, LLRHI::RAIN_TIME_PERIOD);
        const F32 wanted = llclamp(dt, STREAK_TIME, STREAK_TIME_MAX);
        sClock.streak_time += (wanted - sClock.streak_time) * (1.f - expf(-dt / STREAK_TIME_SMOOTHING));
    }

    // The camera's place in rain space (global, 64-bit) for the rain's slope.
    LLVector3d rainSpace(const LLVector3d& camera, const LLVector2& slope)
    {
        Clock& c = sClock;
        if (!c.has_slope)
        {
            c.has_slope = true;
            c.slope = slope;
        }
        else if (slope != c.slope)
        {
            for (U32 i = 0; i < 2; ++i)
            {
                c.shift[i] += (F64)(c.slope.mV[i] - slope.mV[i]) * camera.mdV[VZ];
            }
            c.slope = slope;
        }
        return LLVector3d(camera.mdV[VX] + (F64)slope.mV[VX] * camera.mdV[VZ] + c.shift[0],
                          camera.mdV[VY] + (F64)slope.mV[VY] * camera.mdV[VZ] + c.shift[1],
                          camera.mdV[VZ] + c.fallen);
    }

    // How much farther apart the sheets stand for the rain's slope.
    F32 sheetScale(const LLVector2& slope)
    {
        const F32 stretch = sqrtf(1.f + slope.lengthSquared());
        return powf(2.f, ceilf(log2f(stretch) * SHEET_STEPS_PER_OCTAVE - 1e-4f) / SHEET_STEPS_PER_OCTAVE);
    }

    // The rain's sideways metres per metre of fall (world), for the overhead
    // map's upwind look: the wind's only (the map is the world's, not the camera's).
    LLVector2 rainSlope(bool steady = false)
    {
        const S32 level = fallingLevel();
        const bool snow = snowSetting() > LEVEL_OFF;
        const F32 fall = level > LEVEL_OFF ? fallingStrength(level).fall_speed : RESTING_FALL_SPEED;
        return wind(steady) * (snow ? SNOW_WIND_SHARE : 1.f) / fall;
    }

    // Band `index`'s sheets and cells for `density` drops per cubic metre and
    // streaks up to `longest` m, the sheets sheet_scale times farther apart
    // (slanted rain), and where the camera is among them (rain_camera: its
    // place in rain space, m). The cells depend on neither the wind nor the
    // view, so neither moves the drops.
    // Heavy rain (fix: review-heavy-rain 1): the cells are sized for the full
    // downpour's drops (longest, widest_drop), and of the drops they stand for
    // the share `count` (0..1: fewer, larger drops) is shown. A cell's drops
    // are held at BRIGHTNESS_LIMIT as the level's own cells (level_longest,
    // level_widest: the drops as they were) would hold them, scaled by the
    // larger cells' volume and by count: so in the far bands, where that limit
    // holds, the streaks still cover count times width times length what they
    // did (1.15 times in the downpour, as they were at its start), not what
    // the larger cells' limit would make of it. Only the cells' probability
    // and brightness follow count, so drops go out or come in, none move.
    // With the drops as they were (both the same, count 1) all is as before.
    struct BandCells
    {
        F32 width = 0.f;
        F32 height = 0.f;
        U32 spacing = 1;
        F32 volume = 0.f;       // m^3 of rain a cell stands for (spacing * width^2 * height)
    };

    BandCells bandCells(F32 band_near, F32 area, F32 longest, F32 widest_drop, F32 sheet_scale)
    {
        BandCells cells;
        const F32 spacing_wanted = SHEET_SPACING * band_near;
        // A cell is at least as high as the longest streak, so the cells the
        // shader tries up a sheet (from a streak's length below the ray) stay few.
        cells.height = llmax(longest * 1.05f, sqrtf(area * CELL_ASPECT));
        cells.width = llmax(area / cells.height, llmax(CELL_MIN_ANGLE * band_near, 4.f * widest_drop));
        // The sheets stand whole cells apart (so that they stay at the same
        // world places as the camera's cell changes); slanted rain spreads them.
        cells.spacing = (U32)llclamp(floorf(spacing_wanted * llmax(sheet_scale, 1.f) / cells.width + 0.5f), 1.f, 65536.f);
        cells.volume = (F32)cells.spacing * cells.width * cells.width * cells.height;
        return cells;
    }

    LLRHI::RainBand bandFor(U32 index, F32 density, F32 count, F32 longest, F32 widest_drop, F32 level_longest, F32 level_widest,
                            F32 sheet_scale, const LLVector3d& rain_camera)
    {
        LLRHI::RainBand band;
        const F32 band_near = FIRST_BAND * (F32)(1u << index);     // (not `near`: a Windows macro)
        // Drops per m^2 of a sheet: each family carries all the drops (the
        // shader cross-fades the two by the ray's direction).
        const F32 per_area = density * (SHEET_SPACING * band_near);     // (as before: the spacing wanted first)
        if (per_area <= 0.f)
        {
            return band;
        }
        const F32 area = CELL_FILL / per_area;
        const BandCells cells = bandCells(band_near, area, longest, widest_drop, sheet_scale);
        const BandCells level = bandCells(band_near, area, level_longest, level_widest, sheet_scale);
        const F32 width = cells.width;
        const F32 height = cells.height;
        const U32 spacing = cells.spacing;
        const F32 drops = density * (F32)spacing * width * width * height * count;     // the drops a cell stands for (as before, the same order)
        // The level's cells' limit, for these cells and the share shown (none above: as before).
        const F32 limit = BRIGHTNESS_LIMIT * count * (level.volume > 0.f ? cells.volume / level.volume : 1.f);
        const F32 shown = llmin(drops, limit);
        band.cell_width = width;
        band.cell_height = height;
        band.probability = llmin(shown, 1.f);
        band.brightness = llclamp(shown, 1.f, BRIGHTNESS_CAP);   // more drops than cells: fewer, brighter ones (up to the cap)
        band.plane_spacing = spacing;
        // The camera's cell (64-bit, wrapped to the shader's 32) and its place
        // in it, so the shader works in small numbers near the camera.
        const F64 cell[3] = { width, width, height };
        S64 index_of[3];
        for (U32 c = 0; c < 3; ++c)
        {
            const F64 at = rain_camera.mdV[c] / cell[c];
            index_of[c] = (S64)floor(at);
            band.base[c] = (U32)(U64)index_of[c];
            band.camera[c] = llclamp((F32)((at - (F64)index_of[c]) * cell[c]), 0.f, (F32)cell[c]);
        }
        // The first sheet at or after the base along x and y: sheets stand where
        // the cell index is a whole number of spacings.
        for (U32 c = 0; c < 2; ++c)
        {
            const S64 s = (S64)spacing;
            band.plane_offset[c] = (U32)(((-index_of[c]) % s + s) % s);
        }
        // The level sheets (the shader's snow only; fix-round82): z counted in
        // cell_width cells, the sheets every spacing of them, as along x and y.
        {
            const F64 at = rain_camera.mdV[VZ] / (F64)width;
            const S64 index = (S64)floor(at);
            const S64 s = (S64)spacing;
            band.level_base = (U32)(U64)index;
            band.level_camera = llclamp((F32)((at - (F64)index) * (F64)width), 0.f, width);
            band.level_offset = (U32)(((-index) % s + s) % s);
        }
        return band;
    }

    // ------------------------------------------------------------------
    // Umbrellas (PlusRainUmbrella): attachments of your own avatar whose
    // name holds one of PlusRainUmbrellaNames' words. Its canopy is taken
    // from the attachment's bounding box (with its linked parts): a dome as
    // wide as the box, its top at the box's top. Shielding the rain, splashes
    // on top, drips off the rim: llvkrain.cpp. Other avatars' attachments have
    // no names to find them by (and are not looked at).

    // A canopy must be at least this wide to count as open (m), at most this
    // wide (a huge attachment is no umbrella); its rim hangs this far below
    // its top (m); the rim's columns that drip, per rain level.
    constexpr F32 UMBRELLA_MIN_RADIUS = 0.3f;
    constexpr F32 UMBRELLA_MAX_RADIUS = 1.5f;
    constexpr F32 UMBRELLA_RIM_DROP = 0.12f;
    constexpr F32 UMBRELLA_DRIP[LEVEL_COUNT] = { 0.f, 0.3f, 0.5f, 0.7f };
    // The attachments are looked for again this often (s); their boxes are read every frame.
    constexpr F64 UMBRELLA_RECHECK_SECONDS = 1.0;

    // On the heap and never destroyed: a static vector would let go of the
    // objects in the static destructors, with no GL and no singletons left.
    // Emptied whenever no rain or snow is drawn, so none is held for long.
    std::vector<LLPointer<LLViewerObject>>& umbrellaObjects()
    {
        static auto* objects = new std::vector<LLPointer<LLViewerObject>>();
        return *objects;
    }

    F64 sUmbrellasChecked = -1.0e9;

    // Set at "quitting" (init()): nothing is made again after the map and the umbrellas were let go.
    bool sQuitting = false;

    void forgetUmbrellas()
    {
        umbrellaObjects().clear();
        sUmbrellasChecked = -1.0e9;
    }

    bool umbrellaOn()
    {
        static LLCachedControl<bool> on(gSavedSettings, SETTING_UMBRELLA, false);
        return on;
    }

    // Whether name holds one of the comma-separated words (case ignored).
    bool nameMatches(const std::string& name, const std::string& words)
    {
        if (name.empty())
        {
            return false;
        }
        std::string lower = name;
        LLStringUtil::toLower(lower);
        size_t from = 0;
        while (from <= words.size())
        {
            size_t comma = words.find(',', from);
            if (comma == std::string::npos)
            {
                comma = words.size();
            }
            std::string word = words.substr(from, comma - from);
            LLStringUtil::trim(word);
            LLStringUtil::toLower(word);
            if (!word.empty() && lower.find(word) != std::string::npos)
            {
                return true;
            }
            from = comma + 1;
        }
        return false;
    }

    void findUmbrellas()
    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        if (now - sUmbrellasChecked < UMBRELLA_RECHECK_SECONDS)
        {
            return;
        }
        sUmbrellasChecked = now;
        umbrellaObjects().clear();
        if (!isAgentAvatarValid())
        {
            return;
        }
        const std::string words = gSavedSettings.getString(SETTING_UMBRELLA_NAMES);
        for (const auto& point : gAgentAvatarp->mAttachmentPoints)
        {
            LLViewerJointAttachment* attachment = point.second;
            if (!attachment || attachment->getIsHUDAttachment())
            {
                continue;
            }
            for (LLPointer<LLViewerObject>& object : attachment->mAttachedObjects)
            {
                if (object.notNull() && !object->isDead() && umbrellaObjects().size() < LLRHI::RAIN_UMBRELLAS &&
                    nameMatches(object->getAttachmentItemName(), words))
                {
                    umbrellaObjects().push_back(object);
                }
            }
        }
    }

    // The canopies now, into pass.umbrellas (camera-relative world); and the
    // rim's drips for the level.
    void setUmbrellas(LLRHI::RainStreaks& pass, const LLVector3& camera_agent, F32 drip_share)
    {
        if (!umbrellaOn())
        {
            forgetUmbrellas();
            return;
        }
        findUmbrellas();
        U32 count = 0;
        for (LLPointer<LLViewerObject>& object : umbrellaObjects())
        {
            if (object.isNull() || object->isDead())
            {
                continue;
            }
            LLVector3 low;
            LLVector3 high;
            bool any = false;
            auto add = [&](LLViewerObject* part)
            {
                const LLBBox box = part->getBoundingBoxAgent().getAxisAligned();
                const LLVector3 box_low = box.getMinAgent();
                const LLVector3 box_high = box.getMaxAgent();
                if (!any)
                {
                    low = box_low;
                    high = box_high;
                    any = true;
                    return;
                }
                for (U32 c = 0; c < 3; ++c)
                {
                    low.mV[c] = llmin(low.mV[c], box_low.mV[c]);
                    high.mV[c] = llmax(high.mV[c], box_high.mV[c]);
                }
            };
            add(object);
            for (const LLPointer<LLViewerObject>& child : object->getChildren())
            {
                if (child.notNull() && !child->isDead())
                {
                    add(child);
                }
            }
            const F32 radius = 0.5f * llmax(high.mV[VX] - low.mV[VX], high.mV[VY] - low.mV[VY]);
            if (!any || !llfinite(radius) || radius < UMBRELLA_MIN_RADIUS || radius > UMBRELLA_MAX_RADIUS)
            {
                continue;
            }
            LLRHI::RainUmbrella& out = pass.umbrellas[count++];
            out.top[0] = 0.5f * (low.mV[VX] + high.mV[VX]) - camera_agent.mV[VX];
            out.top[1] = 0.5f * (low.mV[VY] + high.mV[VY]) - camera_agent.mV[VY];
            out.top[2] = high.mV[VZ] - camera_agent.mV[VZ];
            out.radius = radius;
        }
        pass.umbrella_rim_drop = UMBRELLA_RIM_DROP;
        pass.umbrella_drip = count ? drip_share : 0.f;
    }

    void init()
    {
        // Settings: app_settings/settings_plus.xml (PlusRain, PlusRainWind,
        // PlusRainWindDirection, PlusRainSplashes). Photo tools:
        // app_settings/plus/phototools/catalog.xml (tab "light": the Sky & Weather tab).
        // Drawn from the <plus> hooks in display() (the overhead map) and
        // LLPipeline::renderFinalize() (the rain).

        // The avatar mark (plus/skinsss) comes and goes with the rain's
        // splashes; asked once idle, when the cached controls have the new values.
        plus::skinsss::addAvatarMarkUser(&avatarMarkWanted);
        for (const char* setting : { SETTING_LEVEL, SETTING_SNOW, SETTING_SPLASHES, "RenderHDREnabled" })
        {
            if (LLControlVariable* control = gSavedSettings.getControl(setting))
            {
                control->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&)
                                              { doOnIdleOneTime(&plus::skinsss::markWantChanged); });
            }
        }

        // The app quitting (LLApp's "quitting", main thread, before cleanup()): the overhead map's
        // target is freed while the renderer is still there (the map itself is never destroyed, so
        // nothing else would free it), and the umbrellas' objects are let go before the object list.
        LLEventPumps::instance().obtain("LLApp").listen("PlusRain", [](const LLSD& event)
        {
            if (event["status"].asString() == "quitting" && on_main_thread())
            {
                sQuitting = true;
                plus::rain::overhead::release();
                forgetUmbrellas();
            }
            return false;
        });
    }

    plus::FeatureRegistration sRegistration({ "rain", &init, {} });
}

namespace plus::rain
{
    void renderOverhead(LLCamera& camera)
    {
        // Called every frame (but a snapshot's): renderStreaks() is not with HDR off, so the
        // umbrellas are let go here too while none is wanted (empty already: a clear() and a
        // store). (fix-round67 3)
        if (!rainShown() || fallingLevel() == LEVEL_OFF || !umbrellaOn())
        {
            forgetUmbrellas();
        }
        // Wanted by the rain and the wet look (also while what the rain wetted
        // dries: plus/wetlook), while the backend can read it.
        const bool readers = !sQuitting && LLRHI::sRain && ((rainShown() && fallingLevel() > LEVEL_OFF) || plus::wetlook::wantsOverhead());
        if (!readers)
        {
            overhead::release();
            return;
        }
        overhead::update(camera);
    }

    void renderStreaks()
    {
        const S32 level = fallingLevel();
        if (level == LEVEL_OFF || !rainShown() || sQuitting)
        {
            forgetUmbrellas();      // (no object held while nothing falls)
            return;
        }
        const bool snow = snowSetting() > LEVEL_OFF;
        if (gCubeSnapshot || gPipeline.mRT != &gPipeline.mMainRT || LLViewerCamera::getInstance()->cameraUnderWater())
        {
            return;
        }
        LLRenderTarget& screen = gPipeline.mRT->screen;
        LLRenderTarget& gbuffer = gPipeline.mRT->deferredScreen;
        if (!screen.isComplete() || screen.isBoundInStack() || !gPipeline.mExposureMap.isComplete() || !gbuffer.getDepth())
        {
            return;
        }

        const Strength& strength = fallingStrength(level);
        // Heavy rain: the drops now, and the full downpour's (the cells are sized for those, so
        // that they stay put as the heaviness changes). Snow, weak and medium rain: 1 (as they were).
        const DropLook look = snow ? DropLook() : dropLook(heaviness());
        const DropLook full = (snow || level != LEVEL_STRONG) ? DropLook() : dropLook(1.f);
        // The 3D scene's matrices (renderDeferredLighting keeps them; HUDs may have loaded theirs since).
        const glm::mat4 modelview = get_last_modelview();
        const glm::mat4 projection = get_last_projection();
        const glm::mat4 eye_to_agent = glm::inverse(modelview);
        const LLVector3 camera_agent(eye_to_agent[3][0], eye_to_agent[3][1], eye_to_agent[3][2]);
        const LLVector3d camera_global = gAgent.getPosGlobalFromAgent(camera_agent);
        if (!gSnapshot && !plus::freeze::isFrozen())
        {
            advance(strength.fall_speed);
        }

        LLRHI::RainStreaks pass;
        pass.depth_texture = gbuffer.getDepth();
        pass.exposure_texture = gPipeline.mExposureMap.getTexture();
        pass.exposure_scale = plus::exposureScaleForPasses();  // 0 under plus::isNoPost: the exposure taken as 1 (fix-round48)
        memcpy(pass.inverse_projection, glm::value_ptr(glm::inverse(projection)), sizeof(pass.inverse_projection));
        // Eye -> world axes with the camera at the origin; the map from there.
        glm::mat4 eye_to_world = eye_to_agent;
        eye_to_world[3] = glm::vec4(0.f, 0.f, 0.f, 1.f);
        memcpy(pass.eye_to_world, glm::value_ptr(eye_to_world), sizeof(pass.eye_to_world));
        const glm::mat4 world_to_agent = glm::translate(glm::mat4(1.f), glm::vec3(camera_agent.mV[VX], camera_agent.mV[VY], camera_agent.mV[VZ]));
        const LLVector2 slope = rainSlope();     // the wind over the fall speed
        pass.overhead = overhead::lookup(world_to_agent, slope);

        // Rain space: a drop falling with the wind keeps its place there. The
        // camera's place in it (global, 64-bit) gives each band's cell.
        pass.shear[0] = slope.mV[VX];
        pass.shear[1] = slope.mV[VY];
        const LLVector3d rain_camera = rainSpace(camera_global, slope);
        pass.camera_world[0] = (F32)wrapped(camera_global.mdV[VX], LLRHI::RAIN_WORLD_PERIOD);
        pass.camera_world[1] = (F32)wrapped(camera_global.mdV[VY], LLRHI::RAIN_WORLD_PERIOD);
        pass.time = (F32)sClock.time;

        // The light: the sky's tint, more rain to see under clouds, the sun through the drops.
        const F32 clouds = cloudCover();
        const F32 visibility = snow ? 1.f : lerp(CLEAR_SKY_SHARE, 1.f, clouds);
        LLSettingsSky::ptr_t sky = LLEnvironment::instance().getCurrentSky();
        // Under a classic sky the scene takes the sky's ambient and the sun's colour as they are (sRGB,
        // not made linear; the sun 1.35 times), and so does the rain, as the ground fog and the light
        // shafts do: made linear, a dim classic sky showed the drops far darker than the scene around
        // them (no exposure there to make up for it). (fix-round67 7a)
        const bool classic = plus::isClassicSky(sky.get());
        const bool no_post = plus::isNoPost(sky.get());
        const LLColor3 ambient = sky ? plus::skyAmbient(sky.get(), classic) : LLColor3(0.5f, 0.5f, 0.5f);
        const F32 ambient_luma = ambient.mV[VRED] * 0.2126f + ambient.mV[VGREEN] * 0.7152f + ambient.mV[VBLUE] * 0.0722f;
        const F32 drop_light = snow ? SNOW_LIGHT : DROP_LIGHT;
        const LLColor3 tint = rainTint(ambient, ambient_luma);
        for (U32 c = 0; c < 3; ++c)
        {
            pass.drop_light[c] = drop_light * tint.mV[c];
            pass.ambient[c] = ambient.mV[c];
        }
        if (no_post)
        {
            // Under no post every channel is clamped to 1: a tinted sky's strongest channel took a flake
            // (SNOW_LIGHT 0.85 times a tint of 1.4 and more) past 1. Its brightest channel is held at a
            // white flake's, the hue kept. (fix-round67 7d)
            // fix-round68 5c: a drop of rain too: 0.4 is white's, but the tint of a strongly coloured
            // sky (a blue ambient: blue / luminance, 10 and more) took it past 1 as well. Held at the
            // same 0.85 (a tint up to about 3.8 is left as it was).
            // fix-round75: the tint is now at most 1 + TINT_MAX_SPREAD (1.15): a drop's 0.46 stays
            // under it; a flake's 0.98 is still held here. Kept as the guard for both.
            const F32 ceiling = snow ? drop_light : SNOW_LIGHT;
            const F32 brightest = llmax(pass.drop_light[0], pass.drop_light[1], pass.drop_light[2]);
            if (brightest > ceiling)
            {
                for (U32 c = 0; c < 3; ++c)
                {
                    pass.drop_light[c] *= ceiling / brightest;
                }
            }
        }
        pass.drop_opacity = (look.opacity > 1.f ? llmin(strength.opacity * look.opacity, HEAVY_OPACITY_MAX) : strength.opacity) * visibility;
        pass.drop_hide = snow ? SNOW_HIDE : DROP_HIDE;
        const bool sun = LLEnvironment::instance().getIsSunUp();
        const LLColor4& diffuse = sun ? gPipeline.mSunDiffuse : gPipeline.mMoonDiffuse;
        const LLColor3 light = classic ? LLColor3(diffuse.mV[VRED], diffuse.mV[VGREEN], diffuse.mV[VBLUE]) * plus::CLASSIC_SUNLIGHT_SCALE
                                       : linearColor3(diffuse);
        F32 glint = (sun ? (snow ? SNOW_GLINT_SHARE : GLINT_SHARE) : MOON_GLINT_SHARE) * (1.f - CLOUD_GLINT_FADE * clouds) * visibility * look.glint;
        if (no_post)
        {
            // The shader's lobe peaks at 1 (towards the sun) and the drop shows at most drop_light times its opacity.
            const F32 brightest_light = llmax(light.mV[VRED], light.mV[VGREEN], light.mV[VBLUE]);
            const F32 drop_shown = pass.drop_opacity * llmax(pass.drop_light[0], pass.drop_light[1], pass.drop_light[2]);
            const F32 room = llmax(GLINT_NO_POST_CEILING - drop_shown, GLINT_NO_POST_MIN);
            if (brightest_light * glint > room)
            {
                glint = room / brightest_light;
            }
        }
        LLVector3 towards(sun ? gPipeline.mSunDir.mV : gPipeline.mMoonDir.mV);
        if (towards.normalize() <= 0.f)
        {
            towards.set(0.f, 0.f, 1.f);
        }
        for (U32 c = 0; c < 3; ++c)
        {
            pass.glint_color[c] = light.mV[c] * glint;
            pass.sun_direction[c] = towards.mV[c];
        }

        // The streaks: a drop's fall over the shutter, straight up from it in rain space.
        pass.streak_length = strength.fall_speed * sClock.streak_time * look.length;
        const F32 drop_width = snow ? SNOW_DROP_WIDTH : DROP_WIDTH;
        pass.drop_width = drop_width * look.width;
        pass.defocus = snow ? SNOW_DEFOCUS : DEFOCUS;
        pass.sway = snow ? SNOW_SWAY : 0.f;
        // Snow: soft round flakes, not streaks (fix-round79); rain: 0, its streaks and glint as they were.
        pass.flake_stretch = snow ? SNOW_FLAKE_STRETCH : 0.f;
        pass.flake_near_fade = snow ? SNOW_NEAR_FADE : 0.f;
        pass.glint_limit = snow ? SNOW_GLINT_LIMIT : 0.f;
        // Drips off the roofs' edges, from the overhead map: only for rain.
        pass.eave_density = (!snow && eavesOn()) ? EAVE_DENSITY[level] : 0.f;
        pass.eave_step = EAVE_STEP;
        pass.eave_range = EAVE_RANGE;
        // Your umbrella: the rain stops at its canopy (snow too), the rim drips (rain only).
        setUmbrellas(pass, camera_agent, snow ? 0.f : UMBRELLA_DRIP[level]);
        // One pixel's height seen from the camera (projection[1][1] = 1 / tan(fov / 2)).
        pass.pixel_angle = projection[1][1] > 0.f ? 2.f / (projection[1][1] * (F32)llmax(1u, screen.getHeight())) : 0.f;
        pass.first_band = FIRST_BAND;
        pass.band_overlap = BAND_OVERLAP;
        const F32 longest = strength.fall_speed * STREAK_TIME_MAX * (1.f + LENGTH_SPREAD) * full.length;
        const F32 widest_drop = drop_width * full.width * DROP_SIZE_MAX;     // (the sway is the shader's reach, not the cells')
        // The level's own drops (as they were), whose cells' limit the larger cells keep.
        const F32 level_longest = strength.fall_speed * STREAK_TIME_MAX * (1.f + LENGTH_SPREAD);
        const F32 level_widest = drop_width * DROP_SIZE_MAX;
        // (the sheets' spacing steps with the slope: it follows the steady wind, not each gust)
        const F32 sheet_scale = sheetScale(rainSlope(true));
        for (U32 i = 0; i < LLRHI::RAIN_BANDS; ++i)
        {
            pass.bands[i] = bandFor(i, strength.density, look.count, longest, widest_drop, level_longest, level_widest, sheet_scale, rain_camera);
        }
        pass.band_count = LLRHI::RAIN_BANDS;

        // Splashes: the level's, at most the setting's; per cell and second,
        // rounded so that times the clock's period it is whole.
        const F32 per_area = snow ? 0.f : llmin(strength.splashes, splashLimit());
        const F32 per_cell = per_area * LLRHI::RAIN_SPLASH_CELL * LLRHI::RAIN_SPLASH_CELL;
        pass.splash_cell_rate = floorf(per_cell * LLRHI::RAIN_TIME_PERIOD + 0.5f) / LLRHI::RAIN_TIME_PERIOD;
        pass.splash_range = SPLASH_RANGE;
        pass.splash_brightness = SPLASH_BRIGHTNESS;
        pass.splash_scale = look.splash;
        pass.veil = strength.veil * visibility;
        pass.veil_distance = VEIL_DISTANCE;
        // Avatars apart (no rings on them) only when this frame's G-buffer pass
        // wrote their mark; until it does (a frame or two), as before.
        pass.avatars_marked = pass.splash_cell_rate > 0.f && plus::skinsss::avatarsMarkedThisFrame();
        pass.width = screen.getWidth();
        pass.height = screen.getHeight();

        LLRHI::gpuMark("rain");     // the GPU ms line's segment; the tone mapping's is marked again below
        screen.bindTarget();
        LLRHI::sRainStreaks(pass);
        screen.flush();
        LLRHI::gpuMark("tonemap");
    }

    LLRHI::RainOverhead overheadForEye(const glm::mat4& modelview)
    {
        if (!LLRHI::sRain)
        {
            return LLRHI::RainOverhead();
        }
        return overhead::lookup(glm::inverse(modelview), rainSlope());
    }

    S32 rainLevel()
    {
        return rainShown() ? currentLevel() : LEVEL_OFF;
    }

    S32 snowLevel()
    {
        return rainShown() ? snowSetting() : LEVEL_OFF;
    }

    bool wetLookRipples(F32& tilt, F32& rate)
    {
        const S32 level = currentLevel();
        if (level == LEVEL_OFF || !rainShown())
        {
            return false;
        }
        // Heavy rain's large drops stir the puddles more.
        const F32 ripples = dropLook(heaviness()).ripples;
        tilt = ripples > 1.f ? llmin(STRENGTHS[level].ripples * ripples, HEAVY_RIPPLE_MAX) : STRENGTHS[level].ripples;
        rate = STRENGTHS[level].ripple_rate;
        return true;
    }
}
