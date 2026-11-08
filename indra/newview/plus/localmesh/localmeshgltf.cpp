/**
 * @file localmeshgltf.cpp
 * @brief Local mesh: the glTF (.gltf, .glb) reader.
 *
 * Ported from Firestorm's fslocalmeshimportgltf.cpp, by Beq Janus (see ORIGIN.md).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, Beq Janus.
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

// Ported from Firestorm's Local Mesh (fslocalmeshimportgltf.cpp, Beq Janus; LGPL 2.1),
// without its skins. Every node of the default scene that has a mesh is one mesh of the list,
// its primitives are its faces (at most 8), placed by the node's transform within the file,
// turned from glTF's Y up to the viewer's Z up as the official uploader (LLGLTFLoader) does,
// V flipped as it does (1 - v); strips and fans are unrolled.
//
// The file is a user's file and may be broken or made to harm, so this reader does not hand
// it to LL::GLTF::Asset::load(): that one trusts the file (node, accessor, buffer view and
// buffer numbers are used as given, the .glb's lengths can overflow, a parse error throws,
// an unknown accessor type or primitive mode asserts) and registers the file's images in
// Local Textures again on every load. Here the JSON is parsed without exceptions, only the
// parts that make geometry are given to the official structures (Asset::operator=(), which
// checks the JSON's types), and the arrays are read from the buffers by this file, every
// number checked first. The node tree is walked here, each node once (a loop or a node with
// two parents is skipped), without recursion. No image is decoded and nothing waits on the
// main thread; the image files beside the file are only listed (localmesh.cpp adds them to
// Local Textures once).
//
// What a file may ask for is bounded, so that a small file cannot take unbounded memory or
// time: the .glb (1 GB) and the JSON (32 MB), the number of each thing it lists, the buffer
// files (1 GB in all, only their length read; a .glb's own chunk is read in place), the
// meshes read, and the vertices and triangle corners read, spent whether the face is kept
// or not (localmeshread.h); names are cut to 128 bytes.

#include "llviewerprecompiledheaders.h"

#include "localmeshread.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <string_view>

#include "gltf/asset.h"
#include "lldir.h"
#include "llfile.h"
#include "lluri.h"
#include "llvolume.h"

#include <glm/gtc/matrix_inverse.hpp>

namespace plus::localmesh
{
    namespace
    {
        using LL::GLTF::Accessor;

        // glTF is Y up, the viewer Z up (LLGLTFLoader's coord_system_rotation)
        const glm::mat4 TO_VIEWER_AXES(
            1.f, 0.f, 0.f, 0.f,
            0.f, 0.f, 1.f, 0.f,
            0.f, -1.f, 0.f, 0.f,
            0.f, 0.f, 0.f, 1.f);

        constexpr size_t MAX_FACE_VERTICES = 65535;
        constexpr U64 MAX_FILE_BYTES = 1ull << 30;      // a .glb, and the buffer files of a file in all
        constexpr U64 MAX_JSON_BYTES = 32ull << 20;     // a .gltf, or the JSON of a .glb (the geometry is in the buffers)

        // How many of each a file may list: every one costs a structure of some hundred bytes
        // however short it is in the JSON ("{}"). Primitives beyond a mesh's 8 faces are cut.
        constexpr size_t MAX_NODES = 100000;
        constexpr size_t MAX_MESHES = 10000;
        constexpr size_t MAX_ACCESSORS = 100000;
        constexpr size_t MAX_BUFFER_VIEWS = 100000;
        constexpr size_t MAX_BUFFERS = 1024;
        constexpr size_t MAX_SCENES = 1024;
        constexpr size_t MAX_IMAGES = 1024;
        constexpr size_t MAX_ATTRIBUTES = 64;           // of one primitive
        constexpr size_t MAX_EXTENSIONS = 64;           // required by the file
        constexpr size_t MAX_LISTED_IMAGES = 32;        // offered to Local Textures, of one file

        constexpr U32 GLB_MAGIC = 0x46546C67;           // "glTF"
        constexpr U32 GLB_JSON = 0x4E4F534A;            // "JSON"
        constexpr U32 GLB_BIN = 0x004E4942;             // "BIN\0"

        // The file's first `want` bytes (all of it when want is 0), refused when that is more
        // than max_bytes or the file is shorter.
        template <typename Bytes>
        bool readFileBytes(const std::string& path, U64 want, U64 max_bytes, Bytes& out, std::string& error)
        {
            llifstream file(path.c_str(), std::ios::binary);
            if (!file.is_open())
            {
                error = "could not be opened";
                return false;
            }
            file.seekg(0, std::ios::end);
            const std::streamoff size = file.tellg();
            if (size < 0)
            {
                error = "could not be read";
                return false;
            }
            const U64 take = want ? want : (U64)size;
            if (take > max_bytes)
            {
                error = "is larger than " + std::to_string(max_bytes >> 20) + " MB";
                return false;
            }
            if ((U64)size < take)
            {
                error = "is shorter than the glTF says";
                return false;
            }
            file.seekg(0, std::ios::beg);
            out.resize((size_t)take);
            if (take > 0 && !file.read((char*)out.data(), (std::streamsize)take))
            {
                error = "could not be read";
                return false;
            }
            return true;
        }

        // A path with its folders "." and ".." taken out and the system's delimiter between
        // folders, for telling one file from another however the file wrote its name (case is
        // kept; compare it lower-cased, as Windows does).
        std::string tidyPath(const std::string& path)
        {
            const auto separator = [](char c) { return c == '/' || c == '\\'; };
            const bool share = path.size() >= 2 && separator(path[0]) && separator(path[1]);
            const size_t floor = share ? 2 : 0;    // a network share's server and share name stay
            std::vector<std::string> parts;
            std::string part;
            const auto flush = [&]()
            {
                if (part == "..")
                {
                    if (parts.size() > floor && parts.back() != ".." && parts.back().find(':') == std::string::npos)
                    {
                        parts.pop_back();   // a drive ("C:") stays: there is nothing above it
                    }
                    else if (!share && (parts.empty() || parts.back() == ".."))
                    {
                        parts.push_back(part);  // a relative path that goes up
                    }
                }
                else if (!part.empty() && part != ".")
                {
                    parts.push_back(part);
                }
                part.clear();
            };
            for (char c : path)
            {
                if (separator(c))
                {
                    flush();
                }
                else
                {
                    part += c;
                }
            }
            flush();
            const std::string delimiter = gDirUtilp->getDirDelimiter();
            std::string out = share ? delimiter + delimiter : (!path.empty() && separator(path[0])) ? delimiter : "";
            for (size_t i = 0; i < parts.size(); ++i)
            {
                out += (i ? delimiter : "") + parts[i];
            }
            return out;
        }

        // A URI of the file (a buffer, an image) as a path in the opened file's folder or below
        // it, as written or unescaped (the first that names a file; else the one as written).
        // False for one that could name anything else: a drive, a server ("//server/share",
        // which would hand the server the user's Windows sign-in), a scheme ("file:"), a ".."
        // folder (written or escaped as %2E%2E), or a NUL.
        bool pathInFolder(const std::string& folder, const std::string& uri, std::string& path)
        {
            const auto inside = [](const std::string& relative)
            {
                if (relative.empty() || relative.find('\0') != std::string::npos || relative.find(':') != std::string::npos ||
                    relative[0] == '/' || relative[0] == '\\')
                {
                    return false;
                }
                size_t start = 0;
                for (size_t i = 0; i <= relative.size(); ++i)
                {
                    if (i == relative.size() || relative[i] == '/' || relative[i] == '\\')
                    {
                        // a name of dots (and spaces, which Windows drops at the end) is "." or
                        // goes up: only "." itself stays ("./a.png")
                        const std::string part = relative.substr(start, i - start);
                        if (!part.empty() && part != "." && part.find_first_not_of(". ") == std::string::npos)
                        {
                            return false;
                        }
                        // a device (CON, AUX, COM1, ...: also "con.png") would be opened instead of a
                        // file, and CON waits for keyboard input
                        std::string stem = utf8str_tolower(part.substr(0, part.find('.')));
                        while (!stem.empty() && stem.back() == ' ')
                        {
                            stem.pop_back();
                        }
                        static const char* const DEVICES[] = { "con", "prn", "aux", "nul", "conin$", "conout$" };
                        const bool numbered = stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
                                              stem[3] >= '0' && stem[3] <= '9';
                        if (numbered || std::find(std::begin(DEVICES), std::end(DEVICES), stem) != std::end(DEVICES))
                        {
                            return false;
                        }
                        start = i + 1;
                    }
                }
                return true;
            };
            const std::string unescaped = LLURI::unescape(uri);
            if (!inside(uri) || !inside(unescaped))
            {
                return false;
            }
            const std::string delimiter = gDirUtilp->getDirDelimiter();
            path = folder + delimiter + uri;
            if (!gDirUtilp->fileExists(path))
            {
                path = folder + delimiter + unescaped;     // the URI may be escaped
            }
            return true;
        }

        U32 readU32(const std::string& data, size_t at)
        {
            U32 value;
            memcpy(&value, data.data() + at, sizeof(value));
            return value;
        }

        // The JSON chunk of a .glb and its first binary chunk (has_bin false without one).
        bool splitGLB(const std::string& data, std::string_view& json, std::string_view& bin, bool& has_bin, std::string& error)
        {
            if (data.size() < 12 || readU32(data, 0) != GLB_MAGIC)
            {
                error = "not a .glb file (no glTF header)";
                return false;
            }
            if (readU32(data, 4) != 2)
            {
                error = "a .glb of another version than 2";
                return false;
            }
            if (readU32(data, 8) != data.size())
            {
                error = "the length in the .glb's header is not the file's (is the file cut short?)";
                return false;
            }
            bool has_json = false;
            has_bin = false;
            size_t at = 12;
            while (data.size() - at >= 8)
            {
                const size_t length = readU32(data, at);
                const U32 type = readU32(data, at + 4);
                at += 8;
                if (length > data.size() - at)
                {
                    error = "a chunk of the .glb runs past the end of the file";
                    return false;
                }
                const std::string_view chunk(data.data() + at, length);
                if (!has_json)
                {
                    if (type != GLB_JSON)
                    {
                        error = "the .glb does not start with its JSON";
                        return false;
                    }
                    json = chunk;
                    has_json = true;
                }
                else if (type == GLB_BIN && !has_bin)
                {
                    bin = chunk;
                    has_bin = true;
                }
                at += length;   // other chunks are skipped (as the format says)
            }
            if (!has_json)
            {
                error = "the .glb has no JSON";
                return false;
            }
            return true;
        }

        // Extensions that change only materials and textures, which are not read here: a file
        // that requires one still has plain geometry. Any other required extension (mesh
        // compression, quantised attributes...) may change what the arrays mean.
        bool geometryUnaffected(const std::string& extension)
        {
            return extension.rfind("KHR_materials_", 0) == 0 || extension.rfind("KHR_texture_", 0) == 0
                || extension.rfind("EXT_texture_", 0) == 0;
        }

        bool knownAccessorType(std::string_view type)
        {
            return type == "SCALAR" || type == "VEC2" || type == "VEC3" || type == "VEC4"
                || type == "MAT2" || type == "MAT3" || type == "MAT4";
        }

        boost::json::array* arrayOf(boost::json::object& object, const char* key)
        {
            boost::json::value* value = object.if_contains(key);
            return value && value->is_array() ? &value->get_array() : nullptr;
        }

        boost::json::array* arrayOf(boost::json::value& value, const char* key)
        {
            return value.is_object() ? arrayOf(value.get_object(), key) : nullptr;
        }

        // The JSON within the limits above, before the official structures are made of it.
        // primitive_counts: by mesh, how many primitives it had before they were cut to 8.
        bool withinLimits(boost::json::object& object, std::vector<size_t>& primitive_counts, std::string& error)
        {
            static const struct { const char* key; size_t limit; } LISTS[] = {
                { "nodes", MAX_NODES }, { "meshes", MAX_MESHES }, { "accessors", MAX_ACCESSORS },
                { "bufferViews", MAX_BUFFER_VIEWS }, { "buffers", MAX_BUFFERS }, { "scenes", MAX_SCENES },
                { "images", MAX_IMAGES }, { "extensionsRequired", MAX_EXTENSIONS } };
            for (const auto& list : LISTS)
            {
                if (const boost::json::array* array = arrayOf(object, list.key); array && array->size() > list.limit)
                {
                    error = std::string("it lists more than ") + std::to_string(list.limit) + " " + list.key;
                    return false;
                }
            }

            // a tree has fewer links than nodes; more can only be repeats (each node is taken once)
            boost::json::array* nodes = arrayOf(object, "nodes");
            const size_t node_count = nodes ? nodes->size() : 0;
            size_t links = 0;
            if (nodes)
            {
                for (boost::json::value& node : *nodes)
                {
                    const boost::json::array* children = arrayOf(node, "children");
                    links += children ? children->size() : 0;
                }
            }
            if (boost::json::array* scenes = arrayOf(object, "scenes"))
            {
                for (boost::json::value& scene : *scenes)
                {
                    const boost::json::array* roots = arrayOf(scene, "nodes");
                    if (roots && roots->size() > node_count)
                    {
                        error = "a scene lists more nodes than the file has";
                        return false;
                    }
                }
            }
            if (links > node_count)
            {
                error = "its nodes have more children than there are nodes";
                return false;
            }

            if (boost::json::array* meshes = arrayOf(object, "meshes"))
            {
                primitive_counts.resize(meshes->size(), 0);
                for (size_t m = 0; m < meshes->size(); ++m)
                {
                    boost::json::array* primitives = arrayOf((*meshes)[m], "primitives");
                    if (!primitives)
                    {
                        continue;
                    }
                    primitive_counts[m] = primitives->size();
                    if (primitives->size() > (size_t)LL_SCULPT_MESH_MAX_FACES)
                    {
                        primitives->resize(LL_SCULPT_MESH_MAX_FACES);   // only those are read
                    }
                    for (const boost::json::value& primitive : *primitives)
                    {
                        const boost::json::value* attributes = primitive.is_object() ? primitive.get_object().if_contains("attributes") : nullptr;
                        if (attributes && attributes->is_object() && attributes->get_object().size() > MAX_ATTRIBUTES)
                        {
                            error = "a primitive has more than " + std::to_string(MAX_ATTRIBUTES) + " attributes";
                            return false;
                        }
                    }
                }
            }

            if (boost::json::array* accessors = arrayOf(object, "accessors"))
            {
                for (boost::json::value& accessor : *accessors)
                {
                    if (!accessor.is_object())
                    {
                        continue;
                    }
                    // an unknown type asserts in LL::GLTF::gltf_type_to_enum()
                    const boost::json::value* type = accessor.get_object().if_contains("type");
                    if (type && type->is_string()
                        && !knownAccessorType(std::string_view(type->get_string().data(), type->get_string().size())))
                    {
                        error = "an accessor has an unknown type";
                        return false;
                    }
                    // not read here, and as long as the file likes
                    accessor.get_object().erase("max");
                    accessor.get_object().erase("min");
                }
            }
            return true;
        }

        // The parts of the JSON that make geometry, checked where the official structures would
        // assert or take memory without end, as an Asset (its own operator= checks the JSON's
        // types).
        bool assetFromJSON(std::string_view json, LL::GLTF::Asset& asset, std::vector<size_t>& primitive_counts, std::string& error)
        {
            if (json.size() > MAX_JSON_BYTES)
            {
                error = "its JSON is larger than " + std::to_string(MAX_JSON_BYTES >> 20) + " MB";
                return false;
            }
            // names in another encoding than UTF-8 are taken as they are; nesting stays limited
            boost::json::parse_options options;
            options.allow_invalid_utf8 = true;
            options.max_depth = 64;
            boost::system::error_code ec;
            boost::json::value root = boost::json::parse(json, ec, {}, options);
            if (ec)
            {
                error = "it is not valid glTF JSON (" + ec.message() + ")";
                return false;
            }
            if (!root.is_object())
            {
                error = "it is not a glTF document";
                return false;
            }
            boost::json::object& object = root.get_object();
            if (!withinLimits(object, primitive_counts, error))
            {
                return false;
            }

            static const char* const PARTS[] = { "scene", "scenes", "nodes", "meshes", "accessors", "bufferViews",
                                                 "buffers", "images", "extensionsRequired" };
            boost::json::object parts;
            for (const char* part : PARTS)
            {
                if (boost::json::value* value = object.if_contains(part))
                {
                    parts[part] = std::move(*value);
                }
            }
            asset = boost::json::value(std::move(parts));
            return true;
        }

        size_t componentCount(Accessor::Type type)
        {
            switch (type)
            {
            case Accessor::Type::SCALAR: return 1;
            case Accessor::Type::VEC2:   return 2;
            case Accessor::Type::VEC3:   return 3;
            case Accessor::Type::VEC4:   return 4;
            case Accessor::Type::MAT2:   return 4;
            case Accessor::Type::MAT3:   return 9;
            case Accessor::Type::MAT4:   return 16;
            }
            return 0;
        }

        size_t componentSize(Accessor::ComponentType type)
        {
            switch (type)
            {
            case Accessor::ComponentType::BYTE:
            case Accessor::ComponentType::UNSIGNED_BYTE:  return 1;
            case Accessor::ComponentType::SHORT:
            case Accessor::ComponentType::UNSIGNED_SHORT: return 2;
            case Accessor::ComponentType::UNSIGNED_INT:
            case Accessor::ComponentType::FLOAT:          return 4;
            }
            return 0;   // a number the file made up
        }

        F32 readComponent(const U8* p, Accessor::ComponentType type, bool normalized)
        {
            switch (type)
            {
            case Accessor::ComponentType::FLOAT:
            {
                F32 value;
                memcpy(&value, p, sizeof(value));
                return value;
            }
            case Accessor::ComponentType::UNSIGNED_BYTE:
                return normalized ? *p / 255.f : (F32)*p;
            case Accessor::ComponentType::BYTE:
            {
                const S8 value = (S8)*p;
                return normalized ? llmax(value / 127.f, -1.f) : (F32)value;
            }
            case Accessor::ComponentType::UNSIGNED_SHORT:
            {
                U16 value;
                memcpy(&value, p, sizeof(value));
                return normalized ? value / 65535.f : (F32)value;
            }
            case Accessor::ComponentType::SHORT:
            {
                S16 value;
                memcpy(&value, p, sizeof(value));
                return normalized ? llmax(value / 32767.f, -1.f) : (F32)value;
            }
            case Accessor::ComponentType::UNSIGNED_INT:
            {
                U32 value;
                memcpy(&value, p, sizeof(value));
                return (F32)value;
            }
            }
            return 0.f;
        }

        // The asset's arrays, read from its buffers with every number checked. A buffer is
        // loaded when an accessor first needs it.
        class Arrays
        {
        public:
            Arrays(LL::GLTF::Asset& asset, const std::string& folder, std::string_view glb_bin, bool has_glb_bin)
                : mAsset(asset), mFolder(folder), mGLBBin(glb_bin), mHasGLBBin(has_glb_bin),
                  mBufferErrors(asset.mBuffers.size()), mBufferLoaded(asset.mBuffers.size(), false),
                  mBufferData(asset.mBuffers.size())
            {
            }

            // The accessor's elements of `components` floats each (the type must match), at most
            // max_count of them: with more, nothing is read and count says how many there are.
            bool floats(S32 accessor_index, size_t components, size_t max_count, std::vector<F32>& out, size_t& count, std::string& error)
            {
                Element element;
                if (!locate(accessor_index, components, element, error))
                {
                    return false;
                }
                count = element.count;
                if (count > max_count)
                {
                    error = "too many elements";
                    return false;
                }
                out.resize(count * components);
                const size_t size = componentSize(element.type);
                for (size_t i = 0; i < count; ++i)
                {
                    const U8* p = element.first + i * element.stride;
                    for (size_t c = 0; c < components; ++c)
                    {
                        out[i * components + c] = readComponent(p + c * size, element.type, element.normalized);
                    }
                }
                return true;
            }

            // At most max_count of them, as floats() does.
            bool indices(S32 accessor_index, size_t max_count, std::vector<U32>& out, size_t& count, std::string& error)
            {
                Element element;
                if (!locate(accessor_index, 1, element, error))
                {
                    return false;
                }
                if (element.type != Accessor::ComponentType::UNSIGNED_BYTE && element.type != Accessor::ComponentType::UNSIGNED_SHORT
                    && element.type != Accessor::ComponentType::UNSIGNED_INT)
                {
                    error = "the indices are not unsigned integers";
                    return false;
                }
                count = element.count;
                if (count > max_count)
                {
                    error = "too many indices";
                    return false;
                }
                out.resize(element.count);
                for (size_t i = 0; i < element.count; ++i)
                {
                    const U8* p = element.first + i * element.stride;
                    switch (element.type)
                    {
                    case Accessor::ComponentType::UNSIGNED_BYTE:
                        out[i] = *p;
                        break;
                    case Accessor::ComponentType::UNSIGNED_SHORT:
                    {
                        U16 value;
                        memcpy(&value, p, sizeof(value));
                        out[i] = value;
                        break;
                    }
                    default:
                        memcpy(&out[i], p, sizeof(U32));
                        break;
                    }
                }
                return true;
            }

        private:
            struct Span
            {
                const U8* first = nullptr;
                size_t size = 0;
            };

            struct Element
            {
                const U8* first = nullptr;
                size_t stride = 0;
                size_t count = 0;
                Accessor::ComponentType type = Accessor::ComponentType::FLOAT;
                bool normalized = false;
            };

            // Where an accessor's elements are; every index, offset and length checked.
            bool locate(S32 accessor_index, size_t components, Element& out, std::string& error)
            {
                if (accessor_index < 0 || accessor_index >= (S32)mAsset.mAccessors.size())
                {
                    error = "it refers to an accessor that is not in the file";
                    return false;
                }
                const Accessor& accessor = mAsset.mAccessors[accessor_index];
                const size_t size = componentSize(accessor.mComponentType);
                if (size == 0)
                {
                    error = "an accessor has an unknown component type";
                    return false;
                }
                if (componentCount(accessor.mType) != components)
                {
                    error = "an accessor has the wrong type for what it holds";
                    return false;
                }
                if (accessor.mCount <= 0)
                {
                    error = "an accessor is empty";
                    return false;
                }
                if (accessor.mBufferView < 0 || accessor.mBufferView >= (S32)mAsset.mBufferViews.size())
                {
                    error = "an accessor has no buffer view (sparse accessors are not read)";
                    return false;
                }
                const LL::GLTF::BufferView& view = mAsset.mBufferViews[accessor.mBufferView];
                if (view.mBuffer < 0 || view.mBuffer >= (S32)mAsset.mBuffers.size())
                {
                    error = "a buffer view refers to a buffer that is not in the file";
                    return false;
                }
                if (!loadBuffer(view.mBuffer, error))
                {
                    return false;
                }
                const Span& data = mBufferData[view.mBuffer];
                if (view.mByteOffset < 0 || view.mByteLength < 0 || view.mByteStride < 0 || accessor.mByteOffset < 0)
                {
                    error = "a negative offset or length";
                    return false;
                }
                if ((U64)view.mByteOffset + (U64)view.mByteLength > data.size)
                {
                    error = "a buffer view runs past the end of its buffer";
                    return false;
                }
                const U64 element_size = (U64)size * components;
                const U64 stride = view.mByteStride ? (U64)view.mByteStride : element_size;
                if (stride < element_size)
                {
                    error = "a buffer view's stride is smaller than its elements";
                    return false;
                }
                // count and stride are below 2^31: no overflow in 64 bits
                if ((U64)accessor.mByteOffset + stride * ((U64)accessor.mCount - 1) + element_size > (U64)view.mByteLength)
                {
                    error = "an accessor runs past the end of its buffer view";
                    return false;
                }
                out.first = data.first + view.mByteOffset + accessor.mByteOffset;
                out.stride = (size_t)stride;
                out.count = (size_t)accessor.mCount;
                out.type = accessor.mComponentType;
                out.normalized = accessor.mNormalized;
                return true;
            }

            bool loadBuffer(S32 index, std::string& error)
            {
                if (mBufferLoaded[index])
                {
                    return true;
                }
                if (!mBufferErrors[index].empty())
                {
                    error = mBufferErrors[index];
                    return false;
                }
                LL::GLTF::Buffer& buffer = mAsset.mBuffers[index];
                std::string& why = mBufferErrors[index];
                if (buffer.mByteLength <= 0)
                {
                    why = "a buffer has no length";
                }
                else if (buffer.mUri.empty())
                {
                    // the .glb's binary chunk (only the first buffer may be it)
                    if (index != 0 || !mHasGLBBin)
                    {
                        why = "a buffer has no data (no file, and no binary chunk in a .glb)";
                    }
                    else if ((U64)buffer.mByteLength > mGLBBin.size())
                    {
                        why = "the .glb's binary chunk is shorter than its buffer";
                    }
                    else
                    {
                        // read in place: the .glb is in memory while the file is read
                        mBufferData[index] = { (const U8*)mGLBBin.data(), (size_t)buffer.mByteLength };
                    }
                }
                else if (buffer.mUri.rfind("data:", 0) == 0)
                {
                    why = "a buffer is embedded as text (a data: URI), which is not read (export .glb or glTF Separate)";
                }
                else
                {
                    std::string path;
                    // only the buffer's length is read, within what is left of the file's allowance
                    std::string file_error;
                    if (!pathInFolder(mFolder, buffer.mUri, path))
                    {
                        why = "the buffer file \"" + utf8str_truncate(buffer.mUri, MAX_NAME_BYTES) +
                              "\" is not in the file's folder or below it, and is not read";
                    }
                    else if (!readFileBytes(path, (U64)buffer.mByteLength, mBytesLeft, buffer.mData, file_error))
                    {
                        why = "the buffer file \"" + utf8str_truncate(buffer.mUri, MAX_NAME_BYTES) + "\" " + file_error;
                    }
                    else
                    {
                        mBytesLeft -= buffer.mData.size();
                        mBufferData[index] = { buffer.mData.data(), buffer.mData.size() };
                    }
                }
                if (!why.empty())
                {
                    std::vector<U8>().swap(buffer.mData);
                    error = why;
                    return false;
                }
                mBufferLoaded[index] = true;
                return true;
            }

            LL::GLTF::Asset& mAsset;
            std::string mFolder;
            std::string_view mGLBBin;
            bool mHasGLBBin;
            std::vector<std::string> mBufferErrors;     // by buffer: why it could not be loaded
            std::vector<bool> mBufferLoaded;
            std::vector<Span> mBufferData;    // by buffer, once loaded: the .glb's chunk or the buffer's mData
            U64 mBytesLeft = MAX_FILE_BYTES;            // for buffer files: one large file listed many times is refused
        };

        const S32* attribute(const LL::GLTF::Primitive& prim, const char* name)
        {
            auto it = prim.mAttributes.find(name);
            return it != prim.mAttributes.end() ? &it->second : nullptr;
        }

        // What is left of a file's allowance (MAX_FILE_VERTICES, MAX_FILE_CORNERS).
        struct Budget
        {
            size_t vertices = MAX_FILE_VERTICES;
            size_t corners = MAX_FILE_CORNERS;
            bool spent = false;     // a face was refused for it: the rest of the file is not read
        };

        // The primitive's triangles as a list of corners (strips and fans unrolled, as Firestorm),
        // at most max_corners of them.
        bool triangleCorners(LL::GLTF::Primitive::Mode mode, std::vector<U32>&& base, size_t max_corners,
                             std::vector<U32>& out, std::string& error)
        {
            switch (mode)
            {
            case LL::GLTF::Primitive::Mode::TRIANGLES:
                out = std::move(base);
                break;
            case LL::GLTF::Primitive::Mode::TRIANGLE_STRIP:
            case LL::GLTF::Primitive::Mode::TRIANGLE_FAN:
                if (base.size() > max_corners / 3 + 2)
                {
                    error = "too many indices";
                    return false;
                }
                out.reserve(base.size() >= 2 ? (base.size() - 2) * 3 : 0);
                for (size_t i = 2; i < base.size(); ++i)
                {
                    if (mode == LL::GLTF::Primitive::Mode::TRIANGLE_FAN)
                    {
                        out.push_back(base[0]);
                        out.push_back(base[i - 1]);
                        out.push_back(base[i]);
                    }
                    else
                    {
                        const bool odd = (i % 2) == 1;      // every other triangle of a strip is wound the other way
                        out.push_back(base[odd ? i - 1 : i - 2]);
                        out.push_back(base[odd ? i - 2 : i - 1]);
                        out.push_back(base[i]);
                    }
                }
                break;
            default:
                error = "points or lines, not triangles";
                return false;
            }
            if (out.size() % 3 != 0)
            {
                error = "the index count is not a multiple of 3";
                return false;
            }
            return true;
        }

        bool readPrimitive(const LL::GLTF::Primitive& prim, const glm::mat4& transform, Arrays& arrays, Budget& budget,
                           RawFace& face, std::string& error)
        {
            if (prim.mMode != LL::GLTF::Primitive::Mode::TRIANGLES && prim.mMode != LL::GLTF::Primitive::Mode::TRIANGLE_STRIP
                && prim.mMode != LL::GLTF::Primitive::Mode::TRIANGLE_FAN)
            {
                error = "points or lines, not triangles";
                return false;
            }
            const S32* position_accessor = attribute(prim, "POSITION");
            if (!position_accessor)
            {
                error = "no vertices";
                return false;
            }
            // a face larger than a volume face, or than what is left of the file's allowance, is
            // refused before it is read
            std::vector<F32> values;
            size_t count = 0;
            const size_t max_vertices = llmin(MAX_FACE_VERTICES, budget.vertices);
            if (!arrays.floats(*position_accessor, 3, max_vertices, values, count, error))
            {
                if (count > MAX_FACE_VERTICES)
                {
                    error = "more than 65535 vertices (split it in your 3D program)";
                }
                else if (count > max_vertices)
                {
                    error = "the file has more than " + std::to_string(MAX_FILE_VERTICES) + " vertices in all";
                    budget.spent = true;
                }
                return false;
            }
            // what is read is spent, whether the face is kept or not: a file that repeats a
            // primitive that is left out (all triangles degenerate, say) cannot read it without end
            budget.vertices -= count;

            face.positions.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                const glm::vec4 v = transform * glm::vec4(values[i * 3], values[i * 3 + 1], values[i * 3 + 2], 1.f);
                face.positions.emplace_back(v.x, v.y, v.z);
                if (!face.positions.back().isFinite())
                {
                    error = "a vertex position is not a number";
                    return false;
                }
            }

            // normals and UVs that do not fit are left out (normals are then made from the triangles)
            std::string ignored;
            size_t normal_count = 0;
            if (const S32* normal_accessor = attribute(prim, "NORMAL");
                normal_accessor && arrays.floats(*normal_accessor, 3, count, values, normal_count, ignored) && normal_count == count)
            {
                const glm::mat3 normal_transform = glm::inverseTranspose(glm::mat3(transform));
                face.normals.reserve(count);
                for (size_t i = 0; i < count; ++i)
                {
                    const glm::vec3 v = normal_transform * glm::vec3(values[i * 3], values[i * 3 + 1], values[i * 3 + 2]);
                    face.normals.emplace_back(v.x, v.y, v.z);
                    if (!face.normals.back().isFinite())
                    {
                        face.normals.clear();
                        break;
                    }
                }
            }
            size_t uv_count = 0;
            if (const S32* uv_accessor = attribute(prim, "TEXCOORD_0");
                uv_accessor && arrays.floats(*uv_accessor, 2, count, values, uv_count, ignored) && uv_count == count)
            {
                face.uvs.reserve(count);
                for (size_t i = 0; i < count; ++i)
                {
                    LLVector2 uv(values[i * 2], 1.f - values[i * 2 + 1]);
                    if (!uv.isFinite())
                    {
                        uv.setZero();
                    }
                    face.uvs.push_back(uv);
                }
            }
            std::vector<F32>().swap(values);

            std::vector<U32> base;
            if (prim.mIndices != LL::GLTF::INVALID_INDEX)
            {
                size_t index_count = 0;
                if (!arrays.indices(prim.mIndices, budget.corners, base, index_count, error))
                {
                    if (index_count > budget.corners)
                    {
                        error = "the file has more than " + std::to_string(MAX_FILE_CORNERS) + " triangle corners in all";
                        budget.spent = true;
                    }
                    return false;
                }
            }
            else
            {
                if (count > budget.corners)
                {
                    error = "the file has more than " + std::to_string(MAX_FILE_CORNERS) + " triangle corners in all";
                    budget.spent = true;
                    return false;
                }
                base.resize(count);
                for (U32 i = 0; i < (U32)count; ++i)
                {
                    base[i] = i;
                }
            }
            // spent as read (as the vertices above); a strip or fan unrolled spends the rest
            const size_t base_count = base.size();
            const size_t corners_left = budget.corners;
            budget.corners -= llmin(budget.corners, base_count);
            std::vector<U32> corners;
            if (!triangleCorners(prim.mMode, std::move(base), corners_left, corners, error))
            {
                if (prim.mMode != LL::GLTF::Primitive::Mode::TRIANGLES && base_count > corners_left / 3 + 2)
                {
                    error = "the file has more than " + std::to_string(MAX_FILE_CORNERS) + " triangle corners in all";
                    budget.spent = true;
                }
                return false;
            }
            budget.corners -= llmin(budget.corners, corners.size() > base_count ? corners.size() - base_count : 0);

            // a mirroring transform turns the triangles inside out
            const bool flip = glm::determinant(glm::mat3(transform)) < 0.f;
            face.indices.reserve(corners.size());
            for (size_t i = 0; i < corners.size(); i += 3)
            {
                U32 tri[3];
                for (size_t k = 0; k < 3; ++k)
                {
                    tri[k] = corners[i + k];
                    if (tri[k] >= count)
                    {
                        error = "an index is outside the vertices";
                        return false;
                    }
                }
                if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2])
                {
                    continue;
                }
                face.indices.push_back(tri[0]);
                face.indices.push_back(tri[flip ? 2 : 1]);
                face.indices.push_back(tri[flip ? 1 : 2]);
            }
            return true;
        }

        struct MeshNode
        {
            S32 node;
            glm::mat4 matrix;   // within the file
        };

        // The scene's nodes that have a mesh, depth first in the file's order, each with its
        // transform within the file. Every node is taken once: one reached again (a loop, or
        // a node with two parents, which glTF does not allow) or out of range is skipped.
        // No recursion: a deep tree cannot run the stack out.
        std::vector<MeshNode> meshNodes(LL::GLTF::Asset& asset, const LL::GLTF::Scene& scene, bool& broken)
        {
            struct Step
            {
                S32 node;
                glm::mat4 parent;
            };
            std::vector<MeshNode> out;
            std::vector<bool> taken(asset.mNodes.size(), false);
            std::vector<Step> stack;
            const glm::mat4 identity(1.f);
            for (auto it = scene.mNodes.rbegin(); it != scene.mNodes.rend(); ++it)
            {
                stack.push_back({ *it, identity });
            }
            broken = false;
            while (!stack.empty())
            {
                const Step step = stack.back();
                stack.pop_back();
                if (step.node < 0 || step.node >= (S32)asset.mNodes.size() || taken[step.node])
                {
                    broken = true;
                    continue;
                }
                taken[step.node] = true;
                LL::GLTF::Node& node = asset.mNodes[step.node];
                node.makeMatrixValid();     // from its translation, rotation and scale if it has no matrix
                const glm::mat4 matrix = step.parent * node.mMatrix;
                if (node.mMesh >= 0 && node.mMesh < (S32)asset.mMeshes.size())
                {
                    out.push_back({ step.node, matrix });
                }
                else if (node.mMesh != LL::GLTF::INVALID_INDEX)
                {
                    broken = true;
                }
                for (auto it = node.mChildren.rbegin(); it != node.mChildren.rend(); ++it)
                {
                    stack.push_back({ *it, matrix });
                }
            }
            return out;
        }

        // The image files the file lists that are files of their own (not inside it, not
        // asset ids), of the kinds Local Textures opens, each once however it is written
        // ("a.png", "./a.png", "A.PNG"), at most MAX_LISTED_IMAGES.
        void listImages(const LL::GLTF::Asset& asset, const std::string& folder, std::vector<std::string>& images, Log& log)
        {
            static const char* const KINDS[] = { "png", "jpg", "jpeg", "tga", "bmp" };
            std::set<std::string> seen;     // lower case: Windows does not tell case apart
            for (const std::string& image : images)
            {
                seen.insert(utf8str_tolower(image));
            }
            for (const LL::GLTF::Image& image : asset.mImages)
            {
                LLUUID id;
                if (image.mUri.empty() || image.mUri.rfind("data:", 0) == 0
                    || (image.mUri.size() == UUID_STR_SIZE && LLUUID::parseUUID(image.mUri, &id)))
                {
                    continue;
                }
                std::string path;
                if (!pathInFolder(folder, image.mUri, path))
                {
                    log.push_back("The image file \"" + utf8str_truncate(image.mUri, MAX_NAME_BYTES)
                                  + "\" is not in the file's folder or below it; not added to Local Textures.");
                    continue;
                }
                path = tidyPath(path);
                const std::string kind = gDirUtilp->getExtension(path);
                if (std::find_if(std::begin(KINDS), std::end(KINDS), [&](const char* k) { return kind == k; }) == std::end(KINDS)
                    || !gDirUtilp->fileExists(path) || !seen.insert(utf8str_tolower(path)).second)
                {
                    continue;
                }
                if (images.size() >= MAX_LISTED_IMAGES)
                {
                    log.push_back("The file uses more than " + std::to_string(MAX_LISTED_IMAGES)
                                  + " image files; only the first are added to Local Textures.");
                    break;
                }
                images.push_back(path);
            }
        }
    }

    bool readGLTF(const std::string& path, std::vector<RawMesh>& meshes, std::vector<std::string>& images, Log& log)
    {
        const bool glb = gDirUtilp->getExtension(path) == "glb";
        std::string data;
        std::string error;
        if (!readFileBytes(path, 0, glb ? MAX_FILE_BYTES : MAX_JSON_BYTES, data, error))
        {
            log.push_back("The file " + error + ".");
            return false;
        }

        std::string_view json(data);
        std::string_view bin;
        bool has_bin = false;
        if (glb && !splitGLB(data, json, bin, has_bin, error))
        {
            log.push_back("The file could not be read as glTF: " + error + ".");
            return false;
        }
        // the JSON chunk is padded to 4 bytes, with spaces as the format says or with zeros as
        // some exporters do
        while (!json.empty() && (json.back() == 0 || std::isspace((unsigned char)json.back())))
        {
            json.remove_suffix(1);
        }

        LL::GLTF::Asset asset;
        std::vector<size_t> primitive_counts;
        if (!assetFromJSON(json, asset, primitive_counts, error))
        {
            log.push_back("The file could not be read as glTF: " + error + ".");
            return false;
        }
        for (const std::string& extension : asset.mExtensionsRequired)
        {
            if (!geometryUnaffected(extension))
            {
                log.push_back("The file needs the glTF extension "
                              + (extension.empty() ? std::string("(no name)") : utf8str_truncate(extension, MAX_NAME_BYTES))
                              + ", which this viewer does not read.");
                return false;
            }
        }
        if (asset.mScenes.empty())
        {
            log.push_back("The file has no scenes.");
            return false;
        }

        const std::string folder = gDirUtilp->getDirName(path);
        listImages(asset, folder, images, log);

        const S32 scene_index = (asset.mScene >= 0 && asset.mScene < (S32)asset.mScenes.size()) ? asset.mScene : 0;
        bool broken = false;
        const std::vector<MeshNode> nodes = meshNodes(asset, asset.mScenes[scene_index], broken);
        if (broken)
        {
            log.push_back("The file's node tree is broken (a node out of range, or reached twice); those nodes were skipped.");
        }

        Arrays arrays(asset, folder, bin, has_bin);
        Budget budget;
        size_t tried = 0;       // meshes read, kept or not: a mesh repeated by many nodes is read once per node
        for (const MeshNode& mesh_node : nodes)
        {
            if (tried++ >= MAX_FILE_MESHES)
            {
                log.push_back("The file has more than " + std::to_string(MAX_FILE_MESHES) + " meshes; the rest are left out.");
                break;
            }
            if (budget.spent)
            {
                log.push_back("The rest of the file is left out (it is larger than a local mesh may be).");
                break;
            }
            const LL::GLTF::Node& node = asset.mNodes[mesh_node.node];
            const LL::GLTF::Mesh& mesh = asset.mMeshes[node.mMesh];

            RawMesh raw;
            raw.name = utf8str_truncate(!node.mName.empty() ? node.mName
                                        : !mesh.mName.empty() ? mesh.mName
                                        : "node " + std::to_string(mesh_node.node), MAX_NAME_BYTES);
            if (node.mSkin >= 0)
            {
                log.push_back("\"" + raw.name + "\" is rigged; it is shown static (rigging is not read).");
            }
            const size_t primitive_count = (size_t)node.mMesh < primitive_counts.size() ? primitive_counts[node.mMesh] : 0;
            if (primitive_count > (size_t)LL_SCULPT_MESH_MAX_FACES)
            {
                log.push_back("\"" + raw.name + "\" has " + std::to_string(primitive_count)
                              + " primitives; only the first " + std::to_string(LL_SCULPT_MESH_MAX_FACES)
                              + " are shown (a mesh object has at most " + std::to_string(LL_SCULPT_MESH_MAX_FACES) + " faces).");
            }

            const glm::mat4 transform = TO_VIEWER_AXES * mesh_node.matrix;
            for (const LL::GLTF::Primitive& prim : mesh.mPrimitives)
            {
                if (raw.faces.size() >= (size_t)LL_SCULPT_MESH_MAX_FACES)
                {
                    break;
                }
                RawFace face;
                if (!readPrimitive(prim, transform, arrays, budget, face, error))
                {
                    log.push_back("A face of \"" + raw.name + "\" was skipped: " + error + ".");
                }
                else if (!face.indices.empty())
                {
                    raw.faces.push_back(std::move(face));
                }
            }

            if (raw.faces.empty())
            {
                log.push_back("\"" + raw.name + "\" has no faces that could be read; left out.");
                continue;
            }
            meshes.push_back(std::move(raw));
        }

        if (meshes.empty())
        {
            log.push_back(nodes.empty() ? "The file's scene has no meshes." : "No mesh of the file could be read.");
            return false;
        }
        return true;
    }
}
