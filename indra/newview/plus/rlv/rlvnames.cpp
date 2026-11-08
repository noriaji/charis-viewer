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

// Name and location filtering for @shownames / @showloc.
//
// FS's RlvUtil::filterNames() walks LLWorld::getAvatars() and compiles two regular expressions per
// nearby avatar on every call (every chat line, every hover text update). Here the nearby avatars
// are kept in a table that is rebuilt at most once a second, and the result for a given text is
// reused while neither the table nor the set of hidden names changed. The anonyms are FS's. Unlike
// FS (one name after the other, each over the previous result) all names are replaced in a single
// pass over the original text, so an anonym is never searched for the next name (with short names
// whose anonyms hold them, FS's way grows the text exponentially).
//
// FS matches "\b<name>\b" with boost::regex (icase). That misses names that don't begin and end in
// a word character ("さくら", "★Shop★": "\b" then needs a word character *outside* the name), and on
// Windows boost's w32_regex_traits classify and case-fold UTF-8 bytes by the OS code page, so the
// result changes with the user's language settings (cp1252 even folds the continuation bytes 0x8A
// and 0x9A, so "お" matches "ず"). Names here are matched as literal text (nothing from a name is
// ever read as a pattern), ASCII case-insensitive with every other byte exact, and the word boundary
// is checked only on a side of the name that is an ASCII word character, against ASCII word
// characters only - "Bob" is still not found in "Bobby", but is found in "Bobさん".

#include "llviewerprecompiledheaders.h"
#include "llagent.h"
#include "llavatarnamecache.h"
#include "llframetimer.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llworld.h"

#include "rlvactions.h"
#include "rlvcommon.h"

#include <algorithm>
#include <unordered_map>

namespace
{
    constexpr F64    NAME_TABLE_TTL = 1.0;          // look for changed names (display name updates) once a second ...
    constexpr F64    NAME_TABLE_RETRY = 0.1;        // ... or sooner while a nearby avatar's name is still missing
                                                    // (someone arriving or leaving rebuilds the table on the next call)
    constexpr size_t RESULT_CACHE_MAX = 128;        // filtered texts kept
    constexpr size_t RESULT_TEXT_MAX = 4096;        // longer texts aren't cached

    // ------------------------------------------------------------------------
    // Literal name matching (tools/selftest/fuzz/rlv extracts these by marker: keep them std-only)
    //

    // ASCII letters, digits and '_' - what "\b" looks at; a UTF-8 byte never counts as a word character
    inline bool rlvIsNameWordByte(unsigned char ch)
    {
        return ((ch >= 'a') && (ch <= 'z')) || ((ch >= 'A') && (ch <= 'Z')) || ((ch >= '0') && (ch <= '9')) || ('_' == ch);
    }

    inline unsigned char rlvNameFoldByte(unsigned char ch)
    {
        return ((ch >= 'A') && (ch <= 'Z')) ? static_cast<unsigned char>(ch - 'A' + 'a') : ch;
    }

    // A name is looked for in running text only if it has at least 2 characters (UTF-8 continuation bytes don't
    // count) and more than 2 bytes (as FS): a one-character name ("桜") would blank out every such character.
    // (Name tags and sender names are replaced as a whole elsewhere and don't go through this.)
    bool rlvIsFilterableName(const std::string& strName)
    {
        if (strName.size() <= 2)
            return false;
        size_t cntChars = 0;
        for (const char ch : strName)
        {
            if ( (0x80 != (static_cast<unsigned char>(ch) & 0xC0)) && (++cntChars >= 2) )
                return true;
        }
        return false;
    }

    // Does strName occur in strText at pos (ASCII case-insensitive, other bytes exact)? A side of the name that is
    // an ASCII word character must not touch another one in the text ("\b"); a side that isn't (Japanese, symbols,
    // spaces) has no boundary.
    bool rlvIsNameAt(const std::string& strText, size_t pos, const std::string& strName)
    {
        const size_t cchName = strName.size(), cchText = strText.size();
        if ( (0 == cchName) || (pos > cchText) || (cchText - pos < cchName) )
            return false;
        if (rlvNameFoldByte(strText[pos]) != rlvNameFoldByte(strName[0]))
            return false;
        for (size_t idx = 1; idx < cchName; idx++)
        {
            if (rlvNameFoldByte(strText[pos + idx]) != rlvNameFoldByte(strName[idx]))
                return false;
        }
        const size_t posEnd = pos + cchName;
        if ( (rlvIsNameWordByte(strName.front())) && (pos > 0) && (rlvIsNameWordByte(strText[pos - 1])) )
            return false;
        if ( (rlvIsNameWordByte(strName.back())) && (posEnd < cchText) && (rlvIsNameWordByte(strText[posEnd])) )
            return false;
        return true;
    }

