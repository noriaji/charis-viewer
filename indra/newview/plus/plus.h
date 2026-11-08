/**
 * @file plus.h
 * @brief "Plus": the shared foundation for features ported into this viewer
 *        from other viewers (Firestorm, Alchemy, Black Dragon, Catznip, ...).
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

// How it fits together
// --------------------
// Every feature lives in its own folder, indra/newview/plus/<feature>/, and
// registers itself with a static plus::FeatureRegistration in one of its own
// .cpp files. The official viewer only knows about the foundation, through a
// handful of one-time hooks marked <plus> ... </plus> (run plus/plus-hooks.sh
// to list them):
//
//   newview/CMakeLists.txt       include(plus/plus.cmake), which pulls in every
//                                feature's own <feature>.cmake
//   llviewerfloaterreg.cpp       plus::registerFloaters()
//   menu_viewer.xml (en)         an empty top-level "Plus" menu; its items come
//                                from our own menu_plus.xml
//   viewer_manifest.py           packages app_settings/plus/ (feature data files)
//   llui/llcommandmanager.cpp/.h LLCommandManager::load() also reads app_settings/commands_plus.xml
//                                (toolbar buttons of features; loadFile())
//   llappviewer.cpp              LLTransUtil::parseStrings("strings_plus.xml") after strings.xml
//                                (the texts of features: toolbar labels and tooltips);
//                                plus::declareSettings() in initConfiguration() right after
//                                the user's settings.xml (settings_plus.xml before "--set")
//
// Feature hooks: a feature that changes what the official viewer does or draws
// is called from the official file at that place, each call kept short and
// marked <plus> ... </plus> with a note of what it is for. They are not listed
// here (a hand-kept list here fell behind); the lists kept up to date are:
//   plus/plus-hooks.sh           every marked hook with its lines, read from the
//                                source (--brief: file:line of each)
//   OFFICIAL-FILE-CHANGES.md     every change to an official file, by feature and
//                                research note (in the sl-viewer repository,
//                                updated at each merge)
//
// Everything else is ours and needs no official edit when a feature is added:
//   app_settings/settings_plus.xml       settings of all features (gSavedSettings;
//                                        entries with PerAccount 1: gSavedPerAccountSettings)
//   skins/default/xui/<lang>/menu_plus.xml  items of the "Plus" menu
//   skins/default/textures/plus/         icons, declared with declareUIImage()
//   app_settings/plus/<feature>/         data files shipped with a feature
//
// Two folders hold no code of their own: plus/building/ (how to build the
// published source: BUILDING.md, charis-build.sh, the build-variables patch)
// and plus/tonemap/ (the ORIGIN.md of the tonemap types ported into the official
// tonemapUtilF.glsl; drawing code lives in the official files).

#ifndef PLUS_PLUS_H
#define PLUS_PLUS_H

#include <functional>
#include <ios>
#include <iosfwd>
#include <string>

class LLColor3;
class LLControlGroup;
class LLSettingsSky;
class LLViewerRegion;

namespace plus
{
    struct Feature
    {
        // Short folder-style name, e.g. "poser". Used in log lines only.
        std::string name;

        // Called once from LLViewerFloaterReg::registerFloaters(), early in
        // LLAppViewer::init(): settings are loaded, the window is not. Register
        // floaters, UI images and menu callbacks here.
        std::function<void()> init;

        // Called once when the main menu and the UI image list exist
        // (after init_menus(), during login). Optional.
        std::function<void()> onUIReady;
    };

    // Declare a feature from a static object in the feature's .cpp:
    //   static plus::FeatureRegistration sRegistration({ "poser", &init });
    class FeatureRegistration
    {
    public:
        explicit FeatureRegistration(Feature feature);
    };

    // The single entry point called by the official viewer
    // (LLViewerFloaterReg::registerFloaters()).
    void registerFloaters();

    // LLStartUp::setStartupState(): fans the state change out to the features that follow the
    // login (plus/loginfast's timeline, plus/rlv's enabling at STATE_LOGIN_CLEANUP). from and to are
    // EStartupState values. Main thread.
    void onStartupStateChanged(int from, int to, const std::string& to_name);

    // Builds the menu XML file (skins/<skin>/xui/<lang>/) and moves its items into the empty menu called
    // target_name of the menu bar (any depth). From a Feature's onUIReady, main thread. Does nothing while
    // gMenuBarView is null; a missing target is a warning in the log.
    void addMenuItemsFromFile(const char* file, const char* target_name);

    // Declares the controls of app_settings/settings_plus.xml (and lets go of the
    // retired ones, resets the unsaved switches). Called from
    // LLAppViewer::initConfiguration() right after the user's settings.xml was read,
    // so the session settings files, the command line and "--set Plus..." find them;
    // registerFloaters() calls it too (once only: later calls do nothing).
    void declareSettings();

    // LLControlGroup::saveToFile(), written next to path first and moved over it
    // only once it reads back whole: a crash or a full disk while saving leaves
    // the old file, not a cut-off one (which would read back as no settings).
    // For the saves features make while running; the official ones (quit) stay.
    // Returns what saveToFile() returns (the number saved), 0 when not written.
    // gSavedSettings ends a photo-tools compare first (its "as they were" values
    // are not the user's: fix-round49), as the quit does.
    U32 saveSettingsFile(LLControlGroup& group, const std::string& path, bool nondefault_only);

    // The name to write path's new content under before moving it over path:
    // next to it (the same disk, so the move replaces it whole) and with this
    // process's id, so two viewers saving the same file do not share it.
    std::string tempPathFor(const std::string& path);

    // Deletes path's tempPathFor() files that other processes left (a crash while
    // saving): exactly "<file>.<digits>.tmp", not this process's, and only ones
    // not written for an hour. Only when path is in the viewer's own user folder (user_settings,
    // the accounts; not the cache, whose place the user picks): never in a folder the user
    // picked. Once per file in a session (the folder is read then, not at every save).
    // replaceFile() calls it after a good save.
    void removeStaleTemps(const std::string& path);

    // What replaceFile() / writeFileReplacing() did: the file replaced, or why not (each feature
    // logs it in its own words).
    enum class FileWrite
    {
        written,          // path has the new content
        cannot_open,      // the temporary file could not be made
        cannot_write,     // not all of it reached the disk (a full disk), or the writer gave up
        cannot_replace,   // written whole, but path could not be replaced
    };

    // Saves path's new content without ever leaving a cut-off file under its name: write_temp writes it
    // to tempPathFor(path) (the name it is given) and says how that went; only a whole one is moved over
    // path, tried replace_attempts times 10 ms apart (a reader may hold the old file open for a moment on
    // Windows). A failure leaves path as it was and removes the temporary file (except for cannot_open:
    // nothing was made). After a good save the leftovers of crashed saves go (removeStaleTemps(), which
    // looks only in the viewer's own folders); tidy false for a file the user placed (an export), so
    // that folder is not read at all. The caller logs; any thread (only gDirUtilp's folder names are read).
    FileWrite replaceFile(const std::string& path, const std::function<FileWrite(const std::string& temp)>& write_temp,
                          int replace_attempts = 1, bool tidy = true);

    // replaceFile() for content written to a stream: write puts all of it into the open stream, which is
    // then closed (a write that failed, or did not reach the disk, shows only there).
    FileWrite writeFileReplacing(const std::string& path, const std::function<void(std::ostream& out)>& write,
                                 std::ios_base::openmode mode = std::ios_base::out | std::ios_base::trunc,
                                 int replace_attempts = 1, bool tidy = true);

    // Moves path (a file that cannot be read) aside before the feature starts empty, so its next save does
    // not write over what the user had: to "<path>.bad", or while that is taken, the first free one of
    // "<path>.bad<first_number>", "<path>.bad<first_number + 1>", ... (names names in all); with every name
    // taken, the last one gives way. Returns the name it now has, empty when it could not be moved.
    std::string putAside(const std::string& path, int names, int first_number = 2);

    // False when the XML file at path nests elements deeper than max_depth (fix-round101). The official
    // readers have no limit and recurse when the result is used or freed (LLSD at about 1,500 levels,
    // LLXUIParser at about 900): a settings, preset, colours or list file is checked with this before it
    // is read, and refused (a log line) when it is too deep. A file that cannot be opened or is cut short
    // passes (its reader says so). Comments, CDATA, <? ?> and the DOCTYPE are skipped, quotes respected.
    bool xmlNestingOk(const std::string& path, int max_depth = 64);

    // True while the keyboard focus is a control that takes typing (a text field, a line editor): the
    // letters and digits are its own then. For the features' own keys (camera path, photo keys, flycam
    // sensitivity), which add their own rule for the modifier keys. Main thread.
    bool textHasKeyboard();

#if LL_WINDOWS
    // This program's full path, as Windows has it (GetModuleFileNameW: not looked up on PATH or in the
    // current folder, not normalised). Empty on failure. Any thread, also before the viewer is set up.
    std::wstring ownExePath();
#endif

    // Declare a UI image the way textures.xml does. file_name is relative to
    // skins/<skin>/textures/ (e.g. "plus/visual_pose_enabled.png"). The image
    // is added to the UI image list once it exists. Call from Feature::init.
    void declareUIImage(const std::string& name, const std::string& file_name);

    // Shows a folder (or opens a file) in the desktop's file manager
    // (Firestorm has LLWindow::openFile() for this; the official LLWindow does not).
    void openFolder(const std::string& path);

    // Text that came from someone else (a speaker's name and words, an object's, folder's
    // or animation's name, a notecard's lines), made safe to put into a notification or a
    // text box: each line is wrapped in <nolink>...</nolink>, so no link inside it is made.
    // Those text boxes treat their text as trusted (trusted_content defaults to true, unlike
    // the chat history), so a "[secondlife:///app/wear_folder?... label]" left as it was
    // would run there commands that untrusted text may not. So that nothing inside can end
    // the protection early, every "<" of theirs becomes the full-width "＜", a "[" left open
    // at the end of a line the full-width "［", and CR and NUL are dropped. Only for text
    // boxes that parse links (parse_urls): elsewhere the tags would show as they are.
    std::string safeText(const std::string& untrusted);

    // A Windows device name (CON, NUL, COM1, LPT1, ...: "CON.xml" is the console, not a
    // file), judged by the part of a file name before its first dot, trailing spaces ignored.
    bool isWindowsDeviceName(const std::string& file_name);

    // The rule for SAVING: a name the user or a tool gives, used as a file name in a folder of
    // ours (name + an extension, e.g. the poser's poses): 1-200 bytes, no path (\ / :), none
    // of * ? " < > | or control characters, no ".." and no dot or space at either end, no
    // device name. Any other character is fine (Japanese, "Sit (1)", "%20" kept as it is).
    // The poser window's and the MCP's save_pose.
    bool isSafeFileStem(const std::string& name);

    // The rule for LISTING and LOADING a file already in such a folder (its name without the
    // extension): what isSafeFileStem refuses only to keep new names tidy is let through, so
    // files saved elsewhere (Firestorm's poses copied over) still show and load: ".." inside
    // ("Sit..2"), a dot or space at either end, more than 200 bytes. Still refused: empty, a
    // name of dots only, \ / :, * ? " < > |, control characters (NUL too), a device name.
    // The poser window's list and the MCP's list_poses and load_pose: every name listed loads.
    bool isLoadableFileStem(const std::string& name);

    // Whether a region of the viewer's world is around the agent: the agent's region, or one
    // whose square comes within the draw distance and one region's width of the agent (across
    // the ground, on each axis: the eight neighbours always are). After a teleport far away the old region stays in the viewer's world for a
    // while (until its simulator lets it go), with its avatars and objects: it is far, so not
    // around. Judged by distance, not by the eight neighbours, as a grid may connect regions
    // further than those for a long draw distance (OpenSim does): those stay around. The radar
    // and the area search. True when either region is not known (null, or no agent region).
    // (fix-round10 2)
    bool isRegionAround(const LLViewerRegion* region);

    // Whether the tone mapping takes the "no post" shader this frame (LLPipeline::tonemap's
    // no_post, the same test): getReflectionProbeAmbiance(RenderSkyAutoAdjustLegacy) == 0
    // (a classic sky, or a newer sky saved with its reflection probe ambiance at 0); a
    // snapshot taken without post-processing; the build tools open with
    // RenderDisablePostProcessing. That shader applies no exposure and only clamps every
    // channel to 1: what the features hold under 1 so as not to come out white is judged
    // by this, not by isClassicSky(). (fix-round43 1)
    bool isNoPost();
    // The same, for the sky already in hand (LLEnvironment::getCurrentSky(); null as no sky).
    bool isNoPost(const LLSettingsSky* sky);

    // The exposure the tone mapping applies to the scene this frame (LLPipeline::tonemap):
    // 1 under isNoPost(), so RenderExposure and the exposure map do nothing there; else
    // RenderExposure, clamped to 0.5 - 4 as the tone mapping clamps it. For the features
    // that add light to the scene in screen units, or measure it there (glare, rain,
    // fireflies, skin fill). (fix-round39 1)
    F32 effectiveExposure();

    // The exposure scale handed to the passes that read the exposure map (glare, rain,
    // fireflies; the glare in the tone mapping): effectiveExposure(), or 0 under isNoPost(),
    // where those shaders take the exposure as 1 and do not read the map. The "no post"
    // tone mapping applies no exposure at all, and the map is not always 1 there (a snapshot
    // without post-processing under an HDR sky; RenderUseExposureSkySettings). (fix-round48 1)
    F32 exposureScaleForPasses();

    // Whether the sky lights the scene the classic way (LLSettingsVOSky::applySpecial's
    // classic_mode: canAutoAdjust() and not RenderSkyAutoAdjustLegacy): its sun and ambient
    // colours are used as they are (sRGB, not made linear; atmosphericsFuncs.glsl
    // calcAtmosphericVarsLinear) and the sunlight is taken 1.35 times (softenLightF.glsl).
    // How the light is taken, only: whether the picture is clamped to 1 is isNoPost().
    // (fix-round39 2)
    bool isClassicSky();
    // The same, for the sky already in hand (LLEnvironment::getCurrentSky(); null as no sky).
    bool isClassicSky(const LLSettingsSky* sky);

    // The sunlight multiplier softenLightF.glsl applies on top in a classic sky.
    constexpr F32 CLASSIC_SUNLIGHT_SCALE = 1.35f;

    // The sky's ambient (getTotalAmbient) as the features that add the sky's light take it:
    // as it is (sRGB) under a classic sky (classic: isClassicSky(sky)), as the scene takes it
    // there; else made linear. sky must not be null. (fix-round67 7a)
    LLColor3 skyAmbient(const LLSettingsSky* sky, bool classic);

    // A colour's luminance: the Rec. 709 weights (0.2126, 0.7152, 0.0722), as the shaders take it.
    F32 luminance(const LLColor3& color);

    // After a render target could not be made (VRAM full), the next try waits
    // RETRY_SECONDS instead of coming every frame (each try a large allocation
    // that fails again). Before allocating: `if (!retry.due()) return false;`,
    // then failed() or succeeded(). While nothing failed, due() only reads a bool.
    // (fix-round61 3)
    class RetryWait
    {
    public:
        static constexpr F64 RETRY_SECONDS = 5.0;
        bool due() const { return !mWaiting || waitOver(); }
        void failed();
        void succeeded() { mWaiting = false; }

    private:
        bool waitOver() const;
        bool mWaiting = false;
        F64 mNextTry = 0.0;     // LLFrameTimer::getElapsedSeconds()
    };

    // The level for a "targets made for WxH" log line: a window resized by
    // dragging its frame remakes the screen targets every frame, and each
    // feature's line came once per frame. INFO for a new size at most once per
    // INTERVAL_SECONDS (the first size of a drag always); a repeated size, or
    // another within the interval, goes to DEBUG. `detail`: anything else the
    // line reports that may change at the same size (a new value logs like a
    // new size). One per feature, kept (static or member):
    // `const LLError::ELevel log_level = log.level(w, h); LL_VLOGS(log_level, "Tag") << ...`
    // (in a local first: LL_VLOGS reads its level twice). No allocation. (fix-round95 D)
    class ResizeLog
    {
    public:
        static constexpr F64 INTERVAL_SECONDS = 1.0;
        LLError::ELevel level(U32 width, U32 height, U32 detail = 0);

    private:
        U32 mWidth = 0;         // the size (and detail) last logged at INFO
        U32 mHeight = 0;
        U32 mDetail = 0;
        F64 mLastInfo = -1.0e9; // LLFrameTimer::getElapsedSeconds()
    };
}

#endif // PLUS_PLUS_H
