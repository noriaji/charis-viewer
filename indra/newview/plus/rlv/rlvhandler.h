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

#ifndef RLV_HANDLER_H
#define RLV_HANDLER_H

#include <stack>

#include "rlvcommon.h"
#include "rlvhelper.h"

class LLViewerJointAttachment;
class LLViewerObject;

// Charis: moved from FS rlvfloaters.h (the RLV floaters are stage 3; @list / @except still format the restrictions)
enum class ERlvBehaviourFilter {
    BEHAVIOURS_ONLY,
    EXCEPTIONS_ONLY,
    ALL
};

// ============================================================================
// RlvHandler class
//

// Charis: no LLOldEvents::LLSimpleListener / LLParticularGroupObserver bases (they served @setgroup, which is stage 3)
class RlvHandler
{
    // Temporary LLSingleton look-alike
public:
    static RlvHandler& instance();
    static RlvHandler* getInstance();

public:
    RlvHandler();
    ~RlvHandler();

    // --------------------------------

    /*
     * Rule checking functions
     */
    // NOTE: - to check @detach=n    -> (see RlvAttachmentLocks)
    //       - to check @addattach=n -> (see RlvAttachmentLocks)
    //       - to check @remattach=n -> (see RlvAttachmentLocks)
    //       - to check @addoutfit=n -> (see RlvWearableLocks)
    //       - to check @remoutfit=n -> (see RlvWearableLocks)
    //       - to check exceptions   -> isException()
public:
    // Returns a list of all objects containing the specified behaviour
    bool findBehaviour(ERlvBehaviour eBhvr, std::list<const RlvObject*>& lObjects) const;
    // Returns a pointer to an RLV object instance (DO NOT STORE THIS!)
    RlvObject* getObject(const LLUUID& idRlvObj) const;
    // Returns true is at least one object contains the specified behaviour (and optional option)
    bool hasBehaviour(ERlvBehaviour eBhvr) const { return (eBhvr < RLV_BHVR_COUNT) ? (0 != m_Behaviours[eBhvr]) : false; }
    bool hasBehaviour(ERlvBehaviour eBhvr, const std::string& strOption) const;
    // Returns true if the specified object has at least one active behaviour
    bool hasBehaviour(const LLUUID& idObj) { return m_Objects.end() != m_Objects.find(idObj);  }
    // Returns true if the specified object contains the specified behaviour (and optional option)
    bool hasBehaviour(const LLUUID& idObj, ERlvBehaviour eBhvr, const std::string& strOption = LLStringUtil::null) const;
    // Returns true if at least one object (except the specified one) contains the specified behaviour (and optional option)
    bool hasBehaviourExcept(ERlvBehaviour eBhvr, const LLUUID& idObj) const;
    bool hasBehaviourExcept(ERlvBehaviour eBhvr, const std::string& strOption, const LLUUID& idObj) const;
    // Returns true if at least one object in the linkset with specified root ID contains the specified behaviour (and optional option)
    bool hasBehaviourRoot(const LLUUID& idObjRoot, ERlvBehaviour eBhvr, const std::string& strOption = LLStringUtil::null) const;
    // Returns true if the specified object is the only object containing the specified behaviour
    bool ownsBehaviour(const LLUUID& idObj, ERlvBehaviour eBhvr) const;

    // Adds or removes an exception for the specified behaviour
    void addException(const LLUUID& idObj, ERlvBehaviour eBhvr, const RlvExceptionOption& varOption);
    void removeException(const LLUUID& idObj, ERlvBehaviour eBhvr, const RlvExceptionOption& varOption);
    // Returns true if the specified behaviour has an added exception
    bool hasException(ERlvBehaviour eBhvr) const;
    // Returns true if the specified option was added as an exception for the specified behaviour
    bool isException(ERlvBehaviour eBhvr, const RlvExceptionOption& varOption, ERlvExceptionCheck eCheckType = ERlvExceptionCheck::Default) const;
    // Returns true if the specified behaviour should behave "permissive" (rather than "strict"/"secure")
    bool isPermissive(ERlvBehaviour eBhvr) const;