    // A name to look for and what replaces it (both outlive the call)
    struct RlvNamePattern
    {
        const std::string* mName;
        const std::string* mReplace;
    };

    // The output may grow to RLV_NAME_GROWTH times the text (+ slack); a text that would grow more is cut
    constexpr size_t RLV_NAME_GROWTH = 8;
    constexpr size_t RLV_NAME_SLACK = 1024;

    // Replaces the mentions of all patterns in one left-to-right pass over the original text. The output is never
    // searched again, so a replacement can't be matched by another name (an anonym holding a short name would
    // otherwise make the text grow exponentially with the number of names). Where several patterns match at one
    // place the longest wins (a legacy name over the display name it holds, as FS's order does), then the one
    // listed first. Should the output still outgrow its limit the text is cut there and ends in "..." (nothing
    // after the cut is shown). Returns true if the text changed.
    bool rlvReplaceNames(std::string& strText, const std::vector<RlvNamePattern>& patterns)
    {
        const size_t cchText = strText.size();
        if ( (0 == cchText) || (patterns.empty()) )
            return false;

        const size_t cchOutMax = cchText * RLV_NAME_GROWTH + RLV_NAME_SLACK;
        std::string strOut;
        size_t posCopied = 0;
        bool fChanged = false;
        for (size_t pos = 0; pos < cchText; )
        {
            const RlvNamePattern* pBest = nullptr;
            for (const RlvNamePattern& pattern : patterns)
            {
                if ( ((!pBest) || (pattern.mName->size() > pBest->mName->size())) && (rlvIsNameAt(strText, pos, *pattern.mName)) )
                    pBest = &pattern;
            }
            if (!pBest)
            {
                pos++;
                continue;
            }

            if (!fChanged)
            {
                strOut.reserve(cchText);
                fChanged = true;
            }
            strOut.append(strText, posCopied, pos - posCopied);
            if (strOut.size() + pBest->mReplace->size() > cchOutMax)
            {
                strOut.append("...");
                strText.swap(strOut);
                return true;
            }
            strOut.append(*pBest->mReplace);
            pos = posCopied = pos + pBest->mName->size();
        }
        if (!fChanged)
            return false;
        strOut.append(strText, posCopied, std::string::npos);
        strText.swap(strOut);
        return true;
    }

    // ------------------------------------------------------------------------
    // Nearby names
    //

    struct NameEntry
    {
        LLUUID       mId;
        std::string  mDisplayName;  // as returned by the name cache (to notice changes)
        std::string  mLegacyName;
        std::string  mAnonym;
        bool         mHasDisplay = false;   // rlvIsFilterableName()
        bool         mHasLegacy = false;
    };

    struct NameTable
    {
        std::vector<NameEntry> mEntries;    // avatars whose names are known
        std::vector<LLUUID>    mNearby;     // every nearby avatar (sorted), named or not
        uuid_vec_t             mLastIds;    // LLWorld::getAvatars() as of the last rebuild (unsorted, to compare)
        F64  mBuiltAt = -1.0;
        bool mIncomplete = false;           // some nearby avatar's name wasn't cached yet
        U32  mGeneration = 0;               // changes whenever mEntries changes
        bool mValid = false;

        // Filter results: key = flags + text
        std::unordered_map<std::string, std::string> mResults;
        U32         mResultsGeneration = 0;
        std::string mResultsHiddenMask;     // which entries were hidden when the results were made
        std::string mHiddenMask;            // scratch (reused)
        uuid_vec_t  mScratchIds;            // scratch (reused)
        std::vector<RlvNamePattern> mScratchPatterns;   // scratch (reused)
    };

    NameTable& nameTable()
    {
        static NameTable s_Table;
        return s_Table;
    }

    // tbl.mScratchIds holds the current LLWorld::getAvatars()
    bool isTableStale(const NameTable& tbl, F64 tmNow)
    {
        return (!tbl.mValid) || (tbl.mScratchIds != tbl.mLastIds) ||
               (tmNow - tbl.mBuiltAt >= NAME_TABLE_TTL) || ((tbl.mIncomplete) && (tmNow - tbl.mBuiltAt >= NAME_TABLE_RETRY));
    }

