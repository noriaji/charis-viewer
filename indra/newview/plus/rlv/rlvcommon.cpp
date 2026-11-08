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
#include "llagent.h"
#include "llagentui.h"
#include "llavatarnamecache.h"
#include "llcallingcard.h"
#include "llframetimer.h"
#include "llimview.h"
#include "llinstantmessage.h"
#include "llmenugl.h"
#include "llnotificationsutil.h"
#include "llregex.h"
#include "llselectmgr.h"
#include "llsdserialize.h"
#include "lluri.h"
#include "lltrans.h"
#include "llurlentry.h"
#include "llversioninfo.h"
#include "llviewerinventory.h"
#include "llviewermenu.h"
#include "llviewermessage.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerstats.h"
#include "llvoavatarself.h"

#include "rlvactions.h"
#include "rlvcommon.h"
#include "rlvhelper.h"
#include "rlvhandler.h"
#include "rlvforcewear.h"
#include "rlvlocks.h"

#include "plus.h"                               // plus::safeText()

#include "llscriptruntimeperms.h"
#include <boost/algorithm/string/predicate.hpp> // icontains
#include <boost/algorithm/string.hpp>

// ============================================================================
// Forward declarations
//

// llviewermenu.cpp
LLVOAvatar* find_avatar_from_object(LLViewerObject* object);

// Settings that RlvSettingNames (rlvdefines.h) doesn't name
namespace
{
    constexpr std::string_view RLV_SETTING_BLOCKEDCOMMANDS = "RLVaBlockedCommands";
    constexpr std::string_view RLV_SETTING_DEPRECATEEXPLICITPOINT = "RLVaDebugDeprecateExplicitPoint";
    constexpr std::string_view RLV_SETTING_BLOCKEDEXPERIENCES = "RLVaBlockedExperiences";
    constexpr std::string_view RLV_SETTING_COMPATIBILITYLIST = "RLVaCompatibilityModeList";
    constexpr std::string_view RLV_SETTING_EXPERIENCEMATURITY = "RLVaExperienceMaturityThreshold";
    constexpr std::string_view RLV_SETTING_EXPERIMENTALCMDS = "RLVaExperimentalCommands";
}

// ============================================================================
// utf8str_split (FS: llcommon/llstring.cpp)
//

// Returns at most max_len bytes of utf8str starting at index without cutting a multi-byte character
// (FS: utf8str_substr()). Returns at least one character when max_len > 0, so callers always progress.
static std::string rlv_utf8str_substr(const std::string& utf8str, size_t index, size_t max_len)
{
    if ( (0 == max_len) || (index >= utf8str.length()) )
        return std::string();
    if (utf8str.length() - index <= max_len)
        return utf8str.substr(index, max_len);

    size_t cur_char = max_len;
    // Step back from a continuation byte (10xxxxxx) to the start of its character
    while ( (cur_char > 0) && (0x80 == (0xc0 & (U8)utf8str[index + cur_char])) )
        cur_char--;
    if (0 == cur_char)
    {
        // Malformed input (or a single character longer than max_len): take the bytes as they are
        cur_char = max_len;
    }
    return utf8str.substr(index, cur_char);
}

void utf8str_split(std::list<std::string>& split_list, const std::string& utf8str, size_t maxlen, char split_token)
{
    split_list.clear();
    if (0 == maxlen)
        return;

    const std::string::size_type lenMsg = utf8str.length();
    std::string::size_type lenIt = 0;
    while (lenIt < lenMsg)
    {
        std::string strTemp;
        if (lenIt + maxlen < lenMsg)
        {
            // Find the last split character within the first maxlen bytes
            std::string::size_type posSplit = lenIt + maxlen;
            while ( (posSplit > lenIt) && (utf8str[posSplit] != split_token) )
                posSplit--;

            if (posSplit > lenIt)
                strTemp = utf8str.substr(lenIt, posSplit - lenIt);
            else
                strTemp = rlv_utf8str_substr(utf8str, lenIt, maxlen);
        }
        else
        {
            strTemp = utf8str.substr(lenIt, std::string::npos);
        }

        lenIt += strTemp.length();
        split_list.push_back(std::move(strTemp));

        if ( (lenIt < lenMsg) && (utf8str[lenIt] == split_token) )
            lenIt++;
    }
}

// =========================================================================
// RlvSettings
//

#ifdef RLV_EXPERIMENTAL_COMPOSITEFOLDERS
bool RlvSettings::s_fCompositeFolders = false;
#endif // RLV_EXPERIMENTAL_COMPOSITEFOLDERS
// Defaults match the declared settings (research/248 7.1); they only matter if a control is missing
bool RlvSettings::s_fCanOOC = true;
bool RlvSettings::s_fDebug = false;
bool RlvSettings::s_fDebugHideUnsetDup = false;
bool RlvSettings::s_fEnableIMQuery = true;
bool RlvSettings::s_fEnableSharedWear = true;
bool RlvSettings::s_fExperimentalCommands = true;
bool RlvSettings::s_fForbidGiveToRlv = false;
bool RlvSettings::s_fHideLockedAttach = false;
bool RlvSettings::s_fHideLockedInventory = false;
bool RlvSettings::s_fHideLockedLayers = false;
bool RlvSettings::s_fLegacyNaming = true;
bool RlvSettings::s_fNoSetEnv = false;
bool RlvSettings::s_fSharedInvAutoRename = true;
bool RlvSettings::s_fShowAssertionFailures = true;
bool RlvSettings::s_fShowEllipsis = true;
bool RlvSettings::s_fShowRedirectChatTyping = false;
bool RlvSettings::s_fSplitRedirectChat = false;
bool RlvSettings::s_fTempAttach = true;
bool RlvSettings::s_fTopLevelMenu = true;
bool RlvSettings::s_fWearReplaceUnlocked = true;
bool RlvSettings::s_fDebugDeprecateExplicitPoint = false;
S32  RlvSettings::s_nMaturityThreshold = 2;
U8   RlvSettings::s_nExperienceMinMaturity = 0;
std::string RlvSettings::s_strWearAddPrefix = "+";
std::string RlvSettings::s_strWearReplacePrefix;
std::string RlvSettings::s_strBlockedCommands;
std::vector<std::string> RlvSettings::s_BlockedCommands;
U32  RlvSettings::s_nBlockedCommandsVersion = 0;
std::list<std::string> RlvSettings::s_BlockedExperiences;
std::list<LLUUID> RlvSettings::s_CompatItemCreators;
std::list<std::string> RlvSettings::s_CompatItemNames;

namespace
{
    // Every connection we make to a setting's change signal (disconnected in RlvSettings::cleanupClass())
    std::vector<boost::signals2::connection> s_SettingConnections;
    boost::signals2::connection s_MainSettingConnection;

