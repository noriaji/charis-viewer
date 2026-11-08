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

#pragma once

#include "indra_constants.h"
#include "llavatarname.h"
#include "llselectmgr.h"
#include "lltrans.h"
#include "llviewercontrol.h"

#include "rlvdefines.h"

#ifdef LL_WINDOWS
    #pragma warning (push)
    #pragma warning (disable : 4702) // warning C4702: unreachable code
#endif
#include <boost/variant.hpp>
#ifdef LL_WINDOWS
    #pragma warning (pop)
#endif

// ============================================================================
// Forward declarations
//

//
// General viewer source
//
class LLInventoryItem;
class LLUICtrl;
class LLViewerInventoryCategory;
class LLViewerInventoryItem;
class LLViewerJointAttachment;
class LLViewerWearable;
class LLWearable;

//
// RLVa-specific
//
class RlvCommand;
typedef std::list<RlvCommand> rlv_command_list_t;
class RlvObject;

struct RlvException;
typedef boost::variant<std::string, LLUUID, S32, ERlvBehaviour> RlvExceptionOption;
typedef boost::variant<int, float, bool, LLVector3, LLVector3d, LLVector4, LLUUID> RlvBehaviourModifierValue;

class RlvGCTimer;

// Limits shared by rlvcommon.cpp and rlvhandler.cpp (research/248 section 12)
constexpr F64    RLV_AUTORESPONSE_INTERVAL = 60.0;       // seconds between two automatic IM responses to the same avatar
constexpr size_t RLV_MAX_IMQUERY_LIST_MESSAGES = 8;      // IMs in one @list / @except reply
constexpr U32    RLV_MAX_REPLIES_PER_SECOND = 100;       // chat replies (sendChatReply) per second, all objects together
constexpr U32    RLV_MAX_OBJECT_REPLIES_PER_SECOND = 20; // chat replies per second for one object (when the caller names it)

// Indices into SCRIPT_PERMISSIONS (llscriptruntimeperms.h) that FS names there and the official header doesn't
constexpr S32 RLV_SCRIPT_PERMISSION_TAKE_CONTROLS = 1;      // "ActOnControlInputs"
constexpr S32 RLV_SCRIPT_PERMISSION_ATTACH = 4;             // "AttachToYourAvatar"
constexpr S32 RLV_SCRIPT_PERMISSION_TELEPORT = 11;          // "TeleportYourAgent"

// ============================================================================
// utf8str_split - splits a long UTF-8 string into pieces of at most maxlen bytes, preferring to
// break at split_token and never breaking a multi-byte character (FS adds this to llcommon/llstring;
// Charis keeps it here so llcommon stays untouched)
//

void utf8str_split(std::list<std::string>& split_list, const std::string& utf8str, size_t maxlen, char split_token);

// ============================================================================
// RlvSettings
//

#ifdef CATZNIP_STRINGVIEW
template<typename T> inline T rlvGetSetting(const std::string_view& strSetting, const T& defaultValue)
#else
template<typename T> inline T rlvGetSetting(const std::string& strSetting, const T& defaultValue)
#endif // CATZNIP_STRINGVIEW
{
    RLV_ASSERT_DBG(gSavedSettings.controlExists(strSetting));
    return (gSavedSettings.controlExists(strSetting)) ? gSavedSettings.get<T>(strSetting) : defaultValue;
}

#ifdef CATZNIP_STRINGVIEW
template<typename T> inline T rlvGetPerUserSetting(const std::string_view& strSetting, const T& defaultValue)
#else
template<typename T> inline T rlvGetPerUserSetting(const std::string& strSetting, const T& defaultValue)
#endif // CATZNIP_STRINGVIEW
{
    RLV_ASSERT_DBG(gSavedPerAccountSettings.controlExists(strSetting));
    return (gSavedPerAccountSettings.controlExists(strSetting)) ? gSavedPerAccountSettings.get<T>(strSetting) : defaultValue;
}

// All getters return a cached copy (read once in initClass() and kept current through the setting's
// change signal), so they are cheap enough to call per chat line or per frame. Settings that FS only
// reads once at startup (CanOOC, NoSetEnv, the experience settings) are still only read once.
class RlvSettings
{
public:
    static bool getDebug()                      { return s_fDebug; }
    static bool getCanOOC()                     { return s_fCanOOC; }
    static bool getForbidGiveToRLV()            { return s_fForbidGiveToRlv; }
    static bool getNoSetEnv()                   { return s_fNoSetEnv; }