    // Charis: the RLV_EXPERIMENTAL_COMPOSITEFOLDERS functions are left out (the define is off in RLVa)

public:
    // Adds a blocked object (= object that is blocked from issuing commands) by UUID (can be null) and/or name
    void addBlockedObject(const LLUUID& idObj, const std::string& strObjName);
    // Returns true if there's an unresolved blocked object (known name but unknown UUID)
    bool hasUnresolvedBlockedObject() const;
    // Returns true if the object with the specified UUID is blocked from issuing commands
    bool isBlockedObject(const LLUUID& idObj) const;
    // Charis: ... or the root of its linkset is
    bool isBlockedObjectOrRoot(const LLUUID& idObj) const;
    // Removes a blocked object
    void removeBlockedObject(const LLUUID& idObj);

    // --------------------------------

    /*
     * Helper functions
     */
public:
    // Accessors/Mutators
    bool              getCanCancelTp() const        { return m_fCanCancelTp; }                  // @accepttp and @tpto
    void              setCanCancelTp(bool fAllow)   { m_fCanCancelTp = fAllow; }                // @accepttp and @tpto
    const LLVector3d& getSitSource() const                      { return m_posSitSource; }      // @standtp
    void              setSitSource(const LLVector3d& posSource) { m_posSitSource = posSource; } // @standtp

    // Command specific helper functions
    bool filterChat(std::string& strUTF8Text, bool fFilterEmote) const;                         // @sendchat, @recvchat and @redirchat
    bool redirectChatOrEmote(const std::string& strUTF8Test) const;                             // @redirchat and @rediremote

    // @list / @except: the current restrictions (or exceptions) as text (FS RlvFloaterBehaviours::getFormattedBehaviourString())
    std::string getFormattedBehaviourString(ERlvBehaviourFilter eFilter) const;

    // Command processing helper functions
    ERlvCmdRet processCommand(const LLUUID& idObj, const std::string& strCommand, bool fFromObj);
    void       processRetainedCommands(ERlvBehaviour eBhvrFilter = RLV_BHVR_UNKNOWN, ERlvParamType eTypeFilter = RLV_TYPE_UNKNOWN);
    // IM queries (@version, @stopim, @list, @except) in a peer-to-peer IM from an avatar; fOnline = (offline == IM_ONLINE).
    // True: the IM was a query and was answered (don't show it). The sender checks of FS's call site are done inside.
    bool       processIMQuery(const LLUUID& idSender, const std::string& strSenderName, const std::string& strCommand, bool fOnline);

    // Charis: llOwnerSay command lines (rlvhooks_core.cpp handleOwnerSay(), review S-M1)
    enum class ERlvCmdOrigin
    {
        Allowed,        // run the commands now
        Deferred,       // the object isn't known as ours yet: deferCommandLine()
        NotCommand,     // temporary attachments are off (RLVaEnableTemporaryAttachments): show the line as chat (FS)
        Blocked,        // a refused Experience's temporary attachment: drop the line
    };
    ERlvCmdOrigin checkCommandOrigin(const LLUUID& idObj, const std::string& strObjName, const LLViewerObject* pChatter);
    // Keeps a line until its object is known (RLV_DEFER_TIMEOUT), behind the object's earlier waiting lines; =y / =rem and
    // @clear in it run at once and take back the matching waiting adds. strCommands: the line without the '@', lower case.
    // (checkCommandOrigin() runs an object's waiting lines first once the object may run commands.)
    void          deferCommandLine(const LLUUID& idObj, const std::string& strObjName, const std::string& strCommands);
    // Runs only the =y / =rem and @clear of a line (also for Blocked / NotCommand lines: taking back always goes through);
    // returns the line's other commands, parsed
    std::vector<RlvCommand> runTakeBackCommands(const LLUUID& idObj, const std::string& strCommands);

    // rlv-locks: called after the commands of one llOwnerSay line ran (RlvForceWear::done() there)
    typedef void (*command_batch_done_fn)();
    static void setCommandBatchDone(command_batch_done_fn fn) { s_pfnCommandBatchDone = fn; }
    static void onCommandBatchDone();

    // Charis: updates RLVaLoginLastLocation a few seconds later (many sit / stand / @tploc changes in a row save the
    //         per-account settings file once)
    void scheduleLoginLastLocationUpdate();