    // Pre-compiled RLVaCompatibilityModeList "name:" patterns (FS compiles them on every @version)
    std::vector<boost::regex> s_CompatItemPatterns;

    void readSettingValue(const LLSD& sdValue, bool& fValue)        { fValue = sdValue.asBoolean(); }
    void readSettingValue(const LLSD& sdValue, std::string& strValue) { strValue = sdValue.asString(); }

    // Reads the setting into *pValue and keeps it current through the setting's change signal
    template<typename T>
    void bindSetting(const std::string_view& strSetting, T* pValue)
    {
        LLControlVariable* pControl = gSavedSettings.getControl(strSetting).get();
        if (!pControl)
        {
            RLV_DEBUGS << "Missing setting " << strSetting << RLV_ENDL;
            return;
        }
        readSettingValue(pControl->getValue(), *pValue);
        s_SettingConnections.push_back(pControl->getSignal()->connect(
            [pValue](LLControlVariable*, const LLSD& sdNewValue, const LLSD&) { readSettingValue(sdNewValue, *pValue); }));
    }
}

// Checked: 2010-02-27 (RLVa-1.2.0a) | Modified: RLVa-1.1.0i
void RlvSettings::initClass()
{
    static bool fInitialized = false;
    if (!fInitialized)
    {
        initCompatibilityMode(LLStringUtil::null);

        bindSetting(RlvSettingNames::EnableTempAttach, &s_fTempAttach);

        #ifdef RLV_EXPERIMENTAL_COMPOSITEFOLDERS
        bindSetting(RLV_SETTING_ENABLECOMPOSITES, &s_fCompositeFolders);
        #endif // RLV_EXPERIMENTAL_COMPOSITEFOLDERS

        bindSetting(RlvSettingNames::EnableLegacyNaming, &s_fLegacyNaming);
        bindSetting(RlvSettingNames::Debug, &s_fDebug);
        bindSetting(RlvSettingNames::DebugHideUnsetDup, &s_fDebugHideUnsetDup);
        bindSetting(RlvSettingNames::ForbidGiveToRlv, &s_fForbidGiveToRlv);
        bindSetting(RlvSettingNames::EnableIMQuery, &s_fEnableIMQuery);
        bindSetting(RlvSettingNames::EnableSharedWear, &s_fEnableSharedWear);
        bindSetting(RlvSettingNames::HideLockedLayer, &s_fHideLockedLayers);
        bindSetting(RlvSettingNames::HideLockedAttach, &s_fHideLockedAttach);
        if (gSavedSettings.controlExists(RlvSettingNames::HideLockedInventory))
            bindSetting(RlvSettingNames::HideLockedInventory, &s_fHideLockedInventory);
        bindSetting(RlvSettingNames::SharedInvAutoRename, &s_fSharedInvAutoRename);
        bindSetting(RlvSettingNames::ShowAssertionFail, &s_fShowAssertionFailures);
        bindSetting(RlvSettingNames::ShowEllipsis, &s_fShowEllipsis);
        bindSetting(RlvSettingNames::ShowRedirectChatTyping, &s_fShowRedirectChatTyping);
        bindSetting(RlvSettingNames::SplitRedirectChat, &s_fSplitRedirectChat);
        bindSetting(RlvSettingNames::WearReplaceUnlocked, &s_fWearReplaceUnlocked);
        bindSetting(RlvSettingNames::WearAddPrefix, &s_strWearAddPrefix);
        bindSetting(RlvSettingNames::WearReplacePrefix, &s_strWearReplacePrefix);
        bindSetting(RLV_SETTING_EXPERIMENTALCMDS, &s_fExperimentalCommands);
        if (gSavedSettings.controlExists(RLV_SETTING_DEPRECATEEXPLICITPOINT))
            bindSetting(RLV_SETTING_DEPRECATEEXPLICITPOINT, &s_fDebugDeprecateExplicitPoint);

        // Read once (as FS does): changing these while restricted mustn't lift a restriction
        s_fCanOOC = rlvGetSetting<bool>(RlvSettingNames::CanOoc, true);
        if (gSavedSettings.controlExists(RlvSettingNames::NoSetEnv))
            s_fNoSetEnv = gSavedSettings.get<bool>(RlvSettingNames::NoSetEnv);

        // Don't allow toggling RLVaLoginLastLocation from the debug settings floater
        if (gSavedPerAccountSettings.controlExists(RlvSettingNames::LoginLastLocation))
            gSavedPerAccountSettings.getControl(RlvSettingNames::LoginLastLocation)->setHiddenFromSettingsEditor(true);

        if (LLControlVariable* pControl = gSavedSettings.getControl(RlvSettingNames::TopLevelMenu).get())
        {
            s_fTopLevelMenu = pControl->getValue().asBoolean();
            s_SettingConnections.push_back(pControl->getSignal()->connect(
                [](LLControlVariable*, const LLSD& sdNewValue, const LLSD&) { s_fTopLevelMenu = sdNewValue.asBoolean(); onChangedMenuLevel(); }));
        }

        if (LLControlVariable* pControl = gSavedSettings.getControl(RLV_SETTING_BLOCKEDCOMMANDS).get())
        {
            onChangedBlockedCommands(pControl->getValue());
            s_SettingConnections.push_back(pControl->getSignal()->connect(
                [](LLControlVariable*, const LLSD& sdNewValue, const LLSD&) { onChangedBlockedCommands(sdNewValue); }));
        }

        // RLVaExperienceMaturityThreshold: 0 = none, 1 = PG and up, 2 = Mature and up, 3 = Adult only (FS semantics)
        s_nMaturityThreshold = llclamp(rlvGetSetting<S32>(RLV_SETTING_EXPERIENCEMATURITY, 2), 0, 3);
        s_nExperienceMinMaturity = (s_nMaturityThreshold == 0) ? 0 : ((s_nMaturityThreshold == 1) ? SIM_ACCESS_PG : ((s_nMaturityThreshold == 2) ? SIM_ACCESS_MATURE : SIM_ACCESS_ADULT));
        const std::string strBlockedExperiences = rlvGetSetting<std::string>(RLV_SETTING_BLOCKEDEXPERIENCES, LLStringUtil::null);
        boost::split(s_BlockedExperiences, strBlockedExperiences, boost::is_any_of(";"));

        fInitialized = true;
    }
}

void RlvSettings::cleanupClass()
{
    for (boost::signals2::connection& conn : s_SettingConnections)
        conn.disconnect();
    s_SettingConnections.clear();
    s_MainSettingConnection.disconnect();
    s_CompatItemPatterns.clear();
}

void RlvSettings::initMainListener()
{
    if (s_MainSettingConnection.connected())
        return;
    if (LLControlVariable* pControl = gSavedSettings.getControl(RlvSettingNames::Main).get())
    {
        s_MainSettingConnection = pControl->getSignal()->connect(
            [](LLControlVariable*, const LLSD& sdNewValue, const LLSD&) { onChangedSettingMain(sdNewValue); });
    }
}

