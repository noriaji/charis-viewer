/**
 * @file aotransfer.cpp
 * @brief Animation Overrider: sets to and from files (".charis-ao.xml"),
 *        and Firestorm / ZHAO notecards into sets.
 *
 * The notecard part is ported from Firestorm's aoengine.cpp (see SOURCE.md);
 * the files are new.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2011, Zi Ree @ Second Life
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

#include "aotransfer.h"

#include "aoengine.h"
#include "aoset.h"
#include "plus.h"

#include "llagent.h"
#include "llassetstorage.h"
#include "lldate.h"
#include "lldbstrings.h"
#include "llfile.h"
#include "llfilesystem.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llnotecard.h"
#include "llsdserialize.h"
#include "llviewerinventory.h"
#include "roles_constants.h"

#include <map>
#include <sstream>
#include <unordered_map>

namespace
{
    constexpr char FILE_FORMAT[]  = "charis-ao";
    constexpr S32  FILE_VERSION   = 2; // 2: groups and tracks (a version 1 file has neither)
    constexpr S32  MAX_LISTED     = 30; // missing animations named in the report

    // What one import reads at most. Each set costs folders and links on the server (AIS),
    // so a file of thousands (made by hand, or sent by someone) would keep it busy for long.
    constexpr size_t MAX_IMPORT_SETS       = 50;
    constexpr size_t MAX_STATE_ANIMATIONS  = 100;          // per state (and per group or track), from a file or a notecard
    constexpr size_t MAX_STATE_TRACKS      = 20;           // per state, from a file
    constexpr S32    MAX_IMPORT_ANIMATIONS = 2000;         // in all (one link each)
    constexpr S64    MAX_IMPORT_FILE_BYTES = 1024 * 1024;  // a file of 50 full sets is far smaller

    // The line added to the report when a limit left something out.
    std::string limitNote()
    {
        LLSD args;
        args["SETS"]  = (S32)MAX_IMPORT_SETS;
        args["ANIMS"] = (S32)MAX_STATE_ANIMATIONS;
        args["TOTAL"] = MAX_IMPORT_ANIMATIONS;
        return plus::ao::aoText("ao_import_limited", args);
    }

    // Adds item to a state of an import unless it is there already (the same line many times
    // would make as many links) or a limit is reached; limited is set then.
    void addImported(std::vector<LLPointer<LLViewerInventoryItem>>& items, const LLPointer<LLViewerInventoryItem>& item,
                     S32& found, bool& limited)
    {
        for (const LLPointer<LLViewerInventoryItem>& have : items)
        {
            if (have->getUUID() == item->getUUID())
            {
                return;
            }
        }
        if (items.size() >= MAX_STATE_ANIMATIONS || found >= MAX_IMPORT_ANIMATIONS)
        {
            limited = true;
            return;
        }
        items.push_back(item);
        ++found;
    }

    // Adds item as an entry of a state's main list (the notecard has no groups) under the same rules.
    void addImportedStep(plus::ao::NewSet::State& state, const LLPointer<LLViewerInventoryItem>& item, S32& found, bool& limited)
    {
        for (const plus::ao::NewSet::Step& step : state.mSteps)
        {
            for (const LLPointer<LLViewerInventoryItem>& have : step.mItems)
            {
                if (have->getUUID() == item->getUUID())
                {
                    return;
                }
            }
        }
        if (state.mSteps.size() >= MAX_STATE_ANIMATIONS || found >= MAX_IMPORT_ANIMATIONS)
        {
            limited = true;
            return;
        }
        plus::ao::NewSet::Step step;
        step.mItems.push_back(item);
        state.mSteps.push_back(std::move(step));
        ++found;
    }

    // A name read from a notecard or a file, as the report shows it: not without end.
    std::string reportName(const std::string& name)
    {
        return utf8str_truncate(name, 200);
    }

    // Any animation (not a link) the agent has, outside the trash.
    class AnimationCollector : public LLInventoryCollectFunctor
    {
    public:
        bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
        {
            return item && item->getActualType() == LLAssetType::AT_ANIMATION;
        }
    };

    // Looks animations up by asset id, item id, then name.
    class AnimationFinder
    {
    public:
        AnimationFinder()
        {
            plus::ao::LocalTiming timing("index the inventory's animations");

            LLInventoryModel::cat_array_t  cats;
            LLInventoryModel::item_array_t items;
            AnimationCollector             collector;
            gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items, LLInventoryModel::EXCLUDE_TRASH, collector);
            for (const LLPointer<LLViewerInventoryItem>& item : items)
            {
                mByAsset.emplace(item->getAssetUUID(), item);
                mByName.emplace(item->getName(), item);
            }
            mComplete = LLInventoryModelBackgroundFetch::instance().isEverythingFetched();
        }

        // The agent's inventory is not loaded yet, so some may be missed.
        bool isComplete() const { return mComplete; }

        LLPointer<LLViewerInventoryItem> find(const LLUUID& asset_id, const LLUUID& item_id, const std::string& name) const
        {
            if (asset_id.notNull())
            {
                // Several copies: the same item, else the same name, else any.
                auto [first, last] = mByAsset.equal_range(asset_id);
                LLPointer<LLViewerInventoryItem> found;
                for (auto it = first; it != last; ++it)
                {
                    if (it->second->getUUID() == item_id)
                    {
                        return it->second;
                    }
                    if (found.isNull() || (found->getName() != name && it->second->getName() == name))
                    {
                        found = it->second;
                    }
                }
                if (found.notNull())
                {
                    return found;
                }
            }

            if (item_id.notNull())
            {
                LLViewerInventoryItem* item = gInventory.getItem(item_id);
                if (item && item->getActualType() == LLAssetType::AT_ANIMATION &&
                    !gInventory.isObjectDescendentOf(item_id, gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH)))
                {
                    return item;
                }
            }

            if (!name.empty())
            {
                if (auto it = mByName.find(name); it != mByName.end())
                {
                    return it->second;
                }
            }
            return nullptr;
        }

    private:
        std::unordered_multimap<LLUUID, LLPointer<LLViewerInventoryItem>> mByAsset;
        std::multimap<std::string, LLPointer<LLViewerInventoryItem>>      mByName;
        bool                                                              mComplete = true;
    };

    // A set name that is valid and not taken, from any wanted name.
    std::string newSetName(const std::string& wanted, const std::string& fallback)
    {
        std::string name = wanted;
        for (char& c : name)
        {
            if (c == ':' || c == '|' || (unsigned char)c < 0x20 || c == 0x7f)
            {
                c = '-';
            }
        }
        LLStringUtil::trim(name);
        // Room for the flags and a " 99" from uniqueSetName().
        name = utf8str_truncate(name, DB_INV_ITEM_NAME_STR_LEN - 12 - 3);
        LLStringUtil::trim(name);
        if (!plus::ao::AOSet::isValidName(name))
        {
            name = fallback;
        }
        return plus::ao::AOEngine::instance().uniqueSetName(name);
    }

    std::string joinMissing(const std::vector<std::string>& missing)
    {
        std::string text;
        for (size_t i = 0; i < missing.size() && i < (size_t)MAX_LISTED; ++i)
        {
            text += missing[i] + "\n";
        }
        if (missing.size() > (size_t)MAX_LISTED)
        {
            LLSD args;
            args["COUNT"] = (S32)(missing.size() - MAX_LISTED);
            text += plus::ao::aoText("ao_import_more", args) + "\n";
        }
        return text;
    }

    // ------------------------------------------------------------------------
    // Notecards (Firestorm)

    struct NotecardImport
    {
        std::string mName;   // the set's name: the notecard's
        LLUUID      mFolder; // the animations are next to the notecard
    };

    void parseNotecard(const NotecardImport& import, const std::string& text)
    {
        LLViewerInventoryCategory* folder = gInventory.getCategory(import.mFolder);
        if (!folder)
        {
            plus::ao::notifyUser("ao_import_no_folder", LLSD(), true);
            return;
        }

        // The notecard's neighbours by name.
        std::map<std::string, LLPointer<LLViewerInventoryItem>> neighbours;
        LLInventoryModel::cat_array_t*  cats  = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(import.mFolder, cats, items);
        if (items)
        {
            for (const LLPointer<LLViewerInventoryItem>& item : *items)
            {
                if (item->getInventoryType() == LLInventoryType::IT_ANIMATION)
                {
                    neighbours.emplace(item->getName(), item);
                }
            }
        }

        plus::ao::NewSet         description;
        std::vector<std::string> problems;
        S32                      found   = 0;
        bool                     limited = false;

        // [ State ]Anim1|Anim2|Anim3
        std::istringstream lines(text);
        std::string        line;
        for (S32 number = 1; std::getline(lines, line); ++number)
        {
            LLStringUtil::trim(line);
            if (line.empty() || line[0] == '#') // <ND/> FIRE-3801: comments are skipped
            {
                continue;
            }

            LLSD args;
            args["LINE"] = number;
            if (line[0] != '[')
            {
                problems.push_back(plus::ao::aoText("ao_import_no_state_prefix", args));
                continue;
            }
            const size_t end_tag = line.find(']');
            if (end_tag == std::string::npos)
            {
                problems.push_back(plus::ao::aoText("ao_import_no_delimiter", args));
                continue;
            }

            std::string state_name = line.substr(1, end_tag - 1);
            LLStringUtil::trim(state_name);
            const S32 state = plus::ao::stateByName(state_name);
            if (state < 0)
            {
                args["NAME"] = reportName(state_name);
                problems.push_back(plus::ao::aoText("ao_import_unknown_state", args));
                continue;
            }

            std::vector<std::string> names;
            LLStringUtil::getTokens(line.substr(end_tag + 1), names, "|,");
            for (const std::string& name : names)
            {
                auto it = neighbours.find(name);
                if (it == neighbours.end())
                {
                    args["NAME"] = reportName(name);
                    problems.push_back(plus::ao::aoText("ao_import_animation_not_found", args));
                    continue;
                }
                addImportedStep(description.mStates[state], it->second, found, limited);
            }
        }

        if (!found)
        {
            plus::ao::notifyUser("ao_import_invalid", LLSD(), true);
            return;
        }

        description.mName = newSetName(import.mName, "AO");
        if (!plus::ao::AOEngine::instance().addSet(description))
        {
            plus::ao::notifyUser("ao_import_failed", LLSD(), true);
            return;
        }

        LLSD args;
        args["NAME"]  = description.mName;
        args["COUNT"] = found;
        const std::string note = limited ? limitNote() : std::string();
        if (problems.empty())
        {
            plus::ao::notifyUser("ao_import_done", args, limited, note);
        }
        else
        {
            args["DETAILS"] = joinMissing(problems);
            plus::ao::notifyUser("ao_import_notecard_partial", args, true, note);
        }
    }

    void onNotecardLoaded(const NotecardImport& import, const LLUUID& asset_id, LLAssetType::EType type, S32 status)
    {
        if (!plus::ao::AOEngine::instanceExists())
        {
            return;
        }
        if (status != LL_ERR_NOERR)
        {
            plus::ao::notifyUser("ao_import_download_failed", LLSD(), true);
            return;
        }

        LLFileSystem file(asset_id, type, LLFileSystem::READ);
        const S32 size = file.getSize();
        if (size <= 0)  // fix-round53 4: before the string (a negative size throws)
        {
            plus::ao::notifyUser("ao_import_no_text", LLSD(), true);
            return;
        }
        std::string buffer(size, '\0');
        if (!file.read((U8*)buffer.data(), size))
        {
            plus::ao::notifyUser("ao_import_no_text", LLSD(), true);
            return;
        }

        LLNotecard         notecard;
        std::istringstream stream(buffer);
        if (!notecard.importStream(stream))
        {
            plus::ao::notifyUser("ao_import_no_text", LLSD(), true);
            return;
        }
        parseNotecard(import, notecard.getText());
    }

    // The asset id goes into an exported file only for an animation the agent
    // may copy, modify and transfer: a no-copy or no-transfer animation's asset
    // id is not handed around in a file (research/227 F8, TPV policy 2a). An
    // item not in the inventory (yet) counts as not full permission. The import
    // finds an animation without one by its item id, then by its name.
    bool isFullPermission(const LLUUID& item_id)
    {
        const LLViewerInventoryItem* item = gInventory.getItem(item_id);
        return item && item->getIsFullPerm();
    }
}

namespace plus::ao
{
    const char EXPORT_FILE_SUFFIX[] = ".charis-ao.xml";

    std::string exportFileName(const std::string& name)
    {
        return LLDir::getScrubbedFileName(name) + EXPORT_FILE_SUFFIX;
    }

    bool exportSets(const std::vector<const AOSet*>& sets, const std::string& path)
    {
        LocalTiming timing(llformat("export %d set(s)", (S32)sets.size()));

        LLSD root;
        root["format"]   = FILE_FORMAT;
        root["version"]  = FILE_VERSION;
        root["exported"] = LLDate::now();
        root["sets"]     = LLSD::emptyArray();

        for (const AOSet* set : sets)
        {
            LLSD set_sd;
            set_sd["name"]                    = set->getName();
            set_sd["sit_override"]            = set->getSitOverride();
            set_sd["smart"]                   = set->getSmart();
            set_sd["mouselook_stand_disable"] = set->getMouselookStandDisable();
            set_sd["states"]                  = LLSD::emptyMap();

            for (S32 i = 0; i < STATE_COUNT; ++i)
            {
                const AOState& state = set->getState(i);
                if (!state.hasContent() && !state.mCycle && !state.mRandom && state.mCycleTime <= 0.f)
                {
                    continue;
                }

                LLSD state_sd;
                state_sd["cycle"]      = state.mCycle;
                state_sd["random"]     = state.mRandom;
                state_sd["cycle_time"] = state.mCycleTime;
                state_sd["animations"] = LLSD::emptyArray();
                auto animation_sd = [](const AOAnimation& anim)
                {
                    LLSD anim_sd;
                    anim_sd["name"]     = anim.mName;
                    if (isFullPermission(anim.mOriginalID))
                    {
                        anim_sd["asset_id"] = anim.mAssetID;
                    }
                    anim_sd["item_id"]  = anim.mOriginalID;
                    return anim_sd;
                };
                for (const AOAnimation& anim : state.mAnimations)
                {
                    if (!anim.mIsGroup)
                    {
                        state_sd["animations"].append(animation_sd(anim));
                        continue;
                    }
                    LLSD group_sd;
                    group_sd["group"]   = anim.mName;
                    group_sd["members"] = LLSD::emptyArray();
                    for (const AOAnimation& member : anim.mMembers)
                    {
                        group_sd["members"].append(animation_sd(member));
                    }
                    state_sd["animations"].append(group_sd);
                }
                if (!state.mTracks.empty())
                {
                    state_sd["tracks"] = LLSD::emptyArray();
                    for (const AOTrack& track : state.mTracks)
                    {
                        LLSD track_sd;
                        track_sd["name"]       = track.mName;
                        track_sd["cycle"]      = track.mCycle;
                        track_sd["random"]     = track.mRandom;
                        track_sd["cycle_time"] = track.mCycleTime;
                        track_sd["animations"] = LLSD::emptyArray();
                        for (const AOAnimation& anim : track.mAnimations)
                        {
                            track_sd["animations"].append(animation_sd(anim));
                        }
                        state_sd["tracks"].append(track_sd);
                    }
                }
                set_sd["states"][state.getName()] = state_sd;
            }
            root["sets"].append(set_sd);
        }

        // Written next to the file, then moved over it: a failed write leaves an
        // existing file as it was (fix-round49).
        // The user picked the folder (aofloater's file picker): no tidying there (tidy false).
        const plus::FileWrite wrote = plus::writeFileReplacing(path, [&](std::ostream& out) { LLSDSerialize::toPrettyXML(root, out); },
                                                               std::ios_base::out | std::ios_base::trunc, 1, false);
        if (wrote == plus::FileWrite::cannot_replace)
        {
            LL_WARNS("PlusAO") << "Cannot replace " << path << LL_ENDL;
            return false;
        }
        if (wrote != plus::FileWrite::written)
        {
            LL_WARNS("PlusAO") << "Cannot write " << plus::tempPathFor(path) << LL_ENDL;
            return false;
        }
        LL_INFOS("PlusAO") << "Exported " << sets.size() << " set(s) to " << path << LL_ENDL;
        return true;
    }

    void importFile(const std::string& path)
    {
        if (LLFile::size(path) > MAX_IMPORT_FILE_BYTES)
        {
            LLSD args;
            args["FILE"] = path;
            notifyUser("ao_import_file_too_large", args, true);
            return;
        }
        LLSD root;
        {
            llifstream in(path.c_str());
            if (!in.is_open() || LLSDSerialize::fromXML(root, in) == LLSDParser::PARSE_FAILURE)
            {
                LLSD args;
                args["FILE"] = path;
                notifyUser("ao_import_file_unreadable", args, true);
                return;
            }
        }
        if (root["format"].asString() != FILE_FORMAT || !root["sets"].isArray())
        {
            LLSD args;
            args["FILE"] = path;
            notifyUser("ao_import_file_unreadable", args, true);
            return;
        }
        if (root["version"].asInteger() > FILE_VERSION)
        {
            LL_WARNS("PlusAO") << path << " is of a newer version (" << root["version"].asInteger() << "); reading what is known" << LL_ENDL;
        }

        LocalTiming     timing("import " + path);
        AnimationFinder finder;
        AOEngine&       engine = AOEngine::instance();

        std::vector<std::string> created;
        std::vector<std::string> missing;
        S32                      found   = 0;
        bool                     limited = root["sets"].size() > (S32)MAX_IMPORT_SETS;
        size_t                   read    = 0;

        for (const LLSD& set_sd : llsd::inArray(root["sets"]))
        {
            if (read++ >= MAX_IMPORT_SETS)
            {
                break;
            }
            NewSet description;
            description.mName                  = newSetName(set_sd["name"].asString(), "AO");
            description.mSitOverride           = set_sd["sit_override"].asBoolean();
            description.mSmart                 = set_sd["smart"].asBoolean();
            description.mMouselookStandDisable = set_sd["mouselook_stand_disable"].asBoolean();

            for (const auto& [state_name, state_sd] : llsd::inMap(set_sd["states"]))
            {
                const S32 state = stateByName(state_name);
                if (state < 0)
                {
                    LL_WARNS("PlusAO") << "Unknown state " << state_name << " in " << path << LL_ENDL;
                    continue;
                }

                NewSet::State& target = description.mStates[state];
                target.mCycle         = state_sd["cycle"].asBoolean();
                target.mRandom        = state_sd["random"].asBoolean();
                target.mCycleTime     = AOState::clampCycleTime((F32)state_sd["cycle_time"].asReal());

                // An animation of the file as an inventory item the agent has; null (and reported) if not.
                auto resolve = [&](const LLSD& anim_sd, const std::string& where) -> LLPointer<LLViewerInventoryItem>
                {
                    const std::string name = anim_sd["name"].asString();
                    LLPointer<LLViewerInventoryItem> item = finder.find(anim_sd["asset_id"].asUUID(), anim_sd["item_id"].asUUID(), name);
                    if (item.isNull())
                    {
                        missing.push_back(description.mName + " / " + where + " / " + reportName(name));
                    }
                    return item;
                };

                for (const LLSD& anim_sd : llsd::inArray(state_sd["animations"]))
                {
                    if (target.mSteps.size() >= MAX_STATE_ANIMATIONS || found >= MAX_IMPORT_ANIMATIONS)
                    {
                        limited = true;
                        break;
                    }
                    if (anim_sd.has("members"))
                    {
                        NewSet::Step step;
                        step.mIsGroup = true;
                        step.mName    = anim_sd["group"].asString();
                        for (const LLSD& member_sd : llsd::inArray(anim_sd["members"]))
                        {
                            if (limited || step.mItems.size() >= MAX_STATE_ANIMATIONS)
                            {
                                limited = true;
                                break;
                            }
                            if (LLPointer<LLViewerInventoryItem> item = resolve(member_sd, state_name + " / " + reportName(step.mName)); item.notNull())
                            {
                                addImported(step.mItems, item, found, limited);
                            }
                        }
                        if (!step.mItems.empty())
                        {
                            target.mSteps.push_back(std::move(step));
                        }
                        continue;
                    }
                    if (LLPointer<LLViewerInventoryItem> item = resolve(anim_sd, state_name); item.notNull())
                    {
                        addImportedStep(target, item, found, limited);
                    }
                }

                for (const LLSD& track_sd : llsd::inArray(state_sd["tracks"]))
                {
                    if (target.mTracks.size() >= MAX_STATE_TRACKS || found >= MAX_IMPORT_ANIMATIONS)
                    {
                        limited = true;
                        break;
                    }
                    NewSet::Track track;
                    track.mName      = track_sd["name"].asString();
                    track.mCycle     = track_sd["cycle"].asBoolean();
                    track.mRandom    = track_sd["random"].asBoolean();
                    track.mCycleTime = AOState::clampCycleTime((F32)track_sd["cycle_time"].asReal());
                    for (const LLSD& anim_sd : llsd::inArray(track_sd["animations"]))
                    {
                        if (track.mItems.size() >= MAX_STATE_ANIMATIONS || found >= MAX_IMPORT_ANIMATIONS)
                        {
                            limited = true;
                            break;
                        }
                        if (LLPointer<LLViewerInventoryItem> item = resolve(anim_sd, state_name + " / " + reportName(track.mName)); item.notNull())
                        {
                            addImported(track.mItems, item, found, limited);
                        }
                    }
                    // A track costs a folder: only those with animations.
                    if (!track.mItems.empty())
                    {
                        target.mTracks.push_back(std::move(track));
                    }
                }
            }

            if (engine.addSet(description))
            {
                created.push_back(description.mName);
            }
        }

        if (created.empty())
        {
            notifyUser("ao_import_failed", LLSD(), true);
            return;
        }

        std::string names;
        for (const std::string& name : created)
        {
            names += (names.empty() ? "" : ", ") + name;
        }
        LLSD args;
        args["NAME"]  = names;
        args["COUNT"] = found;
        const std::string note = limited ? limitNote() : std::string();
        if (missing.empty())
        {
            notifyUser("ao_import_done", args, limited, note);
            return;
        }

        std::string details = joinMissing(missing);
        args["DETAILS"] = details;
        // ours, not the file's: added as the note (not through safeText)
        std::string tail = finder.isComplete() ? std::string() : aoText("ao_import_inventory_loading", LLSD());
        if (!note.empty())
        {
            tail += (tail.empty() ? "" : "\n") + note;
        }
        notifyUser("ao_import_partial", args, true, tail);
    }

    bool importNotecard(const LLViewerInventoryItem* item)
    {
        if (!item || !gAssetStorage)
        {
            return false;
        }

        LL_INFOS("PlusAO") << "importing AO notecard: " << item->getName() << LL_ENDL;
        if (!gAgent.allowOperation(PERM_COPY, item->getPermissions(), GP_OBJECT_MANIPULATE) && !gAgent.isGodlike())
        {
            notifyUser("ao_import_permission", LLSD(), true);
            return false;
        }
        if (item->getAssetUUID().isNull())
        {
            notifyUser("ao_import_no_text", LLSD(), true);
            return false;
        }

        const NotecardImport import { item->getName(), item->getParentUUID() };
        gAssetStorage->getInvItemAsset(LLHost(), gAgentID, gAgentSessionID, item->getPermissions().getOwner(), LLUUID::null,
                                       item->getUUID(), item->getAssetUUID(), item->getType(),
                                       [import](const LLUUID& asset_id, LLAssetType::EType type, void*, S32 status, LLExtStat)
                                       {
                                           onNotecardLoaded(import, asset_id, type, status);
                                       },
                                       nullptr, true);
        return true;
    }
}
