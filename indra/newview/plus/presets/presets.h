/**
 * @file presets.h
 * @brief The Charis features on the official quality levels (the graphics
 *        slider's seven: featuretable.txt), the one-time move from the
 *        retired Charis presets, and the snapshot boost
 *        (research/183-preset-mapping-to-official-7.md in the sl-viewer repository).
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

#ifndef PLUS_PRESETS_PRESETS_H
#define PLUS_PRESETS_PRESETS_H

#include <functional>
#include <string>
#include <vector>

namespace plus::presets
{
    // The official quality slider's level in use (RenderQualityPerformance, 0 Low .. 6 Ultra).
    U32 graphicsLevel();

    // A setting's value at the level in use as featuretable.txt has it (the smaller of "all" and the level's);
    // the fallback when neither has it.
    F32 graphicsLevelValue(const std::string& name, F32 fallback);

    // The comparison with the official viewer (Preferences > Charis > Development: Mid-High with its far clip, LOD,
    // anisotropy, OIT, the official night sky and no TAA) is what the settings are: then no level is shown in use.
    bool comparisonInUse();

    // Applies one of the official levels (0 Low .. 6 Ultra; higher is Ultra) as the photo
    // tools' Quality buttons ("Plus.Presets.SetLevel") and the photo screen's Quality tab do: a notice
    // first, the level a few frames later (a second click meanwhile replaces the level asked for).
    void requestGraphicsLevel(U32 level);

    // Called on the main thread after one of the levels was applied from the Charis menu, the photo tools,
    // the photo screen, the recommendation or the comparison (the same level again too: its values were
    // written again). Preferences' own slider applies a level without this: it changes
    // RenderQualityPerformance, which can be watched for that. Register from Feature::init.
    void addGraphicsLevelListener(const std::function<void(U32 level)>& listener);

    // LLFeatureManager::applyRecommendedSettings() and getRecommendedSettingsMap(): the level
    // the official viewer picks for this GPU (its benchmark class, 0 to 5), raised to Ultra (6)
    // for a class 5 GPU with 7 GB of VRAM or more (research/183: the official viewer never
    // picks Ultra by itself).
    U32 recommendedGraphicsLevel(U32 level, S32 gpu_class);

    // LLPresetsManager::savePreset(), after LLFloaterPreference::getControlNames(): what a saved
    // graphics preset holds. Adds the
    // Charis settings the quality levels set (the "Plus..." lines and RenderOITMode of
    // featuretable.txt's "all"), so loading a saved preset leaves none of the level before.
    void addPresetControlNames(std::vector<std::string>& names);

    // After settings were brought over (plus/importsettings): the official "Preset in use"
    // (PresetGraphicActive) naming a copy of a retired Charis preset ("Charis ...") that is
    // not in the account's folder names none. Before the login nothing is done here: the
    // copies are removed at the login, and the name is checked then.
    void recheckActivePresetName();

    // The snapshot boost (PlusSnapshotBoost): while a snapshot is rendered,
    // the Charis enhancements are on at their best quality whatever the
    // preset, without changing any setting. Features ask this when they read
    // their settings for a frame.
    bool snapshotBoostActive();

    // True while a snapshot is rendered, whether or not the boost is on (for
    // features applied to snapshots only, like a look with "only in snapshots").
    bool snapshotInProgress();
    // Counts the snapshots begun (the outermost of nested ones): tells one
    // snapshot from the next even with no frame between them (plus/look holds
    // its noise still for one).
    U32 snapshotSerial();

    // research/109: the texture budget (LLViewerTexture::updateClass) does not
    // react to VRAM while this is true: a snapshot renders (its targets, some
    // 3 GB at 3840x2160, are not textures), and for up to 5 s after one until
    // VRAM in use is back within 512 MB of where it was before it.
    bool textureBudgetHeld();
    // updateClass's figures each update, for the log lines after a snapshot
    // (every 5 s for 60 s after one that took a second or more).
    void noteTextureBudget(float used_mb, float limit_mb, float discard_bias, int pressure);
    // plus/shadowdist, each time the screen's and shadows' targets are made: how
    // many sets a snapshot made (two are right: its size, then the window's
    // again; each extra one stays in VRAM until the snapshot ends).
    void noteScreenTargets();

    // The quality level a feature uses while the boost is on (its highest;
    // features clamp it to their own range).
    constexpr unsigned SNAPSHOT_BOOST_QUALITY = 3;

    // The boost's quality for a target of width x height: the best, but one
    // lower above 4K, where the best (full-resolution passes) would take
    // hundreds of MB of VRAM at once on the PCs the boost is meant for.
    inline unsigned snapshotBoostQuality(unsigned width, unsigned height)
    {
        constexpr unsigned long long LARGE_PIXELS = 3840ull * 2160ull;
        return (unsigned long long)width * height > LARGE_PIXELS ? SNAPSHOT_BOOST_QUALITY - 1 : SNAPSHOT_BOOST_QUALITY;
    }

    // Called on the main thread when the boost starts (true) and when it ends
    // (false), before and after the frames of a snapshot: for features whose
    // boosted state needs work ahead of those frames, such as render targets
    // of another size (plus/shadowdist). Register from Feature::init.
    void addSnapshotBoostListener(const std::function<void(bool active)>& listener);

    // Called on the main thread when the outermost snapshot begins (true:
    // snapshotInProgress() is true already) and when it ends (false: it is
    // false again), whether or not the boost is on: for features that draw
    // snapshots otherwise than the screen, such as plus/upscale (snapshots at
    // the full size, research/165). Register from Feature::init.
    void addSnapshotListener(const std::function<void(bool in_progress)>& listener);

    // Held for as long as a snapshot renders (LLViewerWindow's snapshot paths,
    // next to the full-OIT override): snapshotInProgress() is true, and
    // snapshotBoostActive() too when the boost is on and not withheld. Nests.
    class SnapshotBoost
    {
    public:
        SnapshotBoost();
        ~SnapshotBoost();

        SnapshotBoost(const SnapshotBoost&) = delete;
        SnapshotBoost& operator=(const SnapshotBoost&) = delete;

    private:
        bool mActive;
        // research/109: when it began and the VRAM in use then (MB, 0: unknown), for its log line.
        double mStart = 0.0;
        unsigned long long mVRAMBefore = 0;
    };

    // Held around snapshots the boost would spoil or only slow down: the 360
    // snapshot's faces (LLViewerWindow::simpleSnapshot; screen-space effects
    // would seam at their edges) and the pictures the viewer takes for itself
    // (saveSnapshot: the login screen's last and home locations, and LEAP's
    // saveSnapshot). They are still snapshots (snapshotInProgress()), without
    // the boost.
    class SnapshotBoostWithheld
    {
    public:
        SnapshotBoostWithheld();
        ~SnapshotBoostWithheld();

        SnapshotBoostWithheld(const SnapshotBoostWithheld&) = delete;
        SnapshotBoostWithheld& operator=(const SnapshotBoostWithheld&) = delete;
    };
}

#endif // PLUS_PRESETS_PRESETS_H
