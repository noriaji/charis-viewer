/**
 * @file photoscreen_items.h
 * @brief Where the photo screen shows the photo tools' items (research/272 (A)): the one table of the items the
 *        detailed view's left panel (photodetail_left.cpp), its right panel (photodetail_right.cpp) and the simple
 *        view (panel_simple.cpp) place by hand, each in its section and its order. Before, each of the three files
 *        kept its own list.
 *
 * Only the hand-picked places are here. The rows that follow catalog.xml by its tab (the left's Quality tab,
 * the Avatar tab's look, the Environment tab and the Camera tab's lens section) are not repeated: a new item in
 * catalog.xml comes there by itself. The lens section's left-out items are here (Shown::Omit).
 *
 * An id is a setting of catalog.xml, "param:*" (the photo tools' parameters, phototools.cpp) or "composite:*".
 * The rows of one spot are in the order they are shown. An id that the catalog does not have is left out
 * without a word (psui::Stack::itemById, the right panel's Builder::item): tools/predeliver.py is the place to
 * check the table against catalog.xml (research/272 (B)).
 *
 * The screen's plan (2026-11-06): the left tabs are what is photographed (the subject, the framing, the scene),
 * the right sections how the picture is finished (exposure, colour, effects, output). A place added here follows
 * it; the places of before that do not (the effects on both sides) stay as they were until a branch moves them.
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

#ifndef PLUS_PHOTOSCREEN_PHOTOSCREEN_ITEMS_H
#define PLUS_PHOTOSCREEN_PHOTOSCREEN_ITEMS_H

#include <string>

namespace plus::photoscreen::items
{
    // A section of a panel that places items by hand.
    enum class Spot
    {
        // the left panel: what is photographed
        LeftSceneDisplay,   // Scene tab, "display": what the world shows
        LeftLightingLight,  // Lighting tab, "light"
        LeftLightingShadow, // Lighting tab, "shadow"
        LeftEffectsLens,    // Effects tab, "lens"
        LeftEffectsAir,     // Effects tab, "air"
        LeftCameraLens,     // Camera tab, "lens": the catalog's camera rows, but for these (Omit)
        LeftCameraGuide,    // Camera tab, "guide"
        // the right panel: how the picture is finished
        RightCamera,        // "camera": the focal length, then the depth of field's switch and its rows (the bokeh,
                            // the f-number, the focus)
        RightExposure,      // "exposure"
        RightColor,         // "color grading"
        RightEffects,       // "effects"
        RightOutput,        // "output"
        // the simple view
        SimpleFocus,        // the focus page
        SimpleStrip,        // the strip at the bottom middle
    };

    enum class Shown
    {
        Row,    // the item's own row, made from the catalog (left: Stack::itemById; right: Builder::item, `widget` its
                // name in the panel's XML strings)
        Own,    // a widget the panel makes for the item itself (`widget` names it); its code reads the id from the
                // ITEM_ constant below
        Omit,   // left out of a section that takes the catalog's rows by their tab
    };

    // When a row of the right panel can be used (besides the catalog's "needs").
    enum class Gate
    {
        Always,
        WithLook,   // a look is on (the colour grading's switch)
        WithJpeg,   // the format is JPEG (SnapshotFormat 1)
    };

    struct Placement
    {
        Spot        spot;
        const char* id;
        const char* widget = nullptr;
        Shown       shown  = Shown::Row;
        Gate        gate   = Gate::Always;
    };

    // The items the panels' own widgets work on (Shown::Own).
    inline constexpr char ITEM_SHADOWS[]        = "RenderShadowDetail";  // the left's "show shadows" switch
    inline constexpr char ITEM_LOD[]            = "composite:lod";       // the left's "most detail" switch
    inline constexpr char ITEM_CAMERA_ANGLE[]   = "CameraAngle";         // the right's focal length and angle of view
    inline constexpr char ITEM_DEPTH_OF_FIELD[] = "RenderDepthOfField";  // the right's switch, the simple view's button
    inline constexpr char ITEM_GUIDE_ON[]       = "PlusPhotoGuideOn";    // the simple view's thirds button
    inline constexpr char ITEM_GUIDE_KIND[]     = "PlusPhotoGuideKind";  // with it

    inline constexpr Placement PLACEMENTS[] = {
        // ---- the left panel (photodetail_left.cpp) ----
        { Spot::LeftSceneDisplay,   ITEM_SHADOWS, "ShowShadows", Shown::Own },
        { Spot::LeftSceneDisplay,   ITEM_LOD, "MaxLod", Shown::Own },
        { Spot::LeftSceneDisplay,   "PlusTAA" },

        { Spot::LeftLightingLight,  "param:sun" },
        { Spot::LeftLightingLight,  "param:shadow" },
        { Spot::LeftLightingLight,  "PlusSsgi" },
        { Spot::LeftLightingLight,  "PlusSsgiStrength" },
        { Spot::LeftLightingLight,  "PlusSsgiRadius" },
        { Spot::LeftLightingLight,  "PlusSsgiThickness" },

        { Spot::LeftLightingShadow, "RenderShadowDetail" },
        { Spot::LeftLightingShadow, "PlusShadowSoftness" },
        { Spot::LeftLightingShadow, "PlusShadowPenumbra" },
        { Spot::LeftLightingShadow, "PlusShadowDistance" },
        { Spot::LeftLightingShadow, "PlusShadowPrecision" },
        { Spot::LeftLightingShadow, "PlusShadowFocus" },

        { Spot::LeftEffectsLens,    "PlusLensFlare" },
        { Spot::LeftEffectsLens,    "PlusCrossFilter" },
        { Spot::LeftEffectsLens,    "PlusCrossFilterRays" },
        { Spot::LeftEffectsLens,    "PlusCrossFilterAngle" },
        { Spot::LeftEffectsLens,    "PlusCrossFilterSky" },
        { Spot::LeftEffectsLens,    "param:glare" },
        // research/286: the soft glow of the look pass
        { Spot::LeftEffectsLens,    "PlusFxSoftGlow" },
        { Spot::LeftEffectsLens,    "PlusFxSoftGlowSize" },
        { Spot::LeftEffectsLens,    "PlusFxSoftGlowLift" },

        // research/253 9: the one-press sunbeams through clouds first, their own density after the light shafts'
        { Spot::LeftEffectsAir,     "PlusCloudShaftsScene" },
        { Spot::LeftEffectsAir,     "PlusGroundFog" },
        { Spot::LeftEffectsAir,     "PlusGroundFogHeight" },
        { Spot::LeftEffectsAir,     "PlusGroundFogGlow" },
        { Spot::LeftEffectsAir,     "PlusVolumetric" },
        { Spot::LeftEffectsAir,     "PlusVolumetricStrength" },
        { Spot::LeftEffectsAir,     "PlusVolumetricQuality" },
        { Spot::LeftEffectsAir,     "PlusCloudShafts" },

        // photo-camera, research/260: the focal length, the depth of field and the blur are the right panel's, the
        // tilt is the camera controls' tilt row, and the camera's motion is not for the photo screen (the photo
        // tools and Preferences keep these rows). The guide has its own section (LeftCameraGuide).
        { Spot::LeftCameraLens,     "CameraAngle", nullptr, Shown::Omit },
        { Spot::LeftCameraLens,     "RenderDepthOfField", nullptr, Shown::Omit },
        { Spot::LeftCameraLens,     "param:blur", nullptr, Shown::Omit },
        { Spot::LeftCameraLens,     "PlusCameraRoll", nullptr, Shown::Omit },
        { Spot::LeftCameraLens,     "ZoomTime", nullptr, Shown::Omit },
        { Spot::LeftCameraLens,     "DynamicCameraStrength", nullptr, Shown::Omit },
        { Spot::LeftCameraLens,     "CameraOffsetScale", nullptr, Shown::Omit },
        { Spot::LeftCameraLens,     "CameraPositionSmoothing", nullptr, Shown::Omit },
        { Spot::LeftCameraLens,     "MouseSensitivity", nullptr, Shown::Omit },

        { Spot::LeftCameraGuide,    "PlusPhotoGuideOn" },
        { Spot::LeftCameraGuide,    "PlusPhotoGuideKind" },
        { Spot::LeftCameraGuide,    "PlusPhotoGuideColor" },
        { Spot::LeftCameraGuide,    "PlusPhotoGuideOpacity" },
        { Spot::LeftCameraGuide,    "PlusPhotoGuideThickness" },

        // ---- the right panel (photodetail_right.cpp) ----
        { Spot::RightCamera,        ITEM_CAMERA_ANGLE, "focal", Shown::Own },   // and "fov"
        { Spot::RightCamera,        ITEM_DEPTH_OF_FIELD, "sub_dof", Shown::Own },

        // local contrast (research/249): the look pass's
        { Spot::RightExposure,      "PlusFxClarity", "clarity" },
        { Spot::RightExposure,      "PlusFxTexture", "texture" },

        { Spot::RightColor,         "PlusLookStrength", "look_strength", Shown::Row, Gate::WithLook },

        { Spot::RightEffects,       "PlusLensFlare", "lens_flare" },
        { Spot::RightEffects,       "PlusCrossFilter", "cross_filter" },
        { Spot::RightEffects,       "param:glare", "glare" },
        { Spot::RightEffects,       "PlusFxSoftGlow", "soft_glow" },
        { Spot::RightEffects,       "PlusSsgi", "ssgi" },
        { Spot::RightEffects,       "PlusVolumetric", "shafts" },
        { Spot::RightEffects,       "PlusCloudShaftsScene", "cloud_shafts" },  // research/253 9: the one-press recipe

        // size and aspect ratio are the chips above the world (photodetail_center.cpp)
        { Spot::RightOutput,        "SnapshotFormat", "format" },
        { Spot::RightOutput,        "SnapshotQuality", "quality", Shown::Row, Gate::WithJpeg },

        // ---- the simple view (panel_simple.cpp) ----
        { Spot::SimpleFocus,        ITEM_DEPTH_OF_FIELD, "btn_blur_on", Shown::Own },
        { Spot::SimpleStrip,        ITEM_GUIDE_ON, "btn_thirds", Shown::Own },
        { Spot::SimpleStrip,        ITEM_GUIDE_KIND, "btn_thirds", Shown::Own },
    };

    // The rows (Shown::Row) of one spot, in their order.
    template <class F>
    void forEachRow(Spot spot, F&& f)
    {
        for (const Placement& placement : PLACEMENTS)
        {
            if (placement.spot == spot && placement.shown == Shown::Row)
            {
                f(placement);
            }
        }
    }

    // Whether a section that takes the catalog's rows by their tab leaves this item out.
    inline bool omits(Spot spot, const std::string& id)
    {
        for (const Placement& placement : PLACEMENTS)
        {
            if (placement.spot == spot && placement.shown == Shown::Omit && id == placement.id)
            {
                return true;
            }
        }
        return false;
    }
}

#endif // PLUS_PHOTOSCREEN_PHOTOSCREEN_ITEMS_H
