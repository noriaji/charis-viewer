/**
 *
 * Copyright (c) 2009-2018, Kitty Barnett
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

// Charis (plus/rlv): ported from Firestorm's RLVa 2.4.2 rlvhandler.cpp. Stage 3 behaviours (camera, environment,
// debug settings, visual effects, @edit/@fly/@pay/@buy/@setgroup/@showinv/@showhovertext/@shownearby/...) are removed;
// attachment / outfit / folder locks and force wear/detach are in rlvlocks.* / rlvforcewear.cpp (rlv-locks), sitting
// to rlv-sit ("RLV-SIT:"). The Experience handlers moved to rlvexperience.cpp. See ORIGIN.md for the full list.

// Generic includes
#include "llviewerprecompiledheaders.h"
#include "llagent.h"
#include "llagentwearables.h"
#include "llappviewer.h"
#include "llavataractions.h"            // @stopim IM query
#include "llavatarnamecache.h"          // @shownames
#include "llcachename.h"                // @list IM query (group names)
#include "llcallbacklist.h"
#include "llexperiencecache.h"           // Experience lookups (temporary attachments)
#include "llexperiencelog.h"
#include "llinventorymodel.h"
#include "llcallingcard.h"              // @list IM query (friends only)
#include "llmutelist.h"                 // @list IM query (muted senders)
#include "llnotifications.h"            // @list IM query
#include "llnotificationsutil.h"
#include "llregionhandle.h"             // @tpto
#include "llslurl.h"
#include "llstartup.h"
#include "llviewercontrol.h"
#include "llviewerjointattachment.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llworldmapmessage.h"          // @tpto
#include "lldbstrings.h"                // MAX_MSG_STR_LEN

// RLVa includes
#include "rlvactions.h"
#include "rlvattachpt.h"
#include "rlvcommon.h"
#include "rlvforcewear.h"
#include "rlvlocks.h"
#include "rlvhandler.h"
#include "rlvhelper.h"
#include "rlvinventory.h"
#include "rlvmodifiers.h"
#include "rlvui.h"

// Boost includes
#include <boost/algorithm/string.hpp>

#include <cmath>
#include <sstream>

// llappviewer.cpp
extern bool gDoDisconnect;

// ============================================================================
// Charis: limits on what our own objects can make the viewer do (research/248 section 12). The limits only apply to
// adding restrictions (=n / =add), =force and replies; =y / =rem and @clear always go through and are not counted.
//

namespace
{
    constexpr size_t RLV_MAX_COMMANDS_PER_OBJECT     = 1000;    // Restrictions and exceptions held by one object
    constexpr size_t RLV_MAX_RESTRICTING_OBJECTS     = 256;     // Objects holding at least one restriction
    constexpr U32    RLV_MAX_COMMANDS_PER_SECOND     = 200;     // Add / force / reply commands per object per second
    constexpr size_t RLV_MAX_RETAINED_COMMANDS       = 2000;    // Add / force / reply retained while logging in (=y and @clear aren't kept)
    constexpr size_t RLV_MAX_NOTIFY_PER_OBJECT       = 16;      // @notify registrations of one object
    constexpr size_t RLV_MAX_NOTIFY_TOTAL            = 64;      // ... of all objects together
    constexpr F64    RLV_LIMIT_LOG_INTERVAL          = 60.0;    // Seconds between "limit reached" log lines per object
    constexpr F32    RLV_MAX_IM_DISTANCE             = 4096.f;  // @recvim/@sendim/@startim:<dist_min>[;<dist_max>]
    constexpr F64    RLV_MAX_TP_HEIGHT               = 4096.0;  // @tpto destination height
    constexpr F64    RLV_MAX_TP_GLOBAL               = 4294967040.0; // @tpto global x / y (the region handle range)

    // llOwnerSay lines of objects we don't know yet (see RlvHandler::checkCommandOrigin())
    constexpr F64    RLV_DEFER_TIMEOUT               = 10.0;    // Seconds a line waits for its object
    constexpr size_t RLV_MAX_DEFERRED_PER_OBJECT     = 32;      // Lines kept per object
    constexpr size_t RLV_MAX_DEFERRED_TOTAL          = 128;     // Lines kept in all
    constexpr F32    RLV_DEFER_RECHECK_INTERVAL      = 0.25f;   // Seconds between two looks at the waiting lines
    constexpr F64    RLV_EXPERIENCE_LOOKUP_TIMEOUT   = 30.0;    // Seconds a temporary attachment waits for an Experience lookup
    constexpr F64    RLV_IMQUERY_STALE_TIMEOUT       = 900.0;   // A @list / @except confirmation nobody answered is forgotten

    // RLVaLoginLastLocation is written (and the per-account settings file saved) at most this often
    constexpr F32    RLV_LOGIN_LOCATION_DELAY        = 5.f;

    // The confirmation before @list / @except is answered (notifications.xml)
    constexpr char   RLV_IMQUERY_LIST_NOTIFICATION[] = "RLVaListRequested";

    // Pruning of the per-object bookkeeping (only entries that are not active are dropped)
    constexpr size_t RLV_THROTTLE_PRUNE_SIZE         = 1024;
    constexpr size_t RLV_THROTTLE_HARD_SIZE          = 4096;

    // Everything except =y / =rem and @clear is limited (add, force, reply - and malformed commands)
    bool rlvIsLimitedCommand(ERlvParamType eType)
    {
        return (RLV_TYPE_REMOVE != eType) && (RLV_TYPE_CLEAR != eType);
    }

    // True if rlvCmdTakeBack (a =y / =rem or @clear[:<filter>] of the same object) takes back the not yet run rlvCmdAdd
    bool rlvTakesBack(const RlvCommand& rlvCmdTakeBack, const RlvCommand& rlvCmdAdd)
    {
        if ( (RLV_TYPE_ADD != rlvCmdAdd.getParamType()) || (rlvCmdAdd.getObjectID() != rlvCmdTakeBack.getObjectID()) )
            return false;
        if (RLV_TYPE_CLEAR == rlvCmdTakeBack.getParamType())
        {
            if (rlvCmdTakeBack.getParam().empty())
                return true;
            // The same text as rlvCmdAdd.asString() (it is an add), built in a buffer that is kept: @clear:<filter> goes over every
            // pending command of the object and must not allocate a string for each
            static std::string s_strCmdAdd;
            s_strCmdAdd.assign(rlvCmdAdd.getBehaviour());
            if (!rlvCmdAdd.getOption().empty())
                s_strCmdAdd.append(":").append(rlvCmdAdd.getOption());
            return (std::string::npos != s_strCmdAdd.find(rlvCmdTakeBack.getParam()));
        }
        return (RLV_TYPE_REMOVE == rlvCmdTakeBack.getParamType()) && (rlvCmdAdd.getBehaviour() == rlvCmdTakeBack.getBehaviour()) &&
               (rlvCmdAdd.getOption() == rlvCmdTakeBack.getOption()) && (rlvCmdAdd.isStrict() == rlvCmdTakeBack.isStrict());
    }
}

// ============================================================================
// Static variable initialization
//

bool RlvHandler::m_fEnabled = false;

rlv_handler_t gRlvHandler;

// ============================================================================
// Command specific helper functions
//

// Checked: 2009-08-04 (RLVa-1.0.1d) | Added: RLVa-1.0.1d
static bool rlvParseNotifyOption(const std::string& strOption, S32& nChannel, std::string& strFilter)
{
    boost_tokenizer tokens(strOption, boost::char_separator<char>(";", "", boost::keep_empty_tokens));
    boost_tokenizer::const_iterator itTok = tokens.begin();

    // Extract and sanity check the first token (required) which is the channel
    if ( (itTok == tokens.end()) || (!LLStringUtil::convertToS32(*itTok, nChannel)) || (!RlvUtil::isValidReplyChannel(nChannel)) )
        return false;

    // Second token (optional) is the filter
    strFilter.clear();
    if (++itTok != tokens.end())
    {
        strFilter = *itTok;
        ++itTok;
    }

    return (itTok == tokens.end());
}

// Checked: 2014-02-26 (RLVa-1.4.10)
static bool rlvParseGetStatusOption(const std::string& strOption, std::string& strFilter, std::string& strSeparator)
{
    // @getstatus:[<option>][;<separator>]
    //   * Parameters: first and second parameters are both optional
    //   * Examples  : @getstatus=123 ; @getstatus:tp=123 ; @getstatus:tp;|=123 ; @getstatus:;|=123

    boost_tokenizer tokens(strOption, boost::char_separator<char>(";", "", boost::keep_empty_tokens));
    boost_tokenizer::const_iterator itTok = tokens.begin();

    strSeparator = "/";
    strFilter.clear();

    // <option> optional parameter (defaults to empty string if unspecified)
    if (itTok != tokens.end())
        strFilter = *itTok++;
    else
        strFilter.clear();

    // <separator> optional paramter (defaults to '/' if unspecified)
    if ( (itTok != tokens.end()) && (!(*itTok).empty()) )
        strSeparator = *itTok++;
    else
        strSeparator = "/";

    return true;
}

// ============================================================================
// Constructor/destructor
//

// Checked: 2010-04-07 (RLVa-1.2.0d) | Modified: RLVa-1.0.1d
// Charis (rlv-locks): an RlvObject that is dropped must not leave locks behind (the =y of a command can fail, e.g. a folder that is gone)
static void rlvReleaseObjectLocks(const LLUUID& idRlvObj)
{
    gRlvAttachmentLocks.removeAllLocksOf(idRlvObj);
    gRlvWearableLocks.removeAllLocksOf(idRlvObj);
    RlvFolderLocks::instance().removeAllLocksOf(idRlvObj);
}

RlvHandler::RlvHandler() : m_fCanCancelTp(true), m_posSitSource(), m_pGCTimer(NULL)
{
    // Charis: no "new group" listener (@setgroup is stage 3)

    std::fill(std::begin(m_Behaviours), std::end(m_Behaviours), 0);
}

RlvHandler::~RlvHandler()
{
    cleanup();
}

void RlvHandler::cleanup()
{
    // Nothing to clean if we're not enabled (or already cleaned up)
    if (!m_fEnabled)
        return;

    // Charis: write a pending RLVaLoginLastLocation with the restrictions as they are now (the clears below must not change it)
    if (m_fLoginLocationPending)
    {
        m_fLoginLocationPending = false;
        RlvSettings::updateLoginLastLocation();
    }
    m_fInCleanup = true;

    //
    // Clean up any restrictions that are still active
    //
    // Charis: cleanup also runs on a disconnect before the viewer exits, and before STATE_STARTED (where processCommand(idObj, ...)
    //         would only retain the @clear), so clear through the reference overload and report what is left instead of asserting

    // Assume we have no way to predict how m_Objects will change so make a copy ahead of time
    uuid_vec_t idRlvObjects;
    idRlvObjects.reserve(m_Objects.size());
    std::transform(m_Objects.begin(), m_Objects.end(), std::back_inserter(idRlvObjects), [](const rlv_object_map_t::value_type& kvPair) {return kvPair.first; });
    for (const LLUUID & idRlvObj : idRlvObjects)
    {
        const RlvCommand rlvCmdClear(idRlvObj, "clear");
        processCommand(std::ref(rlvCmdClear), false);
    }

    const bool fLeftOver = (!m_Objects.empty()) || (!m_Exceptions.empty()) ||
        (std::any_of(std::begin(m_Behaviours), std::end(m_Behaviours), [](S32 cnt) { return 0 != cnt; }));
    if (fLeftOver)
    {
        RLV_WARNS << "Restrictions left after clearing every object (" << m_Objects.size() << " objects, " << m_Exceptions.size() << " exceptions); dropping them" << RLV_ENDL;
        for (const auto& entryObj : m_Objects)
            rlvReleaseObjectLocks(entryObj.first);
        m_Objects.clear();
        m_Exceptions.clear();
        std::fill(std::begin(m_Behaviours), std::end(m_Behaviours), 0);
    }

    //
    // Clean up what's left
    //
    m_Retained.clear();
    m_CommandThrottle.clear();
    m_DeferredLines.clear();
    m_PendingExperienceLookups.clear();
    m_PendingListQueries.clear();
    m_NotifyCounts.clear();
    m_nNotifyTotal = 0;
    //delete m_pGCTimer;    // <- deletes itself

    // Charis: let go of the signals explicitly (research/149 (E)); onIdleStartup() removes itself once RLVa is off
    m_ExperienceEventConn.disconnect();
    m_TeleportFailedConn.disconnect();
    m_TeleportFinishedConn.disconnect();

    for (RlvExtCommandHandler* pCmdHandler : m_CommandHandlers)
    {
        delete pCmdHandler;
    }
    m_CommandHandlers.clear();

    // Charis (rlv-locks): every restriction is gone, so the lock watchdog has nothing left to reattach; drop it and its timer
    if (RlvAttachmentLockWatchdog::instanceExists())
        RlvAttachmentLockWatchdog::deleteSingleton();

    m_fInCleanup = false;
    m_fEnabled = false;
}

// ============================================================================
// Behaviour related functions
//

bool RlvHandler::findBehaviour(ERlvBehaviour eBhvr, std::list<const RlvObject*>& lObjects) const
{
    lObjects.clear();
    for (const auto& objEntry : m_Objects)
        if (objEntry.second.hasBehaviour(eBhvr, false))
            lObjects.push_back(&objEntry.second);
    return !lObjects.empty();
}

bool RlvHandler::hasBehaviour(const LLUUID& idRlvObj, ERlvBehaviour eBhvr, const std::string& strOption) const
{
    rlv_object_map_t::const_iterator itObj = m_Objects.find(idRlvObj);
    if (m_Objects.end() != itObj)
        return itObj->second.hasBehaviour(eBhvr, strOption, false);
    return false;
}

bool RlvHandler::hasBehaviourExcept(ERlvBehaviour eBhvr, const std::string& strOption, const LLUUID& idObj) const
{
    for (rlv_object_map_t::const_iterator itObj = m_Objects.begin(); itObj != m_Objects.end(); ++itObj)
        if ( (idObj != itObj->second.getObjectID()) && (itObj->second.hasBehaviour(eBhvr, strOption, false)) )
            return true;
    return false;
}

// Checked: 2011-04-11 (RLVa-1.3.0h) | Added: RLVa-1.3.0h
bool RlvHandler::hasBehaviourRoot(const LLUUID& idObjRoot, ERlvBehaviour eBhvr, const std::string& strOption) const
{
    for (rlv_object_map_t::const_iterator itObj = m_Objects.begin(); itObj != m_Objects.end(); ++itObj)
        if ( (idObjRoot == itObj->second.getRootID()) && (itObj->second.hasBehaviour(eBhvr, strOption, false)) )
            return true;
    return false;
}

bool RlvHandler::ownsBehaviour(const LLUUID& idObj, ERlvBehaviour eBhvr) const
{
    bool fHasBhvr = false;
    for (const auto& objEntry : m_Objects)
    {
        if (objEntry.second.hasBehaviour(eBhvr, false))
        {
            if (objEntry.first != idObj)
                return false;
            fHasBhvr = true;
        }
    }
    return fHasBhvr;
}

// ============================================================================
// Behaviour exception handling
//

void RlvHandler::addException(const LLUUID& idObj, ERlvBehaviour eBhvr, const RlvExceptionOption& varOption)
{
    m_Exceptions.insert(std::make_pair(eBhvr, RlvException(idObj, eBhvr, varOption)));
}

bool RlvHandler::isException(ERlvBehaviour eBhvr, const RlvExceptionOption& varOption, ERlvExceptionCheck eCheckType) const
{
    // We need to "strict check" exceptions only if: the restriction is actually in place *and* (isPermissive(eBhvr) == false)
    if (ERlvExceptionCheck::Default == eCheckType)
        eCheckType = ( (hasBehaviour(eBhvr)) && (!isPermissive(eBhvr)) ) ? ERlvExceptionCheck::Strict : ERlvExceptionCheck::Permissive;

    uuid_vec_t objList;
    if (ERlvExceptionCheck::Strict == eCheckType)
    {
        // If we're "strict checking" then we need the UUID of every object that currently has 'eBhvr' restricted
        for (const auto& objEntry : m_Objects)
        {
            if (objEntry.second.hasBehaviour(eBhvr, !hasBehaviour(RLV_BHVR_PERMISSIVE)))
                objList.push_back(objEntry.first);
        }
    }

    for (rlv_exception_map_t::const_iterator itException = m_Exceptions.lower_bound(eBhvr), endException = m_Exceptions.upper_bound(eBhvr); itException != endException; ++itException)
    {
        if (itException->second.varOption == varOption)
        {
            // For permissive checks we just return on the very first match
            if (ERlvExceptionCheck::Permissive == eCheckType)
                return true;

            // For strict checks we don't return until the list is empty (every object with 'eBhvr' restricted also contains the exception)
            uuid_vec_t::iterator itList = std::find(objList.begin(), objList.end(), itException->second.idObject);
            if (itList != objList.end())
                objList.erase(itList);
            if (objList.empty())
                return true;
        }
    }
    return false;
}

bool RlvHandler::isPermissive(ERlvBehaviour eBhvr) const
{
    return (RlvBehaviourDictionary::instance().getHasStrict(eBhvr))
        ? !((hasBehaviour(RLV_BHVR_PERMISSIVE)) || (isException(RLV_BHVR_PERMISSIVE, eBhvr, ERlvExceptionCheck::Permissive)))
        : true;
}

void RlvHandler::removeException(const LLUUID& idObj, ERlvBehaviour eBhvr, const RlvExceptionOption& varOption)
{
    for (rlv_exception_map_t::iterator itException = m_Exceptions.lower_bound(eBhvr), endException = m_Exceptions.upper_bound(eBhvr); itException != endException; ++itException)
    {
        if ( (itException->second.idObject == idObj) && (itException->second.varOption == varOption) )
        {
            m_Exceptions.erase(itException);
            break;
        }
    }
}

// ============================================================================
// Blocked object handling
//

// Charis: how long a refused Experience's attachment waits for its object id (the region's AttachmentResources answer)
constexpr F64 RLV_BLOCKED_UNRESOLVED_TIMEOUT = 300.0;

void RlvHandler::addBlockedObject(const LLUUID& idObj, const std::string& strName)
{
    const F64 tmNow = LLTimer::getTotalSeconds();

    // Charis: an entry that never got its id (the cap failed) is dropped after 300 seconds even when no GC runs (it only runs
    //         while there are restrictions), so it can't keep lines of temporary attachments waiting for good
    m_BlockedObjects.erase(std::remove_if(m_BlockedObjects.begin(), m_BlockedObjects.end(),
        [tmNow](const rlv_blocked_object_t& entry) {
            return (std::get<0>(entry).isNull()) && (tmNow - std::get<2>(entry) >= RLV_BLOCKED_UNRESOLVED_TIMEOUT);
        }), m_BlockedObjects.end());

    m_BlockedObjects.push_back(std::make_tuple(idObj, strName, tmNow));
}

bool RlvHandler::hasUnresolvedBlockedObject() const
{
    const F64 tmNow = LLTimer::getTotalSeconds();
    return std::any_of(m_BlockedObjects.begin(), m_BlockedObjects.end(),
        [tmNow](const rlv_blocked_object_t& entry) { return (std::get<0>(entry).isNull()) && (tmNow - std::get<2>(entry) < RLV_BLOCKED_UNRESOLVED_TIMEOUT); });
}

// Charis: the object or the root of its linkset is blocked
bool RlvHandler::isBlockedObjectOrRoot(const LLUUID& idObj) const
{
    if (isBlockedObject(idObj))
        return true;
    const LLViewerObject* pObj = gObjectList.findObject(idObj);
    const LLViewerObject* pRoot = (pObj) ? pObj->getRootEdit() : nullptr;
    return (pRoot) && (pRoot != pObj) && (isBlockedObject(pRoot->getID()));
}

bool RlvHandler::isBlockedObject(const LLUUID& idObj) const
{
    return std::any_of(m_BlockedObjects.begin(), m_BlockedObjects.end(), [&idObj](const rlv_blocked_object_t& entry) { return std::get<0>(entry) == idObj; });
}

void RlvHandler::removeBlockedObject(const LLUUID& idObj)
{
    m_BlockedObjects.erase(std::remove_if(m_BlockedObjects.begin(), m_BlockedObjects.end(),
        [&idObj](const rlv_blocked_object_t& entry) {
            return (idObj.notNull()) ? std::get<0>(entry) == idObj : false;
        }), m_BlockedObjects.end());
}

// ============================================================================
// Command processing functions
//

// Checked: 2010-04-07 (RLVa-1.2.0d) | Modified: RLVa-1.1.0f
void RlvHandler::addCommandHandler(RlvExtCommandHandler* pCmdHandler)
{
    if ( (pCmdHandler) && (std::find(m_CommandHandlers.begin(), m_CommandHandlers.end(), pCmdHandler) == m_CommandHandlers.end()) )
        m_CommandHandlers.push_back(pCmdHandler);
}

// Checked: 2010-04-07 (RLVa-1.2.0d) | Modified: RLVa-1.1.0f
void RlvHandler::removeCommandHandler(RlvExtCommandHandler* pCmdHandler)
{
    if (pCmdHandler)
        m_CommandHandlers.remove(pCmdHandler);
}

// Checked: 2010-04-07 (RLVa-1.2.0d) | Modified: RLVa-1.1.0a
void RlvHandler::clearCommandHandlers()
{
    std::list<RlvExtCommandHandler*>::const_iterator itHandler = m_CommandHandlers.begin();
    while (itHandler != m_CommandHandlers.end())
    {
        delete *itHandler;
        ++itHandler;
    }
    m_CommandHandlers.clear();
}

// Checked: 2010-04-07 (RLVa-1.2.0d) | Modified: RLVa-1.1.0f
bool RlvHandler::notifyCommandHandlers(rlvExtCommandHandler f, const RlvCommand& rlvCmd, ERlvCmdRet& eRet, bool fNotifyAll) const
{
    std::list<RlvExtCommandHandler*>::const_iterator itHandler = m_CommandHandlers.begin(); bool fContinue = true; eRet = RLV_RET_UNKNOWN;
    while ( (itHandler != m_CommandHandlers.end()) && ((fContinue) || (fNotifyAll)) )
    {
        ERlvCmdRet eCmdRet = RLV_RET_UNKNOWN;
        if ((fContinue = !((*itHandler)->*f)(rlvCmd, eCmdRet)) == false)
            eRet = eCmdRet;
        ++itHandler;
    }
    RLV_ASSERT( (fContinue) || (eRet != RLV_RET_UNKNOWN) );
    return !fContinue;
}

// Checked: 2009-11-25 (RLVa-1.1.0f) | Modified: RLVa-1.1.0f
ERlvCmdRet RlvHandler::processCommand(std::reference_wrapper<const RlvCommand> rlvCmd, bool fFromObj)
{
    {
        const RlvCommand& rlvCmdTmp = rlvCmd; // Reference to the temporary with limited variable scope since we don't want it to leak below

        RLV_DEBUGS << "[" << rlvCmdTmp.getObjectID() << "]: " << rlvCmdTmp.asString() << RLV_ENDL;

        // Charis: a child prim of a blocked (temporary) attachment is blocked as well (review: decide by getRootEdit())
        if ( (!m_BlockedObjects.empty()) && (RLV_TYPE_REMOVE != rlvCmdTmp.getParamType()) && (RLV_TYPE_CLEAR != rlvCmdTmp.getParamType()) &&
             (isBlockedObjectOrRoot(rlvCmdTmp.getObjectID())) )
        {
            RLV_DEBUGS << "\t-> blocked object" << RLV_ENDL;
            return RLV_RET_FAILED_BLOCKED;
        }
        if (!rlvCmdTmp.isValid())
        {
            RLV_DEBUGS << "\t-> invalid syntax" << RLV_ENDL;
            return RLV_RET_FAILED_SYNTAX;
        }
        // Charis: behaviours turned off by the user (RLVaBlockedCommands, FS's RLV_RET_FAILED_DISABLED) - removing a restriction still goes through
        if ( (rlvCmdTmp.isBlocked()) && (RLV_TYPE_REMOVE != rlvCmdTmp.getParamType()) && (RLV_TYPE_CLEAR != rlvCmdTmp.getParamType()) )
        {
            RLV_DEBUGS << "\t-> blocked command" << RLV_ENDL;
            return RLV_RET_FAILED_DISABLED;
        }
        // Charis: limits on add / force / reply (research/248 section 12); =y / =rem and @clear are never counted, and
        //         neither are the retained commands when they are played back (they were counted when they arrived)
        if ( (fFromObj) && (!m_fReplayingRetained) && (rlvIsLimitedCommand(rlvCmdTmp.getParamType())) )
        {
            const ERlvCmdRet eLimitRet = checkCommandLimits(rlvCmdTmp);
            if (RLV_RET_SUCCESS != eLimitRet)
                return eLimitRet;
        }
    }

    // Using a stack for executing commands solves a few problems:
    //   - if we passed RlvObject::m_idObj for idObj somewhere and process a @clear then idObj points to invalid/cleared memory at the end
    //   - if command X triggers command Y along the way then getCurrentCommand()/getCurrentObject() still return Y even when finished
    m_CurCommandStack.push(rlvCmd); m_CurObjectStack.push(rlvCmd.get().getObjectID());
    const LLUUID& idCurObj = m_CurObjectStack.top();

    ERlvCmdRet eRet = RLV_RET_UNKNOWN;
    switch (rlvCmd.get().getParamType())
    {
        case RLV_TYPE_ADD:      // Checked: 2009-11-26 (RLVa-1.1.0f) | Modified: RLVa-1.1.0f
            {
                ERlvBehaviour eBhvr = rlvCmd.get().getBehaviourType();
                if(eBhvr == RLV_BHVR_UNKNOWN)
                {
                    eRet = RLV_RET_FAILED_PARAM;
                    RLV_DEBUGS << "\t- " << rlvCmd.get().getBehaviour() << " is UNKNOWN => Call Kitty!" << RLV_ENDL;
                    break;
                }

                // Charis: the single-holder check of @setcam / @setdebug / @setenv is stage 3

                // Charis: the caller's command; rlvCmd is pointed at the object's copy below, which removeCommand() frees again
                const std::reference_wrapper<const RlvCommand> rlvCmdIssued = rlvCmd;

                rlv_object_map_t::iterator itObj = m_Objects.find(idCurObj); bool fAdded = false;
                if (itObj != m_Objects.end())
                {
                    // Add the command to an existing object
                    rlvCmd = itObj->second.addCommand(rlvCmd, fAdded);
                }
                else
                {
                    // Create a new RLV object and then add the command to it (and grab its reference)
                    itObj = m_Objects.insert(std::pair<LLUUID, RlvObject>(idCurObj, RlvObject(idCurObj))).first;
                    rlvCmd = itObj->second.addCommand(rlvCmd, fAdded);
                }

                RLV_DEBUGS << "\t- " << ( (fAdded) ? "adding behaviour" : "skipping duplicate" ) << RLV_ENDL;

                // Charis: #2 restrictions and exceptions held by one object (counted after the duplicate check, so a repeated
                //         command still answers RLV_RET_SUCCESS_DUPLICATE)
                if ( (fAdded) && (fFromObj) && (!m_fReplayingRetained) && (itObj->second.m_Commands.size() > RLV_MAX_COMMANDS_PER_OBJECT) )
                {
                    logLimit(idCurObj, "an object holds 1000 restrictions");
                    itObj->second.removeCommand(RlvCommand(rlvCmd, RLV_TYPE_REMOVE));
                    rlvCmd = rlvCmdIssued;
                    eRet = RLV_RET_FAILED;
                    break;
                }

                if (fAdded) {   // If false then this was a duplicate, there's no need to handle those
                    if (!m_pGCTimer)
                        m_pGCTimer = new RlvGCTimer();
                    eRet = processAddRemCommand(rlvCmd);
                    if (!RLV_RET_SUCCEEDED(eRet))
                    {
                        RlvCommand rlvCmdRem(rlvCmd, RLV_TYPE_REMOVE);
                        itObj->second.removeCommand(rlvCmdRem);
                        rlvCmd = rlvCmdIssued;  // Charis: m_OnCommand() below must not see the freed copy
                        if (itObj->second.m_Commands.empty())
                        {
                            RLV_DEBUGS << "\t- command list empty => removing " << idCurObj << RLV_ENDL;
                            rlvReleaseObjectLocks(idCurObj);
                            m_Objects.erase(itObj);
                        }
                    }
//                  notifyBehaviourObservers(rlvCmd, !fFromObj);
                }
                else
                {
                    eRet = RLV_RET_SUCCESS_DUPLICATE;
                }
            }
            break;
        case RLV_TYPE_REMOVE:       // Checked: 2009-11-26 (RLVa-1.1.0f) | Modified: RLVa-1.1.0f
            {
                rlv_object_map_t::iterator itObj = m_Objects.find(idCurObj); bool fRemoved = false;
                if (itObj != m_Objects.end())
                    fRemoved = itObj->second.removeCommand(rlvCmd);

                RLV_DEBUGS << "\t- " << ( (fRemoved) ? "removing behaviour"
                                                     : "skipping remove (unset behaviour or unknown object)") << RLV_ENDL;

                if (fRemoved) { // Don't handle non-sensical removes
                    eRet = processAddRemCommand(rlvCmd);
//                  notifyBehaviourObservers(rlvCmd, !fFromObj);

                    if (0 == itObj->second.m_Commands.size())
                    {
                        RLV_DEBUGS << "\t- command list empty => removing " << idCurObj << RLV_ENDL;
                        RlvBehaviourDictionary::instance().clearModifiers(idCurObj);
                        rlvReleaseObjectLocks(idCurObj);
                        m_Objects.erase(itObj);
                    }
                }
                else
                {
                    eRet = RLV_RET_SUCCESS_UNSET;
                }
            }
            break;
        case RLV_TYPE_CLEAR:        // Checked: 2009-11-25 (RLVa-1.1.0f) | Modified: RLVa-1.1.0f
            eRet = processClearCommand(rlvCmd);
            break;
        case RLV_TYPE_FORCE:        // Checked: 2009-11-26 (RLVa-1.1.0f) | Modified: RLVa-1.1.0f
            eRet = processForceCommand(rlvCmd);
            break;
        case RLV_TYPE_REPLY:        // Checked: 2009-11-25 (RLVa-1.1.0f) | Modified: RLVa-1.1.0f
            eRet = processReplyCommand(rlvCmd);
            break;
        case RLV_TYPE_UNKNOWN:      // Checked: 2009-11-25 (RLVa-1.1.0f) | Modified: RLVa-1.1.0f
        default:
            eRet = RLV_RET_FAILED_PARAM;
            break;
    }
    RLV_ASSERT(RLV_RET_UNKNOWN != eRet);

    m_OnCommand(rlvCmd, eRet, !fFromObj);

    RLV_DEBUGS << "\t--> command " << ((eRet & RLV_RET_SUCCESS) ? "succeeded" : "failed") << RLV_ENDL;

    m_CurCommandStack.pop(); m_CurObjectStack.pop();
    return eRet;
}

// Checked: 2009-11-25 (RLVa-1.1.0f) | Modified: RLVa-1.1.0f
ERlvCmdRet RlvHandler::processCommand(const LLUUID& idObj, const std::string& strCommand, bool fFromObj)
{
    if (!isEnabled())
        return RLV_RET_FAILED_DISABLED;

    const RlvCommand rlvCmd(idObj, strCommand);
    if (STATE_STARTED != LLStartUp::getStartupState())
    {
        // Charis: =y / =rem and @clear aren't kept; they take back what this object asked for earlier in the login
        if ( (RLV_TYPE_REMOVE == rlvCmd.getParamType()) || (RLV_TYPE_CLEAR == rlvCmd.getParamType()) )
        {
            const bool fRemoved = removeRetainedCommands(rlvCmd);
            if (m_Objects.end() != m_Objects.find(idObj))
                return processCommand(std::ref(rlvCmd), fFromObj);
            return ( (fRemoved) || (RLV_TYPE_CLEAR == rlvCmd.getParamType()) ) ? RLV_RET_SUCCESS : RLV_RET_SUCCESS_UNSET;
        }
        // Charis: bound what is kept while logging in (research/248 section 12 #5)
        if (m_Retained.size() >= RLV_MAX_RETAINED_COMMANDS)
        {
            logLimit(idObj, "too many commands retained during login");
            return RLV_RET_FAILED;
        }
        m_Retained.push_back(rlvCmd);
        return RLV_RET_RETAINED;
    }
    return processCommand(std::ref(rlvCmd), fFromObj);
}

// Charis: drops the retained (not yet run) restrictions that a =y / =rem or @clear of the same object takes back
bool RlvHandler::removeRetainedCommands(const RlvCommand& rlvCmd)
{
    const size_t cntBefore = m_Retained.size();
    m_Retained.remove_if([&rlvCmd](const RlvCommand& rlvCmdRetained) { return rlvTakesBack(rlvCmd, rlvCmdRetained); });
    return m_Retained.size() != cntBefore;
}

// Checked: 2010-02-27 (RLVa-1.2.0a) | Modified: RLVa-1.1.0f
void RlvHandler::processRetainedCommands(ERlvBehaviour eBhvrFilter /*=RLV_BHVR_UNKNOWN*/, ERlvParamType eTypeFilter /*=RLV_TYPE_UNKNOWN*/)
{
    // Charis: the retained commands were counted against the limits when they arrived (F-M1)
    const bool fWasReplaying = m_fReplayingRetained;
    m_fReplayingRetained = true;

    rlv_command_list_t::iterator itCmd = m_Retained.begin(), itCurCmd;
    while (itCmd != m_Retained.end())
    {
        itCurCmd = itCmd++;  // Point the loop iterator ahead

        const RlvCommand& rlvCmd = *itCurCmd;
        if ( ((RLV_BHVR_UNKNOWN == eBhvrFilter) || (rlvCmd.getBehaviourType() == eBhvrFilter)) &&
             ((RLV_TYPE_UNKNOWN == eTypeFilter) || (rlvCmd.getParamType() == eTypeFilter)) )
        {
            processCommand(std::ref(rlvCmd), true);
            m_Retained.erase(itCurCmd);
        }
    }

    m_fReplayingRetained = fWasReplaying;

    // Charis: force-wear queued by the replayed commands is run now (the lines of onIdleStartup() and the login replay don't go through
    //         a command batch)
    onCommandBatchDone();
}