    static const std::string& getWearAddPrefix()     { return s_strWearAddPrefix; }
    static const std::string& getWearReplacePrefix() { return s_strWearReplacePrefix; }

    static bool getDebugHideUnsetDup()          { return s_fDebugHideUnsetDup; }
    #ifdef RLV_EXPERIMENTAL_COMPOSITEFOLDERS
    static bool getEnableComposites()           { return s_fCompositeFolders; }
    #endif // RLV_EXPERIMENTAL_COMPOSITEFOLDERS
    static bool getEnableIMQuery()              { return s_fEnableIMQuery; }
    static bool getEnableLegacyNaming()         { return s_fLegacyNaming; }
    static bool getEnableSharedWear()           { return s_fEnableSharedWear; }
    static bool getEnableTemporaryAttachments() { return s_fTempAttach; }
    static bool getExperimentalCommands()       { return s_fExperimentalCommands; }
    static bool getHideLockedLayers()           { return s_fHideLockedLayers; }
    static bool getHideLockedAttach()           { return s_fHideLockedAttach; }
    static bool getHideLockedInventory()        { return s_fHideLockedInventory; }
    static bool getSharedInvAutoRename()        { return s_fSharedInvAutoRename; }
    static bool getShowAssertionFailures()      { return s_fShowAssertionFailures; }
    static bool getShowEllipsis()               { return s_fShowEllipsis; }
    static bool getShowRedirectChatTyping()     { return s_fShowRedirectChatTyping; }
    static bool getSplitRedirectChat()          { return s_fSplitRedirectChat; }
    static bool getTopLevelMenu()               { return s_fTopLevelMenu; }
    static bool getWearReplaceUnlocked()        { return s_fWearReplaceUnlocked; }
    static bool getDebugDeprecateExplicitPoint(){ return s_fDebugDeprecateExplicitPoint; }

    // RLVaExperienceMaturityThreshold clamped to 0..3 (0 = no experience may issue commands)
    static S32  getMaturityThreshold()          { return s_nMaturityThreshold; }

    // RLVaBlockedCommands (Charis): comma separated behaviour names the user never wants to obey.
    // getBlockedCommands() is the raw setting; isBlockedCommand() matches one behaviour name
    // (trimmed, case-insensitive). getBlockedCommandsVersion() changes every time the setting does,
    // so callers can cache a per-behaviour table and rebuild it only when the number changes.
    static const std::string& getBlockedCommands() { return s_strBlockedCommands; }
    static bool isBlockedCommand(const std::string_view& strBehaviour);
    static U32  getBlockedCommandsVersion()     { return s_nBlockedCommandsVersion; }

    static bool getLoginLastLocation()          { return rlvGetPerUserSetting<bool>(RlvSettingNames::LoginLastLocation, true); }
    static void updateLoginLastLocation();

    static void initCompatibilityMode(std::string strCompatList);
    static bool isCompatibilityModeObject(const LLUUID& idRlvObject);

    static bool isAllowedExperience(const LLUUID& idExperience, U8 nMaturity);

    static void initClass();
    static void cleanupClass();

    // Listens to "RestrainedLove" so the user gets told when a toggle takes effect. Needs to be
    // connected at startup, before RLVa is (or isn't) enabled, so it is separate from initClass().
    static void initMainListener();
    static void onChangedSettingMain(const LLSD& sdValue);
protected:
    static bool onChangedMenuLevel();
    static void onChangedBlockedCommands(const LLSD& sdValue);

    #ifdef RLV_EXPERIMENTAL_COMPOSITEFOLDERS
    static bool s_fCompositeFolders;
    #endif // RLV_EXPERIMENTAL_COMPOSITEFOLDERS

