/**
 * @file lookfx.cpp
 * @brief Screen effects of the look pass: the settings and the presets.
 *
 * The effects are drawn by the look pass itself (llvulkan/llvklook.cpp, after
 * the LUT), so they cost no extra pass; this file only decides the amounts.
 *
 * Settings (settings_plus.xml):
 *   PlusFxPreset        id of a preset below; empty: the sliders are used
 *   PlusFxStrength      0..100 %, scales every amount (preset or sliders)
 *   PlusFxGrain, PlusFxVignette, PlusFxChromatic, PlusFxHalation, PlusFxDust,
 *   PlusFxScanlines     0..100 %, the sliders (not used while a preset is chosen)
 *   PlusFxSoftGlow, PlusFxSoftGlowSize, PlusFxSoftGlowLift  0..100 %, the soft glow (research/286); the amount
 *   follows PlusFxStrength, with a preset too.
 *   PlusFxClarity, PlusFxTexture  0..100 %, the sliders of the local contrast (research/249):
 *                       large areas and fine detail. Used with a preset too (research/275 L219); a preset
 *                       chosen through chooseFxPreset (no UI calls it yet) sets them to its values first
 *   PlusFxSnapshotOnly  draw the effects in snapshots only (their preview too)
 *   PlusPaintStyle      cartoon, watercolor, sketch or oil (research/142); empty: none
 *   PlusPaintStrength   0..100 %, how much of the painting is mixed in
 *   PlusPaintSnapshotOnly  paint in snapshots only
 *   PlusMiniature       the miniature (tilt-shift, research/236): a sharp band, blur outside it
 *   PlusMiniatureBand, PlusMiniatureWidth, PlusMiniatureTilt  the band: centre -1..1 (bottom..top),
 *                       height as a share of the picture's, slant in degrees
 *   PlusMiniatureBlur, PlusMiniatureSaturation  0..100 %: the blur, the saturation and contrast added
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

#include "llviewerprecompiledheaders.h"

#include "lookfx.h"

#include "presets/presets.h"

#include "lltimer.h"
#include "llviewercontrol.h"

#include <cmath>
#include <cstring>
#include <initializer_list>
#include <utility>

namespace
{
    constexpr char SETTING_FX_PRESET[]  = "PlusFxPreset";
    constexpr char SETTING_FX_CLARITY[] = "PlusFxClarity";
    constexpr char SETTING_FX_TEXTURE[] = "PlusFxTexture";
    constexpr U32 MAX_PERCENT = 100;
    // The moving noise is seeded from the time; wrapping keeps the F32 exact enough.
    constexpr F64 TIME_WRAP_SECONDS = 3600.0;
    // The miniature's light darkening of the corners, added to PlusFxVignette's (research/236 §2.3).
    constexpr F32 MINIATURE_VIGNETTE = 0.15f;
    constexpr F32 MAX_MINIATURE_TILT = 90.f;   // degrees either way
    constexpr U32 MINIATURE_SCREEN_TAPS = 32;    // the most taps of the miniature's blur on the screen
    constexpr U32 MINIATURE_SNAPSHOT_TAPS = 64;  // and while a snapshot is taken
    constexpr U32 CLARITY_SCREEN_TAPS = 24;      // the taps of clarity's disc on the screen
    constexpr U32 CLARITY_SNAPSHOT_TAPS = 48;    // and while a snapshot is taken
    constexpr U32 SOFT_GLOW_SCREEN_TAPS = 32;    // the taps of the soft glow's blur on the screen (research/286)
    constexpr U32 SOFT_GLOW_SNAPSHOT_TAPS = 96;  // and while a snapshot is taken

    LLRHI::LookFx amounts(F32 grain, F32 vignette, F32 chromatic, F32 halation, F32 dust, F32 scanlines)
    {
        LLRHI::LookFx fx;
        fx.grain = grain;
        fx.vignette = vignette;
        fx.chromatic = chromatic;
        fx.halation = halation;
        fx.dust = dust;
        fx.scanlines = scanlines;
        return fx;
    }

    F32 percent(U32 value)
    {
        return (F32)llmin(value, MAX_PERCENT) / (F32)MAX_PERCENT;
    }

    // The amounts PlusFxStrength scales (a preset's or the sliders'; not the painting nor the miniature).
    void scaleAmounts(LLRHI::LookFx& fx, F32 scale)
    {
        for (F32* amount : { &fx.grain, &fx.vignette, &fx.chromatic, &fx.halation, &fx.dust, &fx.scanlines, &fx.clarity, &fx.texture, &fx.soft_glow })
        {
            *amount *= scale;
        }
    }
}

namespace plus::look
{
    const std::vector<FxPreset>& fxPresets()
    {
        //                                                   grain vign. chrom. halat. dust  scan.
        static const std::vector<FxPreset> sPresets = {
            { "film",       "Film",         "フィルム",         amounts(0.35f, 0.30f, 0.00f, 0.25f, 0.00f, 0.00f) },
            { "polaroid",   "Instant photo", "インスタント写真", amounts(0.20f, 0.35f, 0.15f, 0.15f, 0.00f, 0.00f) },
            { "toy-camera", "Toy camera",   "トイカメラ",       amounts(0.15f, 0.75f, 0.55f, 0.10f, 0.00f, 0.00f) },
            { "old-cinema", "Old cinema",   "古い映画",         amounts(0.60f, 0.50f, 0.00f, 0.10f, 0.80f, 0.00f) },
            { "vhs",        "VHS",          "ビデオテープ",     amounts(0.30f, 0.15f, 0.50f, 0.00f, 0.00f, 0.60f) },
            { "glow",       "Glow",         "にじむ光",         amounts(0.00f, 0.20f, 0.00f, 0.70f, 0.00f, 0.00f) },
        };
        return sPresets;
    }

    const std::vector<PaintStyle>& paintStyles()
    {
        static const std::vector<PaintStyle> sStyles = {
            { "cartoon",    "Cartoon",      "カートゥーン", LLRHI::LookFx::PAINT_CARTOON },
            { "watercolor", "Watercolor",   "水彩画",       LLRHI::LookFx::PAINT_WATERCOLOR },
            { "sketch",     "Pencil sketch", "鉛筆画",      LLRHI::LookFx::PAINT_SKETCH },
            { "oil",        "Oil painting", "油絵",         LLRHI::LookFx::PAINT_OIL },
        };
        return sStyles;
    }

    namespace
    {
        const FxPreset* findFxPreset(const std::string& id)
        {
            if (!id.empty())
            {
                for (const FxPreset& p : fxPresets())
                {
                    if (id == p.id)
                    {
                        return &p;
                    }
                }
            }
            return nullptr;
        }
    }

    void chooseFxPreset(const std::string& id)
    {
        gSavedSettings.setString(SETTING_FX_PRESET, id);
        const FxPreset* chosen = findFxPreset(id);
        if (!chosen)
        {
            return;     // none (the sliders alone) or an unknown id: the sliders stay as they are
        }
        for (const auto& [key, amount] : { std::make_pair(SETTING_FX_CLARITY, chosen->amounts.clarity),
                                           std::make_pair(SETTING_FX_TEXTURE, chosen->amounts.texture) })
        {
            if (gSavedSettings.controlExists(key))
            {
                gSavedSettings.setU32(key, (U32)ll_round(llclamp(amount, 0.f, 1.f) * (F32)MAX_PERCENT));
            }
        }
    }

    bool currentFx(LLRHI::LookFx& fx)
    {
        static LLCachedControl<std::string> preset(gSavedSettings, SETTING_FX_PRESET, "");
        static LLCachedControl<U32> strength(gSavedSettings, "PlusFxStrength", MAX_PERCENT);
        static LLCachedControl<U32> grain(gSavedSettings, "PlusFxGrain", 0);
        static LLCachedControl<U32> vignette(gSavedSettings, "PlusFxVignette", 0);
        static LLCachedControl<U32> chromatic(gSavedSettings, "PlusFxChromatic", 0);
        static LLCachedControl<U32> halation(gSavedSettings, "PlusFxHalation", 0);
        static LLCachedControl<U32> dust(gSavedSettings, "PlusFxDust", 0);
        static LLCachedControl<U32> scanlines(gSavedSettings, "PlusFxScanlines", 0);
        static LLCachedControl<U32> clarity(gSavedSettings, SETTING_FX_CLARITY, 0);
        static LLCachedControl<U32> texture(gSavedSettings, SETTING_FX_TEXTURE, 0);
        static LLCachedControl<U32> soft_glow(gSavedSettings, "PlusFxSoftGlow", 0);
        static LLCachedControl<U32> soft_glow_size(gSavedSettings, "PlusFxSoftGlowSize", 50);
        static LLCachedControl<U32> soft_glow_lift(gSavedSettings, "PlusFxSoftGlowLift", 30);
        static LLCachedControl<bool> snapshot_only(gSavedSettings, "PlusFxSnapshotOnly", false);
        static LLCachedControl<std::string> paint_style(gSavedSettings, "PlusPaintStyle", "");
        static LLCachedControl<U32> paint_strength(gSavedSettings, "PlusPaintStrength", MAX_PERCENT);
        static LLCachedControl<bool> paint_snapshot_only(gSavedSettings, "PlusPaintSnapshotOnly", false);
        static LLCachedControl<bool> miniature(gSavedSettings, "PlusMiniature", false);
        static LLCachedControl<F32> miniature_band(gSavedSettings, "PlusMiniatureBand", 0.f);
        static LLCachedControl<F32> miniature_width(gSavedSettings, "PlusMiniatureWidth", 0.25f);
        static LLCachedControl<U32> miniature_blur(gSavedSettings, "PlusMiniatureBlur", 60);
        static LLCachedControl<F32> miniature_tilt(gSavedSettings, "PlusMiniatureTilt", 0.f);
        static LLCachedControl<U32> miniature_saturation(gSavedSettings, "PlusMiniatureSaturation", 40);

        fx = LLRHI::LookFx();
        const bool snapshot = plus::presets::snapshotInProgress();

        if (!(snapshot_only && !snapshot))
        {
            if (const FxPreset* chosen = findFxPreset(preset()))
            {
                fx = chosen->amounts;
            }
            else
            {
                fx = amounts(percent(grain), percent(vignette), percent(chromatic), percent(halation), percent(dust), percent(scanlines));
            }
            // Clarity and texture are their sliders' with a preset too (research/275 L219). A preset chosen through
            // chooseFxPreset (no UI calls it yet) puts its values on the sliders first, so what they are moved to
            // afterwards is the preset's value and the hand's on top of it. The six above stay the preset's.
            fx.clarity = percent(clarity);
            fx.texture = percent(texture);
            // The soft glow is its sliders' with a preset too (research/286); only its amount follows PlusFxStrength.
            fx.soft_glow = percent(soft_glow);
            fx.soft_glow_size = percent(soft_glow_size);
            fx.soft_glow_lift = percent(soft_glow_lift);

            scaleAmounts(fx, percent(strength));
            fx.clarity_taps = snapshot ? CLARITY_SNAPSHOT_TAPS : CLARITY_SCREEN_TAPS;
            fx.soft_glow_taps = snapshot ? SOFT_GLOW_SNAPSHOT_TAPS : SOFT_GLOW_SCREEN_TAPS;
        }

        if (!(paint_snapshot_only && !snapshot))
        {
            const std::string& style = paint_style();
            if (!style.empty())
            {
                for (const PaintStyle& p : paintStyles())
                {
                    if (style == p.id)
                    {
                        fx.paint_style = p.style;
                        fx.paint = percent(paint_strength);
                        break;
                    }
                }
            }
        }
        // The miniature (research/236): on the screen and in snapshots alike, not scaled by PlusFxStrength.
        if (miniature)
        {
            fx.miniature_blur = percent(miniature_blur);
            fx.miniature_saturation = percent(miniature_saturation);
            fx.miniature_band = llclamp((F32)miniature_band, -1.f, 1.f);
            fx.miniature_width = llclamp((F32)miniature_width, 0.f, 1.f);
            fx.miniature_tilt = llclamp((F32)miniature_tilt, -MAX_MINIATURE_TILT, MAX_MINIATURE_TILT);
            // A snapshot (one frame, often larger than the screen) may take more taps; the screen keeps to the cost of every frame.
            fx.miniature_max_taps = snapshot ? MINIATURE_SNAPSHOT_TAPS : MINIATURE_SCREEN_TAPS;
            if (fx.miniature())
            {
                fx.vignette = llmin(fx.vignette + MINIATURE_VIGNETTE, 1.f);
            }
        }
        // The moving noise (grain, dust, the scanlines' wobble) holds still
        // through a snapshot: its tiles, and the frames drawn again for OIT,
        // get the same noise (fix-round6 4).
        static U32 sTimedSnapshot = 0;      // the snapshot sSnapshotTime is for (snapshotSerial)
        static F32 sSnapshotTime = 0.f;
        const F32 now = (F32)std::fmod(LLTimer::getTotalSeconds(), TIME_WRAP_SECONDS);
        if (snapshot && sTimedSnapshot != plus::presets::snapshotSerial())
        {
            sTimedSnapshot = plus::presets::snapshotSerial();
            sSnapshotTime = now;
        }
        fx.time = snapshot ? sSnapshotTime : now;
        return fx.any();
    }
}
