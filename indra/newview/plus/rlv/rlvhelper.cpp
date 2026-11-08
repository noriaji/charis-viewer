/**
 *
 * Copyright (c) 2009-2020, Kitty Barnett
 *
 * The source code in this file is provided to you under the terms of the
 * GNU Lesser General Public License, version 2.1, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. Terms of the LGPL can be found in doc/LGPL-licence.txt
 * in this distribution, or online at http://www.gnu.org/licenses/lgpl-2.1.txt
 *
 * By copying, modifying or distributing this software, you acknowledge that
 * you have read and understood your obligations described above, and agree to
 * abide by those obligations.
 *
 */

#include "llviewerprecompiledheaders.h"
#include "llappearancemgr.h"
#include "llcallbacklist.h"
#include "llinventoryfunctions.h"
#include "llviewerjointattachment.h"
#include "llviewerobjectlist.h"

#include "rlvattachpt.h"
#include "rlvcommon.h"
#include "rlvhelper.h"
#include "rlvhandler.h"
#include "rlvinventory.h"
#include "rlvmodifiers.h"

#include <boost/algorithm/string.hpp>
#include <cmath>

// ============================================================================
// RlvBehaviourDictionary
//

/*
 * Processing of RLVa commands used to be a big switch/case loop with one function for each command type(addrem, reply
 * and force). This is slowly being replaced with templated command handling which might be more confusing intially
 * (also due to my poor naming schemes) but is actually far simpler and less error-prone than the old way.
 *
 * In the general case you just add a definition for the command below and then write the function body in rlvhandler.cpp
 * and you're done! Told you this was easy.
 *
 * Reply command
 * =============
 * Definition: RlvReplyProcessor<RLV_BHVR_COMMANDNAME>("commandname"[, <options>]));
 * Implement : ERlvCmdRet RlvReplyHandler<RLV_BHVR_COMMANDNAME>::onCommand(const RlvCommand& rlvCmd, std::string& strReply)
 *
 * Force command
 * =============
 * Definition: new RlvForceProcessor<RLV_BHVR_COMMANDNAME>("commandname"[, <options>]));
 * Implement : ERlvCmdRet RlvForceProcessor<RLV_BHVR_COMMANDNAME>::onCommand(const RlvCommand& rlvCmd)
 *
 * Behaviours
 * ==========
 * Behaviours come in many forms but the added complexity is only in the variety of choices. The implementation is as
 * easy as reply or force commands.
 *
 * For simple behaviours that only require recordkeeping and don't run code when set/unset (see ERlvBehaviourOptionType):
 *   Definition: RlvBehaviourGenericProcessor<RLV_OPTION_TYPE>("commandname", RLV_BHVR_COMMANDNAME)
 *   Implement : nothing! (it automagically works)
 * For simple behaviours that only require recordkeeping and only run code when they toggle:
 *   Definition: RlvBehaviourGenericToggleProcessor<RLV_BHVR_COMMANDNAME, RLV_OPTION_TYPE>("commandname"))
 *   Implement : void RlvBehaviourToggleHandler<RLV_BHVR_COMMANDNAME>::onCommandToggle(ERlvBehaviour eBhvr, bool fHasBhvr)
 * For behaviours that require manual processing:
 *   Definition: RlvBehaviourProcessor<RLV_BHVR_COMMANDNAME>("commandname"))
 *   Implement : ERlvCmdRet RlvBehaviourHandler<RLV_BHVR_COMMANDNAME>::onCommand(const RlvCommand& rlvCmd, bool& fRefCount)
 * For behaviours that run code when their modifier changes:
 *   Definition: addModifier(RLV_BHVR_COMMANDNAME, RLV_MODIFIER_COMMANDNAME, new RlvBehaviourModifierHandler<RLV_MODIFIER_COMMANDNAME>(<default>, <auto-add>, <comparator>));
 *   Implement : void RlvBehaviourModifierHandler::onValueChanged()
 *
 */