// Checked: 2010-04-01 (RLVa-1.2.0c) | Modified: RLVa-0.2.1d
void RlvSettings::updateLoginLastLocation()
{
    if ( (!LLApp::isExiting()) && (gSavedPerAccountSettings.controlExists(RlvSettingNames::LoginLastLocation)) )
    {
        bool fValue = (gRlvHandler.hasBehaviour(RLV_BHVR_TPLOC)) || (!RlvActions::canStand());
        if (gSavedPerAccountSettings.get<bool>(RlvSettingNames::LoginLastLocation) != fValue)
        {
            gSavedPerAccountSettings.set<bool>(RlvSettingNames::LoginLastLocation, fValue);
            const std::string strPerAccountFile = gSavedSettings.getString("PerAccountSettingsFile");
            if (!strPerAccountFile.empty())
                gSavedPerAccountSettings.saveToFile(strPerAccountFile, true);
        }
    }
}

// Checked: 2011-08-16 (RLVa-1.4.0b) | Added: RLVa-1.4.0b
bool RlvSettings::onChangedMenuLevel()
{
    rlvMenuToggleVisible();
    return true;
}

void RlvSettings::onChangedBlockedCommands(const LLSD& sdValue)
{
    s_strBlockedCommands = sdValue.asString();
    s_BlockedCommands.clear();

    boost_tokenizer tokCommands(s_strBlockedCommands, boost::char_separator<char>(","));
    for (std::string strCommand : tokCommands)
    {
        LLStringUtil::trim(strCommand);
        if ( (!strCommand.empty()) && (RLV_CMD_PREFIX == strCommand.front()) )
            strCommand.erase(0, 1);
        LLStringUtil::toLower(strCommand);
        if ( (!strCommand.empty()) && (s_BlockedCommands.end() == std::find(s_BlockedCommands.begin(), s_BlockedCommands.end(), strCommand)) )
            s_BlockedCommands.push_back(strCommand);
    }
    s_nBlockedCommandsVersion++;
}

bool RlvSettings::isBlockedCommand(const std::string_view& strBehaviour)
{
    if (s_BlockedCommands.empty())
        return false;
    for (const std::string& strBlocked : s_BlockedCommands)
    {
        if ( (strBlocked.size() == strBehaviour.size()) &&
             (std::equal(strBlocked.begin(), strBlocked.end(), strBehaviour.begin(), [](char chBlocked, char ch) { return chBlocked == LLStringOps::toLower(ch); })) )
        {
            return true;
        }
    }
    return false;
}

void RlvSettings::onChangedSettingMain(const LLSD& sdValue)
{
    LLStringUtil::format_map_t args;
    args["[STATE]"] = LLTrans::getString( (sdValue.asBoolean()) ? "RLVaToggleEnabled" : "RLVaToggleDisabled");

    // As long as RLVa hasn't been enabled but >can< be enabled all toggles are instant (everything else will require a restart)
    bool fQuickToggle = (!RlvHandler::isEnabled()) && (RlvHandler::canEnable());
    LLNotificationsUtil::add("GenericAlert", LLSD().with("MESSAGE", LLTrans::getString((fQuickToggle) ? "RLVaToggleMessageLogin" : "RLVaToggleMessageRestart", args)));
}

void RlvSettings::initCompatibilityMode(std::string strCompatList)
{
    // NOTE: this function can be called more than once
    s_CompatItemCreators.clear();
    s_CompatItemNames.clear();
    s_CompatItemPatterns.clear();

    strCompatList.append(";").append(rlvGetSetting<std::string>(RLV_SETTING_COMPATIBILITYLIST, ""));

    boost_tokenizer tokCompatList(strCompatList, boost::char_separator<char>(";", "", boost::drop_empty_tokens));
    for (const std::string& strCompatEntry : tokCompatList)
    {
        if (boost::starts_with(strCompatEntry, "creator:"))
        {
            LLUUID idCreator;
            if ( (44 == strCompatEntry.size()) && (LLUUID::parseUUID(strCompatEntry.substr(8), &idCreator)) &&
                 (s_CompatItemCreators.end() == std::find(s_CompatItemCreators.begin(), s_CompatItemCreators.end(), idCreator)) )
            {
                s_CompatItemCreators.push_back(idCreator);
            }
        }
        else if (boost::starts_with(strCompatEntry, "name:"))
        {
            if (strCompatEntry.size() > 5)
            {
                // A malformed pattern is skipped (FS would throw from boost::regex on every @version)
                try
                {
                    s_CompatItemPatterns.emplace_back(strCompatEntry.substr(5), boost::regex::perl | boost::regex::icase);
                    s_CompatItemNames.push_back(strCompatEntry.substr(5));
                }
                catch (const boost::regex_error&)
                {
                    RLV_DEBUGS << "Ignoring malformed compatibility pattern" << RLV_ENDL;
                }
            }
        }
    }
}

bool RlvSettings::isCompatibilityModeObject(const LLUUID& idRlvObject)
{
    bool fCompatMode = false;
    if (idRlvObject.notNull())
    {
        const LLViewerObject* pObj = gObjectList.findObject(idRlvObject);
        if ( (pObj) && (pObj->isAttachment()) )
        {
            const LLViewerInventoryItem* pItem = gInventory.getItem(pObj->getAttachmentItemID());
            if (pItem)
            {
                fCompatMode = s_CompatItemCreators.end() != std::find(s_CompatItemCreators.begin(), s_CompatItemCreators.end(), pItem->getCreatorUUID());
                if (!fCompatMode)
                {
                    const std::string& strAttachName = pItem->getName();
                    for (const boost::regex& regexp : s_CompatItemPatterns)
                    {
                        if (ll_regex_match(strAttachName, regexp))
                        {
                            fCompatMode = true;
                            break;
                        }
                    }
                }
            }
        }
    }
    return fCompatMode;
}

bool RlvSettings::isAllowedExperience(const LLUUID& idExperience, U8 nMaturity)
{
    // An experience is allowed to interact with RLVa if:
    //   - temporary attachments can interact with RLVa
    //   - the user set a minimum maturity and the specified maturity is equal or higher
    //   - the experience isn't explicitly blocked (NOTE: case-sensitive string comparison)
    return
        (getEnableTemporaryAttachments()) &&
        (s_nExperienceMinMaturity) && (s_nExperienceMinMaturity <= nMaturity) &&
        (s_BlockedExperiences.end() == std::find(s_BlockedExperiences.begin(), s_BlockedExperiences.end(), idExperience.asString()));
}

// ============================================================================
// RlvStrings
//

std::vector<std::string> RlvStrings::m_Anonyms;
RlvStrings::string_map_t RlvStrings::m_StringMap;
std::string RlvStrings::m_StringMapPath;