// Charis: per-object limits on add / force / reply (research/248 section 12 #2-#4)
ERlvCmdRet RlvHandler::checkCommandLimits(const RlvCommand& rlvCmd)
{
    const LLUUID& idObj = rlvCmd.getObjectID();
    const F64 tmNow = LLTimer::getTotalSeconds();

    // Keep the bookkeeping bounded
    if (m_CommandThrottle.size() >= RLV_THROTTLE_PRUNE_SIZE)
        pruneThrottles();

    // #4: commands per object per second
    RlvCommandThrottle& throttle = m_CommandThrottle[idObj];
    if (tmNow - throttle.tmSecondStart >= 1.0)
    {
        throttle.tmSecondStart = tmNow;
        throttle.nCommands = 0;
    }
    if (++throttle.nCommands > RLV_MAX_COMMANDS_PER_SECOND)
    {
        logLimit(idObj, "more than 200 commands in one second");
        return RLV_RET_FAILED_THROTTLED;
    }

    // (#2, the restrictions of one object, is counted in processCommand() after the duplicate check)
    if (RLV_TYPE_ADD == rlvCmd.getParamType())
    {
        if ( (m_Objects.end() == m_Objects.find(idObj)) && (m_Objects.size() >= RLV_MAX_RESTRICTING_OBJECTS) )
        {
            // #3: objects holding restrictions
            logLimit(idObj, "256 objects hold restrictions");
            return RLV_RET_FAILED;
        }
    }
    return RLV_RET_SUCCESS;
}

// Charis: one log line per object and minute at most (the command text itself is never logged here)
void RlvHandler::logLimit(const LLUUID& idObj, const char* pstrReason)
{
    const F64 tmNow = LLTimer::getTotalSeconds();
    RlvCommandThrottle& throttle = m_CommandThrottle[idObj];
    if (tmNow - throttle.tmLastLog < RLV_LIMIT_LOG_INTERVAL)
        return;
    throttle.tmLastLog = tmNow;
    RLV_WARNS << "Refusing commands from " << idObj << ": " << pstrReason << RLV_ENDL;
}

// Charis: drops the throttle entries that no longer limit anything (and everything if a flood of distinct ids filled them)
void RlvHandler::pruneThrottles()
{
    const F64 tmNow = LLTimer::getTotalSeconds();

    for (auto itEntry = m_CommandThrottle.begin(); itEntry != m_CommandThrottle.end(); )
    {
        const bool fIdle = (tmNow - itEntry->second.tmSecondStart >= RLV_LIMIT_LOG_INTERVAL) && (tmNow - itEntry->second.tmLastLog >= RLV_LIMIT_LOG_INTERVAL);
        itEntry = (fIdle) ? m_CommandThrottle.erase(itEntry) : std::next(itEntry);
    }
    if (m_CommandThrottle.size() >= RLV_THROTTLE_HARD_SIZE)
        m_CommandThrottle.clear();
}

// ============================================================================
// Charis: the origin of llOwnerSay command lines (rlvhooks_core.cpp handleOwnerSay(), review S-M1)
//
// Whether a line comes from a temporary attachment (RLVaEnableTemporaryAttachments, the refused Experiences) can only be
// told once the object is known as ours. FS decides on whatever has arrived, so a line that beats its object update
// slips through; here such lines wait (up to RLV_DEFER_TIMEOUT seconds) and are decided when the object is known.
//

RlvHandler::ERlvCmdOrigin RlvHandler::checkCommandOrigin(const LLUUID& idObj, const std::string& strObjName, const LLViewerObject* pChatter)
{
    const ERlvCmdOrigin eOrigin = decideCommandOrigin(idObj, strObjName, pChatter);
    if (hasDeferredLines(idObj))
    {
        // Keep the object's order: once it may run, its waiting lines go first (the caller then runs the new line);
        // while it may not, the caller adds the new line behind them
        if (ERlvCmdOrigin::Allowed == eOrigin)
            runDeferredLines(idObj);
        else if (ERlvCmdOrigin::Deferred != eOrigin)
            dropDeferredLines(idObj);
    }
    return eOrigin;
}

RlvHandler::ERlvCmdOrigin RlvHandler::decideCommandOrigin(const LLUUID& idObj, const std::string& strObjName, const LLViewerObject* pChatter)
{
    // Temporary attachments don't survive a login, so the lines retained during the login don't need to wait
    if (STATE_STARTED != LLStartUp::getStartupState())
        return ERlvCmdOrigin::Allowed;
    if ( (!pChatter) || (!pChatter->permYouOwner()) )
        return ERlvCmdOrigin::Deferred;

    // Attachment flags live on the root of the linkset (a child prim can say the commands)
    const LLViewerObject* pRoot = pChatter->getRootEdit();
    if (!pRoot)
        return ERlvCmdOrigin::Deferred;
    if ( (!pRoot->isAttachment()) || (!pRoot->isTempAttachment()) )
        return ERlvCmdOrigin::Allowed;

    if (!RlvSettings::getEnableTemporaryAttachments())
        return ERlvCmdOrigin::NotCommand;
    const LLUUID& idRoot = pRoot->getID();
    if ( (isBlockedObject(idObj)) || (isBlockedObject(idRoot)) )
        return ERlvCmdOrigin::Blocked;

    // An Experience refused by name (onExperienceAttach()) that the AttachmentResources answer hasn't resolved yet: the
    // temporary attachment with exactly that name is it. Block the whole linkset and take back what any of it set.
    auto itBlocked = std::find_if(m_BlockedObjects.begin(), m_BlockedObjects.end(),
        [&strObjName](const rlv_blocked_object_t& entry) { return (std::get<0>(entry).isNull()) && (std::get<1>(entry) == strObjName); });
    if (m_BlockedObjects.end() != itBlocked)
    {
        std::get<0>(*itBlocked) = idRoot;
        clearLinkset(idRoot, idObj);
        return ERlvCmdOrigin::Blocked;
    }

    // An Experience attach whose lookup hasn't answered yet, or a refused one whose object isn't resolved yet (the
    // AttachmentResources answer), may still turn out to be this one: wait for it
    if ( (hasPendingExperienceLookup()) || (hasUnresolvedBlockedObject()) )
        return ERlvCmdOrigin::Deferred;
    return ERlvCmdOrigin::Allowed;
}