RlvBehaviourDictionary::RlvBehaviourDictionary()
{
    // Array auto-initialization to 0 is still not supported in VS2013
    memset(m_BehaviourModifiers, 0, sizeof(RlvBehaviourModifier*) * RLV_MODIFIER_COUNT);

    //
    // Restrictions
    //
    // Charis: stages 1-2 only (research/248 2.1). Stage 3 behaviours (camera, environment, debug settings, effects,
    // @fly @jump @alwaysrun @temprun @allowidle @sendgesture, @touch* @fartouch @interact @edit* @rez, @showinv
    // @view* @showhovertext* @shownearby @showself*, @share @pay @buy @setgroup @getgroup @getsitid, @adjustheight)
    // are not registered: they don't show up in @getcommand and fail with RLV_RET_FAILED_UNKNOWN.
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE>("acceptpermission", RLV_BHVR_ACCEPTPERMISSION));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE_OR_EXCEPTION>("accepttp", RLV_BHVR_ACCEPTTP, RlvBehaviourInfo::BHVR_STRICT));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE_OR_EXCEPTION>("accepttprequest", RLV_BHVR_ACCEPTTPREQUEST, RlvBehaviourInfo::BHVR_STRICT | RlvBehaviourInfo::BHVR_EXTENDED));
    addEntry(new RlvBehaviourProcessor<RLV_BHVR_ADDATTACH, RlvBehaviourAddRemAttachHandler>("addattach"));
    addEntry(new RlvBehaviourInfo("addoutfit",              RLV_BHVR_ADDOUTFIT,             RLV_TYPE_ADDREM));
    addEntry(new RlvBehaviourInfo("attachthis",             RLV_BHVR_ATTACHTHIS,            RLV_TYPE_ADDREM, RlvBehaviourInfo::FORCEWEAR_NODE));
    addEntry(new RlvBehaviourInfo("attachallthis",          RLV_BHVR_ATTACHTHIS,            RLV_TYPE_ADDREM, RlvBehaviourInfo::FORCEWEAR_SUBTREE));
    addEntry(new RlvBehaviourInfo("attachthis_except",      RLV_BHVR_ATTACHTHISEXCEPT,      RLV_TYPE_ADDREM, RlvBehaviourInfo::FORCEWEAR_NODE));
    addEntry(new RlvBehaviourInfo("attachallthis_except",   RLV_BHVR_ATTACHTHISEXCEPT,      RLV_TYPE_ADDREM, RlvBehaviourInfo::FORCEWEAR_SUBTREE));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE>("chatwhisper", RLV_BHVR_CHATWHISPER));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE>("chatnormal", RLV_BHVR_CHATNORMAL));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE>("chatshout", RLV_BHVR_CHATSHOUT));
    addEntry(new RlvBehaviourProcessor<RLV_BHVR_DETACH>("detach"));
    addEntry(new RlvBehaviourInfo("detachthis",             RLV_BHVR_DETACHTHIS,            RLV_TYPE_ADDREM, RlvBehaviourInfo::FORCEWEAR_NODE));
    addEntry(new RlvBehaviourInfo("detachallthis",          RLV_BHVR_DETACHTHIS,            RLV_TYPE_ADDREM, RlvBehaviourInfo::FORCEWEAR_SUBTREE));
    addEntry(new RlvBehaviourInfo("detachthis_except",      RLV_BHVR_DETACHTHISEXCEPT,      RLV_TYPE_ADDREM, RlvBehaviourInfo::FORCEWEAR_NODE));
    addEntry(new RlvBehaviourInfo("detachallthis_except",   RLV_BHVR_DETACHTHISEXCEPT,      RLV_TYPE_ADDREM, RlvBehaviourInfo::FORCEWEAR_SUBTREE));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE>("emote", RLV_BHVR_EMOTE));
    addEntry(new RlvBehaviourInfo("notify",                 RLV_BHVR_NOTIFY,                RLV_TYPE_ADDREM));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE>("permissive", RLV_BHVR_PERMISSIVE));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE_OR_EXCEPTION>("recvchat", RLV_BHVR_RECVCHAT, RlvBehaviourInfo::BHVR_STRICT));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_EXCEPTION>("recvchatfrom", RLV_BHVR_RECVCHATFROM, RlvBehaviourInfo::BHVR_STRICT));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE_OR_EXCEPTION>("recvemote", RLV_BHVR_RECVEMOTE, RlvBehaviourInfo::BHVR_STRICT));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_EXCEPTION>("recvemotefrom", RLV_BHVR_RECVEMOTEFROM, RlvBehaviourInfo::BHVR_STRICT));
    addEntry(new RlvBehaviourProcessor<RLV_BHVR_RECVIM, RlvBehaviourRecvSendStartIMHandler>("recvim", RlvBehaviourInfo::BHVR_STRICT));
    addModifier(RLV_BHVR_RECVIM, RLV_MODIFIER_RECVIMDISTMIN, new RlvBehaviourModifier("RecvIM Distance (Min)", F32_MAX, true, new RlvBehaviourModifierCompMax));
    addModifier(RLV_BHVR_RECVIM, RLV_MODIFIER_RECVIMDISTMAX, new RlvBehaviourModifier("RecvIM Distance (Max)", F32_MAX, true, new RlvBehaviourModifierCompMin));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_EXCEPTION>("recvimfrom", RLV_BHVR_RECVIMFROM, RlvBehaviourInfo::BHVR_STRICT));
    addEntry(new RlvBehaviourInfo("redirchat",              RLV_BHVR_REDIRCHAT,             RLV_TYPE_ADDREM));
    addEntry(new RlvBehaviourInfo("rediremote",             RLV_BHVR_REDIREMOTE,            RLV_TYPE_ADDREM));
    addEntry(new RlvBehaviourProcessor<RLV_BHVR_REMATTACH, RlvBehaviourAddRemAttachHandler>("remattach"));
    addEntry(new RlvBehaviourInfo("remoutfit",              RLV_BHVR_REMOUTFIT,             RLV_TYPE_ADDREM));
    addEntry(new RlvBehaviourProcessor<RLV_BHVR_SENDCHANNEL, RlvBehaviourSendChannelHandler>("sendchannel", RlvBehaviourInfo::BHVR_STRICT));
    addEntry(new RlvBehaviourProcessor<RLV_BHVR_SENDCHANNELEXCEPT, RlvBehaviourSendChannelHandler>("sendchannel_except", RlvBehaviourInfo::BHVR_STRICT | RlvBehaviourInfo::BHVR_EXPERIMENTAL));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE>("sendchat", RLV_BHVR_SENDCHAT));
    addEntry(new RlvBehaviourToggleProcessor<RLV_BHVR_SENDIM, RlvBehaviourRecvSendStartIMHandler>("sendim", RlvBehaviourInfo::BHVR_STRICT));
    addModifier(RLV_BHVR_SENDIM, RLV_MODIFIER_SENDIMDISTMIN, new RlvBehaviourModifier("SendIM Distance (Min)", F32_MAX, true, new RlvBehaviourModifierCompMax));
    addModifier(RLV_BHVR_SENDIM, RLV_MODIFIER_SENDIMDISTMAX, new RlvBehaviourModifier("SendIM Distance (Max)", F32_MAX, true, new RlvBehaviourModifierCompMin));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_EXCEPTION>("sendimto", RLV_BHVR_SENDIMTO, RlvBehaviourInfo::BHVR_STRICT));
    addEntry(new RlvBehaviourInfo("sharedunwear",           RLV_BHVR_SHAREDUNWEAR,          RLV_TYPE_ADDREM, RlvBehaviourInfo::BHVR_EXTENDED));
    addEntry(new RlvBehaviourInfo("sharedwear",             RLV_BHVR_SHAREDWEAR,            RLV_TYPE_ADDREM, RlvBehaviourInfo::BHVR_EXTENDED));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE>("showloc", RLV_BHVR_SHOWLOC));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE>("showminimap", RLV_BHVR_SHOWMINIMAP));
    addEntry(new RlvBehaviourToggleProcessor<RLV_BHVR_SHOWNAMES>("shownames", RlvBehaviourInfo::BHVR_STRICT));
    addEntry(new RlvBehaviourProcessor<RLV_BHVR_SHOWNAMETAGS>("shownametags", RlvBehaviourInfo::BHVR_STRICT));
    addModifier(RLV_BHVR_SHOWNAMETAGS, RLV_MODIFIER_SHOWNAMETAGSDIST, new RlvBehaviourModifierHandler<RLV_MODIFIER_SHOWNAMETAGSDIST>("Name Tags - Visible Distance", 0.0f, true, new RlvBehaviourModifierCompMin));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE>("showworldmap", RLV_BHVR_SHOWWORLDMAP));
    // RLV-SIT: addEntry "sit" (RlvBehaviourGenericProcessor<RLV_OPTION_NONE>, RLV_BHVR_SIT)
    // RLV-SIT: addEntry "sittp" (RlvBehaviourGenericProcessor<RLV_OPTION_NONE_OR_MODIFIER>, RLV_BHVR_SITTP)
    //          + addModifier(RLV_BHVR_SITTP, RLV_MODIFIER_SITTPDIST, "SitTp Distance", RLV_MODIFIER_SITTP_DEFAULT, RlvBehaviourModifierCompMin)
    // RLV-SIT: addEntry "standtp" (RlvBehaviourGenericProcessor<RLV_OPTION_NONE>, RLV_BHVR_STANDTP)
    addEntry(new RlvBehaviourProcessor<RLV_BHVR_STARTIM, RlvBehaviourRecvSendStartIMHandler>("startim", RlvBehaviourInfo::BHVR_STRICT));
    addModifier(RLV_BHVR_STARTIM, RLV_MODIFIER_STARTIMDISTMIN, new RlvBehaviourModifier("StartIM Distance (Min)", F32_MAX, true, new RlvBehaviourModifierCompMax));
    addModifier(RLV_BHVR_STARTIM, RLV_MODIFIER_STARTIMDISTMAX, new RlvBehaviourModifier("StartIM Distance (Max)", F32_MAX, true, new RlvBehaviourModifierCompMin));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_EXCEPTION>("startimto", RLV_BHVR_STARTIMTO, RlvBehaviourInfo::BHVR_STRICT));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE>("tplm", RLV_BHVR_TPLM));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE>("tploc", RLV_BHVR_TPLOC));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE_OR_MODIFIER>("tplocal", RLV_BHVR_TPLOCAL, RlvBehaviourInfo::BHVR_EXPERIMENTAL));
    addModifier(RLV_BHVR_TPLOCAL, RLV_MODIFIER_TPLOCALDIST, new RlvBehaviourModifier("Local Teleport Distance", RLV_MODIFIER_TPLOCAL_DEFAULT, true, new RlvBehaviourModifierCompMin));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE_OR_EXCEPTION>("tplure", RLV_BHVR_TPLURE, RlvBehaviourInfo::BHVR_STRICT));
    addEntry(new RlvBehaviourGenericProcessor<RLV_OPTION_NONE_OR_EXCEPTION>("tprequest", RLV_BHVR_TPREQUEST, RlvBehaviourInfo::BHVR_STRICT | RlvBehaviourInfo::BHVR_EXTENDED));
    addEntry(new RlvBehaviourInfo("unsharedunwear",         RLV_BHVR_UNSHAREDUNWEAR,        RLV_TYPE_ADDREM));
    addEntry(new RlvBehaviourInfo("unsharedwear",           RLV_BHVR_UNSHAREDWEAR,          RLV_TYPE_ADDREM));
    // RLV-SIT: addEntry "unsit" (RlvBehaviourGenericProcessor<RLV_OPTION_NONE>, RLV_BHVR_UNSIT)

    //
    // Force-wear
    //
    addEntry(new RlvBehaviourInfo("attach",                 RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_REPLACE | RlvBehaviourInfo::FORCEWEAR_NODE    | RlvBehaviourInfo::FORCEWEAR_CONTEXT_NONE));
    addEntry(new RlvBehaviourInfo("attachall",              RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_REPLACE | RlvBehaviourInfo::FORCEWEAR_SUBTREE | RlvBehaviourInfo::FORCEWEAR_CONTEXT_NONE));
    addEntry(new RlvBehaviourInfo("attachover",             RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_ADD     | RlvBehaviourInfo::FORCEWEAR_NODE    | RlvBehaviourInfo::FORCEWEAR_CONTEXT_NONE));
    addEntry(new RlvBehaviourInfo("attachallover",          RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_ADD     | RlvBehaviourInfo::FORCEWEAR_SUBTREE | RlvBehaviourInfo::FORCEWEAR_CONTEXT_NONE));
    addEntry(new RlvBehaviourInfo("attachthis",             RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_REPLACE | RlvBehaviourInfo::FORCEWEAR_NODE    | RlvBehaviourInfo::FORCEWEAR_CONTEXT_OBJECT));
    addEntry(new RlvBehaviourInfo("attachallthis",          RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_REPLACE | RlvBehaviourInfo::FORCEWEAR_SUBTREE | RlvBehaviourInfo::FORCEWEAR_CONTEXT_OBJECT));
    addEntry(new RlvBehaviourInfo("attachthisover",         RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_ADD     | RlvBehaviourInfo::FORCEWEAR_NODE    | RlvBehaviourInfo::FORCEWEAR_CONTEXT_OBJECT));
    addEntry(new RlvBehaviourInfo("attachallthisover",      RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_ADD     | RlvBehaviourInfo::FORCEWEAR_SUBTREE | RlvBehaviourInfo::FORCEWEAR_CONTEXT_OBJECT));
    addEntry(new RlvForceProcessor<RLV_BHVR_DETACH, RlvForceRemAttachHandler>("detach",         RlvBehaviourInfo::FORCEWEAR_WEAR_REMOVE  | RlvBehaviourInfo::FORCEWEAR_NODE    | RlvBehaviourInfo::FORCEWEAR_CONTEXT_NONE));
    addEntry(new RlvBehaviourInfo("detachall",              RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_REMOVE  | RlvBehaviourInfo::FORCEWEAR_SUBTREE | RlvBehaviourInfo::FORCEWEAR_CONTEXT_NONE));
    addEntry(new RlvBehaviourInfo("detachthis",             RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_REMOVE  | RlvBehaviourInfo::FORCEWEAR_NODE    | RlvBehaviourInfo::FORCEWEAR_CONTEXT_OBJECT));
    addEntry(new RlvBehaviourInfo("detachallthis",          RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_REMOVE  | RlvBehaviourInfo::FORCEWEAR_SUBTREE | RlvBehaviourInfo::FORCEWEAR_CONTEXT_OBJECT));
    addEntry(new RlvForceProcessor<RLV_BHVR_REMATTACH, RlvForceRemAttachHandler>("remattach",   RlvBehaviourInfo::FORCEWEAR_WEAR_REMOVE  | RlvBehaviourInfo::FORCEWEAR_NODE    | RlvBehaviourInfo::FORCEWEAR_CONTEXT_NONE));
    addEntry(new RlvForceProcessor<RLV_BHVR_REMOUTFIT>("remoutfit",                             RlvBehaviourInfo::FORCEWEAR_WEAR_REMOVE  | RlvBehaviourInfo::FORCEWEAR_NODE    | RlvBehaviourInfo::FORCEWEAR_CONTEXT_NONE));
    // Synonyms (addoutfit* -> attach*)
    addEntry(new RlvBehaviourInfo("addoutfit",              RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_REPLACE | RlvBehaviourInfo::FORCEWEAR_NODE    | RlvBehaviourInfo::FORCEWEAR_CONTEXT_NONE   | RlvBehaviourInfo::BHVR_SYNONYM));
    addEntry(new RlvBehaviourInfo("addoutfitall",           RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_REPLACE | RlvBehaviourInfo::FORCEWEAR_SUBTREE | RlvBehaviourInfo::FORCEWEAR_CONTEXT_NONE   | RlvBehaviourInfo::BHVR_SYNONYM));
    addEntry(new RlvBehaviourInfo("addoutfitover",          RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_ADD     | RlvBehaviourInfo::FORCEWEAR_NODE    | RlvBehaviourInfo::FORCEWEAR_CONTEXT_NONE   | RlvBehaviourInfo::BHVR_SYNONYM));
    addEntry(new RlvBehaviourInfo("addoutfitallover",       RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_ADD     | RlvBehaviourInfo::FORCEWEAR_SUBTREE | RlvBehaviourInfo::FORCEWEAR_CONTEXT_NONE   | RlvBehaviourInfo::BHVR_SYNONYM));
    addEntry(new RlvBehaviourInfo("addoutfitthis",          RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_REPLACE | RlvBehaviourInfo::FORCEWEAR_NODE    | RlvBehaviourInfo::FORCEWEAR_CONTEXT_OBJECT | RlvBehaviourInfo::BHVR_SYNONYM));
    addEntry(new RlvBehaviourInfo("addoutfitallthis",       RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_REPLACE | RlvBehaviourInfo::FORCEWEAR_SUBTREE | RlvBehaviourInfo::FORCEWEAR_CONTEXT_OBJECT | RlvBehaviourInfo::BHVR_SYNONYM));
    addEntry(new RlvBehaviourInfo("addoutfitthisover",      RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_ADD     | RlvBehaviourInfo::FORCEWEAR_NODE    | RlvBehaviourInfo::FORCEWEAR_CONTEXT_OBJECT | RlvBehaviourInfo::BHVR_SYNONYM));
    addEntry(new RlvBehaviourInfo("addoutfitallthisover",   RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_ADD     | RlvBehaviourInfo::FORCEWEAR_SUBTREE | RlvBehaviourInfo::FORCEWEAR_CONTEXT_OBJECT | RlvBehaviourInfo::BHVR_SYNONYM));
    // Synonyms (attach*overorreplace -> attach*)
    addEntry(new RlvBehaviourInfo("attachoverorreplace",    RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_REPLACE | RlvBehaviourInfo::FORCEWEAR_NODE    | RlvBehaviourInfo::FORCEWEAR_CONTEXT_NONE   | RlvBehaviourInfo::BHVR_SYNONYM));
    addEntry(new RlvBehaviourInfo("attachalloverorreplace", RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_REPLACE | RlvBehaviourInfo::FORCEWEAR_SUBTREE | RlvBehaviourInfo::FORCEWEAR_CONTEXT_NONE   | RlvBehaviourInfo::BHVR_SYNONYM));
    addEntry(new RlvBehaviourInfo("attachthisoverorreplace",RLV_CMD_FORCEWEAR,  RLV_TYPE_FORCE, RlvBehaviourInfo::FORCEWEAR_WEAR_REPLACE | RlvBehaviourInfo::FORCEWEAR_NODE    | RlvBehaviourInfo::FORCEWEAR_CONTEXT_OBJECT | RlvBehaviourInfo::BHVR_SYNONYM));
    addEntry(new RlvBehaviourInfo("attachallthisoverorreplace",RLV_CMD_FORCEWEAR,RLV_TYPE_FORCE,RlvBehaviourInfo::FORCEWEAR_WEAR_REPLACE | RlvBehaviourInfo::FORCEWEAR_SUBTREE | RlvBehaviourInfo::FORCEWEAR_CONTEXT_OBJECT | RlvBehaviourInfo::BHVR_SYNONYM));

    //
    // Force-only
    //
    addEntry(new RlvForceProcessor<RLV_BHVR_DETACHME>("detachme"));
    // RLV-SIT: addEntry "sit" (RlvForceProcessor<RLV_BHVR_SIT>), "sitground" (RlvForceProcessor<RLV_BHVR_SITGROUND>),
    //          "unsit" (RlvBehaviourInfo, RLV_BHVR_UNSIT, RLV_TYPE_FORCE)
    addEntry(new RlvForceProcessor<RLV_BHVR_TPTO>("tpto"));

    //
    // Reply-only
    //
    addEntry(new RlvBehaviourInfo("findfolder",             RLV_BHVR_FINDFOLDER,            RLV_TYPE_REPLY));
    addEntry(new RlvBehaviourInfo("findfolders",            RLV_BHVR_FINDFOLDERS,           RLV_TYPE_REPLY, RlvBehaviourInfo::BHVR_EXTENDED));
    addEntry(new RlvBehaviourInfo("getaddattachnames",      RLV_BHVR_GETADDATTACHNAMES,     RLV_TYPE_REPLY, RlvBehaviourInfo::BHVR_EXPERIMENTAL));
    addEntry(new RlvBehaviourInfo("getaddoutfitnames",      RLV_BHVR_GETADDOUTFITNAMES,     RLV_TYPE_REPLY, RlvBehaviourInfo::BHVR_EXPERIMENTAL));
    addEntry(new RlvBehaviourInfo("getattach",              RLV_BHVR_GETATTACH,             RLV_TYPE_REPLY));
    addEntry(new RlvBehaviourInfo("getattachnames",         RLV_BHVR_GETATTACHNAMES,        RLV_TYPE_REPLY, RlvBehaviourInfo::BHVR_EXPERIMENTAL));
    addEntry(new RlvReplyProcessor<RLV_BHVR_GETCOMMAND>("getcommand", RlvBehaviourInfo::BHVR_EXTENDED));
    addEntry(new RlvBehaviourInfo("getinv",                 RLV_BHVR_GETINV,                RLV_TYPE_REPLY));
    addEntry(new RlvBehaviourInfo("getinvworn",             RLV_BHVR_GETINVWORN,            RLV_TYPE_REPLY));
    addEntry(new RlvBehaviourInfo("getoutfit",              RLV_BHVR_GETOUTFIT,             RLV_TYPE_REPLY));
    addEntry(new RlvBehaviourInfo("getoutfitnames",         RLV_BHVR_GETOUTFITNAMES,        RLV_TYPE_REPLY, RlvBehaviourInfo::BHVR_EXPERIMENTAL));
    addEntry(new RlvBehaviourInfo("getpath",                RLV_BHVR_GETPATH,               RLV_TYPE_REPLY));
    addEntry(new RlvBehaviourInfo("getpathnew",             RLV_BHVR_GETPATHNEW,            RLV_TYPE_REPLY));
    addEntry(new RlvBehaviourInfo("getremattachnames",      RLV_BHVR_GETREMATTACHNAMES,     RLV_TYPE_REPLY, RlvBehaviourInfo::BHVR_EXPERIMENTAL));
    addEntry(new RlvBehaviourInfo("getremoutfitnames",      RLV_BHVR_GETREMOUTFITNAMES,     RLV_TYPE_REPLY, RlvBehaviourInfo::BHVR_EXPERIMENTAL));
    addEntry(new RlvBehaviourInfo("getstatus",              RLV_BHVR_GETSTATUS,             RLV_TYPE_REPLY));
    addEntry(new RlvBehaviourInfo("getstatusall",           RLV_BHVR_GETSTATUSALL,          RLV_TYPE_REPLY));
    addEntry(new RlvBehaviourInfo("version",                RLV_BHVR_VERSION,               RLV_TYPE_REPLY));
    addEntry(new RlvBehaviourInfo("versionnew",             RLV_BHVR_VERSIONNEW,            RLV_TYPE_REPLY));
    addEntry(new RlvBehaviourInfo("versionnum",             RLV_BHVR_VERSIONNUM,            RLV_TYPE_REPLY));

    // Populate m_String2InfoMap (the tuple <behaviour, type> should be unique)
    for (const RlvBehaviourInfo* pBhvrInfo : m_BhvrInfoList)
    {
        RLV_VERIFY(m_String2InfoMap.insert(std::make_pair(std::make_pair(pBhvrInfo->getBehaviour(), (ERlvParamType)pBhvrInfo->getParamTypeMask()), pBhvrInfo)).second == true);
    }

    // Populate m_Bhvr2InfoMap (there can be multiple entries per ERlvBehaviour)
    for (const RlvBehaviourInfo* pBhvrInfo : m_BhvrInfoList)
    {
        if ( (pBhvrInfo->getParamTypeMask() & RLV_TYPE_ADDREM) && (!pBhvrInfo->isSynonym()) )
        {
#ifdef RLV_DEBUG
            for (const rlv_bhvr2info_map_t::value_type& itBhvr : boost::make_iterator_range(m_Bhvr2InfoMap.lower_bound(pBhvrInfo->getBehaviourType()), m_Bhvr2InfoMap.upper_bound(pBhvrInfo->getBehaviourType())))
            {
                RLV_ASSERT( (itBhvr.first != pBhvrInfo->getBehaviourType()) || (itBhvr.second->getBehaviourFlags() != pBhvrInfo->getBehaviourFlags()) );
            }
#endif // RLV_DEBUG
            m_Bhvr2InfoMap.insert(std::pair<ERlvBehaviour, const RlvBehaviourInfo*>(pBhvrInfo->getBehaviourType(), pBhvrInfo));
        }
    }

    // Process blocked commands
    // Charis: RestrainedLoveNoSetEnv only ever blocked @setenv (stage 3, not registered); RLVaBlockedCommands generalises it
    refreshBlockedCommands();
}

