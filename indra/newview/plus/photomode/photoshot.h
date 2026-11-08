/**
 * @file photoshot.h
 * @brief Photo tools: the one-press high resolution snapshot (no snapshot
 *        window), its size and shape, and whether the GPU's memory takes it.
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

#ifndef PLUS_PHOTOMODE_PHOTOSHOT_H
#define PLUS_PHOTOMODE_PHOTOSHOT_H

#include <string>

namespace plus::photomode
{
    // The size menu: the long side of the picture. 0 is the window as it is.
    enum class ShotSize
    {
        Window = 0,
        K2,     // 1920
        K4,     // 3840
        K6,     // 5760
        K8,     // 7680, the most the viewer takes on a side (MAX_SNAPSHOT_IMAGE_SIZE)
        Count
    };
    // The shape menu. Portrait shapes make the long side the height.
    enum class ShotAspect
    {
        Window = 0,
        R16x9,
        R3x2,
        R4x3,
        R1x1,
        R9x16,
        R2x3,
        Count
    };
    constexpr int SHOT_SIZE_COUNT   = static_cast<int>(ShotSize::Count);
    constexpr int SHOT_ASPECT_COUNT = static_cast<int>(ShotAspect::Count);

    struct ShotDims
    {
        int w = 0;
        int h = 0;
        double megapixels() const { return (double)w * h / 1.0e6; }
    };

    // The width over the height of a shape of the menu (PlusPhotoShotAspect), 0 for the window's own shape.
    double shotAspectRatio(int aspect);

    // The picture a size and a shape make, for the window as it is now.
    ShotDims shotDims(int size, int aspect);

    // Whether the GPU's memory takes a picture of this size on top of what is in use now. An estimate
    // (PlusPhotoShotBytesPerPixel for the targets of the picture: the screen set, the shadow maps and the
    // order-independent-transparency buffers; research/109), against the driver's budget. False with
    // a reason for the user (from strings.xml, LLTrans) when it does not, or when the memory cannot be
    // read (then only up to 4K).
    bool shotFits(const ShotDims& dims, std::string& why_not, double* needed_gb = nullptr, double* free_gb = nullptr);

    // The GPU's memory as the driver tells it now, asked once for several sizes (the size menu).
    struct ShotMemory
    {
        bool   known   = false; // false: it cannot be read (the OpenGL path)
        double room_gb = 0.0;   // the budget less what is in use
    };
    ShotMemory shotMemory();
    // shotFits() against memory asked before.
    bool shotFits(const ShotDims& dims, const ShotMemory& memory, std::string& why_not, double* needed_gb = nullptr,
                  double* free_gb = nullptr);

    // What the one-press shot is doing.
    enum class ShotState
    {
        Idle,
        Pending,  // asked; the window shows it once, then the picture is taken
        Busy,     // being taken (the viewer stands still)
        Cooling   // taken; the next one waits until the memory is back and a few seconds have passed
    };
    ShotState shotState();
    // Seconds left of Cooling (0 when it is the memory that is awaited).
    float shotCoolingLeft();

    struct ShotResult
    {
        unsigned    number = 0; // counts up with every try, so the window sees a new one
        bool        ok = false;
        std::string path;       // the file saved
        std::string error;      // for the user (LLTrans), when not ok
        int         w = 0, h = 0;
        int         ms = 0;     // how long it took, in ms
        int         bytes = 0;  // the size of the file, KB
    };
    const ShotResult& lastShot();

    // Takes the picture with the size and the shape that are set (PlusPhotoShotSize, PlusPhotoShotAspect), the
    // format and quality of the snapshot window (SnapshotFormat, SnapshotQuality), into the folder it saves to
    // (SnapshotBaseDir; the Pictures folder when none was chosen yet). False if one is being taken or the
    // memory cannot take this size (lastShot() says so). Not at once: the picture is taken a few frames later.
    bool takePhoto();

    void openLastShot();

    // The folder the photos are saved in: SnapshotBaseDir (shared with the snapshot window), else the Pictures
    // folder. For showing it: nothing is made or looked at on disk (a folder on a network drive that is gone
    // would hold the viewer up every tick). key names the UI string to show it with ("[DIR]" is the folder):
    // "save_dir" when one was chosen, "save_dir_default" when not.
    struct SaveDirView
    {
        std::string dir;
        const char* key = "save_dir";
    };
    SaveDirView saveDirView();
    // Asks for a folder (the system's folder dialog) and keeps it as SnapshotBaseDir. Nothing changes on cancel.
    void chooseSaveDir();
    // Opens the folder the photos are saved in, photo taken or not (the Pictures folder is made first when none
    // was chosen). False when a chosen folder is not there any more: the caller says so.
    bool openSaveDir();
}

#endif // PLUS_PHOTOMODE_PHOTOSHOT_H