// Checked: 2011-11-08 (RLVa-1.5.0)
void RlvStrings::initClass()
{
    static bool fInitialized = false;
    if (!fInitialized)
    {
        // Load the default string values
        std::vector<std::string> files = gDirUtilp->findSkinnedFilenames(LLDir::XUI, RLV_STRINGS_FILE, LLDir::ALL_SKINS);
        m_StringMapPath = (!files.empty()) ? files.front() : LLStringUtil::null;
        for (std::vector<std::string>::const_iterator itFile = files.begin(); itFile != files.end(); ++itFile)
        {
            loadFromFile(*itFile, false);
        }

        // Load the custom string overrides
        loadFromFile(gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, RLV_STRINGS_FILE), true);

        // Sanity check
        if ( (m_StringMap.empty()) || (m_Anonyms.empty()) )
        {
            RLV_ERRS << "Problem parsing RLVa string XML file" << RLV_ENDL;
            return;
        }

        fInitialized = true;
    }
}

// Checked: 2011-11-08 (RLVa-1.5.0)
void RlvStrings::loadFromFile(const std::string& strFilePath, bool fUserOverride)
{
    llifstream fileStream(strFilePath.c_str(), std::ios::binary); LLSD sdFileData;
    if ( (!fileStream.is_open()) || (!LLSDSerialize::fromXMLDocument(sdFileData, fileStream)) )
        return;
    fileStream.close();

    if (sdFileData.has("strings"))
    {
        const LLSD& sdStrings = sdFileData["strings"];
        for (LLSD::map_const_iterator itString = sdStrings.beginMap(); itString != sdStrings.endMap(); ++itString)
        {
            if ( (!itString->second.has("value")) || ((fUserOverride) && (!hasString(itString->first))) )
                continue;

            std::list<std::string>& listValues = m_StringMap[itString->first];
            if (!fUserOverride)
            {
                if (listValues.size() > 0)
                    listValues.pop_front();
                listValues.push_front(itString->second["value"].asString());
            }
            else
            {
                while (listValues.size() > 1)
                    listValues.pop_back();
                listValues.push_back(itString->second["value"].asString());
            }
        }
    }
    if (sdFileData.has("anonyms"))
    {
        const LLSD& sdAnonyms = sdFileData["anonyms"];
        for (LLSD::array_const_iterator itAnonym = sdAnonyms.beginArray(); itAnonym != sdAnonyms.endArray(); ++itAnonym)
        {
            m_Anonyms.push_back((*itAnonym).asString());
        }
    }
}

// Checked: 2011-11-08 (RLVa-1.5.0)
void RlvStrings::saveToFile(const std::string& strFilePath)
{
    LLSD sdFileData;

    LLSD& sdStrings = sdFileData["strings"];
    for (string_map_t::const_iterator itString = m_StringMap.begin(); itString != m_StringMap.end(); ++itString)
    {
        const std::list<std::string>& listValues = itString->second;
        if (listValues.size() > 1)
            sdStrings[itString->first]["value"] = listValues.back();
    }

    llofstream fileStream(strFilePath.c_str());
    if (!fileStream.good())
        return;

    LLSDSerialize::toPrettyXML(sdFileData, fileStream);
    fileStream.close();
}

// Checked: 2009-11-11 (RLVa-1.1.0a) | Modified: RLVa-1.1.0a
std::string RlvStrings::getAnonym(const std::string& strName)
{
    static const std::string strUnknown = LLTrans::getString("Unknown");
    if ( (!RlvActions::isRlvEnabled()) || (m_Anonyms.empty()) )
    {
        return strUnknown;
    }

    const char* pszName = strName.c_str(); U32 nHash = 0;

    // Test with 11,264 SL names showed a 3.33% - 3.82% occurance for each so we *should* get a very even spread
    for (size_t idx = 0, cnt = strName.length(); idx < cnt; idx++)
        nHash += pszName[idx];

    return m_Anonyms[nHash % m_Anonyms.size()];
}

// Checked: 2011-11-08 (RLVa-1.5.0)
#ifdef CATZNIP_STRINGVIEW
const std::string& RlvStrings::getString(const std::string_view& strStringName)
#else
const std::string& RlvStrings::getString(const std::string& strStringName)
#endif // CATZNIP_STRINGVIEW
{
    static const std::string strMissing = "(Missing RLVa string)";
    string_map_t::const_iterator itString = m_StringMap.find(strStringName);
    return ( (itString != m_StringMap.end()) && (!itString->second.empty()) ) ? itString->second.back() : strMissing;
}

// Checked: 2009-11-25 (RLVa-1.1.0f) | Added: RLVa-1.1.0f
const char* RlvStrings::getStringFromReturnCode(ERlvCmdRet eRet)
{
    // TODO-RLVa: [2009-11-25] clean this up along with the calling code in process_chat_from_simulator() once we're happy with the output
    switch (eRet)
    {
        case RLV_RET_SUCCESS_UNSET:
            return "unset";
        case RLV_RET_SUCCESS_DUPLICATE:
            return "duplicate";
        case RLV_RET_SUCCESS_DELAYED:
            return "delayed";
        case RLV_RET_SUCCESS_DEPRECATED:
            return "deprecated";
        case RLV_RET_FAILED_SYNTAX:
            return "thingy error";
        case RLV_RET_FAILED_OPTION:
            return "invalid option";
        case RLV_RET_FAILED_PARAM:
            return "invalid param";
        case RLV_RET_FAILED_LOCK:
            return "locked command";
        case RLV_RET_FAILED_DISABLED:
            return "disabled command";
        case RLV_RET_FAILED_UNKNOWN:
            return "unknown command";
        case RLV_RET_FAILED_NOSHAREDROOT:
            return "missing #RLV";
        case RLV_RET_FAILED_DEPRECATED:
            return "deprecated and disabled";
        case RLV_RET_FAILED_NOBEHAVIOUR:
            return "no active behaviours";
        case RLV_RET_FAILED_UNHELDBEHAVIOUR:
            return "base behaviour not held";
        case RLV_RET_FAILED_BLOCKED:
            return "blocked object";
        case RLV_RET_FAILED_THROTTLED:
            return "throttled";
        // The following are identified by the chat verb
        case RLV_RET_RETAINED:
        case RLV_RET_SUCCESS:
        case RLV_RET_FAILED:
            break;
        // The following shouldn't occur
        case RLV_RET_UNKNOWN:
        default:
            RLV_ASSERT(false);
            break;
    };
    return NULL;
}