RlvBehaviourDictionary::~RlvBehaviourDictionary()
{
    for (const RlvBehaviourInfo* pBhvrInfo : m_BhvrInfoList)
        delete pBhvrInfo;
    m_BhvrInfoList.clear();

    for (int idxBhvrMod = 0; idxBhvrMod < RLV_MODIFIER_COUNT; idxBhvrMod++)
    {
        delete m_BehaviourModifiers[idxBhvrMod];
        m_BehaviourModifiers[idxBhvrMod] = nullptr;
    }
}

void RlvBehaviourDictionary::addEntry(const RlvBehaviourInfo* pEntry)
{
    // Filter experimental commands (if disabled)
    static LLCachedControl<bool> sEnableExperimental(gSavedSettings, std::string(RlvSettingNames::ExperimentalCommands));
    if ( (!pEntry) || ((!sEnableExperimental) && (pEntry->isExperimental())) )
    {
        delete pEntry;  // Charis: FS leaked the filtered entry
        return;
    }

    // Sanity check for duplicate entries
#ifndef LL_RELEASE_FOR_DOWNLOAD
    std::for_each(m_BhvrInfoList.begin(), m_BhvrInfoList.end(),
                  [&pEntry](const RlvBehaviourInfo* pBhvrInfo) {
                    RLV_ASSERT_DBG( ((pBhvrInfo->getBehaviour()) != (pEntry->getBehaviour())) || ((pBhvrInfo->getParamTypeMask() & pEntry->getParamTypeMask()) == 0) );
                  });
#endif // LL_RELEASE_FOR_DOWNLOAD

    m_BhvrInfoList.push_back(pEntry);
}