// Charis: clears the restrictions of every object of the linkset idRoot (and of idObj, which may not be known as part of it)
void RlvHandler::clearLinkset(const LLUUID& idRoot, const LLUUID& idObj)
{
    uuid_vec_t idClear;
    for (const auto& objEntry : m_Objects)
    {
        if ( (objEntry.first == idObj) || (objEntry.first == idRoot) || ((objEntry.second.m_fLookup) && (objEntry.second.m_idRoot == idRoot)) )
            idClear.push_back(objEntry.first);
    }
    for (const LLUUID& idRlvObj : idClear)
    {
        const RlvCommand rlvCmdClear(idRlvObj, "clear");
        processCommand(std::ref(rlvCmdClear), false);
    }
    dropDeferredLines(idObj);
    dropDeferredLines(idRoot);
}

void RlvHandler::deferCommandLine(const LLUUID& idObj, const std::string& strObjName, const std::string& strCommands)
{
    if (!isEnabled())
        return;

    // =y / =rem and @clear never wait (taking a restriction back is always allowed); the rest waits, in order
    std::vector<RlvCommand> kept = runTakeBackCommands(idObj, strCommands);
    if (kept.empty())
        return;

    const size_t cntObject = std::count_if(m_DeferredLines.begin(), m_DeferredLines.end(), [&idObj](const RlvDeferredLine& line) { return line.idObject == idObj; });
    if ( (cntObject >= RLV_MAX_DEFERRED_PER_OBJECT) || (m_DeferredLines.size() >= RLV_MAX_DEFERRED_TOTAL) )
    {
        logLimit(idObj, "too many command lines waiting for their object");
        return;
    }
    m_DeferredLines.push_back(RlvDeferredLine{ idObj, strObjName, std::move(kept), LLTimer::getTotalSeconds() });

    if (!m_fDeferredTimer)
    {
        m_fDeferredTimer = true;
        doPeriodically([]() { return gRlvHandler.onDeferredLinesTimer(); }, RLV_DEFER_RECHECK_INTERVAL);
    }
}

// Runs the =y / =rem and @clear of a line now (they take back the matching adds that still wait - in earlier lines of the
// object and earlier in this line) and returns the line's other commands, parsed once, in order
std::vector<RlvCommand> RlvHandler::runTakeBackCommands(const LLUUID& idObj, const std::string& strCommands)
{
    std::vector<RlvCommand> kept;
    if (!isEnabled())
        return kept;

    bool fRan = false;
    boost_tokenizer tokens(strCommands, boost::char_separator<char>(",", "", boost::drop_empty_tokens));
    for (const std::string& strCmd : tokens)
    {
        RlvCommand rlvCmd(idObj, strCmd);
        if (rlvIsLimitedCommand(rlvCmd.getParamType()))
        {
            kept.push_back(std::move(rlvCmd));
            continue;
        }
        takeBackDeferredCommands(rlvCmd);
        kept.erase(std::remove_if(kept.begin(), kept.end(),
            [&rlvCmd](const RlvCommand& rlvCmdKept) { return rlvTakesBack(rlvCmd, rlvCmdKept); }), kept.end());
        processCommand(idObj, strCmd, true);
        fRan = true;
    }
    if (fRan)
        onCommandBatchDone();
    return kept;
}

bool RlvHandler::hasDeferredLines(const LLUUID& idObj) const
{
    return std::any_of(m_DeferredLines.begin(), m_DeferredLines.end(), [&idObj](const RlvDeferredLine& line) { return line.idObject == idObj; });
}

// Runs the waiting lines of one object, oldest first
void RlvHandler::runDeferredLines(const LLUUID& idObj)
{
    for (auto itLine = m_DeferredLines.begin(); itLine != m_DeferredLines.end(); )
    {
        if (itLine->idObject != idObj)
        {
            ++itLine;
            continue;
        }
        // Take the line out first: the commands can run anything (including cleanup())
        const RlvDeferredLine line = *itLine;
        itLine = m_DeferredLines.erase(itLine);
        // (only lines of STATE_STARTED wait, so nothing here needs retaining: the parsed commands run as they are)
        for (const RlvCommand& rlvCmd : line.commands)
            processCommand(std::ref(rlvCmd), true);
        onCommandBatchDone();
        if (!isEnabled())
            return;
    }
}

void RlvHandler::dropDeferredLines(const LLUUID& idObj)
{
    m_DeferredLines.remove_if([&idObj](const RlvDeferredLine& line) { return line.idObject == idObj; });
}

// A =y / =rem or @clear takes back the matching adds that are still waiting (lines that end up empty go)
void RlvHandler::takeBackDeferredCommands(const RlvCommand& rlvCmdTakeBack)
{
    for (auto itLine = m_DeferredLines.begin(); itLine != m_DeferredLines.end(); )
    {
        if (itLine->idObject == rlvCmdTakeBack.getObjectID())
        {
            std::vector<RlvCommand>& cmds = itLine->commands;
            cmds.erase(std::remove_if(cmds.begin(), cmds.end(),
                [&rlvCmdTakeBack](const RlvCommand& rlvCmd) { return rlvTakesBack(rlvCmdTakeBack, rlvCmd); }), cmds.end());
            if (cmds.empty())
            {
                itLine = m_DeferredLines.erase(itLine);
                continue;
            }
        }
        ++itLine;
    }
}

// Returns true once nothing waits any more (doPeriodically() then stops calling)
bool RlvHandler::onDeferredLinesTimer()
{
    if ( (!isEnabled()) || (m_fInCleanup) )
    {
        m_DeferredLines.clear();
        m_fDeferredTimer = false;
        return true;
    }

    const F64 tmNow = LLTimer::getTotalSeconds();
    for (auto itLine = m_DeferredLines.begin(); itLine != m_DeferredLines.end(); )
    {
        // The lines of one object are decided alike, so the list order keeps each object's order
        const LLUUID idObj = itLine->idObject;
        const ERlvCmdOrigin eOrigin = decideCommandOrigin(idObj, itLine->strObjName, gObjectList.findObject(idObj));
        if (ERlvCmdOrigin::Deferred == eOrigin)
        {
            if (tmNow - itLine->tmQueued < RLV_DEFER_TIMEOUT)
            {
                ++itLine;
                continue;
            }
            RLV_DEBUGS << "Dropping commands of " << idObj << ": the object never became known" << RLV_ENDL;
            itLine = m_DeferredLines.erase(itLine);
        }
        else if (ERlvCmdOrigin::Allowed == eOrigin)
        {
            runDeferredLines(idObj);
            if (!isEnabled())
                break;
            itLine = m_DeferredLines.begin();   // runDeferredLines() changed the list
        }
        else
        {
            dropDeferredLines(idObj);
            itLine = m_DeferredLines.begin();
        }
    }

    m_fDeferredTimer = !m_DeferredLines.empty();
    return !m_fDeferredTimer;
}

// ============================================================================
// Charis: Experience attaches whose lookup (LLExperienceCache, asynchronous) hasn't answered yet. Until it has, a temporary
//         attachment's commands wait (decideCommandOrigin()): the answer may put its name on the blocked list.
//

void RlvHandler::onExperienceEventTracked(const LLSD& sdEvent)
{
    onExperienceEvent(sdEvent);

    // Attach (4) whose experience isn't cached: onExperienceAttach() runs once the lookup answers
    if ( (isEnabled()) && (4 == sdEvent["Permission"].asInteger()) )
    {
        const LLUUID idExperience = sdEvent["public_id"].asUUID();
        if ( (idExperience.notNull()) && (!LLExperienceCache::instance().get(idExperience).has(LLExperienceCache::EXPERIENCE_ID)) )
            m_PendingExperienceLookups.push_back(std::make_pair(idExperience, LLTimer::getTotalSeconds()));
    }
}

bool RlvHandler::hasPendingExperienceLookup()
{
    if (m_PendingExperienceLookups.empty())
        return false;
    const F64 tmNow = LLTimer::getTotalSeconds();
    m_PendingExperienceLookups.erase(std::remove_if(m_PendingExperienceLookups.begin(), m_PendingExperienceLookups.end(),
        [tmNow](const std::pair<LLUUID, F64>& entry)
        {
            return (tmNow - entry.second >= RLV_EXPERIENCE_LOOKUP_TIMEOUT) || (LLExperienceCache::instance().get(entry.first).has(LLExperienceCache::EXPERIENCE_ID));
        }), m_PendingExperienceLookups.end());
    return !m_PendingExperienceLookups.empty();
}

// rlv-locks: RlvForceWear::done() after the commands of one line (set by plus::rlv::setCommandBatchDone())
RlvHandler::command_batch_done_fn RlvHandler::s_pfnCommandBatchDone = nullptr;

void RlvHandler::onCommandBatchDone()
{
    if (s_pfnCommandBatchDone)
        s_pfnCommandBatchDone();
}

// ============================================================================
// Charis: RLVaLoginLastLocation is updated a little later instead of at every sit / stand (review S-L8): each change
//         saves the per-account settings file, and @tploc / @unsit can flip many times in a row
//

void RlvHandler::scheduleLoginLastLocationUpdate()
{
    if ( (!isEnabled()) || (m_fInCleanup) || (m_fLoginLocationPending) )
        return;
    m_fLoginLocationPending = true;
    doAfterInterval([]()
        {
            if (!gRlvHandler.m_fLoginLocationPending)
                return;
            gRlvHandler.m_fLoginLocationPending = false;
            if ( (RlvHandler::isEnabled()) && (!gRlvHandler.m_fInCleanup) )
                RlvSettings::updateLoginLastLocation();
        }, RLV_LOGIN_LOCATION_DELAY);
}

ERlvCmdRet RlvHandler::processClearCommand(const RlvCommand& rlvCmd)
{
    const std::string& strFilter = rlvCmd.getParam(); std::string strCmdRem;

    rlv_object_map_t::const_iterator itObj = m_Objects.find(rlvCmd.getObjectID());
    if (itObj != m_Objects.end())   // No sense in clearing if we don't have any commands for this object
    {
        const RlvObject& rlvObj = itObj->second; bool fContinue = true;
        for (rlv_command_list_t::const_iterator itCmd = rlvObj.m_Commands.begin(), itCurCmd;
                ((fContinue) && (itCmd != rlvObj.m_Commands.end())); )
        {
            itCurCmd = itCmd++;     // Point itCmd ahead so it won't get invalidated if/when we erase a command

            const RlvCommand& rlvCmdRem = *itCurCmd; strCmdRem = rlvCmdRem.asString();
            if ( (strFilter.empty()) || (std::string::npos != strCmdRem.find(strFilter)) )
            {
                fContinue = (rlvObj.m_Commands.size() > 1); // rlvObj will become invalid once we remove the last command
                processCommand(rlvCmd.getObjectID(), strCmdRem.append("=y"), false);
            }
        }
    }

    // Let our observers know about clear commands
    ERlvCmdRet eRet = RLV_RET_SUCCESS;
    notifyCommandHandlers(&RlvExtCommandHandler::onClearCommand, rlvCmd, eRet, true);

    return RLV_RET_SUCCESS; // Don't fail clear commands even if the object didn't exist since it confuses people
}

// Called for a peer-to-peer IM (IM_NOTHING_SPECIAL) from an avatar - not a group, conference or object IM; the caller
// (llimprocessing.cpp, C1) passes fOnline = (offline == IM_ONLINE). The other conditions FS checks at the call site
// (online, not muted, "only friends" setting, '@' and at least 4 characters) are checked here.
bool RlvHandler::processIMQuery(const LLUUID& idSender, const std::string& strSenderName, const std::string& strMessage, bool fOnline)
{
    if ( (!isEnabled()) || (!fOnline) || (idSender.isNull()) || (idSender == gAgent.getID()) ||
         (strMessage.length() <= 3) || (RLV_CMD_PREFIX != strMessage[0]) )
    {
        return false;
    }
    // Muted by id or by name (FS llimprocessing.cpp)
    if (LLMuteList::getInstance()->isMuted(idSender, strSenderName, LLMute::flagTextChat))
        return false;
    static LLCachedControl<bool> s_fFriendsOnly(gSavedPerAccountSettings, "VoiceCallsFriendsOnly", false);
    if ( (s_fFriendsOnly) && (!LLAvatarTracker::instance().getBuddyInfo(idSender)) )
        return false;

    if ("@stopim" == strMessage)
    {
        // If the user can't start an IM session terminate it (if one is open) - always notify the sender in this case
        if (!RlvActions::canStartIM(idSender, true))
        {
            // Charis: sendBusyMessage() sends at most one automatic response per resident and minute (the session is still ended)
            RlvUtil::sendBusyMessage(idSender, RlvStrings::getString(RlvStringKeys::StopIm::EndSessionRemote));
            if (RlvActions::hasOpenP2PSession(idSender))
            {
                LLAvatarActions::endIM(idSender);
                RlvUtil::notifyBlocked(RlvStringKeys::StopIm::EndSessionLocal, LLSD().with("NAME", LLSLURL("agent", idSender, "about").getSLURLString()), true);
            }
            return true;
        }

        // User can start an IM session so we do nothing - notify and hide it from the user only if IM queries are enabled
        if (!RlvSettings::getEnableIMQuery())
            return false;
        RlvUtil::sendBusyMessage(idSender, RlvStrings::getString(RlvStringKeys::StopIm::NoSession));
        return true;
    }
    else if (RlvSettings::getEnableIMQuery())
    {
        if ("@version" == strMessage)
        {
            // An answer that was asked for (not an automatic response): not held back
            RlvUtil::sendBusyMessage(idSender, RlvStrings::getVersion(LLUUID::null), LLUUID::null, false);
            return true;
        }
        else if ( ("@list" == strMessage) || ("@except" == strMessage) )
        {
            // Charis: without the notification template LLNotifications falls back to a "MissingAlert" whose OK is option 0
            //         ("send"); never answer then
            if (!LLNotifications::instance().templateExists(RLV_IMQUERY_LIST_NOTIFICATION))
            {
                RLV_WARNS << "Notification " << RLV_IMQUERY_LIST_NOTIFICATION << " is missing; @list / @except refused" << RLV_ENDL;
                RlvUtil::sendBusyMessage(idSender, RlvStrings::getString("imquery_list_deny"), LLUUID::null, false);
                return true;
            }

            // Charis: "Always allow" (an ignored notification) answers without asking: at most one answer per resident every 10 seconds and
            //         30 a minute altogether, whatever the user chose (the requests above the limit are hidden and not answered)
            const F64 tmNow = LLTimer::getTotalSeconds();
            {
                static std::map<LLUUID, F64> s_LastListQuery;
                static F64 s_tmWindowStart = 0.0;
                static S32 s_nWindowCount = 0;
                if (tmNow - s_tmWindowStart >= 60.0)
                {
                    s_tmWindowStart = tmNow;
                    s_nWindowCount = 0;
                    for (auto itLast = s_LastListQuery.begin(); itLast != s_LastListQuery.end(); )
                        itLast = (tmNow - itLast->second >= 10.0) ? s_LastListQuery.erase(itLast) : std::next(itLast);
                }
                auto itLast = s_LastListQuery.find(idSender);
                if ( ((s_LastListQuery.end() != itLast) && (tmNow - itLast->second < 10.0)) || (s_nWindowCount >= 30) )
                    return true;
                s_nWindowCount++;
                s_LastListQuery[idSender] = tmNow;
            }
            auto itPending = m_PendingListQueries.find(idSender);
            if ( (m_PendingListQueries.end() != itPending) && (tmNow - itPending->second < RLV_IMQUERY_STALE_TIMEOUT) )
                return true;
            m_PendingListQueries[idSender] = tmNow;

            LLNotification::Params params;
            params.name = RLV_IMQUERY_LIST_NOTIFICATION;
            params.functor.function(boost::bind(&RlvHandler::onIMQueryListResponse, this, _1, _2));
            params.substitutions = LLSD().with("NAME_LABEL", LLSLURL("agent", idSender, "completename").getSLURLString()).with("NAME_SLURL", LLSLURL("agent", idSender, "about").getSLURLString());
            params.payload = LLSD().with("from_id", idSender).with("command", strMessage);

            class RlvPostponedOfferNotification : public LLPostponedNotification
            {
            protected:
                void modifyNotificationParams() override
                {
                    LLSD substitutions = mParams.substitutions;
                    substitutions["NAME"] = mName;
                    mParams.substitutions = substitutions;
                }
            };
            LLPostponedNotification::add<RlvPostponedOfferNotification>(params, idSender, false);
            return true;
        }
    }
    return false;
}

void RlvHandler::onIMQueryListResponse(const LLSD& sdNotification, const LLSD sdResponse)
{
    // The user may answer after RLVa was cleaned up (logout); only our own notification counts as an answer
    if ( (!isEnabled()) || (RLV_IMQUERY_LIST_NOTIFICATION != sdNotification["name"].asString()) )
        return;

    const LLUUID idRequester = sdNotification["payload"]["from_id"].asUUID();
    if (idRequester.isNull())
        return;
    m_PendingListQueries.erase(idRequester);

    const int idxOption = LLNotificationsUtil::getSelectedOption(sdNotification, sdResponse);
    if ( (idxOption == 0) || (idxOption == 1) )
    {
        if (idxOption == 1)
        {
            if (LLNotificationPtr pNotif = LLNotificationsUtil::find(sdNotification["id"]))
                pNotif->setIgnored(true);
        }

        // RlvUtil::sendIMMessage() cuts the reply at RLV_MAX_IMQUERY_LIST_MESSAGES IMs (research/248 section 12 #7)
        const std::string& strCommand = sdNotification["payload"]["command"].asStringRef();
        if ("@list" == strCommand)
        {
            RlvUtil::sendIMMessage(idRequester, getFormattedBehaviourString(ERlvBehaviourFilter::BEHAVIOURS_ONLY).append("\n").append(RlvStrings::getString("imquery_list_suffix")), '\n');
        }
        else if ("@except" == strCommand)
        {
            RlvUtil::sendIMMessage(idRequester, getFormattedBehaviourString(ERlvBehaviourFilter::EXCEPTIONS_ONLY), '\n');
        }
    }
    else
    {
        RlvUtil::sendBusyMessage(idRequester, RlvStrings::getString("imquery_list_deny"), LLUUID::null, false);
    }
}

// Charis: moved from FS rlvfloaters.cpp (rlvGetItemNameFromObjID(), rlvGetShowException() and
// RlvFloaterBehaviours::getFormattedBehaviourString()) since the RLV floaters are stage 3
namespace
{
    std::string rlvGetItemNameFromObjID(const LLUUID& idObj, bool fIncludeAttachPt = true)
    {
        const LLViewerObject* pObj = gObjectList.findObject(idObj);
        if ( (pObj) && (pObj->isAvatar()) )
        {
            LLAvatarName avName;
            if (LLAvatarNameCache::get(pObj->getID(), &avName))
                return avName.getCompleteName();
            return ((const LLVOAvatar*)pObj)->getFullname();
        }

        const LLViewerObject* pObjRoot = (pObj) ? pObj->getRootEdit() : NULL;
        const LLViewerInventoryItem* pItem = ((pObjRoot) && (pObjRoot->isAttachment())) ? gInventory.getItem(pObjRoot->getAttachmentItemID()) : NULL;

        const std::string strItemName = (pItem) ? pItem->getName() : idObj.asString();
        if ( (!fIncludeAttachPt) || (!pObj) || (!pObj->isAttachment()) || (!isAgentAvatarValid()) )
            return strItemName;

        const LLViewerJointAttachment* pAttachPt = get_if_there(gAgentAvatarp->mAttachmentPoints, RlvAttachPtLookup::getAttachPointIndex(pObjRoot), (LLViewerJointAttachment*)NULL);
        const std::string strAttachPtName = (pAttachPt) ? pAttachPt->getName() : std::string("Unknown");
        return llformat("%s (%s%s)", strItemName.c_str(), strAttachPtName.c_str(), (pObj == pObjRoot) ? "" : ", child");
    }

