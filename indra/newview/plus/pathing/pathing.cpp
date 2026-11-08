/**
 * @file pathing.cpp
 * @brief LLPathingLib without Havok: the navmesh, the test path and the shapes of the
 *        pathfinding view, built on the client from the region's terrain and objects.
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

// What the Havok library did for the viewer, and what replaces it here:
//
//   navmesh      Linden Lab's simulator sends a baked Havok navmesh; only their library can read
//                it. Here the viewer builds its own navigation grid (navgrid.h) from the terrain
//                and the region's static objects, when the simulator reports a new navmesh (the
//                trigger is the arrival of the simulator's data, which is dumped to the log folder
//                when PlusPathingDumpBlob is on, so that the format can be worked out one day).
//                It is the viewer's own reading of the region, not the simulator's: the simulator
//                counts the pathfinding type of each linkset (walkable, static obstacle, phantom
//                and the per-type walkability), which the grid does not know.
//   test path    the console's two clicks give rays; they are cut with the grid, and a path is
//                searched on it for the chosen character type (size in pathing.h)
//   shapes       the walkable and obstacle objects are the object's own triangles, split by slope
//   capsules     tessellated here
//
// Only the agent's own region is covered (the neighbours' navmesh data is ignored).
// Everything runs on the main thread. Region-local coordinates in the grid; the region's origin
// in agent space is added as a translation when drawing, so a move of the agent's origin
// (crossing into another region) needs no rebuild.

#include "llviewerprecompiledheaders.h"

#include "pathing.h"
#include "navgrid.h"
#include "plus.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <tuple>
#include <vector>

#include "llagent.h"
#include "lldir.h"
#include "llfile.h"
#include "llpathinglib.h"
#include "llpathfindinglinkset.h"
#include "llpathfindingmanager.h"
#include "llpathfindingobjectlist.h"
#include "llrender.h"
#include "llrendernavprim.h"
#include "llsurface.h"
#include "lltimer.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvovolume.h"
#include "llvolume.h"
#include "llworld.h"
#include "m4math.h"

namespace plus::pathing
{
    namespace
    {
        namespace ng = plus::pathing::navgrid;

        constexpr S32 CENTER_REGION = 99;                 // the zone's value for the agent's own region (llpathfindingnavmeshzone.cpp)
        constexpr F32 MAX_REGION_WIDTH = 512.f;           // a bigger region: the grid would not fit in memory
        constexpr F32 MIN_OBJECT_SIZE = 0.25f;            // smaller objects do not count
        constexpr size_t MAX_SHAPE_TRIANGLES = 400000;    // kept for the shapes view
        constexpr F32 LIFT = 0.04f;                       // the drawing sits a little over the floor
        constexpr char SETTING_DUMP[] = "PlusPathingDumpBlob";

        // Base grid: the smallest character, so that every type's grid is a subset of it
        // (no wider than any type, no taller than the lowest one, D).
        constexpr CharacterSize BASE_SIZE = { 0.20f, CHARACTER_TYPE_D.height };
        constexpr F32 LEDGE_HEADROOM = 0.80f; // navgrid::Params::ledgeHeadroom, see rebuild()

        struct Vtx
        {
            LLVector3 p;
            LLVector3 n;
            LLColor4U c;
        };

        LLColor4U toU(const LLColor4& c)
        {
            const auto b = [](F32 v) { return static_cast<U8>(llclamp(ll_round(v * 255.f), 0, 255)); };
            return LLColor4U(b(c.mV[VX]), b(c.mV[VY]), b(c.mV[VZ]), b(c.mV[VW]));
        }

        LLPointer<LLVertexBuffer> makeVB(const std::vector<Vtx>& v)
        {
            if (v.empty())
            {
                return nullptr;
            }
            LLPointer<LLVertexBuffer> vb = new LLVertexBuffer(LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_NORMAL | LLVertexBuffer::MAP_COLOR);
            if (!vb->allocateBuffer(static_cast<U32>(v.size()), 0))
            {
                LL_WARNS("Pathing") << "Could not allocate a vertex buffer of " << v.size() << " vertices" << LL_ENDL;
                return nullptr;
            }
            LLStrider<LLVector3> pos, nrm;
            LLStrider<LLColor4U> col;
            vb->getVertexStrider(pos);
            vb->getNormalStrider(nrm);
            vb->getColorStrider(col);
            for (U32 i = 0; i < static_cast<U32>(v.size()); ++i)
            {
                pos[i] = v[i].p;
                nrm[i] = v[i].n;
                col[i] = v[i].c;
            }
            vb->unmapBuffer();
            return vb;
        }

        // A capsule along z (or along x when horizontal): `cyl` is the length of the straight part.
        void buildCapsule(std::vector<Vtx>& out, F32 radius, F32 cyl, bool horizontal, const LLColor4U& color)
        {
            constexpr int SEGMENTS = 16;
            constexpr int RINGS = 5;
            struct Ring
            {
                F32 r, z, nr, nz;
            };
            std::vector<Ring> profile;
            for (int i = 0; i <= RINGS; ++i)
            {
                const F32 th = F_PI_BY_TWO * static_cast<F32>(i) / RINGS;
                profile.push_back({ radius * sinf(th), cyl * 0.5f + radius * cosf(th), sinf(th), cosf(th) });
            }
            for (int i = 0; i <= RINGS; ++i)
            {
                const F32 th = F_PI_BY_TWO + F_PI_BY_TWO * static_cast<F32>(i) / RINGS;
                profile.push_back({ radius * sinf(th), -cyl * 0.5f + radius * cosf(th), sinf(th), cosf(th) });
            }
            const auto point = [&](const Ring& g, F32 a, bool normal)
            {
                const F32 ca = cosf(a), sa = sinf(a);
                const F32 u = normal ? g.nr : g.r;
                const F32 w = normal ? g.nz : g.z;
                return horizontal ? LLVector3(w, u * ca, u * sa) : LLVector3(u * ca, u * sa, w);
            };
            for (size_t k = 0; k + 1 < profile.size(); ++k)
            {
                for (int j = 0; j < SEGMENTS; ++j)
                {
                    const F32 a0 = F_TWO_PI * static_cast<F32>(j) / SEGMENTS;
                    const F32 a1 = F_TWO_PI * static_cast<F32>(j + 1) / SEGMENTS;
                    const Vtx A{ point(profile[k], a0, false), point(profile[k], a0, true), color };
                    const Vtx B{ point(profile[k], a1, false), point(profile[k], a1, true), color };
                    const Vtx C{ point(profile[k + 1], a1, false), point(profile[k + 1], a1, true), color };
                    const Vtx D{ point(profile[k + 1], a0, false), point(profile[k + 1], a0, true), color };
                    out.push_back(A);
                    out.push_back(D);
                    out.push_back(C);
                    out.push_back(A);
                    out.push_back(C);
                    out.push_back(B);
                }
            }
        }

        // Draws a buffer whose vertices are region-local, moved by `offset` (and turned by `rot`).
        void drawAt(LLVertexBuffer* vb, U32 mode, U32 count, const LLVector3& offset, const LLQuaternion* rot = nullptr)
        {
            if (!vb || count == 0)
            {
                return;
            }
            gGL.pushMatrix();
            if (rot)
            {
                const LLMatrix4 m(*rot, LLVector4(offset.mV[VX], offset.mV[VY], offset.mV[VZ], 1.f));
                gGL.multMatrix(&m.mMatrix[0][0]);
            }
            else
            {
                gGL.translatef(offset.mV[VX], offset.mV[VY], offset.mV[VZ]);
            }
            gGL.syncMatrices();
            gRenderNav.renderNavMeshVB(mode, vb, static_cast<int>(count));
            gGL.popMatrix();
        }

        class PlusPathingLib final : public LLPathingLib
        {
        public:
            PlusPathingLib()
            {
                mColors.mWalkable = LLColor4U(77, 217, 102, 255);
                mColors.mObstacle = LLColor4U(217, 77, 77, 255);
                mColors.mMaterial = LLColor4U(77, 128, 230, 255);
                mColors.mExclusion = LLColor4U(230, 179, 51, 255);
                mColors.mConnectedEdge = LLColor4U(255, 255, 255, 255);
                mColors.mBoundaryEdge = LLColor4U(255, 128, 0, 255);
                mColors.mHeatColorBase = LLColor4(0.9f, 0.2f, 0.2f, 1.f);
                mColors.mHeatColorMax = LLColor4(0.2f, 0.9f, 0.3f, 1.f);
                mColors.mFaceColor = LLColor4U(128, 128, 255, 255);
                mColors.mStarValid = LLColor4U(77, 255, 77, 255);
                mColors.mStarInvalid = LLColor4U(255, 77, 77, 255);
                mColors.mTestPath = LLColor4U(255, 255, 0, 255);
                mColors.mWaterColor = LLColor4U(51, 102, 230, 128);
            }

            // ---- the navmesh from the server --------------------------------------------

            LLPLResult extractNavMeshSrcFromLLSD(const LLSD::Binary& dataBlock, int dir) override
            {
                // The simulator's data is Havok's and cannot be read here. Its arrival is the signal
                // that the region's navmesh is new: the viewer builds its own from the world then.
                LL_INFOS("Pathing") << "Navmesh data from the simulator: direction " << dir << ", " << dataBlock.size() << " bytes" << LL_ENDL;
                if (gSavedSettings.getBOOL(SETTING_DUMP))
                {
                    dump(dataBlock, dir);
                }
                if (dir == CENTER_REGION)
                {
                    mRebuild = true;
                }
                return LLPL_OK;
            }

            void processNavMeshData() override
            {
                if (mRebuild)
                {
                    mRebuild = false;
                    requestLinksets();
                }
            }

            // ---- the test path ----------------------------------------------------------

            LLPLResult generatePath(const PathingPacket& packet) override
            {
                mPath.clear();
                mHasA = mHasB = false;
                mPathOk = false;
                if (!mReady)
                {
                    return LLPL_NO_NAVMESH;
                }
                const LLViewerRegion* region = regionNow();
                if (!region)
                {
                    return LLPL_NO_NAVMESH;
                }
                const LLVector3 origin = region->getOriginAgent();
                const CharacterSize size = sizeFor(packet.mCharacterType, packet.mCharacterWidth);
                mPathSize = size;
                trimAnalyses();
                const ng::Analysis& an = analysis(size, typeBitFor(packet.mCharacterType));

                const auto pickPoint = [&](const LLVector3& s, const LLVector3& e, ng::V3& out)
                {
                    ng::V3 hit;
                    if (!mGrid.pick(local(s, origin), local(e, origin), hit))
                    {
                        return false;
                    }
                    const int span = mGrid.nearestSpan(an, hit.x, hit.y, hit.z, 2);
                    out = span >= 0 ? mGrid.center(span) : hit;
                    if (span >= 0)
                    {
                        // the point the ray hit, on the floor it was snapped to
                        out.x = hit.x;
                        out.y = hit.y;
                    }
                    return true;
                };

                bool missed = false;
                if (packet.mHasPointA)
                {
                    mHasA = pickPoint(packet.mStartPointA, packet.mEndPointA, mA);
                    missed = missed || !mHasA;
                }
                if (packet.mHasPointB)
                {
                    mHasB = pickPoint(packet.mStartPointB, packet.mEndPointB, mB);
                    missed = missed || !mHasB;
                }
                if (!(packet.mHasPointA && packet.mHasPointB))
                {
                    return missed ? LLPL_NO_PATH : LLPL_OK;       // one end only: the console asks for the other
                }
                if (missed)
                {
                    return LLPL_NO_PATH;
                }
                mPathOk = mGrid.findPath(an, mA, mB, mPath);
                return mPathOk ? LLPL_PATH_GENERATED_OK : LLPL_NO_PATH;
            }

            // ---- colours and heat map ---------------------------------------------------

            void setNavMeshMaterialType(LLPLCharacterType type) override
            {
                if (type != mHeatType)
                {
                    mHeatType = type;
                    mNavDirty = true;
                }
            }

            void setNavMeshColors(const NavMeshColors& colors) override
            {
                mColors = colors;
                mNavDirty = true;
                mShapesDirty = true;
                mWaterVB = nullptr;
                mCapsules.clear();
            }

            // ---- drawing ----------------------------------------------------------------

            void renderNavMesh() override
            {
                if (!drawable())
                {
                    return;
                }
                ensureNav();
                drawAt(mNavVB, LLRender::TRIANGLES, mNavCount, mDrawOrigin);
            }

            void renderNavMeshEdges() override
            {
                if (!drawable())
                {
                    return;
                }
                ensureNav();
                drawAt(mEdgeVB, LLRender::LINES, mEdgeCount, mDrawOrigin);
            }

            void renderNavMeshShapesVBO(U32 shapeRenderFlags) override
            {
                if (!drawable())
                {
                    return;
                }
                ensureShapes();
                if (shapeRenderFlags & (1u << LLST_WalkableObjects))
                {
                    drawAt(mWalkVB, LLRender::TRIANGLES, mWalkCount, mDrawOrigin);
                }
                if (shapeRenderFlags & (1u << LLST_ObstacleObjects))
                {
                    drawAt(mObstVB, LLRender::TRIANGLES, mObstCount, mDrawOrigin);
                }
                if (shapeRenderFlags & (1u << LLST_MaterialPhantoms))
                {
                    drawAt(mMatVB, LLRender::TRIANGLES, mMatCount, mDrawOrigin);
                }
                if (shapeRenderFlags & (1u << LLST_ExclusionPhantoms))
                {
                    drawAt(mExclVB, LLRender::TRIANGLES, mExclCount, mDrawOrigin);
                }
            }

            void renderPath() override
            {
                if (!drawable() || mPath.size() < 2)
                {
                    return;
                }
                gGL.setLineWidth(3.f);
                gGL.begin(LLRender::LINE_STRIP);
                gGL.color4ubv(mColors.mTestPath.mV);
                for (const ng::V3& p : mPath)
                {
                    gGL.vertex3f(p.x + mDrawOrigin.mV[VX], p.y + mDrawOrigin.mV[VY], p.z + mDrawOrigin.mV[VZ] + 0.1f);
                }
                gGL.end();
                gGL.flush();
                gGL.setLineWidth(1.f);
            }

            void renderPathBookend(LLRender& gl, LLPLPathBookEnd type) override
            {
                if (!drawable())
                {
                    return;
                }
                const bool start = type == LLPL_START;
                if (start ? !mHasA : !mHasB)
                {
                    return;
                }
                const ng::V3& at = start ? mA : mB;
                const F32 cyl = std::max(0.f, mPathSize.height - 2.f * mPathSize.radius);
                const LLColor4U& color = mPathOk ? mColors.mStarValid : mColors.mStarInvalid;
                LLVertexBuffer* vb = capsuleVB(mPathSize.radius, cyl, false, color);
                const LLVector3 pos = mDrawOrigin + LLVector3(at.x, at.y, at.z + mPathSize.height * 0.5f);
                const LLQuaternion rot;
                drawAt(vb, LLRender::TRIANGLES, capsuleCount(), pos, &rot);
                (void)gl;
            }

            void renderSimpleShapes(LLRender& gl, F32 regionsWaterHeight) override
            {
                const LLViewerRegion* region = regionNow();
                if (!region)
                {
                    return;
                }
                const F32 w = region->getWidth();
                if (!mWaterVB || mWaterWidth != w)
                {
                    std::vector<Vtx> v;
                    const LLVector3 up(0.f, 0.f, 1.f);
                    const LLVector3 a(0.f, 0.f, 0.f), b(w, 0.f, 0.f), c(w, w, 0.f), d(0.f, w, 0.f);
                    for (const LLVector3& p : { a, b, c, a, c, d })
                    {
                        v.push_back({ p, up, mColors.mWaterColor });
                    }
                    mWaterVB = makeVB(v);
                    mWaterWidth = w;
                }
                drawAt(mWaterVB, LLRender::TRIANGLES, 6, region->getOriginAgent() + LLVector3(0.f, 0.f, regionsWaterHeight));
                (void)gl;
            }

            // ---- characters' capsules ---------------------------------------------------

            void createPhysicsCapsuleRep(F32 length, F32 radius, BOOL horizontal, const LLUUID& id) override
            {
                mReps[id] = { length, radius, horizontal != FALSE };
            }

            void cleanupPhysicsCapsuleRepResiduals() override
            {
                mReps.clear();
            }

            void renderSimpleShapeCapsuleID(LLRender& gl, const LLUUID& id, const LLVector3& pos, const LLQuaternion& rot) override
            {
                const auto it = mReps.find(id);
                if (it == mReps.end())
                {
                    return;
                }
                LLVertexBuffer* vb = capsuleVB(it->second.radius, it->second.length, it->second.horizontal, mColors.mFaceColor);
                drawAt(vb, LLRender::TRIANGLES, capsuleCount(), pos, &rot);
                (void)gl;
            }

            // ---- clean up ---------------------------------------------------------------

            void cleanupVBOManager() override
            {
                mNavVB = mEdgeVB = mWalkVB = mObstVB = mMatVB = mExclVB = mWaterVB = nullptr;
                mNavCount = mEdgeCount = mWalkCount = mObstCount = mMatCount = mExclCount = 0;
                mNavDirty = mShapesDirty = true;
                mCapsules.clear();
            }

            void cleanupResidual() override
            {
                cleanupVBOManager();
                mGrid = ng::Grid();
                mAnalyses.clear();
                for (std::vector<F32>* v : { &mWalkTris, &mObstTris, &mMatTris, &mExclTris })
                {
                    v->clear();
                    v->shrink_to_fit();
                }
                mExclusions.clear();
                mPath.clear();
                mHasA = mHasB = mPathOk = false;
                mReady = false;
                mRebuild = false;
            }

        private:
            struct CapsuleRep
            {
                F32 length;
                F32 radius;
                bool horizontal;
            };

            struct CapsuleEntry
            {
                int r, l;
                bool horizontal;
                U32 color;
                LLPointer<LLVertexBuffer> vb;
            };

            ng::Grid mGrid;
            bool mReady = false;
            bool mRebuild = false;
            LLUUID mRegionID;
            std::vector<F32> mWalkTris;     // 9 floats a triangle, region-local
            std::vector<F32> mObstTris;
            std::vector<F32> mMatTris;
            std::vector<F32> mExclTris;
            std::map<std::tuple<int, int, int>, ng::Analysis> mAnalyses;

            // The pathfinding type of each linkset (by its root's id), from the console's linkset list.
            struct LinksetInfo
            {
                LLPathfindingLinkset::ELinksetUse use;
                U8 floorMask;           // bit 0 any type, bits 1..4 characters A..D (walkability coefficient > 0)
            };
            std::map<LLUUID, LinksetInfo> mLinksets;
            bool mHaveTypes = false;
            LLPathfindingManager::request_id_t mRequestId = 0;

            // An exclusion volume as a box, region-local.
            struct Box
            {
                LLVector3 pos;
                LLQuaternion rot;
                LLVector3 scale;
            };
            std::vector<Box> mExclusions;

            NavMeshColors mColors;
            LLPLCharacterType mHeatType = LLPL_CHARACTER_TYPE_NONE;

            bool mNavDirty = true;
            bool mShapesDirty = true;
            LLPointer<LLVertexBuffer> mNavVB, mEdgeVB, mWalkVB, mObstVB, mMatVB, mExclVB, mWaterVB;
            U32 mNavCount = 0, mEdgeCount = 0, mWalkCount = 0, mObstCount = 0, mMatCount = 0, mExclCount = 0;
            F32 mWaterWidth = 0.f;
            LLVector3 mDrawOrigin;

            std::vector<CapsuleEntry> mCapsules;
            std::map<LLUUID, CapsuleRep> mReps;

            ng::V3 mA, mB;
            bool mHasA = false, mHasB = false, mPathOk = false;
            std::vector<ng::V3> mPath;
            CharacterSize mPathSize = CHARACTER_TYPE_A;

            // ---- helpers ----------------------------------------------------------------

            static ng::V3 local(const LLVector3& p, const LLVector3& origin)
            {
                return { p.mV[VX] - origin.mV[VX], p.mV[VY] - origin.mV[VY], p.mV[VZ] - origin.mV[VZ] };
            }

            LLViewerRegion* regionNow() const
            {
                return LLWorld::getInstance()->getRegionFromID(mRegionID);
            }

            // Ready to draw, and sets where the grid is in agent space this frame.
            bool drawable()
            {
                if (!mReady)
                {
                    return false;
                }
                const LLViewerRegion* region = regionNow();
                if (!region)
                {
                    return false;
                }
                mDrawOrigin = region->getOriginAgent();
                return true;
            }

            static CharacterSize sizeFor(LLPLCharacterType type, F32 width)
            {
                switch (type)
                {
                case LLPL_CHARACTER_TYPE_A: return CHARACTER_TYPE_A;
                case LLPL_CHARACTER_TYPE_B: return CHARACTER_TYPE_B;
                case LLPL_CHARACTER_TYPE_C: return CHARACTER_TYPE_C;
                case LLPL_CHARACTER_TYPE_D: return CHARACTER_TYPE_D;
                default: break;
                }
                // No type: the width the console gives (a diameter), a person's height.
                return { std::max(0.1f, width * 0.5f), CHARACTER_TYPE_A.height };
            }

            // Called before a set of analysis() calls, never in the middle of one (references stay valid).
            void trimAnalyses()
            {
                if (mAnalyses.size() >= 24)
                {
                    mAnalyses.clear();
                }
            }

            // Which floors a character type may use: the bit of Span::walk (0: any floor).
            static int typeBitFor(LLPLCharacterType type)
            {
                switch (type)
                {
                case LLPL_CHARACTER_TYPE_A: return 1;
                case LLPL_CHARACTER_TYPE_B: return 2;
                case LLPL_CHARACTER_TYPE_C: return 3;
                case LLPL_CHARACTER_TYPE_D: return 4;
                default: return 0;
                }
            }

            const ng::Analysis& analysis(const CharacterSize& size, int typeBit)
            {
                const std::tuple<int, int, int> key(ll_round(size.radius * 100.f), ll_round(size.height * 100.f), typeBit);
                const auto it = mAnalyses.find(key);
                if (it != mAnalyses.end())
                {
                    return it->second;
                }
                ng::Analysis& an = mAnalyses[key];
                mGrid.analyse(ng::Shape{ size.radius, size.height, typeBit }, an);
                return an;
            }

            void dump(const LLSD::Binary& block, int dir) const
            {
                const std::string name = "navmesh_" + std::to_string(dir) + ".bin";
                const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, name);
                llofstream out(path.c_str(), std::ios::binary);
                if (out && !block.empty())
                {
                    out.write(reinterpret_cast<const char*>(block.data()), static_cast<std::streamsize>(block.size()));
                    LL_INFOS("Pathing") << "Navmesh data written to " << path << LL_ENDL;
                }
            }

            // ---- building the grid from the world -----------------------------------------

            void rebuild()
            {
                LLViewerRegion* region = gAgent.getRegion();
                if (!region)
                {
                    return;
                }
                const F32 width = region->getWidth();
                if (!(width > 0.f) || width > MAX_REGION_WIDTH)
                {
                    LL_WARNS("Pathing") << "Region width " << width << " m is not covered by the client navmesh" << LL_ENDL;
                    return;
                }
                LLTimer timer;
                cleanupResidual();
                mRegionID = region->getRegionID();

                ng::Params params;
                params.width = width;
                params.climb = 0.5f;
                params.headroom = BASE_SIZE.height;
                // The ledge test keeps the height it had (0.8 m): the floor marks are shared by
                // all types, and a lower one would make more floors ledges for A, B and C too.
                params.ledgeHeadroom = LEDGE_HEADROOM;
                mGrid.begin(params);

                // terrain, one height a metre
                const int n = static_cast<int>(width) + 1;
                std::vector<F32> heights(static_cast<size_t>(n) * n);
                const LLSurface& land = region->getLand();
                for (int j = 0; j < n; ++j)
                {
                    for (int i = 0; i < n; ++i)
                    {
                        const F32 x = std::min(static_cast<F32>(i), width - 0.01f);
                        const F32 y = std::min(static_cast<F32>(j), width - 0.01f);
                        heights[static_cast<size_t>(j) * n + i] = land.resolveHeightRegion(x, y);
                    }
                }
                mGrid.addHeightfield(heights, n, 1.f);

                size_t objects = 0, triangles = 0;
                gather(region, objects, triangles);
                const std::function<bool(float, float, float)> excluded = [this](float x, float y, float z)
                {
                    for (const Box& b : mExclusions)
                    {
                        const LLVector3 q = (LLVector3(x, y, z) - b.pos) * ~b.rot;
                        if (fabsf(q.mV[VX]) <= b.scale.mV[VX] * 0.5f && fabsf(q.mV[VY]) <= b.scale.mV[VY] * 0.5f && fabsf(q.mV[VZ]) <= b.scale.mV[VZ] * 0.5f)
                        {
                            return true;
                        }
                    }
                    return false;
                };
                mGrid.finish(mExclusions.empty() ? nullptr : &excluded);

                mReady = true;
                mNavDirty = true;
                mShapesDirty = true;
                LL_INFOS("Pathing") << "Client navmesh of " << width << " m region: " << objects << " objects, " << triangles
                                    << " triangles, " << mGrid.spans.size() << " spans, " << mGrid.walkableCount()
                                    << " floors, " << mExclusions.size() << " exclusion volumes, linkset types "
                                    << (mHaveTypes ? "known" : "unknown") << ", " << ll_round(timer.getElapsedTimeF32() * 1000.f) << " ms" << LL_ENDL;
            }

            // The console's linkset list says what each linkset is (walkable, static obstacle,
            // material volume, exclusion volume...). The grid is built when it has arrived; if the
            // request fails the grid is built without it (every object solid with a walkable top).
            void requestLinksets()
            {
                mHaveTypes = false;
                mLinksets.clear();
                const LLPathfindingManager::request_id_t id = ++mRequestId;
                LLPathfindingManager::getInstance()->requestGetLinksets(id,
                    [this, id](LLPathfindingManager::request_id_t, LLPathfindingManager::ERequestStatus status, LLPathfindingObjectListPtr list)
                    {
                        if (id != mRequestId || status == LLPathfindingManager::kRequestStarted)
                        {
                            return;
                        }
                        if (status == LLPathfindingManager::kRequestCompleted && list)
                        {
                            readLinksets(*list);
                        }
                        rebuild();
                    });
            }

            void readLinksets(const LLPathfindingObjectList& list)
            {
                for (LLPathfindingObjectList::const_iterator it = list.begin(); it != list.end(); ++it)
                {
                    const std::shared_ptr<LLPathfindingLinkset> linkset = std::dynamic_pointer_cast<LLPathfindingLinkset>(it->second);
                    if (!linkset || linkset->isTerrain())
                    {
                        continue;
                    }
                    U8 mask = 1;
                    const S32 coefficient[4] = { linkset->getWalkabilityCoefficientA(), linkset->getWalkabilityCoefficientB(),
                                                 linkset->getWalkabilityCoefficientC(), linkset->getWalkabilityCoefficientD() };
                    for (int t = 0; t < 4; ++t)
                    {
                        if (coefficient[t] > 0)
                        {
                            mask = static_cast<U8>(mask | (2u << t));
                        }
                    }
                    mLinksets[linkset->getUUID()] = { linkset->getLinksetUse(), mask };
                }
                mHaveTypes = true;
                LL_INFOS("Pathing") << mLinksets.size() << " linksets with pathfinding types" << LL_ENDL;
            }

            // The region's prims and meshes as triangles in region-local metres. With the linkset
            // types known: walkable linksets are solid and their tops are floors (for the character
            // types with a walkability above 0), static and unknown ones are solid without floors,
            // dynamic ones and phantoms are left out of the grid, material and exclusion volumes
            // are shapes only (an exclusion volume also takes the floors inside it out of use).
            // Without them every static object is solid with a walkable top.
            void gather(LLViewerRegion* region, size_t& objects, size_t& triangles)
            {
                const LLVector3 origin = region->getOriginAgent();
                const S32 count = gObjectList.getNumObjects();
                std::vector<ng::V3> world;
                for (S32 i = 0; i < count; ++i)
                {
                    LLViewerObject* obj = gObjectList.getObject(i);
                    if (!obj || obj->isDead() || obj->getRegion() != region || obj->getPCode() != LL_PCODE_VOLUME)
                    {
                        continue;
                    }
                    if (obj->isAttachment() || obj->isHUDAttachment() || obj->flagUsePhysics() || obj->flagTemporaryOnRez())
                    {
                        continue;
                    }

                    using Use = LLPathfindingLinkset::ELinksetUse;
                    Use use = Use::kStaticObstacle;
                    U8 floorMask = ng::kFloorAll;
                    if (mHaveTypes)
                    {
                        const LLViewerObject* root = obj->getRootEdit();
                        const auto it = mLinksets.find(root ? root->getID() : obj->getID());
                        use = it != mLinksets.end() ? it->second.use : Use::kUnknown;
                        floorMask = (it != mLinksets.end() && use == Use::kWalkable) ? it->second.floorMask : 0;
                    }
                    const bool volumeOnly = use == Use::kMaterialVolume || use == Use::kExclusionVolume;
                    if (use == Use::kDynamicPhantom || (obj->flagPhantom() && !volumeOnly))
                    {
                        continue;
                    }
                    const bool solid = !volumeOnly && use != Use::kDynamicObstacle;

                    LLVOVolume* vol = static_cast<LLVOVolume*>(obj);
                    if (vol->isFlexible() || vol->isRiggedMesh() || vol->isVolumeGlobal())
                    {
                        continue;
                    }
                    const LLVolume* volume = vol->getVolume();
                    if (!volume)
                    {
                        continue;
                    }
                    const LLVector3 scale = vol->getScale();
                    if (std::max(scale.mV[VX], std::max(scale.mV[VY], scale.mV[VZ])) < MIN_OBJECT_SIZE && !volumeOnly)
                    {
                        continue;
                    }
                    const LLQuaternion rot = vol->getRenderRotation();
                    const LLVector3 pos = vol->getRenderPosition() - origin;
                    ++objects;
                    if (use == Use::kExclusionVolume)
                    {
                        mExclusions.push_back({ pos, rot, scale });
                    }

                    for (S32 f = 0; f < volume->getNumVolumeFaces(); ++f)
                    {
                        const LLVolumeFace& face = volume->getVolumeFace(f);
                        if (!face.mPositions || !face.mIndices || face.mNumVertices <= 0)
                        {
                            continue;
                        }
                        world.resize(static_cast<size_t>(face.mNumVertices));
                        for (S32 v = 0; v < face.mNumVertices; ++v)
                        {
                            const F32* src = face.mPositions[v].getF32ptr();
                            LLVector3 p(src[0] * scale.mV[VX], src[1] * scale.mV[VY], src[2] * scale.mV[VZ]);
                            p = p * rot + pos;
                            world[static_cast<size_t>(v)] = { p.mV[VX], p.mV[VY], p.mV[VZ] };
                        }
                        for (S32 k = 0; k + 2 < face.mNumIndices; k += 3)
                        {
                            const U16 i0 = face.mIndices[k], i1 = face.mIndices[k + 1], i2 = face.mIndices[k + 2];
                            if (i0 >= face.mNumVertices || i1 >= face.mNumVertices || i2 >= face.mNumVertices)
                            {
                                continue;
                            }
                            const ng::V3& a = world[i0];
                            const ng::V3& b = world[i1];
                            const ng::V3& c = world[i2];
                            if (solid)
                            {
                                mGrid.addTriangle(a, b, c, floorMask);
                                ++triangles;
                            }

                            // which shapes list it goes to
                            std::vector<F32>* dst = nullptr;
                            if (!mHaveTypes)
                            {
                                ng::V3 nrm = ng::cross(b - a, c - a);
                                const F32 len = ng::length(nrm);
                                if (!(len > 1e-9f))
                                {
                                    continue;
                                }
                                dst = nrm.z / len >= mGrid.p.slopeCos ? &mWalkTris : &mObstTris;
                            }
                            else if (use == Use::kWalkable)
                            {
                                dst = &mWalkTris;
                            }
                            else if (use == Use::kMaterialVolume)
                            {
                                dst = &mMatTris;
                            }
                            else if (use == Use::kExclusionVolume)
                            {
                                dst = &mExclTris;
                            }
                            else
                            {
                                dst = &mObstTris;
                            }
                            if (dst->size() < MAX_SHAPE_TRIANGLES * 9)
                            {
                                const F32 tri[9] = { a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z };
                                dst->insert(dst->end(), tri, tri + 9);
                            }
                        }
                    }
                }
            }

            // ---- the drawing buffers --------------------------------------------------------

            void ensureNav()
            {
                if (!mNavDirty)
                {
                    return;
                }
                mNavDirty = false;
                trimAnalyses();
                mNavVB = mEdgeVB = nullptr;
                mNavCount = mEdgeCount = 0;

                // The grid of the smallest character is what is drawn; with a type chosen, the floors
                // that type cannot use are shown in the obstacle colour and the rest as a heat map.
                const ng::Analysis& base = analysis(BASE_SIZE, 0);
                const bool typed = mHeatType != LLPL_CHARACTER_TYPE_NONE;
                const ng::Analysis* typeAn = typed ? &analysis(sizeFor(mHeatType, 0.f), typeBitFor(mHeatType)) : nullptr;
                const F32 typeRadius = typed ? sizeFor(mHeatType, 0.f).radius : 0.f;

                const auto packed = [](const LLColor4U& c)
                {
                    return static_cast<U32>(c.mV[0]) | (static_cast<U32>(c.mV[1]) << 8) | (static_cast<U32>(c.mV[2]) << 16) | (static_cast<U32>(c.mV[3]) << 24);
                };
                std::vector<U32> color(mGrid.spans.size(), 0);
                for (size_t s = 0; s < color.size(); ++s)
                {
                    if (base.clear[s] < 0.f)
                    {
                        continue;
                    }
                    if (!typed)
                    {
                        color[s] = packed(mColors.mWalkable);
                    }
                    else if (!mGrid.allowed(*typeAn, static_cast<int>(s)))
                    {
                        color[s] = packed(mColors.mObstacle);
                    }
                    else
                    {
                        const F32 t = llclamp((typeAn->clear[s] - typeRadius) / 2.f, 0.f, 1.f);
                        const LLColor4 c = lerp(mColors.mHeatColorBase, mColors.mHeatColorMax, t);
                        color[s] = packed(toU(c));
                    }
                }
                const auto unpack = [](U32 c) { return LLColor4U(static_cast<U8>(c & 255), static_cast<U8>((c >> 8) & 255), static_cast<U8>((c >> 16) & 255), static_cast<U8>(c >> 24)); };

                const std::vector<ng::Rect> rects = mGrid.greedyRects(color);
                std::vector<Vtx> tris, lines;
                tris.reserve(rects.size() * 6);
                const LLVector3 up(0.f, 0.f, 1.f);
                const F32 cell = mGrid.p.cell;
                for (const ng::Rect& r : rects)
                {
                    const F32 x0 = static_cast<F32>(r.x0) * cell, x1 = static_cast<F32>(r.x1 + 1) * cell;
                    const F32 y0 = static_cast<F32>(r.y0) * cell, y1 = static_cast<F32>(r.y1 + 1) * cell;
                    const F32 z = mGrid.unitsToZ(r.hi) + LIFT;
                    const LLColor4U c = unpack(r.color);
                    const LLVector3 a(x0, y0, z), b(x1, y0, z), d(x1, y1, z), e(x0, y1, z);
                    for (const LLVector3& p : { a, b, d, a, d, e })
                    {
                        tris.push_back({ p, up, c });
                    }
                    // the outline of the block
                    const LLColor4U lc = mColors.mConnectedEdge;
                    for (const auto& seg : { std::make_pair(a, b), std::make_pair(b, d), std::make_pair(d, e), std::make_pair(e, a) })
                    {
                        lines.push_back({ seg.first, up, lc });
                        lines.push_back({ seg.second, up, lc });
                    }
                }
                // and, over it, the sides that have nothing to step to
                for (const ng::Seg& g : mGrid.boundarySegs(color))
                {
                    const F32 z = mGrid.unitsToZ(g.hi) + LIFT;
                    lines.push_back({ LLVector3(g.x0, g.y0, z), up, mColors.mBoundaryEdge });
                    lines.push_back({ LLVector3(g.x1, g.y1, z), up, mColors.mBoundaryEdge });
                }
                mNavVB = makeVB(tris);
                mNavCount = mNavVB ? static_cast<U32>(tris.size()) : 0;
                mEdgeVB = makeVB(lines);
                mEdgeCount = mEdgeVB ? static_cast<U32>(lines.size()) : 0;
            }

            static void trianglesToVertices(const std::vector<F32>& tris, const LLColor4U& color, std::vector<Vtx>& out)
            {
                out.reserve(tris.size() / 3);
                for (size_t i = 0; i + 8 < tris.size(); i += 9)
                {
                    const LLVector3 a(tris[i], tris[i + 1], tris[i + 2]);
                    const LLVector3 b(tris[i + 3], tris[i + 4], tris[i + 5]);
                    const LLVector3 c(tris[i + 6], tris[i + 7], tris[i + 8]);
                    LLVector3 n = (b - a) % (c - a);
                    n.normalize();
                    for (const LLVector3& p : { a, b, c })
                    {
                        out.push_back({ p, n, color });
                    }
                }
            }

            void ensureShapes()
            {
                if (!mShapesDirty)
                {
                    return;
                }
                mShapesDirty = false;
                std::vector<Vtx> v;
                trianglesToVertices(mWalkTris, mColors.mWalkable, v);
                mWalkVB = makeVB(v);
                mWalkCount = mWalkVB ? static_cast<U32>(v.size()) : 0;
                v.clear();
                trianglesToVertices(mObstTris, mColors.mObstacle, v);
                mObstVB = makeVB(v);
                mObstCount = mObstVB ? static_cast<U32>(v.size()) : 0;
                v.clear();
                trianglesToVertices(mMatTris, mColors.mMaterial, v);
                mMatVB = makeVB(v);
                mMatCount = mMatVB ? static_cast<U32>(v.size()) : 0;
                v.clear();
                trianglesToVertices(mExclTris, mColors.mExclusion, v);
                mExclVB = makeVB(v);
                mExclCount = mExclVB ? static_cast<U32>(v.size()) : 0;
            }

            // ---- capsules --------------------------------------------------------------------

            static U32 capsuleCount()
            {
                return 16 * 11 * 6;       // SEGMENTS * (profile points - 1) * 6, as in buildCapsule
            }

            LLVertexBuffer* capsuleVB(F32 radius, F32 cyl, bool horizontal, const LLColor4U& color)
            {
                const int r = ll_round(radius * 1000.f), l = ll_round(cyl * 1000.f);
                const U32 packed = static_cast<U32>(color.mV[0]) | (static_cast<U32>(color.mV[1]) << 8) | (static_cast<U32>(color.mV[2]) << 16) | (static_cast<U32>(color.mV[3]) << 24);
                for (const CapsuleEntry& e : mCapsules)
                {
                    if (e.r == r && e.l == l && e.horizontal == horizontal && e.color == packed)
                    {
                        return e.vb.get();
                    }
                }
                if (mCapsules.size() >= 16)
                {
                    mCapsules.clear();
                }
                std::vector<Vtx> v;
                buildCapsule(v, radius, cyl, horizontal, color);
                mCapsules.push_back({ r, l, horizontal, packed, makeVB(v) });
                return mCapsules.back().vb.get();
            }
        };

        void init()
        {
            // Never deleted: the viewer's statics are destroyed after GL is gone.
            static PlusPathingLib* const lib = new PlusPathingLib;
            LLPathingLib::setReplacement(lib);
        }

        plus::FeatureRegistration sRegistration({ "pathing", &init, {} });
    }
}
