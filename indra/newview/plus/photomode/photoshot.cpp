/**
 * @file photoshot.cpp
 * @brief Photo tools: the one-press high resolution snapshot.
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

// Which official code it uses, and why (research/photo-tools-settings.md, handoff 2026-10-18-photo-tools):
//
//  - The picture: LLViewerWindow::rawSnapshot(), the function the snapshot window's live preview calls
//    (LLSnapshotLivePreview::onIdle), with the same arguments the preview passes when its window has "no
//    UI": show_ui false (so nothing of the viewer's windows is in the picture, detached ones included),
//    the HUD and the post-processing switch from the snapshot window's own settings (RenderHUDInSnapshot,
//    RenderSnapshotNoPost). It renders the frames of the picture itself, so it is called from an idle
//    callback, never from inside a draw. Snapshot boost (plus/presets) is held by rawSnapshot as for any
//    snapshot; the size limits (7680 a side) are rawSnapshot's.
//  - The file: the format and quality of the snapshot window (SnapshotFormat, SnapshotQuality: one setting
//    for both, no second place to keep in step), the folder and base name it saves to (SnapshotBaseDir,
//    SnapshotBaseName of the account), and the same file names (base_date_time01.ext). Written here and
//    not by LLViewerWindow::saveImageNumbered() because that one opens a file dialog when no folder was
//    chosen and does not say which file it made. The size and the shape are the tool's own
//    (PlusPhotoShotSize, PlusPhotoShotAspect): the window's size presets are a list of window sizes.
//
// Research/109: a 4K snapshot makes the screen and shadow targets of a 4K picture, some 2.6 GB over
// what is in use, and the GPU stopped (device lost) after repeated ones. So: the memory is checked
// before (an estimate, against the driver's budget), only one picture is in work, a button press
// during that or the pause after is refused, the pause lasts until the texture budget's hold of
// plus/presets (VRAM back within 512 MB of before) is over and a few seconds have passed.

#include "llviewerprecompiledheaders.h"

#include "photoshot.h"

#include "plus/photoscreen/photoscreen.h"

#include "plus.h"
#include "plus/presets/presets.h"

#include <algorithm>
#include <cmath>

#include "llappviewer.h"
#include "llcallbacklist.h"
#include "lldir.h"
#include "lldirpicker.h"
#include "llfile.h"
#include "llimagebmp.h"
#include "llimagejpeg.h"
#include "llimagepng.h"
#include "llrhi.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"

#if LL_WINDOWS
#include <shlobj.h>
#include <knownfolders.h>
#endif

namespace plus::photomode
{
    namespace
    {
        constexpr char SETTING_SIZE[]   = "PlusPhotoShotSize";
        constexpr char SETTING_ASPECT[] = "PlusPhotoShotAspect";
        constexpr char SETTING_BPP[]    = "PlusPhotoShotBytesPerPixel";

        constexpr int    LONG_EDGE[SHOT_SIZE_COUNT]     = { 0, 1920, 3840, 5760, 7680 };
        constexpr double RATIO[SHOT_ASPECT_COUNT]       = { 0.0, 16.0 / 9.0, 3.0 / 2.0, 4.0 / 3.0, 1.0, 9.0 / 16.0, 2.0 / 3.0 };
        constexpr int    MAX_SIDE                       = 7680;   // MAX_SNAPSHOT_IMAGE_SIZE
        constexpr int    PENDING_FRAMES                 = 3;      // the window shows "taking" first
        constexpr double MARGIN_GB                      = 0.25;
        constexpr double COOL_SECONDS_SMALL             = 1.5;    // up to 4 megapixels
        constexpr double COOL_SECONDS                   = 5.0;
        constexpr double COOL_SECONDS_MAX               = 25.0;   // never waits longer than this for the memory

        ShotState sState = ShotState::Idle;
        int       sFrames = 0;
        ShotDims  sDims;
        LLTimer   sCoolTimer;
        double    sCoolSeconds = 0.0;
        ShotResult sLast;

        // A text of strings_plus.xml (the viewer's language), with its [ARGS].
        std::string shotText(const char* name, const LLStringUtil::format_map_t& args = LLStringUtil::format_map_t())
        {
            return LLTrans::getString(name, args);
        }

        void fail(const std::string& error)
        {
            ++sLast.number;
            sLast.ok    = false;
            sLast.error = error;
            sLast.path.clear();
            LL_WARNS("Plus") << "photo: " << error << LL_ENDL;
        }

        // The user's Pictures folder. On Windows the shell's (it may be moved, to another drive or OneDrive):
        // getOSUserDir() is the roaming AppData folder there, not the user's home.
        std::string userPicturesDir()
        {
#if LL_WINDOWS
            wchar_t* path = nullptr;
            const HRESULT result = SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_CREATE, nullptr, &path);
            std::string dir;
            if (SUCCEEDED(result) && path)
            {
                dir = ll_convert_wide_to_string(path);
            }
            CoTaskMemFree(path); // the contract of SHGetKnownFolderPath(), also when it failed
            if (!dir.empty())
            {
                return dir;
            }
            LL_WARNS("Plus") << "photo: no Pictures folder from the shell (" << (S32)result << "); the profile's is used" << LL_ENDL;
            const std::string profile = LLStringUtil::getenv("USERPROFILE");
            if (!profile.empty())
            {
                return profile + gDirUtilp->getDirDelimiter() + "Pictures";
            }
#endif
            return gDirUtilp->getOSUserDir() + gDirUtilp->getDirDelimiter() + "Pictures";
        }

        // The folder the pictures go to: the snapshot window's (per account), else the Pictures folder
        // (made when it is not there).
        std::string pictureDir()
        {
            std::string dir = LLViewerWindow::getLastSnapshotDir();
            if (dir.empty())
            {
                dir = userPicturesDir();
                LLFile::mkdir(dir);
            }
            return dir;
        }

        void onSaveDirPicked(const std::vector<std::string>& folders)
        {
            if (folders.empty() || folders[0].empty())
            {
                return;
            }
            if (!LLFile::isdir(folders[0]))
            {
                LL_WARNS("Plus") << "photo: the folder chosen for the photos is not a folder" << LL_ENDL;
                return;
            }
            gSavedPerAccountSettings.setString("SnapshotBaseDir", folders[0]);
        }

        // The next unused name, like the snapshot window's: <base>_<date>_<time><nn>.<ext>
        std::string nextPath(const std::string& extension)
        {
            const std::string dir  = pictureDir();
            const std::string base = gSavedPerAccountSettings.getString("SnapshotBaseName").empty() ? std::string("Snapshot")
                                                                                                    : gSavedPerAccountSettings.getString("SnapshotBaseName");
            const LLDate now = LLDate::now();
            std::string  path;
            for (int i = 1; i < 100; ++i)
            {
                path = dir + gDirUtilp->getDirDelimiter() + base + now.toLocalDateString("_%Y-%m-%d_%H%M%S") + llformat("%.2d", i) + "." + extension;
                llstat info;
                if (LLFile::stat(path, &info) != 0)
                {
                    break; // not there yet
                }
            }
            return path;
        }

        void runShot()
        {
            LLTimer timer;
            const ShotDims dims = sDims;
            ++sLast.number;
            sLast.ok    = false;
            sLast.error.clear();
            sLast.path.clear();
            sLast.w     = dims.w;
            sLast.h     = dims.h;

            LLPointer<LLImageRaw> raw = new LLImageRaw;
            const bool hud     = gSavedSettings.getBOOL("RenderHUDInSnapshot");
            const bool no_post = gSavedSettings.getBOOL("RenderSnapshotNoPost");
            LL_INFOS("Plus") << "photo: taking " << dims.w << "x" << dims.h << LL_ENDL;
            const bool taken = gViewerWindow->rawSnapshot(raw, dims.w, dims.h, false, false, false, hud, false, no_post, false);
            if (!taken)
            {
                fail(shotText("PlusPhotoShotErrTake"));
                return;
            }
            sLast.w = raw->getWidth();
            sLast.h = raw->getHeight();

            LLPointer<LLImageFormatted> image;
            switch (gSavedSettings.getS32("SnapshotFormat"))
            {
            case 1:
                image = new LLImageJPEG(gSavedSettings.getS32("SnapshotQuality"));
                break;
            case 2:
                image = new LLImageBMP();
                break;
            default:
                image = new LLImagePNG();
                break;
            }
            LLAppViewer::instance()->pingMainloopTimeout("plus photo: encode");
            if (!image->encode(raw, 0.f))
            {
                fail(shotText("PlusPhotoShotErrEncode"));
                return;
            }
            raw = nullptr; // the pixels go before the file is written

            const std::string path = nextPath(image->getExtension());
            LLAppViewer::instance()->pingMainloopTimeout("plus photo: save");
            if (!image->save(path))
            {
                fail(shotText("PlusPhotoShotErrSave", { { "[PATH]", path } }));
                return;
            }

            sLast.ok    = true;
            sLast.path  = path;
            sLast.bytes = (int)(image->getDataSize() / 1024);
            sLast.ms    = (int)(timer.getElapsedTimeF32() * 1000.f);
            LL_INFOS("Plus") << "photo: " << sLast.w << "x" << sLast.h << " saved to " << path << " (" << sLast.bytes << " KB) in " << sLast.ms << " ms" << LL_ENDL;
            gViewerWindow->playSnapshotAnimAndSound(); // as the snapshot window does (QuietSnapshotsToDisk silences it)
        }

        // Runs every frame while a picture is asked for, being taken or cooling.
        bool step()
        {
            if (sState == ShotState::Pending)
            {
                if (++sFrames < PENDING_FRAMES)
                {
                    return false;
                }
                sState = ShotState::Busy;
                runShot();
                sState = ShotState::Cooling;
                sCoolTimer.reset();
                sCoolSeconds = sDims.megapixels() > 4.0 ? COOL_SECONDS : COOL_SECONDS_SMALL;
                return false;
            }
            if (sState == ShotState::Cooling)
            {
                const double elapsed = sCoolTimer.getElapsedTimeF32();
                // Done when the time has passed and the memory of the picture is given back.
                if (elapsed >= COOL_SECONDS_MAX || (elapsed >= sCoolSeconds && !plus::presets::textureBudgetHeld()))
                {
                    sState = ShotState::Idle;
                    return true;
                }
                return false;
            }
            sState = ShotState::Idle;
            return true;
        }

        bool memoryKnown(U64& usage, U64& budget)
        {
            U64 textures = 0;
            return LLRHI::sVRAMBudget && LLRHI::sVRAMBudget(textures, usage, budget) && budget > 0;
        }
    }

    double shotAspectRatio(int aspect)
    {
        return RATIO[llclamp(aspect, 0, SHOT_ASPECT_COUNT - 1)];
    }

    ShotDims shotDims(int size, int aspect)
    {
        size   = llclamp(size, 0, SHOT_SIZE_COUNT - 1);
        aspect = llclamp(aspect, 0, SHOT_ASPECT_COUNT - 1);

        S32 window_w = gViewerWindow ? gViewerWindow->getWindowWidthRaw() : 1920;
        S32 window_h = gViewerWindow ? gViewerWindow->getWindowHeightRaw() : 1080;
        // <plus> in the photo screen the picture is what the world's rectangle shows (research/218), not the window
        if (gViewerWindow && plus::photoscreen::isActive())
        {
            window_w = gViewerWindow->getWorldViewRectRaw().getWidth();
            window_h = gViewerWindow->getWorldViewRectRaw().getHeight();
        }
        // </plus>
        const double ratio = aspect == 0 ? (double)llmax(window_w, 1) / llmax(window_h, 1) : RATIO[aspect];
        const int long_edge = size == 0 ? llmax(window_w, window_h) : LONG_EDGE[size];

        ShotDims dims;
        if (ratio >= 1.0)
        {
            dims.w = long_edge;
            dims.h = (int)std::lround(long_edge / ratio);
        }
        else
        {
            dims.h = long_edge;
            dims.w = (int)std::lround(long_edge * ratio);
        }
        dims.w = llclamp(dims.w & ~1, 2, MAX_SIDE);
        dims.h = llclamp(dims.h & ~1, 2, MAX_SIDE);
        return dims;
    }

    ShotMemory shotMemory()
    {
        ShotMemory memory;
        U64        usage = 0, budget = 0;
        if (memoryKnown(usage, budget))
        {
            memory.known   = true;
            memory.room_gb = ((double)budget - (double)usage) / 1.0e9;
        }
        return memory;
    }

    bool shotFits(const ShotDims& dims, std::string& why_not, double* needed_gb, double* free_gb)
    {
        return shotFits(dims, shotMemory(), why_not, needed_gb, free_gb);
    }

    bool shotFits(const ShotDims& dims, const ShotMemory& memory, std::string& why_not, double* needed_gb, double* free_gb)
    {
        why_not.clear();
        if (dims.w > MAX_SIDE || dims.h > MAX_SIDE)
        {
            why_not = shotText("PlusPhotoShotTooLarge", { { "[MAX]", std::to_string(MAX_SIDE) } });
            return false;
        }
        const double pixels = (double)dims.w * dims.h;
        if (!memory.known)
        {
            // The memory cannot be read (the OpenGL path): up to 4K, as the snapshot window would.
            if (pixels > 3840.0 * 2160.0)
            {
                why_not = shotText("PlusPhotoShotOnly4K");
                return false;
            }
            return true;
        }
        const double needed = pixels * std::max<U32>(gSavedSettings.getU32(SETTING_BPP), 1u) / 1.0e9;
        const double room   = memory.room_gb;
        if (needed_gb)
        {
            *needed_gb = needed;
        }
        if (free_gb)
        {
            *free_gb = room;
        }
        if (needed + MARGIN_GB > room)
        {
            why_not = shotText("PlusPhotoShotNoMemory",
                                          { { "[NEEDED]", llformat("%.1f", needed) }, { "[FREE]", llformat("%.1f", room) } });
            return false;
        }
        return true;
    }

    ShotState shotState()
    {
        return sState;
    }

    float shotCoolingLeft()
    {
        if (sState != ShotState::Cooling)
        {
            return 0.f;
        }
        return (float)std::max(0.0, sCoolSeconds - (double)sCoolTimer.getElapsedTimeF32());
    }

    const ShotResult& lastShot()
    {
        return sLast;
    }

    bool takePhoto()
    {
        if (sState != ShotState::Idle)
        {
            return false; // one at a time; the window keeps the button off meanwhile
        }
        const ShotDims dims = shotDims(gSavedSettings.getS32(SETTING_SIZE), gSavedSettings.getS32(SETTING_ASPECT));
        std::string    why;
        if (!shotFits(dims, why))
        {
            sLast.w = dims.w;
            sLast.h = dims.h;
            fail(why);
            return false;
        }
        sDims   = dims;
        sFrames = 0;
        sState  = ShotState::Pending;
        doOnIdleRepeating(&step);
        return true;
    }

    void openLastShot()
    {
        if (!sLast.path.empty())
        {
            plus::openFolder(sLast.path); // opens a file with its program as well
        }
    }

    SaveDirView saveDirView()
    {
        SaveDirView view;
        view.dir = LLViewerWindow::getLastSnapshotDir();
        if (view.dir.empty())
        {
            static std::string sPictures; // the shell is asked once, not at every look
            if (sPictures.empty())
            {
                sPictures = userPicturesDir();
            }
            view.dir = sPictures;
            view.key = "save_dir_default";
        }
        return view;
    }

    void chooseSaveDir()
    {
        // (the dialog does not start at the current folder: the picker ignores the proposed name)
        (new LLDirPickerThread([](const std::vector<std::string>& folders, std::string) { onSaveDirPicked(folders); },
                               std::string()))->getFile();
    }

    bool openSaveDir()
    {
        const std::string dir = pictureDir();
        if (!LLFile::isdir(dir)) // only when pressed: a drive that is gone may take a while to answer
        {
            LL_WARNS("Plus") << "photo: the save folder is not there" << LL_ENDL;
            return false;
        }
        plus::openFolder(dir);
        return true;
    }
}