    bool rlvGetShowException(ERlvBehaviour eBhvr)
    {
        switch (eBhvr)
        {
            case RLV_BHVR_RECVCHAT:
            case RLV_BHVR_RECVEMOTE:
            case RLV_BHVR_SENDIM:
            case RLV_BHVR_RECVIM:
            case RLV_BHVR_STARTIM:
            case RLV_BHVR_TPLURE:
            case RLV_BHVR_TPREQUEST:
            case RLV_BHVR_ACCEPTTP:
            case RLV_BHVR_ACCEPTTPREQUEST:
            case RLV_BHVR_SHOWNAMES:
            case RLV_BHVR_SHOWNAMETAGS:
                return true;
            default:
                return false;
        }
    }
}

std::string RlvHandler::getFormattedBehaviourString(ERlvBehaviourFilter eFilter) const
{
    std::ostringstream strRestrictions;

    strRestrictions << RlvStrings::getVersion(LLUUID::null) << "\n";

    for (const auto& rlvObjectEntry : m_Objects)
    {
        strRestrictions << "\n" << rlvGetItemNameFromObjID(rlvObjectEntry.first) << ":\n";
        for (const RlvCommand& rlvCmd : rlvObjectEntry.second.getCommandList())
        {
            bool fIsException = (rlvCmd.hasOption()) && (rlvGetShowException(rlvCmd.getBehaviourType()));
            if ( ((ERlvBehaviourFilter::BEHAVIOURS_ONLY == eFilter) && (fIsException)) ||
                 ((ERlvBehaviourFilter::EXCEPTIONS_ONLY == eFilter) && (!fIsException)) )
            {
                continue;
            }

            std::string strOption; LLUUID idOption;
            if ( (rlvCmd.hasOption()) && (idOption.set(rlvCmd.getOption(), false)) && (idOption.notNull()) )
            {
                LLAvatarName avName;
                if (gObjectList.findObject(idOption))
                    strOption = rlvGetItemNameFromObjID(idOption, true);
                else if (LLAvatarNameCache::get(idOption, &avName))
                    strOption = (!avName.getAccountName().empty()) ? avName.getAccountName() : avName.getDisplayName();
                else if ( (!gCacheName) || (!gCacheName->getGroupName(idOption, strOption)) )
                    strOption = rlvCmd.getOption();
            }

            strRestrictions << "  -> " << rlvCmd.asString();
            if ((!strOption.empty()) && (strOption != rlvCmd.getOption()))
                strRestrictions << "  [" << strOption << "]";
            strRestrictions << "\n";
        }
    }

    return strRestrictions.str();
}

// ============================================================================
// Externally invoked event handlers
//

// Checked: 2010-08-29 (RLVa-1.2.1c) | Modified: RLVa-1.2.1c
void RlvHandler::onSitOrStand(bool fSitting)
{
    if (!isEnabled())
        return;

    scheduleLoginLastLocationUpdate();

    // RLV-SIT: @standtp - when standing up with a recorded m_posSitSource, doOnIdleOneTime(RlvUtil::forceTp(m_posSitSource))
    // RLV-SIT: @sitground=force - when standing up with m_fPendingGroundSit, sit down on the ground (AGENT_CONTROL_SIT_ON_GROUND)

    if (isAgentAvatarValid())
    {
        const LLViewerObject* pSitObj = static_cast<const LLViewerObject*>(gAgentAvatarp->getParent());
        const LLUUID& idSitObj = (pSitObj) ? pSitObj->getID() : LLUUID::null;
        if (fSitting)
        {
            RlvBehaviourNotifyHandler::instance().onSit(idSitObj, !gRlvHandler.hasBehaviourExcept(RLV_BHVR_SIT, m_idPendingSitActor));
            m_idPendingSitActor.setNull();
        }
        else
        {
            RlvBehaviourNotifyHandler::instance().onStand(idSitObj, !gRlvHandler.hasBehaviourExcept(RLV_BHVR_UNSIT, m_idPendingUnsitActor));
            m_idPendingUnsitActor.setNull();
        }
    }
}

// Checked: 2010-03-11 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
void RlvHandler::onAttach(const LLViewerObject* pAttachObj, const LLViewerJointAttachment* pAttachPt)
{
    // Assertion - pAttachObj is never NULL always specifies the root
    RLV_ASSERT( (pAttachObj) && (pAttachObj == pAttachObj->getRootEdit()) );
    // Sanity check - we need to be called *after* LLViewerJointAttachment::addObject()
    RLV_ASSERT( (pAttachPt) && (pAttachPt->isObjectAttached(pAttachObj)) );
    if ( (!isEnabled()) || (!pAttachObj) || (!pAttachPt) || (!pAttachPt->isObjectAttached(pAttachObj)) )
        return;

    // Check if we already have an RlvObject instance for this object or one of its child prims
    for (rlv_object_map_t::iterator itObj = m_Objects.begin(); itObj != m_Objects.end(); ++itObj)
    {
        // Only if we haven't been able to find this object (= attachment that rezzed in) or if it's a rezzed prim attached from in-world
        if ( (!itObj->second.m_fLookup) || (!itObj->second.m_idxAttachPt) )
        {
            const LLViewerObject* pObj = gObjectList.findObject(itObj->first);
            if ( (pObj) && (pObj->getRootEdit()->getID() == pAttachObj->getID()) )
            {
                // Reset any lookup information we might have for this object
                itObj->second.m_fLookup = true;
                itObj->second.m_idxAttachPt = RlvAttachPtLookup::getAttachPointIndex(pAttachObj);
                itObj->second.m_idRoot = pAttachObj->getID();

                // We need to check this object for an active "@detach=n" and actually lock it down now that it's been attached somewhere
                if (itObj->second.hasBehaviour(RLV_BHVR_DETACH, false)) gRlvAttachmentLocks.addAttachmentLock(pAttachObj->getID(), itObj->second.getObjectID());
            }
        }
    }

    // Fetch the inventory item if it isn't already (we need it in case of a reattach-on-detach) and rename it if appropriate
    if ( (STATE_STARTED == LLStartUp::getStartupState()) && (gInventory.isInventoryUsable()) )
    {
        RlvRenameOnWearObserver* pFetcher = new RlvRenameOnWearObserver(pAttachObj->getAttachmentItemID());
        pFetcher->startFetch();
        if (pFetcher->isFinished())
            pFetcher->done();
        else
            gInventory.addObserver(pFetcher);
    }
}

// Checked: 2010-03-11 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
void RlvHandler::onDetach(const LLViewerObject* pAttachObj, const LLViewerJointAttachment* pAttachPt)
{
    // Assertion - pAttachObj is never NULL always specifies the root
    RLV_ASSERT( (pAttachObj) && (pAttachObj == pAttachObj->getRootEdit()) );
    // Sanity check - we need to be called *before* LLViewerJointAttachment::removeObject()
    RLV_ASSERT( (pAttachPt) && (pAttachPt->isObjectAttached(pAttachObj)) );
    if ( (!isEnabled()) || (!pAttachObj) || (!pAttachPt) || (!pAttachPt->isObjectAttached(pAttachObj)) )
        return;

    // If the attachment is no longer attached then then the user "Drop"'ed this attachment somehow
    if (!pAttachObj->isAttachment())
    {
        // Check if we have any RlvObject instances for this object (or any of its child prims)
        for (rlv_object_map_t::iterator itObj = m_Objects.begin(); itObj != m_Objects.end(); ++itObj)
        {
            if ( (itObj->second.m_fLookup) && (itObj->second.m_idRoot == pAttachObj->getID()) )
            {
                // Clear the attachment point lookup since it's now an in-world prim
                itObj->second.m_idxAttachPt = 0;

                // If this object has an active "@detach=n" then we need to release the attachment lock since it's no longer attached
                if (itObj->second.hasBehaviour(RLV_BHVR_DETACH, false)) gRlvAttachmentLocks.removeAttachmentLock(pAttachObj->getID(), itObj->second.getObjectID());
            }
        }
    }
    else
    {
        // If it's still attached then we need to clean up any restrictions this object (or one of its child prims) may still have set
        rlv_object_map_t::iterator itObj = m_Objects.begin(), itCurObj;
        while (itObj != m_Objects.end())
        {
            itCurObj = itObj++; // @clear will invalidate our iterator so point it ahead now
#ifdef RLV_DEBUG
            bool itObj = true;
            RLV_ASSERT(itObj);  // Little hack to push itObj out of scope and prevent it from being accidentally used below
#endif // RLV_DEBUG

            // NOTE: ObjectKill seems to happen in reverse (child prims are killed before the root is) so we can't use gObjectList here
            if (itCurObj->second.m_idRoot == pAttachObj->getID())
            {
                RLV_INFOS << "Clearing " << itCurObj->first.asString() << ":" << RLV_ENDL;
                processCommand(itCurObj->second.getObjectID(), "clear", true);
                RLV_INFOS << "\t-> done" << RLV_ENDL;
            }
        }

        if (pAttachObj->isTempAttachment())
        {
            removeBlockedObject(pAttachObj->getID());
        }
    }
}

// onExperienceAttach(), onExperienceEvent() and getAttachmentResourcesCoro() live in rlvexperience.cpp

// Checked: 2010-03-13 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
bool RlvHandler::onGC()
{
    rlv_object_map_t::iterator itObj = m_Objects.begin(), itCurObj;
    while (itObj != m_Objects.end())
    {
        itCurObj = itObj++; // @clear will invalidate our iterator so point it ahead now
#ifdef RLV_DEBUG
        bool itObj = true;
        RLV_ASSERT(itObj);
#endif // RLV_DEBUG

        // Temporary sanity check
        RLV_ASSERT(itCurObj->first == itCurObj->second.getObjectID());

        const LLViewerObject* pObj = gObjectList.findObject(itCurObj->second.getObjectID());
        if (!pObj)
        {
            // If the RlvObject once existed in gObjectList and now doesn't then expire it right away
            // If the RlvObject never existed in gObjectList and still doesn't then increase its "lookup misses" counter
            // but if that reaches 20 (we run every 30 seconds so that's about 10 minutes) then we'll expire it too
            if ( (itCurObj->second.m_fLookup) || (++itCurObj->second.m_nLookupMisses > 20) )
            {
                RLV_INFOS << "Garbage collecting " << itCurObj->first.asString() << ":" << RLV_ENDL;
                processCommand(itCurObj->first, "clear", true);
                RLV_INFOS << "\t-> done" << RLV_ENDL;
            }
        }
        else
        {
            // Charis: FS asserts that an object first seen here isn't an attachment (onAttach() would have caught it), but the
            //         onAttach() hook (plus::rlv::onAttachObject()) only runs once the attachment is on its point and an attachment can rez in before its first command - treat it the
            //         same way onAttach() does
            if (!itCurObj->second.m_fLookup)
            {
                RLV_INFOS << "Resolved missing object " << itCurObj->first.asString() << RLV_ENDL;
                itCurObj->second.m_fLookup = true;
                itCurObj->second.m_idxAttachPt = RlvAttachPtLookup::getAttachPointIndex(pObj);
                itCurObj->second.m_idRoot = pObj->getRootEdit()->getID();

                // NOTE: the following code should NEVER run (see assertion above), but just to be double-triple safety sure
                //  -> if it does run it likely means that there's a @detach=n in a *child* prim that we couldn't look up in onAttach()
                //  -> since RLV doesn't currently support @detach=n from child prims it's actually not such a big deal right now but still
                if ( (pObj->isAttachment()) && (itCurObj->second.hasBehaviour(RLV_BHVR_DETACH, false)) ) gRlvAttachmentLocks.addAttachmentLock(pObj->getRootEdit()->getID(), itCurObj->second.getObjectID());
            }
        }
    }

    // Verify that we haven't leaked any attachment locks somehow (the check also cleans up, so it runs in every build)
    const bool fLocksVerified = gRlvAttachmentLocks.verifyAttachmentLocks();
    RLV_ASSERT(fLocksVerified);
    (void)fLocksVerified;

    // Charis: drop the command throttle entries of objects that went quiet (see checkCommandLimits())
    pruneThrottles();

    // Clean up pending temp attachments that we were never able to resolve
    rlv_blocked_object_list_t::const_iterator itBlocked = m_BlockedObjects.cbegin(), itCurBlocked;
    while (itBlocked != m_BlockedObjects.end())
    {
        itCurBlocked = itBlocked++;
#ifdef RLV_DEBUG
        bool itBlocked = true;
        RLV_ASSERT(itBlocked);
#endif // RLV_DEBUG

        const LLUUID& idObj = std::get<0>(*itCurBlocked);
        if ( (idObj.notNull()) || (LLTimer::getTotalSeconds() - std::get<2>(*itCurBlocked) < 300.f) )
            continue;

        m_BlockedObjects.erase(itCurBlocked);
    }

    return (0 != m_Objects.size()); // GC will kill itself if it has nothing to do
}

// static
void RlvHandler::cleanupClass()
{
    gRlvHandler.cleanup();
}

// Checked: 2009-11-26 (RLVa-1.1.0f) | Added: RLVa-1.1.0f
void RlvHandler::onIdleStartup(void* pParam)
{
    LLTimer* pTimer = (LLTimer*)pParam;
    if ( (isEnabled()) && (LLStartUp::getStartupState() < STATE_STARTED) )
    {
        // We don't want to run this *too* often
        if ( (LLStartUp::getStartupState() >= STATE_MISC) && (pTimer->getElapsedTimeF32() >= 2.0) )
        {
            RlvHandler::instance().processRetainedCommands(RLV_BHVR_VERSION, RLV_TYPE_REPLY);
            RlvHandler::instance().processRetainedCommands(RLV_BHVR_VERSIONNEW, RLV_TYPE_REPLY);
            RlvHandler::instance().processRetainedCommands(RLV_BHVR_VERSIONNUM, RLV_TYPE_REPLY);
            pTimer->reset();
        }
    }
    else
    {
        // Clean-up
        gIdleCallbacks.deleteFunction(onIdleStartup, pParam);
        delete pTimer;
    }
}

// Checked: 2010-03-09 (RLVa-1.2.0a) | Added: RLVa-1.2.0a
void RlvHandler::onLoginComplete()
{
    if (!isEnabled())
        return;

    RlvInventory::instance().fetchWornItems();
    RlvInventory::instance().fetchSharedInventory();
    RlvSettings::updateLoginLastLocation();

    m_ExperienceEventConn = LLExperienceLog::instance().addUpdateSignal(boost::bind(&RlvHandler::onExperienceEventTracked, this, _1));
    m_TeleportFailedConn = LLViewerParcelMgr::getInstance()->setTeleportFailedCallback(boost::bind(&RlvHandler::onTeleportFailed, this));
    m_TeleportFinishedConn = LLViewerParcelMgr::getInstance()->setTeleportFinishedCallback(boost::bind(&RlvHandler::onTeleportFinished, this, _1));

    processRetainedCommands();
}

void RlvHandler::onTeleportCallback(U64 hRegion, const LLVector3& posRegion, const LLVector3& vecLookAt, const LLUUID& idRlvObj)
{
    // The map reply can arrive after RLVa was cleaned up (logout)
    if ( (hRegion) && (isEnabled()) )
    {
        m_CurObjectStack.push(idRlvObj);

        const LLVector3d posGlobal = from_region_handle(hRegion) + (LLVector3d)posRegion;
        if (vecLookAt.isExactlyZero())
            gAgent.teleportViaLocation(posGlobal);
        else
            gAgent.teleportViaLocationLookAt(posGlobal, vecLookAt);

        m_CurObjectStack.pop();
    }
}

// Checked: 2010-04-05 (RLVa-1.2.0d) | Added: RLVa-1.2.0d
void RlvHandler::onTeleportFailed()
{
    setCanCancelTp(true);
}

// Checked: 2010-04-05 (RLVa-1.2.0d) | Added: RLVa-1.2.0d
void RlvHandler::onTeleportFinished(const LLVector3d& posArrival)
{
    setCanCancelTp(true);
}

// ============================================================================
// String/chat censoring functions
//

static size_t utf8str_strlen(const std::string& utf8)
{
    const char* pUTF8 = utf8.c_str(); size_t length = 0;
    for (size_t idx = 0, cnt = utf8.length(); idx < cnt ;idx++)
    {
        // We're looking for characters that don't start with 10 as their high bits
        if ((pUTF8[idx] & 0xC0) != 0x80)
            length++;
    }
    return length;
}

static std::string utf8str_chtruncate(const std::string& utf8, size_t length)
{
    if (0 == length)
        return std::string();
    if (utf8.length() <= length)
        return utf8;

    const char* pUTF8 = utf8.c_str(); int idx = 0;
    while ( (pUTF8[idx]) && (length > 0) )
    {
        // We're looking for characters that don't start with 10 as their high bits
        if ((pUTF8[idx] & 0xC0) != 0x80)
            length--;
        idx++;
    }

    return utf8.substr(0, idx);
}

// Checked: 2010-03-26 (RLVa-1.2.0b) | Modified: RLVa-1.0.0f
bool RlvHandler::filterChat(std::string& strUTF8Text, bool fFilterEmote) const
{
    if (strUTF8Text.empty())
        return false;

    bool fFilter = false;
    if (RlvUtil::isEmote(strUTF8Text))              // Check if it's an emote
    {
        if (fFilterEmote)                           // Emote filtering depends on fFilterEmote
        {
            if ( (strUTF8Text.find_first_of("\"()*=^_?~") != std::string::npos) ||
                 (strUTF8Text.find(" -") != std::string::npos) || (strUTF8Text.find("- ") != std::string::npos) ||
                 (strUTF8Text.find("''") != std::string::npos) )
            {
                fFilter = true;                     // Emote contains illegal character (or character sequence)
            }
            else if (!hasBehaviour(RLV_BHVR_EMOTE))
            {
                auto idx = strUTF8Text.find('.');    // Truncate at 20 characters or at the dot (whichever is shorter)
                strUTF8Text = utf8str_chtruncate(strUTF8Text, ( (idx > 0) && (idx < 20) ) ? idx + 1 : 20);
            }
        }
    }
    else if (strUTF8Text[0] == '/')                 // Not an emote, but starts with a '/'
    {
        fFilter = (utf8str_strlen(strUTF8Text) > 7);// Allow as long if it's 6 characters or less
    }
    else if ( (!RlvSettings::getCanOOC()) ||
              (strUTF8Text.length() < 4) || (strUTF8Text.compare(0, 2, "((")) || (strUTF8Text.compare(strUTF8Text.length() - 2, 2, "))")) )
    {
        fFilter = true;                             // Regular chat (not OOC)
    }

    if (fFilter)
        strUTF8Text = (RlvSettings::getShowEllipsis()) ? "..." : "";
    return fFilter;
}

// Checked: 2010-11-29 (RLVa-1.3.0c) | Added: RLVa-1.3.0c
bool RlvHandler::hasException(ERlvBehaviour eBhvr) const
{
    return (m_Exceptions.find(eBhvr) != m_Exceptions.end());
}

// Checked: 2010-02-27 (RLVa-1.2.0b) | Modified: RLVa-1.2.0a
bool RlvHandler::redirectChatOrEmote(const std::string& strUTF8Text) const
{
    // Sanity check - @redirchat only for chat and @rediremote only for emotes
    ERlvBehaviour eBhvr = (!RlvUtil::isEmote(strUTF8Text)) ? RLV_BHVR_REDIRCHAT : RLV_BHVR_REDIREMOTE;
    if ( (strUTF8Text.empty()) || (!hasBehaviour(eBhvr)) )
        return false;

    if (RLV_BHVR_REDIRCHAT == eBhvr)
    {
        std::string strText = strUTF8Text;
        if (!filterChat(strText, false))
            return false;   // @sendchat wouldn't filter it so @redirchat won't redirect it either
    }

    for (rlv_exception_map_t::const_iterator itRedir = m_Exceptions.lower_bound(eBhvr),
            endRedir = m_Exceptions.upper_bound(eBhvr); itRedir != endRedir; ++itRedir)
    {
        S32 nChannel = boost::get<S32>(itRedir->second.varOption);
        if (RlvActions::canSendChannel(nChannel))
        {
            if (!RlvSettings::getSplitRedirectChat())
                RlvUtil::sendChatReply(nChannel, strUTF8Text);
            else
                RlvUtil::sendChatReplySplit(nChannel, strUTF8Text);
        }
    }

    return true;
}

