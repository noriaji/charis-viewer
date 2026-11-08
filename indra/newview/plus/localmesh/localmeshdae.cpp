/**
 * @file localmeshdae.cpp
 * @brief Local mesh: the COLLADA (.dae) reader.
 *
 * Ported from Firestorm's vjlocalmeshimportdae.cpp (see ORIGIN.md).
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

// Ported from Firestorm's Local Mesh (vjlocalmeshimportdae.cpp, Vaalith Jinn, 2022; LGPL 2.1),
// itself a refactor of the official LLDAELoader. Kept: every <mesh> of the file is one mesh
// of the list, its <triangles> and <polylist> are its faces (at most 8), vertices with the
// same position, normal and UV are shared. Node transforms are not applied (as Firestorm).
// Changed: the file's unit and up axis are applied as an upload does (Firestorm reads the
// raw numbers, so a Y-up file lies on its side); the path is escaped for the COLLADA DOM as
// LLDAELoader::OpenFile() does (Firestorm's could not open a path with Japanese in it);
// indices out of the file's arrays are refused instead of read past the end; shared
// vertices are found with a map, not a linear search (a big face took minutes); triangles
// with a repeated corner are dropped; an <input> offset is checked as the file has it (64
// bits, at most 63), so a corner always moves the index lists on; a position that is not
// finite refuses the face (a normal or UV that is not finite becomes 0), the unit must be a
// finite number above 0; the file (256 MB), its meshes, vertices and corners are bounded
// (localmeshread.h); the upload's preprocessing (spaces out of ids) is done here in one pass,
// not by LLDAELoader::preprocessDAE(), which takes time growing with the square of the file;
// the text is read through with libxml2 before the DOM gets it, and refused unless it is
// well-formed (the DOM's reader loops for ever on some broken files, holding the DOM's lock);
// a missing semantic attribute (the DOM hands back NULL) is an empty one.
// Left out: skins (rigged meshes show static).

#include "llviewerprecompiledheaders.h"

#include "localmeshread.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <map>
#include <mutex>

#include "llfile.h"
#include "lluri.h"
#include "llvolume.h"

#include <dae.h>
#include <dom/domAsset.h>
#include <dom/domCOLLADA.h>
#include <dom/domConstants.h>
#include <dom/domGeometry.h>
#include <dom/domMesh.h>

#ifndef LIBXML_STATIC
#define LIBXML_STATIC           // the prebuilt libxml2 is a static library (as the COLLADA DOM links it)
#endif
#include <libxml/xmlreader.h>   // plus/localmesh/localmesh.cmake adds its folder

namespace plus::localmesh
{
    namespace
    {
        // The DOM is not known to be safe with two documents open on two threads; local mesh
        // loads are serialised (each is a worker of its own). Held by a lock_guard: an
        // exception (out of memory in the DOM) lets it go.
        std::mutex sDAEMutex;

        constexpr U32 MAX_FACE_VERTICES = 65535;    // indices are 16 bits in a volume face
        constexpr U64 MAX_INPUT_OFFSET = 63;        // of an <input> in a corner's indices (a file has a handful)

        constexpr U64 MAX_FILE_BYTES = 256ull << 20;  // the DOM takes several times the file's size

        // The file's size, or -1.
        S64 fileSize(const std::string& path)
        {
            llstat st;
            return LLFile::stat(path, &st) == 0 ? (S64)st.st_size : -1;
        }

        bool idChar(char c)
        {
            return std::isalnum((unsigned char)c) || c == '_' || c == '.' || c == '@' || c == '#' || c == '$' || c == '-';
        }

        bool spaceChar(char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        }

        // The upload's preprocessing (LLDAELoader::preprocessDAE(), MAINT-5678): in a quoted
        // string made of id characters and white space, with some white space (an id or name
        // with spaces), the spaces become '_'. In one pass: that one replaced each such string
        // all over the file and logged two lines for each, so a file of a million of them kept
        // the reader (and the DOM's lock) for hours.
        void preprocessDAE(std::string& text)
        {
            size_t i = 0;
            while ((i = text.find('"', i)) != std::string::npos)
            {
                size_t j = i + 1;
                bool spaced = false;
                while (j < text.size() && (idChar(text[j]) || spaceChar(text[j])))
                {
                    spaced = spaced || spaceChar(text[j]);
                    ++j;
                }
                if (j < text.size() && text[j] == '"' && spaced)
                {
                    std::replace(text.begin() + i + 1, text.begin() + j, ' ', '_');
                    i = j + 1;
                }
                else
                {
                    i = j;  // a quote that closed nothing may open the next string, as the upload's regex finds it
                }
            }
        }

        // An attribute the DOM hands back as a C string: NULL when the file leaves it out.
        std::string orEmpty(const char* value)
        {
            return value ? std::string(value) : std::string();
        }

        // The line of libxml2's first error (the log says it once, not each of a flood).
        void firstXMLErrorLine(void* line, const xmlError* error)
        {
            int& first = *static_cast<int*>(line);
            if (first == 0 && error && error->level >= XML_ERR_ERROR)  // not a warning
            {
                first = error->line > 0 ? error->line : -1;
            }
        }

        // The text read to the end by libxml2's reader, with no network and no entity
        // substituted (XML_PARSE_NOENT left out, no DTD loaded): false at its first error. The
        // DOM's own reader (daeLIBXMLPlugin::readElement()) goes round for ever on some files
        // that are cut short or whose tags do not match, so it only gets what libxml2 read
        // through. libxml2's default depth limit (256) also refuses a file nested too deep.
        bool wellFormedXML(const std::string& text, std::string& why)
        {
            if (text.size() > (size_t)INT_MAX)
            {
                why = "too large";
                return false;
            }
            xmlTextReaderPtr reader = xmlReaderForMemory(text.data(), (int)text.size(), nullptr, nullptr, XML_PARSE_NONET);
            if (!reader)
            {
                why = "out of memory";
                return false;
            }
            int error_line = 0;
            xmlTextReaderSetStructuredErrorHandler(reader, firstXMLErrorLine, &error_line);
            int ret = 1;
            while (ret == 1)
            {
                ret = xmlTextReaderRead(reader);
            }
            if (ret != 0)
            {
                why = error_line > 0 ? "line " + std::to_string(error_line) : "at the end";
            }
            xmlFreeTextReader(reader);
            return ret == 0;
        }

        bool readText(const std::string& path, std::string& text)
        {
            llifstream file(path.c_str(), std::ios::binary);
            if (!file.is_open())
            {
                return false;
            }
            file.seekg(0, std::ios::end);
            const std::streamoff size = file.tellg();
            if (size < 0 || (U64)size > MAX_FILE_BYTES)
            {
                return false;
            }
            file.seekg(0, std::ios::beg);
            text.resize((size_t)size);
            return size == 0 || (bool)file.read(text.data(), size);
        }

        struct Axes
        {
            domUpAxisType up = UPAXISTYPE_Y_UP;     // COLLADA's default
            F32 metres = 1.f;

            // COLLADA's axes to the viewer's (Z up), as LLDAELoader's rotation does.
            LLVector3 point(F32 x, F32 y, F32 z) const { return turn(x, y, z) * metres; }
            LLVector3 turn(F32 x, F32 y, F32 z) const
            {
                switch (up)
                {
                case UPAXISTYPE_X_UP: return LLVector3(-y, -z, x);
                case UPAXISTYPE_Z_UP: return LLVector3(x, y, z);
                default:              return LLVector3(x, -z, y);
                }
            }
        };

        // One <input> source: its float array and how many floats a value has.
        struct Source
        {
            const domListOfFloats* values = nullptr;
            U32 offset = 0;                         // in the <p> index groups (at most MAX_INPUT_OFFSET)
            U32 width = 0;

            bool valid() const { return values != nullptr; }
            // no product that could overflow: the index comes from the file
            bool has(U64 index) const { return width != 0 && index < values->getCount() / width; }
            F32 at(U64 index, U32 component) const { return (F32)(*values)[(size_t)(index * width + component)]; }
        };

        // The element a source URI names, in this file only. The DOM opens the document of a
        // URI that names another one (daeURIResolver loads external documents unless told not
        // to), so "file://server/share/x.dae#id" would reach out to a share as the file is read
        // (and hand the share the user's Windows sign-in). Such a URI is left unresolved.
        daeElement* localElement(const daeURI& uri)
        {
            const daeElement* container = uri.getContainer();
            daeDocument* own = container ? container->getDocument() : nullptr;
            // the database's own lookup, which never opens a document: the scheme, server,
            // folder and file name must all be this file's (isExternalReference() looks at the
            // folder and file name only)
            if (!own || uri.getReferencedDocument() != own)
            {
                return nullptr;
            }
            return uri.getElement();
        }

        Source sourceOf(daeElement* element, U32 offset, U32 width)
        {
            Source source;
            domSource* dom_source = daeSafeCast<domSource>(element);
            if (dom_source && dom_source->getFloat_array())
            {
                source.values = &dom_source->getFloat_array()->getValue();
                source.offset = offset;
                source.width = width;
            }
            return source;
        }

        // The inputs of a <triangles> or <polylist>: where position, normal and UV are, and
        // how many indices one corner has in <p>.
        struct Inputs
        {
            Source position;
            Source normal;
            Source uv;
            U32 stride = 0;
        };

        bool readInputs(const domInputLocalOffset_Array& inputs, Inputs& out, Log& log)
        {
            U32 max_offset = 0;
            for (size_t i = 0; i < inputs.getCount(); ++i)
            {
                const domInputLocalOffset* input = inputs[i];
                // checked as the file has it (64 bits): a cut to 32 bits made 4294967295 a -1, and
                // the corner's size 0 (the face's loop then never moved on)
                const U64 file_offset = input->getOffset();
                if (file_offset > MAX_INPUT_OFFSET)
                {
                    log.push_back("A face has an <input> offset of " + std::to_string(file_offset) + "; left out.");
                    return false;
                }
                const U32 offset = (U32)file_offset;
                max_offset = llmax(max_offset, offset);
                const std::string semantic = orEmpty(input->getSemantic());
                daeElement* element = localElement(input->getSource());

                if (semantic == COMMON_PROFILE_INPUT_VERTEX)
                {
                    domVertices* vertices = daeSafeCast<domVertices>(element);
                    if (!vertices)
                    {
                        log.push_back("The <vertices> of a face are missing.");
                        return false;
                    }
                    const domInputLocal_Array& vertex_inputs = vertices->getInput_array();
                    for (size_t j = 0; j < vertex_inputs.getCount(); ++j)
                    {
                        const std::string vertex_semantic = orEmpty(vertex_inputs[j]->getSemantic());
                        daeElement* vertex_element = localElement(vertex_inputs[j]->getSource());
                        if (vertex_semantic == COMMON_PROFILE_INPUT_POSITION)
                        {
                            out.position = sourceOf(vertex_element, offset, 3);
                        }
                        else if (vertex_semantic == COMMON_PROFILE_INPUT_NORMAL)
                        {
                            out.normal = sourceOf(vertex_element, offset, 3);
                        }
                        else if (vertex_semantic == COMMON_PROFILE_INPUT_TEXCOORD && !out.uv.valid())
                        {
                            out.uv = sourceOf(vertex_element, offset, 2);
                        }
                    }
                }
                else if (semantic == COMMON_PROFILE_INPUT_NORMAL)
                {
                    out.normal = sourceOf(element, offset, 3);
                }
                else if (semantic == COMMON_PROFILE_INPUT_TEXCOORD && !out.uv.valid())
                {
                    // the first UV set only, as an upload
                    out.uv = sourceOf(element, offset, 2);
                }
            }
            out.stride = max_offset + 1;    // 1 to 64: every corner moves the index lists on

            if (!out.position.valid() || out.position.values->getCount() == 0)
            {
                log.push_back("A face has no vertex positions.");
                return false;
            }
            return true;
        }

        // What is left of a file's allowance (MAX_FILE_VERTICES, MAX_FILE_CORNERS).
        struct Budget
        {
            size_t vertices = MAX_FILE_VERTICES;
            size_t corners = MAX_FILE_CORNERS;
            bool spent = false;     // a face was refused for it: the rest of the file is not read
        };

        // Builds one face corner by corner, sharing a vertex wherever position, normal and UV
        // are all the same.
        class FaceBuilder
        {
        public:
            FaceBuilder(RawFace& face, const Inputs& inputs, const Axes& axes, Budget& budget)
                : mFace(face), mInputs(inputs), mAxes(axes), mBudget(budget)
            {
            }

            // The vertex of the corner whose indices start at p[first]; -1 if an index is
            // outside its array or the face is full.
            S32 corner(const domListOfUInts& p, size_t first)
            {
                if (first + mInputs.stride > p.getCount())
                {
                    mError = "a face's index list ends in the middle of a corner";
                    return -1;
                }
                const U64 pi = p[first + mInputs.position.offset];
                if (!mInputs.position.has(pi))
                {
                    mError = "a position index is outside the positions";
                    return -1;
                }
                // every value finite: a NaN in the key would break the map's order
                std::array<F32, 8> key{};
                const LLVector3 pos = mAxes.point(mInputs.position.at(pi, 0), mInputs.position.at(pi, 1), mInputs.position.at(pi, 2));
                if (!pos.isFinite())
                {
                    mError = "a vertex position is not a number (or too large)";
                    return -1;
                }
                key[0] = pos.mV[VX];
                key[1] = pos.mV[VY];
                key[2] = pos.mV[VZ];
                if (mInputs.normal.valid())
                {
                    const U64 ni = p[first + mInputs.normal.offset];
                    if (!mInputs.normal.has(ni))
                    {
                        mError = "a normal index is outside the normals";
                        return -1;
                    }
                    LLVector3 n = mAxes.turn(mInputs.normal.at(ni, 0), mInputs.normal.at(ni, 1), mInputs.normal.at(ni, 2));
                    if (!n.isFinite())
                    {
                        n.setZero();    // made upright where the face is filled
                    }
                    key[3] = n.mV[VX];
                    key[4] = n.mV[VY];
                    key[5] = n.mV[VZ];
                }
                if (mInputs.uv.valid())
                {
                    const U64 ti = p[first + mInputs.uv.offset];
                    if (!mInputs.uv.has(ti))
                    {
                        mError = "a UV index is outside the UVs";
                        return -1;
                    }
                    LLVector2 uv(mInputs.uv.at(ti, 0), mInputs.uv.at(ti, 1));
                    if (!uv.isFinite())
                    {
                        uv.setZero();
                    }
                    key[6] = uv.mV[VX];
                    key[7] = uv.mV[VY];
                }

                auto [it, added] = mSeen.try_emplace(key, (U32)mFace.positions.size());
                if (added)
                {
                    if (mFace.positions.size() >= MAX_FACE_VERTICES)
                    {
                        mError = "a face has more than 65535 vertices (split it in your 3D program)";
                        return -1;
                    }
                    if (mBudget.vertices == 0)
                    {
                        mError = "the file has more than " + std::to_string(MAX_FILE_VERTICES) + " vertices in all";
                        mBudget.spent = true;
                        return -1;
                    }
                    --mBudget.vertices;
                    mFace.positions.push_back(pos);
                    if (mInputs.normal.valid())
                    {
                        mFace.normals.emplace_back(key[3], key[4], key[5]);
                    }
                    if (mInputs.uv.valid())
                    {
                        mFace.uvs.emplace_back(key[6], key[7]);
                    }
                }
                return (S32)it->second;
            }

            // False when the file's allowance of corners is spent.
            bool triangle(S32 a, S32 b, S32 c)
            {
                if (a != b && b != c && a != c)
                {
                    if (mBudget.corners < 3)
                    {
                        mError = "the file has more than " + std::to_string(MAX_FILE_CORNERS) + " triangle corners in all";
                        mBudget.spent = true;
                        return false;
                    }
                    mBudget.corners -= 3;
                    mFace.indices.push_back(a);
                    mFace.indices.push_back(b);
                    mFace.indices.push_back(c);
                }
                return true;
            }

            const std::string& error() const { return mError; }

        private:
            RawFace& mFace;
            const Inputs& mInputs;
            const Axes& mAxes;
            Budget& mBudget;
            std::map<std::array<F32, 8>, U32> mSeen;
            std::string mError;
        };

        bool readTriangles(const domTriangles* triangles, const Axes& axes, Budget& budget, RawFace& face, Log& log)
        {
            Inputs inputs;
            if (!readInputs(triangles->getInput_array(), inputs, log) || !triangles->getP())
            {
                return false;
            }
            const domListOfUInts& p = triangles->getP()->getValue();
            const size_t corner_size = inputs.stride;
            FaceBuilder builder(face, inputs, axes, budget);
            for (size_t first = 0; first + 3 * corner_size <= p.getCount(); first += 3 * corner_size)
            {
                const S32 a = builder.corner(p, first);
                const S32 b = builder.corner(p, first + corner_size);
                const S32 c = builder.corner(p, first + 2 * corner_size);
                if (a < 0 || b < 0 || c < 0 || !builder.triangle(a, b, c))
                {
                    log.push_back("Face skipped: " + builder.error() + ".");
                    return false;
                }
            }
            return true;
        }

        bool readPolylist(const domPolylist* polylist, const Axes& axes, Budget& budget, RawFace& face, Log& log)
        {
            Inputs inputs;
            if (!readInputs(polylist->getInput_array(), inputs, log) || !polylist->getP() || !polylist->getVcount())
            {
                return false;
            }
            const domListOfUInts& counts = polylist->getVcount()->getValue();
            const domListOfUInts& p = polylist->getP()->getValue();
            const size_t corner_size = inputs.stride;
            FaceBuilder builder(face, inputs, axes, budget);
            size_t first = 0;
            for (size_t polygon = 0; polygon < counts.getCount(); ++polygon)
            {
                // a fan from the polygon's first corner, as LLDAELoader
                const size_t corners = (size_t)counts[polygon];
                S32 start = -1;
                S32 last = -1;
                for (size_t corner = 0; corner < corners; ++corner, first += corner_size)
                {
                    const S32 vertex = builder.corner(p, first);
                    if (vertex < 0)
                    {
                        log.push_back("Face skipped: " + builder.error() + ".");
                        return false;
                    }
                    if (corner == 0)
                    {
                        start = vertex;
                    }
                    else if (corner >= 2 && !builder.triangle(start, last, vertex))
                    {
                        log.push_back("Face skipped: " + builder.error() + ".");
                        return false;
                    }
                    last = vertex;
                }
            }
            return true;
        }

        // The <geometry>'s name (or id) of a <mesh>; Blender puts the object's name there.
        std::string nameOf(daeElement* mesh, size_t index)
        {
            for (daeElement* element = mesh; element; element = element->getParent())
            {
                std::string name = element->getAttribute("name");
                if (name.empty() && element->getID())
                {
                    name = element->getID();
                }
                if (!name.empty())
                {
                    return name;
                }
                if (element != mesh)
                {
                    break;      // the mesh and its <geometry> only
                }
            }
            return "mesh " + std::to_string(index + 1);
        }

        void readMesh(domMesh* mesh, const Axes& axes, Budget& budget, RawMesh& out, Log& log)
        {
            const domTriangles_Array& triangles = mesh->getTriangles_array();
            const domPolylist_Array& polylists = mesh->getPolylist_array();
            const size_t found = triangles.getCount() + polylists.getCount();
            if (found > (size_t)LL_SCULPT_MESH_MAX_FACES)
            {
                log.push_back("\"" + out.name + "\" has " + std::to_string(found) + " faces; only the first "
                              + std::to_string(LL_SCULPT_MESH_MAX_FACES) + " are shown (a mesh object has at most "
                              + std::to_string(LL_SCULPT_MESH_MAX_FACES) + ").");
            }
            if (mesh->getPolygons_array().getCount() > 0 || mesh->getTristrips_array().getCount() > 0
                || mesh->getTrifans_array().getCount() > 0)
            {
                log.push_back("\"" + out.name + "\" has <polygons>, <tristrips> or <trifans>, which are not read "
                              "(export triangles or a polylist).");
            }

            auto add = [&](auto read, const auto* element)
            {
                if (out.faces.size() >= (size_t)LL_SCULPT_MESH_MAX_FACES || budget.spent)
                {
                    return;
                }
                RawFace face;
                if (read(element, axes, budget, face, log) && !face.indices.empty())
                {
                    out.faces.push_back(std::move(face));
                }
            };
            for (size_t i = 0; i < triangles.getCount(); ++i)
            {
                add(readTriangles, triangles[i].cast());
            }
            for (size_t i = 0; i < polylists.getCount(); ++i)
            {
                add(readPolylist, polylists[i].cast());
            }
        }
    }

    bool readDAE(const std::string& path, bool preprocess, std::vector<RawMesh>& meshes, Log& log)
    {
        std::lock_guard<std::mutex> lock(sDAEMutex);

        // Collada expects file and folder names to be escaped (as LLDAELoader::OpenFile()).
        const char* allowed =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
            "abcdefghijklmnopqrstuvwxyz"
            "0123456789"
            "%-._~:\"|\\/";
        const std::string uri = LLURI::escape(path, allowed);

        const S64 size = fileSize(path);
        if (size < 0 || (U64)size > MAX_FILE_BYTES)
        {
            log.push_back(size < 0 ? "The file could not be opened."
                                   : "The file is larger than " + std::to_string(MAX_FILE_BYTES >> 20) + " MB.");
            return false;
        }
        std::string text;
        if (!readText(path, text))
        {
            log.push_back("The file could not be read.");
            return false;
        }
        if (preprocess)
        {
            preprocessDAE(text);
        }
        if (std::string why; !wellFormedXML(text, why))
        {
            log.push_back("The file is broken (not well-formed XML, " + why + "); it may be cut short. Export it again.");
            return false;
        }
        if (!preprocess)
        {
            text = std::string();   // the DOM reads the file itself
        }

        DAE dae;
        domCOLLADA* dom = preprocess ? dae.openFromMemory(uri, text.c_str()) : dae.open(uri);
        daeDocument* document = dom ? dae.getDoc(uri) : nullptr;
        daeElement* root = document ? document->getDomRoot() : nullptr;
        daeDatabase* db = dae.getDatabase();
        if (!root || !db)
        {
            log.push_back("The file could not be read as COLLADA.");
            return false;
        }

        Axes axes;
        if (auto* unit = daeSafeCast<domAsset::domUnit>(root->getDescendant(daeElement::matchType(domAsset::domUnit::ID()))))
        {
            // NaN passes a "<= 0" test, and a double too large for F32 becomes infinite
            axes.metres = (F32)unit->getMeter();
            if (!(axes.metres > 0.f) || !std::isfinite(axes.metres))
            {
                axes.metres = 1.f;
            }
        }
        if (auto* up = daeSafeCast<domAsset::domUp_axis>(root->getDescendant(daeElement::matchType(domAsset::domUp_axis::ID()))))
        {
            axes.up = up->getValue();
        }

        Budget budget;
        const size_t count = db->getElementCount(nullptr, COLLADA_TYPE_MESH);
        for (size_t i = 0; i < count; ++i)
        {
            if (i >= MAX_FILE_MESHES)  // read, kept or not
            {
                log.push_back("The file has more than " + std::to_string(MAX_FILE_MESHES) + " meshes; the rest are left out.");
                break;
            }
            if (budget.spent)
            {
                log.push_back("The rest of the file is left out (it is larger than a local mesh may be).");
                break;
            }
            daeElement* element = nullptr;
            db->getElement(&element, (daeInt)i, nullptr, COLLADA_TYPE_MESH);
            domMesh* mesh = daeSafeCast<domMesh>(element);
            if (!mesh)
            {
                continue;
            }
            RawMesh raw;
            raw.name = utf8str_truncate(nameOf(element, i), MAX_NAME_BYTES);
            readMesh(mesh, axes, budget, raw, log);
            if (raw.faces.empty())
            {
                log.push_back("\"" + raw.name + "\" has no faces that could be read; left out.");
                continue;
            }
            meshes.push_back(std::move(raw));
        }

        if (meshes.empty())
        {
            log.push_back(count ? "No mesh of the file could be read." : "The file has no meshes.");
            return false;
        }
        return true;
    }
}
