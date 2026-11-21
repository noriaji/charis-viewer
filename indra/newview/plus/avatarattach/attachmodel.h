/**
 * @file attachmodel.h
 * @brief The attachments window's work, apart from how it is shown (shared by the XUI and the RmlUi window).
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

// What avatarattach.h describes (finding the avatar, asking the simulator for the names, the refresh once a second,
// the buttons' actions) is here; a window (avatarattach.cpp: the XUI one, frozen; attachrml.cpp: the RmlUi one,
// research/386) gives it a View: where the title, the status line and the rows go, and the window's words. The
// window calls open() / stopAsking() / tick() and the button actions; it keeps the selected row's id itself.
// Main thread only.

#ifndef PLUS_AVATARATTACH_ATTACHMODEL_H
#define PLUS_AVATARATTACH_ATTACHMODEL_H

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/signals2.hpp>

#include "llframetimer.h"
#include "llstring.h"
#include "lluuid.h"

class LLVOAvatar;
class LLViewerJointAttachment;
class LLViewerObject;

namespace plus::avatarattach
{
    // One row of the list, as it is shown.
    struct Row
    {
        LLUUID id;
        std::string point;
        std::string name;
        std::string creator;
        S32 prims = 1;
        S32 distance = -1;      // metres; -1: a HUD attachment

        bool operator==(const Row& other) const
        {
            return id == other.id && prims == other.prims && distance == other.distance
                && point == other.point && name == other.name && creator == other.creator;
        }
    };

    class SimSelection;

    class AttachmentsModel
    {
    public:
        class View
        {
        public:
            virtual ~View() = default;
            // One of the window's <string>s (args: [COUNT], [NAME], [POINT]).
            virtual std::string text(const char* id, const LLStringUtil::format_map_t& args) = 0;
            virtual void setTitle(const std::string& title) = 0;
            // The status line and the list's empty line: called when the text changes only.
            virtual void setStatus(const std::string& text) = 0;
            virtual void setEmptyText(const std::string& text) = 0;
            virtual void clearRows() = 0;
            // now: the rows to show. was: what was shown. same_objects: the same objects in the same order (the
            // cells that changed are enough); then was is a valid row for each of now, one to one.
            virtual void showRows(const std::vector<Row>& now, const std::vector<Row>& was, bool same_objects) = 0;
        };

        explicit AttachmentsModel(View& view);
        ~AttachmentsModel();
        AttachmentsModel(const AttachmentsModel&) = delete;
        AttachmentsModel& operator=(const AttachmentsModel&) = delete;

        // The window's words (loading, not available, no name, HUD): from the window's postBuild.
        void init();
        // The window opens for an avatar (the same one again keeps what was asked).
        void open(const LLUUID& avatar_id);
        // No answer is waited for any more (the window closes, shows another avatar or goes). Idempotent.
        void stopAsking();
        // Every frame the window is open: the list is made again once a second, or at once when a name came.
        // True when it was made again now (the window updates its buttons then).
        bool tick();

        struct Buttons
        {
            bool copy_name = false;
            bool copy_id = false;
            bool inspect = false;
        };
        Buttons buttons(const LLUUID& selected) const;
        void copyName(const LLUUID& selected) const;
        void copyId(const LLUUID& selected) const;
        void inspect(const LLUUID& selected) const;
        // "Refresh": the list at once, and the names that failed are asked for again.
        void askAgain();
        // A row's distance cell: the metres, or the window's "HUD".
        std::string distanceText(const Row& row) const;

        // An ObjectProperties answer for the open window (the hook): true when it had selected the object.
        static bool dispatchObjectProperties(const LLUUID& object_id, const LLUUID& creator_id, const std::string& name);

    private:
        // What the simulator told about one attached object.
        struct Props
        {
            enum class State { Need, Sent, Received, Failed };
            State state = State::Need;
            S32 tries = 0;          // ObjectSelects sent
            F64 since = 0.0;        // viewer seconds: first seen, or "Refresh" pressed
            F64 sentAt = 0.0;       // viewer seconds: the last ObjectSelect
            F64 notBefore = 0.0;    // viewer seconds: not asked again before this
            std::string name;
            LLUUID creatorId;
        };

        bool onObjectProperties(const LLUUID& object_id, const LLUUID& creator_id, const std::string& name);
        LLVOAvatar* findAvatar() const;
        std::string residentName(const LLUUID& id);
        std::string shownName(const LLUUID& id);
        const std::string& pointName(const LLViewerJointAttachment& point);
        const std::string& nameText(const Props& props) const;
        void setStatus(const std::string& text);
        void setEmpty(const std::string& text);
        void updateTitle();
        void refresh();
        void collect(LLVOAvatar& avatar);
        void expireAnswers(F64 now);
        void askNames();
        void fillNames();
        void show();
        void pruneProps();
        const Row* rowOf(const LLUUID& id) const;

        View& mView;
        std::string mTextLoading, mTextUnavailable, mTextNoName, mTextHud;

        LLUUID mAvatarId;               // never a pointer: the avatar is found again each refresh
        bool mGone = false;
        bool mDirty = false;
        LLFrameTimer mRefreshTimer;
        S32 mInFlight = 0;

        std::map<LLUUID, Props> mProps;
        std::unique_ptr<SimSelection> mSelection;   // whatever it still holds is deselected when the window goes
        std::vector<Row> mRows;                     // what the list shows
        std::vector<Row> mScratch;                  // the next rows (kept to reuse its room)
        std::vector<LLViewerObject*> mObjects;      // mScratch's objects, during refresh() only
        std::unordered_map<LLUUID, size_t> mIndexById;
        std::map<std::string, std::string> mPointNames;    // the point's name as shown, by its name
        std::map<LLUUID, boost::signals2::connection> mNameRequests;

        std::string mStatusShown, mTitleName, mCommentShown;
    };
}

#endif // PLUS_AVATARATTACH_ATTACHMODEL_H