    /*
     * Member variables
     */
protected:
    static bool s_fCanOOC;
    static bool s_fDebug;
    static bool s_fDebugHideUnsetDup;
    static bool s_fEnableIMQuery;
    static bool s_fEnableSharedWear;
    static bool s_fExperimentalCommands;
    static bool s_fForbidGiveToRlv;
    static bool s_fHideLockedAttach;
    static bool s_fHideLockedInventory;
    static bool s_fHideLockedLayers;
    static bool s_fLegacyNaming;
    static bool s_fNoSetEnv;
    static bool s_fSharedInvAutoRename;
    static bool s_fShowAssertionFailures;
    static bool s_fShowEllipsis;
    static bool s_fShowRedirectChatTyping;
    static bool s_fSplitRedirectChat;
    static bool s_fTempAttach;
    static bool s_fTopLevelMenu;
    static bool s_fWearReplaceUnlocked;
    static bool s_fDebugDeprecateExplicitPoint;
    static S32  s_nMaturityThreshold;
    static U8   s_nExperienceMinMaturity;
    static std::string s_strWearAddPrefix;
    static std::string s_strWearReplacePrefix;
    static std::string s_strBlockedCommands;
    static std::vector<std::string> s_BlockedCommands;
    static U32  s_nBlockedCommandsVersion;
    static std::list<std::string> s_BlockedExperiences;
    static std::list<LLUUID>      s_CompatItemCreators;
    static std::list<std::string> s_CompatItemNames;
};

// ============================================================================
// RlvStrings
//

class RlvStrings
{
public:
    static void initClass();
    static void loadFromFile(const std::string& strFilePath, bool fDefault);
    static void saveToFile(const std::string& strFilePath);

    static std::string getAnonym(const LLAvatarName& avName);       // @shownames
    static std::string getAnonym(const std::string& strName);       // @shownames
#ifdef CATZNIP_STRINGVIEW
    static const std::string& getString(const std::string_view& strStringName);
#else
    static const std::string& getString(const std::string& strStringName);
#endif // CATZNIP_STRINGVIEW
    static const char*        getStringFromReturnCode(ERlvCmdRet eRet);
    static const std::string& getStringMapPath() { return m_StringMapPath; }
    static std::string        getVersion(const LLUUID& idRlvObject, bool fLegacy = false);
    static std::string        getVersionAbout();
    static std::string        getVersionImplNum();
    static std::string        getVersionNum(const LLUUID& idRlvObject);
    static bool               hasString(const std::string& strStringName, bool fCheckCustom = false);
    static void               setCustomString(const std::string& strStringName, const std::string& strStringValue);

protected:
    static std::vector<std::string> m_Anonyms;
    typedef std::map<std::string, std::list<std::string>, std::less<>> string_map_t;
    static string_map_t m_StringMap;
    static std::string  m_StringMapPath;
};

// ============================================================================
// RlvUtil - Collection of (static) helper functions
//

class RlvUtil
{
public:
    static bool isEmote(const std::string& strUTF8Text);
    static bool isNearbyAgent(const LLUUID& idAgent);                               // @shownames (rlvnames.cpp; callers check for a null id first: it asserts)
    static bool isNearbyRegion(const std::string& strRegion);                       // @showloc   (rlvnames.cpp)

    static void filterLocation(std::string& strUTF8Text);                           // @showloc   (rlvnames.cpp; region / parcel names as they are now, nothing cached)
    static void filterNames(std::string& strUTF8Text, bool fFilterLegacy = true, bool fClearMatches = false);   // @shownames (rlvnames.cpp)
    static void filterMentions(std::string& strUTF8Text);                           // @shownames (mention URIs)
    static void filterScriptQuestions(S32& nQuestions, LLSD& sdPayload);

    static bool isForceTp() { return m_fForceTp; }
    static void forceTp(const LLVector3d& posDest);                                 // Ignores restrictions that might otherwise prevent tp'ing

#ifdef CATZNIP_STRINGVIEW
    static void notifyBlocked(const std::string_view& strNotifcation, const LLSD& sdArgs = LLSD(), bool fLogToChat = false);
#else
    static void notifyBlocked(const std::string& strNotifcation, const LLSD& sdArgs = LLSD(), bool fLogToChat = false);
#endif // CATZNIP_STRINGVIEW
    static void notifyBlockedGeneric() { notifyBlocked(RlvStringKeys::Blocked::Generic); }
    static void notifyBlockedViewXXX(LLAssetType::EType assetType) { notifyBlocked(RlvStringKeys::Blocked::ViewXxx, LLSD().with("[TYPE]", LLTrans::getString(LLAssetType::lookupHumanReadable(assetType)))); }
    static void notifyFailedAssertion(const std::string& strAssert, const std::string& strFile, int nLine);