std::string RlvStrings::getVersion(const LLUUID& idRlvObject, bool fLegacy)
{
    bool fCompatMode = RlvSettings::isCompatibilityModeObject(idRlvObject);
    return llformat("%s viewer v%d.%d.%d (RLVa %d.%d.%d)",
        ( (!fLegacy) ? "RestrainedLove" : "RestrainedLife" ),
        (!fCompatMode) ? RLV_VERSION_MAJOR : RLV_VERSION_MAJOR_COMPAT, (!fCompatMode) ? RLV_VERSION_MINOR : RLV_VERSION_MINOR_COMPAT, (!fCompatMode) ? RLV_VERSION_PATCH : RLV_VERSION_PATCH_COMPAT,
        RLVa_VERSION_MAJOR, RLVa_VERSION_MINOR, RLVa_VERSION_PATCH);
}

std::string RlvStrings::getVersionAbout()
{
    return llformat("RLV v%d.%d.%d / RLVa v%d.%d.%d.%d",
                    RLV_VERSION_MAJOR, RLV_VERSION_MINOR, RLV_VERSION_PATCH, RLVa_VERSION_MAJOR, RLVa_VERSION_MINOR, RLVa_VERSION_PATCH, LLVersionInfo::instance().getBuild());
}

std::string RlvStrings::getVersionNum(const LLUUID& idRlvObject)
{
    bool fCompatMode = RlvSettings::isCompatibilityModeObject(idRlvObject);
    return llformat("%d%02d%02d%02d",
        (!fCompatMode) ? RLV_VERSION_MAJOR : RLV_VERSION_MAJOR_COMPAT, (!fCompatMode) ? RLV_VERSION_MINOR : RLV_VERSION_MINOR_COMPAT,
        (!fCompatMode) ? RLV_VERSION_PATCH : RLV_VERSION_PATCH_COMPAT, (!fCompatMode) ? RLV_VERSION_BUILD : RLV_VERSION_BUILD_COMPAT);
}

std::string RlvStrings::getVersionImplNum()
{
    return llformat("%d%02d%02d%02d", RLVa_VERSION_MAJOR, RLVa_VERSION_MINOR, RLVa_VERSION_PATCH, RLVa_IMPL_ID);
}

// Checked: 2011-11-08 (RLVa-1.5.0)
bool RlvStrings::hasString(const std::string& strStringName, bool fCheckCustom)
{
    string_map_t::const_iterator itString = m_StringMap.find(strStringName);
    return (itString != m_StringMap.end()) && ((!fCheckCustom) || (itString->second.size() > 0));
}

// Checked: 2011-11-08 (RLVa-1.5.0)
void RlvStrings::setCustomString(const std::string& strStringName, const std::string& strStringValue)
{
    if (!hasString(strStringName))
        return;

    std::list<std::string>& listValues = m_StringMap[strStringName];
    while (listValues.size() > 1)
        listValues.pop_back();
    if (!strStringValue.empty())
        listValues.push_back(strStringValue);
}

// ============================================================================
// RlvUtil
//
// NOTE: filterLocation(), filterNames(), isNearbyAgent() and isNearbyRegion() live in rlvnames.cpp
//

bool RlvUtil::m_fForceTp = false;

// Checked: 2026-02-09 (RLVa-2.6.2) | Added: RLVa-2.6.2
void RlvUtil::filterMentions(std::string& strUTF8Text)
{
    if (!RlvActions::isRlvEnabled())
        return;
    if (RlvActions::canShowName(RlvActions::SNC_DEFAULT))
        return;
    // Cheap pre-check: every mention URI ends in "/mention"
    if (std::string::npos == strUTF8Text.find("/mention"))
        return;

    static const boost::regex mention_regex(APP_HEADER_REGEX
                                            "/agent/([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})"
                                            "/mention(?=/|\\?|$)",
                                            boost::regex::perl);
    boost::sregex_iterator it(strUTF8Text.begin(), strUTF8Text.end(), mention_regex);
    boost::sregex_iterator end;

    if (it == end)
        return;

    std::string result;
    result.reserve(strUTF8Text.size());
    size_t last_pos = 0;

    for (; it != end; ++it)
    {
        const boost::smatch& match = *it;
        const size_t start = match.position();
        const size_t length = match.length();

        result.append(strUTF8Text, last_pos, start - last_pos);

        const std::string match_url = match.str();
        std::string agent_id_str;
        {
            LLURI uri(match_url);
            LLSD path_array = uri.pathArray();
            if (path_array.size() == 4)
            {
                agent_id_str = path_array.get(2).asString();
            }
        }
        LLUUID agent_id;
        agent_id.set(agent_id_str, false);
        const bool can_show =
            agent_id.notNull() &&
            (RlvActions::canShowName(RlvActions::SNC_DEFAULT, agent_id));

        if (can_show)
        {
            // Preserve the original mention URI for exceptions so URL-aware floaters keep it clickable.
            result.append(strUTF8Text, start, length);
        }
        else
        {
            // Remove the URI and replace with anonymized name if names are hidden
            LLAvatarName av_name;
            const std::string anonym = (agent_id.notNull() && LLAvatarNameCache::get(agent_id, &av_name))
                                           ? RlvStrings::getAnonym(av_name)
                                           : RlvStrings::getAnonym(agent_id_str.empty() ? LLUUID::null.asString() : agent_id_str);
            result += "@" + anonym;
        }

        last_pos = start + length;
    }

    result.append(strUTF8Text, last_pos, std::string::npos);
    strUTF8Text.swap(result);
}

// Checked: 2012-08-19 (RLVa-1.4.7)
void RlvUtil::filterScriptQuestions(S32& nQuestions, LLSD& sdPayload)
{
    // Check SCRIPT_PERMISSION_ATTACH
    if ( (!gRlvAttachmentLocks.canAttach()) && (SCRIPT_PERMISSIONS[RLV_SCRIPT_PERMISSION_ATTACH].permbit & nQuestions) )
    {
        // Notify the user that we blocked it since they're not allowed to wear any new attachments
        sdPayload["rlv_blocked"] = static_cast<std::string>(RlvStringKeys::Blocked::PermissionAttach);
        nQuestions &= ~SCRIPT_PERMISSIONS[RLV_SCRIPT_PERMISSION_ATTACH].permbit;
    }

    // Check SCRIPT_PERMISSION_TELEPORT
    if ( (gRlvHandler.hasBehaviour(RLV_BHVR_TPLOC)) && (SCRIPT_PERMISSIONS[RLV_SCRIPT_PERMISSION_TELEPORT].permbit & nQuestions) )
    {
        // Notify the user that we blocked it since they're not allowed to teleport
        sdPayload["rlv_blocked"] = static_cast<std::string>(RlvStringKeys::Blocked::PermissionTeleport);
        nQuestions &= ~SCRIPT_PERMISSIONS[RLV_SCRIPT_PERMISSION_TELEPORT].permbit;
    }

    sdPayload["questions"] = nQuestions;
}

// Checked: 2010-08-29 (RLVa-1.2.1c) | Added: RLVa-1.2.1c
void RlvUtil::forceTp(const LLVector3d& posDest)
{
    m_fForceTp = true;
    gAgent.teleportViaLocationLookAt(posDest);
    m_fForceTp = false;
}