    // Returns a pointer to the currently executing command (do *not* save this pointer)
    const RlvCommand* getCurrentCommand() const { return (!m_CurCommandStack.empty()) ? &m_CurCommandStack.top().get() : nullptr; }
    // Returns the UUID of the object we're currently executing a command for
    const LLUUID&     getCurrentObject() const  { return (!m_CurObjectStack.empty()) ? m_CurObjectStack.top() : LLUUID::null; }

    // Initialization
    static bool canEnable();
    static bool isEnabled() { return m_fEnabled; }
    static bool setEnabled(bool fEnable);
protected:
    // Charis: no @setgroup / @setcam helpers (stage 3)
    void onIMQueryListResponse(const LLSD& sdNotification, const LLSD sdResponse);

    // Charis: limits on what our own objects can make the viewer do (research/248 section 12)
    ERlvCmdRet checkCommandLimits(const RlvCommand& rlvCmd);
    void       logLimit(const LLUUID& idObj, const char* pstrReason);
    void       pruneThrottles();
    bool       removeRetainedCommands(const RlvCommand& rlvCmd);
    bool       onDeferredLinesTimer();
    ERlvCmdOrigin decideCommandOrigin(const LLUUID& idObj, const std::string& strObjName, const LLViewerObject* pChatter);
    void       clearLinkset(const LLUUID& idRoot, const LLUUID& idObj);
    bool       hasDeferredLines(const LLUUID& idObj) const;
    void       runDeferredLines(const LLUUID& idObj);
    void       dropDeferredLines(const LLUUID& idObj);
    void       takeBackDeferredCommands(const RlvCommand& rlvCmdTakeBack);
    void       onExperienceEventTracked(const LLSD& sdEvent);
    bool       hasPendingExperienceLookup();

    // --------------------------------

    /*
     * Event handling
     */
public:
    // The behaviour signal is triggered whenever a command is successfully processed and resulted in adding or removing a behaviour
    typedef boost::signals2::signal<void (ERlvBehaviour, ERlvParamType)> rlv_behaviour_signal_t;
    boost::signals2::connection setBehaviourCallback(const rlv_behaviour_signal_t::slot_type& cb )       { return m_OnBehaviour.connect(cb); }
    boost::signals2::connection setBehaviourToggleCallback(const rlv_behaviour_signal_t::slot_type& cb ) { return m_OnBehaviourToggle.connect(cb); }
    // The command signal is triggered whenever a command is processed
    typedef boost::signals2::signal<void (const RlvCommand&, ERlvCmdRet, bool)> rlv_command_signal_t;
    boost::signals2::connection setCommandCallback(const rlv_command_signal_t::slot_type& cb )           { return m_OnCommand.connect(cb); }
    // Charis: the exception signal is triggered whenever a <uuid> exception is added or removed (@shownames:<uuid>, @recvim:<uuid>, ...)
    //         so UI parts can refresh what depends on that one avatar / object (FS does those refreshes in the behaviour handlers)
    typedef boost::signals2::signal<void (ERlvBehaviour, const LLUUID&, ERlvParamType)> rlv_exception_signal_t;
    boost::signals2::connection setExceptionCallback(const rlv_exception_signal_t::slot_type& cb )       { return m_OnException.connect(cb); }

    void addCommandHandler(RlvExtCommandHandler* pHandler);
    void removeCommandHandler(RlvExtCommandHandler* pHandler);
protected:
    void clearCommandHandlers();
    bool notifyCommandHandlers(rlvExtCommandHandler f, const RlvCommand& rlvCmd, ERlvCmdRet& eRet, bool fNotifyAll) const;

    // Externally invoked event handlers
public:
    void cleanup();
    void onAttach(const LLViewerObject* pAttachObj, const LLViewerJointAttachment* pAttachPt);
    void onDetach(const LLViewerObject* pAttachObj, const LLViewerJointAttachment* pAttachPt);
    void onExperienceAttach(const LLSD& sdExperience, const std::string& strObjName);
    void onExperienceEvent(const LLSD& sdEvent);
    bool onGC();
    void onLoginComplete();
    void onSitOrStand(bool fSitting);
    void onTeleportFailed();
    void onTeleportFinished(const LLVector3d& posArrival);
    static void cleanupClass();
    static void onIdleStartup(void* pParam);
protected:
    void getAttachmentResourcesCoro(const std::string& strUrl);
    void onTeleportCallback(U64 hRegion, const LLVector3& posRegion, const LLVector3& vecLookAt, const LLUUID& idRlvObj);