void RlvBehaviourDictionary::addModifier(ERlvBehaviour eBhvr, ERlvBehaviourModifier eModifier, RlvBehaviourModifier* pModifierEntry)
{
    if (eModifier < RLV_MODIFIER_COUNT)
    {
        RLV_ASSERT(!m_BehaviourModifiers[eModifier]);
        delete m_BehaviourModifiers[eModifier];
        m_BehaviourModifiers[eModifier] = pModifierEntry;
        m_Bhvr2ModifierMap.insert(std::make_pair(eBhvr, eModifier));
    }
    else
    {
        delete pModifierEntry;
    }
}

// Convenience function to add both the behaviour entry as well as the corresponding modifier entry
void RlvBehaviourDictionary::addModifier(const RlvBehaviourInfo* pBhvrEntry, ERlvBehaviourModifier eModifier, RlvBehaviourModifier* pModifierEntry)
{
    if (!pBhvrEntry)
    {
        delete pModifierEntry;
        return;
    }
    // Charis: read the type first, addEntry() deletes an entry it filters out
    const ERlvBehaviour eBhvr = pBhvrEntry->getBehaviourType();
    addEntry(pBhvrEntry);
    addModifier(eBhvr, eModifier, pModifierEntry);
}

// TODO: this shouldn't be in this class - find a better spot for it
void RlvBehaviourDictionary::clearModifiers(const LLUUID& idRlvObj)
{
    for (int idxModifier = 0; idxModifier < RLV_MODIFIER_COUNT; idxModifier++)
    {
        if (m_BehaviourModifiers[idxModifier])
            m_BehaviourModifiers[idxModifier]->clearValues(idRlvObj);
    }
}

const RlvBehaviourInfo* RlvBehaviourDictionary::getBehaviourInfo(ERlvBehaviour eBhvr, ERlvParamType eParamType) const
{
    const RlvBehaviourInfo* pBhvrInfo = nullptr;
    for (auto itBhvrLower = m_Bhvr2InfoMap.lower_bound(eBhvr), itBhvrUpper = m_Bhvr2InfoMap.upper_bound(eBhvr);
         std::find_if(itBhvrLower, itBhvrUpper, [eParamType](const rlv_bhvr2info_map_t::value_type& bhvrEntry) { return bhvrEntry.second->getParamTypeMask() == eParamType; }) != itBhvrUpper;
        ++itBhvrLower)
    {
        if (pBhvrInfo)
            return nullptr;
        pBhvrInfo = itBhvrLower->second;
    }
    return pBhvrInfo;
}

const RlvBehaviourInfo* RlvBehaviourDictionary::getBehaviourInfo(const std::string& strBhvr, ERlvParamType eParamType, bool* pfStrict, ERlvLocalBhvrModifier* peBhvrModifier) const
{
    size_t idxBhvrLastPart = strBhvr.find_last_of('_');
    std::string strBhvrLastPart((std::string::npos != idxBhvrLastPart) && (idxBhvrLastPart < strBhvr.size()) ? strBhvr.substr(idxBhvrLastPart + 1) : LLStringUtil::null);

    bool fStrict = (strBhvrLastPart.compare("sec") == 0);
    if (pfStrict)
        *pfStrict = fStrict;
    ERlvLocalBhvrModifier eBhvrModifier = ERlvLocalBhvrModifier::Unknown;

    rlv_string2info_map_t::const_iterator itBhvr = m_String2InfoMap.find(std::make_pair( (!fStrict) ? strBhvr : strBhvr.substr(0, strBhvr.size() - 4), (eParamType & RLV_TYPE_ADDREM) ? RLV_TYPE_ADDREM : eParamType));
    if ( (m_String2InfoMap.end() == itBhvr) && (!fStrict) && (!strBhvrLastPart.empty()) && (RLV_TYPE_FORCE == eParamType) )
    {
        // No match found but it could still be a local scope modifier
        auto itBhvrMod = m_String2InfoMap.find(std::make_pair(strBhvr.substr(0, idxBhvrLastPart), RLV_TYPE_ADDREM));
        if ( (m_String2InfoMap.end() != itBhvrMod) && (eBhvrModifier = itBhvrMod->second->lookupBehaviourModifier(strBhvrLastPart)) != ERlvLocalBhvrModifier::Unknown)
            itBhvr = itBhvrMod;
    }

    if (peBhvrModifier)
        *peBhvrModifier = eBhvrModifier;
    return ( (itBhvr != m_String2InfoMap.end()) && ((!fStrict) || (itBhvr->second->hasStrict())) ) ? itBhvr->second : nullptr;
}

ERlvBehaviour RlvBehaviourDictionary::getBehaviourFromString(const std::string& strBhvr, ERlvParamType eParamType, bool* pfStrict) const
{
    ERlvLocalBhvrModifier eBhvrModifier;
    const RlvBehaviourInfo* pBhvrInfo = getBehaviourInfo(strBhvr, eParamType, pfStrict, &eBhvrModifier);
    // Filter out locally scoped modifier commands since they don't actually have a unique behaviour value of their own
    return (pBhvrInfo && ERlvLocalBhvrModifier::Unknown == eBhvrModifier) ? pBhvrInfo->getBehaviourType() : RLV_BHVR_UNKNOWN;
}

bool RlvBehaviourDictionary::getCommands(const std::string& strMatch, ERlvParamType eParamType, std::list<std::string>& cmdList) const
{
    cmdList.clear();
    for (const RlvBehaviourInfo* pBhvrInfo : m_BhvrInfoList)
    {
        if ( (pBhvrInfo->getParamTypeMask() & eParamType) || (RLV_TYPE_UNKNOWN == eParamType) )
        {
            std::string strCmd = pBhvrInfo->getBehaviour();
            if ( (std::string::npos != strCmd.find(strMatch)) || (strMatch.empty()) )
                cmdList.push_back(strCmd);
            if ( (pBhvrInfo->hasStrict()) && ((std::string::npos != strCmd.append("_sec").find(strMatch)) || (strMatch.empty())) )
                cmdList.push_back(strCmd);
        }
    }
    return !cmdList.empty();
}

bool RlvBehaviourDictionary::getHasStrict(ERlvBehaviour eBhvr) const
{
    for (const rlv_bhvr2info_map_t::value_type& itBhvr : boost::make_iterator_range(m_Bhvr2InfoMap.lower_bound(eBhvr), m_Bhvr2InfoMap.upper_bound(eBhvr)))
    {
        // Only restrictions can be strict
        if (RLV_TYPE_ADDREM != itBhvr.second->getParamTypeMask())
            continue;
        return itBhvr.second->hasStrict();
    }
    RLV_ASSERT(false);
    return false;
}

RlvBehaviourModifier* RlvBehaviourDictionary::getModifierFromBehaviour(ERlvBehaviour eBhvr) const
{
    rlv_bhvr2mod_map_t::const_iterator itMod = m_Bhvr2ModifierMap.find(eBhvr);
    ERlvBehaviourModifier eBhvrMod = (m_Bhvr2ModifierMap.end() != itMod) ? itMod->second : RLV_MODIFIER_UNKNOWN;
    return (eBhvrMod < RLV_MODIFIER_UNKNOWN) ? m_BehaviourModifiers[eBhvrMod] : nullptr;
}

void RlvBehaviourDictionary::toggleBehaviourFlag(const std::string& strBhvr, ERlvParamType eParamType, RlvBehaviourInfo::EBehaviourFlags eBhvrFlag, bool fEnable)
{
    rlv_string2info_map_t::const_iterator itBhvr = m_String2InfoMap.find(std::make_pair(strBhvr, (eParamType & RLV_TYPE_ADDREM) ? RLV_TYPE_ADDREM : eParamType));
    if (m_String2InfoMap.end() != itBhvr)
    {
        const_cast<RlvBehaviourInfo*>(itBhvr->second)->toggleBehaviourFlag(eBhvrFlag, fEnable);
    }
}

bool RlvBehaviourDictionary::isBlocked(ERlvBehaviour eBhvr)
{
    refreshBlockedCommands();
    return (eBhvr < RLV_BHVR_COUNT) && (m_BlockedBhvrs.test(eBhvr));
}

void RlvBehaviourDictionary::refreshBlockedCommands()
{
    // RlvSettings parses RLVaBlockedCommands and bumps the version whenever the setting changes
    const U32 nVersion = RlvSettings::getBlockedCommandsVersion();
    if ( (m_fBlockedCommandsApplied) && (nVersion == m_nBlockedCommandsVersion) )
        return;
    m_fBlockedCommandsApplied = true;
    m_nBlockedCommandsVersion = nVersion;

    m_BlockedBhvrs.reset();
    for (const RlvBehaviourInfo* pBhvrInfo : m_BhvrInfoList)
    {
        // Every command type (restriction, force and reply) of a named behaviour is blocked, "_sec" included
        const bool fBlocked = RlvSettings::isBlockedCommand(pBhvrInfo->getBehaviour());
        const_cast<RlvBehaviourInfo*>(pBhvrInfo)->toggleBehaviourFlag(RlvBehaviourInfo::BHVR_BLOCKED, fBlocked);
        if ( (fBlocked) && (pBhvrInfo->getBehaviourType() < RLV_BHVR_COUNT) )
            m_BlockedBhvrs.set(pBhvrInfo->getBehaviourType());
    }
}

// ============================================================================
// RlvBehaviourInfo
//

