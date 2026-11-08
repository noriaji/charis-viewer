/**
 * @file invsource.h
 * @brief Keeps an invindex Index in step with the viewer's inventory (gInventory).
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

// Main thread only. start() adds an inventory observer that only remembers
// which ids changed; sync() (called before each query) applies the changes to
// the index, or rebuilds it from scratch on the first call, after a re-login
// (the root folder changed) or when too much changed at once.

#ifndef PLUS_INVFINDER_INVSOURCE_H
#define PLUS_INVFINDER_INVSOURCE_H

#include "invindex.h"

#include <memory>
#include <string>
#include <vector>

namespace plus::invfinder
{
    class InvSource
    {
    public:
        static InvSource& instance();

        void start();
        void stop();

        // False while the viewer's inventory is not usable yet (logging in).
        bool sync();

        const Index& index() const { return mIndex; }

        struct Status
        {
            bool        usable = false;
            bool        everythingFetched = false;
            bool        fetchBusy = false; // the background fetch still has work
            std::size_t unfetchedFolders = 0;
            std::vector<std::string> unfetchedExamples; // paths, a few
            std::size_t entries = 0;
            std::size_t folders = 0;
            std::uint64_t generation = 0;
            double      lastRebuildMs = 0;
            double      lastSyncMs = 0;
            bool        rebuiltLastSync = false;
        };
        Status status();

        // The creator's name if the viewer has it, else the id text. Asks for
        // missing names in the background. `known` tells which one it was.
        std::string creatorName(const std::string& id, bool* known = nullptr) const;

        ~InvSource();

    private:
        InvSource();
        class Observer;

        void onChanged(unsigned mask);
        void rebuildAll();
        void applyPending();

        Index mIndex;
        std::unique_ptr<Observer> mObserver;
        struct Pending;
        std::unique_ptr<Pending> mPending;
        bool mNeedRebuild = true;
        std::string mRootId;
        std::string mLibraryId;
        double mLastRebuildMs = 0;
        double mLastSyncMs = 0;
        bool mRebuiltLastSync = false;
    };
}

#endif // PLUS_INVFINDER_INVSOURCE_H
