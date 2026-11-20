/**
 * @file poseshare.cpp
 * @brief The settings, the clock and the session hooks of the pose sharing (research/374). The sending side is
 *        poseshare_send.cpp, the receiving side poseshare_receive.cpp, the packing poseshareproto.cpp.
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

#include "poseshare.h"
#include "poseshareinternal.h"

#include "plus.h"
#include "plus/poser/fsjointpose.h"
#include "plus/poser/fsposeranimator.h"
#include "plus/poser/fsposingmotion.h"

#include "llagent.h"
#include "llapp.h"
#include "llcallbacklist.h"
#include "llstartup.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llviewermessage.h"

#include <set>

namespace plus::poseshare
{
    namespace
    {
        constexpr char SETTING_SEND[]    = "PlusPoseShareMode";
        constexpr char SETTING_RECEIVE[] = "PlusPoseShareReceive";
    }

    SendMode sendMode()
    {
        // Per account (the choice to show your pose is each account's): before a login it holds the declared default
        static LLCachedControl<S32> setting(gSavedPerAccountSettings, SETTING_SEND, static_cast<S32>(SendMode::Off));
        const S32 value = setting;
        if (value < static_cast<S32>(SendMode::Off) || value > static_cast<S32>(SendMode::Everyone))
            return SendMode::Off; // a value no version wrote: the default
        return static_cast<SendMode>(value);
    }

    ReceiveMode receiveMode()
    {
        static LLCachedControl<S32> setting(gSavedPerAccountSettings, SETTING_RECEIVE, static_cast<S32>(ReceiveMode::Friends));
        const S32 value = setting;
        if (value < static_cast<S32>(ReceiveMode::Nobody) || value > static_cast<S32>(ReceiveMode::Everyone))
            return ReceiveMode::Friends;
        return static_cast<ReceiveMode>(value);
    }

    void setSendMode(SendMode mode)
    {
        const S32 value = static_cast<S32>(mode);
        if (value < static_cast<S32>(SendMode::Off) || value > static_cast<S32>(SendMode::Everyone))
            return;
        gSavedPerAccountSettings.setS32(SETTING_SEND, value);
    }

    void setReceiveMode(ReceiveMode mode)
    {
        const S32 value = static_cast<S32>(mode);
        if (value < static_cast<S32>(ReceiveMode::Nobody) || value > static_cast<S32>(ReceiveMode::Everyone))
            return;
        gSavedPerAccountSettings.setS32(SETTING_RECEIVE, value);
    }

    namespace detail
    {
        bool JointCache::matches(const FSPosingMotion* motion) const
        {
            return motion && serial != 0 && motion->instanceSerial() == serial;
        }

        void JointCache::clear()
        {
            serial = 0;
            table.fill(nullptr);
            others.clear();
        }

        FSPoserAnimator& shareAnimator()
        {
            static FSPoserAnimator animator(PoseOwner::Share);
            return animator;
        }

        void buildCache(FSPosingMotion* motion, JointCache& cache)
        {
            cache.clear();
            if (!motion)
                return;
            cache.serial = motion->instanceSerial();
            std::set<const FSJointPose*> inTable;
            for (std::size_t i = 0; i < proto::JOINT_COUNT; ++i)
            {
                cache.table[i] = motion->getJointPoseByJointName(proto::JOINT_NAMES[i]);
                if (cache.table[i])
                    inTable.insert(cache.table[i]);
            }
            // Every other joint pose of the motion, not a list of names: onInitialize() poses all the bones and all the
            // collision volumes of the skeleton, also those the poser's list lacks (mFaceEyeAltLeft, LOWER_BACK, ...)
            for (FSJointPose& pose : motion->jointPoses())
            {
                if (!inTable.contains(&pose))
                    cache.others.push_back(&pose);
            }
        }

        // In our region and within limitMeters of us.
        bool nearEnough(LLVOAvatar* avatar, F64 limitMeters)
        {
            LLViewerRegion* region = gAgent.getRegion();
            if (!avatar || !region || avatar->getRegion() != region)
                return false;
            return dist_vec(avatar->getPositionGlobal(), gAgent.getPositionGlobal()) <= limitMeters;
        }

        // Sets the joints of `avatar` to `pose`: the joints in it stand posed at its values, the others are given back to
        // the avatar's own animations. A new motion has every joint posed (as the poser starts), so the first message
        // (the cache is empty or of another motion) turns the others off in the same call and nothing stands frozen for a
        // frame. The motion is the one this instance holds (a share, or a push onto the own avatar); false when it is not.
        bool applyPoseTo(LLVOAvatar* avatar, JointCache& cache, const proto::Pose& pose)
        {
            FSPosingMotion* motion = avatar ? shareAnimator().sharedMotion(avatar) : nullptr;
            if (!motion)
                return false;

            bool fresh = false;
            if (!cache.matches(motion))
            {
                buildCache(motion, cache);
                fresh = true;
            }

            for (std::size_t i = 0; i < proto::JOINT_COUNT; ++i)
            {
                FSJointPose* joint = cache.table[i];
                if (!joint)
                    continue;
                const proto::JointValue& value  = pose.joints[i];
                const bool               posing = motion->currentlyPosingJoint(joint);
                if (!value.present)
                {
                    if (posing)
                        motion->removeJointFromState(joint);
                    continue;
                }
                if (!posing)
                    motion->addJointToState(joint);
                joint->setSharedTarget(LLQuaternion(value.rotation[0], value.rotation[1], value.rotation[2], value.rotation[3]),
                                       LLVector3(value.position[0], value.position[1], value.position[2]),
                                       LLVector3(value.scale[0], value.scale[1], value.scale[2]));
            }
            if (fresh)
            {
                for (FSJointPose* joint : cache.others)
                {
                    if (motion->currentlyPosingJoint(joint))
                        motion->removeJointFromState(joint);
                }
            }
            return true;
        }
    }

    namespace
    {
        // A region change or a teleport: what was shown is gone from the screen with the avatars, and what we show of
        // ours ends (the end is sent where the bridge can still say it).
        void onSessionBreak()
        {
            detail::receiveClearAll();
            detail::pushedClearAll();
            detail::sendBeginEnding(detail::nowSeconds());
        }

        // Once every TICK_SECONDS: the ledger of the people shown, then our own share. Before that it is one comparison
        // of two numbers; with the sharing off and nobody shown, each tick is two cached settings and an empty map.
        void idle(void*)
        {
            static F64 next_s = 0.0;
            const F64  now_s  = detail::nowSeconds();
            if (now_s < next_s)
                return;
            next_s = now_s + detail::TICK_SECONDS;
            if (!LLApp::isRunning() || LLStartUp::getStartupState() < STATE_STARTED)
                return;

            detail::receiveTick(now_s);
            detail::pushedTick(now_s);
            detail::sendTick(now_s);
        }

        // The menu of the poser's Share button (menu_fs_poser_share.xml): the mode to set, and which one is on
        void registerMenuCallbacks()
        {
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.PoseShare.SetSend", [](LLUICtrl*, const LLSD& param)
                                                                      { setSendMode(static_cast<SendMode>(param.asInteger())); });
            LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Plus.PoseShare.SetReceive", [](LLUICtrl*, const LLSD& param)
                                                                      { setReceiveMode(static_cast<ReceiveMode>(param.asInteger())); });
            LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.PoseShare.IsSend", [](LLUICtrl*, const LLSD& param)
                                                                      { return static_cast<S32>(sendMode()) == param.asInteger(); });
            LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Plus.PoseShare.IsReceive", [](LLUICtrl*, const LLSD& param)
                                                                      { return static_cast<S32>(receiveMode()) == param.asInteger(); });
        }

        void init()
        {
            detail::receiveInit();
            detail::pushedInit();
            registerMenuCallbacks();
            gIdleCallbacks.addFunction(&idle, nullptr);
        }

        void onUIReady()
        {
            gAgent.addRegionChangedCallback([]() { onSessionBreak(); });
            LLViewerMessage::getInstance()->setTeleportStartedCallback([]() { onSessionBreak(); });
        }

        plus::FeatureRegistration sRegistration({ "poseshare", &init, &onUIReady });
    }
}
