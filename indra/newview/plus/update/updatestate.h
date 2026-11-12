/**
 * @file updatestate.h
 * @brief The state of an update the user agreed to (plus/update): looking at the
 *        feed, downloading, ready to install, failed. No viewer code here, so the
 *        test (updatestate_test.cpp) builds on its own.
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

#ifndef PLUS_UPDATE_UPDATESTATE_H
#define PLUS_UPDATE_UPDATESTATE_H

#include <cstdint>
#include <string>

// research/326 section 8 has the table of states this follows.
namespace plus::update
{
    enum class Phase
    {
        Idle,           // nothing going on (also after a cancel)
        Checking,       // the user agreed: the update feed is being read
        Downloading,    // the package is coming
        Ready,          // downloaded and handed to the installer: put in when the viewer ends
        Failed,         // stopped on an error; Retry starts again
    };

    enum class Failure
    {
        None,
        NoUpdateInFeed,     // the feed has nothing newer than the running version
        FeedUnreadable,     // no feed, or one that did not pass the checks
        DownloadFailed,     // the package could not be fetched or written
        PackageMismatch,    // what came is not the size or the SHA-256 the feed gives
        HandOffFailed,      // the installer did not take the package
        VersionMismatch,    // the feed offers another version than the one the user agreed to
    };

    const char* phaseName(Phase phase);
    const char* failureName(Failure failure);

    // The whole state, one copy, on the main thread. Every change that the chip shows
    // adds one to serial(); the progress only when the whole percent or the whole MB
    // changes, so the chip writes its words again at most about 100 times a download.
    // A step that does not fit the state (progress while idle, ready twice) does
    // nothing and returns false.
    class Machine
    {
    public:
        // Idle or Failed -> Checking. version is the words of the version agreed to
        // ("0.1.0 alpha 2 (rev 3300)"), kept for the chip.
        bool begin(const std::string& version);
        // Checking -> Downloading; total_bytes 0 when not known.
        bool downloading(uint64_t total_bytes);
        // Downloading: done_bytes of total_bytes (clamped to it; total 0 keeps the one known).
        bool progress(uint64_t done_bytes, uint64_t total_bytes);
        // Checking or Downloading -> Ready. False while a cancel is asked for (the user took
        // the agreement back: the caller throws the update away and reports cancelled()).
        bool ready();
        // Checking or Downloading -> Failed. False while a cancel is asked for (the caller reports
        // cancelled(): the user stopped it, no Retry is offered).
        bool failed(Failure why);
        // Checking or Downloading: the user asked to stop; the backend says when it has
        // (cancelled()). Asked twice: false the second time.
        bool requestCancel();
        // Checking or Downloading -> Idle (stopped by the user or by a setting).
        bool cancelled();
        // Failed -> Idle (x on the chip); Ready -> Idle (the update thrown away: a setting
        // turned updates off).
        bool clear();
        // Ready: the chip is hidden (the update still goes in when the viewer ends).
        bool hideReady();
        // Ready: "Restart and update" was pressed (once; the viewer is quitting).
        bool requestRestart();
        // Ready: the quit was called off (the user said no to logging out): the button is back.
        bool cancelRestart();

        Phase       phase() const { return mPhase; }
        Failure     failure() const { return mFailure; }
        const std::string& version() const { return mVersion; }
        uint64_t    doneBytes() const { return mDoneBytes; }
        uint64_t    totalBytes() const { return mTotalBytes; }
        bool        cancelRequested() const { return mCancelRequested; }
        bool        readyHidden() const { return mReadyHidden; }
        bool        restartRequested() const { return mRestartRequested; }
        uint32_t    serial() const { return mSerial; }
        // 0..100, 0 while the total is not known.
        int         percent() const;
        // Not Idle: an update the user agreed to is on its way, waiting, or failed (its chip tells of it).
        bool        engaged() const { return mPhase != Phase::Idle; }
        // The chip is wanted: Checking, Downloading, Failed, or Ready and not hidden.
        bool        chipWanted() const;

    private:
        void changed() { ++mSerial; }

        Phase       mPhase = Phase::Idle;
        Failure     mFailure = Failure::None;
        std::string mVersion;
        uint64_t    mDoneBytes = 0;
        uint64_t    mTotalBytes = 0;
        bool        mCancelRequested = false;
        bool        mReadyHidden = false;
        bool        mRestartRequested = false;
        uint32_t    mSerial = 0;
    };

    // Whole MB (1024 * 1024 bytes), rounded down.
    uint64_t wholeMB(uint64_t bytes);
}

#endif
