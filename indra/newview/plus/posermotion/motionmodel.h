/**
 * @file motionmodel.h
 * @brief "Motion Between Poses" (research/324 section 4, 5.1 M): the model file, whether it is here, its download
 * and its removal. The machine M; no window, no helper.
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

// The model (NVIDIA MotionBricks SOMA, one ONNX file, NVIDIA Open Model Agreement) is not shipped. It is fetched
// from noriaji/charis-models only when the user presses the window's button and confirms (research/324 section
// 4.2): nothing here goes out by itself, not even to look for an update. The download is plus/subtitle's
// ModelDownload (curl.exe of Windows, https only, a .part file named only after its size and SHA-256 match).
// "Here" means the file is in motionModelFolder() with the size of the table (no hash at every start, as with the
// translation models); the helper checks the size again before it reads the file, and says when it cannot read it
// (Broken, below).
//
// Main thread only. The viewer keeps one MotionModel for the feature (motionModel(), posermotion.h): the window may
// be closed while it downloads, and the download goes on (a timer of its own moves it on).

#ifndef PLUS_POSERMOTION_MOTIONMODEL_H
#define PLUS_POSERMOTION_MOTIONMODEL_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace plus::posermotion
{
    /// What the model is, and where it comes from. The viewer's is motionModelSpec() (one table, motionmodel.cpp).
    struct MotionModelSpec
    {
        std::string              url;          // https; empty while the release is not published (download: NotPublished)
        std::string              file_name;    // in the model folder; the helper looks for the same name (charis_motion.cpp)
        uint64_t                 size_bytes = 0;
        std::string              sha256;       // lower case hex
        std::vector<std::string> retired_files; // names of earlier versions: removed (with their .part) after a download, and by remove()
    };

    /// The built-in table (one place: motionmodel.cpp). Never a setting: the address is not taken from outside.
    const MotionModelSpec& motionModelSpec();

    /// The states of research/324 section 5.1 (M?, M0 .. M5).
    enum class ModelState : uint8_t
    {
        Unknown,     // M?  not looked yet (refresh() looks)
        Missing,     // M0  no file, or not the size of the table
        Downloading, // M1
        Verifying,   // M2  size and SHA-256 of the downloaded file
        Installed,   // M3
        FetchFailed, // M4  failure() says why
        Broken,      // M5  the right size, but the helper could not read it
    };

    /// Why the last download or removal did not go (the window has its own words for each).
    enum class ModelFailure : uint8_t
    {
        None,
        NotPublished, // the release is not there yet (the table has no address)
        CannotStart,  // curl.exe is missing or could not be started
        BadFolder,    // the folder cannot be made, or is not a folder the helper can be given (a drive path, short enough)
        Network,      // curl gave up (no connection, an HTTP error, too slow)
        Size,         // the download was not the size of the table
        Hash,         // the download did not match its SHA-256
        Disk,         // the file could not be written or named (disk full, ...)
        InUse,        // a file could not be removed (another viewer has it open): the state stays
    };

    /// What the helper said about the file (its "reason" of kind "model"): Missing and Size are M0, Unreadable M5.
    enum class ModelLoadFailure : uint8_t
    {
        Missing,
        Size,
        Unreadable,
    };

    /// The download itself, behind a face of its own so that the machine can be tried without the network
    /// (the viewer's is plus::subtitle::ModelDownload, posermotion.cpp; a test has a fake).
    class ModelFetcher
    {
    public:
        enum class Progress : uint8_t
        {
            Idle, // never started, or cancelled (the .part is gone)
            Running,
            Verifying,
            Done, // the file has its name
            Failed,
        };

        virtual ~ModelFetcher() = default;
        /// False (and why, failure()) when it cannot start; it stays Idle then.
        virtual bool         start(const MotionModelSpec& spec, const std::string& folder, ModelFailure& failure) = 0;
        virtual void         cancel() = 0;   // removes the .part; safe at any time
        virtual Progress     progress() const = 0;
        virtual double       fraction() const = 0;   // 0..1 while Running
        virtual ModelFailure failure() const = 0;    // when Failed (Network, Size, Hash or Disk)
    };

    class MotionModel
    {
    public:
        /// folder: where the file is kept (motionModelFolder() in the viewer).
        MotionModel(MotionModelSpec spec, std::string folder, std::unique_ptr<ModelFetcher> fetcher);
        ~MotionModel();   // cancels a download
        MotionModel(const MotionModel&) = delete;
        MotionModel& operator=(const MotionModel&) = delete;

        ModelState   state() const { return mState; }
        /// Why FetchFailed, why a "fetch again" from Broken was refused (Broken stays, with the reason), or why the
        /// last remove() / startDownload() could not remove a file (InUse); None otherwise.
        ModelFailure failure() const { return mFailure; }
        /// 0..1 while Downloading, else 0.
        double       fraction() const;
        uint64_t     sizeBytes() const { return mSpec.size_bytes; }
        /// The release is out (the table has an address): the window may offer the download.
        bool         published() const { return !mSpec.url.empty(); }
        const std::string& folder() const { return mFolder; }
        /// The model file's full path (the folder and the table's name).
        std::string  filePath() const;

        /// When the window opens: looks at the file (size only). Unknown, Missing, Installed and FetchFailed become
        /// Missing or Installed; Broken stays unless the file is gone (then Missing); Downloading and Verifying are
        /// left to poll(). Forgets the failure of a remove().
        void refresh();

        /// After the user confirmed (research/324 section 4.2): from Missing, FetchFailed or Broken (Unknown looks at
        /// the file first, refresh()). True when it started (Downloading). False when it did not: Installed,
        /// Downloading and Verifying change nothing; a download that cannot be (no address, the folder, no curl)
        /// is FetchFailed with failure() saying why (from Broken: stays Broken with the reason, the file kept),
        /// checked before anything is removed; Broken ("fetch again") then removes the broken file, and stays Broken
        /// with InUse when it cannot. The helper must not be running (stop it first: it may hold the file).
        bool startDownload();
        /// Downloading or Verifying: stops, removes the .part, Missing. When the download ended just before the
        /// cancel: Installed if the file is here, else FetchFailed (Disk). Safe in any state.
        void cancelDownload();
        /// Removes the model file, the .part and the retired files (the helper must not be running). True and
        /// Missing when they are gone; false and InUse when one could not be removed (the state stays). A download
        /// that runs is cancelled first.
        bool remove();

        /// The helper could not use the file (call it after the helper was stopped). Installed or Unknown: Missing
        /// (Missing, Size) or Broken (Unreadable). Other states: nothing (a download is under way, or it is known).
        void onHelperLoadFailure(ModelLoadFailure why);

        /// Moves a download on (Downloading -> Verifying -> Installed / FetchFailed / Missing). The viewer's timer
        /// calls it while a download runs; calling it at other times does nothing.
        void poll();
        /// The viewer quits: a download is cancelled. Safe to call twice.
        void shutdown();

    private:
        bool fileIsHere() const;          // the file with the table's size
        bool fileExists() const;          // the file, any size
        bool removeFile(const std::string& name) const;   // true when it is gone (or was never there)
        bool removeAll();                 // the file, its .part, the retired files and theirs
        void removeRetired();             // after a download: the retired files and their .part (failures ignored)
        void finishDownload();            // the download is Done: Installed when the file is here, else FetchFailed (Disk)
        bool refuseDownload(ModelFailure why);   // FetchFailed with why; returns false

        MotionModelSpec               mSpec;
        std::string                   mFolder;
        std::unique_ptr<ModelFetcher> mFetcher;
        ModelState                    mState   = ModelState::Unknown;
        ModelFailure                  mFailure = ModelFailure::None;
    };

    /// The folder the model is kept in: <subtitle::modelFolder()>/motion (one place for the models of the viewer,
    /// research/324 section 2.6). Not created here.
    std::string motionModelFolder();

    /// A folder the helper takes (charis_motion.cpp --models): a drive path ("C:\..."), no quote, no trailing
    /// separator, no ':' after the drive's, no "..", no control character, and short enough that the folder and
    /// file_name + ".part" stay within MAX_PATH. file_name: a plain name. folder is UTF-8.
    bool helperModelFolderOk(const std::string& folder, const std::string& file_name);
} // namespace plus::posermotion

#endif // PLUS_POSERMOTION_MOTIONMODEL_H
