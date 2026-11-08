/**
 * @file camerapath.h
 * @brief Camera path: a list of camera points (place, direction, angle, tilt) the camera goes
 *        through in turn, smoothly, or with a cut; camera shake; looking at an avatar; a countdown; the
 *        route drawn on the screen; scripts; a numbered picture for every frame
 *        (research/150-camera-path.md, phases 1 to 3).
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

#ifndef PLUS_CAMERAPATH_CAMERAPATH_H
#define PLUS_CAMERAPATH_CAMERAPATH_H

#include <cmath>
#include <string>
#include <vector>

#include "llkeyboard.h"   // EKeystate
#include "lluuid.h"
#include "v3dmath.h"

#include "pathmath.h"
#include "shakemath.h"
#include "plus/camerashot/camerashot.h"

// One path is open at a time (path()): the window edits it, the keys add to it, play() runs it. Paths are
// saved by name as files in the user settings folder (camerapaths/<name>.xml), shared by every account;
// scripts (camerapaths/<name>.json, see camerapathscript.cpp) are listed with them and opened the same way.
// Nothing is sent to the simulator; only the view of this viewer changes.
//
// The keys (key_bindings.xml, the key settings list them): camera_path_add puts a point at the camera,
// camera_path_play starts or stops the path.
namespace plus::camerapath
{
    constexpr size_t MAX_POINTS = 32;
    constexpr int    MAX_COUNTDOWN = 10;   // seconds
    constexpr float  MAX_TILT = 180.f;     // degrees, of the tilt a point keeps
    constexpr float  MAX_SPIN = 8.f;       // turns, of the extra spin into a point
    constexpr float  MAX_ORBIT = 4.f;      // laps, of the extra laps round the pivot of an orbit point
    constexpr float  MIN_POINT_FOV = 0.15f;   // radians, the angle a point keeps (about 8.6 degrees)
    constexpr float  MAX_POINT_FOV = 2.2f;    // about 126 degrees
    constexpr unsigned MAX_SEED = 999999;  // of the shake's seed: the window's spinner, the files and the scripts alike

    // The places a point may have (a hand-made or damaged file, a script): finite, and inside what a grid can
    // hold. A place in the region (a script's "coords": "region") is checked before it is made global: a
    // number too large for F32 would turn infinite there.
    constexpr double MAX_REGION_XY = 16384.0;   // metres from the region's corner (the largest var regions, with room)
    constexpr double MAX_GLOBAL_XY = 4.3e9;     // metres in the grid (a region handle is a U32 of metres)
    constexpr double MIN_PLACE_Z   = -1000.0;
    constexpr double MAX_PLACE_Z   = 50000.0;

    inline bool saneZ(double z)
    {
        return z >= MIN_PLACE_Z && z <= MAX_PLACE_Z;   // false for a NaN either way
    }

    inline bool saneRegionPlace(const double v[3])
    {
        return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && std::fabs(v[0]) <= MAX_REGION_XY
               && std::fabs(v[1]) <= MAX_REGION_XY && saneZ(v[2]);
    }

    inline bool saneGlobalPlace(const LLVector3d& v)
    {
        return v.isFinite() && std::fabs(v.mdV[VX]) <= MAX_GLOBAL_XY && std::fabs(v.mdV[VY]) <= MAX_GLOBAL_XY
               && saneZ(v.mdV[VZ]);
    }

    struct Point
    {
        std::string name;                // may be empty: the window then shows the number
        LLVector3d  position;            // global
        LLVector3d  focus;               // global; the direction is position -> focus
        float       fov  = 0.f;          // radians; 0: the angle the camera has when the path starts
        float       roll = 0.f;          // degrees, + tilted right (plus/camroll)
        // Extra turns of the picture while the camera moves into this point (1: one full turn; + is the way
        // + tilt goes, the horizon turning counter-clockwise). The tilt at the point is still 'roll'.
        float       spin = 0.f;
        // Motion::Orbit: the move into this point goes round a pivot (the subject when there is one, else 'focus')
        // instead of in a line: the short way round from the point before, plus this many whole laps (+ or -).
        // Two points in the same place with 1 lap make a full circle.
        float       orbit = 0.f;
        double      seconds = 3.0;       // from the point before; 0 is a cut. Not used for the first point
        double      hold    = 0.0;       // the camera stays here this long
        Motion      motion  = Motion::Smooth;
        // A subject: the camera looks at that part of the avatar (the avatar moves, the direction follows) and
        // 'focus' is only where it looks when the avatar cannot be seen. A null id: the direction is the saved one.
        LLUUID                  subject;
        plus::camerashot::Part  part = plus::camerashot::Part::Face;
    };

    struct Path
    {
        std::string        name;         // the file's name; empty before it was saved or loaded
        std::string        region;       // where the first point was made (a warning shows in other regions)
        bool               loop = false; // after the last point, back to the first with a cut
        int                countdown = 0;// seconds of "3, 2, 1" before it starts (0: none)
        ShakeParams        shake;        // the camera shake over the whole path
        std::vector<Point> points;
    };

    // --- The open path ---------------------------------------------------

    // The window changes the points in place and calls changed() after (the times are worked out again,
    // the window is told). Add / remove / move do that themselves.
    Path& path();
    void  changed();
    // Goes up with every change of the path, of what plays, and of the file list: the window redraws when it moved.
    unsigned revision();

    // The chosen point (-1: none). The window, the timeline and the route on the screen share it.
    int  selectedPoint();
    void selectPoint(int index);

    // A point at the camera now, at the end. False with a text key for the user in 'error'
    // ("path_err_no_camera", "path_err_full").
    bool addPoint(std::string& error);
    // The camera now into point 'index'.
    bool overwritePoint(size_t index, std::string& error);
    void removePoint(size_t index);
    // Moves point 'index' up (earlier) or down in the list.
    void movePoint(size_t index, int direction);
    // A copy of the point, right after it.
    void duplicatePoint(size_t index);
    // Starts again with an empty path.
    void clearPath();

    // The length of the path in seconds, and when the camera is at point 'index'.
    double totalSeconds();
    double arrivalSeconds(size_t index);
    // True when the path was made in another region than the avatar is in.
    bool otherRegion();

    // --- Playing -------------------------------------------------------------

    enum class State
    {
        Idle,
        Counting,    // the countdown before the path
        Playing,
        InPlace,     // the camera stays where it is and only shakes (play of the shake alone)
        Exporting    // a numbered picture for every frame of the path
    };
    State state();
    // Anything but Idle: the path holds the camera, and stop() ends it.
    bool playing();

    // Starts the path from its first point (after the countdown, when the path has one). It takes the free camera
    // (the camera leaves the avatar, as Esc / the camera controls bring it back). False with a text key in 'error'
    // ("path_err_no_camera", "path_err_empty", "path_err_flycam").
    bool play(std::string& error);
    // Turns the picture once round in 'seconds' seconds (the camera keeps where it is and may be moved meanwhile):
    // the tilt goes a full turn to the right (or the left) and back to what it was. Not while something plays.
    // False with a text key in 'error' ("path_err_busy", "path_err_no_camera", "path_err_flycam").
    bool spinOnce(double seconds, bool right, std::string& error);
    bool spinning();

    // The camera stays where it is now and only shakes (with the path's shake), until stop().
    bool playInPlace(std::string& error);
    // Ends where the camera is.
    void stop();
    // Seconds into the path now (0 when none plays or in the countdown).
    double time();
    // The seconds of the countdown left (0 when none counts).
    int countdownLeft();
    // Puts the camera at point 'index' (a stop first).
    bool goToPoint(size_t index, std::string& error);
    // Puts the camera where the path is at 'seconds', with the shake at that time (a stop first): for looking along the
    // timeline.
    bool previewAt(double seconds, std::string& error);

    // --- A picture for every frame -------------------------------------------

    // Steps through the path in 1/fps seconds, taking a picture at each step with the photo tools' size, shape
    // and format (plus/photomode/photoshot), and moves them into a folder as frame_00001.png ... for a video
    // program. Slow (the photo button's rest between pictures), but every frame is the same however long it took. False with
    // a text key in 'error' ("path_err_no_camera", "path_err_empty", "path_err_flycam", "path_err_short",
    // "path_err_export_busy"). stop() cancels.
    bool startExport(int fps, std::string& error);
    // The frame being taken and the number of frames (0, 0 when none).
    void exportProgress(int& frame, int& frames);
    // The folder the pictures go to (empty before the first one).
    std::string exportFolder();

    // --- The route on the screen -------------------------------------------

    // The route as lines (a cut starts a new line), without the shake, worked out again when the path changed.
    const std::vector<std::vector<LLVector3d>>& route();
    // Drawn on the screen while the window is open, nothing plays and no picture is taken (never into a picture).
    void setShowRoute(bool show);
    bool showRoute();
    // The window tells it is open or closed.
    void setEditorOpen(bool open);
    // Whether the route is drawn now (the window is open, the switch is on, nothing plays, the path has points).
    bool routeVisible();
    // Puts the view that draws the route in the window (once the UI exists, from the feature's onUIReady).
    void attachOverlay();

    // --- Files -----------------------------------------------------------

    // The names of the saved paths and scripts (a script's name ends with ".json"), sorted.
    std::vector<std::string> savedNames();
    // Saves the open path under 'name' (the path's name becomes it). False with a text key in 'error'.
    bool save(const std::string& name, std::string& error);
    bool load(const std::string& name, std::string& error);
    bool removeSaved(const std::string& name);
    // Opens a path file anywhere (a full file name: .xml as save() writes, or a .json script), named after the file
    // without its extension. False with a text key in 'error' ("path_err_read", "path_err_script").
    bool loadFile(const std::string& file, std::string& error);

    // Reads a script file (camerapathscript.cpp) into 'result'. False with a text key in 'error' ("path_err_read",
    // "path_err_script").
    bool importScript(const std::string& file, Path& result, std::string& error);

    // --- Measuring runs (camerapathbench.cpp, research/153-moving-measure.md) ---

    // Starts watching the cue files <LLRHI_CUE_DIR>/camerapath.run and camerapath.stop (once the UI exists, from the
    // feature's onUIReady). Without LLRHI_CUE_DIR (the downloadable viewer) nothing is registered and nothing runs.
    // A run plays a path for a set number of laps and logs "PerfMark" lines (its start and end, every lap's frame
    // times) for tools/perf-summary.py.
    void attachBench();

    // --- Keys (llviewerinput.cpp registers them) -----------------------------

    bool keyAddPoint(EKeystate state);
    bool keyTogglePlay(EKeystate state);
}

#endif // PLUS_CAMERAPATH_CAMERAPATH_H