    // Automatic IM response (do-not-disturb style). With fThrottle (automatic responses to @recvim, @tplure,
    // @tprequest, @stopim, ...) it goes out at most once per RLV_AUTORESPONSE_INTERVAL seconds to the same
    // recipient; pass false for answers that were asked for (@version, the @list / @except refusal).
    // Returns false when this one was held back.
    static bool sendBusyMessage(const LLUUID& idTo, const std::string& strMsg, const LLUUID& idSession = LLUUID::null, bool fThrottle = true);
    static bool isValidReplyChannel(S32 nChannel, bool fLoopback = false);
    // Chat replies are limited to RLV_MAX_REPLIES_PER_SECOND in all and, when idObject (the object whose command
    // is answered) is given, RLV_MAX_OBJECT_REPLIES_PER_SECOND for that object (checkReplyBudget()).
    // fBudget = false for messages that must not be dropped by the limit.
    static bool sendChatReply(S32 nChannel, const std::string& strUTF8Text, bool fBudget = true, const LLUUID& idObject = LLUUID::null);
    static bool sendChatReply(const std::string& strChannel, const std::string& strUTF8Text, const LLUUID& idObject = LLUUID::null);
    // Counts one reply against this second's budgets; false (and nothing counted) once either is used up
    static bool checkReplyBudget(const LLUUID& idObject = LLUUID::null);
    static bool sendChatReplySplit(S32 nChannel, const std::string& strUTF8Text, char chSplit = ' ', const LLUUID& idObject = LLUUID::null);
    // Sends a (long) IM split into pieces; stops after nMaxMessages pieces and ends the last one with "(truncated)"
    static void sendIMMessage(const LLUUID& idTo, const std::string& strMsg, char chSplit, size_t nMaxMessages = RLV_MAX_IMQUERY_LIST_MESSAGES);
protected:
    static bool m_fForceTp;                                                         // @standtp
};

// ============================================================================
// RlvNearbyNames - the table behind RlvUtil::filterNames() / isNearbyAgent() (rlvnames.cpp)
//
// The nearby avatars' names are collected at most once a second and reused; the result of filtering a
// given text is cached until the table or the set of hidden names changes. Names (and region / parcel
// names) are matched as literal text, ASCII case-insensitive, with a word boundary only on a side that
// is an ASCII word character (so Japanese and symbol names are found too; no regex is built from a name),
// all in one pass over the original text (an anonym is never searched again); avatar names need 2+ characters.
// invalidate() forces a rebuild (called on @shownames toggles and exception changes).
//

class RlvNearbyNames
{
public:
    static void invalidate();
    static void cleanupClass();
};

// ============================================================================
// Extensibility classes
//

class RlvExtCommandHandler
{
public:
    virtual ~RlvExtCommandHandler() {}
    virtual bool onAddRemCommand(const RlvCommand& rlvCmd, ERlvCmdRet& cmdRet) { return false; }
    virtual bool onClearCommand(const RlvCommand& rlvCmd, ERlvCmdRet& cmdRet)  { return false; }
    virtual bool onReplyCommand(const RlvCommand& rlvCmd, ERlvCmdRet& cmdRet)  { return false; }
    virtual bool onForceCommand(const RlvCommand& rlvCmd, ERlvCmdRet& cmdRet)  { return false; }
};
typedef bool (RlvExtCommandHandler::*rlvExtCommandHandler)(const RlvCommand& rlvCmd, ERlvCmdRet& cmdRet);

// ============================================================================
// Generic menu enablers
//

bool rlvMenuMainToggleVisible(LLUICtrl* pMenuItem);
void rlvMenuToggleVisible();
bool rlvMenuCanShowName();
bool rlvMenuEnableIfNot(const LLSD& sdParam);

// ============================================================================
// Selection functors
//

// Charis: the @edit / @rez parts of rlvCanDeleteOrReturn() and RlvSelectIsEditable are stage 3
bool rlvCanDeleteOrReturn();
bool rlvCanDeleteOrReturn(const LLViewerObject* pObj);

struct RlvSelectHasLockedAttach : public LLSelectedNodeFunctor
{
    RlvSelectHasLockedAttach() {}
    virtual bool apply(LLSelectNode* pNode);
};