// A notification argument that is exactly a link we build ourselves (an agent or group SLURL) stays
// clickable; anything else (names, object names, ...) comes from other residents and is defused.
static bool rlvIsOwnSLURL(const std::string& strValue)
{
    static const boost::regex own_slurl("^secondlife:///app/(agent|group)/[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}/(about|inspect|completename|displayname|username|rlvanonym)$",
                                        boost::regex::perl);
    return (strValue.size() < 128) && (ll_regex_match(strValue, own_slurl));
}

// Checked: 2011-04-11 (RLVa-1.3.0h) | Modified: RLVa-1.3.0h
#ifdef CATZNIP_STRINGVIEW
void RlvUtil::notifyBlocked(const std::string_view& strNotifcation, const LLSD& sdArgs, bool fLogToChat)
#else
void RlvUtil::notifyBlocked(const std::string& strNotifcation, const LLSD& sdArgs, bool fLogToChat)
#endif // CATZNIP_STRINGVIEW
{
    std::string strMsg = RlvStrings::getString(strNotifcation);
    if (sdArgs.isMap())
    {
        LLSD sdSafeArgs = LLSD::emptyMap();
        for (LLSD::map_const_iterator itArg = sdArgs.beginMap(); itArg != sdArgs.endMap(); ++itArg)
        {
            const std::string strValue = itArg->second.asString();
            sdSafeArgs[itArg->first] = (rlvIsOwnSLURL(strValue)) ? strValue : plus::safeText(strValue);
        }
        LLStringUtil::format(strMsg, sdSafeArgs);
    }

    LLSD sdNotify;
    sdNotify["MESSAGE"] = strMsg;
    if (!fLogToChat)
        LLNotificationsUtil::add("SystemMessageTip", sdNotify);
    else
        LLNotificationsUtil::add("ChatSystemMessageTip", sdNotify);
}

// Checked: 2010-11-11 (RLVa-1.2.1g) | Added: RLVa-1.2.1g
void RlvUtil::notifyFailedAssertion(const std::string& strAssert, const std::string& strFile, int nLine)
{
    // Don't show the same assertion over and over, or if the user opted out
    static std::string strAssertPrev, strFilePrev; static int nLinePrev;
    if ( ((strAssertPrev == strAssert) && (strFile == strFilePrev) && (nLine == nLinePrev)) ||
         (!RlvSettings::getShowAssertionFailures()) )
    {
        return;
    }

    strAssertPrev = strAssert;
    strFilePrev = strFile;
    nLinePrev = nLine;

    LLSD argsNotify;
    argsNotify["MESSAGE"] = llformat("RLVa assertion failure: %s (%s - %d)", strAssert.c_str(), strFile.c_str(), nLine);
    LLNotificationsUtil::add("SystemMessageTip", argsNotify);
}

// Checked: 2010-03-27 (RLVa-1.2.0b) | Modified: RLVa-1.2.0b
bool RlvUtil::sendBusyMessage(const LLUUID& idTo, const std::string& strMsg, const LLUUID& idSession, bool fThrottle)
{
    if (idTo.isNull())
        return false;

    // Automatic responses go out at most once per interval per recipient (an avatar sending IMs in a
    // loop must not be able to make us send IMs in a loop); the first one goes out as in FS.
    if (fThrottle)
    {
        static std::map<LLUUID, F64> s_LastResponse;
        const F64 tmNow = LLFrameTimer::getTotalSeconds();

        auto itLast = s_LastResponse.find(idTo);
        if ( (s_LastResponse.end() != itLast) && (tmNow - itLast->second < RLV_AUTORESPONSE_INTERVAL) )
        {
            RLV_DEBUGS << "Held back an automatic response to " << idTo << RLV_ENDL;
            return false;
        }

        if (s_LastResponse.size() >= 256)
        {
            // Forget everyone whose interval has run out
            for (auto itEntry = s_LastResponse.begin(); itEntry != s_LastResponse.end(); )
                itEntry = (tmNow - itEntry->second >= RLV_AUTORESPONSE_INTERVAL) ? s_LastResponse.erase(itEntry) : std::next(itEntry);
        }
        s_LastResponse[idTo] = tmNow;
    }

    // [See process_improved_im()]
    std::string strFullName;
    LLAgentUI::buildFullname(strFullName);

    pack_instant_message(gMessageSystem, gAgent.getID(), false, gAgent.getSessionID(), idTo, strFullName,
        utf8str_truncate(strMsg, MAX_MSG_BUF_SIZE - 1), IM_ONLINE, IM_DO_NOT_DISTURB_AUTO_RESPONSE, idSession);
    gAgent.sendReliableMessage();
    return true;
}

// Checked: 2010-03-09 (RLVa-1.2.0a) | Modified: RLVa-1.0.1e
namespace
{
    // One second's worth of replies
    struct RlvReplyWindow
    {
        F64  mStart = -1.0;
        U32  mCount = 0;
        bool mLogged = false;

        // Starts a new second if this one is over; true while fewer than nMax were counted
        bool hasRoom(F64 tmNow, U32 nMax)
        {
            if (tmNow - mStart >= 1.0)
            {
                mStart = tmNow;
                mCount = 0;
                mLogged = false;
            }
            return mCount < nMax;
        }
    };

    constexpr size_t RLV_REPLY_OBJECTS_MAX = 64;    // per-object windows kept before the finished ones are dropped
}

bool RlvUtil::checkReplyBudget(const LLUUID& idObject)
{
    static RlvReplyWindow s_AllReplies;
    static std::map<LLUUID, RlvReplyWindow> s_ObjectReplies;

    const F64 tmNow = LLFrameTimer::getTotalSeconds();
    if (!s_AllReplies.hasRoom(tmNow, RLV_MAX_REPLIES_PER_SECOND))
    {
        if (!s_AllReplies.mLogged)
        {
            RLV_DEBUGS << "More than " << RLV_MAX_REPLIES_PER_SECOND << " replies this second, dropping the rest" << RLV_ENDL;
            s_AllReplies.mLogged = true;
        }
        return false;
    }

    RlvReplyWindow* pObjectWindow = nullptr;
    if (idObject.notNull())
    {
        if ( (s_ObjectReplies.size() >= RLV_REPLY_OBJECTS_MAX) && (s_ObjectReplies.end() == s_ObjectReplies.find(idObject)) )
        {
            // Keep the table small: forget every object whose second is over
            for (auto itEntry = s_ObjectReplies.begin(); itEntry != s_ObjectReplies.end(); )
                itEntry = (tmNow - itEntry->second.mStart >= 1.0) ? s_ObjectReplies.erase(itEntry) : std::next(itEntry);
        }

        pObjectWindow = &s_ObjectReplies[idObject];
        if (!pObjectWindow->hasRoom(tmNow, RLV_MAX_OBJECT_REPLIES_PER_SECOND))
        {
            if (!pObjectWindow->mLogged)
            {
                RLV_DEBUGS << "More than " << RLV_MAX_OBJECT_REPLIES_PER_SECOND << " replies this second for " << idObject << ", dropping the rest" << RLV_ENDL;
                pObjectWindow->mLogged = true;
            }
            return false;
        }
    }

    s_AllReplies.mCount++;
    if (pObjectWindow)
        pObjectWindow->mCount++;
    return true;
}

