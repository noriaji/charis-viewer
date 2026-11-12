/**
 * @file updatestate.cpp
 * @brief The state of an update the user agreed to (plus/update).
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

#include "updatestate.h"

#include <algorithm>

namespace plus::update
{
    const char* phaseName(Phase phase)
    {
        switch (phase)
        {
        case Phase::Idle:        return "idle";
        case Phase::Checking:    return "checking";
        case Phase::Downloading: return "downloading";
        case Phase::Ready:       return "ready";
        case Phase::Failed:      return "failed";
        }
        return "?";
    }

    const char* failureName(Failure failure)
    {
        switch (failure)
        {
        case Failure::None:            return "none";
        case Failure::NoUpdateInFeed:  return "no newer version in the feed";
        case Failure::FeedUnreadable:  return "the feed could not be read";
        case Failure::DownloadFailed:  return "the download failed";
        case Failure::PackageMismatch: return "the package is not the one the feed names";
        case Failure::HandOffFailed:   return "the installer did not take the package";
        case Failure::VersionMismatch: return "the feed offers another version than the one agreed to";
        }
        return "?";
    }

    uint64_t wholeMB(uint64_t bytes)
    {
        return bytes / (uint64_t(1024) * 1024);
    }

    bool Machine::begin(const std::string& version)
    {
        if (mPhase != Phase::Idle && mPhase != Phase::Failed)
        {
            return false;
        }
        mPhase = Phase::Checking;
        mFailure = Failure::None;
        mVersion = version;
        mDoneBytes = 0;
        mTotalBytes = 0;
        mCancelRequested = false;
        mReadyHidden = false;
        mRestartRequested = false;
        changed();
        return true;
    }

    bool Machine::downloading(uint64_t total_bytes)
    {
        if (mPhase != Phase::Checking)
        {
            return false;
        }
        mPhase = Phase::Downloading;
        mDoneBytes = 0;
        mTotalBytes = total_bytes;
        changed();
        return true;
    }

    bool Machine::progress(uint64_t done_bytes, uint64_t total_bytes)
    {
        if (mPhase != Phase::Downloading)
        {
            return false;
        }
        const uint64_t total = total_bytes ? total_bytes : mTotalBytes;
        const uint64_t done = total ? std::min(done_bytes, total) : done_bytes;
        const bool shown_changed = total != mTotalBytes
            || wholeMB(done) != wholeMB(mDoneBytes)
            || (total && done * 100 / total != mDoneBytes * 100 / total);
        mDoneBytes = done;
        mTotalBytes = total;
        if (shown_changed)
        {
            changed();
        }
        return true;
    }

    bool Machine::ready()
    {
        if ((mPhase != Phase::Checking && mPhase != Phase::Downloading) || mCancelRequested)
        {
            return false;
        }
        mPhase = Phase::Ready;
        mDoneBytes = mTotalBytes;
        mReadyHidden = false;
        mRestartRequested = false;
        changed();
        return true;
    }

    bool Machine::failed(Failure why)
    {
        if ((mPhase != Phase::Checking && mPhase != Phase::Downloading) || why == Failure::None || mCancelRequested)
        {
            return false;
        }
        mPhase = Phase::Failed;
        mFailure = why;
        mCancelRequested = false;
        changed();
        return true;
    }

    bool Machine::requestCancel()
    {
        if ((mPhase != Phase::Checking && mPhase != Phase::Downloading) || mCancelRequested)
        {
            return false;
        }
        mCancelRequested = true;
        changed();
        return true;
    }

    bool Machine::cancelled()
    {
        if (mPhase != Phase::Checking && mPhase != Phase::Downloading)
        {
            return false;
        }
        mPhase = Phase::Idle;
        mCancelRequested = false;
        mDoneBytes = 0;
        mTotalBytes = 0;
        changed();
        return true;
    }

    bool Machine::clear()
    {
        if (mPhase != Phase::Failed && mPhase != Phase::Ready)
        {
            return false;
        }
        mPhase = Phase::Idle;
        mFailure = Failure::None;
        mReadyHidden = false;
        mRestartRequested = false;
        mDoneBytes = 0;
        mTotalBytes = 0;
        changed();
        return true;
    }

    bool Machine::hideReady()
    {
        if (mPhase != Phase::Ready || mReadyHidden)
        {
            return false;
        }
        mReadyHidden = true;
        changed();
        return true;
    }

    bool Machine::requestRestart()
    {
        if (mPhase != Phase::Ready || mRestartRequested)
        {
            return false;
        }
        mRestartRequested = true;
        changed();
        return true;
    }

    bool Machine::cancelRestart()
    {
        if (mPhase != Phase::Ready || !mRestartRequested)
        {
            return false;
        }
        mRestartRequested = false;
        changed();
        return true;
    }

    int Machine::percent() const
    {
        if (mTotalBytes == 0)
        {
            return 0;
        }
        return static_cast<int>(std::min<uint64_t>(mDoneBytes, mTotalBytes) * 100 / mTotalBytes);
    }

    bool Machine::chipWanted() const
    {
        switch (mPhase)
        {
        case Phase::Idle:   return false;
        case Phase::Ready:  return !mReadyHidden;
        default:            return true;
        }
    }
}