struct RlvSelectIsSittingOn : public LLSelectedNodeFunctor
{
    RlvSelectIsSittingOn(const LLVOAvatar* pAvatar) : m_pAvatar(pAvatar) {}
    /*virtual*/ bool apply(LLSelectNode* pNode);
protected:
    const LLVOAvatar* m_pAvatar;
};

// ============================================================================
// Predicates
//

bool rlvPredCanWearItem(const LLViewerInventoryItem* pItem, ERlvWearMask eWearMask);
bool rlvPredCanNotWearItem(const LLViewerInventoryItem* pItem, ERlvWearMask eWearMask);
bool rlvPredCanRemoveItem(const LLUUID& idItem);
bool rlvPredCanRemoveItem(const LLViewerInventoryItem* pItem);
bool rlvPredCanNotRemoveItem(const LLViewerInventoryItem* pItem);

struct RlvPredCanWearItem
{
    RlvPredCanWearItem(ERlvWearMask eWearMask) : m_eWearMask(eWearMask) {}
    bool operator()(const LLViewerInventoryItem* pItem) { return rlvPredCanWearItem(pItem, m_eWearMask); }
protected:
    ERlvWearMask m_eWearMask;
};

struct RlvPredCanNotWearItem
{
    RlvPredCanNotWearItem(ERlvWearMask eWearMask) : m_eWearMask(eWearMask) {}
    bool operator()(const LLViewerInventoryItem* pItem) { return rlvPredCanNotWearItem(pItem, m_eWearMask); }
protected:
    ERlvWearMask m_eWearMask;
};

struct RlvPredCanRemoveItem
{
    RlvPredCanRemoveItem() {}
    bool operator()(const LLViewerInventoryItem* pItem) { return rlvPredCanRemoveItem(pItem); }
};

struct RlvPredCanNotRemoveItem
{
    RlvPredCanNotRemoveItem() {}
    bool operator()(const LLViewerInventoryItem* pItem) { return rlvPredCanNotRemoveItem(pItem); }
};

struct RlvPredIsEqualOrLinkedItem
{
    RlvPredIsEqualOrLinkedItem(const LLViewerInventoryItem* pItem) : m_pItem(pItem) {}
    RlvPredIsEqualOrLinkedItem(const LLUUID& idItem);
    bool operator()(const LLViewerInventoryItem* pItem) const;
protected:
    const LLViewerInventoryItem* m_pItem;
};

template<typename T> struct RlvPredValuesEqual
{
    bool operator()(const T* pT2) const { return (pT1) && (pT2) && (*pT1 == *pT2); }
    const T* pT1;
};

// ============================================================================
// Various public helper functions
//

std::string rlvGetAnonym(const LLAvatarName& avName);

// ============================================================================
// Inlined class member functions
//

// Checked: 2010-10-31 (RLVa-1.2.2a) | Added: RLVa-1.2.2a
inline std::string RlvStrings::getAnonym(const LLAvatarName& avName)
{
    return getAnonym(avName.getLegacyName());
}

// Checked: 2010-03-26 (RLVa-1.2.0b) | Modified: RLVa-1.0.2a
inline bool RlvUtil::isEmote(const std::string& strUTF8Text)
{
    return (strUTF8Text.length() > 4) && ( (strUTF8Text.compare(0, 4, "/me ") == 0) || (strUTF8Text.compare(0, 4, "/me'") == 0) );
}

// Checked: 2010-03-09 (RLVa-1.2.0a) | Added: RLVa-1.0.1e
// Replies go to channel 1 and up (never to public chat and never to the debug channel); loopback
// (a command the user typed themselves) may also use channel 0, which is shown locally only.
inline bool RlvUtil::isValidReplyChannel(S32 nChannel, bool fLoopback /*=false*/)
{
    return (nChannel > ((!fLoopback) ? 0 : -1)) && (CHAT_CHANNEL_DEBUG != nChannel);
}

// Checked: 2009-08-05 (RLVa-1.0.1e) | Added: RLVa-1.0.0e
inline bool RlvUtil::sendChatReply(const std::string& strChannel, const std::string& strUTF8Text, const LLUUID& idObject)
{
    S32 nChannel;
    return (LLStringUtil::convertToS32(strChannel, nChannel)) ? sendChatReply(nChannel, strUTF8Text, true, idObject) : false;
}

// ============================================================================