    // Rebuilds from tbl.mScratchIds (the current LLWorld::getAvatars())
    void rebuildNameTable(NameTable& tbl, F64 tmNow)
    {
        tbl.mLastIds = tbl.mScratchIds;
        tbl.mNearby = tbl.mScratchIds;
        std::sort(tbl.mNearby.begin(), tbl.mNearby.end());

        std::vector<NameEntry> entries;
        entries.reserve(tbl.mScratchIds.size());
        bool fIncomplete = false, fChanged = (!tbl.mValid);
        for (const LLUUID& idAgent : tbl.mScratchIds)
        {
            LLAvatarName avName;
            if (!LLAvatarNameCache::get(idAgent, &avName))
            {
                fIncomplete = true;
                continue;
            }

            const std::string strDisplayName = avName.getDisplayName();
            const std::string strLegacyName = avName.getLegacyName();

            // Keep the entry if this avatar's names haven't changed
            auto itOld = std::find_if(tbl.mEntries.begin(), tbl.mEntries.end(), [&idAgent](const NameEntry& e) { return e.mId == idAgent; });
            if ( (tbl.mEntries.end() != itOld) && (itOld->mDisplayName == strDisplayName) && (itOld->mLegacyName == strLegacyName) )
            {
                // The cached results are keyed on entry positions, so a reordering counts as a change
                if (static_cast<size_t>(itOld - tbl.mEntries.begin()) != entries.size())
                    fChanged = true;
                entries.push_back(std::move(*itOld));
                continue;
            }

            fChanged = true;
            NameEntry entry;
            entry.mId = idAgent;
            entry.mDisplayName = strDisplayName;
            entry.mLegacyName = strLegacyName;
            entry.mAnonym = RlvStrings::getAnonym(avName);

            entry.mHasDisplay = rlvIsFilterableName(strDisplayName);
            entry.mHasLegacy = rlvIsFilterableName(strLegacyName);
            entries.push_back(std::move(entry));
        }
        if (entries.size() != tbl.mEntries.size())
            fChanged = true;

        tbl.mEntries.swap(entries);
        tbl.mIncomplete = fIncomplete;
        tbl.mBuiltAt = tmNow;
        tbl.mValid = true;
        if (fChanged)
            tbl.mGeneration++;
    }

    NameTable& currentNameTable()
    {
        NameTable& tbl = nameTable();
        const F64 tmNow = LLFrameTimer::getElapsedSeconds();

        // Who is nearby is checked on every call (the vector keeps its capacity, so no allocation once warm)
        // so that someone who just arrived is hidden at once; comparing is a size check plus an element walk
        tbl.mScratchIds.clear();
        LLWorld::getInstance()->getAvatars(&tbl.mScratchIds, nullptr);

        if (isTableStale(tbl, tmNow))
            rebuildNameTable(tbl, tmNow);
        return tbl;
    }

    // All hidden names in one pass (FS replaced name by name, each over the previous result; the longest-match rule
    // of rlvReplaceNames() gives FS's "legacy name first if it holds the display name")
    void applyNameTable(NameTable& tbl, std::string& strUTF8Text, bool fFilterLegacy, bool fClearMatches)
    {
        std::vector<RlvNamePattern>& patterns = tbl.mScratchPatterns;
        patterns.clear();
        for (size_t idxEntry = 0, cntEntry = tbl.mEntries.size(); idxEntry < cntEntry; idxEntry++)
        {
            if ('1' != tbl.mHiddenMask[idxEntry])
                continue;

            const NameEntry& entry = tbl.mEntries[idxEntry];
            const std::string* pAnonym = (!fClearMatches) ? &entry.mAnonym : &LLStringUtil::null;
            if ( (fFilterLegacy) && (entry.mHasLegacy) )
                patterns.push_back({ &entry.mLegacyName, pAnonym });
            if (entry.mHasDisplay)
                patterns.push_back({ &entry.mDisplayName, pAnonym });
        }
        rlvReplaceNames(strUTF8Text, patterns);
    }
}

// ============================================================================
// RlvNearbyNames
//

void RlvNearbyNames::invalidate()
{
    NameTable& tbl = nameTable();
    tbl.mValid = false;
    tbl.mResults.clear();
    tbl.mResultsHiddenMask.clear();
}

void RlvNearbyNames::cleanupClass()
{
    NameTable& tbl = nameTable();
    tbl.mEntries.clear();
    tbl.mNearby.clear();
    tbl.mLastIds.clear();
    tbl.mScratchIds.clear();
    tbl.mScratchPatterns.clear();
    tbl.mResults.clear();
    tbl.mValid = false;
}

// ============================================================================
// RlvUtil - name and location filtering
//