// ============================================================================
// Initialization helper functions
//

bool RlvHandler::canEnable()
{
    return LLStartUp::getStartupState() <= STATE_LOGIN_CLEANUP;
}

bool RlvHandler::setEnabled(bool fEnable)
{
    if (m_fEnabled == fEnable)
        return fEnable;

    if ( (fEnable) && (canEnable()) )
    {
        RLV_INFOS << "Enabling Restrained Love API support - " << RlvStrings::getVersionAbout() << RLV_ENDL;
        m_fEnabled = true;

        // Initialize static classes
        RlvSettings::initClass();
        RlvStrings::initClass();

        // Charis: no RlvEnvironment / RlvExtGetSet command handlers (@setenv, @setdebug and @getdebug are stage 3)

        // Make sure we get notified when login is successful
        // (onIdleStartup() is registered at STATE_MISC by plus::rlv::onStartupState(), as FS does in llstartup.cpp)
        if (LLStartUp::getStartupState() < STATE_STARTED)
            LLAppViewer::instance()->setOnLoginCompletedCallback(boost::bind(&RlvHandler::onLoginComplete, RlvHandler::getInstance()));
        else
            RlvHandler::instance().onLoginComplete();

        // Set up RlvUIEnabler
        RlvUIEnabler::getInstance();

        // Reset to show assertions if the viewer version changed
        if (gSavedSettings.getString("LastRunVersion") != gLastRunVersion)
            gSavedSettings.set<bool>(std::string(RlvSettingNames::ShowAssertionFail), true);
    }

    return m_fEnabled;
}

// ============================================================================
// Command handlers (RLV_TYPE_ADD and RLV_TYPE_REMOVE)
//

#define VERIFY_OPTION(x)        { if (!(x)) { eRet = RLV_RET_FAILED_OPTION; break; } }
#define VERIFY_OPTION_REF(x)    { if (!(x)) { eRet = RLV_RET_FAILED_OPTION; break; } fRefCount = true; }

// Checked: 2010-03-03 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
ERlvCmdRet RlvHandler::processAddRemCommand(const RlvCommand& rlvCmd)
{
    // NOTE: - at this point the command has already been:
    //            * added to the RlvObject
    //            * removed from the RlvObject (which still exists at this point even if this is the last restriction)
    //       - the object's UUID may or may not exist in gObjectList (see handling of @detach=n|y)

    ERlvBehaviour eBhvr = rlvCmd.getBehaviourType(); ERlvParamType eType = rlvCmd.getParamType();

    // Try a command processor first
    ERlvCmdRet eRet = rlvCmd.processCommand();
    if (RLV_RET_NO_PROCESSOR != eRet)
    {
        return eRet;
    }

    // Process the command the legacy way

    eRet = RLV_RET_SUCCESS; bool fRefCount = false; const std::string& strOption = rlvCmd.getOption();
    switch (eBhvr)
    {
        case RLV_BHVR_ATTACHTHIS:           // @attachthis[:<option>]=n|y
        case RLV_BHVR_DETACHTHIS:           // @detachthis[:<option>]=n|y
            eRet = onAddRemFolderLock(rlvCmd, fRefCount);
            break;
        case RLV_BHVR_ATTACHTHISEXCEPT:     // @attachthisexcept[:<option>]=n|y
        case RLV_BHVR_DETACHTHISEXCEPT:     // @detachthisexcept[:<option>]=n|y
            eRet = onAddRemFolderLockException(rlvCmd, fRefCount);
            break;
        case RLV_BHVR_ADDOUTFIT:            // @addoutfit[:<layer>]=n|y         - Checked: 2010-08-29 (RLVa-1.2.1c) | Modified: RLVa-1.2.1c
        case RLV_BHVR_REMOUTFIT:            // @remoutfit[:<layer>]=n|y         - Checked: 2010-08-29 (RLVa-1.2.1c) | Modified: RLVa-1.2.1c
            {
                // If there's an option it should specify a wearable type name (reference count on no option *and* a valid option)
                RlvCommandOptionGeneric rlvCmdOption;
                RlvCommandOptionHelper::parseOption(rlvCmd.getOption(), rlvCmdOption);
                VERIFY_OPTION_REF( (rlvCmdOption.isEmpty()) || (rlvCmdOption.isWearableType()) );

                // We need to flush any queued force-wear commands before changing the restrictions
                if (RlvForceWear::instanceExists())
                    RlvForceWear::instance().done();

                ERlvLockMask eLock = (RLV_BHVR_ADDOUTFIT == eBhvr) ? RLV_LOCK_ADD : RLV_LOCK_REMOVE;
                for (int idxType = 0; idxType < LLWearableType::WT_COUNT; idxType++)
                {
                    if ( (rlvCmdOption.isEmpty()) || ((LLWearableType::EType)idxType == rlvCmdOption.getWearableType()) )
                    {
                        if (RLV_TYPE_ADD == eType)
                            gRlvWearableLocks.addWearableTypeLock((LLWearableType::EType)idxType, rlvCmd.getObjectID(), eLock);
                        else
                            gRlvWearableLocks.removeWearableTypeLock((LLWearableType::EType)idxType, rlvCmd.getObjectID(), eLock);
                    }
                }
            }
            break;
        case RLV_BHVR_SHAREDWEAR:           // @sharedwear=n|y                  - Checked: 2011-03-28 (RLVa-1.3.0g) | Added: RLVa-1.3.0g
        case RLV_BHVR_SHAREDUNWEAR:         // @sharedunwear=n|y                - Checked: 2011-03-28 (RLVa-1.3.0g) | Added: RLVa-1.3.0g
            {
                VERIFY_OPTION_REF(strOption.empty());

                RlvFolderLocks::folderlock_source_t lockSource(RlvFolderLocks::ST_SHAREDPATH, LLStringUtil::null);
                RlvFolderLocks::ELockScope eLockScope = RlvFolderLocks::SCOPE_SUBTREE;
                ERlvLockMask eLockType = (RLV_BHVR_SHAREDUNWEAR == eBhvr) ? RLV_LOCK_REMOVE : RLV_LOCK_ADD;

                if (RLV_TYPE_ADD == eType)
                    RlvFolderLocks::instance().addFolderLock(lockSource, RlvFolderLocks::PERM_DENY, eLockScope, rlvCmd.getObjectID(), eLockType);
                else
                    RlvFolderLocks::instance().removeFolderLock(lockSource, RlvFolderLocks::PERM_DENY, eLockScope, rlvCmd.getObjectID(), eLockType);
            }
            break;
        case RLV_BHVR_UNSHAREDWEAR:         // @unsharedwear=n|y                - Checked: 2011-03-28 (RLVa-1.3.0g) | Added: RLVa-1.3.0g
        case RLV_BHVR_UNSHAREDUNWEAR:       // @unsharedunwear=n|y              - Checked: 2011-03-28 (RLVa-1.3.0g) | Added: RLVa-1.3.0g
            {
                VERIFY_OPTION_REF(strOption.empty());

                // Lock down the inventory root
                RlvFolderLocks::folderlock_source_t lockSource(RlvFolderLocks::ST_ROOTFOLDER, 0);
                RlvFolderLocks::ELockScope eLockScope = RlvFolderLocks::SCOPE_SUBTREE;
                ERlvLockMask eLockType = (RLV_BHVR_UNSHAREDUNWEAR == eBhvr) ? RLV_LOCK_REMOVE : RLV_LOCK_ADD;

                if (RLV_TYPE_ADD == eType)
                    RlvFolderLocks::instance().addFolderLock(lockSource, RlvFolderLocks::PERM_DENY, eLockScope, rlvCmd.getObjectID(), eLockType);
                else
                    RlvFolderLocks::instance().removeFolderLock(lockSource, RlvFolderLocks::PERM_DENY, eLockScope, rlvCmd.getObjectID(), eLockType);

                // Add the #RLV shared folder as an exception
                lockSource = RlvFolderLocks::folderlock_source_t(RlvFolderLocks::ST_SHAREDPATH, LLStringUtil::null);
                if (RLV_TYPE_ADD == eType)
                    RlvFolderLocks::instance().addFolderLock(lockSource, RlvFolderLocks::PERM_ALLOW, eLockScope, rlvCmd.getObjectID(), eLockType);
                else
                    RlvFolderLocks::instance().removeFolderLock(lockSource, RlvFolderLocks::PERM_ALLOW, eLockScope, rlvCmd.getObjectID(), eLockType);
            }
            break;
        case RLV_BHVR_REDIRCHAT:            // @redirchat:<channel>=n|y         - Checked: 2010-03-26 (RLVa-1.2.0b) | Modified: RLVa-1.1.0h
        case RLV_BHVR_REDIREMOTE:           // @rediremote:<channel>=n|y        - Checked: 2010-03-26 (RLVa-1.2.0b) | Modified: RLVa-1.1.0h
            {
                // There should be an option which should specify a valid reply channel (if there's an empty option the command is invalid)
                S32 nChannel = 0;
                VERIFY_OPTION_REF( (LLStringUtil::convertToS32(strOption, nChannel)) && (RlvUtil::isValidReplyChannel(nChannel)) );

                if (RLV_TYPE_ADD == eType)
                    addException(rlvCmd.getObjectID(), eBhvr, nChannel);
                else
                    removeException(rlvCmd.getObjectID(), eBhvr, nChannel);
            }
            break;
        case RLV_BHVR_NOTIFY:               // @notify:<params>=add|rem         - Checked: 2010-03-03 (RLVa-1.2.0a) | Modified: RLVa-1.2.0a
            {
                // There should be an option that we can successfully parse (if there's an empty option the command is invalid)
                S32 nChannel; std::string strFilter;
                VERIFY_OPTION_REF( (!strOption.empty()) && (rlvParseNotifyOption(strOption, nChannel, strFilter)) );

                // Charis: at most RLV_MAX_NOTIFY_PER_OBJECT registrations per object and RLV_MAX_NOTIFY_TOTAL in all (review S-M3),
                //         counted as they are added and removed. Removing always goes through.
                if (RLV_TYPE_ADD == eType)
                {
                    U32& nObjNotify = m_NotifyCounts[rlvCmd.getObjectID()];
                    if ( (nObjNotify >= RLV_MAX_NOTIFY_PER_OBJECT) || (m_nNotifyTotal >= RLV_MAX_NOTIFY_TOTAL) )
                    {
                        if (0 == nObjNotify)
                            m_NotifyCounts.erase(rlvCmd.getObjectID());
                        logLimit(rlvCmd.getObjectID(), "too many @notify registrations");
                        fRefCount = false;
                        eRet = RLV_RET_FAILED;
                        break;
                    }
                    nObjNotify++;
                    m_nNotifyTotal++;
                    RlvBehaviourNotifyHandler::getInstance()->addNotify(rlvCmd.getObjectID(), nChannel, strFilter);
                }
                else
                {
                    auto itCount = m_NotifyCounts.find(rlvCmd.getObjectID());
                    if (m_NotifyCounts.end() != itCount)
                    {
                        if (--itCount->second == 0)
                            m_NotifyCounts.erase(itCount);
                        if (m_nNotifyTotal > 0)
                            m_nNotifyTotal--;
                    }
                    RlvBehaviourNotifyHandler::getInstance()->removeNotify(rlvCmd.getObjectID(), nChannel, strFilter);
                }
            }
            break;
        //
        // Unknown or invalid
        //
        case RLV_BHVR_UNKNOWN:
            // Pass unknown commands on to registered command handlers
            return (notifyCommandHandlers(&RlvExtCommandHandler::onAddRemCommand, rlvCmd, eRet, false)) ? eRet : RLV_RET_FAILED_UNKNOWN;
        default:
            // Fail with "Invalid param" if none of the above handled it
            eRet = RLV_RET_FAILED_PARAM;
            break;
    }

    // If this command represents a behaviour restriction that's been added/removed then we need to do some additional processing
    if ( (RLV_RET_SUCCESS == eRet) && (fRefCount) )
    {
        if (RLV_TYPE_ADD == eType)
        {
            if (rlvCmd.isStrict())
                addException(rlvCmd.getObjectID(), RLV_BHVR_PERMISSIVE, eBhvr);
            m_Behaviours[eBhvr]++;
            rlvCmd.markRefCounted();
        }
        else
        {
            if (rlvCmd.isStrict())
                removeException(rlvCmd.getObjectID(), RLV_BHVR_PERMISSIVE, eBhvr);
            m_Behaviours[eBhvr]--;
        }

        m_OnBehaviour(eBhvr, eType);
        if ( ((RLV_TYPE_ADD == eType) && (1 == m_Behaviours[eBhvr])) || ((RLV_TYPE_REMOVE == eType) && (0 == m_Behaviours[eBhvr])) )
            m_OnBehaviourToggle(eBhvr, eType);
    }

    return eRet;
}

// Handles reference counting of behaviours and tracks strict exceptions for @permissive (all restriction handlers should call this function)
ERlvCmdRet RlvCommandHandlerBaseImpl<RLV_TYPE_ADDREM>::processCommand(const RlvCommand& rlvCmd, RlvBhvrHandlerFunc* pHandlerFunc, RlvBhvrToggleHandlerFunc* pToggleHandlerFunc)
{
    ERlvBehaviour eBhvr = rlvCmd.getBehaviourType();
    bool fRefCount = false, fHasBhvr = gRlvHandler.hasBehaviour(eBhvr);

    ERlvCmdRet eRet = (*pHandlerFunc)(rlvCmd, fRefCount);

    // If this command represents a restriction that's been added/removed then we need to do some additional processing
    if ( (RLV_RET_SUCCESS == eRet) && (fRefCount) )
    {
        if (RLV_TYPE_ADD == rlvCmd.getParamType())
        {
            if (rlvCmd.isStrict())
                gRlvHandler.addException(rlvCmd.getObjectID(), RLV_BHVR_PERMISSIVE, eBhvr);
            gRlvHandler.m_Behaviours[eBhvr]++;
            rlvCmd.markRefCounted();
        }
        else
        {
            if (rlvCmd.isStrict())
                gRlvHandler.removeException(rlvCmd.getObjectID(), RLV_BHVR_PERMISSIVE, eBhvr);
            if (RlvObject* pRlvObj = gRlvHandler.getObject(rlvCmd.getObjectID()))
                pRlvObj->clearModifiers(eBhvr);
            gRlvHandler.m_Behaviours[eBhvr]--;
        }

        gRlvHandler.m_OnBehaviour(eBhvr, rlvCmd.getParamType());
        if (fHasBhvr != gRlvHandler.hasBehaviour(eBhvr))
        {
            if (pToggleHandlerFunc)
                (*pToggleHandlerFunc)(eBhvr, !fHasBhvr);
            gRlvHandler.m_OnBehaviourToggle(eBhvr, rlvCmd.getParamType());
        }
    }

    return eRet;
}

// Handles: @bhvr=n|y
template<>
ERlvCmdRet RlvBehaviourGenericHandler<RLV_OPTION_NONE>::onCommand(const RlvCommand& rlvCmd, bool& fRefCount)
{
    // There should be no option
    if (rlvCmd.hasOption())
        return RLV_RET_FAILED_OPTION;

    fRefCount = true;
    return RLV_RET_SUCCESS;
}

// Handles: @bhvr:<uuid>=n|y
template<>
ERlvCmdRet RlvBehaviourGenericHandler<RLV_OPTION_EXCEPTION>::onCommand(const RlvCommand& rlvCmd, bool& fRefCount)
{
    // There should be an option and it should specify a valid UUID
    LLUUID idException;
    if (!RlvCommandOptionHelper::parseOption(rlvCmd.getOption(), idException))
        return RLV_RET_FAILED_OPTION;

    if (RLV_TYPE_ADD == rlvCmd.getParamType())
        RlvHandler::instance().addException(rlvCmd.getObjectID(), rlvCmd.getBehaviourType(), idException);
    else
        RlvHandler::instance().removeException(rlvCmd.getObjectID(), rlvCmd.getBehaviourType(), idException);

    // Charis: lets the UI parts refresh what depends on one avatar / object (e.g. @shownames:<uuid> name tags, rlvui_<branch>.cpp)
    gRlvHandler.m_OnException(rlvCmd.getBehaviourType(), idException, rlvCmd.getParamType());

    fRefCount = true;
    return RLV_RET_SUCCESS;
}

// Handles: @bhvr[:<uuid>]=n|y
template<>
ERlvCmdRet RlvBehaviourGenericHandler<RLV_OPTION_NONE_OR_EXCEPTION>::onCommand(const RlvCommand& rlvCmd, bool& fRefCount)
{
    // If there is an option then it should specify a valid UUID (but don't reference count)
    if (rlvCmd.hasOption())
    {
        ERlvCmdRet eRet = RlvBehaviourGenericHandler<RLV_OPTION_EXCEPTION>::onCommand(rlvCmd, fRefCount);
        fRefCount = false;
        return eRet;
    }
    return RlvBehaviourGenericHandler<RLV_OPTION_NONE>::onCommand(rlvCmd, fRefCount);
}

// Handles: @bhvr:<modifier>=n|y
template<>
ERlvCmdRet RlvBehaviourGenericHandler<RLV_OPTION_MODIFIER>::onCommand(const RlvCommand& rlvCmd, bool& fRefCount)
{
    // There should be an option and it should specify a valid modifier (RlvBehaviourModifier performs the appropriate type checks)
    RlvBehaviourModifier* pBhvrModifier = RlvBehaviourDictionary::instance().getModifierFromBehaviour(rlvCmd.getBehaviourType());
    RlvBehaviourModifierValue modValue;
    if ( (!rlvCmd.hasOption()) || (!pBhvrModifier) || (!pBhvrModifier->convertOptionValue(rlvCmd.getOption(), pBhvrModifier->getType(), modValue)) )
        return RLV_RET_FAILED_OPTION;

    // HACK-RLVa: reference counting doesn't happen until control returns to our caller but the modifier callbacks will happen now so we need to adjust the reference counts here
    if (RLV_TYPE_ADD == rlvCmd.getParamType())
    {
        gRlvHandler.m_Behaviours[rlvCmd.getBehaviourType()]++;
        pBhvrModifier->addValue(modValue, rlvCmd.getObjectID(), rlvCmd.getBehaviourType());
        gRlvHandler.m_Behaviours[rlvCmd.getBehaviourType()]--;
    }
    else
    {
        gRlvHandler.m_Behaviours[rlvCmd.getBehaviourType()]--;
        pBhvrModifier->removeValue(modValue, rlvCmd.getObjectID(), rlvCmd.getBehaviourType());
        gRlvHandler.m_Behaviours[rlvCmd.getBehaviourType()]++;
    }

    fRefCount = true;
    return RLV_RET_SUCCESS;
}

// Handles: @bhvr=n, @bhvr:<global modifier>=n|y and @bhvr:<local modifier>=force
template<>
ERlvCmdRet RlvBehaviourGenericHandler<RLV_OPTION_NONE_OR_MODIFIER>::onCommand(const RlvCommand& rlvCmd, bool& fRefCount)
{
    if ( (rlvCmd.getParamType() & RLV_TYPE_ADDREM) && (rlvCmd.hasOption()) )
    {
        // @bhvr:<global modifier>=n|y : if there is an option then it should specify a valid global modifier and if so we reference count
        return RlvBehaviourGenericHandler<RLV_OPTION_MODIFIER>::onCommand(rlvCmd, fRefCount);
    }
    else if (rlvCmd.getParamType() == RLV_TYPE_FORCE)
    {
        // @bhvr:<local modifier>=force : local modifiers hide behind their primary behaviour which knows how to handle them
        return rlvCmd.getBehaviourInfo()->processModifier(rlvCmd);
    }

    // @bhvr=n : add the default option on an empty modifier if needed
    RlvBehaviourModifier* pBhvrModifier = RlvBehaviourDictionary::instance().getModifierFromBehaviour(rlvCmd.getBehaviourType());
    if ( (pBhvrModifier) && (pBhvrModifier->getAddDefault()) )
    {
        // HACK-RLVa: reference counting doesn't happen until control returns to our caller but the modifier callbacks will happen now so we need to adjust the reference counts here
        if (RLV_TYPE_ADD == rlvCmd.getParamType())
        {
            gRlvHandler.m_Behaviours[rlvCmd.getBehaviourType()]++;
            pBhvrModifier->addValue(pBhvrModifier->getDefaultValue(), rlvCmd.getObjectID(), rlvCmd.getBehaviourType());
            gRlvHandler.m_Behaviours[rlvCmd.getBehaviourType()]--;
        }
        else
        {
            gRlvHandler.m_Behaviours[rlvCmd.getBehaviourType()]--;
            pBhvrModifier->removeValue(pBhvrModifier->getDefaultValue(), rlvCmd.getObjectID(), rlvCmd.getBehaviourType());
            gRlvHandler.m_Behaviours[rlvCmd.getBehaviourType()]++;
        }
    }

    fRefCount = true;
    return RLV_RET_SUCCESS;
}

