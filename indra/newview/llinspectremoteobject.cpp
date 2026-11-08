/**
 * @file llinspectremoteobject.cpp
 *
 * $LicenseInfo:firstyear=2009&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llfloaterreg.h"
#include "llinspectremoteobject.h"
#include "llinspect.h"
#include "llmutelist.h"
#include "llpanelblockedlist.h"
#include "llslurl.h"
#include "lltrans.h"
#include "llui.h"
#include "lluictrl.h"
#include "llurlaction.h"
// <plus> rlv: RLVa (plus/rlv) (research/248)
#include "plus/rlv/rlvhooks.h"
#include "llweb.h" // LLWeb::escapeURL() (the hidden region of an escaped slurl)
// </plus>

//////////////////////////////////////////////////////////////////////////////
// LLInspectRemoteObject
//////////////////////////////////////////////////////////////////////////////

// Remote Object Inspector, a small information window used to
// display information about potentially-remote objects. Used
// to display details about objects sending messages to the user.
class LLInspectRemoteObject : public LLInspect
{
    friend class LLFloaterReg;

public:
    LLInspectRemoteObject(const LLSD& object_id);
    virtual ~LLInspectRemoteObject() {};

    /*virtual*/ bool postBuild(void);
    /*virtual*/ void onOpen(const LLSD& avatar_id);

    void onClickMap();
    void onClickBlock();
    void onClickClose();

private:
    void update();

private:
    LLUUID       mObjectID;
    LLUUID       mOwnerID;
    std::string  mSLurl;
    std::string  mName;
    bool         mGroupOwned;
    // <plus> rlv: @shownames - the link that opened the inspector said the owner is hidden (research/248 E22)
    bool         mRlvHideNames = false;
    // </plus>
};

LLInspectRemoteObject::LLInspectRemoteObject(const LLSD& sd) :
    LLInspect(LLSD()),
    mObjectID(NULL),
    mOwnerID(NULL),
    mSLurl(""),
    mName(""),
    mGroupOwned(false)
{
}

/*virtual*/
bool LLInspectRemoteObject::postBuild(void)
{
    // hook up the inspector's buttons
    getChild<LLUICtrl>("map_btn")->setCommitCallback(
        boost::bind(&LLInspectRemoteObject::onClickMap, this));
    getChild<LLUICtrl>("block_btn")->setCommitCallback(
        boost::bind(&LLInspectRemoteObject::onClickBlock, this));
    getChild<LLUICtrl>("close_btn")->setCommitCallback(
        boost::bind(&LLInspectRemoteObject::onClickClose, this));

    return true;
}

/*virtual*/
void LLInspectRemoteObject::onOpen(const LLSD& data)
{
    // Start animation
    LLInspect::onOpen(data);

    // Extract appropriate object information from input LLSD
    // (Eventually, it might be nice to query server for details
    // rather than require caller to pass in the information.)
    mObjectID   = data["object_id"].asUUID();
    mName       = data["name"].asString();
    mOwnerID    = data["owner_id"].asUUID();
    mGroupOwned = data["group_owned"].asBoolean();
    mSLurl      = data["slurl"].asString();
    // <plus> rlv: @shownames - "rlv_shownames" of the objectim link (set by plus/rlv) (research/248 E22)
    // (FS keeps the previous value when the key is missing; here every opening starts from "not hidden")
    mRlvHideNames = plus::rlv::enabled() && data.has("rlv_shownames") && data["rlv_shownames"].asBoolean();
    // </plus>

    // update the inspector with the current object state
    update();

    LLInspect::repositionInspector(data);
}

void LLInspectRemoteObject::onClickMap()
{
    std::string url = "secondlife://" + mSLurl;
    LLUrlAction::showLocationOnMap(url);
    closeFloater();
}

void LLInspectRemoteObject::onClickBlock()
{
    LLMute mute(mObjectID, mName, LLMute::OBJECT);
    LLMuteList::getInstance()->add(mute);
    LLPanelBlockedList::showPanelAndSelect(mute.mID);
    closeFloater();
}

void LLInspectRemoteObject::onClickClose()
{
    closeFloater();
}

void LLInspectRemoteObject::update()
{
    // show the object name as the inspector's title
    // (don't hyperlink URLs in object names)
    getChild<LLUICtrl>("object_name")->setValue("<nolink>" + mName + "</nolink>");

    // show the object's owner - click it to show profile
    std::string owner;
    if (! mOwnerID.isNull())
    {
        if (mGroupOwned)
        {
            owner = LLSLURL("group", mOwnerID, "about").getSLURLString();
        }
        else
        {
            owner = LLSLURL("agent", mOwnerID, "about").getSLURLString();
            // <plus> rlv: @shownames - a hidden owner is shown by the anonym (plus/rlv) (research/248 E22)
            if (mRlvHideNames)
            {
                owner = LLSLURL("agent", mOwnerID, "rlvanonym").getSLURLString();
            }
            // </plus>
        }
    }
    else
    {
        owner = LLTrans::getString("Unknown");
    }
    getChild<LLUICtrl>("object_owner")->setValue(owner);

    // display the object's SLurl - click it to teleport
    std::string url;
    if (! mSLurl.empty())
    {
        url = "secondlife:///app/teleport/" + mSLurl;
    }
    getChild<LLUICtrl>("object_slurl")->setValue(url);

    // disable the Map button if we don't have a SLurl
    getChild<LLUICtrl>("map_btn")->setEnabled(! mSLurl.empty());

    // disable the Block button if we don't have the object ID (will this ever happen?)
    getChild<LLUICtrl>("block_btn")->setEnabled(!mObjectID.isNull() && !LLMuteList::getInstance()->isMuted(mObjectID));

    // <plus> rlv: @showloc - a hidden place is shown as text (no teleport link) and the Map button is off (plus/rlv) (research/248 E22)
    // (the objectim handler and the chat header pass the slurl through LLWeb::escapeURL(): compare escaped with escaped)
    if (plus::rlv::enabled() && ((plus::rlv::hiddenRegionText() == mSLurl) || (LLWeb::escapeURL(plus::rlv::hiddenRegionText()) == mSLurl)))
    {
        getChild<LLUICtrl>("object_slurl")->setValue(plus::rlv::hiddenRegionText());
        getChild<LLUICtrl>("map_btn")->setEnabled(false);
    }
    // </plus>
}

//////////////////////////////////////////////////////////////////////////////
// LLInspectRemoteObjectUtil
//////////////////////////////////////////////////////////////////////////////
void LLInspectRemoteObjectUtil::registerFloater()
{
    LLFloaterReg::add("inspect_remote_object", "inspect_remote_object.xml",
                      &LLFloaterReg::build<LLInspectRemoteObject>);
}