// virtual
ERlvCmdRet RlvBehaviourInfo::processModifier(const RlvCommand& rlvCmd) const
{
    // The object should have the base behaviour set (or else there's nothing to modify)
    if (!gRlvHandler.hasBehaviour(rlvCmd.getObjectID(), rlvCmd.getBehaviourType()))
        return RLV_RET_FAILED_UNHELDBEHAVIOUR;

    auto itBhvrModifier = std::find_if(m_BhvrModifiers.begin(), m_BhvrModifiers.end(), [&rlvCmd](const modifier_lookup_t::value_type& entry) { return std::get<0>(entry.second) == rlvCmd.getBehaviourModifier(); });
    if (m_BhvrModifiers.end() == itBhvrModifier)
        return RLV_RET_FAILED_UNKNOWN;

    ERlvCmdRet eCmdRet; const modifier_handler_func_t& fnHandler = std::get<2>(itBhvrModifier->second);
    if (rlvCmd.hasOption())
    {
        // If there's an option parse it (and perform type checking)
        RlvBehaviourModifierValue modValue;
        if ( (rlvCmd.hasOption()) && (!RlvBehaviourModifier::convertOptionValue(rlvCmd.getOption(), std::get<1>(itBhvrModifier->second), modValue)) )
            return RLV_RET_FAILED_OPTION;
        eCmdRet = (fnHandler) ? fnHandler(rlvCmd.getObjectID(), modValue) : RLV_RET_SUCCESS;
        if (RLV_RET_SUCCESS == eCmdRet)
            gRlvHandler.getObject(rlvCmd.getObjectID())->setModifierValue(rlvCmd.getBehaviourModifier(), modValue);
    }
    else
    {
        eCmdRet = (fnHandler) ? fnHandler(rlvCmd.getObjectID(), boost::none) : RLV_RET_SUCCESS;
        if (RLV_RET_SUCCESS == eCmdRet)
            gRlvHandler.getObject(rlvCmd.getObjectID())->clearModifierValue(rlvCmd.getBehaviourModifier());
    }

    return eCmdRet;
}

// ============================================================================
// RlvBehaviourModifier
//

RlvBehaviourModifier::RlvBehaviourModifier(std::string strName, const RlvBehaviourModifierValue& defaultValue, bool fAddDefaultOnEmpty, RlvBehaviourModifierComp* pValueComparator)
    : m_strName(strName), m_DefaultValue(defaultValue), m_fAddDefaultOnEmpty(fAddDefaultOnEmpty), m_pValueComparator(pValueComparator)
{
    m_pValueComparator = (pValueComparator) ? pValueComparator : new RlvBehaviourModifierComp();
}

RlvBehaviourModifier::~RlvBehaviourModifier()
{
    if (m_pValueComparator)
    {
        delete m_pValueComparator;
        m_pValueComparator = NULL;
    }
}

bool RlvBehaviourModifier::addValue(const RlvBehaviourModifierValue& modValue, const LLUUID& idRlvObj, ERlvBehaviour eBhvr)
{
    if (modValue.which() == m_DefaultValue.which())
    {
        m_Values.insert((m_pValueComparator) ? std::lower_bound(m_Values.begin(), m_Values.end(), std::make_tuple(modValue, idRlvObj, eBhvr), boost::bind(&RlvBehaviourModifierComp::operator(), m_pValueComparator, _1, _2)) : m_Values.end(), std::make_tuple(modValue, idRlvObj, eBhvr));
        // NOTE: change signal needs to trigger before modifier handlers so cached values have a chance to update properly
        m_ChangeSignal(getValue());
        onValueChange();
        return true;
    }
    return false;
}

void RlvBehaviourModifier::clearValues(const LLUUID& idRlvObj)
{
    size_t origCount = m_Values.size();
    m_Values.erase(std::remove_if(m_Values.begin(), m_Values.end(),
                                  [&idRlvObj](const RlvBehaviourModifierValueTuple& modValue) {
                                    return (std::get<1>(modValue) == idRlvObj) && (std::get<2>(modValue) == RLV_BHVR_UNKNOWN);
                                  }), m_Values.end());
    if (origCount != m_Values.size())
    {
        onValueChange();
        m_ChangeSignal(getValue());
    }
}

const LLUUID& RlvBehaviourModifier::getPrimaryObject() const
{
    return (m_pValueComparator) ? m_pValueComparator->m_idPrimaryObject : LLUUID::null;
}

bool RlvBehaviourModifier::hasValue() const {
    // If no primary object is set this returns "any value set"; otherwise it returns "any value set by the primary object"
    if ( (!m_pValueComparator) || (m_pValueComparator->m_idPrimaryObject.isNull()) )
        return !m_Values.empty();
    return (!m_Values.empty()) ? std::get<1>(m_Values.front()) == m_pValueComparator->m_idPrimaryObject : false;
}

bool RlvBehaviourModifier::hasValue(const LLUUID& idRlvObj) const
{
    return m_Values.end() != std::find_if(m_Values.begin(), m_Values.end(), [&idRlvObj](const RlvBehaviourModifierValueTuple& cmpValue) { return std::get<1>(cmpValue) == idRlvObj; });
}

void RlvBehaviourModifier::removeValue(const RlvBehaviourModifierValue& modValue, const LLUUID& idRlvObj, ERlvBehaviour eBhvr)
{
    if ( (modValue.which() == m_DefaultValue.which()) )
    {
        auto itValue = std::find(m_Values.begin(), m_Values.end(), std::make_tuple(modValue, idRlvObj, eBhvr));
        if (m_Values.end() != itValue)
        {
            m_Values.erase(itValue);
            onValueChange();
            m_ChangeSignal(getValue());
        }
    }
}

void RlvBehaviourModifier::setPrimaryObject(const LLUUID& idPrimaryObject)
{
    if (m_pValueComparator)
    {
        m_pValueComparator->m_idPrimaryObject = idPrimaryObject;
        m_Values.sort(boost::bind(&RlvBehaviourModifierComp::operator(), m_pValueComparator, _1, _2));
        onValueChange();
        m_ChangeSignal(getValue());
    }
}

void RlvBehaviourModifier::setValue(const RlvBehaviourModifierValue& modValue, const LLUUID& idRlvObj)
{
    if ( (modValue.which() == m_DefaultValue.which()) )
    {
        auto itValue = std::find_if(m_Values.begin(), m_Values.end(),
                                    [&idRlvObj](const RlvBehaviourModifierValueTuple& cmpValue) {
                                        return (std::get<1>(cmpValue) == idRlvObj) && (std::get<2>(cmpValue) == RLV_BHVR_UNKNOWN);
                                    });
        if (m_Values.end() != itValue)
        {
            // The object already has a modifier value set => change it
            std::get<0>(*itValue) = modValue;
            if (m_pValueComparator)
                m_Values.sort(boost::bind(&RlvBehaviourModifierComp::operator(), m_pValueComparator, _1, _2));
            onValueChange();
            m_ChangeSignal(getValue());
        }
        else
        {
            // The command doesn't have a modifier value yet => add it
            addValue(modValue, idRlvObj, RLV_BHVR_UNKNOWN);
        }
    }
}