// Handles: @addattach[:<attachpt>]=n|y and @remattach[:<attachpt>]=n|y
template<> template<>
ERlvCmdRet RlvBehaviourAddRemAttachHandler::onCommand(const RlvCommand& rlvCmd, bool& fRefCount)
{
    // Sanity check - if there's an option it should specify a valid attachment point name
    S32 idxAttachPt = RlvAttachPtLookup::getAttachPointIndex(rlvCmd.getOption());
    if ( (!idxAttachPt) && (!rlvCmd.getOption().empty())  )
        return RLV_RET_FAILED_OPTION;

    if (!isAgentAvatarValid())
        return RLV_RET_FAILED;

    // We need to flush any queued force-wear commands before changing the restrictions
    if (RlvForceWear::instanceExists())
        RlvForceWear::instance().done();

    ERlvLockMask eLock = (RLV_BHVR_REMATTACH == rlvCmd.getBehaviourType()) ? RLV_LOCK_REMOVE : RLV_LOCK_ADD;
    for (LLVOAvatar::attachment_map_t::const_iterator itAttach = gAgentAvatarp->mAttachmentPoints.begin();
            itAttach != gAgentAvatarp->mAttachmentPoints.end(); ++itAttach)
    {
        if ( (0 == idxAttachPt) || (itAttach->first == idxAttachPt) )
        {
            if (RLV_TYPE_ADD == rlvCmd.getParamType())
                gRlvAttachmentLocks.addAttachmentPointLock(itAttach->first, rlvCmd.getObjectID(), eLock);
            else
                gRlvAttachmentLocks.removeAttachmentPointLock(itAttach->first, rlvCmd.getObjectID(), eLock);
        }
    }

    fRefCount = rlvCmd.getOption().empty(); // Only reference count global locks
    return RLV_RET_SUCCESS;
}

// Handles: @detach[:<attachpt>]=n|y
template<> template<>
ERlvCmdRet RlvBehaviourHandler<RLV_BHVR_DETACH>::onCommand(const RlvCommand& rlvCmd, bool& fRefCount)
{
    // We need to flush any queued force-wear commands before changing the restrictions
    if (RlvForceWear::instanceExists())
        RlvForceWear::instance().done();

    if (rlvCmd.getOption().empty()) // @detach=n|y - RLV_LOCK_REMOVE locks an attachment *object*
    {
        // The object may or may not exist (it may not have rezzed yet, or it may have already been killed):
        //   * @detach=n: - if it has rezzed then we'll already have looked up what we need (see next line if it's not an attachment)
        //                - if it hasn't rezzed yet then it's a @detach=n from a non-attachment and RlvHandler::onAttach() takes care of it
        //   * @detach=y: - if it ever rezzed as an attachment we'll have cached the UUID of its root
        //                - if it never rezzed as an attachment there won't be a lock to remove
        RlvHandler::rlv_object_map_t::const_iterator itObj = gRlvHandler.m_Objects.find(rlvCmd.getObjectID());
        if ( (itObj != gRlvHandler.m_Objects.end()) && (itObj->second.hasLookup()) && (itObj->second.getAttachPt()) )
        {
            if (RLV_TYPE_ADD == rlvCmd.getParamType())
                gRlvAttachmentLocks.addAttachmentLock(itObj->second.getRootID(), itObj->first);
            else
                gRlvAttachmentLocks.removeAttachmentLock(itObj->second.getRootID(), itObj->first);
        }
    }
    else                            // @detach:<attachpt>=n|y - RLV_LOCK_ADD and RLV_LOCK_REMOVE locks an attachment *point*
    {
        // The attachment point index should always be non-zero for @detach:<attachpt>=n|y
        S32 idxAttachPt = RlvAttachPtLookup::getAttachPointIndex(rlvCmd.getOption());
        if (0 == idxAttachPt)
            return RLV_RET_FAILED_OPTION;

        if (RLV_TYPE_ADD == rlvCmd.getParamType())
            gRlvAttachmentLocks.addAttachmentPointLock(idxAttachPt, rlvCmd.getObjectID(), (ERlvLockMask)(RLV_LOCK_ADD | RLV_LOCK_REMOVE));
        else
            gRlvAttachmentLocks.removeAttachmentPointLock(idxAttachPt, rlvCmd.getObjectID(), (ERlvLockMask)(RLV_LOCK_ADD | RLV_LOCK_REMOVE));
    }

    fRefCount = false;  // Don't reference count @detach[:<option>]=n
    return RLV_RET_SUCCESS;
}

// Checked: 2010-11-30 (RLVa-1.3.0b) | Added: RLVa-1.3.0b
ERlvCmdRet RlvHandler::onAddRemFolderLock(const RlvCommand& rlvCmd, bool& fRefCount)
{
    RlvCommandOptionGeneric rlvCmdOption;
    RlvCommandOptionHelper::parseOption(rlvCmd.getOption(), rlvCmdOption);

    RlvFolderLocks::folderlock_source_t lockSource;
    if (RLV_TYPE_REMOVE == rlvCmd.getParamType())
    {
        // Charis: =y must never fail on the inventory (the folder may be gone by now, which would leave the lock behind with no
        //         owner): find the lock's source from the option's text only, in the order parseOption() tried at =n
        LLWearableType::EType wtType = rlvTypeNameToType(rlvCmd.getOption());
        S32 idxAttachPt = (rlvCmd.getOption().empty()) ? 0 : RlvAttachPtLookup::getAttachPointIndex(rlvCmd.getOption());
        if (rlvCmd.getOption().empty())
            lockSource = RlvFolderLocks::folderlock_source_t(RlvFolderLocks::ST_ATTACHMENT, rlvCmd.getObjectID());
        else if (wtType != LLWearableType::WT_INVALID)
            lockSource = RlvFolderLocks::folderlock_source_t(RlvFolderLocks::ST_WEARABLETYPE, wtType);
        else if (idxAttachPt)
            lockSource = RlvFolderLocks::folderlock_source_t(RlvFolderLocks::ST_ATTACHMENTPOINT, idxAttachPt);
        else
            lockSource = RlvFolderLocks::folderlock_source_t(RlvFolderLocks::ST_SHAREDPATH, rlvCmd.getOption());
    }
    else if (rlvCmdOption.isEmpty())
    {
        lockSource = RlvFolderLocks::folderlock_source_t(RlvFolderLocks::ST_ATTACHMENT, rlvCmd.getObjectID());
    }
    else if (rlvCmdOption.isSharedFolder())
    {
        lockSource = RlvFolderLocks::folderlock_source_t(RlvFolderLocks::ST_SHAREDPATH, rlvCmd.getOption());
    }
    else if (rlvCmdOption.isAttachmentPoint())
    {
        lockSource = RlvFolderLocks::folderlock_source_t(RlvFolderLocks::ST_ATTACHMENTPOINT, RlvAttachPtLookup::getAttachPointIndex(rlvCmdOption.getAttachmentPoint()));
    }
    else if (rlvCmdOption.isWearableType())
    {
        lockSource = RlvFolderLocks::folderlock_source_t(RlvFolderLocks::ST_WEARABLETYPE, rlvCmdOption.getWearableType());
    }
    else
    {
        fRefCount = false;  // Don't reference count failure
        return RLV_RET_FAILED_OPTION;
    }

    // Determine the lock type
    ERlvLockMask eLockType = (RLV_BHVR_ATTACHTHIS == rlvCmd.getBehaviourType()) ? RLV_LOCK_ADD : RLV_LOCK_REMOVE;

    // Determine the folder lock options from the issued behaviour
    RlvFolderLocks::ELockPermission eLockPermission = RlvFolderLocks::PERM_DENY;
    RlvFolderLocks::ELockScope eLockScope = (RlvBehaviourInfo::FORCEWEAR_SUBTREE & rlvCmd.getBehaviourFlags()) ? RlvFolderLocks::SCOPE_SUBTREE : RlvFolderLocks::SCOPE_NODE;

    if (RLV_TYPE_ADD == rlvCmd.getParamType())
        RlvFolderLocks::instance().addFolderLock(lockSource, eLockPermission, eLockScope, rlvCmd.getObjectID(), eLockType);
    else
        RlvFolderLocks::instance().removeFolderLock(lockSource, eLockPermission, eLockScope, rlvCmd.getObjectID(), eLockType);

    fRefCount = true;
    return RLV_RET_SUCCESS;
}

// Checked: 2011-03-27 (RLVa-1.3.0g) | Added: RLVa-1.3.0g
ERlvCmdRet RlvHandler::onAddRemFolderLockException(const RlvCommand& rlvCmd, bool& fRefCount)
{
    // Sanity check - the option should specify a shared folder path (Charis: only when adding; =y takes the text as it is, so a
    //                folder that is gone can't keep the lock alive)
    RlvCommandOptionGeneric rlvCmdOption;
    RlvCommandOptionHelper::parseOption(rlvCmd.getOption(), rlvCmdOption);
    if ( (RLV_TYPE_REMOVE != rlvCmd.getParamType()) && (!rlvCmdOption.isSharedFolder()) )
        return RLV_RET_FAILED_OPTION;

    ERlvBehaviour eBhvr = rlvCmd.getBehaviourType();

    // Determine the lock type
    ERlvLockMask eLockType = (RLV_BHVR_ATTACHTHISEXCEPT == eBhvr) ? RLV_LOCK_ADD : RLV_LOCK_REMOVE;

    // Determine the folder lock options from the issued behaviour
    RlvFolderLocks::ELockPermission eLockPermission = RlvFolderLocks::PERM_ALLOW;
    RlvFolderLocks::ELockScope eLockScope = (RlvBehaviourInfo::FORCEWEAR_SUBTREE & rlvCmd.getBehaviourFlags()) ? RlvFolderLocks::SCOPE_SUBTREE : RlvFolderLocks::SCOPE_NODE;

    RlvFolderLocks::folderlock_source_t lockSource(RlvFolderLocks::ST_SHAREDPATH, rlvCmd.getOption());
    if (RLV_TYPE_ADD == rlvCmd.getParamType())
        RlvFolderLocks::instance().addFolderLock(lockSource, eLockPermission, eLockScope, rlvCmd.getObjectID(), eLockType);
    else
        RlvFolderLocks::instance().removeFolderLock(lockSource, eLockPermission, eLockScope, rlvCmd.getObjectID(), eLockType);

    fRefCount = true;
    return RLV_RET_SUCCESS;
}

// Handles: @sendchannel[:<channel>]=n|y and @sendchannel_except[:<channel>]=n|y
template<> template<>
ERlvCmdRet RlvBehaviourSendChannelHandler::onCommand(const RlvCommand& rlvCmd, bool& fRefCount)
{
    // If there's an option then it should be a valid (= positive and non-zero) chat channel
    if (rlvCmd.hasOption())
    {
        S32 nChannel = 0;
        if ( (!RlvCommandOptionHelper::parseOption(rlvCmd.getOption(), nChannel)) || (nChannel <= 0) )
            return RLV_RET_FAILED_OPTION;

        if (RLV_TYPE_ADD == rlvCmd.getParamType())
            gRlvHandler.addException(rlvCmd.getObjectID(),  rlvCmd.getBehaviourType(), nChannel);
        else
            gRlvHandler.removeException(rlvCmd.getObjectID(),  rlvCmd.getBehaviourType(), nChannel);
    }
    else
    {
        fRefCount = true;
    }
    return RLV_RET_SUCCESS;
}

// Handles: @recvim[:<uuid|range>]=n|y, @sendim[:<uuid|range>]=n|y and @startim[:<uuid|range>]=n|y
template<> template<>
ERlvCmdRet RlvBehaviourRecvSendStartIMHandler::onCommand(const RlvCommand& rlvCmd, bool& fRefCount)
{
    ERlvCmdRet eRet = RlvBehaviourGenericHandler<RLV_OPTION_NONE_OR_EXCEPTION>::onCommand(rlvCmd, fRefCount);
    if ( (RLV_RET_SUCCESS != eRet) && (rlvCmd.hasOption()) )
    {
        // Check for <dist_min>[;<dist_max>] option
        std::vector<std::string> optionList; float nDistMin = F32_MAX, nDistMax = F32_MAX;
        if ( (!RlvCommandOptionHelper::parseStringList(rlvCmd.getOption(), optionList)) || (optionList.empty()) || (optionList.size() > 2) ||
             (!RlvCommandOptionHelper::parseOption(optionList[0], nDistMin)) || (!std::isfinite(nDistMin)) || (nDistMin < 0) ||
             ( (optionList.size() >= 2) && ((!RlvCommandOptionHelper::parseOption(optionList[1], nDistMax)) || (!std::isfinite(nDistMax))) ) || (nDistMax < 0) )
        {
            return RLV_RET_FAILED_OPTION;
        }
        // Charis: keep the distances in range (research/248 6.2); the same clamp on =y finds the value that =n added
        nDistMin = llmin(nDistMin, RLV_MAX_IM_DISTANCE);
        nDistMax = llmin(nDistMax, RLV_MAX_IM_DISTANCE);

        // Valid option(s) - figure out which modifier(s) to change
        ERlvBehaviourModifier eModDistMin, eModDistMax;
        switch (rlvCmd.getBehaviourType())
        {
            case RLV_BHVR_RECVIM:
                eModDistMin = RLV_MODIFIER_RECVIMDISTMIN; eModDistMax = RLV_MODIFIER_RECVIMDISTMAX;
                break;
            case RLV_BHVR_SENDIM:
                eModDistMin = RLV_MODIFIER_SENDIMDISTMIN; eModDistMax = RLV_MODIFIER_SENDIMDISTMAX;
                break;
            case RLV_BHVR_STARTIM:
                eModDistMin = RLV_MODIFIER_STARTIMDISTMIN; eModDistMax = RLV_MODIFIER_STARTIMDISTMAX;
                break;
            default:
                return RLV_RET_FAILED_OPTION;
        }

        RlvBehaviourModifier *pBhvrModDistMin = RlvBehaviourDictionary::instance().getModifier(eModDistMin), *pBhvrModDistMax = RlvBehaviourDictionary::instance().getModifier(eModDistMax);
        if ( (!pBhvrModDistMin) || (!pBhvrModDistMax) )
            return RLV_RET_FAILED;
        if (RLV_TYPE_ADD == rlvCmd.getParamType())
        {
            pBhvrModDistMin->addValue(nDistMin * nDistMin, rlvCmd.getObjectID(), rlvCmd.getBehaviourType());
            if (optionList.size() >= 2)
                pBhvrModDistMax->addValue(nDistMax * nDistMax, rlvCmd.getObjectID(), rlvCmd.getBehaviourType());
        }
        else
        {
            pBhvrModDistMin->removeValue(nDistMin * nDistMin, rlvCmd.getObjectID(), rlvCmd.getBehaviourType());
            if (optionList.size() >= 2)
                pBhvrModDistMax->removeValue(nDistMax * nDistMax, rlvCmd.getObjectID(), rlvCmd.getBehaviourType());
        }

        fRefCount = true;
        return RLV_RET_SUCCESS;
    }
    return eRet;
}

// Handles: @sendim=n|y toggles
template<> template<>
void RlvBehaviourToggleHandler<RLV_BHVR_SENDIM>::onCommandToggle(ERlvBehaviour eBhvr, bool fHasBhvr)
{
    if (LLControlVariable* pControl = gSavedPerAccountSettings.getControl("DoNotDisturbModeResponse"))
        pControl->setHiddenFromSettingsEditor(fHasBhvr);
}

// Handles: @shownames[:<uuid>]=n|y toggles
template<> template<>
void RlvBehaviourToggleHandler<RLV_BHVR_SHOWNAMES>::onCommandToggle(ERlvBehaviour eBhvr, bool fHasBhvr)
{
    if (LLApp::isExiting() || gDoDisconnect)
        return; // Nothing to do if the viewer is shutting down

    // Update the shownames context
    RlvActions::setShowName(RlvActions::SNC_DEFAULT, !fHasBhvr);

    // Charis: the rest of FS's toggle (radar and voice list names, forcing display names, LLVOAvatar::invalidateNameTags(),
    // refreshing the hover text) is UI and runs from the toggle registered with RlvUIEnabler (rlvui_show.cpp, design 3.1)
}

// Handles: @shownames[:<uuid>]=n|y
template<> template<>
ERlvCmdRet RlvBehaviourHandler<RLV_BHVR_SHOWNAMES>::onCommand(const RlvCommand& rlvCmd, bool& fRefCount)
{
    // Charis: FS refreshes the radar / voice list names, the avatar's name tag and the hover text here when an exception
    // changes; the exception signal (RlvHandler::setExceptionCallback(), fired by the generic handler) lets rlvui_show.cpp do that
    return RlvBehaviourGenericHandler<RLV_OPTION_NONE_OR_EXCEPTION>::onCommand(rlvCmd, fRefCount);
}

// Handles: @shownametags[:<distance>] value changes
template<>
void RlvBehaviourModifierHandler<RLV_MODIFIER_SHOWNAMETAGSDIST>::onValueChange() const
{
    // Charis: FS calls LLVOAvatar::invalidateNameTags() here; rlvui_show.cpp listens to this modifier's getSignal() instead
}

// Handles: @shownametags[:<distance|uuid>]=n|y
template<> template<>
ERlvCmdRet RlvBehaviourHandler<RLV_BHVR_SHOWNAMETAGS>::onCommand(const RlvCommand& rlvCmd, bool& fRefCount)
{
    LLUUID idOption;
    if ( (rlvCmd.hasOption()) && (RlvCommandOptionHelper::parseOption(rlvCmd.getOption(), idOption)) )
    {
        // Charis: the name tag of idOption is refreshed through the exception signal (FS: LLVOAvatar::invalidateNameTag(idOption))
        ERlvCmdRet eRet = RlvBehaviourGenericHandler<RLV_OPTION_EXCEPTION>::onCommand(rlvCmd, fRefCount);
        fRefCount = false;
        return eRet;
    }
    return RlvBehaviourGenericHandler<RLV_OPTION_NONE_OR_MODIFIER>::onCommand(rlvCmd, fRefCount);
}

// ============================================================================
// Command handlers (RLV_TYPE_FORCE)
//

ERlvCmdRet RlvCommandHandlerBaseImpl<RLV_TYPE_FORCE>::processCommand(const RlvCommand& rlvCmd, RlvForceHandlerFunc* pHandler)
{
    return (*pHandler)(rlvCmd);
}

// Handles: @bhvr:<modifier>=force
template<>
ERlvCmdRet RlvForceGenericHandler<RLV_OPTION_MODIFIER>::onCommand(const RlvCommand& rlvCmd)
{
    // The object should be holding at least one active behaviour
    if (!gRlvHandler.hasBehaviour(rlvCmd.getObjectID()))
        return RLV_RET_FAILED_NOBEHAVIOUR;

    // There should be an option and it should specify a valid modifier (RlvBehaviourModifier performs the appropriate type checks)
    RlvBehaviourModifier* pBhvrModifier = RlvBehaviourDictionary::instance().getModifierFromBehaviour(rlvCmd.getBehaviourType());
    RlvBehaviourModifierValue modValue;
    if ( (!rlvCmd.hasOption()) || (!pBhvrModifier) || (!pBhvrModifier->convertOptionValue(rlvCmd.getOption(), pBhvrModifier->getType(), modValue)) )
        return RLV_RET_FAILED_OPTION;

    pBhvrModifier->setValue(modValue, rlvCmd.getObjectID());
    return RLV_RET_SUCCESS;
}