    /*
     * Command processing
     */
protected:
    ERlvCmdRet processCommand(std::reference_wrapper<const RlvCommand> rlvCmdRef, bool fFromObj);
    ERlvCmdRet processClearCommand(const RlvCommand& rlvCmd);

    // Command handlers (RLV_TYPE_ADD and RLV_TYPE_CLEAR)
    ERlvCmdRet processAddRemCommand(const RlvCommand& rlvCmd);
    ERlvCmdRet onAddRemFolderLock(const RlvCommand& rlvCmd, bool& fRefCount);
    ERlvCmdRet onAddRemFolderLockException(const RlvCommand& rlvCmd, bool& fRefCount);
    // Command handlers (RLV_TYPE_FORCE)
    ERlvCmdRet processForceCommand(const RlvCommand& rlvCmd) const;
    ERlvCmdRet onForceWear(const LLViewerInventoryCategory* pFolder, U32 nFlags) const;
    void       onForceWearCallback(const uuid_vec_t& idItems, U32 nFlags, const LLUUID& idRlvObj) const;
    // Command handlers (RLV_TYPE_REPLY)
    ERlvCmdRet processReplyCommand(const RlvCommand& rlvCmd) const;
    ERlvCmdRet onFindFolder(const RlvCommand& rlvCmd, std::string& strReply) const;
    ERlvCmdRet onGetAttach(const RlvCommand& rlvCmd, std::string& strReply) const;
    ERlvCmdRet onGetAttachNames(const RlvCommand& rlvCmd, std::string& strReply) const;
    ERlvCmdRet onGetInv(const RlvCommand& rlvCmd, std::string& strReply) const;
    ERlvCmdRet onGetInvWorn(const RlvCommand& rlvCmd, std::string& strReply) const;
    ERlvCmdRet onGetOutfit(const RlvCommand& rlvCmd, std::string& strReply) const;
    ERlvCmdRet onGetOutfitNames(const RlvCommand& rlvCmd, std::string& strReply) const;
    ERlvCmdRet onGetPath(const RlvCommand& rlvCmd, std::string& strReply) const;

    // --------------------------------

    /*
     * Member variables
     */
public:
    typedef std::map<LLUUID, RlvObject> rlv_object_map_t;
    typedef std::tuple<LLUUID, std::string, double> rlv_blocked_object_t;
    typedef std::list<rlv_blocked_object_t> rlv_blocked_object_list_t;

    struct RlvException
    {
    public:
        LLUUID              idObject;    // UUID of the object that added the exception
        ERlvBehaviour       eBehaviour;  // Behaviour the exception applies to
        RlvExceptionOption  varOption;   // Exception data (type is dependent on eBehaviour)

        RlvException(const LLUUID& idObj, ERlvBehaviour eBhvr, const RlvExceptionOption& option) : idObject(idObj), eBehaviour(eBhvr), varOption(option) {}
    private:
        RlvException();
    };
    typedef std::multimap<ERlvBehaviour, RlvException> rlv_exception_map_t;
protected:
    rlv_object_map_t      m_Objects;                // Map of objects that have active restrictions (idObj -> RlvObject)
    rlv_blocked_object_list_t m_BlockedObjects;     // List of (attached) objects that can't issue commands
    rlv_exception_map_t   m_Exceptions;             // Map of currently active restriction exceptions (ERlvBehaviour -> RlvException)
    S32                   m_Behaviours[RLV_BHVR_COUNT];

    rlv_command_list_t    m_Retained;
    bool                  m_fReplayingRetained = false; // Charis: processRetainedCommands() is running (no limits, F-M1)
    bool                  m_fInCleanup = false;     // Charis: cleanup() is clearing the objects
    RlvGCTimer*           m_pGCTimer;