// static
bool RlvBehaviourModifier::convertOptionValue(const std::string& optionValue, const std::type_index& modType, RlvBehaviourModifierValue& modValue,
                                              float nMinFloat, float nMaxFloat)
{
    try
    {
        if (modType == typeid(float))
        {
            // Charis: std::stof() reads "nan" and "inf"; reject those and clamp the rest (the value may come from anyone's script via a relay)
            float nValue = 0.f;
            if (!rlvParseFloat(optionValue, nValue))
                return false;
            modValue = llclamp(nValue, nMinFloat, nMaxFloat);
            return true;
        }
        else if (modType == typeid(int))
        {
            modValue = std::stoi(optionValue);
            return true;
        }
        else if (modType == typeid(LLVector3))
        {
            LLVector3 vecOption;
            if ( (3 == sscanf(optionValue.c_str(), "%f/%f/%f", vecOption.mV + 0, vecOption.mV + 1, vecOption.mV + 2)) && (rlvIsFinite(vecOption.mV, 3)) )
            {
                modValue = vecOption;
                return true;
            }
        }
        else if (modType == typeid(LLVector4))
        {
            LLVector4 vecOption;
            if ( (4 == sscanf(optionValue.c_str(), "%f/%f/%f/%f", vecOption.mV + 0, vecOption.mV + 1, vecOption.mV + 2, vecOption.mV + 3)) && (rlvIsFinite(vecOption.mV, 4)) )
            {
                modValue = vecOption;
                return true;
            }
        }
        else if (modType == typeid(LLUUID))
        {
            LLUUID idOption;
            if (idOption.set(optionValue, false))
            {
                modValue = idOption;
                return true;
            }
        }
        return false;
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
    catch (const std::out_of_range&)
    {
        return false;
    }
}

// ============================================================================
// RlvCommmand
//

RlvCommand::RlvCommand(const LLUUID& idObj, const std::string& strCommand)
    : m_idObj(idObj)
{
    if ((m_fValid = parseCommand(strCommand, m_strBehaviour, m_strOption, m_strParam)))
    {
        S32 nTemp = 0;
        if ( ("n" == m_strParam) || ("add" == m_strParam) )
            m_eParamType = RLV_TYPE_ADD;
        else if ( ("y" == m_strParam) || ("rem" == m_strParam) )
            m_eParamType = RLV_TYPE_REMOVE;
        else if (m_strBehaviour == "clear")                     // clear is the odd one out so just make it its own type
            m_eParamType = RLV_TYPE_CLEAR;
        else if ("force" == m_strParam)
            m_eParamType = RLV_TYPE_FORCE;
        else if (LLStringUtil::convertToS32(m_strParam, nTemp)) // Assume it's a reply command if we can convert <param> to an S32
            m_eParamType = RLV_TYPE_REPLY;
        else
        {
            m_eParamType = RLV_TYPE_UNKNOWN;
            m_fValid = false;
        }
    }

    if (!m_fValid)
    {
        m_strOption = m_strParam = "";
        return;
    }

    RlvBehaviourDictionary& rlvDictionary = RlvBehaviourDictionary::instance();
    rlvDictionary.refreshBlockedCommands();
    m_pBhvrInfo = rlvDictionary.getBehaviourInfo(m_strBehaviour, m_eParamType, &m_fStrict, &m_eBhvrModifier);
}

RlvCommand::RlvCommand(const RlvCommand& rlvCmd, ERlvParamType eParamType)
    : m_fValid(rlvCmd.m_fValid), m_idObj(rlvCmd.m_idObj), m_strBehaviour(rlvCmd.m_strBehaviour), m_pBhvrInfo(rlvCmd.m_pBhvrInfo)
    , m_eParamType( (RLV_TYPE_UNKNOWN == eParamType) ? rlvCmd.m_eParamType : eParamType), m_eBhvrModifier(rlvCmd.m_eBhvrModifier)
    , m_fStrict(rlvCmd.m_fStrict), m_strOption(rlvCmd.m_strOption), m_strParam(rlvCmd.m_strParam), m_fRefCounted(rlvCmd.m_fRefCounted)
{
}

bool RlvCommand::parseCommand(const std::string& strCommand, std::string& strBehaviour, std::string& strOption, std::string& strParam)
{
    // (See behaviour notes for the command parsing truth table)

    // Format: <behaviour>[:<option>]=<param>
    int idxParam  = static_cast<int>(strCommand.find('='));
    int idxOption = (idxParam > 0) ? static_cast<int>(strCommand.find(':')) : -1;
    if (idxOption > idxParam - 1)
        idxOption = -1;

    // If <behaviour> is missing it's always an improperly formatted command
    if ( (0 == idxOption) || (0 == idxParam) )
        return false;

    strBehaviour = strCommand.substr(0, (-1 != idxOption) ? idxOption : idxParam);
    strOption = strParam = "";

    // If <param> is missing it's an improperly formatted command
    if ( (-1 == idxParam) || ((int)strCommand.length() - 1 == idxParam) )
    {
        // Unless "<behaviour> == "clear" AND (idxOption == 0)"
        // OR <behaviour> == "clear" AND (idxParam != 0) [see table above]
        if ( ("clear" == strBehaviour) && ( (!idxOption) || (idxParam) ) )
            return true;
        return false;
    }

    if ( (-1 != idxOption) && (idxOption + 1 != idxParam) )
        strOption = strCommand.substr(idxOption + 1, idxParam - idxOption - 1);
    strParam = strCommand.substr(idxParam + 1);

    return true;
}

// ============================================================================
// Command option parsing utility classes
//

template<>
bool RlvCommandOptionHelper::parseOption<std::string>(const std::string& strOption, std::string& valueOption)
{
    valueOption = strOption;
    return true;
}

template<>
bool RlvCommandOptionHelper::parseOption<LLUUID>(const std::string& strOption, LLUUID& idOption)
{
    return idOption.set(strOption, false);
}

template<>
bool RlvCommandOptionHelper::parseOption<int>(const std::string& strOption, int& nOption)
{
    try
    {
        nOption = std::stoi(strOption);
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
    catch (const std::out_of_range&)
    {
        return false;
    }
    return true;
}

template<>
bool RlvCommandOptionHelper::parseOption<bool>(const std::string& strOption, bool& fOption)
{
    try
    {
        // Try and parse it as a number first
        int nOption = std::stoi(strOption);
        fOption = (bool)nOption;
        return (nOption == 0) || (nOption == 1);
    }
    catch (const std::invalid_argument&)
    {
        // Then try and parse it as true/false
        std::istringstream ss(strOption);
        ss >> std::boolalpha >> fOption;
        return !ss.fail();
    }
    catch (const std::out_of_range&)
    {
        return false;
    }
}

template<>
bool RlvCommandOptionHelper::parseOption<float>(const std::string& strOption, float& nOption)
{
    // Charis: also rejects nan/inf (callers clamp to their own range)
    return rlvParseFloat(strOption, nOption);
}

template<>
bool RlvCommandOptionHelper::parseOption<LLWearableType::EType>(const std::string& strOption, LLWearableType::EType& wtOption)
{
    wtOption = rlvTypeNameToType(strOption);
    return LLWearableType::WT_INVALID != wtOption;
}

template<>
bool RlvCommandOptionHelper::parseOption<LLViewerJointAttachment*>(const std::string& strOption, LLViewerJointAttachment*& pAttachPt)
{
    pAttachPt = RlvAttachPtLookup::getAttachPoint(strOption);
    return pAttachPt != NULL;
}

template<>
bool RlvCommandOptionHelper::parseOption<ERlvAttachGroupType>(const std::string& strOption, ERlvAttachGroupType& eAttachGroup)
{
    eAttachGroup = rlvAttachGroupFromString(strOption);
    return eAttachGroup != RLV_ATTACHGROUP_INVALID;
}

template<>
bool RlvCommandOptionHelper::parseOption<LLViewerInventoryCategory*>(const std::string& strOption, LLViewerInventoryCategory*& pFolder)
{
    pFolder = RlvInventory::instance().getSharedFolder(strOption);
    return pFolder != NULL;
}

template<>
bool RlvCommandOptionHelper::parseOption<LLVector2>(const std::string& strOption, LLVector2& vecOption)
{
    if (!strOption.empty())
    {
        S32 cntToken = sscanf(strOption.c_str(), "%f/%f", vecOption.mV + 0, vecOption.mV + 1);
        return (2 == cntToken) && (rlvIsFinite(vecOption.mV, 2));
    }
    return false;
}

template<>
bool RlvCommandOptionHelper::parseOption<LLVector3>(const std::string& strOption, LLVector3& vecOption)
 {
    if (!strOption.empty())
    {
        S32 cntToken = sscanf(strOption.c_str(), "%f/%f/%f", vecOption.mV + 0, vecOption.mV + 1, vecOption.mV + 2);
        return (3 == cntToken) && (rlvIsFinite(vecOption.mV, 3));
    }
    return false;
}

template<>
bool RlvCommandOptionHelper::parseOption<LLVector3d>(const std::string& strOption, LLVector3d& vecOption)
 {
    if (!strOption.empty())
    {
        S32 cntToken = sscanf(strOption.c_str(), "%lf/%lf/%lf", vecOption.mdV + 0, vecOption.mdV + 1, vecOption.mdV + 2);
        return (3 == cntToken) && (rlvIsFinite(vecOption.mdV, 3));
    }
    return false;
}

template<>
bool RlvCommandOptionHelper::parseOption<LLColor3>(const std::string& strOption, LLColor3& clrOption)
{
    if (!strOption.empty())
    {
        S32 cntToken = sscanf(strOption.c_str(), "%f/%f/%f", clrOption.mV + 0, clrOption.mV + 1, clrOption.mV + 2);
        return (3 == cntToken) && (rlvIsFinite(clrOption.mV, 3));
    }
    return false;
}

template<>
bool RlvCommandOptionHelper::parseOption<RlvCommandOptionGeneric>(const std::string& strOption, RlvCommandOptionGeneric& genericOption)
{
    LLWearableType::EType wtType(LLWearableType::WT_INVALID); LLUUID idOption; ERlvAttachGroupType eAttachGroup(RLV_ATTACHGROUP_INVALID);
    LLViewerJointAttachment* pAttachPt = NULL; LLViewerInventoryCategory* pFolder = NULL; LLVector3d posOption; float nOption;

    if (!strOption.empty())                                                                     // <option> could be an empty string
    {
        if (RlvCommandOptionHelper::parseOption(strOption, wtType))
            genericOption = wtType;                                                             // ... or specify a (valid) clothing layer
        else if (RlvCommandOptionHelper::parseOption(strOption, pAttachPt))
            genericOption = pAttachPt;                                                          // ... or specify an attachment point
        else if (RlvCommandOptionHelper::parseOption(strOption, idOption))
            genericOption = idOption;                                                           // ... or specify an UUID
        else if (RlvCommandOptionHelper::parseOption(strOption, pFolder))
            genericOption = pFolder;                                                            // ... or specify a shared folder path
        else if (RlvCommandOptionHelper::parseOption(strOption, eAttachGroup))
            genericOption = eAttachGroup;                                                       // ... or specify an attachment point group
        else if (RlvCommandOptionHelper::parseOption(strOption, posOption))
            genericOption = posOption;                                                          // ... or specify a vector (region or global coordinates)
        else if (RlvCommandOptionHelper::parseOption(strOption, nOption))
            genericOption = nOption;                                                            // ... or specify a number
        else
            genericOption = strOption;                                                          // ... or it might just be a string
    }
    return true;
}

bool RlvCommandOptionHelper::parseStringList(const std::string& strOption, std::vector<std::string>& optionList, const std::string& strSeparator)
{
    if (!strOption.empty())
        boost::split(optionList, strOption, boost::is_any_of(strSeparator));
    return !optionList.empty();
}

// ============================================================================
// RlvCommandOption structures
//

// Checked: 2012-07-28 (RLVa-1.4.7)
// Charis: FS listens to an attachment signal it adds to LLVOAvatarSelf (setAttachmentCallback), which the official
// viewer doesn't have. Instead the pending request looks the object up once per idle until it shows up as an
// attachment or 30 seconds pass. It only exists while a force-wear of "this object's folder" waits for the object.
class RlvCommandOptionGetPathCallback
{
public:
    RlvCommandOptionGetPathCallback(const LLUUID& idAttachObj, RlvCommandOptionGetPath::getpath_callback_t cb)
        : mObjectId(idAttachObj), mCallback(cb)
    {
        sPendingCount++;
        gIdleCallbacks.addFunction(&onIdle, this);
    }

    ~RlvCommandOptionGetPathCallback()
    {
        gIdleCallbacks.deleteFunction(&onIdle, this);
        sPendingCount--;
    }

    // Charis: each pending request costs one object lookup per idle for up to 30 seconds, so cap how many can wait
    static bool canAdd() { return sPendingCount < cMaxPending; }

    static void onIdle(void* pData)
    {
        RlvCommandOptionGetPathCallback* pInstance = reinterpret_cast<RlvCommandOptionGetPathCallback*>(pData);

        const LLViewerObject* pAttachObj = gObjectList.findObject(pInstance->mObjectId);
        if ( (pAttachObj) && (pAttachObj->isAttachment()) && (pAttachObj->permYouOwner()) && (pAttachObj->getAttachmentItemID().notNull()) )
        {
            uuid_vec_t idItems(1, pAttachObj->getAttachmentItemID());
            RlvCommandOptionGetPath::getpath_callback_t cb = pInstance->mCallback;
            delete pInstance;   // Unregisters first so the callback can't run twice whatever it does
            cb(idItems);
            return;
        }

        if (pInstance->mExpirationTimer.getElapsedTimeF32() > 30.0f)
            delete pInstance;
    }

protected:
    LLUUID                      mObjectId;
    RlvCommandOptionGetPath::getpath_callback_t mCallback;
    LLFrameTimer                mExpirationTimer;

    static constexpr S32        cMaxPending = 64;
    static S32                  sPendingCount;
};
S32 RlvCommandOptionGetPathCallback::sPendingCount = 0;

// Checked: 2010-11-30 (RLVa-1.3.0b) | Modified: RLVa-1.3.0b
RlvCommandOptionGetPath::RlvCommandOptionGetPath(const RlvCommand& rlvCmd, getpath_callback_t cb)
    : m_fCallback(false)
{
    m_fValid = true;    // Assume the option will be a valid one until we find out otherwise

    // @getpath[:<option>]=<channel> => <option> is transformed to a list of inventory item UUIDs to get the path of
    RlvCommandOptionGeneric rlvCmdOption;
    RlvCommandOptionHelper::parseOption(rlvCmd.getOption(), rlvCmdOption);
    if (rlvCmdOption.isWearableType())          // <option> can be a clothing layer
    {
        getItemIDs(rlvCmdOption.getWearableType(), m_idItems);
    }
    else if (rlvCmdOption.isAttachmentPoint())  // ... or it can specify an attachment point
    {
        getItemIDs(rlvCmdOption.getAttachmentPoint(), m_idItems);
    }
    else if (rlvCmdOption.isUUID())             // ... or it can specify a specific attachment
    {
        const LLViewerObject* pAttachObj = gObjectList.findObject(rlvCmdOption.getUUID());
        if ( (pAttachObj) && (pAttachObj->isAttachment()) && (pAttachObj->permYouOwner()) )
            m_idItems.push_back(pAttachObj->getAttachmentItemID());
        else
            m_fValid = false;
    }
    else if (rlvCmdOption.isEmpty())            // ... or it can be empty (in which case we act on the object that issued the command)
    {
        const LLViewerObject* pObj = gObjectList.findObject(rlvCmd.getObjectID());
        if (pObj)
        {
            if (pObj->isAttachment())
                m_idItems.push_back(pObj->getAttachmentItemID());
        }
        else if ( (cb) && (RlvCommandOptionGetPathCallback::canAdd()) )
        {
            new RlvCommandOptionGetPathCallback(rlvCmd.getObjectID(), cb);
            m_fCallback = true;
            return;
        }
    }
    else                                        // ... but anything else isn't a valid option
    {
        m_fValid = false;
        return;
    }

    if (cb)
    {
        cb(getItemIDs());
    }
}

// Checked: 2013-10-12 (RLVa-1.4.9)
bool RlvCommandOptionGetPath::getItemIDs(const LLViewerJointAttachment* pAttachPt, uuid_vec_t& idItems)
{
    uuid_vec_t::size_type cntItemsPrev = idItems.size();

    LLInventoryModel::cat_array_t folders; LLInventoryModel::item_array_t items;
    RlvFindAttachmentsOnPoint f(pAttachPt);
    gInventory.collectDescendentsIf(LLAppearanceMgr::instance().getCOF(), folders, items, false, f);
    for (LLInventoryModel::item_array_t::const_iterator itItem = items.begin(); itItem != items.end(); ++itItem)
    {
        const LLViewerInventoryItem* pItem = *itItem;
        if (pItem)
            idItems.push_back(pItem->getLinkedUUID());
    }

    return (cntItemsPrev != idItems.size());
}

// Checked: 2013-10-12 (RLVa-1.4.9)
bool RlvCommandOptionGetPath::getItemIDs(LLWearableType::EType wtType, uuid_vec_t& idItems)
{
    uuid_vec_t::size_type cntItemsPrev = idItems.size();

    LLInventoryModel::cat_array_t folders; LLInventoryModel::item_array_t items;
    LLFindWearablesOfType f(wtType);
    gInventory.collectDescendentsIf(LLAppearanceMgr::instance().getCOF(), folders, items, false, f);
    for (LLInventoryModel::item_array_t::const_iterator itItem = items.begin(); itItem != items.end(); ++itItem)
    {
        const LLViewerInventoryItem* pItem = *itItem;
        if (pItem)
            idItems.push_back(pItem->getLinkedUUID());
    }

    return (cntItemsPrev != idItems.size());
}

// =========================================================================
// RlvObject
//

RlvObject::RlvObject(const LLUUID& idObj) : m_idObj(idObj), m_nLookupMisses(0)
{
    LLViewerObject* pObj = gObjectList.findObject(idObj);
    m_fLookup = (NULL != pObj);
    m_idxAttachPt = (pObj) ? ATTACHMENT_ID_FROM_STATE(pObj->getAttachmentState()) : 0;
    m_idRoot = (pObj) ? pObj->getRootEdit()->getID() : LLUUID::null;
}

const RlvCommand& RlvObject::addCommand(const RlvCommand& rlvCmd, bool& fAdded)
{
    RLV_ASSERT(RLV_TYPE_ADD == rlvCmd.getParamType());

    // Don't add duplicate commands for this object (ie @detach=n followed by another @detach=n later on)
    for (rlv_command_list_t::iterator itCmd = m_Commands.begin(); itCmd != m_Commands.end(); ++itCmd)
    {
        if ( (itCmd->getBehaviour() == rlvCmd.getBehaviour()) && (itCmd->getOption() == rlvCmd.getOption()) &&
             (itCmd->isStrict() == rlvCmd.isStrict() ) )
        {
            fAdded = false;
            return *itCmd;
        }
    }

    // Now that we know it's not a duplicate, add it to the end of the list
    m_Commands.push_back(rlvCmd);
    fAdded = true;
    return m_Commands.back();
}

bool RlvObject::removeCommand(const RlvCommand& rlvCmd)
{
    RLV_ASSERT(RLV_TYPE_REMOVE == rlvCmd.getParamType());

    for (rlv_command_list_t::iterator itCmd = m_Commands.begin(); itCmd != m_Commands.end(); ++itCmd)
    {
        //if (*itCmd == rlvCmd) <- commands will never be equal since one is an add and the other is a remove *rolls eyes*
        if ( (itCmd->getBehaviour() == rlvCmd.getBehaviour()) && (itCmd->getOption() == rlvCmd.getOption()) &&
             (itCmd->isStrict() == rlvCmd.isStrict() ) )
        {
            m_Commands.erase(itCmd);
            return true;
        }
    }
    return false;   // Command was never added so nothing to remove now
}

bool RlvObject::hasBehaviour(ERlvBehaviour eBehaviour, bool fStrictOnly) const
{
    for (rlv_command_list_t::const_iterator itCmd = m_Commands.begin(); itCmd != m_Commands.end(); ++itCmd)
        if ( (itCmd->getBehaviourType() == eBehaviour) && (itCmd->getOption().empty()) && ((!fStrictOnly) || (itCmd->isStrict())) )
            return true;
    return false;
}

bool RlvObject::hasBehaviour(ERlvBehaviour eBehaviour, const std::string& strOption, bool fStrictOnly) const
{
    for (const RlvCommand& rlvCmd : m_Commands)
    {
        // The specified behaviour is contained within the current object if:
        //   - the (parsed) behaviour matches
        //   - the option matches (or we're checking for an empty option and the command was reference counted)
        //   - we're not matching on strict (or it is a strict command)
        if ( (rlvCmd.getBehaviourType() == eBehaviour) &&
             ( (rlvCmd.getOption() == strOption) || ((strOption.empty()) && (rlvCmd.isRefCounted())) ) &&
             ( (!fStrictOnly) ||(rlvCmd.isStrict()) ) )
        {
            return true;
        }
    }
    return false;
}

// Checked: 2009-11-27 (RLVa-1.1.0f) | Modified: RLVa-1.1.0f
std::string RlvObject::getStatusString(const std::string& strFilter, const std::string& strSeparator) const
{
    std::string strStatus, strCmd;

    for (rlv_command_list_t::const_iterator itCmd = m_Commands.begin(); itCmd != m_Commands.end(); ++itCmd)
    {
        strCmd = itCmd->asString();
        if ( (strFilter.empty()) || (std::string::npos != strCmd.find(strFilter)) )
            strStatus.append(strSeparator).append(strCmd);
    }

    return strStatus;
}

void RlvObject::clearModifiers(ERlvBehaviour eBhvr)
{
    if (const RlvBehaviourInfo* pBhvrInfo = RlvBehaviourDictionary::instance().getBehaviourInfo(eBhvr, RLV_TYPE_ADDREM))
    {
        for (const auto& modifierEntry : pBhvrInfo->getModifiers())
        {
            clearModifierValue(std::get<0>(modifierEntry.second));
        }
    }
}

void RlvObject::clearModifierValue(ERlvLocalBhvrModifier eBhvrModifier)
{
    m_Modifiers.erase(eBhvrModifier);
}

void RlvObject::setModifierValue(ERlvLocalBhvrModifier eBhvrModifier, const RlvBehaviourModifierValue& newValue)
{
    auto itBhvrModifierValue = m_Modifiers.find(eBhvrModifier);
    if (m_Modifiers.end() != itBhvrModifierValue)
        itBhvrModifierValue->second = newValue;
    else
        m_Modifiers.insert(std::make_pair(eBhvrModifier, newValue));
}
// ============================================================================
// RlvBehaviourNotifyObserver
//

// Checked: 2010-03-03 (RLVa-1.2.0a) | Added: RLVa-1.2.0a
RlvBehaviourNotifyHandler::RlvBehaviourNotifyHandler()
{
}

void RlvBehaviourNotifyHandler::addNotify(const LLUUID& idObj, S32 nChannel, const std::string& strFilter)
{
    m_Notifications.insert(std::pair<LLUUID, notifyData>(idObj, notifyData(nChannel, strFilter)));
    if (!m_ConnCommand.connected())
    {
        // NOTE: the reason we use rlv_command_signal_t instead of the better-suited rlv_behaviour_signal_t is because
        //       RLV will notify scripts about "invalid" commands so we need to as well
        m_ConnCommand = gRlvHandler.setCommandCallback(boost::bind(&RlvBehaviourNotifyHandler::onCommand, this, _1, _2, _3));
    }
}

void RlvBehaviourNotifyHandler::removeNotify(const LLUUID& idObj, S32 nChannel, const std::string& strFilter)
{
    for (std::multimap<LLUUID, notifyData>::iterator itNotify = m_Notifications.lower_bound(idObj),
            endNotify = m_Notifications.upper_bound(idObj); itNotify != endNotify; ++itNotify)
    {
        if ( (itNotify->second.nChannel == nChannel) && (itNotify->second.strFilter == strFilter) )
        {
            m_Notifications.erase(itNotify);
            break;
        }
    }
    // Nothing left to notify: stop listening to commands (FS deleted the singleton here)
    if ( (m_Notifications.empty()) && (m_ConnCommand.connected()) )
        m_ConnCommand.disconnect();
}

// Checked: 2010-03-03 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
void RlvBehaviourNotifyHandler::onCommand(const RlvCommand& rlvCmd, ERlvCmdRet eRet, bool fInternal)
{
    if (fInternal)
        return;

    switch (rlvCmd.getParamType())
    {
        case RLV_TYPE_ADD:
        case RLV_TYPE_REMOVE:
            sendNotification(rlvCmd.asString(), "=" + rlvCmd.getParam());
            break;
        case RLV_TYPE_CLEAR:
            sendNotification(rlvCmd.asString());
            break;
        default:
            break;
    }
}

// Checked: 2011-03-31 (RLVa-1.3.0f) | Modify: RLVa-1.3.0f
void RlvBehaviourNotifyHandler::sendNotification(const std::string& strText, const std::string& strSuffix)
{
    if (instanceExists())
    {
        RlvBehaviourNotifyHandler* pThis = getInstance();

        // NOTE: notifications have two parts (which are concatenated without token) where only the first part is subject to the filter
        for (std::multimap<LLUUID, notifyData>::const_iterator itNotify = pThis->m_Notifications.begin();
                itNotify != pThis->m_Notifications.end(); ++itNotify)
        {
            if ( (itNotify->second.strFilter.empty()) || (boost::icontains(strText, itNotify->second.strFilter)) )
            {
                // Charis: per-object reply budget - of its own, so a flood of notifications (a restriction taken back over and over,
                //         which is not counted) can't use up what the same object needs to answer its queries
                static const LLUUID idNotifyBudget("7d6c4f0a-2b53-4f6e-9a3c-5e1b8d92c4a1");
                RlvUtil::sendChatReply(itNotify->second.nChannel, "/" + strText + strSuffix, true, itNotify->first ^ idNotifyBudget);
            }
        }
    }
}

// Checked: 2011-03-31 (RLVa-1.3.0f) | Added: RLVa-1.3.0f
void RlvBehaviourNotifyHandler::onWear(LLWearableType::EType eType, bool fAllowed)
{
    sendNotification(llformat("worn %s %s", (fAllowed) ? "legally" : "illegally", LLWearableType::getInstance()->getTypeName(eType).c_str()));
}

// Checked: 2011-03-31 (RLVa-1.3.0f) | Added: RLVa-1.3.0f
void RlvBehaviourNotifyHandler::onTakeOff(LLWearableType::EType eType, bool fAllowed)
{
    sendNotification(llformat("unworn %s %s", (fAllowed) ? "legally" : "illegally", LLWearableType::getInstance()->getTypeName(eType).c_str()));
}

// Checked: 2011-03-31 (RLVa-1.3.0f) | Added: RLVa-1.3.0f
void RlvBehaviourNotifyHandler::onAttach(const LLViewerJointAttachment* pAttachPt, bool fAllowed)
{
    sendNotification(llformat("attached %s %s", (fAllowed) ? "legally" : "illegally", pAttachPt->getName().c_str()));
}

// Checked: 2011-03-31 (RLVa-1.3.0f) | Added: RLVa-1.3.0f
void RlvBehaviourNotifyHandler::onDetach(const LLViewerJointAttachment* pAttachPt, bool fAllowed)
{
    sendNotification(llformat("detached %s %s", (fAllowed) ? "legally" : "illegally", pAttachPt->getName().c_str()));
}

// Checked: 2011-03-31 (RLVa-1.3.0f) | Added: RLVa-1.3.0f
void RlvBehaviourNotifyHandler::onReattach(const LLViewerJointAttachment* pAttachPt, bool fAllowed)
{
    sendNotification(llformat("reattached %s %s", (fAllowed) ? "legally" : "illegally", pAttachPt->getName().c_str()));
}

void RlvBehaviourNotifyHandler::onSit(const LLUUID& idObj, bool fAllowed)
{
    if (idObj.isNull())
        sendNotification(llformat("sat ground %s", (fAllowed) ? "legally" : "illegally"));
    else
        sendNotification(llformat("sat object %s %s", (fAllowed) ? "legally" : "illegally", idObj.asString().c_str()));
}

void RlvBehaviourNotifyHandler::onStand(const LLUUID& idObj, bool fAllowed)
{
    if (idObj.isNull())
        sendNotification(llformat("unsat ground %s", (fAllowed) ? "legally" : "illegally"));
    else
        sendNotification(llformat("unsat object %s %s", (fAllowed) ? "legally" : "illegally", idObj.asString().c_str()));
}

// =========================================================================
// Various helper classes/timers/functors
//

// Checked: 2010-03-13 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
bool RlvGCTimer::tick()
{
    bool fContinue = gRlvHandler.onGC();
    if (!fContinue)
        gRlvHandler.m_pGCTimer = NULL;
    return !fContinue;
}

// ============================================================================
// Various helper functions
//

// ============================================================================
// Attachment group helper functions
//

// Has to match the order of ERlvAttachGroupType
const std::string cstrAttachGroups[RLV_ATTACHGROUP_COUNT] = { "head", "torso", "arms", "legs", "hud" };

// Checked: 2009-10-19 (RLVa-1.1.0e) | Added: RLVa-1.1.0e
ERlvAttachGroupType rlvAttachGroupFromIndex(S32 idxGroup)
{
    switch (idxGroup)
    {
        case 0: // Right Hand
        case 1: // Right Arm
        case 3: // Left Arm
        case 4: // Left Hand
            return RLV_ATTACHGROUP_ARMS;
        case 2: // Head
            return RLV_ATTACHGROUP_HEAD;
        case 5: // Left Leg
        case 7: // Right Leg
            return RLV_ATTACHGROUP_LEGS;
        case 6: // Torso
            return RLV_ATTACHGROUP_TORSO;
        case 8: // HUD
            return RLV_ATTACHGROUP_HUD;
        default:
            return RLV_ATTACHGROUP_INVALID;
    }
}

// Checked: 2009-10-19 (RLVa-1.1.0e) | Added: RLVa-1.1.0e
ERlvAttachGroupType rlvAttachGroupFromString(const std::string& strGroup)
{
    for (int idx = 0; idx < RLV_ATTACHGROUP_COUNT; idx++)
        if (cstrAttachGroups[idx] == strGroup)
            return (ERlvAttachGroupType)idx;
    return RLV_ATTACHGROUP_INVALID;
}

// =========================================================================
// String helper functions
//

// Checked: 2009-07-29 (RLVa-1.0.1b) | Added: RLVa-1.0.1b
std::string rlvGetFirstParenthesisedText(const std::string& strText, std::string::size_type* pidxMatch /*=NULL*/)
{
    if (pidxMatch)
        *pidxMatch = std::string::npos; // Assume we won't find anything

    std::string::size_type idxIt, idxStart; int cntLevel = 1;
    if ((idxStart = strText.find_first_of('(')) == std::string::npos)
        return std::string();

    const char* pstrText = strText.c_str(); idxIt = idxStart;
    while ( (cntLevel > 0) && (++idxIt < strText.length()) )
    {
        if ('(' == pstrText[idxIt])
            cntLevel++;
        else if (')' == pstrText[idxIt])
            cntLevel--;
    }

    if (idxIt < strText.length())
    {
        if (pidxMatch)
            *pidxMatch = idxStart;  // Return the character index of the starting '('
        return strText.substr(idxStart + 1, idxIt - idxStart - 1);
    }
    return std::string();
}

// Checked: 2009-07-29 (RLVa-1.0.1b) | Added: RLVa-1.0.1b
std::string rlvGetLastParenthesisedText(const std::string& strText, std::string::size_type* pidxStart /*=NULL*/)
{
    if (pidxStart)
        *pidxStart = std::string::npos; // Assume we won't find anything

    // Extracts the last - matched - parenthesised text from the input string
    size_t idxIt; std::string::size_type idxEnd; int cntLevel = 1;
    if ((idxEnd = strText.find_last_of(')')) == std::string::npos)
        return std::string();

    const char* pstrText = strText.c_str(); idxIt = idxEnd;
    while ( (cntLevel > 0) && (--idxIt >= 0) && (idxIt < strText.length()) )
    {
        if (')' == pstrText[idxIt])
            cntLevel++;
        else if ('(' == pstrText[idxIt])
            cntLevel--;
    }

    if ( (idxIt >= 0) && (idxIt < strText.length()) )   // NOTE: allow for std::string::size_type to be signed or unsigned
    {
        if (pidxStart)
            *pidxStart = idxIt;     // Return the character index of the starting '('
        return strText.substr(idxIt + 1, idxEnd - idxIt - 1);
    }
    return std::string();
}

// =========================================================================
// Various helper functions
//

// Charis: Rlv::forceAtmosphericShadersIfAvailable(), Rlv::getObjectLinkNumber() and Rlv::getObjectRootId() were only
// used by @setenv / @setsphere (stage 3) and are not ported.

LLWearableType::EType rlvTypeNameToType(const std::string& strTypeName)
{
    const S32 nType = (S32)LLWearableType::getInstance()->typeNameToType(strTypeName);
    return ( (nType >= 0) && (nType < (S32)LLWearableType::WT_COUNT) ) ? (LLWearableType::EType)nType : LLWearableType::WT_INVALID;
}

bool rlvParseFloat(const std::string& strValue, float& nValue)
{
    try
    {
        const float nParsed = std::stof(strValue);
        if (!std::isfinite(nParsed))
            return false;
        nValue = nParsed;
        return true;
    }
    catch (const std::invalid_argument&)
    {
        return false;
    }
    catch (const std::out_of_range&)
    {
        return false;
    }
}

bool rlvIsFinite(const float* pValues, int nCount)
{
    for (int idx = 0; idx < nCount; idx++)
    {
        if (!std::isfinite(pValues[idx]))
            return false;
    }
    return true;
}

bool rlvIsFinite(const double* pValues, int nCount)
{
    for (int idx = 0; idx < nCount; idx++)
    {
        if (!std::isfinite(pValues[idx]))
            return false;
    }
    return true;
}

// =========================================================================