// Checked: 2010-04-07 (RLVa-1.2.0d) | Modified: RLVa-1.1.0j
ERlvCmdRet RlvHandler::processForceCommand(const RlvCommand& rlvCmd) const
{
    RLV_ASSERT(RLV_TYPE_FORCE == rlvCmd.getParamType());

    // Try a command processor first
    ERlvCmdRet eRet = rlvCmd.processCommand();
    if (RLV_RET_NO_PROCESSOR != eRet)
    {
        return eRet;
    }

    // Process the command the legacy way
    eRet = RLV_RET_SUCCESS;
    switch (rlvCmd.getBehaviourType())
    {
        // RLV-SIT: case RLV_BHVR_UNSIT - @unsit=force (AGENT_CONTROL_STAND_UP + send_agent_update(), m_idPendingUnsitActor = getCurrentObject())
        case RLV_CMD_FORCEWEAR:     // @forcewear[:<options>]=force         - Checked: 2011-09-12 (RLVa-1.5.0)
            {
                if (RlvBehaviourInfo::FORCEWEAR_CONTEXT_NONE & rlvCmd.getBehaviourFlags())
                {
                    RlvCommandOptionGeneric rlvCmdOption;
                    RlvCommandOptionHelper::parseOption(rlvCmd.getOption(), rlvCmdOption);
                    VERIFY_OPTION(rlvCmdOption.isSharedFolder());
                    eRet = onForceWear(rlvCmdOption.getSharedFolder(), rlvCmd.getBehaviourFlags());
                }
                else if (RlvBehaviourInfo::FORCEWEAR_CONTEXT_OBJECT & rlvCmd.getBehaviourFlags())
                {
                    RlvCommandOptionGetPath rlvGetPathOption(rlvCmd, boost::bind(&RlvHandler::onForceWearCallback, this, _1, rlvCmd.getBehaviourFlags(), rlvCmd.getObjectID()));
                    VERIFY_OPTION(rlvGetPathOption.isValid());
                    eRet = (!rlvGetPathOption.isCallback()) ? RLV_RET_SUCCESS : RLV_RET_SUCCESS_DELAYED;
                }
            }
            break;
        // Charis: @adjustheight=force is stage 3
        case RLV_BHVR_UNKNOWN:
            // Pass unknown commands on to registered command handlers
            return (notifyCommandHandlers(&RlvExtCommandHandler::onForceCommand, rlvCmd, eRet, false)) ? eRet : RLV_RET_FAILED_UNKNOWN;
        default:
            // Fail with "Invalid param" if none of the above handled it
            eRet = RLV_RET_FAILED_PARAM;
            break;
    }
    return eRet;
}

// Handles: @detachme=force
template<> template<>
ERlvCmdRet RlvForceHandler<RLV_BHVR_DETACHME>::onCommand(const RlvCommand& rlvCmd)
{
    if (rlvCmd.hasOption())
        return RLV_RET_FAILED_OPTION;

    // NOTE: @detachme should respect locks but shouldn't respect things like nostrip
    const LLViewerObject* pAttachObj = gObjectList.findObject(rlvCmd.getObjectID());
    if ( (pAttachObj) && (pAttachObj->isAttachment()) )
        LLVOAvatarSelf::detachAttachmentIntoInventory(pAttachObj->getAttachmentItemID());

    return RLV_RET_SUCCESS;
}

// Handles: @remattach[:<folder|attachpt|attachgroup>]=force
template<> template<>
ERlvCmdRet RlvForceRemAttachHandler::onCommand(const RlvCommand& rlvCmd)
{
    if (!isAgentAvatarValid())
        return RLV_RET_FAILED;

    RlvCommandOptionGeneric rlvCmdOption;
    RlvCommandOptionHelper::parseOption(rlvCmd.getOption(), rlvCmdOption);
    if (rlvCmdOption.isSharedFolder())
        return gRlvHandler.onForceWear(rlvCmdOption.getSharedFolder(), rlvCmd.getBehaviourFlags());

    // @remattach:<attachpt>=force - force detach single attachment point
    if (rlvCmdOption.isAttachmentPoint())
    {
        RlvForceWear::instance().forceDetach(rlvCmdOption.getAttachmentPoint());
        return RLV_RET_SUCCESS;
    }
    // @remattach:<group>=force - force detach attachments points belonging to <group>
    // @remattach=force         - force detach all attachments points
    else if ( (rlvCmdOption.isAttachmentPointGroup()) || (rlvCmdOption.isEmpty()) )
    {
        for (const auto& entryAttachPt : gAgentAvatarp->mAttachmentPoints)
        {
            const LLViewerJointAttachment* pAttachPt = entryAttachPt.second;
            if ( (pAttachPt) && (pAttachPt->getNumObjects()) && ((rlvCmdOption.isEmpty()) || (rlvAttachGroupFromIndex(pAttachPt->getGroup()) == rlvCmdOption.getAttachmentPointGroup())) )
            {
                RlvForceWear::instance().forceDetach(pAttachPt);
            }
        }
        return RLV_RET_SUCCESS;
    }
    // @remattach:<uuid>=force - force detach a specific attachment
    else if (rlvCmdOption.isUUID())
    {
        const LLViewerObject* pAttachObj = gObjectList.findObject(rlvCmdOption.getUUID());
        if ( (pAttachObj) && (pAttachObj->isAttachment()) && (pAttachObj->permYouOwner()) )
            RlvForceWear::instance().forceDetach(pAttachObj);
        return RLV_RET_SUCCESS;
    }
    return RLV_RET_FAILED_OPTION;
}
// Handles: @remoutfit[:<folder|layer>]=force
template<> template<>
ERlvCmdRet RlvForceHandler<RLV_BHVR_REMOUTFIT>::onCommand(const RlvCommand& rlvCmd)
{
    RlvCommandOptionGeneric rlvCmdOption;
    RlvCommandOptionHelper::parseOption(rlvCmd.getOption(), rlvCmdOption);
    if (rlvCmdOption.isSharedFolder())
        return gRlvHandler.onForceWear(rlvCmdOption.getSharedFolder(), rlvCmd.getBehaviourFlags());

    if ( (!rlvCmdOption.isWearableType()) && (!rlvCmdOption.isEmpty()) )
        return RLV_RET_FAILED_OPTION;

    for (int idxType = 0; idxType < LLWearableType::WT_COUNT; idxType++)
    {
        if ( (rlvCmdOption.isEmpty()) || ((LLWearableType::EType)idxType == rlvCmdOption.getWearableType()))
            RlvForceWear::instance().forceRemove((LLWearableType::EType)idxType);
    }
    return RLV_RET_SUCCESS;
}

// Checked: 2010-08-30 (RLVa-1.2.1c) | Modified: RLVa-1.2.1c
ERlvCmdRet RlvHandler::onForceWear(const LLViewerInventoryCategory* pFolder, U32 nFlags) const
{
    if ( (!pFolder) || (!RlvInventory::instance().isSharedFolder(pFolder->getUUID())) )
        return RLV_RET_FAILED_OPTION;

    RlvForceWear::EWearAction eAction = (RlvBehaviourInfo::FORCEWEAR_WEAR_REMOVE & nFlags) ? RlvForceWear::ACTION_REMOVE
                                                                                           : ((RlvBehaviourInfo::FORCEWEAR_WEAR_ADD & nFlags) ? RlvForceWear::ACTION_WEAR_ADD
                                                                                                                                              : RlvForceWear::ACTION_WEAR_REPLACE);
    RlvForceWear::EWearFlags eFlags = (RlvBehaviourInfo::FORCEWEAR_SUBTREE & nFlags) ? RlvForceWear::FLAG_MATCHALL : RlvForceWear::FLAG_DEFAULT;

    RlvForceWear::instance().forceFolder(pFolder, eAction, eFlags);
    return RLV_RET_SUCCESS;
}

void RlvHandler::onForceWearCallback(const uuid_vec_t& idItems, U32 nFlags, const LLUUID& idRlvObj) const
{
    // Charis: this runs later than the command (the attachment it waited for showed up), so RLVa may be off by then and the
    //         object may have been blocked (a refused Experience) since
    if ( (!isEnabled()) || (isBlockedObjectOrRoot(idRlvObj)) )
        return;

    LLInventoryModel::cat_array_t folders;
    if (RlvInventory::instance().getPath(idItems, folders))
    {
        for (size_t idxFolder = 0, cntFolder = folders.size(); idxFolder < cntFolder; idxFolder++)
            onForceWear(folders.at(idxFolder), nFlags);

        // If we're not executing a command then we're a delayed callback and need to manually call done()
        if ( (!getCurrentCommand()) && (RlvForceWear::instanceExists()) )
            RlvForceWear::instance().done();
    }
}

// RLV-SIT: RlvForceHandler<RLV_BHVR_SITGROUND>::onCommand() - @sitground=force
// RLV-SIT: RlvForceHandler<RLV_BHVR_SIT>::onCommand() - @sit:<uuid>=force (AgentRequestSit, @standtp source position)

// Handles: @tpto:<vector>[;<angle>]=force and @tpto:<region>/<vector>[;<angle>]=force
template<> template<>
ERlvCmdRet RlvForceHandler<RLV_BHVR_TPTO>::onCommand(const RlvCommand& rlvCmd)
{
    std::vector<std::string> optionList;
    if (!RlvCommandOptionHelper::parseStringList(rlvCmd.getOption(), optionList))
        return RLV_RET_FAILED_OPTION;

    // <destination>[;<angle>] - anything after the angle is ignored (FS)
    if (optionList.empty())
        return RLV_RET_FAILED_OPTION;

    // We need the look-at first
    LLVector3 vecLookAt = LLVector3::zero;
    if (optionList.size() > 1)
    {
        float nAngle = 0.0f;
        if ( (!RlvCommandOptionHelper::parseOption(optionList[1], nAngle)) || (!std::isfinite(nAngle)) )
            return RLV_RET_FAILED_OPTION;

        vecLookAt = LLVector3::x_axis;
        vecLookAt.rotVec(nAngle, LLVector3::z_axis);
        vecLookAt.normalize();
    }

    // Next figure out the destination
    LLVector3d posGlobal;
    if (RlvCommandOptionHelper::parseOption(optionList[0], posGlobal))
    {
        if (!posGlobal.isFinite())
            return RLV_RET_FAILED_OPTION;
        // Charis: keep the destination on the grid (review S-L6)
        posGlobal.mdV[VX] = llclamp(posGlobal.mdV[VX], 0.0, RLV_MAX_TP_GLOBAL);
        posGlobal.mdV[VY] = llclamp(posGlobal.mdV[VY], 0.0, RLV_MAX_TP_GLOBAL);
        posGlobal.mdV[VZ] = llclamp(posGlobal.mdV[VZ], 0.0, RLV_MAX_TP_HEIGHT);
        if (optionList.size() == 1)
            gAgent.teleportViaLocation(posGlobal);
        else
            gAgent.teleportViaLocationLookAt(posGlobal, vecLookAt);
    }
    else
    {
        // Charis: region names are short (35 characters in SL), refuse anything longer than 64 and non-finite coordinates
        std::vector<std::string> posList; LLVector3 posRegion;
        if ( (!RlvCommandOptionHelper::parseStringList(optionList[0], posList, std::string("/"))) || (4 != posList.size()) ||
             (posList[0].empty()) || (posList[0].size() > 64) ||
             (!RlvCommandOptionHelper::parseOption(optionList[0].substr(posList[0].size() + 1), posRegion)) || (!posRegion.isFinite()) )
        {
            return RLV_RET_FAILED_OPTION;
        }
        // Charis: keep the position inside the region (review S-L6)
        posRegion.mV[VX] = llclamp(posRegion.mV[VX], 0.f, 256.f);
        posRegion.mV[VY] = llclamp(posRegion.mV[VY], 0.f, 256.f);
        posRegion.mV[VZ] = llclamp(posRegion.mV[VZ], 0.f, (F32)RLV_MAX_TP_HEIGHT);

        LLWorldMapMessage::url_callback_t cb = boost::bind(&RlvHandler::onTeleportCallback, &gRlvHandler, _1, posRegion, vecLookAt, rlvCmd.getObjectID());
        LLWorldMapMessage::getInstance()->sendNamedRegionRequest(posList[0], cb, std::string(""), true);
    }

    return RLV_RET_SUCCESS;
}

// ============================================================================
// Command handlers (RLV_TYPE_REPLY)
//

ERlvCmdRet RlvCommandHandlerBaseImpl<RLV_TYPE_REPLY>::processCommand(const RlvCommand& rlvCmd, RlvReplyHandlerFunc* pHandler)
{
    // Sanity check - <param> should specify a - valid - reply channel
    // Charis: only positive channels that aren't the debug channel; never channel 0 (the RLV console that answered on 0 is stage 3)
    S32 nChannel;
    if ( (!LLStringUtil::convertToS32(rlvCmd.getParam(), nChannel)) || (!RlvUtil::isValidReplyChannel(nChannel)) )
        return RLV_RET_FAILED_PARAM;

    std::string strReply;
    ERlvCmdRet eRet = (*pHandler)(rlvCmd, strReply);

    // If we made it this far then:
    //   - the command was handled successfully so we send off the response
    //   - the command failed but we still send off an - empty - response to keep the issuing script from blocking
    RlvUtil::sendChatReply(nChannel, strReply, true, rlvCmd.getObjectID());  // Charis: counted against the object's reply budget too

    return eRet;
}

// Checked: 2010-04-07 (RLVa-1.2.0d) | Modified: RLVa-1.1.0f
ERlvCmdRet RlvHandler::processReplyCommand(const RlvCommand& rlvCmd) const
{
    RLV_ASSERT(RLV_TYPE_REPLY == rlvCmd.getParamType());

    // Try a command processor first
    ERlvCmdRet eRet = rlvCmd.processCommand();
    if (RLV_RET_NO_PROCESSOR != eRet)
    {
        return eRet;
    }

    // Sanity check - <param> should specify a - valid - reply channel (Charis: never channel 0, see above)
    S32 nChannel;
    if ( (!LLStringUtil::convertToS32(rlvCmd.getParam(), nChannel)) || (!RlvUtil::isValidReplyChannel(nChannel)) )
        return RLV_RET_FAILED_PARAM;

    // Process the command the legacy way
    eRet = RLV_RET_SUCCESS; std::string strReply;
    switch (rlvCmd.getBehaviourType())
    {
        case RLV_BHVR_VERSION:          // @version=<channel>                   - Checked: 2010-03-27 (RLVa-1.4.0a)
        case RLV_BHVR_VERSIONNEW:       // @versionnew=<channel>                - Checked: 2010-03-27 (RLVa-1.4.0a) | Added: RLVa-1.2.0b
            // NOTE: RLV will respond even if there's an option
            strReply = RlvStrings::getVersion(rlvCmd.getObjectID(), RLV_BHVR_VERSION == rlvCmd.getBehaviourType());
            break;
        case RLV_BHVR_VERSIONNUM:       // @versionnum=<channel>                - Checked: 2010-03-27 (RLVa-1.4.0a) | Added: RLVa-1.0.4b
            // NOTE: RLV will respond even if there's an option
            if (!rlvCmd.hasOption())
                strReply = RlvStrings::getVersionNum(rlvCmd.getObjectID());
            else if ("impl" == rlvCmd.getOption())
                strReply = RlvStrings::getVersionImplNum();
            break;
        case RLV_BHVR_GETATTACH:        // @getattach[:<layer>]=<channel>
            eRet = onGetAttach(rlvCmd, strReply);
            break;
        case RLV_BHVR_GETATTACHNAMES:   // @getattachnames[:<grp>]=<channel>
        case RLV_BHVR_GETADDATTACHNAMES:// @getaddattachnames[:<grp>]=<channel>
        case RLV_BHVR_GETREMATTACHNAMES:// @getremattachnames[:<grp>]=<channel>
            eRet = onGetAttachNames(rlvCmd, strReply);
            break;
        case RLV_BHVR_GETOUTFIT:        // @getoutfit[:<layer>]=<channel>
            eRet = onGetOutfit(rlvCmd, strReply);
            break;
        case RLV_BHVR_GETOUTFITNAMES:   // @getoutfitnames=<channel>
        case RLV_BHVR_GETADDOUTFITNAMES:// @getaddoutfitnames=<channel>
        case RLV_BHVR_GETREMOUTFITNAMES:// @getremoutfitnames=<channel>
            eRet = onGetOutfitNames(rlvCmd, strReply);
            break;
        case RLV_BHVR_FINDFOLDER:       // @findfolder:<criteria>=<channel>
        case RLV_BHVR_FINDFOLDERS:      // @findfolders:<criteria>=<channel>
            eRet = onFindFolder(rlvCmd, strReply);
            break;
        case RLV_BHVR_GETPATH:          // @getpath[:<option>]=<channel>
        case RLV_BHVR_GETPATHNEW:       // @getpathnew[:<option>]=<channel>
            eRet = onGetPath(rlvCmd, strReply);
            break;
        case RLV_BHVR_GETINV:           // @getinv[:<path>]=<channel>
            eRet = onGetInv(rlvCmd, strReply);
            break;
        case RLV_BHVR_GETINVWORN:       // @getinvworn[:<path>]=<channel>
            eRet = onGetInvWorn(rlvCmd, strReply);
            break;
        // Charis: @getgroup and @getsitid are stage 3
        case RLV_BHVR_GETSTATUS:        // @getstatus                           - Checked: 2010-04-07 (RLVa-1.2.0d) | Modified: RLVa-1.1.0f
            {
                std::string strFilter, strSeparator;
                if (rlvParseGetStatusOption(rlvCmd.getOption(), strFilter, strSeparator))
                {
                    // NOTE: specification says response should start with '/' but RLV-1.16.1 returns an empty string when no rules are set
                    rlv_object_map_t::const_iterator itObj = m_Objects.find(rlvCmd.getObjectID());
                    if (itObj != m_Objects.end())
                        strReply = itObj->second.getStatusString(strFilter, strSeparator);
                }
            }
            break;
        case RLV_BHVR_GETSTATUSALL:     // @getstatusall                        - Checked: 2010-04-07 (RLVa-1.2.0d) | Modified: RLVa-1.1.0f
            {
                std::string strFilter, strSeparator;
                if (rlvParseGetStatusOption(rlvCmd.getOption(), strFilter, strSeparator))
                {
                    // NOTE: specification says response should start with '/' but RLV-1.16.1 returns an empty string when no rules are set
                    // Charis: a reply is one chat message (MAX_MSG_STR_LEN bytes); stop building once it is that long (review S-L6)
                    for (rlv_object_map_t::const_iterator itObj = m_Objects.begin(); (itObj != m_Objects.end()) && (strReply.size() <= (size_t)MAX_MSG_STR_LEN); ++itObj)
                        strReply += itObj->second.getStatusString(strFilter, strSeparator);
                }
            }
            break;
        case RLV_BHVR_UNKNOWN:
            // Pass unknown commands on to registered command handlers
            return (notifyCommandHandlers(&RlvExtCommandHandler::onReplyCommand, rlvCmd, eRet, false)) ? eRet : RLV_RET_FAILED_UNKNOWN;
        default:
            // Fail with "Invalid param" if none of the above handled it
            return RLV_RET_FAILED_PARAM;
    }

    // If we made it this far then:
    //   - the command was handled successfully so we send off the response
    //   - the command failed but we still send off an - empty - response to keep the issuing script from blocking
    RlvUtil::sendChatReply(nChannel, strReply, true, rlvCmd.getObjectID());  // Charis: counted against the object's reply budget too

    return eRet;
}

