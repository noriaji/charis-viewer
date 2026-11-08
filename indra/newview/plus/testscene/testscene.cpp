/**
 * @file testscene.cpp
 * @brief Test scene: building and removing the made-up objects (see testscene.h).
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

// Nothing here sends anything (checked against the official code, testscene 2026-10-24):
//   - createObjectViewer() makes the object in the viewer only (as the sky, the water and the preview avatars).
//   - The extra parameter of a mesh is set with local_origin false (true would send ObjectExtraParams).
//   - No texture is asked for by UUID: the TEs get our textures (setTEImage), which are made from pictures with
//     FTT_LOCAL_FILE at full size; an LLViewerFetchedTexture never scales down (only LLViewerLODTexture does), so the
//     fetcher never has a smaller level to fill in.
//   - A mesh's 4 LODs are loaded volumes before the object takes one (as plus/localmesh does), and the skin info is
//     marked unavailable: LLVOVolume::setVolume() never calls loadMesh() or getSkinInfo().
//   - mbCanSelect false: no pick (LLVOVolume::lineSegmentIntersect()), so no hover, touch, grab, look-at or select
//     messages; and no rectangle selection (LLSelectMgr::canSelectObject(), the <plus> line), no area search.

#include "llviewerprecompiledheaders.h"

#include "testscene.h"
#include "testscenedef.h"

#include "plus/plus.h"
#include "plus/rlv/rlvhooks.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llapp.h"
#include "llcallbacklist.h"
#include "lldrawable.h"
#include "llevents.h"
#include "llfile.h"
#include "llframetimer.h"
#include "llimage.h"
#include "llprimitive.h"
#include "llrhi.h"
#include "llrhihash.h"
#include "llsdutil.h"
#include "llsdutil_math.h"
#include "llspatialpartition.h"
#include "llstartup.h"
#include "llthread.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewertextureanim.h"
#include "llvolume.h"
#include "llvolumemgr.h"
#include "llvovolume.h"
#include "pipeline.h"

#include <array>
#include <cmath>
#include <memory>
#include <set>
#include <sstream>

namespace plus::testscene
{
    namespace
    {
        constexpr char SETTING[]      = "PlusTestScene";
        constexpr char BUILD_CUE[]    = "testscene.build";
        constexpr char CLEAR_CUE[]    = "testscene.clear";
        constexpr char RESULT_CUE[]   = "testscene.result";
        constexpr F64  POLL_SECONDS   = 1.0;
        constexpr S64  MAX_CUE_BYTES  = 64 * 1024;
        constexpr F32  ANCHOR_GRID    = 8.f;      // the avatar's place is rounded to this (metres)
        constexpr F32  MAX_HEIGHT     = 4000.f;   // the highest place an object is put (the region's limit is 4096)
        constexpr F32  SHEET_THICKNESS = 0.01f;
        constexpr S32  LOD_COUNT      = 4;
        constexpr U8   SHEET_FACE     = 0;        // a box's top face: the sheet's one drawn face

        // LLViewerObject::mStatic ("the object does not move") is protected, and only processUpdateMessage() sets it
        // (true for an object the simulator sends without velocity). A viewer-made object keeps false: it stays on
        // the object list's active list, an idleUpdate() every frame, which a still object of the simulator's does
        // not have. A pointer to the member, formed in a class derived from LLViewerObject (the only place the
        // language lets the protected member be named), sets it without an edit to the official header.
        struct StaticFlag : public LLViewerObject
        {
            using Member = bool LLViewerObject::*;
            StaticFlag() = delete;
            static Member member() { return &StaticFlag::mStatic; }
        };

        // splitmix64: the same numbers on every compiler (the <random> distributions are not).
        class Rng
        {
        public:
            explicit Rng(U64 seed) : mState(seed) {}
            U64 next()
            {
                U64 z = (mState += 0x9E3779B97F4A7C15ull);
                z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
                z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
                return z ^ (z >> 31);
            }
            F32 unit() { return (F32)((next() >> 40) * (1.0 / 16777216.0)); }   // [0, 1)
            F32 range(F32 low, F32 high) { return low + (high - low) * unit(); }
            U32 below(U32 n) { return n ? (U32)((next() >> 32) % n) : 0; }
        private:
            U64 mState;
        };

        // FNV-1a over what was built (relative places, sizes, turns, textures): the same scene, the same hash,
        // wherever the avatar stands.
        class Hash
        {
        public:
            void bytes(const void* data, size_t size)
            {
                mValue = LLRHI::fnv1a(mValue, data, size);
            }
            template <typename T> void add(const T& value) { bytes(&value, sizeof(value)); }
            void vec(const LLVector3& v) { bytes(v.mV, sizeof(v.mV)); }
            void quat(const LLQuaternion& q) { bytes(q.mQ, sizeof(q.mQ)); }
            U64 value() const { return mValue; }
        private:
            U64 mValue = LLRHI::FNV1A_BASIS;
        };

        // Different streams for the different parts: a change of one count leaves the rest as they were.
        enum Stream : U64 { STREAM_TEXTURES = 1, STREAM_ALPHA_TEXTURES, STREAM_PRIMS, STREAM_MESHES, STREAM_SHEETS, STREAM_CLUSTERS };

        U64 streamSeed(U32 seed, Stream stream)
        {
            return ((U64)seed << 8) ^ ((U64)stream * 0xD1B54A32D192ED03ull);
        }

        struct MeshKind
        {
            LLVolumeParams                  params;
            std::array<LLVolume*, LOD_COUNT> lods {};   // our references, held while the scene is
        };

        class Scene
        {
        public:
            Definition  def;
            U64         regionHandle = 0;
            std::string regionName;
            LLVector3   middle;                 // region coordinates
            U64         hash = 0;
            F64         builtAt = 0.0;
            U32         faces = 0;
            U32         alphaFaces = 0;
            U32         animated = 0;
            U64         texels = 0;
            std::vector<LLPointer<LLViewerObject>>         objects;
            std::vector<LLPointer<LLViewerFetchedTexture>> textures;
            std::vector<LLPointer<LLViewerFetchedTexture>> alphaTextures;
            std::vector<MeshKind>                          meshKinds;
            boost::signals2::connection                    regionChanged;

            Scene() = default;
            Scene(const Scene&) = delete;
            Scene& operator=(const Scene&) = delete;
            ~Scene() { release(); }

            // Every way out (a clear, a failed build, the region, quitting) comes here, once.
            void release()
            {
                regionChanged.disconnect();
                for (LLPointer<LLViewerObject>& object : objects)
                {
                    if (object.notNull() && !object->isDead())
                    {
                        gObjectList.killObject(object);
                    }
                }
                objects.clear();
                // The objects keep their own references to the volumes and the textures until the object list lets
                // them go (cleanDeadObjects); ours go now. Textures nobody uses leave the texture list by themselves
                // (LLViewerTextureList's lazy flush, 30 s).
                for (MeshKind& kind : meshKinds)
                {
                    for (LLVolume*& volume : kind.lods)
                    {
                        if (volume)
                        {
                            LLPrimitive::getVolumeManager()->unrefVolume(volume);
                            volume = nullptr;
                        }
                    }
                }
                meshKinds.clear();
                textures.clear();
                alphaTextures.clear();
            }
        };

        // A plain pointer on purpose: no static destructor runs after the object list and the pipeline are gone.
        // "quitting" clears it before that.
        Scene* sScene = nullptr;

        std::string sCueDir;
        std::string sBuildCue;    // <LLRHI_CUE_DIR>/testscene.build, made once
        std::string sClearCue;
        F64         sNextPoll = 0.0;

        std::string hex(U64 value)
        {
            return llformat("%016llx", (unsigned long long)value);
        }

        // --- shapes and textures ------------------------------------------------------------------

        LLVolumeParams shapeParams(Shape shape)
        {
            LLVolumeParams p;
            switch (shape)
            {
            case Shape::Cylinder:
                p.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_LINE);
                p.setRatio(1.f, 1.f);
                break;
            case Shape::Sphere:
                p.setType(LL_PCODE_PROFILE_CIRCLE_HALF, LL_PCODE_PATH_CIRCLE);
                p.setRatio(1.f, 1.f);
                break;
            case Shape::Torus:
                p.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_CIRCLE);
                p.setRatio(1.f, 0.25f);
                break;
            case Shape::Prism:
                p.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
                p.setRatio(0.f, 1.f);
                p.setShear(-0.5f, 0.f);
                break;
            case Shape::Tube:
                p.setType(LL_PCODE_PROFILE_CIRCLE, LL_PCODE_PATH_LINE);
                p.setRatio(1.f, 1.f);
                p.setHollow(0.5f);
                break;
            case Shape::Box:
            default:
                p.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
                p.setRatio(1.f, 1.f);
                break;
            }
            p.setBeginAndEndS(0.f, 1.f);
            p.setBeginAndEndT(0.f, 1.f);
            if (shape != Shape::Prism)
            {
                p.setShear(0.f, 0.f);
            }
            return p;
        }

        // A picture of two colours (checks, stripes or a gradient); with alpha, streaks of rain in the alpha.
        LLPointer<LLViewerFetchedTexture> makeTexture(Rng& rng, U32 size, bool alpha, Hash& hash)
        {
            const S8 components = alpha ? 4 : 3;
            LLPointer<LLImageRaw> raw = new LLImageRaw((U16)size, (U16)size, components);
            U8* data = raw->getData();
            if (!data)
            {
                return nullptr;
            }
            U8 a[3], b[3];
            for (int c = 0; c < 3; ++c)
            {
                a[c] = (U8)(64 + rng.below(192));
                b[c] = (U8)(a[c] / 3);
            }
            const U32 pattern = rng.below(3);
            const U32 cells   = 2u << rng.below(4);   // 2 to 16 across
            const U32 streaks = 4u + rng.below(28);   // columns of rain in the alpha
            hash.add(pattern);
            hash.add(cells);
            hash.bytes(a, sizeof(a));
            for (U32 y = 0; y < size; ++y)
            {
                for (U32 x = 0; x < size; ++x)
                {
                    F32 t = 0.f;
                    if (pattern == 0)
                    {
                        t = (F32)(((x * cells / size) + (y * cells / size)) & 1);
                    }
                    else if (pattern == 1)
                    {
                        t = (F32)((x * cells / size) & 1);
                    }
                    else
                    {
                        t = (F32)(x + y) / (F32)(2 * size);
                    }
                    U8* px = data + ((size_t)y * size + x) * components;
                    for (int c = 0; c < 3; ++c)
                    {
                        px[c] = (U8)((F32)a[c] * (1.f - t) + (F32)b[c] * t);
                    }
                    if (alpha)
                    {
                        const U32 column = x * streaks / size;
                        const bool streak = ((column * 2654435761u) >> 29) == 0 || (x * streaks) % size < size / 8;
                        px[3] = streak ? (U8)(160 + ((y * 7) & 63)) : (U8)40;
                    }
                }
            }
            return LLViewerTextureManager::getFetchedTexture(raw, FTT_LOCAL_FILE, true);
        }

        // --- objects ------------------------------------------------------------------------------

        LLVOVolume* newVolume(LLViewerRegion* region)
        {
            LLViewerObject* object = gObjectList.createObjectViewer(LL_PCODE_VOLUME, region);
            LLVOVolume* volume = dynamic_cast<LLVOVolume*>(object);
            if (!volume)
            {
                if (object)
                {
                    gObjectList.killObject(object);
                }
                return nullptr;
            }
            volume->mbCanSelect = false;
            return volume;
        }

        // The end of an object, as processUpdateCore() ends one the simulator sent: still (off the active list),
        // then its drawable. Everything is set by now: a change after addObject() would move it (an active drawable).
        void finish(Scene& scene, LLVOVolume* volume)
        {
            volume->*StaticFlag::member() = true;
            gObjectList.updateActive(volume);
            gPipeline.addObject(volume);
            volume->setPixelAreaAndAngle(gAgent);
            scene.faces += volume->getNumTEs();
            scene.objects.emplace_back(volume);
        }

        LLVector3 clampToRegion(const LLVector3& place, F32 width)
        {
            return LLVector3(llclamp(place.mV[VX], 0.5f, width - 0.5f), llclamp(place.mV[VY], 0.5f, width - 0.5f),
                             llclamp(place.mV[VZ], 1.f, MAX_HEIGHT));
        }

        // Places: evenly within the box, or near one of the clusters.
        class Placer
        {
        public:
            Placer(const Definition& def, const LLVector3& middle, F32 width) : mDef(def), mMiddle(middle), mWidth(width)
            {
                Rng rng(streamSeed(def.seed, STREAM_CLUSTERS));
                for (U32 i = 0; i < def.clusters; ++i)
                {
                    mClusters.emplace_back(rng.range(-def.radius, def.radius), rng.range(-def.radius, def.radius),
                                           rng.range(-def.vertical * 0.5f, def.vertical * 0.5f));
                }
            }

            LLVector3 place(Rng& rng) const
            {
                LLVector3 offset;
                if (mClusters.empty())
                {
                    offset.set(rng.range(-mDef.radius, mDef.radius), rng.range(-mDef.radius, mDef.radius),
                               rng.range(-mDef.vertical * 0.5f, mDef.vertical * 0.5f));
                }
                else
                {
                    const F32 r = mDef.clusterRadius;
                    offset = mClusters[rng.below((U32)mClusters.size())] +
                             LLVector3(rng.range(-r, r), rng.range(-r, r), rng.range(-r * 0.5f, r * 0.5f));
                }
                return clampToRegion(mMiddle + offset, mWidth);
            }

        private:
            const Definition&      mDef;
            LLVector3              mMiddle;
            F32                    mWidth;
            std::vector<LLVector3> mClusters;
        };

        LLQuaternion randomTurn(Rng& rng)
        {
            const LLQuaternion yaw(rng.range(0.f, F_TWO_PI), LLVector3::z_axis);
            const LLQuaternion tilt(rng.range(-0.4f, 0.4f), LLVector3::x_axis);
            return tilt * yaw;
        }

        LLVector3 randomSize(Rng& rng, F32 low, F32 high)
        {
            return LLVector3(rng.range(low, high), rng.range(low, high), rng.range(low, high));
        }

        // Prims and meshes: opaque, a texture for the object (or each face), some full bright.
        void dressOpaque(Scene& scene, LLVOVolume* volume, Rng& rng, Hash& hash)
        {
            const Definition& def = scene.def;
            const U32 shared = rng.below((U32)scene.textures.size());
            const F32 tint   = rng.range(0.75f, 1.f);
            const bool full  = rng.unit() < def.fullbright;
            hash.add(shared);
            hash.add(full);
            for (U8 te = 0; te < volume->getNumTEs(); ++te)
            {
                const U32 index = def.texturePerFace ? rng.below((U32)scene.textures.size()) : shared;
                volume->setTEImage(te, scene.textures[index]);
                volume->setTEColor(te, LLColor4(tint, tint, tint, 1.f));
                if (full)
                {
                    volume->setTEFullbright(te, 1);
                }
                if (def.texturePerFace)
                {
                    hash.add(index);
                }
            }
        }

        bool buildPrims(Scene& scene, LLViewerRegion* region, const Placer& placer, Hash& hash, std::string& error)
        {
            const Definition& def = scene.def;
            Rng rng(streamSeed(def.seed, STREAM_PRIMS));
            for (U32 i = 0; i < def.prims; ++i)
            {
                const Shape     shape = def.shapes[rng.below((U32)def.shapes.size())];
                const LLVector3 place = placer.place(rng);
                const LLQuaternion turn = randomTurn(rng);
                const LLVector3 size  = randomSize(rng, def.sizeMin, def.sizeMax);
                LLVOVolume* volume = newVolume(region);
                if (!volume)
                {
                    error = "the object list made no object";
                    return false;
                }
                volume->setPositionRegion(place);
                volume->setRotation(turn);
                volume->setScale(size, false);
                volume->setVolume(shapeParams(shape), 0);
                dressOpaque(scene, volume, rng, hash);
                hash.add((U8)shape);
                hash.vec(place - scene.middle);
                hash.quat(turn);
                hash.vec(size);
                finish(scene, volume);
            }
            return true;
        }

        // The kinds of mesh: the faces of a prim at each LOD, under an id of our own, as a loaded mesh asset.
        bool makeMeshKinds(Scene& scene, Hash& hash, std::string& error)
        {
            const Definition& def = scene.def;
            LLVolumeMgr* volumes = LLPrimitive::getVolumeManager();
            scene.meshKinds.resize(def.meshKinds);
            for (U32 k = 0; k < def.meshKinds; ++k)
            {
                MeshKind& kind = scene.meshKinds[k];
                LLVolumeParams shape = shapeParams(def.shapes[k % def.shapes.size()]);
                // A twist for the kinds past the shapes: other meshes from the same prims.
                const F32 twist = (F32)(k / def.shapes.size()) * 0.25f;
                shape.setTwistEnd(twist - std::floor(twist));
                hash.add(k);
                hash.add(twist);

                LLUUID mesh_id;
                mesh_id.generate();
                // The volume params of an uploaded mesh (LLMeshUploadThread, plus/localmesh makeVolumes()).
                kind.params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
                kind.params.setBeginAndEndS(0.f, 1.f);
                kind.params.setBeginAndEndT(0.f, 1.f);
                kind.params.setRatio(1.f, 1.f);
                kind.params.setShear(0.f, 0.f);
                kind.params.setSculptID(mesh_id, LL_SCULPT_TYPE_MESH);
                for (S32 lod = 0; lod < LOD_COUNT; ++lod)
                {
                    LLVolume* source = volumes->refVolume(shape, lod);
                    LLVolume* mesh   = volumes->refVolume(kind.params, lod);
                    if (!source || !mesh)
                    {
                        if (source)
                        {
                            volumes->unrefVolume(source);
                        }
                        if (mesh)
                        {
                            volumes->unrefVolume(mesh);
                        }
                        error = "no volume for a mesh";
                        return false;
                    }
                    kind.lods[lod] = mesh;
                    mesh->copyFacesFrom(source->getVolumeFaces());
                    mesh->setMeshAssetLoaded(true);
                    volumes->unrefVolume(source);
                    if (mesh->getNumVolumeFaces() == 0)
                    {
                        error = "a mesh with no faces";
                        return false;
                    }
                }
            }
            return true;
        }

        bool buildMeshes(Scene& scene, LLViewerRegion* region, const Placer& placer, Hash& hash, std::string& error)
        {
            const Definition& def = scene.def;
            if (!def.meshes)
            {
                return true;
            }
            if (!makeMeshKinds(scene, hash, error))
            {
                return false;
            }
            Rng rng(streamSeed(def.seed, STREAM_MESHES));
            for (U32 i = 0; i < def.meshes; ++i)
            {
                const U32       k     = rng.below((U32)scene.meshKinds.size());
                const LLVector3 place = placer.place(rng);
                const LLQuaternion turn = randomTurn(rng);
                const LLVector3 size  = randomSize(rng, def.sizeMin, def.sizeMax);
                LLVOVolume* volume = newVolume(region);
                if (!volume)
                {
                    error = "the object list made no object";
                    return false;
                }
                volume->setPositionRegion(place);
                volume->setRotation(turn);
                volume->setScale(size, false);
                // Not rigged: no skin info to look for under the made-up id. The sculpt parameter makes it a mesh
                // (isMesh()); local_origin false: kept in the viewer (true would send ObjectExtraParams).
                volume->notifySkinInfoUnavailable();
                LLSculptParams sculpt;
                sculpt.setSculptTexture(scene.meshKinds[k].params.getSculptID(), LL_SCULPT_TYPE_MESH);
                volume->setParameterEntry(LLNetworkData::PARAMS_SCULPT, sculpt, false);
                volume->setVolume(scene.meshKinds[k].params, 0);
                dressOpaque(scene, volume, rng, hash);
                hash.add(k);
                hash.vec(place - scene.middle);
                hash.quat(turn);
                hash.vec(size);
                finish(scene, volume);
            }
            return true;
        }

        // Alpha sheets: a flat box stood up, its top face drawn at the scene's alpha, the others fully transparent
        // (not drawn): one alpha face a sheet. Some scroll their texture.
        bool buildSheets(Scene& scene, LLViewerRegion* region, const Placer& placer, Hash& hash, std::string& error)
        {
            const Definition& def = scene.def;
            Rng rng(streamSeed(def.seed, STREAM_SHEETS));
            const LLVolumeParams box = shapeParams(Shape::Box);
            const LLQuaternion stand(F_PI_BY_TWO, LLVector3::x_axis);
            for (U32 i = 0; i < def.alphaSheets; ++i)
            {
                const LLVector3 place = placer.place(rng);
                const LLQuaternion turn = stand * LLQuaternion(rng.range(0.f, F_TWO_PI), LLVector3::z_axis);
                const LLVector3 size(rng.range(def.sheetSizeMin, def.sheetSizeMax), rng.range(def.sheetSizeMin, def.sheetSizeMax),
                                     SHEET_THICKNESS);
                const U32  index    = rng.below((U32)scene.alphaTextures.size());
                const bool animated = rng.unit() < def.texanim;
                const F32  rate     = def.texanimRate * rng.range(0.6f, 1.4f);
                LLVOVolume* volume = newVolume(region);
                if (!volume)
                {
                    error = "the object list made no object";
                    return false;
                }
                volume->setPositionRegion(place);
                volume->setRotation(turn);
                volume->setScale(size, false);
                volume->setVolume(box, 0);
                for (U8 te = 0; te < volume->getNumTEs(); ++te)
                {
                    volume->setTEImage(te, scene.alphaTextures[index]);
                    volume->setTEColor(te, LLColor4(1.f, 1.f, 1.f, te == SHEET_FACE ? def.alpha : 0.f));
                }
                ++scene.alphaFaces;
                hash.add(index);
                hash.add(animated);
                hash.add(animated ? rate : 0.f);
                hash.vec(place - scene.middle);
                hash.quat(turn);
                hash.vec(size);
                finish(scene, volume);
                // After the drawable: LLVOVolume::animateTextures() (every frame, from LLViewerTextureAnim::updateClass())
                // reads it. With RenderDelayCreation the drawable comes later: no animation then (counted in texanim).
                if (animated && volume->mDrawable.notNull())
                {
                    // As an ObjectUpdate's texture animation (processUpdateMessage()): smooth scroll of every face.
                    volume->mTextureAnimp = new LLViewerTextureAnim(volume);
                    volume->mTextureAnimp->mMode   = LLTextureAnim::ON | LLTextureAnim::LOOP | LLTextureAnim::SMOOTH;
                    volume->mTextureAnimp->mFace   = -1;
                    volume->mTextureAnimp->mSizeX  = 1;
                    volume->mTextureAnimp->mSizeY  = 1;
                    volume->mTextureAnimp->mStart  = 0.f;
                    volume->mTextureAnimp->mLength = 1.f;
                    volume->mTextureAnimp->mRate   = rate;
                    volume->mTexAnimMode = 0;
                    ++scene.animated;
                }
            }
            return true;
        }

        bool makeTextures(Scene& scene, Hash& hash, std::string& error)
        {
            const Definition& def = scene.def;
            const auto make = [&](U32 count, bool alpha, Stream stream, std::vector<LLPointer<LLViewerFetchedTexture>>& out)
            {
                Rng rng(streamSeed(def.seed, stream));
                out.reserve(count);
                for (U32 i = 0; i < count; ++i)
                {
                    LLPointer<LLViewerFetchedTexture> texture = makeTexture(rng, def.textureSize, alpha, hash);
                    if (texture.isNull())
                    {
                        error = "a texture could not be made (out of memory?)";
                        return false;
                    }
                    out.push_back(texture);
                    scene.texels += (U64)def.textureSize * def.textureSize;
                }
                return true;
            };
            hash.add(def.textureSize);
            return (def.prims + def.meshes == 0 || make(def.textures, false, STREAM_TEXTURES, scene.textures)) &&
                   (def.alphaSheets == 0 || make(def.alphaTextures, true, STREAM_ALPHA_TEXTURES, scene.alphaTextures));
        }

        // The avatar's place rounded to the grid, + height; or the definition's place.
        bool middleOf(const Definition& def, LLViewerRegion* region, LLVector3& middle, std::string& error)
        {
            const F32 width = region->getWidth();
            if (def.centerOnAgent)
            {
                const LLVector3 agent = region->getPosRegionFromAgent(gAgent.getPositionAgent());
                for (int i = 0; i < 3; ++i)
                {
                    middle.mV[i] = std::round(agent.mV[i] / ANCHOR_GRID) * ANCHOR_GRID;
                }
                middle.mV[VZ] += def.height;
            }
            else
            {
                middle = def.center;
            }
            if (middle.mV[VX] < 0.f || middle.mV[VY] < 0.f || middle.mV[VX] > width || middle.mV[VY] > width ||
                middle.mV[VZ] < 1.f || middle.mV[VZ] > MAX_HEIGHT)
            {
                error = llformat("the middle %.1f, %.1f, %.1f is outside the region (width %.0f, height 1..%.0f)", middle.mV[VX],
                                 middle.mV[VY], middle.mV[VZ], width, MAX_HEIGHT);
                return false;
            }
            return true;
        }

        void writeResult(const std::string& line)
        {
            if (sCueDir.empty())
            {
                return;
            }
            const std::string file = gDirUtilp->add(sCueDir, RESULT_CUE);
            if (LLFILE* f = LLFile::fopen(file, LLFILE_MODE("wb")))
            {
                fwrite(line.data(), 1, line.size(), f);
                fputc('\n', f);
                LLFile::close(f);
            }
        }

        // --- the cue files ------------------------------------------------------------------------

        // testscene.build: the scene's name on a line of its own (or "scene = name"), "look = 1" for the camera,
        // any other "key = value" an override of the scene's keys.
        void buildFromCue(const std::string& text)
        {
            std::string scene;
            bool look = false;
            LLSD overrides = LLSD::emptyMap();
            std::istringstream lines(text);
            std::string line;
            while (std::getline(lines, line))
            {
                const size_t comment = line.find('#');
                if (comment != std::string::npos)
                {
                    line.erase(comment);
                }
                if (line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0)
                {
                    line.erase(0, 3);
                }
                LLStringUtil::trim(line);
                if (line.empty())
                {
                    continue;
                }
                const size_t equals = line.find('=');
                if (equals == std::string::npos)
                {
                    scene = line;
                    continue;
                }
                std::string key   = line.substr(0, equals);
                std::string value = line.substr(equals + 1);
                LLStringUtil::trim(key);
                LLStringUtil::trim(value);
                if (key == "scene")
                {
                    scene = value;
                }
                else if (key == "look")
                {
                    look = value == "1" || value == "true" || value == "on";
                }
                else
                {
                    overrides[key] = value;
                }
            }
            std::string error, summary;
            if (scene.empty())
            {
                error = "no scene named";
            }
            else if (build(scene, overrides, error, &summary) && (!look || lookAt(error)))
            {
                writeResult("built " + summary);
                return;
            }
            LL_WARNS("TestScene") << "TESTSCENE failed reason=" << error << LL_ENDL;
            writeResult("failed " + error);
        }

        bool readCue(const std::string& file, std::string& text)
        {
            const S64 bytes = LLFile::size(file);
            if (bytes < 0 || bytes > MAX_CUE_BYTES)
            {
                return false;
            }
            llifstream stream(file.c_str(), std::ios::binary);
            if (!stream.is_open())
            {
                return false;
            }
            std::ostringstream all;
            all << stream.rdbuf();
            text = all.str();
            return true;
        }

        // Every frame, only with LLRHI_CUE_DIR: nothing while PlusTestScene is off, else a clock check.
        void onIdle(void*)
        {
            static LLCachedControl<bool> on(gSavedSettings, SETTING, false);
            if (!on)
            {
                return;
            }
            const F64 now = LLFrameTimer::getElapsedSeconds();
            if (now < sNextPoll)
            {
                return;
            }
            sNextPoll = now + POLL_SECONDS;
            if (LLFile::isfile(sClearCue))
            {
                LLFile::remove(sClearCue);
                clear("cue");
                writeResult("cleared");
            }
            if (LLFile::isfile(sBuildCue))
            {
                // Once a second at most, and only when the file is there: reading it may allocate.
                std::string text;
                const bool read = readCue(sBuildCue, text);
                LLFile::remove(sBuildCue);
                if (!read)
                {
                    LL_WARNS("TestScene") << "TESTSCENE failed reason=cannot read " << sBuildCue << LL_ENDL;
                    writeResult("failed cannot read the cue");
                    return;
                }
                buildFromCue(text);
            }
        }

        void init()
        {
            // Quitting (before cleanup(): the object list and the pipeline are still there).
            LLEventPumps::instance().obtain("LLApp").listen("PlusTestScene", [](const LLSD& event)
            {
                if (event["status"].asString() == "quitting" && on_main_thread())
                {
                    clear("quitting");
                }
                return false;
            });
            sCueDir = LLRHI::cueDir();     // UTF-8, as LLFile takes it
            if (!sCueDir.empty())
            {
                sBuildCue = gDirUtilp->add(sCueDir, BUILD_CUE);
                sClearCue = gDirUtilp->add(sCueDir, CLEAR_CUE);
                gIdleCallbacks.addFunction(&onIdle, nullptr);
            }
        }

        plus::FeatureRegistration sRegistration({ "testscene", &init });
    }

    bool enabled()
    {
        static LLCachedControl<bool> on(gSavedSettings, SETTING, false);
        return on;
    }

    bool isBuilt()
    {
        return sScene != nullptr;
    }

    std::string tag()
    {
        return sScene ? sScene->def.name + ":" + hex(sScene->hash) : std::string();
    }

    void clear(const std::string& reason)
    {
        if (!sScene)
        {
            return;
        }
        Scene* scene = sScene;
        sScene = nullptr;   // first: a callback from within the release finds no scene
        const size_t count = scene->objects.size();
        const std::string name = scene->def.name;
        const std::string hash = hex(scene->hash);
        delete scene;
        LL_INFOS("TestScene") << "TESTSCENE cleared name=" << name << " objects=" << count << " hash=" << hash
                              << " reason=" << reason << LL_ENDL;
    }

    bool build(const std::string& name, const LLSD& overrides, std::string& error, std::string* summary)
    {
        if (!on_main_thread())
        {
            error = "not on the main thread";
            return false;
        }
        if (!enabled())
        {
            error = "PlusTestScene is off (turn it on in Debug Settings, or start with --set PlusTestScene 1)";
            return false;
        }
        LLViewerRegion* region = gAgent.getRegion();
        if (LLApp::isExiting() || LLStartUp::getStartupState() < STATE_STARTED || !region || !gPipeline.isInit())
        {
            error = "not logged in";
            return false;
        }
        Definition def;
        if (!loadDefinition(name, def, error))
        {
            return false;
        }
        if (overrides.isMap() && overrides.size() > 0)
        {
            if (!applyMap(overrides, def, error))
            {
                error = "override: " + error;
                return false;
            }
        }
        if (!checkTotals(def, error))
        {
            return false;
        }
        LLVector3 middle;
        if (!middleOf(def, region, middle, error))
        {
            return false;
        }

        clear("rebuilt");
        LLTimer timer;
        // Built aside: a build that fails part way lets go of what it made (the destructor).
        auto scene = std::make_unique<Scene>();
        scene->def          = def;
        scene->regionHandle = region->getHandle();
        scene->regionName   = region->getName();
        scene->middle       = middle;
        scene->objects.reserve(def.objectCount());

        Hash hash;
        hash.add(def.seed);
        const Placer placer(scene->def, middle, region->getWidth());
        if (!makeTextures(*scene, hash, error) || !buildPrims(*scene, region, placer, hash, error) ||
            !buildMeshes(*scene, region, placer, hash, error) || !buildSheets(*scene, region, placer, hash, error))
        {
            LL_WARNS("TestScene") << "TESTSCENE failed name=" << def.name << " reason=" << error << LL_ENDL;
            return false;
        }
        scene->hash    = hash.value();
        scene->builtAt = LLFrameTimer::getElapsedSeconds();
        // Leaving the region (a teleport, a crossing) takes the scene with it. The signal also comes for a teleport
        // within the region (LLAgent::setRegion() with the same region): the scene stays then.
        scene->regionChanged = gAgent.addRegionChangedCallback([]()
        {
            const LLViewerRegion* now = gAgent.getRegion();
            if (sScene && (!now || now->getHandle() != sScene->regionHandle))
            {
                clear("region changed");
            }
        });

        const F32 seconds = timer.getElapsedTimeF32();   // a plain number for llformat()
        std::string region_word = scene->regionName;      // one word in the key=value line
        LLStringUtil::replaceChar(region_word, ' ', '_');
        U32 drawables = 0;
        for (const LLPointer<LLViewerObject>& object : scene->objects)
        {
            drawables += object->mDrawable.notNull() ? 1 : 0;
        }
        const std::string line = llformat("name=%s objects=%u drawables=%u faces=%u alpha_faces=%u texanim=%u textures=%u texels=%llu "
                                          "center=%.1f,%.1f,%.1f region=%s hash=%s ms=%.0f ",
                                          def.name.c_str(), (U32)scene->objects.size(), drawables, scene->faces, scene->alphaFaces,
                                          scene->animated, (U32)(scene->textures.size() + scene->alphaTextures.size()),
                                          (unsigned long long)scene->texels, middle.mV[VX], middle.mV[VY], middle.mV[VZ],
                                          region_word.c_str(), hex(scene->hash).c_str(), seconds * 1000.f) +
                                 describe(def);
        sScene = scene.release();
        LL_INFOS("TestScene") << "TESTSCENE built " << line << LL_ENDL;
        if (summary)
        {
            *summary = line;
        }
        return true;
    }

    LLSD stats()
    {
        if (!sScene)
        {
            return LLSD();
        }
        const Scene& scene = *sScene;
        U32 alive = 0, dead = 0, drawables = 0, active = 0, in_volume_octree = 0, elsewhere = 0, not_placed = 0, on_active_list = 0;
        std::set<const LLSpatialGroup*> groups;
        for (const LLPointer<LLViewerObject>& object : scene.objects)
        {
            if (object->isDead())
            {
                ++dead;
                continue;
            }
            ++alive;
            on_active_list += object->onActiveList() ? 1 : 0;
            LLDrawable* drawable = object->mDrawable;
            if (!drawable)
            {
                continue;
            }
            ++drawables;
            active += drawable->isActive() ? 1 : 0;
            LLSpatialGroup* group = drawable->getSpatialGroup();
            if (!group)
            {
                ++not_placed;
                continue;
            }
            groups.insert(group);
            LLSpatialPartition* partition = group->getSpatialPartition();
            if (partition && !partition->isBridge() && partition->mPartitionType == LLViewerRegion::PARTITION_VOLUME)
            {
                ++in_volume_octree;
            }
            else
            {
                ++elsewhere;
            }
        }
        LLSD s;
        s["name"]          = scene.def.name;
        s["hash"]          = hex(scene.hash);
        // @showloc: the region is not named (research/248 3.4)
        s["region"]        = plus::rlv::enabled() ? plus::rlv::regionNameForPlus(scene.regionName) : scene.regionName;
        s["center"]        = ll_sd_from_vector3(scene.middle);
        const F64 now      = LLFrameTimer::getElapsedSeconds();
        s["seconds"]       = now - scene.builtAt;
        s["definition"]    = describe(scene.def);
        s["objects"]       = (S32)scene.objects.size();
        s["alive"]         = (S32)alive;
        s["dead"]          = (S32)dead;      // killed by the viewer (the region went)
        s["drawables"]     = (S32)drawables;
        s["static_in_volume_octree"] = (S32)in_volume_octree;   // as a still object of the simulator's
        s["active_drawables"]        = (S32)active;              // should be 0
        s["in_other_partitions"]     = (S32)elsewhere;           // a bridge or another partition: should be 0
        s["not_in_octree_yet"]       = (S32)not_placed;          // geometry not built yet (the first frames)
        s["on_active_list"]          = (S32)on_active_list;      // should be 0
        s["spatial_groups"] = (S32)groups.size();
        s["faces"]          = (S32)scene.faces;
        s["alpha_faces"]    = (S32)scene.alphaFaces;
        s["texanim"]        = (S32)scene.animated;
        s["textures"]       = (S32)scene.textures.size();
        s["alpha_textures"] = (S32)scene.alphaTextures.size();
        s["texels"]         = (LLSD::Real)scene.texels;
        s["mesh_kinds"]     = (S32)scene.meshKinds.size();
        return s;
    }

    bool lookAt(std::string& error)
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (!sScene)
        {
            error = "no scene is built";
            return false;
        }
        if (!region || region->getHandle() != sScene->regionHandle)
        {
            error = "the scene is in another region";
            return false;
        }
        const Definition& def = sScene->def;
        const LLVector3 offset = def.hasView ? def.view : LLVector3(-def.radius * 1.15f, 0.f, def.vertical * 0.5f + def.radius * 0.3f);
        const LLVector3 camera = clampToRegion(sScene->middle + offset, region->getWidth() + 64.f);
        gAgentCamera.setCameraPosAndFocusGlobal(region->getPosGlobalFromRegion(camera), region->getPosGlobalFromRegion(sScene->middle),
                                                LLUUID::null);
        return true;
    }

    std::string mcpInstructions()
    {
        std::string names;
        for (const std::string& name : sceneNames())
        {
            names += (names.empty() ? "" : ", ") + name;
        }
        return "TEST SCENE (development, for repeatable frame-time measurements; needs the setting PlusTestScene on, which "
               "is not saved): testscene_build makes a made-up load of objects that only this viewer has (prims, meshes, "
               "alpha sheets with texture animations, generated textures) above the avatar, from a scene file (" + names +
               ") with optional overrides of its keys; nothing is sent to the simulator, and the objects cannot be clicked "
               "or selected. The same seed makes the same scene (the hash in the log line \"TESTSCENE built\"). "
               "testscene_stats counts what was built (all should be static in the region's octree); testscene_clear "
               "removes it (so do a teleport, a region crossing and quitting). look=true puts the camera where it sees "
               "the scene.";
    }
}