    // Charis: per-object command rate (research/248 section 12 #4) and the last "limit reached" log line per object
    struct RlvCommandThrottle
    {
        F64 tmSecondStart = 0.0;
        U32 nCommands = 0;
        F64 tmLastLog = 0.0;
    };
    std::map<LLUUID, RlvCommandThrottle> m_CommandThrottle;

    // Charis: llOwnerSay lines waiting for their object (deferCommandLine())
    struct RlvDeferredLine
    {
        LLUUID      idObject;
        std::string strObjName;
        std::vector<RlvCommand> commands;    // parsed once when the line is kept
        F64         tmQueued;
    };
    std::list<RlvDeferredLine> m_DeferredLines;
    bool                       m_fDeferredTimer = false;
    bool                       m_fLoginLocationPending = false;
    static command_batch_done_fn s_pfnCommandBatchDone;
    std::vector<std::pair<LLUUID, F64>> m_PendingExperienceLookups;   // Charis: Experience attaches being looked up
    std::map<LLUUID, F64>      m_PendingListQueries;   // Charis: residents with an open @list / @except confirmation
    std::map<LLUUID, U32>      m_NotifyCounts;         // Charis: @notify registrations per object
    U32                        m_nNotifyTotal = 0;

    std::stack<std::reference_wrapper<const RlvCommand>> m_CurCommandStack; // Convenience (see @tpto)
    std::stack<LLUUID>    m_CurObjectStack;         // Convenience (see @tpto)

    rlv_behaviour_signal_t m_OnBehaviour;
    rlv_behaviour_signal_t m_OnBehaviourToggle;
    rlv_command_signal_t   m_OnCommand;
    rlv_exception_signal_t m_OnException;
    mutable std::list<RlvExtCommandHandler*> m_CommandHandlers;
    boost::signals2::scoped_connection       m_ExperienceEventConn;
    boost::signals2::scoped_connection       m_TeleportFailedConn;
    boost::signals2::scoped_connection       m_TeleportFinishedConn;

    static bool         m_fEnabled;                 // Use setEnabled() to toggle this

    bool                                    m_fCanCancelTp;                 // @accepttp=n and @tpto=force
    mutable LLVector3d                      m_posSitSource;                 // @standtp=n (mutable because onForceXXX handles are all declared as const)
    bool                                    m_fPendingGroundSit = false;    // @sitground=force
    LLUUID                                  m_idPendingSitActor;            // @sit=force and @sitground=force
    LLUUID                                  m_idPendingUnsitActor;          // @unsit=force
    // Charis: no @setgroup / @setcam state (stage 3)

    friend class RlvSharedRootFetcher;              // Fetcher needs access to m_fFetchComplete
    friend class RlvGCTimer;                        // Timer clear its own point at destruction
    template<ERlvBehaviourOptionType> friend struct RlvBehaviourGenericHandler;
    template<ERlvParamType> friend struct RlvCommandHandlerBaseImpl;
    template<ERlvParamType, ERlvBehaviour> friend struct RlvCommandHandler;
    template<ERlvBehaviourModifier> friend class RlvBehaviourModifierHandler;

    // --------------------------------

    /*
     * Internal access functions
     */
public:
    const rlv_object_map_t& getObjectMap() const { return m_Objects; }
};

typedef RlvHandler rlv_handler_t;
extern rlv_handler_t gRlvHandler;

// ============================================================================
// Inlined member functions
//

inline RlvHandler& RlvHandler::instance()
{
    return gRlvHandler;
}

inline RlvHandler* RlvHandler::getInstance()
{
    return &gRlvHandler;
}

inline RlvObject* RlvHandler::getObject(const LLUUID& idRlvObj) const
{
    auto itObj = m_Objects.find(idRlvObj);
    return (m_Objects.end() != itObj) ? const_cast<RlvObject*>(&itObj->second) : nullptr;
}

inline bool RlvHandler::hasBehaviour(ERlvBehaviour eBhvr, const std::string& strOption) const
{
    return hasBehaviourExcept(eBhvr, strOption, LLUUID::null);
}

inline bool RlvHandler::hasBehaviourExcept(ERlvBehaviour eBhvr, const LLUUID& idObj) const
{
    return hasBehaviourExcept(eBhvr, LLStringUtil::null, idObj);
}

// ============================================================================

#endif // RLV_HANDLER_H