// Checked: 2010-04-07 (RLVa-1.2.0d) | Modified: RLVa-1.1.0f
ERlvCmdRet RlvHandler::onFindFolder(const RlvCommand& rlvCmd, std::string& strReply) const
{
    RLV_ASSERT(RLV_TYPE_REPLY == rlvCmd.getParamType());
    RLV_ASSERT( (RLV_BHVR_FINDFOLDER == rlvCmd.getBehaviourType()) || (RLV_BHVR_FINDFOLDERS == rlvCmd.getBehaviourType()) );

    // (Compatibility: RLV 1.16.1 returns the first random folder it finds while we return a blank on no option)
    if (rlvCmd.getOption().empty())
        return RLV_RET_FAILED_OPTION;

    LLInventoryModel::cat_array_t folders;
    if (RlvInventory::instance().findSharedFolders(rlvCmd.getOption(), folders))
    {
        if (RLV_BHVR_FINDFOLDER == rlvCmd.getBehaviourType())
        {
            // We need to return an "in depth" result so whoever has the most '/' is our lucky winner
            // (maxSlashes needs to be initialized to -1 since children of the #RLV folder won't have '/' in their shared path)
            int maxSlashes = -1, curSlashes; std::string strFolderName;
            for (size_t idxFolder = 0, cntFolder = folders.size(); idxFolder < cntFolder; idxFolder++)
            {
                strFolderName = RlvInventory::instance().getSharedPath(folders.at(idxFolder));

                curSlashes = (int)std::count(strFolderName.begin(), strFolderName.end(), '/');
                if (curSlashes > maxSlashes)
                {
                    maxSlashes = curSlashes;
                    strReply = strFolderName;
                }
            }
        }
        else if (RLV_BHVR_FINDFOLDERS == rlvCmd.getBehaviourType())
        {
            for (size_t idxFolder = 0, cntFolder = folders.size(); idxFolder < cntFolder; idxFolder++)
            {
                if (!strReply.empty())
                    strReply.push_back(',');
                strReply += RlvInventory::instance().getSharedPath(folders.at(idxFolder));
            }
        }
    }
    return RLV_RET_SUCCESS;
}

// Checked: 2010-03-19 (RLVa-1.4.0a) | Modified: RLVa-1.1.0e
ERlvCmdRet RlvHandler::onGetAttach(const RlvCommand& rlvCmd, std::string& strReply) const
{
    RLV_ASSERT(RLV_TYPE_REPLY == rlvCmd.getParamType());
    RLV_ASSERT(RLV_BHVR_GETATTACH == rlvCmd.getBehaviourType());

    if (!isAgentAvatarValid())
        return RLV_RET_FAILED;

    // Sanity check - <option> should specify an attachment point or be empty
    S32 idxAttachPt = 0;
    if ( (rlvCmd.hasOption()) && ((idxAttachPt = RlvAttachPtLookup::getAttachPointIndex(rlvCmd.getOption())) == 0) )
        return RLV_RET_FAILED_OPTION;

    // If we're fetching all worn attachments then the reply should start with 0
    if (0 == idxAttachPt)
        strReply.push_back('0');

    for (LLVOAvatar::attachment_map_t::const_iterator itAttach = gAgentAvatarp->mAttachmentPoints.begin();
            itAttach != gAgentAvatarp->mAttachmentPoints.end(); ++itAttach)
    {
        const LLViewerJointAttachment* pAttachPt = itAttach->second;
        if ( (pAttachPt) && ((0 == idxAttachPt) || (itAttach->first == idxAttachPt)) )
        {
            // Charis: no LSL bridge to leave out (FS subtracts its bridge attachment here)
            bool fWorn = (pAttachPt->getNumObjects() > 0) &&
                ( (!RlvSettings::getHideLockedAttach()) || (RlvForceWear::isForceDetachable(pAttachPt, true, rlvCmd.getObjectID())) );
            strReply.push_back( (fWorn) ? '1' : '0' );
        }
    }
    return RLV_RET_SUCCESS;
}

// Checked: 2011-05-28 (RLVa-1.4.0a) | Modified: RLVa-1.4.0a
ERlvCmdRet RlvHandler::onGetAttachNames(const RlvCommand& rlvCmd, std::string& strReply) const
{
    RLV_ASSERT(RLV_TYPE_REPLY == rlvCmd.getParamType());
    RLV_ASSERT( (RLV_BHVR_GETATTACHNAMES == rlvCmd.getBehaviourType()) || (RLV_BHVR_GETADDATTACHNAMES == rlvCmd.getBehaviourType()) ||
                (RLV_BHVR_GETREMATTACHNAMES == rlvCmd.getBehaviourType()) );

    if (!isAgentAvatarValid())
        return RLV_RET_FAILED;

    ERlvAttachGroupType eAttachGroup = rlvAttachGroupFromString(rlvCmd.getOption());
    for (LLVOAvatar::attachment_map_t::const_iterator itAttach = gAgentAvatarp->mAttachmentPoints.begin();
            itAttach != gAgentAvatarp->mAttachmentPoints.end(); ++itAttach)
    {
        const LLViewerJointAttachment* pAttachPt = itAttach->second;
        if ( (pAttachPt) && ((RLV_ATTACHGROUP_INVALID == eAttachGroup) || (rlvAttachGroupFromIndex(pAttachPt->getGroup()) == eAttachGroup)) )
        {
            bool fAdd = false;
            switch (rlvCmd.getBehaviourType())
            {
                case RLV_BHVR_GETATTACHNAMES:       // Every attachment point that has an attached object
                    // Charis: no LSL bridge to leave out (FS subtracts its bridge attachment here)
                    fAdd = (pAttachPt->getNumObjects() > 0);
                    break;
                case RLV_BHVR_GETADDATTACHNAMES:    // Every attachment point that can be attached to (wear replace OR wear add)
                    fAdd = (gRlvAttachmentLocks.canAttach(pAttachPt) & RLV_WEAR);
                    break;
                case RLV_BHVR_GETREMATTACHNAMES:    // Every attachment point that has at least one attachment that can be force-detached
                    fAdd = RlvForceWear::isForceDetachable(pAttachPt);
                    break;
                default:
                    break;
            }

            if (fAdd)
            {
                if (!strReply.empty())
                    strReply.push_back(',');
                strReply.append(pAttachPt->getName());
            }
        }
    }
    return RLV_RET_SUCCESS;
}

// Handles: @getcommand[:<behaviour>[;<type>[;<separator>]]]=<channel>
template<> template<>
ERlvCmdRet RlvReplyHandler<RLV_BHVR_GETCOMMAND>::onCommand(const RlvCommand& rlvCmd, std::string& strReply)
{
    std::vector<std::string> optionList;
    RlvCommandOptionHelper::parseStringList(rlvCmd.getOption(), optionList);

    // If a second parameter is present it'll specify the command type
    ERlvParamType eType = RLV_TYPE_UNKNOWN;
    if (optionList.size() >= 2)
    {
        if ( (optionList[1] == "any") || (optionList[1].empty()) )
            eType = RLV_TYPE_UNKNOWN;
        else if (optionList[1] == "add")
            eType = RLV_TYPE_ADDREM;
        else if (optionList[1] == "force")
            eType = RLV_TYPE_FORCE;
        else if (optionList[1] == "reply")
            eType = RLV_TYPE_REPLY;
        else
            return RLV_RET_FAILED_OPTION;
    }

    std::list<std::string> cmdList;
    if (RlvBehaviourDictionary::instance().getCommands((optionList.size() >= 1) ? optionList[0] : LLStringUtil::null, eType, cmdList))
        strReply = boost::algorithm::join(cmdList, (optionList.size() >= 3) ? optionList[2] : std::string(RLV_OPTION_SEPARATOR) );
    return RLV_RET_SUCCESS;
}

// Checked: 2010-03-09 (RLVa-1.2.0a) | Modified: RLVa-1.1.0f
ERlvCmdRet RlvHandler::onGetInv(const RlvCommand& rlvCmd, std::string& strReply) const
{
    RLV_ASSERT(RLV_TYPE_REPLY == rlvCmd.getParamType());
    RLV_ASSERT(RLV_BHVR_GETINV == rlvCmd.getBehaviourType());

    const LLViewerInventoryCategory* pFolder = RlvInventory::instance().getSharedFolder(rlvCmd.getOption());
    if (!pFolder)
        return (RlvInventory::instance().getSharedRoot() != NULL) ? RLV_RET_FAILED_OPTION : RLV_RET_FAILED_NOSHAREDROOT;

    LLInventoryModel::cat_array_t* pFolders; LLInventoryModel::item_array_t* pItems;
    gInventory.getDirectDescendentsOf(pFolder->getUUID(), pFolders, pItems);
    if (!pFolders)
        return RLV_RET_FAILED;

    for (size_t idxFolder = 0, cntFolder = pFolders->size(); idxFolder < cntFolder; idxFolder++)
    {
        // Return all folders that:
        //   - aren't hidden
        //   - aren't a folded folder (only really matters when "Enable Legacy Naming" is enabled - see related blog post)
        //     (we can skip checking for .<composite> folders since the ones we'll want to hide start with '.' anyway)
        //   - don't have any invalid characters
        const std::string& strFolder = pFolders->at(idxFolder)->getName();
        if ( (!strFolder.empty()) && (RLV_FOLDER_PREFIX_HIDDEN != strFolder[0]) &&
             (!RlvInventory::isFoldedFolder(pFolders->at(idxFolder).get(), false)) &&
             (std::string::npos == strFolder.find_first_of(RLV_FOLDER_INVALID_CHARS)) )
        {
            if (!strReply.empty())
                strReply.push_back(',');
            strReply += strFolder;
        }
    }
    return RLV_RET_SUCCESS;
}

struct rlv_wear_info { U32 cntWorn, cntTotal, cntChildWorn, cntChildTotal; };

// Checked: 2010-04-05 (RLVa-1.2.0d) | Modified: RLVa-1.1.0f
ERlvCmdRet RlvHandler::onGetInvWorn(const RlvCommand& rlvCmd, std::string& strReply) const
{
    // Sanity check - gAgentAvatarp can't be NULL [see RlvForceWear::isWearingItem()]
    if (!isAgentAvatarValid())
        return RLV_RET_FAILED;
    // Sanity check - folder should exist
    LLViewerInventoryCategory* pFolder = RlvInventory::instance().getSharedFolder(rlvCmd.getOption());
    if (!pFolder)
        return (RlvInventory::instance().getSharedRoot() != NULL) ? RLV_RET_FAILED_OPTION : RLV_RET_FAILED_NOSHAREDROOT;

    // Collect everything @attachall would be attaching
    LLInventoryModel::cat_array_t folders; LLInventoryModel::item_array_t items;
    RlvWearableItemCollector f(pFolder, RlvForceWear::ACTION_WEAR_REPLACE, RlvForceWear::FLAG_MATCHALL);
    RlvInventory::collectDescendentsIf(pFolder->getUUID(), folders, items, false, f, true);  // Charis: the official one has no follow_folder_links

    rlv_wear_info wi = {0};

    // Add all the folders to a lookup map
    std::map<LLUUID, rlv_wear_info> mapFolders;
    mapFolders.insert(std::pair<LLUUID, rlv_wear_info>(pFolder->getUUID(), wi));
    for (size_t idxFolder = 0, cntFolder = folders.size(); idxFolder < cntFolder; idxFolder++)
        mapFolders.insert(std::pair<LLUUID, rlv_wear_info>(folders.at(idxFolder)->getUUID(), wi));

    // Iterate over all the found items
    LLViewerInventoryItem* pItem; std::map<LLUUID, rlv_wear_info>::iterator itFolder;
    for (size_t idxItem = 0, cntItem = items.size(); idxItem < cntItem; idxItem++)
    {
        pItem = items.at(idxItem);
        if (!RlvForceWear::isWearableItem(pItem))
            continue;

        // The "folded parent" is the folder this item should be considered a direct descendent of (may or may not match actual parent)
        const LLUUID& idParent = f.getFoldedParent(pItem->getParentUUID());

        // Walk up the tree: sooner or later one of the parents will be a folder in the map
        // Charis: stop on a missing category instead of dereferencing it (inventory still loading or changing under us)
        LLViewerInventoryCategory* pParent = gInventory.getCategory(idParent); S32 nDepth = 0;
        while ( (pParent) && ((itFolder = mapFolders.find(pParent->getUUID())) == mapFolders.end()) )
            pParent = (++nDepth <= RlvInventory::cMaxFolderDepth) ? gInventory.getCategory(pParent->getParentUUID()) : nullptr;
        if (!pParent)
            continue;

        U32 &cntWorn  = (idParent == pParent->getUUID()) ? itFolder->second.cntWorn : itFolder->second.cntChildWorn,
            &cntTotal = (idParent == pParent->getUUID()) ? itFolder->second.cntTotal : itFolder->second.cntChildTotal;

        if (RlvForceWear::isWearingItem(pItem))
            cntWorn++;
        cntTotal++;
    }

    // Extract the result for the main folder
    itFolder = mapFolders.find(pFolder->getUUID());
    wi.cntWorn = itFolder->second.cntWorn;
    wi.cntTotal = itFolder->second.cntTotal;
    mapFolders.erase(itFolder);

    // Build the result for each child folder
    for (itFolder = mapFolders.begin(); itFolder != mapFolders.end(); ++itFolder)
    {
        rlv_wear_info& wiFolder = itFolder->second;

        wi.cntChildWorn += wiFolder.cntWorn + wiFolder.cntChildWorn;
        wi.cntChildTotal += wiFolder.cntTotal + wiFolder.cntChildTotal;

        strReply += llformat(",%s|%d%d", gInventory.getCategory(itFolder->first)->getName().c_str(),
         (0 == wiFolder.cntTotal) ? 0 : (0 == wiFolder.cntWorn) ? 1 : (wiFolder.cntWorn != wiFolder.cntTotal) ? 2 : 3,
         (0 == wiFolder.cntChildTotal) ? 0 : (0 == wiFolder.cntChildWorn) ? 1 : (wiFolder.cntChildWorn != wiFolder.cntChildTotal) ? 2 : 3
        );
    }

    // Now just prepend the root and done
    strReply = llformat("|%d%d", (0 == wi.cntTotal) ? 0 : (0 == wi.cntWorn) ? 1 : (wi.cntWorn != wi.cntTotal) ? 2 : 3,
        (0 == wi.cntChildTotal) ? 0 : (0 == wi.cntChildWorn) ? 1 : (wi.cntChildWorn != wi.cntChildTotal) ? 2: 3) + strReply;

    return RLV_RET_SUCCESS;
}

// Checked: 2010-03-19 (RLVa-1.4.0a) | Modified: RLVa-1.2.0a
ERlvCmdRet RlvHandler::onGetOutfit(const RlvCommand& rlvCmd, std::string& strReply) const
{
    RLV_ASSERT(RLV_TYPE_REPLY == rlvCmd.getParamType());
    RLV_ASSERT(RLV_BHVR_GETOUTFIT == rlvCmd.getBehaviourType());

    // (Compatibility: RLV-1.16.1 will execute @getoutfit=<channel> if <layer> is invalid while we just return failure)
    LLWearableType::EType wtType = LLWearableType::WT_INVALID;
    if ( (rlvCmd.hasOption()) && ((wtType = rlvTypeNameToType(rlvCmd.getOption())) == LLWearableType::WT_INVALID) )
        return RLV_RET_FAILED_OPTION;

    const LLWearableType::EType wtRlvTypes[] =
        {
            LLWearableType::WT_GLOVES, LLWearableType::WT_JACKET, LLWearableType::WT_PANTS, LLWearableType::WT_SHIRT,
            LLWearableType::WT_SHOES, LLWearableType::WT_SKIRT, LLWearableType::WT_SOCKS, LLWearableType::WT_UNDERPANTS,
            LLWearableType::WT_UNDERSHIRT, LLWearableType::WT_SKIN, LLWearableType::WT_EYES, LLWearableType::WT_HAIR,
            LLWearableType::WT_SHAPE, LLWearableType::WT_ALPHA, LLWearableType::WT_TATTOO, LLWearableType::WT_PHYSICS,
            LLWearableType::WT_UNIVERSAL,
        };

    for (int idxType = 0, cntType = sizeof(wtRlvTypes) / sizeof(LLWearableType::EType); idxType < cntType; idxType++)
    {
        if ( (LLWearableType::WT_INVALID == wtType) || (wtRlvTypes[idxType] == wtType) )
        {
            // We never hide body parts, even if they're "locked" and we're hiding locked layers
            // (nor do we hide a layer if the issuing object is the only one that has this layer locked)
            bool fWorn = (gAgentWearables.getWearableCount(wtRlvTypes[idxType]) > 0) &&
                ( (!RlvSettings::getHideLockedLayers()) ||
                  (LLAssetType::AT_BODYPART == LLWearableType::getInstance()->getAssetType(wtRlvTypes[idxType])) ||
                  (RlvForceWear::isForceRemovable(wtRlvTypes[idxType], true, rlvCmd.getObjectID())) );
            strReply.push_back( (fWorn) ? '1' : '0' );
        }
    }
    return RLV_RET_SUCCESS;
}

// Checked: 2011-05-28 (RLVa-1.4.0a) | Modified: RLVa-1.4.0a
ERlvCmdRet RlvHandler::onGetOutfitNames(const RlvCommand& rlvCmd, std::string& strReply) const
{
    RLV_ASSERT(RLV_TYPE_REPLY == rlvCmd.getParamType());
    RLV_ASSERT( (RLV_BHVR_GETOUTFITNAMES == rlvCmd.getBehaviourType()) || (RLV_BHVR_GETADDOUTFITNAMES == rlvCmd.getBehaviourType()) ||
                (RLV_BHVR_GETREMOUTFITNAMES == rlvCmd.getBehaviourType()) );

    // Sanity check - all these commands are optionless
    if (rlvCmd.hasOption())
        return RLV_RET_FAILED_OPTION;

    for (int idxType = 0; idxType < LLWearableType::WT_COUNT; idxType++)
    {
        bool fAdd = false; LLWearableType::EType wtType = (LLWearableType::EType)idxType;
        switch (rlvCmd.getBehaviourType())
        {
            case RLV_BHVR_GETOUTFITNAMES:       // Every layer that has at least one worn wearable
                fAdd = (gAgentWearables.getWearableCount(wtType) > 0);
                break;
            case RLV_BHVR_GETADDOUTFITNAMES:    // Every layer that can be worn on (wear replace OR wear add)
                fAdd = (gRlvWearableLocks.canWear(wtType) & RLV_WEAR);
                break;
            case RLV_BHVR_GETREMOUTFITNAMES:    // Every layer that has at least one wearable that can be force-removed
                fAdd = RlvForceWear::isForceRemovable(wtType);
                break;
            default:
                break;
        }

        if (fAdd)
        {
            if (!strReply.empty())
                strReply.push_back(',');
            strReply.append(LLWearableType::getInstance()->getTypeName(wtType));
        }
    }
    return RLV_RET_SUCCESS;
}

// Checked: 2010-08-30 (RLVa-1.2.1c) | Modified: RLVa-1.2.1c
ERlvCmdRet RlvHandler::onGetPath(const RlvCommand& rlvCmd, std::string& strReply) const
{
    RLV_ASSERT(RLV_TYPE_REPLY == rlvCmd.getParamType());
    RLV_ASSERT( (RLV_BHVR_GETPATH == rlvCmd.getBehaviourType()) || (RLV_BHVR_GETPATHNEW == rlvCmd.getBehaviourType()) );

    RlvCommandOptionGetPath rlvGetPathOption(rlvCmd);
    if (!rlvGetPathOption.isValid())
        return RLV_RET_FAILED_OPTION;

    LLInventoryModel::cat_array_t folders;
    if (RlvInventory::instance().getPath(rlvGetPathOption.getItemIDs(), folders))
    {
        if (RLV_BHVR_GETPATH == rlvCmd.getBehaviourType())
        {
            strReply = RlvInventory::instance().getSharedPath(folders.at(0));
        }
        else if (RLV_BHVR_GETPATHNEW == rlvCmd.getBehaviourType())
        {
            for (size_t idxFolder = 0, cntFolder = folders.size(); idxFolder < cntFolder; idxFolder++)
            {
                if (!strReply.empty())
                    strReply.push_back(',');
                strReply += RlvInventory::instance().getSharedPath(folders.at(idxFolder));
            }
        }
    }
    return RLV_RET_SUCCESS;
}