bool RlvUtil::sendChatReply(S32 nChannel, const std::string& strUTF8Text, bool fBudget, const LLUUID& idObject)
{
    if (!isValidReplyChannel(nChannel))
        return false;
    if ( (fBudget) && (!checkReplyBudget(idObject)) )
        return false;

    // Copy/paste from send_chat_from_viewer()
    gMessageSystem->newMessageFast(_PREHASH_ChatFromViewer);
    gMessageSystem->nextBlockFast(_PREHASH_AgentData);
    gMessageSystem->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    gMessageSystem->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    gMessageSystem->nextBlockFast(_PREHASH_ChatData);
    gMessageSystem->addStringFast(_PREHASH_Message, utf8str_truncate(strUTF8Text, MAX_MSG_STR_LEN));
    gMessageSystem->addU8Fast(_PREHASH_Type, CHAT_TYPE_SHOUT);
    gMessageSystem->addS32("Channel", nChannel);
    gAgent.sendReliableMessage();
    add(LLStatViewer::CHAT_COUNT, 1);

    return true;
}

bool RlvUtil::sendChatReplySplit(S32 nChannel, const std::string& strMsg, char chSplit, const LLUUID& idObject)
{
    std::list<std::string> msgList;
    utf8str_split(msgList, strMsg, MAX_MSG_STR_LEN, chSplit);
    for (const std::string& strMsgPart : msgList)
    {
        if (!sendChatReply(nChannel, strMsgPart, true, idObject))
        {
            return false;
        }
    }
    return true;
}

void RlvUtil::sendIMMessage(const LLUUID& idRecipient, const std::string& strMsg, char chSplit, size_t nMaxMessages)
{
    if ( (idRecipient.isNull()) || (0 == nMaxMessages) )
        return;

    const LLUUID idSession = gIMMgr->computeSessionID(IM_NOTHING_SPECIAL, idRecipient);
    const LLRelationship* pBuddyInfo = LLAvatarTracker::instance().getBuddyInfo(idRecipient);
    std::string strAgentName;
    LLAgentUI::buildFullname(strAgentName);

    std::list<std::string> msgList;
    utf8str_split(msgList, strMsg, MAX_MSG_STR_LEN, chSplit);
    if (msgList.size() > nMaxMessages)
    {
        static const std::string strTruncated = "\n(truncated)";
        msgList.resize(nMaxMessages);
        std::string& strLast = msgList.back();
        strLast = utf8str_truncate(strLast, (S32)(MAX_MSG_STR_LEN - strTruncated.size())) + strTruncated;
    }

    for (const std::string& strMsgPart : msgList)
    {
        pack_instant_message(
            gMessageSystem,
            gAgent.getID(),
            false,
            gAgent.getSessionID(),
            idRecipient,
            strAgentName.c_str(),
            strMsgPart.c_str(),
            ((!pBuddyInfo) || (pBuddyInfo->isOnline())) ? IM_ONLINE : IM_OFFLINE,
            IM_NOTHING_SPECIAL,
            idSession);
        gAgent.sendReliableMessage();
    }
}

// ============================================================================
// Generic menu enablers
//

// Checked: 2015-05-25 (RLVa-1.5.0)
bool rlvMenuMainToggleVisible(LLUICtrl* pMenuCtrl)
{
    LLMenuItemCheckGL* pMenuItem = dynamic_cast<LLMenuItemCheckGL*>(pMenuCtrl);
    if (pMenuItem)
    {
        static std::string strLabel = pMenuItem->getLabel();
        if ((bool)gSavedSettings.get<bool>(RlvSettingNames::Main) == rlv_handler_t::isEnabled())
            pMenuItem->setLabel(strLabel);
        else
            pMenuItem->setLabel(strLabel + " " + LLTrans::getString("RLVaPendingRestart"));
    }
    return true;
}

// Checked: 2011-08-16 (RLVa-1.4.0b) | Added: RLVa-1.4.0b
// NOTE: the menu names are FS's; rlv-ui builds the Charis "RLVa" menu and adjusts them if they differ
void rlvMenuToggleVisible()
{
    if (!gMenuBarView)
        return;

    bool fTopLevel = RlvSettings::getTopLevelMenu();
    bool fRlvEnabled = rlv_handler_t::isEnabled();

    LLMenuGL* pRLVaMenuMain = gMenuBarView->findChildMenuByName("RLVa Main", false);
    LLMenuGL* pAdvancedMenu = gMenuBarView->findChildMenuByName("Advanced", false);
    LLMenuGL* pRLVaMenuEmbed = (pAdvancedMenu) ? pAdvancedMenu->findChildMenuByName("RLVa Embedded", false) : nullptr;

    gMenuBarView->setItemVisible("RLVa Main", (fRlvEnabled) && (fTopLevel));
    if (pAdvancedMenu)
        pAdvancedMenu->setItemVisible("RLVa Embedded", (fRlvEnabled) && (!fTopLevel));

    if ( (fRlvEnabled) && (pRLVaMenuMain) && (pRLVaMenuEmbed) &&
         ( ((fTopLevel) && (1 == pRLVaMenuMain->getItemCount())) || ((!fTopLevel) && (1 == pRLVaMenuEmbed->getItemCount())) ) )
    {
        LLMenuGL* pMenuFrom = (fTopLevel) ? pRLVaMenuEmbed : pRLVaMenuMain;
        LLMenuGL* pMenuTo = (fTopLevel) ? pRLVaMenuMain : pRLVaMenuEmbed;
        while (LLMenuItemGL* pItem = pMenuFrom->getItem(1))
        {
            pMenuFrom->removeChild(pItem);
            pMenuTo->addChild(pItem);
            pItem->updateBranchParent(pMenuTo);
        }
    }
}

bool rlvMenuCanShowName()
{
    bool fEnable = true;
    if (rlv_handler_t::isEnabled())
    {
        const LLVOAvatar* pAvatar = find_avatar_from_object(LLSelectMgr::getInstance()->getSelection()->getPrimaryObject());
        fEnable = (pAvatar) && (RlvActions::canShowName(RlvActions::SNC_DEFAULT, pAvatar->getID()));
    }
    return fEnable;
}

