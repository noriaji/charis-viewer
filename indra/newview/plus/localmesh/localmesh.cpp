/**
 * @file localmesh.cpp
 * @brief Local mesh: loading the files, the volumes, and showing them on objects.
 *
 * Ported from Firestorm's vjlocalmesh.cpp, Local Mesh by Vaalith Jinn (see ORIGIN.md).
 *
 * $LicenseInfo:firstyear=2022&license=viewerlgpl$
 * Local Mesh contribution source code
 * Copyright (C) 2022, Vaalith Jinn.
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

// Changed from Firestorm's (vjlocalmesh.cpp), besides the readers (see their files):
// - Every LOD's volume is filled before the object is switched, and the volumes are held
//   while the file is loaded. Firestorm switches first and fills after, and lets the lower
//   LODs' volumes go as soon as it has filled them: the object's first look at a lower LOD
//   then asked the mesh repository for the made-up id, and a "not found" from the simulator
//   turned the object into a plain box from the next LOD change on.
// - The volume faces get their extents and go through cacheOptimize() (tangents, vertex
//   cache) as a downloaded mesh's do; normals are made when the file has none.
// - Taking the local mesh off puts back the object's own volume params (not only its mesh
//   id), its scale, and its texture entries.
// - The objects are known by id and held (an LLPointer, so a new object cannot take a bound
//   one's address), never used once dead; mesh skin info is not touched (rigged meshes are
//   not supported yet).
// - A lower LOD file that fails to read is left out, the others still load; a reload that
//   fails keeps the last good version on the objects (as Firestorm's auto reload). Whatever
//   the reading throws (out of memory, a broken file) is a failed load, never a crash.
// - Nothing of the local mesh goes to the simulator: texture entries, PBR and legacy materials
//   and media are not sent while it is shown (the object's faces and their count are the local
//   mesh's), nor a scale given to it (see localmesh.h); taking it off asks for the object
//   again, and for its children when the linked set was scaled meanwhile.
// - The image files a glTF file uses go to Local Textures once, not on every (re)load.
// - Left out: rigged meshes and their joint options, "Rez Selected" (it needs a hook in
//   LLViewerObjectList for new objects), the choice of LOD suffixes (the upload's _LOD0,
//   _LOD1, _LOD2 are used), the scan period (a second).
//
// Settings: PlusLocalMeshAutoReload (bool: reload a file when it changes on disk),
// PlusLocalMeshFileSize (bool: the window's "size of the file" box).

#include "llviewerprecompiledheaders.h"

#include "localmesh.h"
#include "localmeshread.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <map>
#include <memory>
#include <set>

#include <boost/signals2.hpp>

#include "llcallbacklist.h"
#include "lldir.h"
#include "lldrawable.h"
#include "llfile.h"
#include "llframetimer.h"
#include "lllocalbitmaps.h"
#include "llprimitive.h"
#include "llselectmgr.h"
#include "llspatialpartition.h"
#include "llviewercontrol.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvocache.h"
#include "llvolume.h"
#include "llvolumemgr.h"
#include "llvovolume.h"
#include "pipeline.h"

namespace plus::localmesh
{
    namespace
    {
        // ---- meshes ready for the volumes (made on the loader's worker thread) ----

        struct Model
        {
            std::string name;
            LLVector3 size;                                             // in metres, as in the file
            std::array<std::vector<LLVolumeFace>, LOD_COUNT> faces;     // every LOD (a missing one copies a better one);
                                                                        // emptied once in the volumes (makeVolumes())
        };

        struct LoadResult
        {
            bool ok = false;
            std::vector<Model> models;
            std::array<bool, LOD_COUNT> lods{};
            std::vector<std::string> images;    // image files beside a glTF file, for Local Textures
            Log log;
        };

        // A vertex further than this from the file's origin (10,000 km) is taken as broken: the
        // box and the scale made from such numbers are no longer finite in F32.
        constexpr F32 MAX_COORDINATE = 1.e7f;
        constexpr F32 MAX_UV = 1.e6f;   // a texture repeated a million times

        // The high LOD's box: every LOD is centred and scaled by it into the unit cube
        // (as LLModel::normalizeVolumeFaces() for an upload).
        struct Frame
        {
            LLVector3 centre;
            LLVector3 size;         // the box
            LLVector3 divisor;      // the box, with 1 for a flat axis
        };

        Frame frameOf(const RawMesh& mesh)
        {
            LLVector3 low(F32_MAX, F32_MAX, F32_MAX);
            LLVector3 high(-F32_MAX, -F32_MAX, -F32_MAX);
            for (const RawFace& face : mesh.faces)
            {
                for (const LLVector3& p : face.positions)
                {
                    low.set(llmin(low.mV[VX], p.mV[VX]), llmin(low.mV[VY], p.mV[VY]), llmin(low.mV[VZ], p.mV[VZ]));
                    high.set(llmax(high.mV[VX], p.mV[VX]), llmax(high.mV[VY], p.mV[VY]), llmax(high.mV[VZ], p.mV[VZ]));
                }
            }
            Frame frame;
            frame.centre = (low + high) * 0.5f;
            frame.size = high - low;
            for (S32 i = 0; i < 3; ++i)
            {
                frame.divisor.mV[i] = frame.size.mV[i] > F_APPROXIMATELY_ZERO ? frame.size.mV[i] : 1.f;
            }
            return frame;
        }

        // Smooth normals from the triangles (area weighted), for a file that has none.
        std::vector<LLVector3> makeNormals(const RawFace& face)
        {
            std::vector<LLVector3> normals(face.positions.size(), LLVector3::zero);
            for (size_t i = 0; i + 2 < face.indices.size(); i += 3)
            {
                const U32 a = face.indices[i], b = face.indices[i + 1], c = face.indices[i + 2];
                const LLVector3 n = (face.positions[b] - face.positions[a]) % (face.positions[c] - face.positions[a]);
                normals[a] += n;
                normals[b] += n;
                normals[c] += n;
            }
            return normals;
        }

        // One volume face, in the unit cube. False when it could not be allocated.
        bool fillFace(const RawFace& raw, const std::vector<LLVector3>& normals, const Frame& frame, LLVolumeFace& face)
        {
            const S32 vertex_count = (S32)raw.positions.size();
            const S32 index_count = (S32)raw.indices.size();
            face.resizeVertices(vertex_count);
            face.resizeIndices(index_count);
            if (!face.mPositions || !face.mIndices)
            {
                return false;
            }
            // what the upload records as "NormalizedScale": tangents are made in the file's frame
            face.mNormalizedScale = frame.divisor;

            LLVector4a& low = face.mExtents[0];
            LLVector4a& high = face.mExtents[1];
            for (S32 i = 0; i < vertex_count; ++i)
            {
                LLVector3 p = raw.positions[i] - frame.centre;
                p.mV[VX] /= frame.divisor.mV[VX];
                p.mV[VY] /= frame.divisor.mV[VY];
                p.mV[VZ] /= frame.divisor.mV[VZ];
                face.mPositions[i].set(p.mV[VX], p.mV[VY], p.mV[VZ]);
                if (i == 0)
                {
                    low = high = face.mPositions[0];
                }
                else
                {
                    low.setMin(low, face.mPositions[i]);
                    high.setMax(high, face.mPositions[i]);
                }

                // a normal scales the other way round from its position; one that is not a
                // number, too short or too long to normalise stands up (a NaN passes a "< eps")
                LLVector3 n = normals[i];
                n.scaleVec(frame.divisor);
                const F32 length = n.normalize();
                if (!(length >= F_APPROXIMATELY_ZERO) || !std::isfinite(length) || !n.isFinite())
                {
                    n.set(0.f, 0.f, 1.f);
                }
                face.mNormals[i].set(n.mV[VX], n.mV[VY], n.mV[VZ]);

                // a UV that is not a number or absurdly far (the tangents made from it would not be finite) is 0
                const bool good_uv = !raw.uvs.empty() && raw.uvs[i].isFinite()
                                     && fabsf(raw.uvs[i].mV[VX]) <= MAX_UV && fabsf(raw.uvs[i].mV[VY]) <= MAX_UV;
                face.mTexCoords[i] = good_uv ? raw.uvs[i] : LLVector2::zero;
            }
            for (S32 i = 0; i < index_count; ++i)
            {
                face.mIndices[i] = (U16)raw.indices[i];
            }

            LLVector2& tc_low = face.mTexCoordExtents[0];
            LLVector2& tc_high = face.mTexCoordExtents[1];
            tc_low = tc_high = vertex_count ? face.mTexCoords[0] : LLVector2::zero;
            for (S32 i = 1; i < vertex_count; ++i)
            {
                update_min_max(tc_low, tc_high, face.mTexCoords[i]);
            }
            return true;
        }

        // Leaves out the faces that cannot be used as they are, before anything is made of
        // them (the box included). The readers check their files; this is the last word for both.
        void dropBrokenFaces(RawMesh& mesh, Log& log)
        {
            const auto broken = [](const RawFace& raw)
            {
                const auto index_past_end = [&](U32 index) { return index >= raw.positions.size(); };
                const auto bad_position = [](const LLVector3& p)
                {
                    return !p.isFinite() || llmax(fabsf(p.mV[VX]), fabsf(p.mV[VY]), fabsf(p.mV[VZ])) > MAX_COORDINATE;
                };
                return raw.positions.empty() || raw.positions.size() > 65535 || raw.indices.size() % 3 != 0
                    || std::any_of(raw.indices.begin(), raw.indices.end(), index_past_end)
                    || std::any_of(raw.positions.begin(), raw.positions.end(), bad_position)
                    || (!raw.normals.empty() && raw.normals.size() != raw.positions.size())
                    || (!raw.uvs.empty() && raw.uvs.size() != raw.positions.size());
            };
            const size_t before = mesh.faces.size();
            mesh.faces.erase(std::remove_if(mesh.faces.begin(), mesh.faces.end(), broken), mesh.faces.end());
            if (mesh.faces.size() != before)
            {
                log.push_back(std::to_string(before - mesh.faces.size()) + " face(s) of \"" + mesh.name
                              + "\" are malformed (an index outside the vertices, a vertex that is not a number or"
                                " further than 10,000 km...) and were left out.");
            }
        }

        // A lower LOD's faces must lie near the high LOD's box (within its size again on every
        // side, measured as the faces will be divided: a flat axis's divisor is a metre): one far
        // out would be scaled by the high LOD's box into a huge object (a thin high LOD divides
        // by almost nothing).
        void dropFacesOutside(RawMesh& mesh, const Frame& frame, Log& log)
        {
            LLVector3 reach;
            for (S32 i = 0; i < 3; ++i)
            {
                reach.mV[i] = frame.divisor.mV[i] * 1.5f;
            }
            const auto outside = [&](const RawFace& raw)
            {
                return std::any_of(raw.positions.begin(), raw.positions.end(), [&](const LLVector3& p)
                                   {
                                       const LLVector3 d = p - frame.centre;
                                       return !(fabsf(d.mV[VX]) <= reach.mV[VX] && fabsf(d.mV[VY]) <= reach.mV[VY]
                                                && fabsf(d.mV[VZ]) <= reach.mV[VZ]);
                                   });
            };
            const size_t before = mesh.faces.size();
            mesh.faces.erase(std::remove_if(mesh.faces.begin(), mesh.faces.end(), outside), mesh.faces.end());
            if (mesh.faces.size() != before)
            {
                log.push_back(std::to_string(before - mesh.faces.size()) + " face(s) of a lower LOD of \"" + mesh.name
                              + "\" lie far outside the high LOD and were left out.");
            }
        }

        // The volume faces of one LOD of a mesh, optimized as a downloaded mesh's
        // (LLVolume::unpackVolumeFacesInternal()). The faces went through dropBrokenFaces().
        bool makeFaces(const RawMesh& mesh, const Frame& frame, std::vector<LLVolumeFace>& out, Log& log)
        {
            for (const RawFace& raw : mesh.faces)
            {
                const std::vector<LLVector3> normals = raw.normals.empty() ? makeNormals(raw) : raw.normals;

                out.emplace_back();
                if (!fillFace(raw, normals, frame, out.back()))
                {
                    log.push_back("Out of memory for a face of \"" + mesh.name + "\".");
                    out.pop_back();
                    return false;
                }
                if (!out.back().cacheOptimize(true))
                {
                    // shown all the same, unoptimized and without tangents (no normal maps)
                    log.push_back("A face of \"" + mesh.name + "\" could not be optimized (degenerate triangles?).");
                    out.back() = LLVolumeFace();
                    fillFace(raw, normals, frame, out.back());
                }
            }
            return !out.empty();
        }

        bool readFile(const std::string& path, bool preprocess, std::vector<RawMesh>& meshes,
                      std::vector<std::string>& images, Log& log)
        {
            const std::string extension = gDirUtilp->getExtension(path);    // lower case
            if (extension == "dae")
            {
                return readDAE(path, preprocess, meshes, log);
            }
            if (extension == "gltf" || extension == "glb")
            {
                return readGLTF(path, meshes, images, log);
            }
            log.push_back("Not a mesh file this viewer reads (.dae, .gltf, .glb).");
            return false;
        }

        // The worker: reads the LOD files (paths[lod], empty for none) and makes the models.
        // May throw (out of memory): see loadCaught().
        void load(const std::array<std::string, LOD_COUNT>& paths, bool preprocess, LoadResult& result)
        {
            std::array<std::vector<RawMesh>, LOD_COUNT> raw;
            for (S32 lod = LOD_HIGH; lod >= 0; --lod)
            {
                if (paths[lod].empty())
                {
                    continue;
                }
                result.log.push_back("Reading LOD " + std::to_string(lod) + ": " + gDirUtilp->getBaseFileName(paths[lod]));
                if (!readFile(paths[lod], preprocess, raw[lod], result.images, result.log))
                {
                    if (lod == LOD_HIGH)
                    {
                        result.log.push_back("The file could not be read.");
                        return;
                    }
                    result.log.push_back("LOD " + std::to_string(lod) + " left out.");
                    raw[lod].clear();
                    continue;
                }
                for (RawMesh& mesh : raw[lod])
                {
                    dropBrokenFaces(mesh, result.log);
                }
                result.lods[lod] = true;
            }

            const std::vector<RawMesh>& high = raw[LOD_HIGH];
            for (size_t m = 0; m < high.size(); ++m)
            {
                const Frame frame = frameOf(high[m]);
                Model model;
                model.name = high[m].name;
                model.size = frame.size;
                for (S32 lod = LOD_HIGH; lod >= 0; --lod)
                {
                    // a lower LOD's meshes are matched to the high LOD's in file order
                    if (!result.lods[lod] || m >= raw[lod].size())
                    {
                        continue;
                    }
                    if (lod != LOD_HIGH)
                    {
                        dropFacesOutside(raw[lod][m], frame, result.log);
                    }
                    if (!makeFaces(raw[lod][m], frame, model.faces[lod], result.log))
                    {
                        model.faces[lod].clear();
                    }
                    else if (lod != LOD_HIGH && model.faces[lod].size() != model.faces[LOD_HIGH].size())
                    {
                        result.log.push_back("LOD " + std::to_string(lod) + " of \"" + model.name + "\" has "
                                             + std::to_string(model.faces[lod].size()) + " faces, the high LOD "
                                             + std::to_string(model.faces[LOD_HIGH].size())
                                             + ": textures will not match between them.");
                    }
                }
                if (model.faces[LOD_HIGH].empty())
                {
                    result.log.push_back("\"" + model.name + "\" left out: no face could be made.");
                    continue;
                }
                for (S32 lod = LOD_HIGH - 1; lod >= 0; --lod)
                {
                    if (model.faces[lod].empty())
                    {
                        model.faces[lod] = model.faces[lod + 1];
                    }
                }
                result.models.push_back(std::move(model));
            }
            for (S32 lod = 0; lod < LOD_HIGH; ++lod)
            {
                if (result.lods[lod] && raw[lod].size() != high.size())
                {
                    result.log.push_back("LOD " + std::to_string(lod) + " has " + std::to_string(raw[lod].size())
                                         + " meshes, the high LOD " + std::to_string(high.size()) + ".");
                }
            }

            result.ok = !result.models.empty();
            result.log.push_back(result.ok ? "Loaded: " + std::to_string(result.models.size()) + " mesh(es)."
                                           : "Nothing could be loaded.");
        }

        // A broken file can say the same thing thousands of times; the main thread logs and
        // shows every line. The first lines and the last (the outcome) are kept.
        void trimLog(Log& log)
        {
            constexpr size_t HEAD = 200;
            constexpr size_t TAIL = 20;
            if (log.size() > HEAD + TAIL + 1)
            {
                const size_t dropped = log.size() - HEAD - TAIL;
                log.erase(log.begin() + HEAD, log.end() - TAIL);
                log.insert(log.begin() + HEAD, "... " + std::to_string(dropped) + " more lines ...");
            }
        }

        // load(), with what it throws turned into a failed load (a broken or huge file must not
        // reach the main thread's future.get()). The log keeps what was said before.
        LoadResult loadCaught(const std::array<std::string, LOD_COUNT>& paths, bool preprocess)
        {
            LoadResult result;
            const char* failure = nullptr;
            std::string detail;
            try
            {
                load(paths, preprocess, result);
                trimLog(result.log);
                return result;
            }
            catch (const std::bad_alloc&)
            {
                failure = "Out of memory while reading the file (is it too large?).";
            }
            catch (const std::exception& e)
            {
                failure = "Reading the file failed: ";
                detail = e.what();
            }
            catch (...)
            {
                failure = "Reading the file failed (unknown error).";
            }
            result.ok = false;
            std::vector<Model>().swap(result.models);   // gives the memory back first
            result.lods = {};
            result.images.clear();
            try
            {
                result.log.push_back(failure + detail);
                trimLog(result.log);
            }
            catch (...)
            {
            }
            return result;
        }

        // ---- files, volumes and objects (main thread) ----

        // A model's volumes in the volume manager, one per LOD, each holding a reference.
        struct Prepared
        {
            LLVolumeParams params;
            std::array<LLVolume*, LOD_COUNT> volumes{};
        };

        // When a file changed on disk (size and time of each LOD file).
        struct Stamp
        {
            std::array<S64, LOD_COUNT> times{};
            std::array<S64, LOD_COUNT> sizes{};
            bool operator==(const Stamp&) const = default;
        };

        struct File
        {
            LLUUID id;
            std::string path;                           // the high LOD file
            Status status = Status::Loading;
            Log log;
            std::array<std::string, LOD_COUNT> paths;   // as last read
            Stamp stamp;                                // as last read
            std::array<bool, LOD_COUNT> lods{};
            std::vector<Model> models;
            std::vector<Prepared> prepared;             // by model, made the first time one is shown
            std::future<LoadResult> loading;
            std::set<std::string> images;               // image files offered to Local Textures (once each; lower case)
        };

        // An object showing a local mesh, and what to put back.
        struct Binding
        {
            // Held, so that its memory is not given to a new object while it is bound: an
            // object of the same id made again (out of the interest list and back) at the same
            // address would otherwise pass for it, and its updates be held back. Dead objects
            // are let go within a second (onIdle()).
            LLPointer<LLViewerObject> object;
            LLUUID fileID;
            std::string meshName;                       // finds the mesh again after a reload
            S32 meshIndex = 0;
            bool fileSize = false;
            LLVolumeParams originalParams;
            LLVolume* originalVolume = nullptr;         // a reference, so taking off finds a loaded LOD
            LLVector3 originalScale;
            std::vector<LLTextureEntry> originalTEs;
            // The linked set was scaled while it showed the local mesh: the scale went nowhere
            // (simulatorUpdateType()), but the children moved and grew here. Taking off puts
            // them back as the simulator has them.
            bool childrenScaled = false;
        };

        struct State
        {
            std::vector<std::unique_ptr<File>> files;
            std::map<LLUUID, Binding> bindings;         // by object id
            std::map<std::string, LLUUID> images;       // image file (lower case) -> its Local Textures tracking id (added here)
            boost::signals2::signal<void()> changed;
            LLFrameTimer checkTimer;
            bool idle = false;
        };

        // Never destroyed: at exit the volume manager and the objects are gone before static
        // destructors run, and there is nothing to give back then.
        State& state()
        {
            static State* s = new State;
            return *s;
        }

        void notify()
        {
            state().changed();
        }

        File* findFile(const LLUUID& id)
        {
            for (auto& file : state().files)
            {
                if (file->id == id)
                {
                    return file.get();
                }
            }
            return nullptr;
        }

        LLVOVolume* liveObject(const LLUUID& id, const Binding& binding)
        {
            LLViewerObject* object = gObjectList.findObject(id);
            return (object && object == binding.object.get() && !object->isDead()) ? (LLVOVolume*)object : nullptr;
        }

        // ---- loading ----

        std::array<std::string, LOD_COUNT> lodPaths(const std::string& high)
        {
            static const char* SUFFIXES[LOD_HIGH] = { "_LOD0", "_LOD1", "_LOD2" };   // as the upload's
            std::array<std::string, LOD_COUNT> paths;
            paths[LOD_HIGH] = high;
            const std::string dir = gDirUtilp->getDirName(high);
            const std::string stem = gDirUtilp->getBaseFileName(high, true);
            const size_t dot = high.rfind('.');
            const std::string extension = dot == std::string::npos ? std::string() : high.substr(dot);
            for (S32 lod = 0; lod < LOD_HIGH; ++lod)
            {
                const std::string path = dir + gDirUtilp->getDirDelimiter() + stem + SUFFIXES[lod] + extension;
                if (LLFile::isfile(path))
                {
                    paths[lod] = path;
                }
            }
            return paths;
        }

        Stamp stampOf(const std::array<std::string, LOD_COUNT>& paths)
        {
            Stamp stamp;
            for (S32 lod = 0; lod < LOD_COUNT; ++lod)
            {
                llstat st;
                if (!paths[lod].empty() && LLFile::stat(paths[lod], &st) == 0)
                {
                    stamp.times[lod] = (S64)st.st_mtime;
                    stamp.sizes[lod] = (S64)st.st_size;
                }
            }
            return stamp;
        }

        void startLoad(File& file)
        {
            file.status = Status::Loading;
            file.log.clear();
            file.paths = lodPaths(file.path);
            file.stamp = stampOf(file.paths);   // set now: a file that keeps failing is not read again until it changes
            const bool preprocess = gSavedSettings.getBOOL("ImporterPreprocessDAE");
            try
            {
                file.loading = std::async(std::launch::async, [paths = file.paths, preprocess]()
                                          {
                                              return loadCaught(paths, preprocess);
                                          });
            }
            catch (const std::exception& e)     // no thread could be started
            {
                LL_WARNS("LocalMesh") << "Could not start loading " << file.path << ": " << e.what() << LL_ENDL;
                file.log.push_back("Could not start reading the file.");
                file.status = file.models.empty() ? Status::Error : Status::Active;
            }
        }

        void makeVolumes(File& file, size_t index)
        {
            if (file.prepared.size() != file.models.size())
            {
                file.prepared.resize(file.models.size());
            }
            Prepared& prepared = file.prepared[index];
            if (prepared.volumes[0])
            {
                return;
            }

            // the volume params of an uploaded mesh (LLMeshUploadThread), with an id of our own
            LLUUID mesh_id;
            mesh_id.generate();
            prepared.params.setType(LL_PCODE_PROFILE_SQUARE, LL_PCODE_PATH_LINE);
            prepared.params.setBeginAndEndS(0.f, 1.f);
            prepared.params.setBeginAndEndT(0.f, 1.f);
            prepared.params.setRatio(1.f, 1.f);
            prepared.params.setShear(0.f, 0.f);
            prepared.params.setSculptID(mesh_id, LL_SCULPT_TYPE_MESH);

            Model& model = file.models[index];
            for (S32 lod = 0; lod < LOD_COUNT; ++lod)
            {
                LLVolume* volume = LLPrimitive::getVolumeManager()->refVolume(prepared.params, lod);
                volume->copyFacesFrom(model.faces[lod]);
                volume->setMeshAssetLoaded(true);
                prepared.volumes[lod] = volume;
                // the volumes are held until the file is reloaded or removed: the faces are not needed twice
                std::vector<LLVolumeFace>().swap(model.faces[lod]);
            }
        }

        void releaseVolumes(std::vector<Prepared>& prepared)
        {
            for (Prepared& p : prepared)
            {
                for (LLVolume*& volume : p.volumes)
                {
                    if (volume)
                    {
                        LLPrimitive::getVolumeManager()->unrefVolume(volume);
                        volume = nullptr;
                    }
                }
            }
            prepared.clear();
        }

        // ---- objects ----

        // As a mesh that has just been downloaded: the faces and texture entries follow the
        // volume's face count (lodOrSculptChanged()), and the group's old batches are not kept
        // for a frame or two while the rebuild waits its turn (may_wait_for_rebuild()).
        void rebuild(LLVOVolume& object)
        {
            object.notifyMeshLoaded();
            object.markForUpdate();
            if (LLDrawable* drawable = object.mDrawable)
            {
                gPipeline.markRebuild(drawable, LLDrawable::REBUILD_ALL);
                if (LLSpatialGroup* group = drawable->getSpatialGroup())
                {
                    group->mRHIRebuildNow = true;
                }
            }
        }

        LLVector3 fileScale(const Model& model)
        {
            LLVector3 scale = model.size;
            for (S32 i = 0; i < 3; ++i)
            {
                scale.mV[i] = llclamp(scale.mV[i], 0.01f, 64.f);    // the limits of an object's size
            }
            return scale;
        }

        void apply(LLVOVolume& object, File& file, S32 index, const Binding& binding)
        {
            makeVolumes(file, index);
            // the object is not rigged: no skin to look for under the made-up id
            object.notifySkinInfoUnavailable();
            object.setVolume(file.prepared[index].params, LOD_HIGH);
            if (!object.isAttachment())
            {
                object.setScale(binding.fileSize ? fileScale(file.models[index]) : binding.originalScale, false);
            }
            rebuild(object);
        }

        // Puts the object as the simulator has it. An object the cache keeps has the latest
        // full update there (the cache takes it before the object is asked; asking the
        // simulator again would bring the same, and be dropped as a duplicate): apply that.
        // Any other is asked for again.
        void askSimulatorState(LLViewerObject& object)
        {
            if (LLViewerRegion* region = object.getRegion())
            {
                LLVOCacheEntry* entry = region->getCacheEntry(object.getLocalID());
                if (entry && entry->getDP())
                {
                    gObjectList.processObjectUpdateFromCache(entry, region);
                }
                else
                {
                    region->addCacheMissFull(object.getLocalID());
                }
            }
        }

        void restore(LLVOVolume& object, Binding& binding)
        {
            object.setVolume(binding.originalParams, LOD_HIGH);
            if (!object.isAttachment())
            {
                object.setScale(binding.originalScale, false);
            }
            // every texture entry as it was: the faces the local mesh did not have lost theirs,
            // and what was changed on the others while it was shown never went to the simulator
            // (sendTEUpdate() is held back, see holdsUpdates())
            const S32 original_tes = (S32)binding.originalTEs.size();
            if (original_tes > 0)
            {
                object.setNumTEs((U8)original_tes);
                for (S32 te = 0; te < original_tes; ++te)
                {
                    object.setTE((U8)te, binding.originalTEs[te]);
                }
            }
            rebuild(object);
            // What the simulator said of it meanwhile was held back.
            askSimulatorState(object);
            // The children a linked set scale moved here only (not the avatars sitting on it).
            if (binding.childrenScaled)
            {
                // a copy: an update from the cache may link or unlink
                const LLViewerObject::child_list_t children = object.getChildren();
                for (const LLPointer<LLViewerObject>& child : children)
                {
                    LLViewerObject* object_child = child.get();
                    if (object_child && !object_child->isDead() && !object_child->isAvatar())
                    {
                        askSimulatorState(*object_child);
                    }
                }
            }
        }

        void forget(Binding& binding)
        {
            if (binding.originalVolume)
            {
                LLPrimitive::getVolumeManager()->unrefVolume(binding.originalVolume);
                binding.originalVolume = nullptr;
            }
        }

        // Takes the local meshes off the objects for which keep() is false.
        template <typename Keep>
        void takeOffWhere(Keep keep)
        {
            auto& bindings = state().bindings;
            for (auto it = bindings.begin(); it != bindings.end();)
            {
                if (keep(it->first, it->second))
                {
                    ++it;
                    continue;
                }
                if (LLVOVolume* object = liveObject(it->first, it->second))
                {
                    Binding binding = it->second;
                    it = bindings.erase(it);    // first: the object takes updates again
                    restore(*object, binding);
                    forget(binding);
                }
                else
                {
                    forget(it->second);
                    it = bindings.erase(it);
                }
            }
        }

        // The mesh a binding showed, in a reloaded file: by name if one mesh has it, else
        // where it was.
        S32 findMesh(const File& file, const Binding& binding)
        {
            S32 found = -1;
            for (size_t i = 0; i < file.models.size(); ++i)
            {
                if (file.models[i].name == binding.meshName)
                {
                    if (found >= 0)
                    {
                        found = -1;     // two of that name: by place
                        break;
                    }
                    found = (S32)i;
                }
            }
            if (found < 0 && binding.meshIndex < (S32)file.models.size())
            {
                found = binding.meshIndex;
            }
            return found;
        }

        // The image files a glTF file uses, in Local Textures (as the official glTF code's local
        // preview does), each once: that code added them again on every load, so an automatic
        // reload added copies without end and decoded them on the main thread each time. A file
        // offered once is not offered again for that list entry (taken out of Local Textures,
        // it stays out), nor for another entry while it is still there.
        void addImages(File& file, const std::vector<std::string>& images)
        {
            LLLocalBitmapMgr* bitmaps = LLLocalBitmapMgr::getInstance();
            for (const std::string& path : images)
            {
                const std::string key = utf8str_tolower(path);     // the reader tidied the path; Windows ignores case
                if (!file.images.insert(key).second)
                {
                    continue;
                }
                auto& added = state().images;
                auto it = added.find(key);
                if (it != added.end() && !bitmaps->getFilename(it->second).empty())
                {
                    continue;
                }
                const LLUUID tracking_id = bitmaps->addUnit(path);
                if (tracking_id.notNull())
                {
                    added[key] = tracking_id;
                }
            }
        }

        void finishLoad(File& file)
        {
            LoadResult result;
            try
            {
                result = file.loading.get();
            }
            catch (const std::exception& e)     // loadCaught() catches what the readers throw: a last resort
            {
                result = LoadResult();
                result.log.push_back(std::string("Reading the file failed: ") + e.what());
            }
            catch (...)
            {
                result = LoadResult();
                result.log.push_back("Reading the file failed (unknown error).");
            }
            file.log.insert(file.log.end(), result.log.begin(), result.log.end());
            for (const std::string& line : result.log)
            {
                LL_INFOS("LocalMesh") << gDirUtilp->getBaseFileName(file.path) << ": " << line << LL_ENDL;
            }

            if (!result.ok)
            {
                if (!file.models.empty())
                {
                    file.log.push_back("Kept the last version that loaded.");
                    file.status = Status::Active;
                }
                else
                {
                    file.status = Status::Error;
                }
                notify();
                return;
            }

            std::vector<Prepared> old_volumes = std::move(file.prepared);
            file.prepared.clear();
            file.models = std::move(result.models);
            file.lods = result.lods;
            file.status = Status::Active;
            addImages(file, result.images);

            // the objects showing the file get the new version (then the old volumes go)
            std::vector<LLUUID> lost;
            for (auto& [id, binding] : state().bindings)
            {
                if (binding.fileID != file.id)
                {
                    continue;
                }
                LLVOVolume* object = liveObject(id, binding);
                if (!object)
                {
                    continue;   // gone: swept by onIdle()
                }
                const S32 index = findMesh(file, binding);
                if (index < 0)
                {
                    lost.push_back(id);
                    continue;
                }
                binding.meshIndex = index;
                binding.meshName = file.models[index].name;
                apply(*object, file, index, binding);
            }
            if (!lost.empty())
            {
                file.log.push_back("A mesh shown on an object is not in the file any more: taken off.");
                takeOffWhere([&](const LLUUID& id, const Binding&)
                             {
                                 return std::find(lost.begin(), lost.end(), id) == lost.end();
                             });
            }
            releaseVolumes(old_volumes);
            notify();
        }

        void onIdle(void*)
        {
            State& s = state();
            for (auto& file : s.files)
            {
                if (file->loading.valid() && file->loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                {
                    finishLoad(*file);
                }
            }

            if (s.checkTimer.getElapsedTimeF32() < 1.f)
            {
                return;
            }
            s.checkTimer.reset();

            // objects that are gone (killed, out of the region, logged out)
            const size_t before = s.bindings.size();
            for (auto it = s.bindings.begin(); it != s.bindings.end();)
            {
                if (liveObject(it->first, it->second))
                {
                    ++it;
                }
                else
                {
                    forget(it->second);
                    it = s.bindings.erase(it);
                }
            }
            if (s.bindings.size() != before)
            {
                notify();
            }

            static LLCachedControl<bool> auto_reload(gSavedSettings, "PlusLocalMeshAutoReload", false);
            if (auto_reload)
            {
                for (auto& file : s.files)
                {
                    if (!file->loading.valid() && !(stampOf(lodPaths(file->path)) == file->stamp))
                    {
                        startLoad(*file);
                        notify();
                    }
                }
            }
        }

        void updateIdle()
        {
            State& s = state();
            const bool wanted = !s.files.empty() || !s.bindings.empty();
            if (wanted != s.idle)
            {
                s.idle = wanted;
                if (wanted)
                {
                    gIdleCallbacks.addFunction(onIdle, nullptr);
                }
                else
                {
                    gIdleCallbacks.deleteFunction(onIdle, nullptr);
                }
            }
        }
    }

    void addFile(const std::string& path)
    {
        auto file = std::make_unique<File>();
        file->id.generate();
        file->path = path;
        startLoad(*file);
        state().files.push_back(std::move(file));
        updateIdle();
        notify();
    }

    void removeFile(const LLUUID& file_id)
    {
        auto& files = state().files;
        auto it = std::find_if(files.begin(), files.end(), [&](const auto& file) { return file->id == file_id; });
        if (it == files.end() || (*it)->loading.valid())
        {
            return;     // a loading file waits (the window does not offer it)
        }
        takeOffWhere([&](const LLUUID&, const Binding& binding) { return binding.fileID != file_id; });
        releaseVolumes((*it)->prepared);
        files.erase(it);
        updateIdle();
        notify();
    }

    void reloadFile(const LLUUID& file_id)
    {
        File* file = findFile(file_id);
        if (file && !file->loading.valid())
        {
            startLoad(*file);
            notify();
        }
    }

    std::vector<FileInfo> files()
    {
        std::vector<FileInfo> out;
        for (const auto& file : state().files)
        {
            FileInfo info;
            info.id = file->id;
            info.name = gDirUtilp->getBaseFileName(file->path);
            info.status = file->status;
            info.lods = file->lods;
            for (const Model& model : file->models)
            {
                info.meshes.push_back(model.name);
            }
            for (const auto& [id, binding] : state().bindings)
            {
                info.shownOn += binding.fileID == file->id ? 1 : 0;
            }
            out.push_back(std::move(info));
        }
        return out;
    }

    std::vector<std::string> fileLog(const LLUUID& file_id)
    {
        const File* file = findFile(file_id);
        return file ? file->log : std::vector<std::string>();
    }

    bool canShowOn(const LLViewerObject* object, std::string* why_not)
    {
        auto refuse = [&](const char* reason)
        {
            if (why_not)
            {
                *why_not = reason;
            }
            return false;
        };
        if (!object || object->isDead() || object->getPCode() != LL_PCODE_VOLUME)
        {
            return refuse("NoObject");
        }
        if (!object->isMesh())
        {
            return refuse("NotMesh");
        }
        if (object->isRiggedMesh())
        {
            return refuse("Rigged");
        }
        if (!isShownOn(object) && (!object->getVolume() || !object->getVolume()->isMeshAssetLoaded()))
        {
            return refuse("NotLoaded");
        }
        return true;
    }

    bool show(const LLUUID& file_id, S32 mesh_index, LLViewerObject* object, bool file_size)
    {
        File* file = findFile(file_id);
        if (!file || file->status != Status::Active || file->loading.valid() || mesh_index < 0
            || mesh_index >= (S32)file->models.size() || !canShowOn(object))
        {
            return false;
        }
        LLVOVolume* volume_object = (LLVOVolume*)object;

        auto& bindings = state().bindings;
        auto it = bindings.find(object->getID());
        if (it != bindings.end() && it->second.object.get() != object)
        {
            forget(it->second);     // a dead object's, not yet swept
            bindings.erase(it);
            it = bindings.end();
        }
        if (it == bindings.end())
        {
            // what to put back when it is taken off
            Binding binding;
            binding.object = object;
            binding.originalParams = object->getVolume()->getParams();
            const S32 detail = LLVolumeLODGroup::getVolumeDetailFromScale(object->getVolume()->getDetail());
            binding.originalVolume = LLPrimitive::getVolumeManager()->refVolume(binding.originalParams, detail);
            binding.originalScale = object->getScale();
            for (U8 te = 0; te < object->getNumTEs(); ++te)
            {
                binding.originalTEs.push_back(*object->getTE(te));
            }
            it = bindings.emplace(object->getID(), std::move(binding)).first;
        }

        Binding& binding = it->second;
        binding.fileID = file_id;
        binding.meshIndex = mesh_index;
        binding.meshName = file->models[mesh_index].name;
        binding.fileSize = file_size;
        apply(*volume_object, *file, mesh_index, binding);
        updateIdle();
        notify();
        return true;
    }

    void takeOff(LLViewerObject* object)
    {
        if (!isShownOn(object))
        {
            return;
        }
        const LLUUID id = object->getID();
        takeOffWhere([&](const LLUUID& binding_id, const Binding&) { return binding_id != id; });
        updateIdle();
        notify();
    }

    bool isShownOn(const LLViewerObject* object)
    {
        return holdsUpdates(object);
    }

    boost::signals2::connection setChangedCallback(const std::function<void()>& callback)
    {
        return state().changed.connect(callback);
    }

    bool holdsUpdates(const LLViewerObject* object)
    {
        const auto& bindings = state().bindings;
        if (bindings.empty() || !object)
        {
            return false;
        }
        auto it = bindings.find(object->getID());
        return it != bindings.end() && it->second.object.get() == object;
    }

    U8 simulatorUpdateType(const LLViewerObject* object, U8 type)
    {
        // a scale is sent with the position it moved the centre to (llmanipscale.cpp): both stay
        if ((type & UPD_SCALE) && holdsUpdates(object))
        {
            type &= ~(UPD_SCALE | UPD_POSITION);
            // the whole linked set (the root's update with UPD_LINKED_SETS, which the simulator
            // would apply to the children): they moved here only, and go back on taking off
            if ((type & UPD_LINKED_SETS) && !object->getChildren().empty())
            {
                state().bindings[object->getID()].childrenScaled = true;
            }
        }
        return type;
    }
}
