/**
 * @file llpreviewanim.cpp
 * @brief LLPreviewAnim class implementation
 *
 * $LicenseInfo:firstyear=2004&license=viewerlgpl$
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

#include "llpreviewanim.h"
#include "llbutton.h"
#include "llresmgr.h"
#include "llinventory.h"
#include "llvoavatarself.h"
#include "llagent.h"          // gAgent
#include "llkeyframemotion.h"
#include "llfilepicker.h"
#include "lllineeditor.h"
#include "lltrans.h"
#include "lluictrlfactory.h"
#include "lluictrlfactory.h"
#include "llmultifloater.h" // <plus> anim-preview-fix: growToFit for the tabbed host </plus>
#include "lldatapacker.h"
#include "plus/animquick/animquick.h" // <plus> animquick (research/332) </plus>

extern LLAgent gAgent;
const S32 ADVANCED_VPAD = 3;

LLPreviewAnim::LLPreviewAnim(const LLSD& key)
    : LLPreview( key )
{
    mCommitCallbackRegistrar.add("PreviewAnim.Play", boost::bind(&LLPreviewAnim::play, this, _2));
}

// virtual
bool LLPreviewAnim::postBuild()
{
    childSetCommitCallback("desc", LLPreview::onText, this);
    getChild<LLLineEditor>("desc")->setPrevalidate(&LLTextValidate::validateASCIIPrintableNoPipe);
    getChild<LLTextBox>("adv_trigger")->setClickedCallback(boost::bind(&LLPreviewAnim::showAdvanced, this));
    pAdvancedStatsTextBox = getChild<LLTextBox>("AdvancedStats");

    // Assume that advanced stats start visible (for XUI preview tool's purposes)
    pAdvancedStatsTextBox->setVisible(false);
    // <plus> anim-preview-fix: the collapsed height comes from the stats' position (fitAdvancedHeight), as everywhere else
    fitAdvancedHeight();
    // </plus>

    return LLPreview::postBuild();
}

// <plus> animquick (research/332)
// A window without the check box (a language's XUI that lacks it) counts as pinned: never reused.
bool LLPreviewAnim::isPinned()
{
    LLUICtrl* pin = findChild<LLUICtrl>("pin");
    return !pin || pin->getValue().asBoolean();
}

// A play that found its motion and has not ended: starting (not yet active) or active. mItemID is set when a play starts
// (even before the motion object exists) and cleared by cleanup() when the motion has stopped or the window closes;
// isPlaying() is false only when the motion object is gone (the asset failed to load and the motion was dropped);
// a motion that is still loading has its object already, so it counts as playing.
bool LLPreviewAnim::isPlaying()
{
    if (mItemID.isNull() || !isAgentAvatarValid())
    {
        return false;
    }
    if (!gAgentAvatarp->findMotion(mItemID))
    {
        return false; // the asset failed to load and the motion was dropped: draw() never clears mItemID then
    }
    return !mDidStart || gAgentAvatarp->isMotionActive(mItemID);
}

void LLPreviewAnim::setPinned(bool pinned)
{
    if (LLUICtrl* pin = findChild<LLUICtrl>("pin"))
    {
        pin->setValue(pinned);
    }
}
// </plus>

// llinventorybridge also calls into here
void LLPreviewAnim::play(const LLSD& param)
{
    const LLInventoryItem *item = getItem();

    if(item)
    {
        LLUUID itemID=item->getAssetUUID();

        std::string btn_name = param.asString();
        LLButton* btn_inuse;
        LLButton* btn_other;

        if ("Inworld" == btn_name)
        {
            btn_inuse = getChild<LLButton>("Inworld");
            btn_other = getChild<LLButton>("Locally");
        }
        else if ("Locally" == btn_name)
        {
            btn_inuse = getChild<LLButton>("Locally");
            btn_other = getChild<LLButton>("Inworld");
        }
        else
        {
            return;
        }

        if (btn_inuse)
        {
            btn_inuse->toggleState();
        }

        if (btn_other)
        {
            btn_other->setEnabled(false);
        }

        if (getChild<LLUICtrl>(btn_name)->getValue().asBoolean() )
        {
            plus::animquick::rememberMode("Inworld" == btn_name); // <plus> animquick: the next quick open plays the same way </plus>
            if("Inworld" == btn_name)
            {
                gAgent.sendAnimationRequest(itemID, ANIM_REQUEST_START);
            }
            else
            {
                gAgentAvatarp->startMotion(item->getAssetUUID());
                plus::animquick::noteLocalPlay(item->getAssetUUID()); // <plus> anim-preview-fix: for Stop Animating </plus>
            }
            // <plus> anim-preview-fix: remembered even when no motion object exists yet, so closing stops it. The old code
            // set these only when findMotion() found a motion right after the start (a block removed here).
            mItemID = itemID;
            mDidStart = false;
            mPlayedInworld = ("Inworld" == btn_name);
            // </plus>
        }
        else
        {
            gAgentAvatarp->stopMotion(itemID);
            gAgent.sendAnimationRequest(itemID, ANIM_REQUEST_STOP);
            plus::animquick::forgetLocalPlay(itemID); // <plus> anim-preview-fix </plus>

            if (btn_other)
            {
                btn_other->setEnabled(true);
            }
        }
    }
}

// virtual
void LLPreviewAnim::draw()
{
    LLPreview::draw();
    // <plus> anim-preview-fix: a window that left a tabbed host comes back at the size the host remembered; fit it
    // again, once (not every frame: a minimized window is only shrunk to its header and must not be reshaped back)
    if (getHost())
    {
        mWasHosted = true;
    }
    else if (mWasHosted && !isMinimized())
    {
        mWasHosted = false;
        fitAdvancedHeight();
    }
    // </plus>
    if (!this->mItemID.isNull() && isAgentAvatarValid()) // <plus> anim-preview-fix: avatar guard </plus>
    {
        LLMotion* motion = gAgentAvatarp->findMotion(this->mItemID);
        if (motion)
        {
            if (motion->isStopped() && this->mDidStart)
            {
                cleanup();
            }
            if(gAgentAvatarp->isMotionActive(this->mItemID) && !this->mDidStart)
            {
                const LLInventoryItem *item = getItem();
                LLMotion* motion = gAgentAvatarp->findMotion(this->mItemID);
                if (item && motion)
                {
                    motion->setName(item->getName());
                }
                this->mDidStart = true;
            }
        }
    }
}

// virtual
void LLPreviewAnim::refreshFromItem()
{
    const LLInventoryItem* item = getItem();
    if (!item)
    {
        return;
    }

    // Preload motion
    gAgentAvatarp->createMotion(item->getAssetUUID());

    LLPreview::refreshFromItem();
}

void LLPreviewAnim::cleanup()
{
    plus::animquick::forgetLocalPlay(mItemID); // <plus> anim-preview-fix: the play ended; do not keep the id forever </plus>
    this->mItemID = LLUUID::null;
    this->mDidStart = false;
    getChild<LLUICtrl>("Inworld")->setValue(false);
    getChild<LLUICtrl>("Locally")->setValue(false);
    getChild<LLUICtrl>("Inworld")->setEnabled(true);
    getChild<LLUICtrl>("Locally")->setEnabled(true);
}

// virtual
void LLPreviewAnim::onClose(bool app_quitting)
{
    // <plus> anim-preview-fix: was: stop the item's asset, whether or not this window played it
    stopOwnPlay(!app_quitting);
    // </plus>
}

// <plus> anim-preview-fix
// Stop the play this window started (mItemID), and only that: an asset the item shares with an AO or a script that is
// playing it for another reason is left alone. Stopped at once, not faded, so windows closed one after another leave no
// tail. 'send' tells the server, and only for a play started Inworld (a Locally play is not known to the server, and a
// STOP for the asset would stop an AO's or a script's play of the same asset).
void LLPreviewAnim::stopOwnPlay(bool send)
{
    if (mItemID.isNull())
    {
        return;
    }
    if (isAgentAvatarValid())
    {
        gAgentAvatarp->stopMotion(mItemID, true);
    }
    if (send && mPlayedInworld)
    {
        gAgent.sendAnimationRequest(mItemID, ANIM_REQUEST_STOP);
    }
    plus::animquick::forgetLocalPlay(mItemID);
    mItemID = LLUUID::null;
    mDidStart = false;
}

// A tabbed window (LLMultiPreview) is created without postBuild, so the host's close does not reach onClose and the
// children are destroyed instead: the same stop runs here. Not at exit: the connection is going away.
LLPreviewAnim::~LLPreviewAnim()
{
    stopOwnPlay(!LLApp::isExiting());
}

// The height of a window that is not in a host follows the stats' position. The stats are anchored to the top, so
// (window height - stats bottom edge) is the same whatever the window's height is: the expanded height is that plus
// ADVANCED_VPAD, the collapsed height drops the stats and the gap. Any way the window came back from a host (which
// restores the height it had when it joined) ends at the right height.
void LLPreviewAnim::fitAdvancedHeight()
{
    if (!pAdvancedStatsTextBox)
    {
        return;
    }
    const LLRect rect = getRect();
    const LLRect stats = pAdvancedStatsTextBox->getRect();
    S32 target = rect.getHeight() - stats.mBottom;
    if (pAdvancedStatsTextBox->getVisible())
    {
        target += ADVANCED_VPAD;
    }
    else
    {
        target -= stats.getHeight();
    }
    if (target != rect.getHeight())
    {
        reshape(rect.getWidth(), target, false);
    }
}
// </plus>

void LLPreviewAnim::showAdvanced()
{
    bool was_visible =  pAdvancedStatsTextBox->getVisible();

    // <plus> anim-preview-fix: a window tabbed into a multi-preview is sized by its host, not by itself. Reshaping it
    // made it taller than its tab panel, which pushed the contents up and squashed the Play buttons and tabs. Here the
    // host grows to fit the stats (idempotent: nothing to grow when they already fit), and a collapse leaves the size.
    // A window of its own takes its height from fitAdvancedHeight().
    LLMultiFloater* host = getHost();
    if (was_visible)
    {
        pAdvancedStatsTextBox->setVisible(false);
        if (!host)
        {
            fitAdvancedHeight();
        }
    }
    else
    {
        pAdvancedStatsTextBox->setVisible(true);
        if (host)
        {
            // the stats are anchored to the top, so the height this window needs is down to the stats' bottom edge
            const S32 needed = getRect().getHeight() - pAdvancedStatsTextBox->getRect().mBottom + ADVANCED_VPAD;
            if (needed > getRect().getHeight())
            {
                host->growToFit(getRect().getWidth(), needed);
                // growToFit sizes by the header and tab strip as the floater's own estimate; the real tab area can be a
                // few pixels smaller. Grow the host by the shortfall, keeping its top left corner.
                const S32 shortfall = needed - getRect().getHeight();
                if (shortfall > 0)
                {
                    host->reshape(host->getRect().getWidth(), host->getRect().getHeight() + shortfall);
                    host->translate(0, -shortfall);
                }
            }
        }
        else
        {
            fitAdvancedHeight();
        }
    // </plus>

        LLMotion *motion = NULL;
        const LLInventoryItem* item = getItem();
        if (item)
        {
            // if motion exists, will return existing one.
            // Needed because viewer can purge motions
            motion = gAgentAvatarp->createMotion(item->getAssetUUID());
        }

        // set text
        if (motion)
        {
            pAdvancedStatsTextBox->setTextArg("[PRIORITY]", llformat("%d", motion->getPriority()));
            pAdvancedStatsTextBox->setTextArg("[DURATION]", llformat("%.2f", motion->getDuration()));
            pAdvancedStatsTextBox->setTextArg("[EASE_IN]", llformat("%.2f", motion->getEaseInDuration()));
            pAdvancedStatsTextBox->setTextArg("[EASE_OUT]", llformat("%.2f", motion->getEaseOutDuration()));
            pAdvancedStatsTextBox->setTextArg("[IS_LOOP]", (motion->getLoop() ? LLTrans::getString("PermYes") : LLTrans::getString("PermNo")));
            pAdvancedStatsTextBox->setTextArg("[NUM_JOINTS]", llformat("%d", motion->getNumJointMotions()));
        }
    }
}