// Checked: 2010-04-23 (RLVa-1.2.0g) | Modified: RLVa-1.2.0g
bool rlvMenuEnableIfNot(const LLSD& sdParam)
{
    bool fEnable = true;
    if (rlv_handler_t::isEnabled())
    {
        ERlvBehaviour eBhvr = RlvBehaviourDictionary::instance().getBehaviourFromString(sdParam.asString(), RLV_TYPE_ADDREM);
        fEnable = (eBhvr != RLV_BHVR_UNKNOWN) ? !gRlvHandler.hasBehaviour(eBhvr) : true;
    }
    return fEnable;
}

// ============================================================================
// Selection functors
//

// Checked: 2011-05-28 (RLVa-1.4.6) | Modified: RLVa-1.4.0
// Charis: stage 1-2 - only the @unsit part (@edit / @rez / RlvActions::canEdit() are stage 3)
bool rlvCanDeleteOrReturn(const LLViewerObject* pObj)
{
    // Block if: @unsit=n restricted and being sat on by us
    return (!pObj) || (!gRlvHandler.hasBehaviour(RLV_BHVR_UNSIT)) || (!isAgentAvatarValid()) || (!pObj->getRootEdit()->isChild(gAgentAvatarp));
}

// Checked: 2011-05-28 (RLVa-1.4.6) | Modified: RLVa-1.4.0
bool rlvCanDeleteOrReturn()
{
    if (gRlvHandler.hasBehaviour(RLV_BHVR_UNSIT))
    {
        struct RlvCanDeleteOrReturn : public LLSelectedObjectFunctor
        {
            /*virtual*/ bool apply(LLViewerObject* pObj) { return rlvCanDeleteOrReturn(pObj); }
        } f;
        LLObjectSelectionHandle hSel = LLSelectMgr::getInstance()->getSelection();
        return (hSel.notNull()) && (0 != hSel->getRootObjectCount()) && (hSel->applyToRootObjects(&f, false));
    }
    return true;
}

// Checked: 2010-04-20 (RLVa-1.2.0f) | Modified: RLVa-0.2.0f
bool RlvSelectHasLockedAttach::apply(LLSelectNode* pNode)
{
    return (pNode->getObject()) ? gRlvAttachmentLocks.isLockedAttachment(pNode->getObject()->getRootEdit()) : false;
}

// Checked: 2011-05-28 (RLVa-1.4.0a) | Modified: RLVa-1.4.0a
bool RlvSelectIsSittingOn::apply(LLSelectNode* pNode)
{
    // LLViewerObject::isChild() only compares pointers (FS made it take a const pointer)
    return (pNode->getObject()) && (pNode->getObject()->getRootEdit()->isChild(const_cast<LLVOAvatar*>(m_pAvatar)));
}

// ============================================================================
// Predicates
//

// Checked: 2010-11-11 (RLVa-1.2.1g) | Modified: RLVa-1.2.1g
bool rlvPredCanWearItem(const LLViewerInventoryItem* pItem, ERlvWearMask eWearMask)
{
    if ( (pItem) && (RlvForceWear::isWearableItem(pItem)) )
    {
        if (RlvForceWear::isWearingItem(pItem))
            return true; // Special exception for currently worn items
        switch (pItem->getType())
        {
            case LLAssetType::AT_BODYPART:
                // NOTE: only one body part of each type is allowed so the only way to wear one is if we can replace the current one
                return (RLV_WEAR_LOCKED != (gRlvWearableLocks.canWear(pItem) & RLV_WEAR_REPLACE & eWearMask));
            case LLAssetType::AT_CLOTHING:
                return (RLV_WEAR_LOCKED != (gRlvWearableLocks.canWear(pItem) & eWearMask));
            case LLAssetType::AT_OBJECT:
                return (RLV_WEAR_LOCKED != (gRlvAttachmentLocks.canAttach(pItem) & eWearMask));
            case LLAssetType::AT_GESTURE:
                return true;
            default:
                RLV_ASSERT(false);
        }
    }
    return false;
}

// Checked: 2010-03-22 (RLVa-1.2.0c) | Added: RLVa-1.2.0a
bool rlvPredCanNotWearItem(const LLViewerInventoryItem* pItem, ERlvWearMask eWearMask)
{
    return !rlvPredCanWearItem(pItem, eWearMask);
}

// Checked: 2014-11-02 (RLVa-1.4.11)
bool rlvPredCanRemoveItem(const LLUUID& idItem)
{
    // Check the inventory item if it's available
    const LLViewerInventoryItem* pItem = gInventory.getItem(idItem);
    if (pItem)
    {
        return rlvPredCanRemoveItem(pItem);
    }

    // Temporary attachments don't have inventory items associated with them so check the attachment itself
    if (isAgentAvatarValid())
    {
        const LLViewerObject* pAttachObj = gAgentAvatarp->getWornAttachment(idItem);
        return (pAttachObj) && (!gRlvAttachmentLocks.isLockedAttachment(pAttachObj));
    }

    return false;
}

// Checked: 2010-03-22 (RLVa-1.2.0)
bool rlvPredCanRemoveItem(const LLViewerInventoryItem* pItem)
{
    if (pItem)
    {
        switch (pItem->getType())
        {
            case LLAssetType::AT_BODYPART:
            case LLAssetType::AT_CLOTHING:
                return gRlvWearableLocks.canRemove(pItem);
            case LLAssetType::AT_OBJECT:
                return gRlvAttachmentLocks.canDetach(pItem);
            case LLAssetType::AT_GESTURE:
                return true;
            case LLAssetType::AT_LINK:
            case LLAssetType::AT_LINK_FOLDER:
                // Broken links can always be removed since they don't represent a worn item
                return true;
            default:
                RLV_ASSERT(!RlvForceWear::isWearableItem(pItem));
        }
    }
    return false;
}

// Checked: 2010-03-22 (RLVa-1.2.0c) | Added: RLVa-1.2.0a
bool rlvPredCanNotRemoveItem(const LLViewerInventoryItem* pItem)
{
    return !rlvPredCanRemoveItem(pItem);
}

// Checked: 2010-04-24 (RLVa-1.2.0f) | Added: RLVa-1.2.0f
RlvPredIsEqualOrLinkedItem::RlvPredIsEqualOrLinkedItem(const LLUUID& idItem)
{
    m_pItem = gInventory.getItem(idItem);
}

// Checked: 2010-04-24 (RLVa-1.2.0f) | Added: RLVa-1.2.0f
bool RlvPredIsEqualOrLinkedItem::operator()(const LLViewerInventoryItem* pItem) const
{
    return (m_pItem) && (pItem) && (m_pItem->getLinkedUUID() == pItem->getLinkedUUID());
}

// ============================================================================
// Various public helper functions
//

// Checked: 2010-11-01 (RLVa-1.2.2a) | Added: RLVa-1.2.2a
std::string rlvGetAnonym(const LLAvatarName& avName)
{
    return RlvStrings::getAnonym(avName);
}

// ============================================================================
