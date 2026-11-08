/**
 * @file testscenedef.cpp
 * @brief Test scene: the scene definition, read and checked.
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

// The keys of a scene file (an LLSD map; app_settings/plus/testscene/*.xml are the examples):
//
//   seed             U32     the generator's seed: the same seed and keys make the same scene (the same hash)
//   description      string  free text
//   center           "agent" (the avatar's place rounded to 8 m, + height) or [x, y, z] in the region
//   height           m       the middle above the avatar (-100..1000)
//   radius           m       objects within +- radius on x and y (1..128)
//   vertical         m       and within +- vertical / 2 on z (0..256)
//   clusters         count   0: spread evenly; N: N places, each object near one (0..1024)
//   cluster_radius   m       how near (0.5..64)
//   prims            count   opaque prims
//   shapes           list    box, cylinder, sphere, torus, prism, tube (the prims' and the meshes' shapes)
//   size_min/max     m       an object's size, each axis apart (0.05..32)
//   textures         count   generated textures for prims and meshes (1..2048)
//   texture_size     px      32..1024, a power of two (all of the scene's textures)
//   texture_per_face bool    a texture for each face instead of one for the object
//   fullbright       0..1    the part of the opaque objects drawn full bright
//   meshes           count   mesh objects (made from prims' faces, 4 LODs, no file)
//   mesh_kinds       count   different meshes (1..32)
//   alpha_sheets     count   flat alpha sheets, one face each
//   alpha            0.05..0.95  their opacity
//   alpha_textures   count   their generated textures (with alpha streaks; 1..2048)
//   sheet_size_min/max m     a sheet's width and height (0.1..32)
//   texanim          0..1    the part of the sheets with a scrolling texture
//   texanim_rate     /s      its speed in texture widths (0.01..10)
//   view             [x,y,z] the camera of lookAt() from the middle (default: from the -x side, a little above)
//   comment          any     ignored
//
// At most MAX_OBJECTS objects and MAX_TEXELS texels in all (testscenedef.h).

#include "llviewerprecompiledheaders.h"

#include "testscenedef.h"

#include "plus/plus.h"

#include "lldir.h"
#include "llfile.h"
#include "llsdserialize.h"
#include "llsdutil.h"
#include "llstring.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <sstream>

namespace plus::testscene
{
    namespace
    {
        const char* const SHAPE_NAMES[] = { "box", "cylinder", "sphere", "torus", "prism", "tube" };
        static_assert(sizeof(SHAPE_NAMES) / sizeof(SHAPE_NAMES[0]) == (size_t)Shape::Count, "one name a shape");

        std::string sceneDir()
        {
            return gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "plus", "testscene");
        }

        bool parseNumber(const std::string& text, double& out)
        {
            std::string t = text;
            LLStringUtil::trim(t);
            const char* begin = t.c_str();
            char*       end   = nullptr;
            const double v = std::strtod(begin, &end);
            if (end == begin || *end || !std::isfinite(v))
            {
                return false;
            }
            out = v;
            return true;
        }

        bool number(const LLSD& value, double& out)
        {
            if (value.isReal() || value.isInteger())
            {
                out = value.asReal();
                return std::isfinite(out);
            }
            if (value.isBoolean())
            {
                out = value.asBoolean() ? 1.0 : 0.0;
                return true;
            }
            return value.isString() && parseNumber(value.asString(), out);
        }

        bool boolean(const LLSD& value, bool& out)
        {
            if (value.isBoolean())
            {
                out = value.asBoolean();
                return true;
            }
            if (value.isString())
            {
                std::string t = value.asString();
                LLStringUtil::trim(t);
                LLStringUtil::toLower(t);
                if (t == "1" || t == "true" || t == "on")
                {
                    out = true;
                    return true;
                }
                if (t == "0" || t == "false" || t == "off")
                {
                    out = false;
                    return true;
                }
                return false;
            }
            double v = 0.0;
            if (number(value, v) && (v == 0.0 || v == 1.0))
            {
                out = v != 0.0;
                return true;
            }
            return false;
        }

        // An array of strings or numbers, or one string with commas.
        std::vector<LLSD> items(const LLSD& value)
        {
            std::vector<LLSD> out;
            if (value.isArray())
            {
                for (const LLSD& item : llsd::inArray(value))
                {
                    out.push_back(item);
                }
            }
            else if (value.isString())
            {
                std::istringstream in(value.asString());
                std::string part;
                while (std::getline(in, part, ','))
                {
                    LLStringUtil::trim(part);
                    if (!part.empty())
                    {
                        out.push_back(LLSD(part));
                    }
                }
            }
            return out;
        }

        bool vector3(const LLSD& value, F32 low, F32 high, LLVector3& out)
        {
            const std::vector<LLSD> parts = items(value);
            if (parts.size() != 3)
            {
                return false;
            }
            for (int i = 0; i < 3; ++i)
            {
                double v = 0.0;
                if (!number(parts[i], v) || v < low || v > high)
                {
                    return false;
                }
                out.mV[i] = (F32)v;
            }
            return true;
        }

        using Setter = std::function<bool(const LLSD& value, Definition& def)>;

        Setter u32In(U32 Definition::*member, U32 low, U32 high)
        {
            return [member, low, high](const LLSD& value, Definition& def)
            {
                double v = 0.0;
                if (!number(value, v) || v != std::floor(v) || v < (double)low || v > (double)high)
                {
                    return false;
                }
                def.*member = (U32)v;
                return true;
            };
        }

        Setter f32In(F32 Definition::*member, F32 low, F32 high)
        {
            return [member, low, high](const LLSD& value, Definition& def)
            {
                double v = 0.0;
                if (!number(value, v) || v < (double)low || v > (double)high)
                {
                    return false;
                }
                def.*member = (F32)v;
                return true;
            };
        }

        Setter flag(bool Definition::*member)
        {
            return [member](const LLSD& value, Definition& def) { return boolean(value, def.*member); };
        }

        struct Key
        {
            Setter      set;
            const char* range;   // for the error
        };

        const std::map<std::string, Key>& keys()
        {
            static const std::map<std::string, Key> sKeys = {
                { "seed", { u32In(&Definition::seed, 0, 0xFFFFFFFFu), "a whole number 0..4294967295" } },
                { "description", { [](const LLSD& v, Definition& d) { d.description = v.asString().substr(0, 200); return true; }, "text" } },
                { "comment", { [](const LLSD&, Definition&) { return true; }, "anything" } },
                { "center", { [](const LLSD& v, Definition& d)
                              {
                                  if (v.isString() && LLStringUtil::compareInsensitive(v.asString(), "agent") == 0)
                                  {
                                      d.centerOnAgent = true;
                                      return true;
                                  }
                                  LLVector3 c;
                                  if (!vector3(v, 0.f, 16384.f, c) || c.mV[VZ] > 4096.f)
                                  {
                                      return false;
                                  }
                                  d.centerOnAgent = false;
                                  d.center = c;
                                  return true;
                              }, "\"agent\" or [x, y, z] in the region (z 0..4096)" } },
                { "height", { f32In(&Definition::height, -100.f, 1000.f), "-100..1000 m" } },
                { "radius", { f32In(&Definition::radius, 1.f, 128.f), "1..128 m" } },
                { "vertical", { f32In(&Definition::vertical, 0.f, 256.f), "0..256 m" } },
                { "clusters", { u32In(&Definition::clusters, 0, MAX_CLUSTERS), "0..1024" } },
                { "cluster_radius", { f32In(&Definition::clusterRadius, 0.5f, 64.f), "0.5..64 m" } },
                { "prims", { u32In(&Definition::prims, 0, MAX_OBJECTS), "0..20000" } },
                { "shapes", { [](const LLSD& v, Definition& d)
                              {
                                  std::vector<Shape> shapes;
                                  for (const LLSD& item : items(v))
                                  {
                                      std::string name = item.asString();
                                      LLStringUtil::toLower(name);
                                      const auto it = std::find_if(std::begin(SHAPE_NAMES), std::end(SHAPE_NAMES),
                                                                   [&name](const char* n) { return name == n; });
                                      if (it == std::end(SHAPE_NAMES))
                                      {
                                          return false;
                                      }
                                      shapes.push_back((Shape)(it - std::begin(SHAPE_NAMES)));
                                  }
                                  if (shapes.empty() || shapes.size() > 64)
                                  {
                                      return false;
                                  }
                                  d.shapes = std::move(shapes);
                                  return true;
                              }, "a list of box, cylinder, sphere, torus, prism, tube" } },
                { "size_min", { f32In(&Definition::sizeMin, 0.05f, 32.f), "0.05..32 m" } },
                { "size_max", { f32In(&Definition::sizeMax, 0.05f, 32.f), "0.05..32 m" } },
                { "textures", { u32In(&Definition::textures, 1, MAX_TEXTURES), "1..2048" } },
                { "texture_size", { [](const LLSD& v, Definition& d)
                                    {
                                        double n = 0.0;
                                        if (!number(v, n) || n < 32.0 || n > 1024.0 || n != std::floor(n))
                                        {
                                            return false;
                                        }
                                        const U32 size = (U32)n;
                                        if (size & (size - 1))
                                        {
                                            return false;
                                        }
                                        d.textureSize = size;
                                        return true;
                                    }, "32, 64, 128, 256, 512 or 1024" } },
                { "texture_per_face", { flag(&Definition::texturePerFace), "true or false" } },
                { "fullbright", { f32In(&Definition::fullbright, 0.f, 1.f), "0..1" } },
                { "meshes", { u32In(&Definition::meshes, 0, MAX_OBJECTS), "0..20000" } },
                { "mesh_kinds", { u32In(&Definition::meshKinds, 1, MAX_MESH_KINDS), "1..32" } },
                { "alpha_sheets", { u32In(&Definition::alphaSheets, 0, MAX_OBJECTS), "0..20000" } },
                { "alpha", { f32In(&Definition::alpha, 0.05f, 0.95f), "0.05..0.95" } },
                { "alpha_textures", { u32In(&Definition::alphaTextures, 1, MAX_TEXTURES), "1..2048" } },
                { "sheet_size_min", { f32In(&Definition::sheetSizeMin, 0.1f, 32.f), "0.1..32 m" } },
                { "sheet_size_max", { f32In(&Definition::sheetSizeMax, 0.1f, 32.f), "0.1..32 m" } },
                { "texanim", { f32In(&Definition::texanim, 0.f, 1.f), "0..1" } },
                { "texanim_rate", { f32In(&Definition::texanimRate, 0.01f, 10.f), "0.01..10" } },
                { "view", { [](const LLSD& v, Definition& d)
                            {
                                if (!vector3(v, -512.f, 512.f, d.view))
                                {
                                    return false;
                                }
                                d.hasView = true;
                                return true;
                            }, "[x, y, z], each -512..512 m" } },
            };
            return sKeys;
        }
    }

    const char* shapeName(Shape shape)
    {
        return (size_t)shape < (size_t)Shape::Count ? SHAPE_NAMES[(size_t)shape] : "?";
    }

    bool applyMap(const LLSD& map, Definition& def, std::string& error)
    {
        if (map.isUndefined())
        {
            return true;
        }
        if (!map.isMap())
        {
            error = "the scene must be a map of keys";
            return false;
        }
        for (const auto& [name, value] : llsd::inMap(map))
        {
            const auto it = keys().find(name);
            if (it == keys().end())
            {
                error = "unknown key '" + name + "'";
                return false;
            }
            if (!it->second.set(value, def))
            {
                error = "'" + name + "' must be " + it->second.range + " (got " + value.asString().substr(0, 60) + ")";
                return false;
            }
        }
        return true;
    }

    bool checkTotals(const Definition& def, std::string& error)
    {
        const U64 objects = (U64)def.prims + def.meshes + def.alphaSheets;
        if (objects == 0)
        {
            error = "no objects (prims, meshes and alpha_sheets are all 0)";
            return false;
        }
        if (objects > MAX_OBJECTS)
        {
            error = llformat("%llu objects, at most %u", (unsigned long long)objects, MAX_OBJECTS);
            return false;
        }
        if (def.sizeMin > def.sizeMax || def.sheetSizeMin > def.sheetSizeMax)
        {
            error = "a *_min is larger than its *_max";
            return false;
        }
        const U64 count  = (U64)(def.prims + def.meshes ? def.textures : 0) + (def.alphaSheets ? def.alphaTextures : 0);
        const U64 texels = count * def.textureSize * def.textureSize;
        if (texels > MAX_TEXELS)
        {
            error = llformat("%llu texels of textures, at most %u (fewer textures, or a smaller texture_size)",
                             (unsigned long long)texels, MAX_TEXELS);
            return false;
        }
        return true;
    }

    bool loadDefinition(const std::string& name, Definition& def, std::string& error)
    {
        def = Definition();
        std::string path;
        std::string extension = gDirUtilp->getExtension(name);
        LLStringUtil::toLower(extension);
        if (extension == "xml" && LLFile::isfile(name))
        {
            path = name;
            def.name = gDirUtilp->getBaseFileName(name, true);
        }
        else if (plus::isLoadableFileStem(name) && name.size() <= 100)
        {
            path = gDirUtilp->add(sceneDir(), name + ".xml");
            def.name = name;
        }
        else
        {
            error = "'" + name.substr(0, 100) + "' is neither a scene name nor an .xml file";
            return false;
        }
        const S64 bytes = LLFile::size(path);
        if (bytes <= 0 || bytes > MAX_FILE_BYTES)
        {
            error = bytes > MAX_FILE_BYTES ? "the scene file is larger than 256 KB" : "no scene file " + path;
            return false;
        }
        llifstream in(path.c_str(), std::ios::binary);
        LLSD map;
        if (!in.is_open() || LLSDParser::PARSE_FAILURE == LLSDSerialize::fromXML(map, in))
        {
            error = "cannot read " + path + " (an LLSD map in XML)";
            return false;
        }
        if (!applyMap(map, def, error))
        {
            error = def.name + ": " + error;
            return false;
        }
        return true;
    }

    std::string describe(const Definition& def)
    {
        return llformat("seed=%u prims=%u meshes=%u(%u kinds) alpha_sheets=%u alpha=%.2f texanim=%.2f textures=%u+%u size=%u "
                        "radius=%.0f vertical=%.0f clusters=%u",
                        def.seed, def.prims, def.meshes, def.meshKinds, def.alphaSheets, def.alpha, def.texanim, def.textures,
                        def.alphaTextures, def.textureSize, def.radius, def.vertical, def.clusters);
    }

    std::vector<std::string> sceneNames()
    {
        std::vector<std::string> names;
        for (const std::string& file : gDirUtilp->getFilesInDir(sceneDir()))
        {
            std::string extension = gDirUtilp->getExtension(file);
            LLStringUtil::toLower(extension);
            if (extension == "xml")
            {
                names.push_back(gDirUtilp->getBaseFileName(file, true));
            }
        }
        std::sort(names.begin(), names.end());
        return names;
    }
}