// Checked: 2009-07-04 (RLVa-1.0.0a) | Modified: RLVa-1.0.0a
void RlvUtil::filterLocation(std::string& strUTF8Text)
{
    if (strUTF8Text.empty())
        return;

    // Nothing is compiled or cached: the region and parcel names are read as they are now on every call, and all
    // of them are replaced in one pass (rlvReplaceNames; a parcel name holding a region name is hidden whole).
    // An empty name never matches - FS's "\b\b" would litter the text with "(Hidden ...)". Place names have no
    // minimum length (unlike avatar names): a short parcel name is still hidden.
    static std::vector<RlvNamePattern> s_Patterns;  // reused (main thread only)
    s_Patterns.clear();

    // The surrounding region names
    const std::string& strHiddenRegion = RlvStrings::getString(RlvStringKeys::Hidden::Region);
    for (const LLViewerRegion* pRegion : LLWorld::getInstance()->getRegionList())
    {
        if (pRegion)
            s_Patterns.push_back({ &pRegion->getName(), &strHiddenRegion });
    }

    // The parcel name
    if (LLViewerParcelMgr* pParcelMgr = LLViewerParcelMgr::getInstance())
        s_Patterns.push_back({ &pParcelMgr->getAgentParcelName(), &RlvStrings::getString(RlvStringKeys::Hidden::Parcel) });

    rlvReplaceNames(strUTF8Text, s_Patterns);
    s_Patterns.clear();     // no pointers kept past the call
}

// Checked: 2010-12-08 (RLVa-1.2.2c) | Modified: RLVa-1.2.2c
void RlvUtil::filterNames(std::string& strUTF8Text, bool fFilterLegacy, bool fClearMatches)
{
    if (!strUTF8Text.empty())
    {
        NameTable& tbl = currentNameTable();

        // Which of the nearby avatars are hidden right now (exceptions can change at any time)
        // NOTE: if we're agressively culling nearby names then ignore exceptions
        tbl.mHiddenMask.assign(tbl.mEntries.size(), '0');
        bool fAnyHidden = false;
        for (size_t idxEntry = 0, cntEntry = tbl.mEntries.size(); idxEntry < cntEntry; idxEntry++)
        {
            if ( (fClearMatches) || (!RlvActions::canShowName(RlvActions::SNC_DEFAULT, tbl.mEntries[idxEntry].mId)) )
            {
                tbl.mHiddenMask[idxEntry] = '1';
                fAnyHidden = true;
            }
        }

        if (fAnyHidden)
        {
            if ( (tbl.mResultsGeneration != tbl.mGeneration) || (tbl.mResultsHiddenMask != tbl.mHiddenMask) )
            {
                tbl.mResults.clear();
                tbl.mResultsGeneration = tbl.mGeneration;
                tbl.mResultsHiddenMask = tbl.mHiddenMask;
            }

            if (strUTF8Text.size() <= RESULT_TEXT_MAX)
            {
                std::string strKey;
                strKey.reserve(strUTF8Text.size() + 2);
                strKey.push_back((fFilterLegacy) ? 'L' : '-');
                strKey.push_back((fClearMatches) ? 'C' : '-');
                strKey.append(strUTF8Text);

                auto itResult = tbl.mResults.find(strKey);
                if (tbl.mResults.end() != itResult)
                {
                    strUTF8Text = itResult->second;
                }
                else
                {
                    applyNameTable(tbl, strUTF8Text, fFilterLegacy, fClearMatches);
                    if (tbl.mResults.size() >= RESULT_CACHE_MAX)
                        tbl.mResults.clear();
                    tbl.mResults.emplace(std::move(strKey), strUTF8Text);
                }
            }
            else
            {
                applyNameTable(tbl, strUTF8Text, fFilterLegacy, fClearMatches);
            }
        }
    }

    filterMentions(strUTF8Text);
}

// Checked: 2010-04-22 (RLVa-1.2.0f) | Modified: RLVa-1.2.0f
bool RlvUtil::isNearbyAgent(const LLUUID& idAgent)
{
    // Sanity check since we call this with notification payloads as well and those strings tend to change from one release to another
    RLV_ASSERT(idAgent.notNull());
    if ( (idAgent.isNull()) || (gAgent.getID() == idAgent) )
        return false;

    const NameTable& tbl = currentNameTable();
    return std::binary_search(tbl.mNearby.begin(), tbl.mNearby.end(), idAgent);
}

// Checked: 2010-04-05 (RLVa-1.2.0d) | Modified: RLVa-1.2.0d
bool RlvUtil::isNearbyRegion(const std::string& strRegion)
{
    const LLWorld::region_list_t& regions = LLWorld::getInstance()->getRegionList();
    for (const LLViewerRegion* pRegion : regions)
    {
        if ( (pRegion) && (pRegion->getName() == strRegion) )
            return true;
    }
    return false;
}

// ============================================================================
