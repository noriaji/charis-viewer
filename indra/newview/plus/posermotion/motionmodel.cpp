/**
 * @file motionmodel.cpp
 * @brief "Motion Between Poses": the model file and the machine M (see motionmodel.h).
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

#include "motionmodel.h"

#include "plus/subtitle/subtitleclient.h"   // modelFolder()

#include "llstring.h"

#include <filesystem>
#include <system_error>

namespace plus::posermotion
{
    namespace
    {
        // Where the model is published: GitHub Releases of noriaji/charis-models (research/324 section 4.1). The
        // file there is pinned by its size and SHA-256 below, and a release's files are never replaced: a new model
        // gets a new release (motion-2 ...), a new row here, and its old name goes to RETIRED. The helper pins the
        // same name and size (tools/motion-helper/src/charis_motion.cpp MODEL_FILE, MODEL_BYTES): change both.
        // The release is out (motion-1, 2026-11-13, checked by fetching it without a login: size and SHA-256 are the
        // ones below). Set false only to hold the fetch back (the address then stays empty and the window says the
        // model cannot be fetched yet).
        constexpr bool MOTION_MODEL_PUBLISHED = true;
        constexpr char MOTION_MODEL_URL[] =
            "https://github.com/noriaji/charis-models/releases/download/motion-1/mmm_v6_soma_fp32.onnx";
        constexpr char MOTION_MODEL_FILE[] = "mmm_v6_soma_fp32.onnx";   // MotionBricks v0.2.0 (commit 54fc563), unchanged
        constexpr uint64_t MOTION_MODEL_BYTES = 757864273ULL;
        constexpr char MOTION_MODEL_SHA256[] = "856419dc72aeb14e591eebf4dd0ea05734a2506ddb00bcd39dd6dac2ad60f4fb";
        // The names of models that an earlier viewer fetched and this one no longer uses (none yet).
        const char* const RETIRED[] = { nullptr };

        constexpr char PART_SUFFIX[] = ".part";   // ModelDownload's partial file

        constexpr char separator()
        {
#if LL_WINDOWS
            return '\\';
#else
            return '/';
#endif
        }

        // std::filesystem with error codes, not LLFile: LLFile's warnings put the path (the user's name) in the log
        std::filesystem::path pathOf(const std::string& utf8)
        {
            return std::filesystem::path(ll_convert_string_to_wide(utf8));
        }

        bool hasControlChar(const std::string& s)
        {
            for (const unsigned char c : s)
            {
                if (c < 0x20 || c == 0x7f)
                {
                    return true;
                }
            }
            return false;
        }

        // A plain file name: no folder in it (no separator, no ':' of a drive or a stream), not "." or "..", no
        // control character.
        bool plainFileName(const std::string& name)
        {
            return !name.empty() && name != "." && name != ".." && name.find_first_of("\\/:") == std::string::npos &&
                   !hasControlChar(name);
        }

        // A ".." between separators (or at an end): the folder would climb out of where it says it is.
        bool hasParentStep(const std::string& folder)
        {
            size_t begin = 0;
            for (;;)
            {
                const size_t end = folder.find_first_of("\\/", begin);
                const size_t len = (end == std::string::npos ? folder.size() : end) - begin;
                if (len == 2 && folder.compare(begin, 2, "..") == 0)
                {
                    return true;
                }
                if (end == std::string::npos)
                {
                    return false;
                }
                begin = end + 1;
            }
        }
    }

    const MotionModelSpec& motionModelSpec()
    {
        static const MotionModelSpec sSpec = []
        {
            MotionModelSpec spec;
            spec.url        = MOTION_MODEL_PUBLISHED ? MOTION_MODEL_URL : "";
            spec.file_name  = MOTION_MODEL_FILE;
            spec.size_bytes = MOTION_MODEL_BYTES;
            spec.sha256     = MOTION_MODEL_SHA256;
            for (const char* const* name = RETIRED; *name; ++name)
            {
                spec.retired_files.emplace_back(*name);
            }
            return spec;
        }();
        return sSpec;
    }

    std::string motionModelFolder()
    {
        return plus::subtitle::modelFolder() + separator() + "motion";
    }

    bool helperModelFolderOk(const std::string& folder, const std::string& file_name)
    {
        // charis_motion.cpp takes a folder on a drive only (a share, \\?\ or \\.\ or a relative path is refused),
        // and has no long path manifest: the model's path must stay within MAX_PATH (260 UTF-16 units with its end),
        // and so must the download's .part next to it (the longer of the two).
        constexpr size_t MAX_PATH_UNITS = 259;
        if (folder.size() < 3 || !((folder[0] >= 'A' && folder[0] <= 'Z') || (folder[0] >= 'a' && folder[0] <= 'z')) ||
            folder[1] != ':' || (folder[2] != '\\' && folder[2] != '/'))
        {
            return false;
        }
        // The folder goes onto the helper's command line in quotes: no quote in it, and no separator at its end
        // (a backslash before the closing quote would escape it). No ':' after the drive's (a stream of a file),
        // no "..", no control character.
        if (folder.find('"') != std::string::npos || folder.back() == '\\' || folder.back() == '/' ||
            folder.find(':', 2) != std::string::npos || hasParentStep(folder) || hasControlChar(folder) ||
            !plainFileName(file_name))
        {
            return false;
        }
        const std::wstring wide = ll_convert_string_to_wide(folder + separator() + file_name + PART_SUFFIX);
        return !wide.empty() && wide.size() <= MAX_PATH_UNITS;
    }

    MotionModel::MotionModel(MotionModelSpec spec, std::string folder, std::unique_ptr<ModelFetcher> fetcher)
        : mSpec(std::move(spec)), mFolder(std::move(folder)), mFetcher(std::move(fetcher))
    {
    }

    MotionModel::~MotionModel()
    {
        shutdown();
    }

    std::string MotionModel::filePath() const
    {
        return mFolder + separator() + mSpec.file_name;
    }

    double MotionModel::fraction() const
    {
        return (mState == ModelState::Downloading && mFetcher) ? mFetcher->fraction() : 0.0;
    }

    bool MotionModel::fileExists() const
    {
        std::error_code ec;
        return plainFileName(mSpec.file_name) && std::filesystem::is_regular_file(pathOf(filePath()), ec);
    }

    bool MotionModel::fileIsHere() const
    {
        if (!fileExists())
        {
            return false;
        }
        std::error_code ec;
        const uintmax_t bytes = std::filesystem::file_size(pathOf(filePath()), ec);
        return !ec && bytes == mSpec.size_bytes;
    }

    void MotionModel::refresh()
    {
        switch (mState)
        {
        case ModelState::Downloading:
        case ModelState::Verifying:
            return;   // poll() moves it on
        case ModelState::Broken:
            // the same file stays broken; one that is gone (removed by hand) is simply missing
            if (!fileExists())
            {
                mState = ModelState::Missing;
            }
            break;
        default:
            mState = fileIsHere() ? ModelState::Installed : ModelState::Missing;
            break;
        }
        if (mState != ModelState::FetchFailed)
        {
            mFailure = ModelFailure::None;
        }
    }

    bool MotionModel::removeFile(const std::string& name) const
    {
        if (!plainFileName(name))
        {
            return true;   // never a path from the table: nothing of ours to remove
        }
        std::error_code ec;
        const std::filesystem::path path = pathOf(mFolder + separator() + name);
        if (!std::filesystem::exists(path, ec))
        {
            return true;
        }
        // only a file: a folder of that name is not ours to take away
        if (!std::filesystem::is_regular_file(path, ec))
        {
            return false;
        }
        std::filesystem::remove(path, ec);
        return !ec && !std::filesystem::exists(path, ec);
    }

    bool MotionModel::removeAll()
    {
        bool gone = removeFile(mSpec.file_name);
        gone = removeFile(mSpec.file_name + PART_SUFFIX) && gone;
        for (const std::string& name : mSpec.retired_files)
        {
            gone = removeFile(name) && gone;
            gone = removeFile(name + PART_SUFFIX) && gone;
        }
        return gone;
    }

    void MotionModel::removeRetired()
    {
        for (const std::string& name : mSpec.retired_files)
        {
            removeFile(name);
            removeFile(name + PART_SUFFIX);
        }
    }

    bool MotionModel::refuseDownload(ModelFailure why)
    {
        mFailure = why;
        // a broken file that is still there stays Broken (refresh() would take it for Installed from FetchFailed)
        if (mState != ModelState::Broken)
        {
            mState = ModelState::FetchFailed;
        }
        return false;
    }

    bool MotionModel::startDownload()
    {
        if (mState == ModelState::Unknown)
        {
            refresh();   // the window did not look yet: a file that is here is not fetched again
        }
        if (mState != ModelState::Missing && mState != ModelState::FetchFailed && mState != ModelState::Broken)
        {
            return false;   // Installed, Downloading, Verifying: the window's button is off then; nothing changes
        }
        // what refuses the download comes before anything is removed
        if (!mFetcher)
        {
            return refuseDownload(ModelFailure::CannotStart);
        }
        if (!published())
        {
            return refuseDownload(ModelFailure::NotPublished);
        }
        if (!helperModelFolderOk(mFolder, mSpec.file_name))
        {
            return refuseDownload(ModelFailure::BadFolder);
        }
        if (mState == ModelState::Broken)
        {
            // "fetch again": the file that cannot be read goes first (the helper was stopped by the caller)
            if (!removeFile(mSpec.file_name))
            {
                LL_WARNS("PoserMotion") << "cannot remove the model file that could not be read (in use)" << LL_ENDL;
                mFailure = ModelFailure::InUse;
                return false;
            }
            mState = ModelState::Missing;
        }
        ModelFailure why = ModelFailure::CannotStart;
        if (!mFetcher->start(mSpec, mFolder, why))
        {
            return refuseDownload(why == ModelFailure::None ? ModelFailure::CannotStart : why);
        }
        mFailure = ModelFailure::None;
        mState = ModelState::Downloading;
        return true;
    }

    void MotionModel::cancelDownload()
    {
        if (mState != ModelState::Downloading && mState != ModelState::Verifying)
        {
            return;
        }
        if (mFetcher)
        {
            mFetcher->cancel();
            // the check may have ended (and the file got its name) just before the cancel came: a file that is
            // all here is kept, not fetched again
            if (mFetcher->progress() == ModelFetcher::Progress::Done)
            {
                finishDownload();
                return;
            }
        }
        mFailure = ModelFailure::None;
        mState = ModelState::Missing;
    }

    void MotionModel::finishDownload()
    {
        if (fileIsHere())
        {
            removeRetired();
            mFailure = ModelFailure::None;
            mState = ModelState::Installed;
            return;
        }
        // named, then gone or changed before we looked (another viewer removed it): not here
        LL_WARNS("PoserMotion") << "the model was downloaded but is not there afterwards" << LL_ENDL;
        mFailure = ModelFailure::Disk;
        mState = ModelState::FetchFailed;
    }

    bool MotionModel::remove()
    {
        cancelDownload();
        if (!removeAll())
        {
            // what could not go is still there: the state is what the file says (refresh() keeps Broken)
            LL_WARNS("PoserMotion") << "cannot remove the model files (in use)" << LL_ENDL;
            refresh();
            mFailure = ModelFailure::InUse;
            return false;
        }
        mFailure = ModelFailure::None;
        mState = ModelState::Missing;
        return true;
    }

    void MotionModel::onHelperLoadFailure(ModelLoadFailure why)
    {
        if (mState != ModelState::Installed && mState != ModelState::Unknown)
        {
            return;
        }
        mState = why == ModelLoadFailure::Unreadable ? ModelState::Broken : ModelState::Missing;
        mFailure = ModelFailure::None;
    }

    void MotionModel::poll()
    {
        if ((mState != ModelState::Downloading && mState != ModelState::Verifying) || !mFetcher)
        {
            return;
        }
        switch (mFetcher->progress())
        {
        case ModelFetcher::Progress::Running:
            mState = ModelState::Downloading;
            break;
        case ModelFetcher::Progress::Verifying:
            mState = ModelState::Verifying;
            break;
        case ModelFetcher::Progress::Done:
            finishDownload();
            break;
        case ModelFetcher::Progress::Failed:
        {
            const ModelFailure why = mFetcher->failure();
            mFailure = why == ModelFailure::None ? ModelFailure::Network : why;
            mState = ModelState::FetchFailed;
            break;
        }
        case ModelFetcher::Progress::Idle:
            // cancelled where we did not cancel it (cannot happen through this class); the .part is gone
            mFailure = ModelFailure::None;
            mState = ModelState::Missing;
            break;
        }
    }

    void MotionModel::shutdown()
    {
        cancelDownload();
    }
} // namespace plus::posermotion
