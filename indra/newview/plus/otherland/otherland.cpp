/**
 * @file otherland.cpp
 * @brief Hides the objects on other parcels, or on land of another owner, than the parcel the agent is on.
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

#include "otherland.h"

#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "plus.h"
#include "llrhihash.h"

#include "llagent.h"
#include "llcallbacklist.h"
#include "lldrawable.h"
#include "llhudtext.h"
#include "llparcel.h"
#include "llremoteparcelrequest.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerparceloverlay.h"
#include "llviewerpartsim.h"
#include "llviewerpartsource.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llvovolume.h"
#include "llworld.h"
#include "pipeline.h"

namespace plus::otherland
{
    namespace
    {
        constexpr char SETTING_OWNER[] = "PlusHideOtherLandObjects";
        constexpr char SETTING_PARCEL[] = "PlusHideOtherParcelObjects";

        constexpr S32 SWEEP_PER_FRAME = 3000;     // objects looked at per frame (a full sweep in a few frames)
        constexpr S32 NEW_TAIL_MARGIN = 64;       // objects before the expected start of the new ones, looked at again
        constexpr F64 LINES_CHECK_SECONDS = 1.0;  // how often the regions' property lines are compared
        constexpr F64 LINES_CHECK_UNCUT_SECONDS = 0.25; // ... while a region is not cut yet
        constexpr F64 FIRST_CUT_SETTLE_SECONDS = 0.5;   // lines unchanged this long before a region is first cut
        constexpr F64 LINES_SETTLE_SECONDS = 2.0; // lines unchanged this long before the parcels are cut again
        constexpr size_t MAX_QUERIES = 8;         // owner questions in flight at once
        constexpr F64 STATUS_LOG_SECONDS = 5.0;   // at most this often a status line, when it changed
        constexpr F64 QUERY_TIMEOUT_SECONDS = 20.0;
        constexpr F64 RETRY_SECONDS = 30.0;
        constexpr U8 MAX_TRIES = 3;               // then the next try after REFRESH_SECONDS
        constexpr F64 REFRESH_SECONDS = 600.0;    // an owner is asked again after this (a sale between two
                                                  // others changes neither lines nor ownership bits)
        constexpr U16 NO_PARCEL = 0xffff;

        // One parcel as the overlay shows it: the cells joined without a property line between them.
        struct Parcel
        {
            LLVector3 sample;          // the middle of one of its cells, region coordinates
            U32 cellCount = 0;
            LLUUID owner;              // null while unknown; kept (and used) while asked again
            F64 confirmedAt = -1.0;    // when the simulator said so; < 0: not for this cut of the parcels
            bool asking = false;
            U8 tries = 0;
            F64 retryAt = 0.0;
        };

        struct RegionLand
        {
            U64 seenFingerprint = 0;   // the overlay's lines and ownership at the last check
            U64 cutFingerprint = 0;    // ... when the parcels below were cut
            F64 changedAt = 0.0;
            bool cut = false;          // parcels and cells below are made (kept while the lines change)
            S32 grids = 0;             // cells per edge
            std::vector<U8> cells;     // each cell's ownership and lines when cut
            std::vector<U16> cellParcel;
            std::vector<Parcel> parcels;
        };

        std::map<U64, RegionLand> sLands; // by region handle

        // One owner question: the parcel id for a point, then the parcel's owner.
        class OwnerQuery : public LLRemoteParcelInfoObserver
        {
        public:
            OwnerQuery(U64 handle, U64 fingerprint, U16 parcel)
                : mHandle(handle), mFingerprint(fingerprint), mParcel(parcel)
            {
                mTimer.reset();
            }

            ~OwnerQuery() override
            {
                // At exit the processor may be gone before this list: do not make it again.
                if (mParcelId.notNull() && LLRemoteParcelInfoProcessor::instanceExists())
                {
                    LLRemoteParcelInfoProcessor::getInstance()->removeObserver(mParcelId, this);
                }
            }

            void processParcelInfo(const LLParcelData& data) override
            {
                if (mDone || data.parcel_id != mParcelId)
                {
                    return;
                }
                if (Parcel* parcel = findParcel())
                {
                    parcel->owner = data.owner_id;
                    parcel->confirmedAt = LLFrameTimer::getElapsedSeconds();
                    parcel->asking = false;
                    parcel->tries = 0;
                }
                mDone = true; // deleted in the next idle, not inside the processor's loop
            }

            void setParcelID(const LLUUID& parcel_id) override
            {
                if (mDone)
                {
                    return;
                }
                if (parcel_id.isNull())
                {
                    fail();
                    return;
                }
                mParcelId = parcel_id;
                LLRemoteParcelInfoProcessor::getInstance()->addObserver(parcel_id, this);
                LLRemoteParcelInfoProcessor::getInstance()->sendParcelInfoRequest(parcel_id);
            }

            void setErrorStatus(S32 status, const std::string& reason) override
            {
                LL_DEBUGS("OtherLand") << "parcel question failed: " << status << " " << reason << LL_ENDL;
                fail();
            }

            bool done() const { return mDone; }

            void timeOut()
            {
                if (!mDone && mTimer.getElapsedTimeF64() > QUERY_TIMEOUT_SECONDS)
                {
                    fail();
                }
            }

        private:
            Parcel* findParcel() const
            {
                auto it = sLands.find(mHandle);
                if (it == sLands.end() || it->second.cutFingerprint != mFingerprint || !it->second.cut
                    || mParcel >= it->second.parcels.size())
                {
                    return nullptr; // cut again meanwhile: the answer is for parcels that are gone
                }
                return &it->second.parcels[mParcel];
            }

            void fail()
            {
                if (Parcel* parcel = findParcel())
                {
                    const F64 now = LLFrameTimer::getElapsedSeconds();
                    parcel->asking = false;
                    if (parcel->tries >= MAX_TRIES)
                    {
                        parcel->tries = 0;
                        parcel->retryAt = now + REFRESH_SECONDS;
                    }
                    else
                    {
                        parcel->retryAt = now + RETRY_SECONDS;
                    }
                }
                mDone = true;
            }

            U64 mHandle;
            U64 mFingerprint;
            U16 mParcel;
            LLUUID mParcelId;
            LLTimer mTimer;
            bool mDone = false;
        };

        // Where the agent stands, against which every object is judged.
        struct Here
        {
            bool byOwner = false;      // PlusHideOtherLandObjects
            bool byParcel = false;     // PlusHideOtherParcelObjects
            LLUUID owner;              // of the agent's parcel (a group for group-owned land), null while unknown
            U64 region = 0;            // the agent's region and parcel in it, NO_PARCEL while unknown
            U16 parcel = NO_PARCEL;

            bool active() const
            {
                return (byOwner && owner.notNull()) || (byParcel && parcel != NO_PARCEL);
            }

            bool operator==(const Here& other) const
            {
                return byOwner == other.byOwner && byParcel == other.byParcel && owner == other.owner
                    && region == other.region && parcel == other.parcel;
            }
        };

        std::list<std::unique_ptr<OwnerQuery>> sQueries;
        std::unordered_set<LLUUID> sHidden;  // linkset roots hidden by us
        // Their prims that were FORCE_INVISIBLE before (the pathfinding window's), with the root they
        // were hidden under: a root that dies takes its entries along (forgetForeign).
        std::unordered_map<LLUUID, LLUUID> sForeign;
        // Particle sources of hidden linksets suspended by us (LLViewerPartSource::getID()), and the
        // ones still so this frame (kept as members: swapped, not made, every frame).
        std::unordered_set<U32> sSuspended;
        std::unordered_set<U32> sStillSuspended;
        S32 sSweepIndex = 0;
        S32 sNewObjectsFrom = 0;             // where the objects made since the last frame start in gObjectList
        F64 sLinesCheckedAt = 0.0;
        Here sLastHere;

        U64 fingerprintOf(const LLViewerParcelOverlay& overlay, S32 grids, std::vector<U8>& cells)
        {
            // FNV-1a over each cell's ownership and lines. 0: nothing received yet
            // (all public, no line), which is not cut into parcels.
            cells.resize((size_t)grids * grids);
            U64 hash = LLRHI::FNV1A_BASIS;
            bool any = false;
            for (S32 row = 0; row < grids; ++row)
            {
                for (S32 col = 0; col < grids; ++col)
                {
                    const LLVector3 middle((col + 0.5f) * PARCEL_GRID_STEP_METERS, (row + 0.5f) * PARCEL_GRID_STEP_METERS, 0.f);
                    const U8 cell = overlay.ownership(middle) | overlay.parcelLineFlags(row, col);
                    cells[(size_t)row * grids + col] = cell;
                    any = any || cell != 0;
                    hash = LLRHI::fnv1aMix(hash, cell);
                }
            }
            return any ? (hash | 1) : 0;
        }

        // Cuts the region into parcels: cells joined where no property line lies between them.
        // PARCEL_WEST_LINE is a line on the west edge of a cell, PARCEL_SOUTH_LINE on its south edge.
        // A parcel the same as before (the same cells, none of them changed) keeps its owner as it
        // was; another one takes the owner of the old parcel under its sample cell for the time
        // being and is asked again, so nothing that was hidden shows while the answers come.
        void cutParcels(RegionLand& land, const std::vector<U8>& cells)
        {
            const S32 grids = land.grids;
            const bool had_cut = land.cut;
            std::vector<U16> old_cell_parcel;
            std::vector<Parcel> old_parcels;
            std::vector<U8> old_cells;
            old_cell_parcel.swap(land.cellParcel);
            old_parcels.swap(land.parcels);
            old_cells.swap(land.cells);

            land.cells = cells;
            land.cellParcel.assign((size_t)grids * grids, NO_PARCEL);

            auto lines = [&](S32 row, S32 col) { return cells[(size_t)row * grids + col]; };

            std::vector<S32> stack;
            for (S32 start = 0; start < grids * grids; ++start)
            {
                if (land.cellParcel[start] != NO_PARCEL || land.parcels.size() >= NO_PARCEL)
                {
                    continue;
                }
                const U16 index = (U16)land.parcels.size();
                const U16 old_index = had_cut ? old_cell_parcel[start] : NO_PARCEL;
                bool same = old_index != NO_PARCEL && old_index < old_parcels.size();
                U32 count = 0;

                land.cellParcel[start] = index;
                stack.assign(1, start);
                while (!stack.empty())
                {
                    const S32 cell = stack.back();
                    stack.pop_back();
                    ++count;
                    same = same && old_cell_parcel[cell] == old_index && old_cells[cell] == cells[cell];
                    const S32 row = cell / grids;
                    const S32 col = cell % grids;

                    auto join = [&](S32 next_row, S32 next_col)
                    {
                        const S32 next = next_row * grids + next_col;
                        if (land.cellParcel[next] == NO_PARCEL)
                        {
                            land.cellParcel[next] = index;
                            stack.push_back(next);
                        }
                    };
                    if (col > 0 && !(lines(row, col) & PARCEL_WEST_LINE))
                    {
                        join(row, col - 1);
                    }
                    if (col + 1 < grids && !(lines(row, col + 1) & PARCEL_WEST_LINE))
                    {
                        join(row, col + 1);
                    }
                    if (row > 0 && !(lines(row, col) & PARCEL_SOUTH_LINE))
                    {
                        join(row - 1, col);
                    }
                    if (row + 1 < grids && !(lines(row + 1, col) & PARCEL_SOUTH_LINE))
                    {
                        join(row + 1, col);
                    }
                }

                Parcel parcel;
                parcel.sample.set(((start % grids) + 0.5f) * PARCEL_GRID_STEP_METERS,
                                  ((start / grids) + 0.5f) * PARCEL_GRID_STEP_METERS, 25.f);
                parcel.cellCount = count;
                if (old_index != NO_PARCEL && old_index < old_parcels.size())
                {
                    const Parcel& old = old_parcels[old_index];
                    parcel.owner = old.owner;
                    if (same && count == old.cellCount)
                    {
                        parcel.confirmedAt = old.confirmedAt;
                        parcel.tries = old.tries;
                        parcel.retryAt = old.retryAt;
                    }
                }
                land.parcels.push_back(parcel);
            }
            land.cutFingerprint = land.seenFingerprint;
            land.cut = true;
        }

        // Follows the regions' property lines while a switch is on (off, nothing is looked at; what was
        // cut is kept). Turning a switch on (switched_on) looks at every region and cuts it at once: the
        // lines are long there by then. A region that comes later is first cut once its lines stood
        // still for half a second; the parcels of a region whose lines or ownership changed are cut
        // again once the lines have settled, and until then the old ones stay in use.
        void updateLands(F64 now, bool switched_on)
        {
            static bool all_cut = false;
            if (!switched_on && now - sLinesCheckedAt < (all_cut ? LINES_CHECK_SECONDS : LINES_CHECK_UNCUT_SECONDS))
            {
                return;
            }
            sLinesCheckedAt = now;
            all_cut = true;

            static std::vector<U8> cells;
            std::unordered_set<U64> present;
            for (LLViewerRegion* region : LLWorld::getInstance()->getRegionList())
            {
                const LLViewerParcelOverlay* overlay = region ? region->getParcelOverlay() : nullptr;
                if (!overlay)
                {
                    continue;
                }
                present.insert(region->getHandle());
                RegionLand& land = sLands[region->getHandle()];
                const S32 grids = llclamp(S32(region->getWidth() / PARCEL_GRID_STEP_METERS), 1, 1024);
                if (grids != land.grids)
                {
                    land = RegionLand();
                    land.grids = grids;
                }
                const U64 fingerprint = fingerprintOf(*overlay, grids, cells);
                if (fingerprint != land.seenFingerprint)
                {
                    land.seenFingerprint = fingerprint;
                    land.changedAt = now;
                }
                if (fingerprint != 0 && (!land.cut || fingerprint != land.cutFingerprint)
                    && (switched_on || now - land.changedAt >= (land.cut ? LINES_SETTLE_SECONDS : FIRST_CUT_SETTLE_SECONDS)))
                {
                    cutParcels(land, cells);
                }
                all_cut = all_cut && (land.cut || land.seenFingerprint == 0); // 0: nothing to cut yet
            }
            for (auto it = sLands.begin(); it != sLands.end();)
            {
                it = present.count(it->first) ? std::next(it) : sLands.erase(it);
            }
        }

        // The agent's region has property lines that are not cut into parcels yet, or were cut from lines
        // that have changed since (a switch turned on again after the lines moved while it was off).
        bool agentRegionUncut()
        {
            LLViewerRegion* region = gAgent.getRegion();
            const auto it = region ? sLands.find(region->getHandle()) : sLands.end();
            if (it == sLands.end() || it->second.seenFingerprint == 0)
            {
                return false;
            }
            return !it->second.cut || it->second.cutFingerprint != it->second.seenFingerprint;
        }

        // The parcel at a point of a region, NO_PARCEL while the region is not cut.
        U16 parcelAt(LLViewerRegion* region, const LLVector3& pos, RegionLand** land_out)
        {
            auto it = region ? sLands.find(region->getHandle()) : sLands.end();
            if (it == sLands.end() || !it->second.cut)
            {
                return NO_PARCEL;
            }
            RegionLand& land = it->second;
            const S32 col = llclamp(S32(pos.mV[VX] / PARCEL_GRID_STEP_METERS), 0, land.grids - 1);
            const S32 row = llclamp(S32(pos.mV[VY] / PARCEL_GRID_STEP_METERS), 0, land.grids - 1);
            const U16 index = land.cellParcel[(size_t)row * land.grids + col];
            if (index >= land.parcels.size())
            {
                return NO_PARCEL;
            }
            *land_out = &land;
            return index;
        }

        // Asks the owner of a parcel when it is not known (or known too long ago) and a question may go.
        void askIfNeeded(LLViewerRegion* region, RegionLand& land, U16 index, F64 now)
        {
            Parcel& parcel = land.parcels[index];
            if (parcel.asking || now < parcel.retryAt
                || (parcel.confirmedAt >= 0.0 && now - parcel.confirmedAt <= REFRESH_SECONDS))
            {
                return;
            }
            if (sQueries.size() >= MAX_QUERIES)
            {
                return;
            }
            LLViewerRegion* agent_region = gAgent.getRegion();
            const std::string url = agent_region ? agent_region->getCapability("RemoteParcelRequest") : std::string();
            if (url.empty())
            {
                return;
            }
            sQueries.push_back(std::make_unique<OwnerQuery>(region->getHandle(), land.cutFingerprint, index));
            parcel.asking = true;
            ++parcel.tries;
            const LLVector3d global = region->getPosGlobalFromRegion(parcel.sample);
            LLRemoteParcelInfoProcessor::getInstance()->requestRegionParcelInfo(url, region->getRegionID(),
                parcel.sample, global, sQueries.back()->getObserverHandle());
        }

        bool isHideable(LLViewerObject* object)
        {
            return object
                && !object->isDead()
                && object->getParent() == nullptr // a linkset root: not a child, not worn (the avatar is the parent)
                && object->getPCode() == LL_PCODE_VOLUME
                && !object->isAvatar()
                && !object->isOrphaned()
                && !object->mGLTFAsset            // a GLTF scene is drawn and picked apart from FORCE_INVISIBLE
                && object->mDrawable.notNull();
        }

        // What the agent works with stays: the seat and what is selected.
        bool isInUse(LLViewerObject* root)
        {
            if (isAgentAvatarValid() && gAgentAvatarp->isSitting() && gAgentAvatarp->getRootEdit() == root)
            {
                return true;
            }
            if (root->isSelected())
            {
                return true;
            }
            for (LLViewerObject* child : root->getChildren())
            {
                if (child && !child->isAvatar() && child->isSelected())
                {
                    return true;
                }
            }
            return false;
        }

        bool shouldHide(LLViewerObject* object, const Here& here, F64 now)
        {
            if (!isHideable(object) || isInUse(object))
            {
                return false;
            }
            LLViewerRegion* region = object->getRegion();
            RegionLand* land = nullptr;
            const U16 index = parcelAt(region, object->getPositionRegion(), &land);
            if (index == NO_PARCEL)
            {
                return false; // not yet known where it stands
            }
            if (here.byParcel && here.parcel != NO_PARCEL)
            {
                return region->getHandle() != here.region || index != here.parcel;
            }
            if (here.byOwner && here.owner.notNull())
            {
                askIfNeeded(region, *land, index, now);
                const LLUUID& owner = land->parcels[index].owner;
                return owner.notNull() && owner != here.owner;
            }
            return false;
        }

        // The root and its prims; avatars sitting on it are children too and stay as they are.
        template <typename F>
        void forEachPrim(LLViewerObject* root, F f)
        {
            if (root->mDrawable.notNull())
            {
                f(root, root->mDrawable.get());
            }
            for (LLViewerObject* child : root->getChildren())
            {
                if (child && !child->isAvatar() && child->mDrawable.notNull())
                {
                    f(child, child->mDrawable.get());
                }
            }
        }

        // Sets FORCE_INVISIBLE where an update cleared it, and takes the lights and the
        // floating text out (an update may bring a new text).
        // FORCE_INVISIBLE is read only where a spatial group's geometry is made again
        // (LLVolumeGeometryManager::rebuildGeom(), which the official draws and the RHI lists
        // follow through rhiBuilds()); a drawable's own rebuild (markRebuild) only updates the
        // group's vertices. So the group is made dirty where the state really changes. An update
        // that cleared the state put the drawable in the rebuild queue in the same frame: its
        // group still has the faces out, and setting the state again is enough. Without that
        // (a prim newly linked to a hidden root) the group is rebuilt.
        void keepHidden(LLViewerObject* root)
        {
            forEachPrim(root, [](LLViewerObject* prim, LLDrawable* drawable)
                {
                    if (!drawable->isState(LLDrawable::FORCE_INVISIBLE))
                    {
                        // Set by us now: a "hidden by someone else" mark (the viewer cleared its own
                        // hiding, an update or an orphan joined to its parent) goes, or show() would
                        // leave the prim hidden for ever.
                        if (!sForeign.empty())
                        {
                            sForeign.erase(prim->getID());
                        }
                        drawable->setState(LLDrawable::FORCE_INVISIBLE);
                        if (!drawable->isState(LLDrawable::IN_REBUILD_Q))
                        {
                            prim->dirtySpatialGroup();
                        }
                    }
                    if (drawable->isState(LLDrawable::LIGHT))
                    {
                        gPipeline.setLight(drawable, false);
                    }
                    if (prim->mText.notNull())
                    {
                        prim->mText->setHidden(true);
                    }
                });
        }

        void hide(LLViewerObject* root)
        {
            const LLUUID root_id = root->getID();
            forEachPrim(root, [&root_id](LLViewerObject* prim, LLDrawable* drawable)
                {
                    if (drawable->isState(LLDrawable::FORCE_INVISIBLE))
                    {
                        sForeign[prim->getID()] = root_id; // hidden by someone else: left hidden by show()
                    }
                    else
                    {
                        drawable->setState(LLDrawable::FORCE_INVISIBLE);
                        prim->dirtySpatialGroup(); // its faces out of the group's geometry this frame
                    }
                });
            sHidden.insert(root_id);
            keepHidden(root);
        }

        // A hidden root that is gone (dead, or out of the list): its prims' "hidden by someone else"
        // marks go too, or the same prims coming back (the same ids) would be left FORCE_INVISIBLE by
        // show() for ever. Only the pathfinding window makes entries here, so the map is small.
        void forgetForeign(const LLUUID& root_id)
        {
            for (auto it = sForeign.begin(); it != sForeign.end();)
            {
                it = (it->second == root_id) ? sForeign.erase(it) : std::next(it);
            }
        }

        void show(LLViewerObject* root)
        {
            forEachPrim(root, [](LLViewerObject* prim, LLDrawable* drawable)
                {
                    if (!sForeign.erase(prim->getID()) && drawable->isState(LLDrawable::FORCE_INVISIBLE))
                    {
                        drawable->clearState(LLDrawable::FORCE_INVISIBLE);
                        prim->dirtySpatialGroup(); // its faces back in the group's geometry this frame
                    }
                    LLVOVolume* volume = drawable->getVOVolume();
                    if (volume && volume->getIsLight() && !drawable->isState(LLDrawable::LIGHT))
                    {
                        gPipeline.setLight(drawable, true);
                    }
                    if (prim->mText.notNull())
                    {
                        prim->mText->setHidden(false);
                    }
                });
        }

        void showAll()
        {
            for (const LLUUID& id : sHidden)
            {
                LLViewerObject* object = gObjectList.findObject(id);
                if (object && !object->isDead())
                {
                    show(object);
                }
            }
            sHidden.clear();
            sForeign.clear();
        }

        void judge(LLViewerObject* object, const Here& here, F64 now)
        {
            const bool hidden = sHidden.count(object->getID()) != 0;
            const bool hide_it = shouldHide(object, here, now);
            if (hide_it && !hidden)
            {
                hide(object);
            }
            else if (!hide_it && hidden)
            {
                sHidden.erase(object->getID());
                show(object);
            }
        }

        // The objects made since the last frame (new ones and ones made again from the cache),
        // judged at once so that they are never drawn before they are hidden. gObjectList adds at
        // the end and takes the dead out after the idle callbacks, by moving the last ones into
        // their places, so the new ones start where the list ended less its dead.
        void judgeNewObjects(const Here& here, F64 now)
        {
            const S32 count = gObjectList.getNumObjects();
            for (S32 i = llclamp(sNewObjectsFrom - NEW_TAIL_MARGIN, 0, count); i < count; ++i)
            {
                if (LLViewerObject* object = gObjectList.getObject(i))
                {
                    judge(object, here, now);
                }
            }
        }

        void sweep(const Here& here, F64 now)
        {
            const S32 count = gObjectList.getNumObjects();
            if (sSweepIndex >= count)
            {
                sSweepIndex = 0;
            }
            const S32 end = llmin(count, sSweepIndex + SWEEP_PER_FRAME);
            for (; sSweepIndex < end; ++sSweepIndex)
            {
                if (LLViewerObject* object = gObjectList.getObject(sSweepIndex))
                {
                    judge(object, here, now);
                }
            }
        }

        Here whereAmI(bool by_owner, bool by_parcel)
        {
            Here here;
            here.byOwner = by_owner;
            here.byParcel = by_parcel;
            if (by_owner)
            {
                LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();
                here.owner = parcel ? parcel->getOwnerID() : LLUUID::null;
            }
            if (by_parcel)
            {
                LLViewerRegion* region = gAgent.getRegion();
                RegionLand* land = nullptr;
                here.parcel = region ? parcelAt(region, region->getPosRegionFromAgent(gAgent.getPositionAgent()), &land)
                                     : NO_PARCEL;
                here.region = here.parcel != NO_PARCEL ? region->getHandle() : 0;
            }
            return here;
        }

        // A line in the log while on, when what is hidden or known changed (looked at every 5 s).
        void logStatus(F64 now)
        {
            static F64 logged_at = 0.0;
            static std::string last;
            if (now - logged_at < STATUS_LOG_SECONDS)
            {
                return;
            }
            logged_at = now; // look again in 5 s whether or not the line changed
            size_t cut = 0;
            size_t parcels = 0;
            size_t owners = 0;
            for (const auto& entry : sLands)
            {
                if (entry.second.cut)
                {
                    ++cut;
                    parcels += entry.second.parcels.size();
                    for (const Parcel& parcel : entry.second.parcels)
                    {
                        owners += parcel.owner.notNull() ? 1 : 0;
                    }
                }
            }
            std::ostringstream line;
            line << sHidden.size() << " linksets hidden; regions cut " << cut << "/" << sLands.size() << ", "
                 << parcels << " parcels, " << owners << " owners known, " << sQueries.size() << " questions out";
            std::string text = line.str();
            if (text != last)
            {
                last = std::move(text);
                LL_INFOS("OtherLand") << last << LL_ENDL;
            }
        }

        // The particle sources of hidden linksets (any prim of them) stop making particles, as the
        // viewer stops an orphaned prim's (LLViewerPartSource::setSuspended()); the particles they made
        // already live out their time. A source suspended by someone else is left as it is, and only
        // ours are let go again. Runs after the idle callbacks' turn, before
        // LLWorld::updateParticles(), so a source never makes a particle in a frame it is hidden;
        // nothing to do once nothing is hidden and nothing of ours is suspended.
        void updateParticleSources()
        {
            if (sHidden.empty() && sSuspended.empty())
            {
                return;
            }
            sStillSuspended.clear();
            for (const LLPointer<LLViewerPartSource>& pointer : *LLViewerPartSim::getInstance()->getParticleSystemList())
            {
                LLViewerPartSource* source = pointer.get(); // the list is const; its sources are not
                if (!source || source->isDead())
                {
                    continue;
                }
                LLViewerObject* object = source->mSourceObjectp.get();
                LLViewerObject* root = object && !object->isDead() ? object->getRootEdit() : nullptr;
                const bool hidden = root && sHidden.count(root->getID()) != 0;
                const bool ours = sSuspended.count(source->getID()) != 0;
                if (hidden && (ours || !source->isSuspended()))
                {
                    source->setSuspended(true);
                    sStillSuspended.insert(source->getID());
                }
                else if (!hidden && ours && !(object && object->isOrphaned()))
                {
                    // an orphaned prim's source was stopped by the viewer as well (hideExtraDisplayItems):
                    // it is left to the viewer, which lets it go when the parent comes
                    source->setSuspended(false);
                }
            }
            sSuspended.swap(sStillSuspended);
        }

        void idle(void*)
        {
            static LLCachedControl<bool> by_owner(gSavedSettings, SETTING_OWNER, false);
            static LLCachedControl<bool> by_parcel(gSavedSettings, SETTING_PARCEL, false);
            static bool was_on = false;

            for (auto it = sQueries.begin(); it != sQueries.end();)
            {
                (*it)->timeOut();
                it = (*it)->done() ? sQueries.erase(it) : std::next(it);
            }

            const F64 now = LLFrameTimer::getElapsedSeconds();
            if (!gAgent.getRegion())
            {
                sSuspended.clear(); // logged out: the sources are gone
                return; // not logged in
            }
            const bool on = by_owner || by_parcel;
            const bool switched_on = on && !was_on;
            was_on = on;
            Here here;
            if (on)
            {
                updateLands(now, switched_on);
                here = whereAmI(by_owner, by_parcel);
            }
            if (!here.active())
            {
                // Off, or not yet known where we are: everything as the simulator sends it.
                if (!sHidden.empty())
                {
                    LL_INFOS("OtherLand") << "off (or not known where we are): " << sHidden.size()
                                          << " linksets shown again" << LL_ENDL;
                    showAll();
                }
                updateParticleSources();
                sLastHere = here;
                sNewObjectsFrom = gObjectList.getNumObjects() - gObjectList.mNumDeadObjects;
                return;
            }

            if (!sLastHere.active())
            {
                // Just turned on (or just known where we are): every object at once, not over
                // the frames of a sweep. The lines are followed only while on: when the agent's region
                // has lines that are not cut yet (a region just come), or that changed since they were
                // cut (a few seconds), wait for them rather than judge by the old parcels. Turning a
                // switch on cuts every region in the same frame, so it does not wait then.
                if (agentRegionUncut())
                {
                    sNewObjectsFrom = gObjectList.getNumObjects() - gObjectList.mNumDeadObjects;
                    return;
                }
                LLTimer timer;
                const S32 count = gObjectList.getNumObjects();
                for (S32 i = 0; i < count; ++i)
                {
                    if (LLViewerObject* object = gObjectList.getObject(i))
                    {
                        judge(object, here, now);
                    }
                }
                sSweepIndex = 0;
                sLastHere = here;
                LL_INFOS("OtherLand") << "on (" << (here.byParcel && here.parcel != NO_PARCEL ? "other parcels" : "other owners")
                                      << "): " << count << " objects judged in " << timer.getElapsedTimeF32() * 1000.f
                                      << " ms, " << sHidden.size() << " linksets hidden" << LL_ENDL;
            }
            else if (!(here == sLastHere))
            {
                // Somewhere else now: what was hidden and is not any more shows at once (the sweep
                // finds what is to be hidden now).
                std::vector<LLUUID> hidden(sHidden.begin(), sHidden.end());
                for (const LLUUID& id : hidden)
                {
                    LLViewerObject* object = gObjectList.findObject(id);
                    if (object && !object->isDead())
                    {
                        judge(object, here, now);
                    }
                }
                sLastHere = here;
            }

            judgeNewObjects(here, now);
            sweep(here, now);

            // Every frame, after this frame's messages (idleNetwork() runs before the idle
            // callbacks): an update of the object cleared FORCE_INVISIBLE.
            for (auto it = sHidden.begin(); it != sHidden.end();)
            {
                LLViewerObject* object = gObjectList.findObject(*it);
                if (!object || object->isDead())
                {
                    if (!sForeign.empty())
                    {
                        forgetForeign(*it);
                    }
                    it = sHidden.erase(it);
                    continue;
                }
                if (isInUse(object))
                {
                    it = sHidden.erase(it);
                    show(object);
                    continue;
                }
                keepHidden(object);
                ++it;
            }
            if (sHidden.empty())
            {
                sForeign.clear();
            }
            updateParticleSources();

            sNewObjectsFrom = gObjectList.getNumObjects() - gObjectList.mNumDeadObjects;
            logStatus(now);
        }

        void onUIReady()
        {
            gIdleCallbacks.addFunction(&idle, nullptr);
        }

        plus::FeatureRegistration sRegistration({ "otherland", {}, &onUIReady });
    }
}
