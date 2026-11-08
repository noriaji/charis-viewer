/**
 * @file avatarattach.cpp
 * @brief The attachments of an avatar, listed in a window (the avatar's context menu).
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

// See avatarattach.h for what the window does and where its names come from.

#include "llviewerprecompiledheaders.h"

#include "avatarattach.h"
#include "plus.h"
#include "plus/rlv/rlvhooks.h"

#include <map>
#include <unordered_map>
#include <vector>

#include "llagent.h"
#include "llapp.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llbutton.h"
#include "llclipboard.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llhost.h"
#include "llinventorymodel.h"
#include "message.h"
#include "llscrolllistcell.h"
#include "llscrolllistcolumn.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llselectmgr.h"
#include "lltextbox.h"
#include "lltrans.h"
#include "lluictrlfactory.h"
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
        constexpr char FLOATER_NAME[] = "plus_avatar_attachments";
        constexpr F32 REFRESH_SECONDS = 1.f;    // the list is made again this often while the window is open
        constexpr F64 ANSWER_SECONDS  = 6.0;    // no answer in this time: asked again (or given up)
        constexpr F64 RETRY_SECONDS   = 3.0;    // the wait before asking again, times the tries made
        constexpr S32 MAX_TRIES       = 3;      // then "not available" (until "Refresh")
        constexpr F64 NO_REGION_SECONDS = 20.0; // an object with no region this long: "not available"
        constexpr S32 MAX_IN_FLIGHT   = 8;      // objects asked about and not answered yet, at most

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

        // The avatar of the context menu: the selection's primary object is the avatar (or, on the
        // menu of an attachment, the attachment). As the read-aloud voice of the same menu does.
        LLUUID selectedAvatarId()
        {
            LLViewerObject* object = LLSelectMgr::getInstance()->getSelection()->getPrimaryObject();
            if (!object)
            {
                return LLUUID::null;
            }
            if (object->isAvatar())
            {
                return object->getID();
            }
            if (LLVOAvatar* avatar = object->getAvatar())
            {
                return avatar->getID();
            }
            return LLUUID::null;
        }

        void copyText(const std::string& text)
        {
            if (text.empty())
            {
                return;
            }
            const LLWString wide = utf8str_to_wstring(text);
            LLClipboard::instance().copyToClipboard(wide, 0, static_cast<S32>(wide.length()));
        }

        class AttachmentsFloater;
        AttachmentsFloater* sOpen = nullptr;    // the window while it is open (single instance)

        class AttachmentsFloater : public LLFloater
        {
            friend class ::LLFloaterReg;

        public:
            explicit AttachmentsFloater(const LLSD& key) : LLFloater(key) {}

            ~AttachmentsFloater() override
            {
                stopAsking();
            }

            bool postBuild() override
            {
                mList   = getChild<LLScrollListCtrl>("attachments_list");
                mStatus = getChild<LLTextBox>("status");
                mCopyNameBtn = getChild<LLButton>("copy_name_btn");
                mCopyIdBtn   = getChild<LLButton>("copy_id_btn");
                mInspectBtn  = getChild<LLButton>("inspect_btn");

                mColPoint    = columnIndex("point");
                mColName     = columnIndex("name");
                mColCreator  = columnIndex("creator");
                mColPrims    = columnIndex("prims");
                mColDistance = columnIndex("distance");

                mTextLoading     = getString("loading");
                mTextUnavailable = getString("name_unavailable");
                mTextNoName      = getString("no_name");
                mTextHud         = getString("hud_distance");

                mList->setCommitCallback([this](LLUICtrl*, const LLSD&) { updateButtons(); });
                mList->setDoubleClickCallback([this]() { inspectSelected(); });
                mCopyNameBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { copySelectedName(); });
                mCopyIdBtn->setCommitCallback([this](LLUICtrl*, const LLSD&)
                {
                    if (const Row* row = selectedRow())
                    {
                        copyText(row->id.asString());
                    }
                });
                mInspectBtn->setCommitCallback([this](LLUICtrl*, const LLSD&) { inspectSelected(); });
                getChild<LLUICtrl>("refresh_btn")->setCommitCallback([this](LLUICtrl*, const LLSD&) { askAgain(); });
                updateButtons();
                return true;
            }

            void onOpen(const LLSD& key) override
            {
                const LLUUID id = key.asUUID();
                if (id != mAvatarId)
                {
                    // Another avatar: nothing of the last one stays.
                    stopAsking();
                    mAvatarId = id;
                    mProps.clear();
                    mRows.clear();
                    mList->deleteAllItems();
                    mGone = false;
                }
                sOpen = this;   // after stopAsking(), which lets go of it
                refresh();
            }

            void onClose(bool app_quitting) override
            {
                stopAsking();
                LLFloater::onClose(app_quitting);
            }

            void draw() override
            {
                if (mDirty || mRefreshTimer.getElapsedTimeF32() >= REFRESH_SECONDS)
                {
                    refresh();
                }
                LLFloater::draw();
            }

            // An ObjectProperties answer: only the ones this window asked about are taken (a late
            // answer after a timeout too). True when this window had the object selected.
            bool onObjectProperties(const LLUUID& object_id, const LLUUID& creator_id, const std::string& name)
            {
                const bool held = mSelection.release(object_id);
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

        private:
            S32 columnIndex(const std::string& name)
            {
                LLScrollListColumn* column = mList->getColumn(name);
                return column ? column->mIndex : -1;
            }

            // No answer is waited for any more (the window closes or shows another avatar).
            void stopAsking()
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
                mSelection.releaseAll();
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

            LLVOAvatar* findAvatar() const
            {
                LLViewerObject* object = mAvatarId.notNull() ? gObjectList.findObject(mAvatarId) : nullptr;
                return (object && object->isAvatar() && !object->isDead()) ? static_cast<LLVOAvatar*>(object) : nullptr;
            }

            // The display name of a resident from the name cache; asked for once when not there yet.
            std::string residentName(const LLUUID& id)
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
            std::string shownName(const LLUUID& id)
            {
                std::string name = residentName(id);
                if (plus::rlv::enabled() && id == mAvatarId && name != mTextLoading)
                {
                    name = plus::rlv::avatarNameForPlus(id, name);
                }
                return name;
            }

            const std::string& pointName(const LLViewerJointAttachment& point)
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
                    name = getString("hud_point", args);
                }
                return mPointNames.emplace(point.getName(), name).first->second;
            }

            const std::string& nameText(const Props& props) const
            {
                switch (props.state)
                {
                case Props::State::Received: return props.name.empty() ? mTextNoName : props.name;
                case Props::State::Failed:   return mTextUnavailable;
                default:                     return mTextLoading;
                }
            }

            void setStatus(const std::string& text)
            {
                if (text != mStatusShown)
                {
                    mStatusShown = text;
                    mStatus->setText(text);
                }
            }

            void setCommentIfEmpty(const std::string& text)
            {
                if (text != mCommentShown)
                {
                    mCommentShown = text;
                    mList->setCommentText(text);
                }
            }

            void updateTitle()
            {
                const std::string name = shownName(mAvatarId);
                if (name == mTitleName)
                {
                    return;
                }
                mTitleName = name;
                LLStringUtil::format_map_t args;
                args["[NAME]"] = name;
                setTitle(getString("title_format", args));
            }

            void refresh()
            {
                mDirty = false;
                mRefreshTimer.reset();
                if (mAvatarId.isNull())
                {
                    return;
                }
                updateTitle();

                LLVOAvatar* avatar = findAvatar();
                if (!avatar)
                {
                    // Gone, or out of the agent's view: the rows of what was seen go.
                    if (!mGone)
                    {
                        mGone = true;
                        mRows.clear();
                        mList->deleteAllItems();
                    }
                    const std::string& gone = getString("gone");
                    setCommentIfEmpty(gone);
                    setStatus(gone);
                    updateButtons();
                    return;
                }
                mGone = false;

                collect(*avatar);
                askNames();
                fillNames();
                show();

                LLStringUtil::format_map_t args;
                args["[COUNT]"] = llformat("%d", static_cast<S32>(mRows.size()));
                setStatus(getString(avatar->isSelf() ? "count_self" : "count_other", args));
                setCommentIfEmpty(getString("none"));
                updateButtons();
            }

            // The attached objects (the roots) of every attachment point, into mScratch / mObjects.
            void collect(LLVOAvatar& avatar)
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
            void expireAnswers(F64 now)
            {
                for (auto& entry : mProps)
                {
                    Props& props = entry.second;
                    if (props.state != Props::State::Sent || now - props.sentAt <= ANSWER_SECONDS)
                    {
                        continue;
                    }
                    mSelection.release(entry.first);
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
            void askNames()
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
                    if (mSelection.select(*object))
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
            void fillNames()
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

            LLSD cellValue(const Row& row, S32 column) const
            {
                if (column == mColPoint)    return LLSD(row.point);
                if (column == mColName)     return LLSD(row.name);
                if (column == mColCreator)  return LLSD(row.creator);
                if (column == mColPrims)    return LLSD(row.prims);
                if (row.distance < 0)       return LLSD(mTextHud);
                return LLSD(row.distance);
            }

            // mScratch onto the list: nothing when nothing changed, the changed cells when the same
            // objects are there, the whole list (selection and scroll kept) otherwise.
            void show()
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

                if (same_objects)
                {
                    mIndexById.clear();
                    for (size_t i = 0; i < mScratch.size(); ++i)
                    {
                        mIndexById[mScratch[i].id] = i;
                    }
                    const S32 columns[] = { mColPoint, mColName, mColCreator, mColPrims, mColDistance };
                    for (LLScrollListItem* item : mList->getAllData())
                    {
                        auto found = item ? mIndexById.find(item->getUUID()) : mIndexById.end();
                        if (found == mIndexById.end())
                        {
                            continue;
                        }
                        const Row& now = mScratch[found->second];
                        const Row& was = mRows[found->second];
                        for (S32 column : columns)
                        {
                            LLScrollListCell* cell = column >= 0 ? item->getColumn(column) : nullptr;
                            const LLSD value = cellValue(now, column);
                            if (cell && cellValue(was, column).asString() != value.asString())
                            {
                                cell->setValue(value);
                            }
                        }
                    }
                    mList->setNeedsSort();
                }
                else
                {
                    const LLUUID selected = mList->getSelectedValue().asUUID();
                    const S32 scroll = mList->getScrollPos();
                    mList->deleteAllItems();
                    for (const Row& row : mScratch)
                    {
                        LLSD element;
                        element["id"] = row.id;
                        LLSD& columns = element["columns"];
                        const std::pair<const char*, S32> names[] = {
                            { "point", mColPoint }, { "name", mColName }, { "creator", mColCreator },
                            { "prims", mColPrims }, { "distance", mColDistance } };
                        S32 n = 0;
                        for (const auto& name : names)
                        {
                            if (name.second < 0)
                            {
                                continue;
                            }
                            columns[n]["column"] = name.first;
                            columns[n]["value"]  = cellValue(row, name.second);
                            ++n;
                        }
                        mList->addElement(element);
                    }
                    if (selected.notNull())
                    {
                        mList->selectByID(selected);
                    }
                    mList->setScrollPos(scroll);
                    pruneProps();
                }
                mRows.swap(mScratch);
            }

            // What was told about objects no longer worn goes (not the ones still asked about).
            void pruneProps()
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

            const Row* selectedRow() const
            {
                const LLUUID id = mList->getSelectedValue().asUUID();
                for (const Row& row : mRows)
                {
                    if (row.id == id)
                    {
                        return &row;
                    }
                }
                return nullptr;
            }

            void updateButtons()
            {
                const Row* row = selectedRow();
                const auto props = row ? mProps.find(row->id) : mProps.end();
                const bool has_name = props != mProps.end() && props->second.state == Props::State::Received
                                      && !props->second.name.empty();
                mCopyNameBtn->setEnabled(has_name);
                mCopyIdBtn->setEnabled(row != nullptr);
                mInspectBtn->setEnabled(row != nullptr);
            }

            void copySelectedName()
            {
                const Row* row = selectedRow();
                const auto props = row ? mProps.find(row->id) : mProps.end();
                if (props != mProps.end() && props->second.state == Props::State::Received)
                {
                    copyText(props->second.name);
                }
            }

            // As "Inspect" on the attachment's own menu: selected, and the object's properties shown
            // (only when the selection takes it).
            void inspectSelected()
            {
                const Row* row = selectedRow();
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
            void askAgain()
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

            LLScrollListCtrl* mList   = nullptr;
            LLTextBox*        mStatus = nullptr;
            LLButton*         mCopyNameBtn = nullptr;
            LLButton*         mCopyIdBtn   = nullptr;
            LLButton*         mInspectBtn  = nullptr;
            S32 mColPoint = -1, mColName = -1, mColCreator = -1, mColPrims = -1, mColDistance = -1;
            std::string mTextLoading, mTextUnavailable, mTextNoName, mTextHud;

            LLUUID mAvatarId;               // never a pointer: the avatar is found again each refresh
            bool mGone = false;
            bool mDirty = false;
            LLFrameTimer mRefreshTimer;
            S32 mInFlight = 0;

            std::map<LLUUID, Props> mProps;
            SimSelection mSelection;                    // whatever it still holds is deselected when the window goes
            std::vector<Row> mRows;                     // what the list shows
            std::vector<Row> mScratch;                  // the next rows (kept to reuse its room)
            std::vector<LLViewerObject*> mObjects;      // mScratch's objects, during refresh() only
            std::unordered_map<LLUUID, size_t> mIndexById;
            std::map<std::string, std::string> mPointNames;    // the point's name as shown, by its name
            std::map<LLUUID, boost::signals2::connection> mNameRequests;

            std::string mStatusShown, mTitleName, mCommentShown;
        };

        void init()
        {
            LLFloaterReg::add(FLOATER_NAME, "floater_plus_avatar_attachments.xml",
                              (LLFloaterBuildFunc)&LLFloaterReg::build<AttachmentsFloater>);

            // "self": the agent's own menu (the agent is not the selection there).
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.AvatarAttachments.Show", [](LLUICtrl*, const LLSD& param)
            {
                if (LLApp::isExiting())
                {
                    return;
                }
                const LLUUID id = param.asString() == "self" ? gAgentID : selectedAvatarId();
                if (id.notNull())
                {
                    LLFloaterReg::showInstance(FLOATER_NAME, LLSD(id), true);
                }
            });
        }

        plus::FeatureRegistration sRegistration({ "avatarattach", &init });
    }

    bool onObjectProperties(const LLUUID& object_id, const LLUUID& creator_id, const std::string& name)
    {
        return sOpen && sOpen->onObjectProperties(object_id, creator_id, name);
    }
}
