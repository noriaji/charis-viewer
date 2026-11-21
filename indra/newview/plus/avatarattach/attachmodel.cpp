/**
 * @file attachmodel.cpp
 * @brief The attachments window's work, apart from how it is shown (research/386).
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

// See attachmodel.h.

#include "llviewerprecompiledheaders.h"

#include "attachmodel.h"
#include "plus/rlv/rlvhooks.h"

#include <memory>

#include "llagent.h"
#include "llapp.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llclipboard.h"
#include "llfloaterreg.h"
#include "llhost.h"
#include "llinventorymodel.h"
#include "message.h"
#include "llselectmgr.h"
#include "lltrans.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "llworld.h"

namespace plus::avatarattach
{
    namespace
    {
        constexpr F32 REFRESH_SECONDS = 1.f;    // the list is made again this often while the window is open
        constexpr F64 ANSWER_SECONDS  = 6.0;    // no answer in this time: asked again (or given up)
        constexpr F64 RETRY_SECONDS   = 3.0;    // the wait before asking again, times the tries made
        constexpr S32 MAX_TRIES       = 3;      // then "not available" (until "Refresh")
        constexpr F64 NO_REGION_SECONDS = 20.0; // an object with no region this long: "not available"
        constexpr S32 MAX_IN_FLIGHT   = 8;      // objects asked about and not answered yet, at most

    }

    // The objects selected on the simulator only (ObjectSelect), so that it sends their
    // ObjectProperties as it does for "Inspect", without the viewer's selection: nothing goes
    // into LLSelectMgr, nothing is highlighted, the agent does not point at it. Each one is
    // deselected by release(), and all that are left when this goes (RAII: the window closes,
    // shows another avatar or is destroyed) - but never one the user has selected in the
    // meantime (LLSelectMgr deselects that one itself when the user lets it go).
    class SimSelection
    {
    public:
        SimSelection() = default;
        SimSelection(const SimSelection&) = delete;
        SimSelection& operator=(const SimSelection&) = delete;
        ~SimSelection() { releaseAll(); }

        // False when the object's region is not there (nothing sent).
        bool select(const LLViewerObject& object)
        {
            LLViewerRegion* region = object.getRegion();
            if (!region || !gMessageSystem || LLApp::isExiting())
            {
                return false;
            }
            Held& held = mHeld[object.getID()];     // again for one held: its address now
            held.host = region->getHost();
            held.localId = object.getLocalID();
            send(_PREHASH_ObjectSelect, held);
            return true;
        }

        // True when it was held (selected by this and not released yet).
        bool release(const LLUUID& id)
        {
            auto it = mHeld.find(id);
            if (it == mHeld.end())
            {
                return false;
            }
            const Held held = it->second;
            mHeld.erase(it);
            deselect(id, held);
            return true;
        }

        void releaseAll()
        {
            for (const auto& entry : mHeld)
            {
                deselect(entry.first, entry.second);
            }
            mHeld.clear();
        }

    private:
        struct Held
        {
            LLHost host;
            U32 localId = 0;
        };

        static void deselect(const LLUUID& id, const Held& held)
        {
            // The user's selection keeps it selected; the simulator forgets it at logout.
            if (LLApp::isExiting() || !gMessageSystem || !LLWorld::instanceExists()
                || !LLWorld::getInstance()->getRegion(held.host))
            {
                return;
            }
            const LLViewerObject* object = gObjectList.findObject(id);
            if (object && object->isSelected())
            {
                return;
            }
            send(_PREHASH_ObjectDeselect, held);
        }

        static void send(const char* message, const Held& held)
        {
            LLMessageSystem* msg = gMessageSystem;
            msg->newMessageFast(message);
            msg->nextBlockFast(_PREHASH_AgentData);
            msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
            msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            msg->nextBlockFast(_PREHASH_ObjectData);
            msg->addU32Fast(_PREHASH_ObjectLocalID, held.localId);
            msg->sendReliable(held.host);
        }

        std::unordered_map<LLUUID, Held> mHeld;
    };

    namespace
    {
        void copyText(const std::string& text)
        {
            if (text.empty())
            {
                return;
            }
            const LLWString wide = utf8str_to_wstring(text);
            LLClipboard::instance().copyToClipboard(wide, 0, static_cast<S32>(wide.length()));
        }

        AttachmentsModel* sOpen = nullptr;    // the window's model while it is open (single instance)
    }

    AttachmentsModel::AttachmentsModel(View& view) : mView(view), mSelection(new SimSelection) {}

    AttachmentsModel::~AttachmentsModel()
    {
        stopAsking();
    }

    void AttachmentsModel::init()
    {
        const LLStringUtil::format_map_t none;
        mTextLoading     = mView.text("loading", none);
        mTextUnavailable = mView.text("name_unavailable", none);
        mTextNoName      = mView.text("no_name", none);
        mTextHud         = mView.text("hud_distance", none);
    }

    void AttachmentsModel::open(const LLUUID& id)
    {
        if (id != mAvatarId)
        {
            // Another avatar: nothing of the last one stays.
            stopAsking();
            mAvatarId = id;
            mProps.clear();
            mRows.clear();
            mView.clearRows();
            mGone = false;
        }
        sOpen = this;   // after stopAsking(), which lets go of it
        refresh();
    }

    bool AttachmentsModel::tick()
    {
        if (mDirty || mRefreshTimer.getElapsedTimeF32() >= REFRESH_SECONDS)
        {
            refresh();
            return true;
        }
        return false;
    }

    bool AttachmentsModel::dispatchObjectProperties(const LLUUID& object_id, const LLUUID& creator_id, const std::string& name)
    {
        return sOpen && sOpen->onObjectProperties(object_id, creator_id, name);
    }

    // An ObjectProperties answer: only the ones this window asked about are taken (a late
    // answer after a timeout too). True when this window had the object selected.
    bool AttachmentsModel::onObjectProperties(const LLUUID& object_id, const LLUUID& creator_id, const std::string& name)
    {
        const bool held = mSelection->release(object_id);
        auto it = mProps.find(object_id);
        if (it == mProps.end() || it->second.state == Props::State::Received)
        {
            return held;
        }
        Props& props = it->second;
        if (props.state == Props::State::Sent)
        {
            mInFlight = llmax(0, mInFlight - 1);
        }
        props.state     = Props::State::Received;
        props.name      = name;
        props.creatorId = creator_id;
        mDirty = true;
        return held;
    }

    // No answer is waited for any more (the window closes or shows another avatar).
    void AttachmentsModel::stopAsking()
    {
        if (sOpen == this)
        {
            sOpen = nullptr;
        }
        for (auto& request : mNameRequests)
        {
            request.second.disconnect();
        }
        mNameRequests.clear();
        mSelection->releaseAll();
        for (auto& entry : mProps)
        {
            if (entry.second.state == Props::State::Sent)
            {
                entry.second.state = Props::State::Need;  // asked again if the window opens again
                entry.second.notBefore = 0.0;
            }
        }
        mInFlight = 0;
    }

    LLVOAvatar* AttachmentsModel::findAvatar() const
    {
        LLViewerObject* object = mAvatarId.notNull() ? gObjectList.findObject(mAvatarId) : nullptr;
        return (object && object->isAvatar() && !object->isDead()) ? static_cast<LLVOAvatar*>(object) : nullptr;
    }

    // The display name of a resident from the name cache; asked for once when not there yet.
    std::string AttachmentsModel::residentName(const LLUUID& id)
    {
        LLAvatarName av_name;
        if (LLAvatarNameCache::get(id, &av_name))
        {
            return av_name.getDisplayName();
        }
        if (mNameRequests.find(id) == mNameRequests.end())
        {
            mNameRequests[id] = LLAvatarNameCache::get(id, [this](const LLUUID&, const LLAvatarName&) { mDirty = true; });
        }
        return mTextLoading;
    }

    // residentName() as the window shows it: RLVa's @shownames hides the wearer's name, in the title and as
    // the creator of what it wears (plus/rlv, research/248 3.4). Firestorm has no such window.
    std::string AttachmentsModel::shownName(const LLUUID& id)
    {
        std::string name = residentName(id);
        if (plus::rlv::enabled() && id == mAvatarId && name != mTextLoading)
        {
            name = plus::rlv::avatarNameForPlus(id, name);
        }
        return name;
    }

    const std::string& AttachmentsModel::pointName(const LLViewerJointAttachment& point)
    {
        // By the point's name (the same for every avatar), not its address: the points
        // of an avatar that went may be at the same addresses as another's.
        auto it = mPointNames.find(point.getName());
        if (it != mPointNames.end())
        {
            return it->second;
        }
        std::string name;
        if (!LLTrans::findString(name, point.getName()))
        {
            name = point.getName();
        }
        if (point.getIsHUDAttachment())
        {
            LLStringUtil::format_map_t args;
            args["[POINT]"] = name;
            name = mView.text("hud_point", args);
        }
        return mPointNames.emplace(point.getName(), name).first->second;
    }

    const std::string& AttachmentsModel::nameText(const Props& props) const
    {
        switch (props.state)
        {
        case Props::State::Received: return props.name.empty() ? mTextNoName : props.name;
        case Props::State::Failed:   return mTextUnavailable;
        default:                     return mTextLoading;
        }
    }

    void AttachmentsModel::setStatus(const std::string& text)
    {
        if (text != mStatusShown)
        {
            mStatusShown = text;
            mView.setStatus(text);
        }
    }

    void AttachmentsModel::setEmpty(const std::string& text)
    {
        if (text != mCommentShown)
        {
            mCommentShown = text;
            mView.setEmptyText(text);
        }
    }

    void AttachmentsModel::updateTitle()
    {
        const std::string name = shownName(mAvatarId);
        if (name == mTitleName)
        {
            return;
        }
        mTitleName = name;
        LLStringUtil::format_map_t args;
        args["[NAME]"] = name;
        mView.setTitle(mView.text("title_format", args));
    }

    void AttachmentsModel::refresh()
    {
        mDirty = false;
        mRefreshTimer.reset();
        if (mAvatarId.isNull())
        {
            return;
        }
        updateTitle();

        const LLStringUtil::format_map_t none;
        LLVOAvatar* avatar = findAvatar();
        if (!avatar)
        {
            // Gone, or out of the agent's view: the rows of what was seen go.
            if (!mGone)
            {
                mGone = true;
                mRows.clear();
                mView.clearRows();
            }
            const std::string gone = mView.text("gone", none);
            setEmpty(gone);
            setStatus(gone);
            return;
        }
        mGone = false;

        collect(*avatar);
        askNames();
        fillNames();
        show();

        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", static_cast<S32>(mRows.size()));
        setStatus(mView.text(avatar->isSelf() ? "count_self" : "count_other", args));
        setEmpty(mView.text("none", none));
    }

    // The attached objects (the roots) of every attachment point, into mScratch / mObjects.
    void AttachmentsModel::collect(LLVOAvatar& avatar)
    {
        mScratch.clear();
        mObjects.clear();
        const LLVector3d agent_pos = gAgent.getPositionGlobal();
        for (const auto& entry : avatar.mAttachmentPoints)
        {
            const LLViewerJointAttachment* point = entry.second;
            if (!point)
            {
                continue;
            }
            for (const LLPointer<LLViewerObject>& attached : point->mAttachedObjects)
            {
                LLViewerObject* object = attached.get();
                if (!object || object->isDead())
                {
                    continue;
                }
                Row& row = mScratch.emplace_back();
                row.id = object->getID();
                row.point = pointName(*point);
                row.prims = 1 + object->numChildren();
                row.distance = object->isHUDAttachment()
                    ? -1 : ll_round(static_cast<F32>(dist_vec(agent_pos, object->getPositionGlobal())));
                mObjects.push_back(object);
            }
        }
    }

    // The objects asked about and not answered in ANSWER_SECONDS are deselected and asked
    // again later (RETRY_SECONDS times the tries), MAX_TRIES at most, then "not available".
    // Over all of mProps, not only the rows: one no longer worn must not keep its place
    // among the MAX_IN_FLIGHT for ever (the rows after it would stay "Loading...").
    void AttachmentsModel::expireAnswers(F64 now)
    {
        for (auto& entry : mProps)
        {
            Props& props = entry.second;
            if (props.state != Props::State::Sent || now - props.sentAt <= ANSWER_SECONDS)
            {
                continue;
            }
            mSelection->release(entry.first);
            mInFlight = llmax(0, mInFlight - 1);
            if (props.tries >= MAX_TRIES)
            {
                props.state = Props::State::Failed;
            }
            else
            {
                props.state = Props::State::Need;
                props.notBefore = now + RETRY_SECONDS * props.tries;
            }
        }
    }

    // One ObjectSelect a row, a few at a time. The agent's own attachments are named from
    // the inventory (nothing asked).
    void AttachmentsModel::askNames()
    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        expireAnswers(now);
        for (size_t i = 0; i < mScratch.size(); ++i)
        {
            auto [found, inserted] = mProps.try_emplace(mScratch[i].id);
            Props& props = found->second;
            if (inserted)
            {
                props.since = now;
            }
            if (props.state != Props::State::Need || now < props.notBefore)
            {
                continue;
            }
            LLViewerObject* object = mObjects[i];
            if (object->permYouOwner())
            {
                if (const LLViewerInventoryItem* item = gInventory.getItem(object->getAttachmentItemID()))
                {
                    props.state     = Props::State::Received;
                    props.name      = item->getName();
                    props.creatorId = item->getCreatorUUID();
                    continue;
                }
            }
            if (mInFlight >= MAX_IN_FLIGHT)
            {
                continue;
            }
            if (mSelection->select(*object))
            {
                props.state  = Props::State::Sent;
                props.sentAt = now;
                ++props.tries;
                ++mInFlight;
            }
            else if (now - props.since > NO_REGION_SECONDS)
            {
                props.state = Props::State::Failed;     // its region never came
            }
        }
    }

    // The creator's name once the simulator has told it; "Loading..." until then, nothing
    // when it could not be had.
    void AttachmentsModel::fillNames()
    {
        for (Row& row : mScratch)
        {
            const Props& props = mProps[row.id];
            row.name = nameText(props);
            if (props.state == Props::State::Received)
            {
                row.creator = props.creatorId.notNull() ? shownName(props.creatorId) : std::string();
            }
            else if (props.state == Props::State::Failed)
            {
                row.creator.clear();
            }
            else
            {
                row.creator = mTextLoading;
            }
        }
    }

    std::string AttachmentsModel::distanceText(const Row& row) const
    {
        return row.distance < 0 ? mTextHud : std::to_string(row.distance);
    }

    // mScratch to the window: nothing when nothing changed; the view is told whether the same objects are there
    // (the changed cells are enough) or not (the whole list; the old objects' answers go).
    void AttachmentsModel::show()
    {
        if (mScratch == mRows)
        {
            return;
        }
        bool same_objects = mScratch.size() == mRows.size();
        for (size_t i = 0; same_objects && i < mScratch.size(); ++i)
        {
            same_objects = mScratch[i].id == mRows[i].id;
        }
        mView.showRows(mScratch, mRows, same_objects);
        if (!same_objects)
        {
            pruneProps();
        }
        mRows.swap(mScratch);
    }

    // What was told about objects no longer worn goes (not the ones still asked about).
    void AttachmentsModel::pruneProps()
    {
        mIndexById.clear();
        for (size_t i = 0; i < mScratch.size(); ++i)
        {
            mIndexById[mScratch[i].id] = i;
        }
        for (auto it = mProps.begin(); it != mProps.end();)
        {
            if (it->second.state != Props::State::Sent && mIndexById.find(it->first) == mIndexById.end())
            {
                it = mProps.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    const Row* AttachmentsModel::rowOf(const LLUUID& id) const
    {
        for (const Row& row : mRows)
        {
            if (row.id == id)
            {
                return &row;
            }
        }
        return nullptr;
    }

    AttachmentsModel::Buttons AttachmentsModel::buttons(const LLUUID& selected) const
    {
        const Row* row = rowOf(selected);
        const auto props = row ? mProps.find(row->id) : mProps.end();
        Buttons buttons;
        buttons.copy_name = props != mProps.end() && props->second.state == Props::State::Received
                            && !props->second.name.empty();
        buttons.copy_id = row != nullptr;
        buttons.inspect = row != nullptr;
        return buttons;
    }

    void AttachmentsModel::copyName(const LLUUID& selected) const
    {
        const Row* row = rowOf(selected);
        const auto props = row ? mProps.find(row->id) : mProps.end();
        if (props != mProps.end() && props->second.state == Props::State::Received)
        {
            copyText(props->second.name);
        }
    }

    void AttachmentsModel::copyId(const LLUUID& selected) const
    {
        if (const Row* row = rowOf(selected))
        {
            copyText(row->id.asString());
        }
    }

    // As "Inspect" on the attachment's own menu: selected, and the object's properties shown
    // (only when the selection takes it).
    void AttachmentsModel::inspect(const LLUUID& selected) const
    {
        const Row* row = rowOf(selected);
        LLViewerObject* object = row ? gObjectList.findObject(row->id) : nullptr;
        if (!object || object->isDead() || LLApp::isExiting())
        {
            return;
        }
        LLSelectMgr* select = LLSelectMgr::getInstance();
        select->deselectAll();
        select->selectObjectAndFamily(object);
        if (!select->getSelection()->isEmpty())
        {
            LLFloaterReg::showInstance("task_properties");
        }
    }

    // The "Refresh" button: the list at once, and the names not sent are asked for again
    // (MAX_TRIES more at most).
    void AttachmentsModel::askAgain()
    {
        const F64 now = LLFrameTimer::getElapsedSeconds();
        for (auto& entry : mProps)
        {
            Props& props = entry.second;
            if (props.state == Props::State::Failed)
            {
                props.state = Props::State::Need;
                props.tries = 0;
                props.since = now;
                props.notBefore = 0.0;
            }
        }
        mDirty = true;
    }
}
