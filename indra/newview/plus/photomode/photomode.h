/**
 * @file photomode.h
 * @brief Photo mode: the look sliders and presets, the sky and water picker,
 *        the camera memory (research/photo-mode-ui.md in the sl-viewer
 *        repository). The card itself is phototoolsfloater.* (the photo tools).
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

#ifndef PLUS_PHOTOMODE_PHOTOMODE_H
#define PLUS_PHOTOMODE_PHOTOMODE_H

#include <map>
#include <string>
#include <vector>

#include "llsd.h"
#include "lluuid.h"
#include "v3dmath.h"

namespace plus::photomode
{
    // The look sliders. Each one is a way of writing the viewer's own settings
    // (the settings stay the single source of truth: the card reads them back
    // when it opens), none of which rebuilds the pipeline:
    //
    //   Brightness  -100..100  RenderExposure = 2^(v/100)          (+-1 stop)
    //   Blur           0..100  CameraFNumber, f/22 .. f/1.4 (log)   (needs RenderDepthOfField)
    //   Tint        -100..100  PlusLook (cool-morning / warm-afternoon) and PlusLookStrength = |v|
    //   Sun            0..360  the sun's direction against the camera: 0 sun behind the camera
    //                          (front light), 180 sun in front (backlight). Writes the sun of the
    //                          personal (local) sky, as the official "Adjust Environment" does
    //   Shadow         0..100  RenderSkyAmbientScale = 1.0 * 2^((50-v)/50)  (less ambient: darker shadows)
    //   Contrast       0..100  RenderTonemapMix = 0.4 + 0.6 * v/100
    //   Glare            0..3  PlusGlare
    enum class Param
    {
        Brightness,
        Blur,
        Tint,
        Sun,
        Shadow,
        Contrast,
        Glare,
        Count
    };
    constexpr int PARAM_COUNT = static_cast<int>(Param::Count);

    // A value a preset leaves alone.
    constexpr float UNSET = -1000.f;

    struct ParamRange
    {
        float min;
        float max;
    };
    const ParamRange& range(Param param);

    struct Preset
    {
        std::string id;          // "portrait", "backlight", "sunset", "night", "studio", "natural", "autumn"
        float       values[PARAM_COUNT];   // UNSET: the preset does not touch it
        // Settings the preset sets as they are, besides the sliders (a look, its screen effects, the shadows'
        // softness), never one the quality levels decide (photomode.cpp, the top).
        // One that does not exist (a branch not merged) is left alone. Another preset that does not name it,
        // or Reset All of the photo tools, gives it back as it was (givePresetSettingsBack()).
        std::vector<std::pair<std::string, LLSD>> settings;
    };
    // The built-in presets, replaced by id from app_settings/plus/photomode/presets.xml when it exists.
    const std::vector<Preset>& presets();
    const Preset*              findPreset(const std::string& id);

    // The value of a slider as the viewer's settings have it now (UNSET: the sun,
    // when the card has not set it in this session).
    float value(Param param);
    // True for the two looks the tint slider puts on (cool-morning, warm-afternoon): another look in
    // PlusLook is the user's own choice, not a tint.
    bool isTintLook(const std::string& look);
    // Writes a slider's value to the settings it stands for. The first write of a
    // session remembers the settings as they were (the tint's "none" is the look they had).
    void setValue(Param param, float value);
    // The slider value that writes no change (a photo tools row's "D" button resets to this): each
    // param's own fixed point in its formula (not the setting's shipped default read back:
    // CameraFNumber's own default, 9.0, is not "no blur"). UNSET for one with none (the sun: a
    // direction, not a level).
    float paramDefault(Param param);
    // Sets every value of a preset that it names, and remembers it as the current one.
    bool applyPreset(const std::string& id);
    // The preset last applied ("" none). Kept when a value is changed afterwards: the card
    // lights the button of this one (updatePresetButtons).
    std::string currentPreset();
    // A setting of the preset's own list (Preset::settings) that it wrote in this session and that differs now
    // (changed by hand since).
    bool presetSettingsChanged(const Preset& preset);
    // The settings presets wrote (Preset::settings) back as they were before; one changed since stays as it is.
    void givePresetSettingsBack();
    // Forgets what the presets wrote without giving anything back (a preset of the user's own was put on over it).
    void forgetPresetSettings();

    // Blur works only with depth of field on (Preferences > Graphics): turning it
    // on rebuilds the pipeline, so the card does not.
    bool blurAvailable();

    // The sky and water. Applying one is the double-click of the official
    // "My Environments" window without the editor: the personal (local)
    // environment is set from the item's asset, which needs no copy of an item
    // of the library.
    //
    // The sky and the water are separate layers of the environment (EEP), and the card keeps them
    // apart: a sky leaves the water in use as it is, and a water the sky. A water put on while no
    // personal environment is in use takes the land's day cycle along into it, so the sky goes on
    // moving with the time of day as the land's does (the official "Apply Locally" of a water would
    // stop it where it is).
    bool isEnvironmentItem(const LLUUID& item_id);
    // False when it could not be put on at once (no such item, or not a sky or water): not to be noted as used.
    bool applyEnvironmentItem(const LLUUID& item_id);
    // "Back to the land's settings": clears the personal environment (sky and water).
    void clearEnvironment();
    // The sky back to the land's; a water chosen stays.
    void restoreLandSky();
    // The water back to the land's; the sky stays.
    void restoreLandWater();

    // What the environment in use shows as its sky or water.
    enum class EnvSource
    {
        Land,   // the land's own (no personal one of this layer)
        Asset,  // a sky or water put on from an inventory item (its asset id is given)
        Other   // made or changed here, or by the official editors (not an item as it is)
    };
    EnvSource environmentSource(bool water, LLUUID& asset);
    // Called after the card sets another sky: the sun slider no longer says anything.
    void forgetSun();
    // The sun slider (or a preset) has turned the sun, and the personal environment is still the one it
    // was turned in.
    bool sunChanged();
    // The personal environment back as it was before the sun was first turned (a day cycle moves
    // again; none is none again), then forgetSun(). Nothing is put back when a sky was chosen since.
    void restoreSun();

    // Gaze (plus/gaze): 0 normal, 1 only me, 2 everyone. "Everyone" is for developers
    // (PlusGazeAllowEveryone, off by default: showing other people's eyes turned at your
    // camera may fall under the third-party viewer policy), so the card hides it, and shows a
    // saved 2 as 1, until that setting is on. gazeAllowsEveryone() is false while the setting
    // does not exist.
    // gazeAvailable(): the setting PlusGazeMode exists (plus/gaze is merged; false only
    // with settings that lack it).
    bool gazeAvailable();
    bool gazeAllowsEveryone();
    int  gazeMode();
    void setGazeMode(int mode);

    // --- Camera memory (photocamera.cpp) ---------------------------------

    struct CameraView
    {
        LLVector3d  position;   // global
        LLVector3d  focus;      // global; the direction is position -> focus
        float       fov = 0.f;  // radians, the camera's angle (goToView puts it on the camera, not in CameraAngle)
        float       roll = 0.f; // degrees, + tilted right (plus/camroll); 0 for a position saved before roll existed
        std::string name;
        std::string region;

        LLSD toLLSD() const;
        static bool fromLLSD(const LLSD& data, CameraView& view);
    };

    constexpr int MAX_NAMED_VIEWS = 8; // in each region
    constexpr int MAX_VIEWS_ALL   = 64; // of all regions together (not to grow without end when many regions are visited)

    // The view now, the tilt included. False when there is no avatar or camera yet. With the flycam
    // (3D mouse) on, the flycam's camera: its position, a focus point ahead of it and its angle of view.
    bool currentView(CameraView& view);
    // Goes to a view; smooth glides, else jumps.
    void goToView(const CameraView& view, bool smooth);

    // The temporary view (one, overwritten). It is of the region it was saved in: in another region
    // there is none (hasTempView() false, goToTempView() does nothing).
    void saveTempView();
    bool hasTempView();
    bool goToTempView(bool smooth);

    // The named views of the region the avatar is in, in the order they were made.
    std::vector<CameraView> namedViews();
    // Saves the view now. An empty name takes the next free "Camera N". False when the
    // region already has MAX_NAMED_VIEWS, all regions together MAX_VIEWS_ALL (allNamedViewsFull()),
    // there is no camera, or namedViewsHidden().
    bool saveNamedView(const std::string& name);
    bool allNamedViewsFull();
    // index counts namedViews().
    bool goToNamedView(size_t index, bool smooth);
    bool renameNamedView(size_t index, const std::string& name);
    bool deleteNamedView(size_t index);
    // The name of the region the avatar is in ("" before the first one).
    std::string regionName();
    // RLVa's @showloc: the region's named views are neither listed nor saved (their names, and the parcel's picture
    // the photo screen shows with them, may tell the place; plus/rlv, research/248 3.4). namedViews() is empty then.
    bool namedViewsHidden();
}

#endif // PLUS_PHOTOMODE_PHOTOMODE_H
